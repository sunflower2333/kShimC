#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""USB end-to-end test on QEMU virt: menu input over USB HID, quiesced handoff.

Each case boots a Manifest image of configs/qemu_usb_defconfig on
`-M virt,gic-version=3` with a PCI xHCI controller (qemu-xhci or
nec-usb-xhci, INTx through the gpex interrupt-map), a usb-hub on its first
root port and a usb-kbd plus a usb-mouse behind the hub. The serial boot
menu has a long autoboot timeout and entry A as its default. Once both HID
interfaces are bound and the menu prompt is up, the script moves the mouse
and types `sendkey down` and `sendkey ret` on the QEMU (HMP) monitor. The
case passes only when entry B boots.

Entry B is a payload that dumps what it inherits: the GICv3 distributor,
redistributor and CPU interface, the configuration header of every PCI
function on bus 0 and, after assigning BAR0 itself as an OS would, the xHCI
capability, operational, port, interrupter 0 and extended-capability
registers. The same payload is also booted directly, without kShimC, with
the same devices. The handoff dump must equal the direct one, and it is
checked explicitly too: bus mastering off on every function, no SPI enabled
at the distributor, xHCI USBCMD.R/S clear and USBSTS.HCHalted set.

Cases: qemu-xhci and nec-usb-xhci, each at EL1 (PSCI over HVC) and EL2
(virtualization=on, PSCI over SMC). Nothing here runs on hardware.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import socket
import subprocess
import sys
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

