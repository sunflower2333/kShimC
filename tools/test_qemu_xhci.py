#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""End-to-end xHCI test in QEMU (configs/qemu_xhci_defconfig).

QEMU virt (GICv3) with qemu-xhci or nec-usb-xhci (INTx, no MSI) behind the gpex
ECAM host, a usb-kbd and a usb-storage stick on its root ports. The Manifest
runtime enumerates both through the xHCI driver and the USB core, logs the
descriptors, runs the self-test class driver (HID report descriptor over a
control IN transfer, SET_IDLE, an interrupt-IN URB that receives the key
presses this script sends over QMP, a Bulk-Only INQUIRY over bulk OUT/IN),
then boots its default entry. That entry is a payload which dumps the xHCI
function: its configuration header and, after assigning BAR0 itself, the
capability, operational, port, interrupter 0 and extended-capability
registers. The dump must equal the one taken when the payload boots directly
(kShimC never ran): the controller is halted and reset and every config
register kShimC changed is restored.

Two runtimes are built: interrupt-driven (INTx through the PCI core and the
gpex interrupt-map) and polled (CONFIG_KSHIM_USB_XHCI_FORCE_POLL). The PCI
layer is the real PCI core with the generic ECAM host (CONFIG_KSHIM_PCI_ECAM).
"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import socket
import subprocess
import threading
import time

PAYLOAD_ENTRY = r"""
.global _start
.text
_start:
    b start
    nop
    .quad 0
    .quad 4096
    .quad 0
    .quad 0
    .quad 0
    .quad 0
    .ascii "ARM\x64"
    .long 0
start:
    mov x9, #0x4f000000
    mov sp, x9
    bl payload_main
    b .
"""

# Reads the xHCI function the way a payload finds it after handoff. QEMU virt
# facts: PL011 at 0x09000000, high ECAM at 0x4010000000, 32-bit MMIO window at
# 0x10000000. Register offsets: PCI header, xHCI 1.2 chapter 5.
PAYLOAD_MAIN = r"""
#include <stdint.h>
#define UART 0x09000000UL
#define ECAM 0x4010000000UL
#define BAR  0x10000000UL
#define SYSREG(name) ({ uint64_t v; __asm__ volatile("mrs %0, " #name : "=r"(v)); v; })

static void out(char c) { *(volatile uint32_t *)UART = (uint32_t)c; }
static void text(const char *s) { while (*s) out(*s++); }
static void hex(uint64_t v, int digits) { for (int i = digits * 4 - 4; i >= 0; i -= 4) out("0123456789abcdef"[(v >> i) & 15]); }
static uint32_t r32(uint64_t a) { return *(volatile uint32_t *)a; }
static void w32(uint64_t a, uint32_t v) { *(volatile uint32_t *)a = v; }
static void reg(const char *name, uint32_t key, uint32_t value)
{
    text("XHCI "); text(name); out(' '); hex(key, 4); out(' '); hex(value, 8); out('\n');
}

void payload_main(void)
{
    uint64_t cfg = 0;
    for (uint32_t dev = 0; dev < 32 && !cfg; ++dev) {
        uint64_t c = ECAM + ((uint64_t)dev << 15);
        if ((r32(c) & 0xffff) != 0xffff && (r32(c + 8) >> 8) == 0x0c0330) cfg = c;
    }
    if (!cfg) {
        text("XHCI_NONE\n");
    } else {
        for (uint32_t off = 0; off < 0x100; off += 4) reg("cfg", off, r32(cfg + off));
        /* Assign BAR0 (64-bit) and enable memory decode, as an OS would. */
        w32(cfg + 0x10, (uint32_t)BAR);
        w32(cfg + 0x14, 0);
        w32(cfg + 0x04, 2);
        uint32_t caplen = r32(BAR) & 0xff;
        uint64_t op = BAR + caplen, rt = BAR + (r32(BAR + 0x18) & ~0x1fu);
        for (uint32_t off = 0; off < 0x20; off += 4) reg("cap", off, r32(BAR + off));
        static const uint32_t ops[] = {0x00, 0x04, 0x08, 0x14, 0x18, 0x1c, 0x30, 0x34, 0x38};
        for (unsigned i = 0; i < sizeof(ops) / sizeof(ops[0]); ++i) reg("op", ops[i], r32(op + ops[i]));
        uint32_t ports = r32(BAR + 4) >> 24;
        for (uint32_t p = 0; p < ports; ++p)
            for (uint32_t off = 0; off < 0x10; off += 4) reg("port", 0x400 + 0x10 * p + off, r32(op + 0x400 + 0x10 * p + off));
        for (uint32_t off = 0x20; off < 0x40; off += 4) reg("ir0", off, r32(rt + off));
        uint32_t xecp = (r32(BAR + 0x10) >> 16) * 4;
        for (unsigned guard = 0; xecp && guard < 16; ++guard) {
            for (uint32_t off = 0; off < 16; off += 4) reg("xcap", xecp + off, r32(BAR + xecp + off));
            uint32_t next = (r32(BAR + xecp) >> 8) & 0xff;
            xecp = next ? xecp + next * 4 : 0;
        }
    }
    text("XHCI_DONE\n");
    register uint64_t x0 __asm__("x0") = 0x84000008; /* PSCI SYSTEM_OFF */
    if (((SYSREG(CurrentEL) >> 2) & 3) == 2) __asm__ volatile("smc #0" : : "r"(x0));
    else __asm__ volatile("hvc #0" : : "r"(x0));
    for (;;) {}
}
"""

