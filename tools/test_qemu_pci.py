#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""PCI enumeration and handoff check on QEMU virt (gpex ECAM host).

The runtime (configs/qemu_pci_defconfig: KSHIM_PCI, KSHIM_PCI_ECAM,
KSHIM_PCI_SELFTEST) is packed as a Manifest image whose default entry is a
dump payload. QEMU gets a topology with every case the PCI core handles:

  00:01.0  qemu-xhci                      root bus, 64-bit BAR
  00:02.0  pcie-root-port (rp1)           PCIe port: only device 0 below
  01:00.0    nec-usb-xhci                 behind the root port
  00:03.0  pci-bridge (br2)               conventional bridge with an SHPC BAR
  02:05.0    qemu-xhci                    INTx swizzled through the bridge
  00:04.0  qemu-xhci, function 0 + 1      multifunction

The serial log must show every function enumerated, every BAR assigned
inside the host's MMIO window, both bridges numbered and their windows
opened, and the self-test reading each xHCI's capability registers through
BAR0 (CAPLENGTH 0x40, HCIVERSION 0x0100) and resolving its INTx through the
interrupt-map (GIC SPI 3-6 with the gpex swizzle).

The payload then dumps the GICv3 state (as tests/handoff_qemu.py does) and
the first 256 bytes of configuration space of every function reachable by
ECAM, and powers off. The same payload booted directly, without kShimC,
must produce an identical dump: the config journal must have put back every
BAR, command register, bridge bus number and window, at EL1 (hvc) and EL2
(smc).
"""
import argparse
import json
from pathlib import Path
import re
import subprocess

ECAM = 0x4010000000
MMIO32 = (0x10000000, 0x10000000 + 0x2eff0000)

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

PAYLOAD_MAIN = r"""
#include <stdint.h>
#define UART 0x09000000UL
#define GICD 0x08000000UL
#define GICR 0x080a0000UL
#define ECAM 0x4010000000UL
#define SYSREG(name) ({ uint64_t v; __asm__ volatile("mrs %0, " #name : "=r"(v)); v; })

static void out(char c) { *(volatile uint32_t *)UART = (uint32_t)c; }
static void text(const char *s) { while (*s) out(*s++); }
static void hex(uint64_t v) { for (int i = 60; i >= 0; i -= 4) out("0123456789abcdef"[(v >> i) & 15]); }
static uint32_t r32(uint64_t a) { return *(volatile uint32_t *)a; }
static void reg(const char *name, uint64_t key, uint64_t value)
{
    text("HANDOFF "); text(name); out(' '); hex(key); out(' '); hex(value); out('\n');
}
static void mmio(const char *name, uint64_t address) { reg(name, address, r32(address)); }

static void pci(void)
{
    unsigned functions = 0;
    for (uint64_t bus = 0; bus < 8; ++bus)
        for (uint64_t dev = 0; dev < 32; ++dev)
            for (uint64_t fn = 0; fn < 8; ++fn) {
                uint64_t base = ECAM + (bus << 20) + (dev << 15) + (fn << 12);
                uint32_t id = r32(base);
                if ((id & 0xffff) == 0xffff || (id & 0xffff) == 0) {
                    if (!fn) break;
                    continue;
                }
                ++functions;
                for (uint64_t off = 0; off < 0x100; off += 4) mmio("pcicfg", base + off);
                if (!fn && !(r32(base + 0xc) & 0x800000)) break;
            }
    reg("pci_functions", 0, functions);
}

