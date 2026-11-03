#!/usr/bin/env python3
"""Run the interrupt self-test (CONFIG_KSHIM_IRQ_SELFTEST) in QEMU TCG.

GICv3 at EL1 (hvc) and EL2 (smc), 1-8 CPUs: timer interrupts and WFI idle,
SGI wake-up of the PSCI workers, the PL011 receive interrupt (fed through
the serial port), and the GIC/CPU-interface/timer state after the handoff
teardown compared with a seeded pre-kShimC snapshot.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import shutil
import subprocess
import threading
import time

RX_INPUT = b"kshim\r"


def run_qemu(command, timeout):
    """Run QEMU, type RX_INPUT once the receive test is armed."""
    process = subprocess.Popen(command, stdin=subprocess.PIPE,
                               stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    lines = []

    def reader():
        pending = b""
        while True:
            chunk = process.stdout.read(1)
            if not chunk:
                break
            pending += chunk
            if chunk == b"\n":
                line = pending.decode(errors="replace")
                lines.append(line)
                pending = b""
                if "IRQ_SELFTEST rx: ready" in line:
                    time.sleep(0.2)
                    process.stdin.write(RX_INPUT)
                    process.stdin.flush()
        if pending:
            lines.append(pending.decode(errors="replace"))

    thread = threading.Thread(target=reader, daemon=True)
    thread.start()
    timed_out = False
    try:
        process.wait(timeout=timeout)
    except subprocess.TimeoutExpired:
        process.kill()
        process.wait()
        timed_out = True
    thread.join(5)
    return process.returncode, "".join(lines), timed_out


def parse(output, cpus, el):
    """Evidence from the self-test lines and whether it is sufficient."""
    evidence, problems = {}, []
    timer = re.search(r"IRQ_SELFTEST timer: intid=(\d+) el=(\d+) delay_ms=(\d+) "
                      r"timer_irqs=(\d+) wfi=(\d+) wait_ms=(\d+) wait_timer_irqs=(\d+) "
                      r"wait_wfi=(\d+)", output)
    if timer:
        keys = ("intid", "el", "delay_ms", "timer_irqs", "wfi", "wait_ms",
                "wait_timer_irqs", "wait_wfi")
        evidence["timer"] = dict(zip(keys, map(int, timer.groups())))
        t = evidence["timer"]
        if t["intid"] != (26 if el == 2 else 27) or t["el"] != el:
            problems.append("timer PPI/EL")
        if not (200 <= t["delay_ms"] < 1000 and t["timer_irqs"] >= 1 and 1 <= t["wfi"] <= 10):
            problems.append("delay() did not sleep on the timer interrupt")
        if not (150 <= t["wait_ms"] < 1000 and t["wait_timer_irqs"] >= 1 and 1 <= t["wait_wfi"] <= 10):
            problems.append("kshim_wait_event() did not sleep to its deadline")
    else:
        problems.append("no timer evidence")
    sgi = re.search(r"IRQ_SELFTEST sgi: self=(\d+) workers=(\d+)((?: cpu\d+=\d+/\d+)*)", output)
    if sgi:
        workers = {f"cpu{c}": {"wake_sgis": int(w), "wfi": int(s)}
                   for c, w, s in re.findall(r"cpu(\d+)=(\d+)/(\d+)", sgi.group(3))}
        evidence["sgi"] = {"self": int(sgi.group(1)), "workers": int(sgi.group(2)),
                           "per_worker": workers}
        if int(sgi.group(1)) < 1 or int(sgi.group(2)) != min(cpus - 1, 7):
            problems.append("SGI self-test or worker count")
        if any(w["wake_sgis"] < 1 or w["wfi"] < 1 for w in workers.values()):
            problems.append("a worker was not woken from WFI by an SGI")
    else:
        problems.append("no SGI evidence")
    rx = re.search(r"IRQ_SELFTEST rx: intid=(\d+) irqs=(\d+) wakes=(\d+) chars=(\d+) data=(\S*)", output)
    if rx:
        evidence["rx"] = {"intid": int(rx.group(1)), "irqs": int(rx.group(2)),
                          "wakes": int(rx.group(3)), "chars": int(rx.group(4)), "data": rx.group(5)}
        r = evidence["rx"]
        if r["intid"] != 33 or r["irqs"] < 1 or r["chars"] != 6 or r["data"] != "kshim|":
            problems.append("PL011 receive interrupt")
    else:
        problems.append("no RX evidence")
    state = re.search(r"IRQ_SELFTEST gic-state: handoff=(-?\d+) words=(\d+) pre=([0-9a-f]+) "
                      r"post=([0-9a-f]+) irqs_total=(\d+) daif_i=(\d) ready=(\d) (\w+)", output)
    inherit = re.search(r"IRQ_SELFTEST inherit: el=(\d+) words=(\d+) hash=([0-9a-f]+)", output)
    if state and inherit:
        evidence["gic_state"] = {
            "handoff": int(state.group(1)), "words": int(state.group(2)),
            "pre": state.group(3), "post": state.group(4), "irqs_total": int(state.group(5)),
            "daif_i": int(state.group(6)), "ready": int(state.group(7)), "result": state.group(8),
            "inherited_hash": inherit.group(3)}
        g = evidence["gic_state"]
        if (g["result"] != "MATCH" or g["pre"] != g["post"] or g["pre"] != g["inherited_hash"]
                or g["handoff"] != 0 or g["daif_i"] != 1 or g["ready"] != 0):
            problems.append("handoff state differs from the inherited snapshot")
    else:
        problems.append("no GIC state evidence")
    if "SMP_SELFTEST PASS (0)" not in output:
        problems.append("SMP self-test")
    if "IRQ_SELFTEST PASS (0)" not in output:
        problems.append("IRQ self-test verdict")
    return evidence, problems


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--build", type=Path, default=Path("build/qemu-irq"))
    parser.add_argument("--cpus", default="1,2,4,8")
    parser.add_argument("--repeat", type=int, default=1)
    parser.add_argument("--no-build", action="store_true")
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    out = args.build.resolve()
    out.mkdir(parents=True, exist_ok=True)
    if not args.no_build:
        config = (root / "configs/qemu_smp_defconfig").read_text()
        config += "CONFIG_KSHIM_IRQ=y\nCONFIG_KSHIM_IRQ_SELFTEST=y\n"
        (out / ".config").write_text(config)
        with (out / "configure.log").open("w") as log:
            subprocess.run(["cmake", "-S", root, "-B", out, "-G", "Ninja",
                            "-DKSHIM_STANDALONE=ON"], stdout=log, stderr=subprocess.STDOUT, check=True)
        with (out / "build.log").open("w") as log:
            subprocess.run(["cmake", "--build", out, "-j8"], stdout=log,
                           stderr=subprocess.STDOUT, check=True)
    image = out / "kShimC.bin"
    results = []
    for virt in (False, True):
        el = 2 if virt else 1
        for count in (int(x) for x in args.cpus.split(",")):
            for repeat in range(args.repeat):
                name = f"{'el2-smc' if virt else 'el1-hvc'}-{count}cpu-{repeat}"
                command = ["qemu-system-aarch64", "-machine",
                           f"virt,gic-version=3,virtualization={'on' if virt else 'off'}",
                           "-accel", "tcg,thread=multi", "-cpu", "cortex-a76",
                           "-smp", str(count), "-m", "256M", "-display", "none",
                           "-monitor", "none", "-serial", "stdio", "-no-reboot",
                           "-kernel", str(image)]
                start = time.monotonic()
                returncode, output, timed_out = run_qemu(command, 90)
                evidence, problems = parse(output, count, el)
                if timed_out:
                    problems.insert(0, "timeout")
                elif returncode != 0:
                    problems.insert(0, f"qemu exit {returncode}")
                (out / (name + ".log")).write_text(output)
                results.append({"name": name, "command": command, "passed": not problems,
                                "problems": problems, "evidence": evidence,
                                "seconds": time.monotonic() - start})
                print(f"{name}: {'PASS' if not problems else 'FAIL ' + '; '.join(problems)}", flush=True)
    report = {"image_sha256": hashlib.sha256(image.read_bytes()).hexdigest(),
              "qemu": subprocess.check_output(["qemu-system-aarch64", "--version"], text=True).splitlines()[0],
              "tests": results, "passed": all(x["passed"] for x in results)}
    (out / "validation.json").write_text(json.dumps(report, indent=2) + "\n")
    if not report["passed"]:
        raise SystemExit(1)


if __name__ == "__main__":
    main()
