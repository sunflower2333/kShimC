#!/usr/bin/env python3
"""Exercise the sibling packer's actual Manifest ABI with the ARM64 runtime."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import struct


def run(command, **kwargs):
    return subprocess.run([str(x) for x in command], check=True, **kwargs)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--runtime", type=Path, default=Path("build/manifest/kShimC.bin"))
    parser.add_argument("--out", type=Path, default=Path("build/manifest-regression"))
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    packer_root = root.parent / "MultiBootKernelPatcher"
    out = args.out.resolve()
    out.mkdir(parents=True, exist_ok=True)
    runtime = args.runtime.resolve()
    assert runtime.stat().st_size == 4 * 1024 * 1024, "Manifest ABI requires the 4 MiB slot"
    with (out / "packer-build.log").open("w") as log:
        run(["cmake", "-S", packer_root, "-B", out / "packer", "-DBUILD_SHIMS=OFF"],
            stdout=log, stderr=subprocess.STDOUT)
        run(["cmake", "--build", out / "packer", "-j4"], stdout=log, stderr=subprocess.STDOUT)

    payload_dts = out / "payload.dts"
    payload_dtb = out / "payload.dtb"
    payload_dts.write_text('/dts-v1/; / { model="kshim-payload-fixture"; };\n')
    run(["dtc", "-I", "dts", "-O", "dtb", "-o", payload_dtb, payload_dts])
    for name in ("A", "B", "D"):
        assembly = out / (name + ".S")
        assembly.write_text('''.global _start
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
    cbz x0, fail
    ldr w1, [x0]
    mov w2, #0x0dd0
    movk w2, #0xedfe, lsl #16
    cmp w1, w2
    b.ne fail
    adr x4, message
    b print
fail:
    adr x4, error
print:
    mov x5, #0x09000000
1:  ldrb w6, [x4], #1
    cbz w6, finish
    str w6, [x5]
    b 1b
finish:
    mov x0, #8
    movk x0, #0x8400, lsl #16
    hvc #0
    b .
message: .asciz "PAYLOAD_''' + name + '''_PASS\\n"
error: .asciz "PAYLOAD_BAD_DTB\\n"
''')
        if name == "D":
            assembly.write_text(assembly.read_text().replace(
                "    adr x4, message", f"    ldr w1, [x0, #4]\n    rev w1, w1\n"
                f"    ldr w2, ={payload_dtb.stat().st_size}\n    cmp w1, w2\n"
                "    b.ne fail\n    adr x4, message"))
        run(["aarch64-linux-gnu-gcc", "-nostdlib", "-static", "-Wl,--build-id=none",
             "-Wl,-Ttext=0", assembly, "-o", out / (name + ".elf")], capture_output=True)
        run(["aarch64-linux-gnu-objcopy", "-O", "binary", out / (name + ".elf"),
             out / (name + ".bin")])

    results = []
    for name, default, key, copied in (("default", "Image-A", b"", False),
                                      ("select-linux", "Image-A", b"2\n", False),
                                      ("copy-freeexec", "Image-B", b"", True),
                                      ("payload-dtb", "Image-B", b"", False),
                                      ("short-dtb-entry", "Image-B", b"", False),
                                      ("bad-dtb-structure", "Image-B", b"", False)):
        config = out / (name + ".cfg")
        packed = out / (name + ".bin")
        extra = "Type=FreeExec\nCopyTo=0x47000000\nCopySizeMax=0x10000\n" if copied else "Type=Linux\n"
        with_dtb = name in ("payload-dtb", "short-dtb-entry", "bad-dtb-structure")
        dtb_section = ""
        base_reference = ""
        if with_dtb:
            extra += "DeviceTreeBlob=Image-DTB\n"
            # Both executable entries deliberately share this payload DT.
            # The sibling packer currently emits the base entry's reference
            # for secondary entries (pack.c); distinct overrides need its
            # own repair. This still proves separation from the runtime DT.
            base_reference = "DeviceTreeBlob=Image-DTB\n"
            dtb_section = f"[Image-DTB]\nName=Payload DT\nType=DTB\nPath={payload_dtb}\nBaseImage=false\n"
        config.write_text(f"""[Pack]
Shim={runtime}
Output={packed}
Default={default}
Timeout=1500
[Image-A]
Name=Linux A
Path={out / 'A.bin'}
Type=Linux
BaseImage=true
{base_reference}
[Image-B]
Name=Alternate B
Path={out / ('D.bin' if with_dtb else 'B.bin')}
BaseImage=false
{extra}
{dtb_section}
[Manifest]
Type=Manifest
""")
        with (out / (name + "-pack.log")).open("w") as log:
            run([out / "packer/MultiBootKernelPatcher", config], stdout=log, stderr=subprocess.STDOUT)
        if name in ("short-dtb-entry", "bad-dtb-structure"):
            data = bytearray(packed.read_bytes())
            manifest = struct.unpack_from("<Q", data, 0x20)[0]
            cursor = manifest + 60
            for _ in range(struct.unpack_from("<I", data, manifest + 8)[0]):
                offset, size, entry_type = struct.unpack_from("<QQI", data, cursor)
                if entry_type == 5:
                    if name == "short-dtb-entry":
                        struct.pack_into("<Q", data, cursor + 8, size - 1)
                    else:
                        structure = struct.unpack_from(">I", data, offset + 8)[0]
                        struct.pack_into(">I", data, offset + structure, 0xffffffff)
                    break
                cursor += (56 + struct.unpack_from("<I", data, cursor + 52)[0] + 3) & ~3
            else:
                raise AssertionError("No DTB entry in packed fixture")
            packed.write_bytes(data)
        command = ["qemu-system-aarch64", "-machine", "virt,gic-version=3",
                   "-accel", "tcg", "-cpu", "cortex-a76", "-smp", "1", "-m", "256M",
                   "-display", "none", "-monitor", "none", "-serial", "stdio",
                   "-no-reboot", "-kernel", packed]
        result = run(command, input=key, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=20)
        output = result.stdout.decode(errors="replace")
        (out / (name + ".log")).write_text(output)
        rejected = name in ("short-dtb-entry", "bad-dtb-structure")
        expected = "PAYLOAD_A_PASS" if name == "default" or rejected else "PAYLOAD_D_PASS" if with_dtb else "PAYLOAD_B_PASS"
        assert expected in output, output
        if rejected:
            assert "Manifest not found, booting base image" in output, output
        results.append({"case": name, "passed": True})
        print(name + ": PASS", flush=True)
    (out / "validation.json").write_text(json.dumps({
        "runtime_sha256": hashlib.sha256(runtime.read_bytes()).hexdigest(),
        "dtb_scope": "One shared payload DT, separate from boot/runtime DT. Distinct per-image packer references are not covered.",
        "tests": results}, indent=2) + "\n")


if __name__ == "__main__":
    main()