void payload_main(void)
{
    uint64_t el = (SYSREG(CurrentEL) >> 2) & 3;
    uint32_t lines = ((r32(GICD + 4) & 31) + 1) * 32;
    if (lines > 1020) lines = 1020;
    reg("el", 0, el);
    reg("daif", 0, SYSREG(daif));
    mmio("gicd_ctlr", GICD);
    for (uint32_t w = 1; w < (lines + 31) / 32; ++w) {
        mmio("isenabler", GICD + 0x100 + 4 * w);
        mmio("ispendr", GICD + 0x200 + 4 * w);
        mmio("isactiver", GICD + 0x300 + 4 * w);
    }
    for (uint32_t i = 2; i < lines / 16; ++i) mmio("icfgr", GICD + 0xc00 + 4 * i);
    for (uint32_t n = 32; n < 64; ++n) {
        mmio("irouter_lo", GICD + 0x6000 + 8 * n);
        mmio("irouter_hi", GICD + 0x6004 + 8 * n);
    }
    reg("icc_igrpen1_el1", 0, SYSREG(icc_igrpen1_el1));
    reg("icc_pmr_el1", 0, SYSREG(icc_pmr_el1));
    pci();
    text("HANDOFF_DONE\n");
    register uint64_t x0 __asm__("x0") = 0x84000008; /* PSCI SYSTEM_OFF */
    if (el == 2) __asm__ volatile("smc #0" : : "r"(x0));
    else __asm__ volatile("hvc #0" : : "r"(x0));
    for (;;) {}
}
"""

DEVICES = [
    "-nic", "none",
    "-device", "qemu-xhci,id=xhci0,bus=pcie.0,addr=0x1",
    "-device", "pcie-root-port,id=rp1,bus=pcie.0,addr=0x2,chassis=1,slot=1",
    "-device", "nec-usb-xhci,id=xhci1,bus=rp1",
    "-device", "pci-bridge,id=br2,bus=pcie.0,addr=0x3,chassis_nr=2",
    "-device", "qemu-xhci,id=xhci2,bus=br2,addr=0x5",
    "-device", "qemu-xhci,id=xhci3,bus=pcie.0,addr=0x4.0x0,multifunction=on",
    "-device", "qemu-xhci,id=xhci4,bus=pcie.0,addr=0x4.0x1",
]

# (bus, slot, function, vendor:device) the runtime must enumerate; buses
# behind bridges are assigned depth first.
EXPECTED = [
    (0, 1, 0, "1b36:000d"),
    (0, 2, 0, "1b36:000c"),
    (1, 0, 0, "1033:0194"),
    (0, 3, 0, "1b36:0001"),
    (2, 5, 0, "1b36:000d"),
    (0, 4, 0, "1b36:000d"),
    (0, 4, 1, "1b36:000d"),
]
XHCI = [(b, s, f) for b, s, f, i in EXPECTED if i in ("1b36:000d", "1033:0194")]


def run(command, **kwargs):
    return subprocess.run([str(x) for x in command], check=True, **kwargs)


def dump(output):
    lines = [line.strip() for line in output.splitlines() if line.startswith("HANDOFF ")]
    if "HANDOFF_DONE" not in output or not lines:
        raise AssertionError("payload did not finish its dump:\n" + output[-3000:])
    return lines


def gpex_intid(slot_on_root, pin):
    """GIC INTID of the gpex interrupt-map: SPI 3 + (slot + pin - 1) % 4."""
    return 32 + 3 + (slot_on_root + pin - 1) % 4


def check_enumeration(log):
    problems, evidence = [], {}
    for bus, slot, fn, ident in EXPECTED:
        pattern = (rf"PCI: 0000:{bus:02x}:{slot:02x}\.{fn} \[{ident}\] class 0x([0-9a-f]{{6}})"
                   r"([^\n]*)")
        m = re.search(pattern, log)
        if not m:
            problems.append(f"{bus:02x}:{slot:02x}.{fn} {ident} not enumerated")
            continue
        evidence[f"{bus:02x}:{slot:02x}.{fn}"] = m.group(0)
        for bar in re.finditer(r"BAR(\d) (mem64|mem|io) 0x([0-9a-f]+)\+0x([0-9a-f]+)( unassigned)?",
                               m.group(2)):
            index, kind, base, size, unassigned = bar.groups()
            base, size = int(base, 16), int(size, 16)
            if unassigned:
                problems.append(f"{bus:02x}:{slot:02x}.{fn} BAR{index} unassigned")
            elif kind.startswith("mem") and not (MMIO32[0] <= base and base + size <= MMIO32[1]):
                problems.append(f"{bus:02x}:{slot:02x}.{fn} BAR{index} outside the MMIO window")
            elif base % size:
                problems.append(f"{bus:02x}:{slot:02x}.{fn} BAR{index} misaligned")
    for name, buses in (("00:02.0", "01-01"), ("00:03.0", "02-02")):
        line = evidence.get(name, "")
        if f"bridge buses {buses}" not in line:
            problems.append(f"{name} bridge bus numbers (want {buses})")
    for bus, slot, fn in XHCI:
        m = re.search(rf"PCI_SELFTEST {bus:02x}:{slot:02x}\.{fn} \[[0-9a-f:]+\] bar0=0x([0-9a-f]+) "
                      r"caplength=0x([0-9a-f]+) hciversion=0x([0-9a-f]+) ports=(\d+) slots=(\d+) "
                      r"irq=(\w+) intid=(\d+)", log)
        if not m:
            problems.append(f"{bus:02x}:{slot:02x}.{fn} self-test missing")
            continue
        bar0, caplength, version, ports, slots, irq, intid = m.groups()
        evidence[f"selftest {bus:02x}:{slot:02x}.{fn}"] = m.group(0)
        if int(caplength, 16) != 0x40 or int(version, 16) != 0x100 or int(ports) == 0:
            problems.append(f"{bus:02x}:{slot:02x}.{fn} xHCI capability registers not readable")
        # INTA, swizzled by the device's slot when behind a bridge, then
        # mapped with the slot of the root-bus device it arrives through.
        if bus == 0:
            root_slot, pin = slot, 1
        else:
            root_slot, pin = {1: 2, 2: 3}[bus], (slot % 4) + 1
        if irq != "intx" or int(intid) != gpex_intid(root_slot, pin):
            problems.append(f"{bus:02x}:{slot:02x}.{fn} INTx {irq}/{intid}, want "
                            f"{gpex_intid(root_slot, pin)}")
    return problems, evidence


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--runtime", type=Path, default=Path("build/fw-qemu_pci/kShimC.bin"),
                        help="kShimC.bin built from configs/qemu_pci_defconfig")
    parser.add_argument("--out", type=Path, default=Path("build/pci-qemu"))
    parser.add_argument("--packer", type=Path, default=None,
                        help="MultiBootKernelPatcher checkout (default: ../MultiBootKernelPatcher)")
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    packer_root = (args.packer or root.parent / "MultiBootKernelPatcher").resolve()
    out = args.out.resolve()
    out.mkdir(parents=True, exist_ok=True)
    runtime = args.runtime.resolve()

    run(["qemu-system-aarch64", "-machine", f"virt,gic-version=3,dumpdtb={out / 'virt.dtb'}",
         "-cpu", "cortex-a76", "-m", "256M", "-display", "none", *DEVICES],
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    dts = run(["dtc", "-I", "dtb", "-O", "dts", out / "virt.dtb"], capture_output=True,
              text=True).stdout
    if "reg = <0x40 0x10000000 0x00 0x10000000>" not in dts:
        raise SystemExit("unexpected gpex ECAM location in the QEMU virt DT")

    with (out / "packer-build.log").open("w") as log:
        run(["cmake", "-S", packer_root, "-B", out / "packer", "-DBUILD_SHIMS=OFF"],
            stdout=log, stderr=subprocess.STDOUT)
        run(["cmake", "--build", out / "packer", "-j4"], stdout=log, stderr=subprocess.STDOUT)
    (out / "entry.S").write_text(PAYLOAD_ENTRY)
    (out / "payload.c").write_text(PAYLOAD_MAIN)
    run(["aarch64-linux-gnu-gcc", "-nostdlib", "-static", "-ffreestanding", "-fno-pic", "-Os",
         "-march=armv8-a", "-mgeneral-regs-only", "-mstrict-align", "-Wl,--build-id=none",
         "-Wl,-Ttext=0", "-Wl,-e,_start", out / "entry.S", out / "payload.c",
         "-o", out / "payload.elf"])
    run(["aarch64-linux-gnu-objcopy", "-O", "binary", out / "payload.elf", out / "payload.bin"])
    config, packed = out / "pci.cfg", out / "pci.bin"
    config.write_text(f"""[Pack]