# QEMU virt facts: PL011 0x09000000, GICv3 distributor 0x08000000 and
# redistributors 0x080a0000 (128 KiB frames), high ECAM 0x4010000000, 32-bit
# PCI MMIO window from 0x10000000. Register offsets: GICv3 architecture
# specification, PCI local bus header, xHCI 1.2 chapter 5.
PAYLOAD_MAIN = r"""
#include <stdint.h>
#define UART 0x09000000UL
#define GICD 0x08000000UL
#define GICR 0x080a0000UL
#define ECAM 0x4010000000UL
#define BAR  0x10000000UL
#define SYSREG(name) ({ uint64_t v; __asm__ volatile("mrs %0, " #name : "=r"(v)); v; })

static void out(char c) { *(volatile uint32_t *)UART = (uint32_t)c; }
static void text(const char *s) { while (*s) out(*s++); }
static void hex(uint64_t v, int digits) { for (int i = digits * 4 - 4; i >= 0; i -= 4) out("0123456789abcdef"[(v >> i) & 15]); }
static uint32_t r32(uint64_t a) { return *(volatile uint32_t *)a; }
static void w32(uint64_t a, uint32_t v) { *(volatile uint32_t *)a = v; }
static void reg(const char *name, uint64_t key, uint64_t value)
{
    text("DUMP "); text(name); out(' '); hex(key, 10); out(' '); hex(value, 16); out('\n');
}
static void mmio(const char *name, uint64_t address) { reg(name, address, r32(address)); }

static void gic(void)
{
    uint32_t lines = ((r32(GICD + 4) & 31) + 1) * 32;
    if (lines > 1020) lines = 1020;
    mmio("gicd_ctlr", GICD);
    for (uint32_t w = 1; w < (lines + 31) / 32; ++w) {
        mmio("igroupr", GICD + 0x80 + 4 * w);
        mmio("isenabler", GICD + 0x100 + 4 * w);
        mmio("ispendr", GICD + 0x200 + 4 * w);
        mmio("isactiver", GICD + 0x300 + 4 * w);
        mmio("igrpmodr", GICD + 0xd00 + 4 * w);
    }
    for (uint32_t i = 8; i < lines / 4; ++i) mmio("ipriorityr", GICD + 0x400 + 4 * i);
    for (uint32_t i = 2; i < lines / 16; ++i) mmio("icfgr", GICD + 0xc00 + 4 * i);
    for (uint32_t n = 32; n < lines; ++n) {
        mmio("irouter_lo", GICD + 0x6000 + 8 * n);
        mmio("irouter_hi", GICD + 0x6004 + 8 * n);
    }
    for (uint64_t rd = GICR;; rd += 0x20000) {
        uint64_t sgi = rd + 0x10000;
        mmio("gicr_ctlr", rd);
        mmio("gicr_waker", rd + 0x14);
        mmio("gicr_igroupr0", sgi + 0x80);
        mmio("gicr_isenabler0", sgi + 0x100);
        mmio("gicr_ispendr0", sgi + 0x200);
        mmio("gicr_isactiver0", sgi + 0x300);
        mmio("gicr_icfgr1", sgi + 0xc04);
        for (uint32_t i = 0; i < 8; ++i) mmio("gicr_ipriorityr", sgi + 0x400 + 4 * i);
        if (r32(rd + 8) & 0x10) break; /* GICR_TYPER.Last */
    }
    reg("icc_pmr_el1", 0, SYSREG(icc_pmr_el1));
    reg("icc_igrpen1_el1", 0, SYSREG(icc_igrpen1_el1));
    reg("daif", 0, SYSREG(daif));
    reg("cntv_ctl", 0, SYSREG(cntv_ctl_el0));
    reg("cntp_ctl", 0, SYSREG(cntp_ctl_el0));
}

static void xhci(uint64_t cfg)
{
    /* Assign BAR0 (64-bit) and enable memory decode only, as an OS would. */
    w32(cfg + 0x10, (uint32_t)BAR);
    w32(cfg + 0x14, 0);
    w32(cfg + 0x04, 2);
    uint32_t caplen = r32(BAR) & 0xff;
    uint64_t op = BAR + caplen, rt = BAR + (r32(BAR + 0x18) & ~0x1fu);
    for (uint32_t off = 0; off < 0x20; off += 4) reg("xhci_cap", off, r32(BAR + off));
    static const uint32_t ops[] = {0x00, 0x04, 0x08, 0x14, 0x18, 0x1c, 0x30, 0x34, 0x38};
    for (unsigned i = 0; i < sizeof(ops) / sizeof(ops[0]); ++i) reg("xhci_op", ops[i], r32(op + ops[i]));
    uint32_t ports = r32(BAR + 4) >> 24;
    for (uint32_t p = 0; p < ports; ++p)
        for (uint32_t off = 0; off < 0x10; off += 4)
            reg("xhci_port", 0x400 + 0x10 * p + off, r32(op + 0x400 + 0x10 * p + off));
    for (uint32_t off = 0x20; off < 0x40; off += 4) reg("xhci_ir0", off, r32(rt + off));
    uint32_t xecp = (r32(BAR + 0x10) >> 16) * 4;
    for (unsigned guard = 0; xecp && guard < 16; ++guard) {
        for (uint32_t off = 0; off < 16; off += 4) reg("xhci_xcap", xecp + off, r32(BAR + xecp + off));
        uint32_t next = (r32(BAR + xecp) >> 8) & 0xff;
        xecp = next ? xecp + next * 4 : 0;
    }
    w32(cfg + 0x04, 0);
    w32(cfg + 0x10, 0);
}

void payload_main(void)
{
    uint64_t el = (SYSREG(CurrentEL) >> 2) & 3;
    text("PAYLOAD_" PAYLOAD_NAME "_BOOTED EL"); out((char)('0' + el)); out('\n');
#if PAYLOAD_DUMP
    reg("el", 0, el);
    gic();
    uint64_t xhci_cfg = 0;
    for (uint32_t dev = 0; dev < 32; ++dev) {
        for (uint32_t fn = 0; fn < 8; ++fn) {
            uint64_t c = ECAM + ((uint64_t)dev << 15) + ((uint64_t)fn << 12);
            if ((r32(c) & 0xffff) == 0xffff) {
                if (!fn) break;
                continue;
            }
            uint32_t bdf = dev << 3 | fn;
            reg("pci_id", bdf, r32(c));
            for (uint32_t off = 0; off < 0x100; off += 4) reg("pci_cfg", bdf << 8 | off, r32(c + off));
            if ((r32(c + 8) >> 8) == 0x0c0330 && !xhci_cfg) xhci_cfg = c;
            if (!fn && !(r32(c + 0xc) & 0x800000)) break; /* single function */
        }
    }
    if (xhci_cfg) xhci(xhci_cfg);
    else text("DUMP_NO_XHCI\n");
    text("DUMP_DONE\n");
#endif
    register uint64_t x0 __asm__("x0") = 0x84000008; /* PSCI SYSTEM_OFF */
    if (el == 2) __asm__ volatile("smc #0" : : "r"(x0));
    else __asm__ volatile("hvc #0" : : "r"(x0));
    for (;;) {}
}
"""

