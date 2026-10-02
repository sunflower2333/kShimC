#!/usr/bin/env python3
"""Exercise Manifest handoff after the real LVGL framebuffer menu."""

import argparse
import hashlib
import json
from pathlib import Path
import shutil
import subprocess
import time


FRAMEBUFFER_ADDRESS = 0x44000000
FRAMEBUFFER_PIXELS = 320 * 240
FREEEXEC_ADDRESS = 0x47000000
BASE_IMAGE_ADDRESS = 0x40200000


def run(command, **kwargs):
    return subprocess.run([str(item) for item in command], check=True, **kwargs)


def payload_source(identity, expected_address):
    success = (
        f"PAYLOAD_{identity}_PASS dtb=ok mmu=off cpus=off "
        f"framebuffer=varied identity="
        f"{'in-place' if identity == 'A' else 'copied'}\\n"
    )
    return f'''/* Position-independent handoff verifier. */
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
    .ascii "ARM\\x64"
    .long 0

start:
    mov x19, x0
    cbz x19, fail_dtb
    ldr w1, [x19]
    mov w2, #0x0dd0
    movk w2, #0xedfe, lsl #16
    cmp w1, w2
    b.ne fail_dtb

    adr x1, _start
    ldr x2, ={expected_address:#x}
    cmp x1, x2
    b.ne fail_identity

    mrs x1, CurrentEL
    lsr x1, x1, #2
    cmp x1, #1
    b.eq read_sctlr_el1
    cmp x1, #2
    b.ne fail_el
    mrs x2, sctlr_el2
    b check_sctlr
read_sctlr_el1:
    mrs x2, sctlr_el1
check_sctlr:
    tbnz x2, #0, fail_mmu

    mov x20, #1
check_cpus:
    mov x0, #4
    movk x0, #0xc400, lsl #16
    mov x1, x20
    mov x2, #0
    hvc #0
    cmp x0, #1
    b.ne fail_cpus
    add x20, x20, #1
    cmp x20, #4
    b.lo check_cpus

    ldr x3, ={FRAMEBUFFER_ADDRESS:#x}
    ldr w4, [x3]
    cmp w4, #0
    cset w7, ne
    mov w6, #0
    mov x5, #1
    ldr x8, ={FRAMEBUFFER_PIXELS}
check_framebuffer:
    ldr w9, [x3, x5, lsl #2]
    cmp w9, #0
    cset w10, ne
    orr w7, w7, w10
    cmp w9, w4
    cset w10, ne
    orr w6, w6, w10
    add x5, x5, #1
    cmp x5, x8
    b.lo check_framebuffer
    cbz w7, fail_framebuffer_blank
    cbz w6, fail_framebuffer_flat

    adr x4, message_success
    b print
fail_dtb:
    adr x4, message_dtb
    b print
fail_identity:
    adr x4, message_identity
    b print
fail_el:
    adr x4, message_el
    b print
fail_mmu:
    adr x4, message_mmu
    b print
fail_cpus:
    adr x4, message_cpus
    b print
fail_framebuffer_blank:
    adr x4, message_framebuffer_blank
    b print
fail_framebuffer_flat:
    adr x4, message_framebuffer_flat

print:
    ldr x5, =0x09000000
1:
    ldrb w6, [x4], #1
    cbz w6, power_off
    str w6, [x5]
    b 1b
power_off:
    mov x0, #8
    movk x0, #0x8400, lsl #16
    hvc #0
2:
    wfe
    b 2b

    .ltorg
message_success: .asciz "{success}"
message_dtb: .asciz "PAYLOAD_{identity}_FAIL dtb\\n"
message_identity: .asciz "PAYLOAD_{identity}_FAIL identity\\n"
message_el: .asciz "PAYLOAD_{identity}_FAIL exception-level\\n"
message_mmu: .asciz "PAYLOAD_{identity}_FAIL mmu-enabled\\n"
message_cpus: .asciz "PAYLOAD_{identity}_FAIL secondary-cpu-online\\n"
message_framebuffer_blank: .asciz "PAYLOAD_{identity}_FAIL framebuffer-blank\\n"
message_framebuffer_flat: .asciz "PAYLOAD_{identity}_FAIL framebuffer-flat\\n"
'''


