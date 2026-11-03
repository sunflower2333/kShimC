#!/usr/bin/env python3
"""Check the interrupt state a payload receives from kShimC.

A position-independent payload dumps the GICv3 distributor, every
redistributor frame, its CPU interface, VBAR, DAIF, its timer and (EL2)
HCR_EL2.IMO, then powers off. It runs twice on QEMU virt (GICv3): booted
directly, so nothing touched the GIC, and as the default entry of a
Manifest image behind the kShimC runtime, which brought interrupts up and
tore them down in prepare_boot(). The dumps must be identical, at EL1 (hvc)
and EL2 (smc).
"""
import argparse
import json
from pathlib import Path
import subprocess

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

void payload_main(void)
{
    uint64_t el = (SYSREG(CurrentEL) >> 2) & 3;
    uint32_t lines = ((r32(GICD + 4) & 31) + 1) * 32;
    if (lines > 1020) lines = 1020;
    reg("el", 0, el);
    reg("daif", 0, SYSREG(daif));
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
        mmio("gicr_icfgr0", sgi + 0xc00);
        mmio("gicr_icfgr1", sgi + 0xc04);
        mmio("gicr_igrpmodr0", sgi + 0xd00);
        for (uint32_t i = 0; i < 8; ++i) mmio("gicr_ipriorityr", sgi + 0x400 + 4 * i);
        if (r32(rd + 8) & 0x10) break; /* GICR_TYPER.Last */
    }
    reg("icc_sre_el1", 0, SYSREG(icc_sre_el1));
    reg("icc_ctlr_el1", 0, SYSREG(icc_ctlr_el1));
    reg("icc_pmr_el1", 0, SYSREG(icc_pmr_el1));
    reg("icc_bpr1_el1", 0, SYSREG(icc_bpr1_el1));
    reg("icc_igrpen1_el1", 0, SYSREG(icc_igrpen1_el1));
    reg("cntv_ctl", 0, SYSREG(cntv_ctl_el0));
    reg("cntv_cval", 0, SYSREG(cntv_cval_el0));
    reg("cntp_ctl", 0, SYSREG(cntp_ctl_el0));
    reg("vbar_el1", 0, SYSREG(vbar_el1));
    if (el == 2) {
        reg("icc_sre_el2", 0, SYSREG(icc_sre_el2));
        reg("vbar_el2", 0, SYSREG(vbar_el2));
        reg("hcr_el2_imo", 0, SYSREG(hcr_el2) & 0x10);
        reg("cnthp_ctl", 0, SYSREG(cnthp_ctl_el2));
        reg("cnthp_cval", 0, SYSREG(cnthp_cval_el2));
    }
    text("HANDOFF_DONE\n");
    register uint64_t x0 __asm__("x0") = 0x84000008; /* PSCI SYSTEM_OFF */
    if (el == 2) __asm__ volatile("smc #0" : : "r"(x0));
    else __asm__ volatile("hvc #0" : : "r"(x0));
    for (;;) {}
}
"""


def run(command, **kwargs):
    return subprocess.run([str(x) for x in command], check=True, **kwargs)


def dump(output):
    lines = [line.strip() for line in output.splitlines() if line.startswith("HANDOFF ")]
    if "HANDOFF_DONE" not in output or not lines:
        raise AssertionError("payload did not finish its dump:\n" + output[-2000:])
    return lines


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--runtime", type=Path, default=Path("build/manifest/kShimC.bin"))
    parser.add_argument("--out", type=Path, default=Path("build/handoff-regression"))
    parser.add_argument("--packer", type=Path, default=None,
                        help="MultiBootKernelPatcher checkout (default: ../MultiBootKernelPatcher)")
    parser.add_argument("--smp", type=int, default=2)
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    packer_root = (args.packer or root.parent / "MultiBootKernelPatcher").resolve()
    out = args.out.resolve()
    out.mkdir(parents=True, exist_ok=True)
    runtime = args.runtime.resolve()
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

    config = out / "handoff.cfg"
    packed = out / "handoff.bin"
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
        dumps = {}
        for case, image in (("direct", out / "payload.bin"), ("kshimc", packed)):
            command = ["qemu-system-aarch64", "-machine", f"virt,gic-version=3,virtualization={virt}",
                       "-accel", "tcg", "-cpu", "cortex-a76", "-smp", str(args.smp), "-m", "256M",
                       "-display", "none", "-monitor", "none", "-serial", "stdio",
                       "-no-reboot", "-kernel", image]
            output = run(command, stdin=subprocess.DEVNULL, stdout=subprocess.PIPE,
                         stderr=subprocess.STDOUT, timeout=60).stdout.decode(errors="replace")
            (out / f"{el}-{case}.log").write_text(output)
            dumps[case] = dump(output)
        brought_up = "IRQ: GICv3" in (out / f"{el}-kshimc.log").read_text()
        differences = [(a, b) for a, b in zip(dumps["direct"], dumps["kshimc"]) if a != b]
        if len(dumps["direct"]) != len(dumps["kshimc"]):
            differences.append(("register count", f"{len(dumps['direct'])} != {len(dumps['kshimc'])}"))
        passed = brought_up and not differences
        results.append({"case": el, "registers": len(dumps["direct"]), "kshimc_irq_up": brought_up,
                        "differences": differences[:32], "passed": passed})
        print(f"{el}: {'PASS' if passed else 'FAIL'} ({len(dumps['direct'])} registers, "
              f"{len(differences)} differ, kShimC IRQ {'up' if brought_up else 'not up'})", flush=True)
        for direct, kshimc in differences[:16]:
            print(f"  direct: {direct}\n  kshimc: {kshimc}")
    (out / "validation.json").write_text(json.dumps({"tests": results}, indent=2) + "\n")
    if not all(r["passed"] for r in results):
        raise SystemExit(1)


if __name__ == "__main__":
    main()