CONTROLLERS = {
    # INTx only: qemu-xhci has no msi/msix switch in QEMU 8.2 and kShimC's
    # ECAM host offers no MSI; nec-usb-xhci gets MSI off explicitly.
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


def build_runtime(root, out, jobs):
    run([sys.executable, root / "tools/build_runtime.py", "--config", root / "configs/qemu_usb_defconfig",
         "--out", out, "--jobs", str(jobs)], stdout=subprocess.DEVNULL)
    return out / "kShimC.bin"


def build_payloads(out):
    (out / "entry.S").write_text(PAYLOAD_ENTRY)
    (out / "payload.c").write_text(PAYLOAD_MAIN)
    images = {}
    for name, dump in (("A", 0), ("B", 1)):
        elf = out / f"payload-{name}.elf"
        run(["aarch64-linux-gnu-gcc", "-nostdlib", "-static", "-ffreestanding", "-fno-pic", "-Os",
             "-march=armv8-a", "-mgeneral-regs-only", "-mstrict-align", "-Wl,--build-id=none",
             "-Wl,-Ttext=0", "-Wl,-e,_start", f'-DPAYLOAD_NAME="{name}"', f"-DPAYLOAD_DUMP={dump}",
             out / "entry.S", out / "payload.c", "-o", elf])
        images[name] = out / f"payload-{name}.bin"
        run(["aarch64-linux-gnu-objcopy", "-O", "binary", elf, images[name]])
    return images


def pack(packer, runtime, payloads, out):
    config = out / "usb.cfg"
    packed = out / "usb.bin"
    # Entry A is the default; the long timeout leaves the choice to the keys
    # (the first key press cancels autoboot).
    config.write_text(f"""[Pack]
Shim={runtime}
Output={packed}
Default=Image-A
Timeout=30000
[Image-A]
Name=Entry A
Path={payloads['A']}
Type=Linux
BaseImage=true
[Image-B]
Name=Entry B
Path={payloads['B']}
Type=Linux
BaseImage=false
[Manifest]
Type=Manifest
""")
    with (out / "pack.log").open("w") as log:
        run([packer, config], stdout=log, stderr=subprocess.STDOUT)
    return packed


class Serial:
    """Console capture in raw chunks: the menu prompt has no newline."""

    def __init__(self, process):
        self.text = []
        self.changed = threading.Condition()
        threading.Thread(target=self._read, args=(process,), daemon=True).start()

    def _read(self, process):
        while True:
            chunk = os.read(process.stdout.fileno(), 4096)
            if not chunk:
                break
            with self.changed:
                self.text.append(chunk.decode(errors="replace"))
                self.changed.notify_all()

    def wait(self, pattern, timeout):
        deadline = time.monotonic() + timeout
        with self.changed:
            while not re.search(pattern, "".join(self.text)):
                left = deadline - time.monotonic()
                if left <= 0:
                    return False
                self.changed.wait(min(left, 0.2))
        return True


class Monitor:
    """The QEMU human monitor (HMP) on a UNIX socket."""

    def __init__(self, path, timeout):
        deadline = time.monotonic() + timeout
        while True:
            try:
                self.sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
                self.sock.connect(str(path))
                break
            except OSError:
                if time.monotonic() > deadline:
                    raise
                time.sleep(0.05)
        self.sock.settimeout(5)
        self.transcript = []
        self._until_prompt()

    def _until_prompt(self):
        data = b""
        while not data.endswith(b"(qemu) "):
            chunk = self.sock.recv(4096)
            if not chunk:
                break
            data += chunk
        self.transcript.append(data.decode(errors="replace"))

    def command(self, line):
        self.sock.sendall(line.encode() + b"\n")
        self._until_prompt()

    def close(self):
        self.sock.close()


def dump(output):
    lines = [line.strip() for line in output.splitlines() if line.startswith("DUMP ")]
    return lines if "DUMP_DONE" in output and lines else None


def parse(lines):
    """{(name, key): value} from the payload's dump lines."""
    values = {}
    for line in lines:
        _, name, key, value = line.split()
        values[(name, int(key, 16))] = int(value, 16)
    return values


def qemu_command(controller, image, el2, monitor=None):
    machine = f"virt,gic-version=3,virtualization={'on' if el2 else 'off'}"
    command = ["qemu-system-aarch64", "-machine", machine, "-accel", "tcg", "-cpu", "cortex-a76",
               "-smp", "1", "-m", "256M", "-display", "none", "-serial", "stdio", "-no-reboot",
               "-device", CONTROLLERS[controller]["device"],
               "-device", "usb-hub,bus=xhci.0,port=1,id=hub",
               "-device", "usb-kbd,bus=xhci.0,port=1.1",
               "-device", "usb-mouse,bus=xhci.0,port=1.2",
               "-kernel", image]
    command += ["-monitor", f"unix:{monitor},server=on,wait=off"] if monitor else ["-monitor", "none"]
    return [str(x) for x in command]


def boot_direct(controller, payload, el2, timeout):
    output = subprocess.run(qemu_command(controller, payload, el2), stdin=subprocess.DEVNULL,
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                            timeout=timeout).stdout.decode(errors="replace")
    return output


def boot_menu(controller, packed, el2, out, name, timeout):
    """Boot kShimC, drive the menu over the monitor; returns (log, monitor transcript, notes)."""
    socket_path = out / f"{name}.monitor"
    socket_path.unlink(missing_ok=True)
    process = subprocess.Popen(qemu_command(controller, packed, el2, socket_path),
                               stdin=subprocess.DEVNULL, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    serial = Serial(process)
    notes, transcript = [], []
    try:
        monitor = Monitor(socket_path, 10)
        ready = (serial.wait(r"USB: \S+\.1 if0 bound to usb-hid", timeout) and
                 serial.wait(r"USB: \S+\.2 if0 bound to usb-hid", timeout) and
                 serial.wait(r"Select kernel and press Enter: ", timeout))
        if not ready:
            notes.append("menu or HID binding not reached")
        else:
            # Pointer traffic first: it must not disturb the key path.
            monitor.command("mouse_move 20 -10")
            time.sleep(0.3)
            monitor.command("sendkey down")
            if not serial.wait(r"> Entry B", 10):
                notes.append("selection did not move to entry B")
            monitor.command("sendkey ret")
            if not serial.wait(r"DUMP_DONE|PAYLOAD_A_BOOTED", timeout):
                notes.append("no payload after Enter")
        transcript = monitor.transcript
        monitor.close()
    finally:
        try:
            process.wait(timeout=30)
        except subprocess.TimeoutExpired:
            process.kill()
            process.wait()
            notes.append("timeout")
    return "".join(serial.text), transcript, notes


def check_kshimc_log(log, controller):
    """Evidence and problems from the kShimC part of the log."""
    evidence, problems = {}, []
    ident = CONTROLLERS[controller]["id"]
    m = re.search(rf"PCI: 0000:00:([0-9a-f]{{2}})\.0 \[{ident}\] class 0x0c0330 BAR0 \S+ "
                  r"0x([0-9a-f]+)\+0x([0-9a-f]+)[^\n]* INTA->(\d+)", log)
    if m:
        evidence["xhci_function"] = f"00:{m.group(1)}.0"
        evidence["intx_intid"] = int(m.group(4))
    else:
        problems.append("xHCI function not enumerated with BAR0 and INTx")
    if not re.search(r"PCI: 0000:00:[0-9a-f]{2}\.0 bound to xhci", log):
        problems.append("xHCI driver not bound")
    if not re.search(r"USB: \S+ registered: \d+ root ports, interrupt-driven", log):
        problems.append("xHCI not interrupt-driven")
    if not re.search(r"USB: \S+ if0 bound to usb-hub", log):
        problems.append("usb-hub not bound")
    for port, kind in (("1", "keyboard"), ("2", "pointer")):
        if not re.search(rf"USB: \S+\.{port} if0 bound to usb-hid", log):
            problems.append(f"{kind} behind the hub not bound")
    if "> Entry B" not in log:
        problems.append("menu selection did not move")
    stopped = re.search(r"xhci: \S+: stopped \(USBCMD 0x([0-9a-f]+) USBSTS 0x([0-9a-f]+), (\d+) interrupts\)", log)
    if stopped:
        evidence["xhci_stop"] = {"usbcmd": stopped.group(1), "usbsts": stopped.group(2),
                                 "interrupts": int(stopped.group(3))}
        if int(stopped.group(3)) == 0:
            problems.append("no xHCI interrupt taken")
    else:
        problems.append("no xHCI stop at handoff")
    disconnected = re.findall(r"USB: (\S+) disconnected", log)
    evidence["disconnect_order"] = disconnected
    hub = next((i for i, path in enumerate(disconnected) if "." not in path), None)
    if hub is None or any("." in path for path in disconnected[hub + 1:]):
        problems.append("hub not disconnected after its children")
    if "refusing payload handoff" in log or "quiesce failed" in log or "Device shutdown incomplete" in log:
        problems.append("handoff refused or a quiesce failed")
    if "PAYLOAD_A_BOOTED" in log:
        problems.append("the default entry A booted")
    if "PAYLOAD_B_BOOTED" not in log:
        problems.append("entry B did not boot")
    return evidence, problems


def check_handoff(values):
    """Explicit quiesce checks on the payload's view."""
    problems = []
    for (name, key), value in values.items():
        if name == "pci_cfg" and key & 0xff == 0x04 and value & 0x4:
            problems.append(f"bus mastering on at {key >> 11:02x}.{(key >> 8) & 7}")
        if name == "isenabler" and value:
            problems.append(f"SPIs enabled at GICD_ISENABLER 0x{key:x}: 0x{value:08x}")
    usbcmd, usbsts = values.get(("xhci_op", 0x00)), values.get(("xhci_op", 0x04))
    if usbcmd is None or usbsts is None:
        problems.append("no xHCI operational registers in the dump")
    else:
        if usbcmd & 1:
            problems.append("xHCI USBCMD.R/S set")
        if usbcmd & 0x4:
            problems.append("xHCI USBCMD.INTE set")
        if not usbsts & 1:
            problems.append("xHCI USBSTS.HCHalted clear")
    return problems


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--out", type=Path, default=Path("build/qemu-usb"))
    parser.add_argument("--packer", type=Path, default=None,
                        help="MultiBootKernelPatcher checkout (default: searched next to the tree)")
    parser.add_argument("--runtime", type=Path, default=None,
                        help="prebuilt configs/qemu_usb_defconfig kShimC.bin (default: build it)")
    parser.add_argument("--timeout", type=float, default=90.0)
    parser.add_argument("--jobs", type=int, default=8)
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    out = args.out.resolve()
    out.mkdir(parents=True, exist_ok=True)
    packer_root = find_packer(root, args.packer)
    with (out / "packer-build.log").open("w") as log:
        run(["cmake", "-S", packer_root, "-B", out / "packer", "-DBUILD_SHIMS=OFF"],
            stdout=log, stderr=subprocess.STDOUT)
        run(["cmake", "--build", out / "packer", "-j4"], stdout=log, stderr=subprocess.STDOUT)
    runtime = args.runtime.resolve() if args.runtime else build_runtime(root, out / "runtime", args.jobs)
    payloads = build_payloads(out)
    packed = pack(out / "packer/MultiBootKernelPatcher", runtime, payloads, out)

    results = []
    for controller in CONTROLLERS:
        for el2 in (False, True):
            name = f"{controller}-{'el2' if el2 else 'el1'}"
            start = time.monotonic()
            direct_log = boot_direct(controller, payloads["B"], el2, 60)
            (out / f"{name}-direct.log").write_text(direct_log)
            log, transcript, notes = boot_menu(controller, packed, el2, out, name, args.timeout)
            (out / f"{name}.log").write_text(log)
            (out / f"{name}.monitor.log").write_text("".join(transcript))
            evidence, problems = check_kshimc_log(log, controller)
            problems = notes + problems
            direct, handoff = dump(direct_log), dump(log)
            differences = []
            if direct is None:
                problems.append("direct boot did not dump")
            if handoff is None:
                problems.append("entry B did not dump after the handoff")
            if direct and handoff:
                differences = [(a, b) for a, b in zip(direct, handoff) if a != b]
                if len(direct) != len(handoff):
                    differences.append(("register count", f"{len(direct)} != {len(handoff)}"))
                if differences:
                    problems.append(f"{len(differences)} registers differ from a direct boot")
                problems += check_handoff(parse(handoff))
            evidence["registers_compared"] = len(handoff or [])
            results.append({"name": name, "passed": not problems, "problems": problems,
                            "evidence": evidence, "differences": differences[:32],
                            "seconds": round(time.monotonic() - start, 1)})
            print(f"{name}: {'PASS' if not problems else 'FAIL ' + '; '.join(problems)} "
                  f"({len(handoff or [])} registers compared)", flush=True)
            for a, b in differences[:12]:
                print(f"  direct: {a}\n  kshimc: {b}")
    report = {"runtime_sha256": hashlib.sha256(runtime.read_bytes()).hexdigest(),
              "qemu": subprocess.check_output(["qemu-system-aarch64", "--version"], text=True).splitlines()[0],
              "hardware_tested": False, "tests": results, "passed": all(r["passed"] for r in results)}
    (out / "validation.json").write_text(json.dumps(report, indent=2) + "\n")
    print(f"{sum(r['passed'] for r in results)}/{len(results)} passed")
    return 0 if report["passed"] else 1


if __name__ == "__main__":
    sys.exit(main())