def build_payload(out, identity, expected_address):
    assembly = out / f"payload-{identity}.S"
    elf = out / f"payload-{identity}.elf"
    binary = out / f"payload-{identity}.bin"
    assembly.write_text(payload_source(identity, expected_address))
    run([
        "aarch64-linux-gnu-gcc", "-nostdlib", "-static",
        "-Wl,--build-id=none", "-Wl,-Ttext=0", assembly, "-o", elf,
    ], stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    run(["aarch64-linux-gnu-objcopy", "-O", "binary", elf, binary])
    return binary


def write_pack_config(path, runtime, output, payload_a, payload_b, default):
    path.write_text(f"""[Pack]
Shim={runtime}
Output={output}
Default=Image-{default}
Timeout=1500

[Image-A]
Name=Linux A
Path={payload_a}
Type=Linux
BaseImage=true

[Image-B]
Name=FreeExec B
Path={payload_b}
Type=FreeExec
BaseImage=false
CopyTo={FREEEXEC_ADDRESS:#x}
CopySizeMax=0x10000

[Manifest]
Type=Manifest
""")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build", type=Path,
                        default=Path("build/qemu-manifest-ui"))
    parser.add_argument("--timeout", type=float, default=45.0)
    parser.add_argument("--no-build", action="store_true")
    args = parser.parse_args()

    root = Path(__file__).resolve().parents[1]
    packer_root = root.parent / "MultiBootKernelPatcher"
    out = args.build.resolve()
    out.mkdir(parents=True, exist_ok=True)

    if not args.no_build:
        shutil.copy2(root / "configs/qemu_manifest_ui_defconfig",
                     out / ".config")
        with (out / "configure.log").open("w") as log:
            run([
                "cmake", "-S", root, "-B", out, "-G", "Ninja",
                "-DKSHIM_STANDALONE=OFF",
            ], stdout=log, stderr=subprocess.STDOUT)
        with (out / "build.log").open("w") as log:
            run(["cmake", "--build", out, "-j8"], stdout=log,
                stderr=subprocess.STDOUT)

    runtime = out / "kShimC.bin"
    if runtime.stat().st_size != 4 * 1024 * 1024:
        raise RuntimeError("Manifest ABI requires an exact 4 MiB runtime slot")

    packer_build = out / "packer"
    with (out / "packer-build.log").open("w") as log:
        run(["cmake", "-S", packer_root, "-B", packer_build,
             "-DBUILD_SHIMS=OFF"], stdout=log, stderr=subprocess.STDOUT)
        run(["cmake", "--build", packer_build, "-j4"], stdout=log,
            stderr=subprocess.STDOUT)
    packer = packer_build / "MultiBootKernelPatcher"

    payload_a = build_payload(out, "A", BASE_IMAGE_ADDRESS)
    payload_b = build_payload(out, "B", FREEEXEC_ADDRESS)
    results = []

    for identity in ("A", "B"):
        name = f"default-{identity.lower()}"
        config = out / f"{name}.cfg"
        packed = out / f"{name}.bin"
        write_pack_config(config, runtime, packed, payload_a, payload_b,
                          identity)
        with (out / f"{name}-pack.log").open("w") as log:
            run([packer, config], stdout=log, stderr=subprocess.STDOUT)

        command = [
            "qemu-system-aarch64", "-machine",
            "virt,gic-version=3,virtualization=off", "-accel",
            "tcg,thread=multi", "-cpu", "cortex-a76", "-smp", "4",
            "-m", "256M", "-display", "none", "-monitor", "none",
            "-serial", "stdio", "-no-reboot", "-kernel", packed,
        ]
        started = time.monotonic()
        timed_out = False
        returncode = None
        try:
            result = subprocess.run(
                [str(item) for item in command], stdin=subprocess.DEVNULL,
                stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                timeout=args.timeout, check=False)
            output = result.stdout.decode(errors="replace")
            returncode = result.returncode
        except subprocess.TimeoutExpired as error:
            output = (error.stdout or b"").decode(errors="replace")
            timed_out = True

        expected = (
            f"PAYLOAD_{identity}_PASS dtb=ok mmu=off cpus=off "
            f"framebuffer=varied identity="
            f"{'in-place' if identity == 'A' else 'copied'}"
        )
        passed = not timed_out and returncode == 0 and expected in output
        (out / f"{name}.log").write_text(output)
        results.append({
            "case": name,
            "default": f"Image-{identity}",
            "packed_sha256": hashlib.sha256(packed.read_bytes()).hexdigest(),
            "command": [str(item) for item in command],
            "returncode": returncode,
            "timed_out": timed_out,
            "seconds": time.monotonic() - started,
            "passed": passed,
        })
        print(f"{name}: {'PASS' if passed else 'FAIL'}", flush=True)

    report = {
        "runtime_sha256": hashlib.sha256(runtime.read_bytes()).hexdigest(),
        "runtime_size": runtime.stat().st_size,
        "qemu": subprocess.check_output(
            ["qemu-system-aarch64", "--version"], text=True).splitlines()[0],
        "framebuffer": {
            "address": f"{FRAMEBUFFER_ADDRESS:#x}",
            "width": 320,
            "height": 240,
            "stride": 1280,
            "format": "ARGB8888",
        },
        "tests": results,
        "passed": all(item["passed"] for item in results),
    }
    (out / "validation.json").write_text(json.dumps(report, indent=2) + "\n")
    if not report["passed"]:
        raise SystemExit(1)


if __name__ == "__main__":
    main()