Shim={runtime}
Output={packed}
Default=Image-A
Timeout=300
[Image-A]
Name=Dump A
Path={out / 'payload.bin'}
Type=Linux
BaseImage=true
[Image-B]
Name=Dump B
Path={out / 'payload.bin'}
BaseImage=false
Type=Linux
[Manifest]
Type=Manifest
""")
    with (out / "pack.log").open("w") as log:
        run([out / "packer/MultiBootKernelPatcher", config], stdout=log, stderr=subprocess.STDOUT)

    results = []
    for virt in ("off", "on"):
        el = "el2-smc" if virt == "on" else "el1-hvc"
        dumps, logs = {}, {}
        for case, image in (("direct", out / "payload.bin"), ("kshimc", packed)):
            command = ["qemu-system-aarch64", "-machine", f"virt,gic-version=3,virtualization={virt}",
                       "-accel", "tcg", "-cpu", "cortex-a76", "-smp", "2", "-m", "256M",
                       "-display", "none", "-monitor", "none", "-serial", "stdio",
                       "-no-reboot", *DEVICES, "-kernel", image]
            output = run(command, stdin=subprocess.DEVNULL, stdout=subprocess.PIPE,
                         stderr=subprocess.STDOUT, timeout=120).stdout.decode(errors="replace")
            (out / f"{el}-{case}.log").write_text(output)
            logs[case] = output
            dumps[case] = dump(output)
        problems, evidence = check_enumeration(logs["kshimc"])
        differences = [(a, b) for a, b in zip(dumps["direct"], dumps["kshimc"]) if a != b]
        if len(dumps["direct"]) != len(dumps["kshimc"]):
            differences.append(("register count", f"{len(dumps['direct'])} != {len(dumps['kshimc'])}"))
        functions = next((l.split()[-1] for l in dumps["direct"] if "pci_functions" in l), "0")
        passed = not problems and not differences
        results.append({"case": el, "registers": len(dumps["direct"]),
                        "functions_at_handoff": int(functions, 16), "problems": problems,
                        "differences": differences[:32], "evidence": evidence, "passed": passed})
        print(f"{el}: {'PASS' if passed else 'FAIL'} ({len(EXPECTED)} functions enumerated, "
              f"{len(XHCI)} xHCI self-tests, {len(dumps['direct'])} handoff registers, "
              f"{len(differences)} differ)", flush=True)
        for problem in problems:
            print(f"  {problem}")
        for direct, kshimc in differences[:16]:
            print(f"  direct: {direct}\n  kshimc: {kshimc}")
    (out / "validation.json").write_text(json.dumps({"tests": results}, indent=2) + "\n")
    if not all(r["passed"] for r in results):
        raise SystemExit(1)


if __name__ == "__main__":
    main()