CONTROLLERS = {
    # qemu-xhci has no msi/msix properties in QEMU 8.2; kShimC never enables
    # MSI on it (the ECAM host offers INTx only).
    "qemu-xhci": {"device": "qemu-xhci,id=xhci", "id": "1b36:000d"},
    "nec-usb-xhci": {"device": "nec-usb-xhci,id=xhci,msi=off,msix=off", "id": "1033:0194"},
}


def run(command, **kwargs):
    return subprocess.run([str(x) for x in command], check=True, **kwargs)


def find_packer(root, explicit):
    if explicit:
        return explicit.resolve()
    for parent in root.parents:
        candidate = parent / "MultiBootKernelPatcher"
        if (candidate / "CMakeLists.txt").exists():
            return candidate
    raise SystemExit("MultiBootKernelPatcher not found; pass --packer")


def build_runtime(root, out, extra, jobs):
    out.mkdir(parents=True, exist_ok=True)
    config = (root / "configs/qemu_xhci_defconfig").read_text() + extra
    (out / ".config").write_text(config)
    with (out / "configure.log").open("w") as log:
        run(["cmake", "-S", root, "-B", out, "-G", "Ninja", "-DKSHIM_STANDALONE=OFF",
             "-DKSHIM_RUNTIME_SLOT_SIZE=0x400000", "-DKSHIM_UEFI_PAYLOAD=",
             "-DCMAKE_BUILD_TYPE=Release"], stdout=log, stderr=subprocess.STDOUT)
    with (out / "build.log").open("w") as log:
        run(["cmake", "--build", out, "-j", str(jobs)], stdout=log, stderr=subprocess.STDOUT)
    binary = out / "kShimC.bin"
    assert binary.stat().st_size == 0x400000, "Manifest runtime must be 4 MiB"
    return binary


def qmp_keys(path, keys, log):
    """Type keys on the usb-kbd through QMP send-key."""
    try:
        for _ in range(50):
            try:
                sock = socket.socket(socket.AF_UNIX)
                sock.connect(str(path))
                break
            except OSError:
                time.sleep(0.1)
        stream = sock.makefile("rw")
        stream.readline()
        stream.write(json.dumps({"execute": "qmp_capabilities"}) + "\n")
        stream.flush()
        stream.readline()
        for key in keys:
            stream.write(json.dumps({"execute": "send-key", "arguments": {
                "keys": [{"type": "qcode", "data": key}]}}) + "\n")
            stream.flush()
            log.append("qmp: " + stream.readline().strip())
            time.sleep(0.2)
        sock.close()
    except OSError as error:
        log.append(f"qmp: {error}")


