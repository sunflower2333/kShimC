#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""QEMU smoke test: kShimC UFSHCI core and SD card layer (over CrDK
SdhciMsmLib) against QEMU's PCI ufs/ufs-lu and sdhci-pci/sd-card devices.

Builds a bare-metal AArch64 ELF from the real driver sources plus
tests/qemu_storage, writes patterned disk images, boots it on
-M virt,highmem=off and checks the serial log.
"""
import argparse
from pathlib import Path
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]


def pattern_image(path, blocks, block_size, fn):
    with path.open("wb") as f:
        for lba in range(blocks):
            f.write(bytes(fn(lba, i) for i in range(block_size)))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--crdk", type=Path, default=ROOT / "build/host/crdk/source",
                        help="prepared CrDK tree (ports/crdk/prepare.py output)")
    parser.add_argument("--out", type=Path, default=ROOT / "build/storage-qemu")
    parser.add_argument("--cc", default="aarch64-linux-gnu-gcc")
    parser.add_argument("--qemu", default="qemu-system-aarch64")
    args = parser.parse_args()
    out = args.out.resolve()
    out.mkdir(parents=True, exist_ok=True)
    crdk = args.crdk.resolve()
    if not (crdk / "Library/SdhciMsmLib/SdhciMsm.c").exists():
        sys.exit(f"prepared CrDK tree not found at {crdk}; configure the host tests first")

    elf = out / "storage-smoke.elf"
    sources = [ROOT / "tests/qemu_storage/start.S", ROOT / "tests/qemu_storage/main.c",
               ROOT / "src/drivers/block/ufshci.c", ROOT / "src/drivers/block/sdmmc.c",
               crdk / "Library/SdhciMsmLib/SdhciMsm.c"]
    flags = ["-ffreestanding", "-nostdlib", "-nostartfiles", "-static", "-no-pie", "-fno-pic",
             "-O2", "-g", "-mstrict-align", "-mgeneral-regs-only", "-fno-stack-protector",
             "-U_FORTIFY_SOURCE", "-D_FORTIFY_SOURCE=0", "-fno-builtin", "-Wall", "-Wno-misleading-indentation",
             "-DCR_FREESTANDING=1", "-DUFSHCI_POOL_HOSTS=1",
             f"-I{ROOT / 'src/include'}", f"-I{crdk / 'Include'}", f"-I{ROOT / 'ports/crdk/include'}",
             f"-Wl,-T,{ROOT / 'tests/qemu_storage/link.ld'}", "-Wl,--build-id=none"]
    subprocess.run([args.cc, *flags, *map(str, sources), "-o", str(elf), "-lgcc"], check=True)

    ufs_img, sd_img = out / "ufs.img", out / "sd.img"
    pattern_image(ufs_img, 4096, 4096, lambda lba, i: (lba * 131 + i * 7 + 0x5a) & 0xff)
    pattern_image(sd_img, 8192, 512, lambda lba, i: (lba * 29 + i * 3 + 0xa5) & 0xff)

    command = [args.qemu, "-M", "virt,highmem=off", "-cpu", "cortex-a57", "-m", "256M",
               "-display", "none", "-monitor", "none", "-serial", "stdio", "-no-reboot",
               "-kernel", str(elf),
               "-drive", f"if=none,id=ufsdisk,file={ufs_img},format=raw",
               "-device", "ufs,id=ufs0,serial=kshimtest",
               "-device", "ufs-lu,drive=ufsdisk,lun=0",
               "-drive", f"if=none,id=sddisk,file={sd_img},format=raw",
               "-device", "sdhci-pci,id=sdhci0",
               "-device", "sd-card,drive=sddisk"]
    result = subprocess.run(command, capture_output=True, text=True, timeout=120)
    log = result.stdout + result.stderr
    (out / "qemu.log").write_text(log)
    print(log)
    required = ["STORAGE_SMOKE_PASS", "UFS_PATH first=full-init", "UFS_PATH second=inherited"]
    missing = [r for r in required if r not in log]
    if missing:
        sys.exit(f"storage QEMU smoke test failed; missing {missing}")
    print("storage_qemu: PASS")


if __name__ == "__main__":
    main()
