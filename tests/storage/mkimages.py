#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Build disposable storage fixtures for the kShimC block/fs/firmware tests.

Filesystems come from the real tools (mkfs.vfat + mtools, mkfs.ext4 -d,
mkfs.f2fs + sload.f2fs; mkfs.erofs when present).  GPT disks come from
sgdisk (512-byte LUN) and from an independent writer here (4096-byte LUN),
and the Android liblp "super" image from a writer here.  Exit status 77
means a required tool is missing (ctest SKIP).

Output (in --out):
  payload/<fs>/...         reference file trees
  fat12.img fat16.img fat32.img ext4.img f2fs.img [erofs.img]
  lun0.img                 4096-byte sectors: misc, super (vendor_a ext4 in
                           two extents, vendor_b f2fs)
  lun4.img                 512-byte sectors (sgdisk): modem_a/modem_b FAT16,
                           persist ext4, garbage, plus a non-ASCII label
  manifest.txt             "<disk> <label> <first_lba> <blocks>" from sgdisk/our writer
"""
import argparse
import hashlib
import os
import shutil
import struct
import subprocess
import sys
import uuid
import zlib
from pathlib import Path

SKIP = 77
REQUIRED = ["mkfs.vfat", "mcopy", "mmd", "mkfs.ext4", "mkfs.f2fs", "sload.f2fs", "sgdisk"]


def run(*cmd):
    r = subprocess.run([str(c) for c in cmd], stdout=subprocess.PIPE,
                       stderr=subprocess.STDOUT, text=True, timeout=300)
    if r.returncode:
        raise SystemExit(f"{' '.join(map(str, cmd))} failed:\n{r.stdout}")
    return r.stdout


def pattern(n, seed=0):
    return bytes(((i * 31 + i // 173 + seed) % 251) for i in range(n))


def sparse(path, size):
    with open(path, "wb") as f:
        f.truncate(size)


# ---- PIL MDT/bXX fixtures -------------------------------------------------

def mdt_group(stem, marker, seg_sizes):
    """ELF32 MDT header with a contained hash segment and split PT_LOAD
    segments that live in <stem>.bII files (II = program header index)."""
    phnum = 2 + len(seg_sizes) + 1           # header, hash, loads..., bss
    hash_bytes = bytes([marker]) * 64
    phoff = 52
    hash_off = phoff + phnum * 32
    mdt_size = hash_off + len(hash_bytes)
    eh = bytearray(52)
    eh[0:4] = b"\x7fELF"
    eh[4], eh[5], eh[6] = 1, 1, 1
    struct.pack_into("<HHIIIIIHHHHHH", eh, 16, 2, 40, 1, 0x1000, phoff, 0, 0,
                     52, 32, phnum, 0, 0, 0)
    ph = bytearray()
    # [0] ELF header segment (type NULL, flags 0x07000000)
    ph += struct.pack("<IIIIIIII", 0, 0, 0, 0, hash_off, 0, 0x07000000, 0)
    # [1] hash segment, contained in the MDT
    ph += struct.pack("<IIIIIIII", 0, hash_off, 0, 0, len(hash_bytes), 0x1000, 0x02200000, 0x1000)
    parts = {}
    off = 0x10000
    for i, n in enumerate(seg_sizes, start=2):
        ph += struct.pack("<IIIIIIII", 1, off, 0x80000000 + off, 0x80000000 + off,
                          n, n, 0x80000000 | 5, 0x1000)
        parts[i] = pattern(n, marker + i)
        off += (n + 0xfff) & ~0xfff
    # bss: PT_LOAD with no file bytes
    ph += struct.pack("<IIIIIIII", 1, 0, 0x90000000, 0x90000000, 0, 0x1000, 6, 0x1000)
    mdt = bytes(eh) + bytes(ph) + hash_bytes
    assert len(mdt) == mdt_size
    files = {f"{stem}.mdt": mdt}
    files.update({f"{stem}.b{i:02d}": data for i, data in parts.items()})
    return files


def write_tree(root, files):
    for rel, data in files.items():
        p = root / rel
        p.parent.mkdir(parents=True, exist_ok=True)
        p.write_bytes(data)


def fs_tree(root, symlink=True):
    deep = root / "firmware" / "deep"
    deep.mkdir(parents=True)
    for n in (0, 1, 4095, 4096, 4097, 65537, 1048591):
        (deep / f"pattern-{n}.bin").write_bytes(pattern(n))
    (deep / ("long-" + "x" * 180 + ".bin")).write_bytes(b"long name")
    (root / "firmware" / "big.bin").write_bytes(pattern(3 * 1024 * 1024, 7))
    (root / "top.txt").write_bytes(b"top level\n")
    if symlink:
        (deep / "relative-link").symlink_to("pattern-4097.bin")


# ---- filesystems -----------------------------------------------------------

def mkfat(image, root, size, bits):
    sparse(image, size)
    run("mkfs.vfat", "-F", str(bits), "-n", "KSHIMTEST", image)
    for d in sorted(p for p in root.rglob("*") if p.is_dir()):
        run("mmd", "-i", image, "::/" + str(d.relative_to(root)))
    for f in sorted(p for p in root.rglob("*") if p.is_file() and not p.is_symlink()):
        run("mcopy", "-i", image, f, "::/" + str(f.relative_to(root)))


def mkext4(image, root, size):
    sparse(image, size)
    run("mkfs.ext4", "-q", "-F", "-b", "4096", "-O", "metadata_csum,64bit",
        "-E", "root_owner=0:0", "-d", root, image)


def mkf2fs(image, root, size):
    sparse(image, size)
    run("mkfs.f2fs", "-q", "-f", "-O", "extra_attr,inode_checksum,sb_checksum", image)
    run("sload.f2fs", "-f", root, image)


def crc32c_raw(data, crc=0xFFFFFFFF):
    for b in data:
        crc ^= b
        for _ in range(8):
            crc = (crc >> 1) ^ (0x82F63B78 & -(crc & 1))
    return crc


def erofs_image(path, root):
    """Minimal uncompressed EROFS (4 KiB blocks, compact inodes, FLAT_PLAIN
    data, superblock checksum), used only when mkfs.erofs is unavailable."""
    nodes = []  # (path, kind)

    def walk(p):
        nodes.append(p)
        if p.is_dir() and not p.is_symlink():
            for c in sorted(p.iterdir(), key=lambda x: x.name.encode()):
                walk(c)
    walk(root)
    nid = {p: i for i, p in enumerate(nodes)}
    parent = {root: root}
    for p in nodes:
        if p.is_dir() and not p.is_symlink():
            for c in p.iterdir():
                parent[c] = p
    meta_blocks = (len(nodes) * 32 + 4095) // 4096
    blocks = [bytes(4096)] + [b""] * meta_blocks
    inodes = bytearray(meta_blocks * 4096)

    def ftype(p):
        return 7 if p.is_symlink() else 2 if p.is_dir() else 1

    for p in nodes:
        if p.is_symlink():
            data, mode = os.readlink(p).encode(), 0o120777
        elif p.is_dir():
            ents = [(b".", p), (b"..", parent[p])] + [
                (c.name.encode(), c) for c in p.iterdir()]
            ents.sort(key=lambda e: e[0])
            data = bytearray()
            block_entries = []
            # pack entries greedily into 4 KiB directory blocks
            for name, target in ents:
                trial = block_entries + [(name, target)]
                if 12 * len(trial) + sum(len(n) for n, _ in trial) > 4096:
                    data += pack_dir_block(block_entries, nid, ftype, True)
                    block_entries = [(name, target)]
                else:
                    block_entries = trial
            data += pack_dir_block(block_entries, nid, ftype, False)
            data, mode = bytes(data), 0o040755
        else:
            data, mode = p.read_bytes(), 0o100644
        addr = len(blocks) if data else 0
        for at in range(0, len(data), 4096):
            blocks.append(data[at:at + 4096].ljust(4096, b"\0"))
        struct.pack_into("<HHHHIIII", inodes, nid[p] * 32, 0, 0, mode, 1, len(data), 0,
                         addr, nid[p])
    for i in range(meta_blocks):
        blocks[1 + i] = bytes(inodes[i * 4096:(i + 1) * 4096])
    img = bytearray(b"".join(blocks))
    sb = memoryview(img)[1024:1024 + 128]
    struct.pack_into("<IIIBBHQ", sb, 0, 0xE0F5E1E2, 0, 1, 12, 0, nid[root], len(nodes))
    struct.pack_into("<III", sb, 36, len(blocks), 1, 0)
    sb[48:64] = uuid.uuid5(DISK_NS, "erofs").bytes
    struct.pack_into("<I", img, 1028, crc32c_raw(bytes(img[1024:4096])))
    path.write_bytes(bytes(img))


def pack_dir_block(entries, nid, ftype, full):
    head = bytearray()
    names = b""
    base = 12 * len(entries)
    for name, target in entries:
        head += struct.pack("<QHBB", nid[target], base + len(names), ftype(target), 0)
        names += name
    block = bytes(head) + names
    return block.ljust(4096, b"\0") if full else block


# ---- GPT (independent writer, any sector size) ---------------------------

TYPE_DATA = uuid.UUID("ebd0a0a2-b9e5-4433-87c0-68b6b72699c7")
DISK_NS = uuid.UUID("6e2c6c1f-7cb8-4f43-9a2e-0c9f0b7b1a00")


def gpt_write(path, sector, total_blocks, parts):
    """parts: [(label, first_lba, blocks, payload_bytes_or_None)]"""
    entries, stride = 128, 128
    table = bytearray(entries * stride)
    for i, (label, first, blocks, _) in enumerate(parts):
        e = memoryview(table)[i * stride:(i + 1) * stride]
        e[0:16] = TYPE_DATA.bytes_le
        e[16:32] = uuid.uuid5(DISK_NS, f"{path.name}:{label}").bytes_le
        struct.pack_into("<QQQ", e, 32, first, first + blocks - 1, 0)
        name = label.encode("utf-16-le")
        e[56:56 + len(name)] = name
    table_blocks = (len(table) + sector - 1) // sector
    last = total_blocks - 1
    first_usable = 2 + table_blocks
    last_usable = last - table_blocks - 1
    table_crc = zlib.crc32(table) & 0xffffffff
    disk_guid = uuid.uuid5(DISK_NS, path.name).bytes_le

    def header(my_lba, alt_lba, table_lba):
        h = bytearray(92)
        h[0:8] = b"EFI PART"
        struct.pack_into("<IIIIQQQQ", h, 8, 0x00010000, 92, 0, 0, my_lba, alt_lba,
                         first_usable, last_usable)
        h[56:72] = disk_guid
        struct.pack_into("<QIII", h, 72, table_lba, entries, stride, table_crc)
        struct.pack_into("<I", h, 16, zlib.crc32(h) & 0xffffffff)
        return bytes(h)

    mbr = bytearray(512)
    mbr[446 + 4] = 0xee
    struct.pack_into("<II", mbr, 446 + 8, 1, min(total_blocks - 1, 0xffffffff))
    mbr[510:512] = b"\x55\xaa"
    with open(path, "wb") as f:
        f.truncate(total_blocks * sector)
        f.seek(0); f.write(mbr)
        f.seek(sector); f.write(header(1, last, 2))
        f.seek(2 * sector); f.write(table)
        f.seek((last - table_blocks) * sector); f.write(table)
        f.seek(last * sector); f.write(header(last, 1, last - table_blocks))
        for label, first, blocks, payload in parts:
            assert first >= first_usable and first + blocks - 1 <= last_usable, label
            if payload is not None:
                assert len(payload) <= blocks * sector, label
                f.seek(first * sector); f.write(payload)


# ---- liblp super (metadata v10.0) ------------------------------------------

def super_image(size, partitions, slots=2, max_meta=65536):
    """partitions: [(name, [chunk bytes...])]; each chunk becomes one LINEAR
    extent.  Extents are laid out in reverse chunk order so a logical
    partition is genuinely non-contiguous on the device."""
    first_sector = 2048
    img = bytearray(size)
    geom = bytearray(52)
    struct.pack_into("<II", geom, 0, 0x616C4467, 52)
    struct.pack_into("<III", geom, 40, max_meta, slots, 4096)
    geom[8:40] = hashlib.sha256(geom).digest()
    img[4096:4148] = geom
    img[8192:8244] = geom
    # place data
    cursor = first_sector * 512
    placed = []
    for name, chunks in partitions:
        locs = []
        for chunk in reversed(chunks):
            assert len(chunk) % 512 == 0
            img[cursor:cursor + len(chunk)] = chunk
            locs.append((cursor // 512, len(chunk) // 512))
            cursor += (len(chunk) + 1048575) // 1048576 * 1048576
        locs.reverse()
        placed.append((name, locs))
    assert cursor <= size, "super too small"
    part_t, ext_t = bytearray(), bytearray()
    for name, locs in placed:
        p = bytearray(52)
        p[0:len(name)] = name.encode()
        struct.pack_into("<IIII", p, 36, 0, len(ext_t) // 24, len(locs), 0)
        part_t += p
        for sector, count in locs:
            ext_t += struct.pack("<QIQI", count, 0, sector, 0)
    group = bytearray(48); group[0:7] = b"default"
    dev = bytearray(64)
    struct.pack_into("<QIIQ", dev, 0, first_sector, 1048576, 0, size)
    dev[24:29] = b"super"
    tables = bytes(part_t + ext_t + group + dev)
    hdr = bytearray(128)
    struct.pack_into("<IHHI", hdr, 0, 0x414C5030, 10, 0, 128)
    struct.pack_into("<I", hdr, 44, len(tables))
    hdr[48:80] = hashlib.sha256(tables).digest()
    offs = [0, len(part_t), len(part_t) + len(ext_t), len(part_t) + len(ext_t) + 48]
    counts = [len(placed), len(ext_t) // 24, 1, 1]
    for i, (o, c, s) in enumerate(zip(offs, counts, (52, 24, 48, 64))):
        struct.pack_into("<III", hdr, 80 + i * 12, o, c, s)
    hdr[12:44] = hashlib.sha256(hdr).digest()
    meta = bytes(hdr) + tables
    for s in range(slots * 2):  # primary slots, then backups
        at = 12288 + s * max_meta
        img[at:at + len(meta)] = meta
    return bytes(img)


def misc_image(size, state=0, version=2):
    img = bytearray(size)
    msg = bytearray(64)
    msg[0] = version
    struct.pack_into("<I", msg, 1, 0x56740AB0)
    msg[5] = state
    img[32768:32832] = msg
    return bytes(img)


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--out", type=Path, required=True)
    a = ap.parse_args()
    missing = [t for t in REQUIRED if not shutil.which(t)]
    if missing:
        print("SKIP: missing tools:", ", ".join(missing))
        return SKIP
    out = a.out.resolve()
    if out.exists():
        shutil.rmtree(out)
    out.mkdir(parents=True)
    pay = out / "payload"

    # Standalone filesystem images, same reference tree.
    fs_root = pay / "fs"
    fs_tree(fs_root)
    fat_root = pay / "fsfat"
    fs_tree(fat_root, symlink=False)
    mkfat(out / "fat12.img", fat_root, 8 * 1024 * 1024, 12)
    mkfat(out / "fat16.img", fat_root, 32 * 1024 * 1024, 16)
    mkfat(out / "fat32.img", fat_root, 64 * 1024 * 1024, 32)
    mkext4(out / "ext4.img", fs_root, 16 * 1024 * 1024)
    mkf2fs(out / "f2fs.img", fs_root, 64 * 1024 * 1024)
    if shutil.which("mkfs.erofs"):
        run("mkfs.erofs", out / "erofs.img", fs_root)
        (out / "erofs.origin").write_text("mkfs.erofs\n")
    else:
        erofs_image(out / "erofs.img", fs_root)
        (out / "erofs.origin").write_text("mkimages.py minimal FLAT_PLAIN writer\n")

    # Android partitions.
    va = pay / "vendor_a"
    write_tree(va, {**{f"firmware/{k}": v for k, v in mdt_group("adsp", 0xa1, [5000, 70000]).items()},
                    "firmware/a_only.bin": b"vendor_a only\n",
                    "firmware/wlan.bin": pattern(12345, 0xa1)})
    vb = pay / "vendor_b"
    write_tree(vb, {**{f"firmware/{k}": v for k, v in mdt_group("adsp", 0xb1, [5000, 70000]).items()},
                    "firmware/b_only.bin": b"vendor_b only\n",
                    "firmware/wlan.bin": pattern(12345, 0xb1)})
    ma = pay / "modem_a"
    group = mdt_group("modem", 0x5a, [4096, 100000, 3])
    cdsp = mdt_group("cdsp", 0xcd, [1000, 2000])
    del cdsp["cdsp.b03"]  # incomplete group: a required member is missing
    write_tree(ma, {**{f"image/{k}": v for k, v in group.items()},
                    **{f"image/{k}": v for k, v in cdsp.items()},
                    "image/venus.mbn": pattern(777, 0x5e),
                    "image/wlan.bin": pattern(12345, 0x5a),
                    "image/big.bin": pattern(2 * 1024 * 1024, 3)})
    mb = pay / "modem_b"
    write_tree(mb, {**{f"image/{k}": v for k, v in mdt_group("modem", 0x5b, [4096, 100000, 3]).items()},
                    "image/venus.mbn": pattern(777, 0x5f)})
    pe = pay / "persist"
    write_tree(pe, {"touch/cal.bin": pattern(300, 0x77)})

    mkext4(out / "vendor_a.ext4", va, 8 * 1024 * 1024)
    mkf2fs(out / "vendor_b.f2fs", vb, 64 * 1024 * 1024)
    mkfat(out / "modem_a.fat", ma, 16 * 1024 * 1024, 16)
    mkfat(out / "modem_b.fat", mb, 16 * 1024 * 1024, 16)
    mkext4(out / "persist.ext4", pe, 4 * 1024 * 1024)

    vendor_a = (out / "vendor_a.ext4").read_bytes()
    half = len(vendor_a) // 2
    super_bytes = super_image(80 * 1024 * 1024, [
        ("vendor_a", [vendor_a[:half], vendor_a[half:]]),
        ("vendor_b", [(out / "vendor_b.f2fs").read_bytes()]),
    ])
    (out / "super.img").write_bytes(super_bytes)

    manifest = []
    # LUN0: 4096-byte sectors, independent GPT writer.
    s4 = 4096
    misc_blocks = 64 * 1024 // s4
    super_blocks = len(super_bytes) // s4
    lun0 = [("misc", 256, misc_blocks, misc_image(64 * 1024)),
            ("super", 512, super_blocks, super_bytes)]
    total0 = 512 + super_blocks + 256
    gpt_write(out / "lun0.img", s4, total0, lun0)
    manifest += [f"lun0 {l} {f} {b}" for l, f, b, _ in lun0]

    # LUN4: 512-byte sectors, partitioned by sgdisk.
    lun4 = out / "lun4.img"
    sizes = [("modem_a", 32768, out / "modem_a.fat"), ("modem_b", 32768, out / "modem_b.fat"),
             ("persist", 8192, out / "persist.ext4"), ("garbage", 2048, None)]
    sparse(lun4, (2048 + sum(s for _, s, _ in sizes) + 4096) * 512)
    args, first = ["sgdisk", "-o"], 2048
    for i, (label, blocks, _) in enumerate(sizes, start=1):
        args += ["-n", f"{i}:{first}:{first + blocks - 1}", "-c", f"{i}:{label}", "-t", f"{i}:0700"]
        first += blocks
    # A label that is not plain ASCII ("rés") must not be matchable by name.
    args += ["-n", f"5:{first}:{first + 63}", "-c", "5:rés", "-t", "5:0700"]
    run(*args, lun4)
    # Read back sgdisk's layout as the oracle.
    for i, (label, blocks, img) in enumerate(sizes, start=1):
        info = run("sgdisk", "-i", str(i), lun4)
        start = int(info.split("First sector: ")[1].split()[0])
        end = int(info.split("Last sector: ")[1].split()[0])
        manifest.append(f"lun4 {label} {start} {end - start + 1}")
        if img is not None:
            with open(lun4, "r+b") as f:
                f.seek(start * 512)
                f.write(img.read_bytes())
    with open(lun4, "r+b") as f:  # garbage: non-filesystem bytes
        start = int(manifest[-1].split()[2])
        f.seek(start * 512)
        f.write(pattern(2048 * 512, 99))
    (out / "manifest.txt").write_text("\n".join(manifest) + "\n")
    print("fixtures:", out)
    return 0


if __name__ == "__main__":
    sys.exit(main())