def run_qemu(command, qmp, timeout):
    process = subprocess.Popen([str(x) for x in command], stdin=subprocess.DEVNULL,
                               stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    lines, qmp_log = [], []

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
                if qmp and "USB_SELFTEST armed" in line:
                    threading.Thread(target=qmp_keys, args=(qmp, ["a"], qmp_log), daemon=True).start()
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
    return "".join(lines), qmp_log, timed_out


def dump(output):
    lines = [line.strip() for line in output.splitlines() if line.startswith("XHCI ")]
    if "XHCI_DONE" not in output or not lines:
        return None
    return lines


def check_log(output, controller, mode):
    """Evidence from the kShimC part of the log and the problems found."""
    evidence, problems = {}, []
    ident = CONTROLLERS[controller]["id"]
    if not re.search(rf"PCI: 0000:00:[0-9a-f]{{2}}\.0 \[{ident}\] class 0x0c0330 BAR0 mem(64)? "
                     r"0x[0-9a-f]+\+0x[0-9a-f]+[^\n]* INTA->(\d+)", output):
        problems.append("xHCI function not enumerated with BAR0 and INTx")
    if not re.search(rf"PCI: 0000:00:[0-9a-f]{{2}}\.0 bound to ", output):
        problems.append("xHCI function not bound")
    started = re.search(r"xhci: (\S+): v(\d+)\.(\d+), (\d+) slots, (\d+) ports", output)
    if started:
        evidence["controller"] = {"version": f"{started.group(2)}.{started.group(3)}",
                                  "slots": int(started.group(4)), "ports": int(started.group(5))}
    else:
        problems.append("controller did not start")
    if mode == "irq":
        intid = re.search(r"xhci: \S+: interrupt INTID (\d+)", output)
        if not intid or "interrupt-driven" not in output:
            problems.append("not interrupt-driven")
        else:
            evidence["intid"] = int(intid.group(1))
    elif "no interrupt, event ring polled" not in output or "interrupt-driven" in output:
        problems.append("not polled")
    kbd = re.search(r"USB: (\S+) addr (\d+) (\S+)-speed 0627:0001 .* ep0 (\d+)", output)
    if kbd and re.search(r"if0 class 3/1/1, 1 endpoints 81:int/8/", output):
        evidence["keyboard"] = {"path": kbd.group(1), "address": int(kbd.group(2)),
                                "speed": kbd.group(3), "ep0": int(kbd.group(4))}
    else:
        problems.append("usb-kbd descriptors")
    stick = re.search(r"USB: (\S+) addr (\d+) (\S+)-speed 46f4:0001 .* ep0 (\d+)", output)
    if stick and re.search(r"if0 class 8/6/80, 2 endpoints", output):
        evidence["storage"] = {"path": stick.group(1), "speed": stick.group(3), "ep0": int(stick.group(4))}
    else:
        problems.append("usb-storage descriptors")
    if not re.search(r"USB_SELFTEST if0 report-descriptor status=0 len=63 ", output):
        problems.append("HID report descriptor (control IN)")
    if not re.search(r'USB_SELFTEST bulk if0 status=0 out=31 in=36 csw=ok vendor="QEMU    "', output):
        problems.append("Bulk-Only INQUIRY (bulk OUT/IN)")
    reports = re.findall(r"USB_SELFTEST report if0 len=8 data=([0-9a-f]+)", output)
    evidence["reports"] = reports
    if len(reports) < 2 or reports[0] != "0000040000000000" or reports[1] != "0000000000000000":
        problems.append("key press/release reports (interrupt IN)")
    stopped = re.search(r"xhci: \S+: stopped \(USBCMD 0x([0-9a-f]+) USBSTS 0x([0-9a-f]+), (\d+) interrupts\)", output)
    if stopped:
        evidence["handoff"] = {"usbcmd": stopped.group(1), "usbsts": stopped.group(2),
                               "interrupts": int(stopped.group(3))}
        if stopped.group(1) != "00000000" or not int(stopped.group(2), 16) & 1:
            problems.append("controller not halted at handoff")
        if mode == "irq" and int(stopped.group(3)) == 0:
            problems.append("no interrupt taken in interrupt mode")
        if mode == "poll" and int(stopped.group(3)) != 0:
            problems.append("interrupt taken in polled mode")
    else:
        problems.append("no xHCI stop at handoff")
    # The PCI core's journal replay is checked by the payload's config-space
    # dump, which must equal a direct boot.
    if "Device shutdown incomplete" in output:
        problems.append("handoff refused")
    return evidence, problems


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--out", type=Path, default=Path("build/qemu-xhci"))
    parser.add_argument("--packer", type=Path, default=None,
                        help="MultiBootKernelPatcher checkout (default: searched next to the tree)")
    parser.add_argument("--jobs", type=int, default=8)
    parser.add_argument("--no-build", action="store_true")
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    out = args.out.resolve()
    out.mkdir(parents=True, exist_ok=True)
    packer_root = find_packer(root, args.packer)

    runtimes = {"irq": out / "runtime-irq/kShimC.bin", "poll": out / "runtime-poll/kShimC.bin"}
    if not args.no_build:
        build_runtime(root, out / "runtime-irq", "", args.jobs)
        build_runtime(root, out / "runtime-poll", "CONFIG_KSHIM_USB_XHCI_FORCE_POLL=y\n", args.jobs)
        with (out / "packer-build.log").open("w") as log:
            run(["cmake", "-S", packer_root, "-B", out / "packer", "-DBUILD_SHIMS=OFF"],
                stdout=log, stderr=subprocess.STDOUT)
            run(["cmake", "--build", out / "packer", "-j4"], stdout=log, stderr=subprocess.STDOUT)
    (out / "entry.S").write_text(PAYLOAD_ENTRY)
    (out / "payload.c").write_text(PAYLOAD_MAIN)
    run(["aarch64-linux-gnu-gcc", "-nostdlib", "-static", "-ffreestanding", "-fno-pic", "-Os",
         "-march=armv8-a", "-mgeneral-regs-only", "-mstrict-align", "-Wl,--build-id=none",
         "-Wl,-Ttext=0", "-Wl,-e,_start", out / "entry.S", out / "payload.c", "-o", out / "payload.elf"])
    run(["aarch64-linux-gnu-objcopy", "-O", "binary", out / "payload.elf", out / "payload.bin"])
    packed = {}
    for mode, runtime in runtimes.items():
        config = out / f"pack-{mode}.cfg"
        packed[mode] = out / f"xhci-{mode}.bin"
        config.write_text(f"""[Pack]
Shim={runtime}
Output={packed[mode]}
Default=Image-A
Timeout=300
[Image-A]
Name=xHCI dump
Path={out / 'payload.bin'}
Type=Linux
BaseImage=true
[Image-B]
Name=xHCI dump B
Path={out / 'payload.bin'}
BaseImage=false
Type=Linux
[Manifest]
Type=Manifest
""")
        with (out / f"pack-{mode}.log").open("w") as log:
            run([out / "packer/MultiBootKernelPatcher", config], stdout=log, stderr=subprocess.STDOUT)
    disk = out / "stick.img"
    with disk.open("wb") as handle:
        handle.truncate(1 << 20)

    def command(controller, image, el2, qmp):
        machine = f"virt,gic-version=3,virtualization={'on' if el2 else 'off'}"
        cmd = ["qemu-system-aarch64", "-machine", machine, "-accel", "tcg", "-cpu", "cortex-a76",
               "-m", "256M", "-display", "none", "-monitor", "none", "-serial", "stdio",
               "-no-reboot", "-nic", "none",
               "-device", CONTROLLERS[controller]["device"],
               "-device", "usb-kbd,bus=xhci.0",
               "-drive", f"if=none,id=stick,format=raw,file={disk}",
               "-device", "usb-storage,bus=xhci.0,drive=stick",
               "-kernel", image]
        if qmp:
            cmd += ["-qmp", f"unix:{qmp},server=on,wait=off"]
        return cmd

    results = []
    direct = {}
    for controller in CONTROLLERS:
        output, _, timed_out = run_qemu(command(controller, out / "payload.bin", False, None), None, 60)
        (out / f"{controller}-direct.log").write_text(output)
        direct[controller] = dump(output)
        if not direct[controller] or timed_out:
            raise SystemExit(f"{controller}: direct payload dump failed")
    cases = [(c, m, False) for c in CONTROLLERS for m in ("irq", "poll")] + [("qemu-xhci", "irq", True)]
    for controller, mode, el2 in cases:
        name = f"{controller}-{mode}-{'el2' if el2 else 'el1'}"
        qmp = out / f"{name}.qmp"
        if qmp.exists():
            qmp.unlink()
        start = time.monotonic()
        output, qmp_log, timed_out = run_qemu(command(controller, packed[mode], el2, qmp), qmp, 120)
        (out / f"{name}.log").write_text(output + "".join(f"\n# {x}" for x in qmp_log))
        evidence, problems = check_log(output, controller, mode)
        handoff = dump(output)
        if timed_out:
            problems.insert(0, "timeout")
        differences = []
        if handoff is None:
            problems.append("payload did not dump the xHCI state")
        else:
            differences = [(a, b) for a, b in zip(direct[controller], handoff) if a != b]
            if len(handoff) != len(direct[controller]):
                differences.append(("register count", f"{len(direct[controller])} != {len(handoff)}"))
            if differences:
                problems.append(f"{len(differences)} registers differ from a direct boot")
        evidence["registers_compared"] = len(handoff or [])
        results.append({"name": name, "passed": not problems, "problems": problems,
                        "evidence": evidence, "differences": differences[:32],
                        "seconds": round(time.monotonic() - start, 1)})
        print(f"{name}: {'PASS' if not problems else 'FAIL ' + '; '.join(problems)} "
              f"({len(handoff or [])} registers compared)", flush=True)
        for a, b in differences[:12]:
            print(f"  direct: {a}\n  kshimc: {b}")
    report = {"runtimes": {m: hashlib.sha256(p.read_bytes()).hexdigest() for m, p in runtimes.items()},
              "qemu": subprocess.check_output(["qemu-system-aarch64", "--version"], text=True).splitlines()[0],
              "hardware_tested": False, "tests": results, "passed": all(r["passed"] for r in results)}
    (out / "validation.json").write_text(json.dumps(report, indent=2) + "\n")
    print(f"{sum(r['passed'] for r in results)}/{len(results)} passed")
    if not report["passed"]:
        raise SystemExit(1)


if __name__ == "__main__":
    main()
