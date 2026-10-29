# Block layer, filesystems and firmware loader

kShimC reads Android firmware from the board's internal storage using
CrDK's `Library/FirmwareLoaderLib` (CrDK `feature/2610`, `f23922d`). The
FirmwareLoaderLib sources are compiled unmodified from the materialized CrDK
tree (`${CRDK_ROOT}`, falling back to the `lib/CrDK` submodule). No CrDK patch
is involved. kShimC supplies the environment around them:

| Layer | Files | CrDK code used |
| --- | --- | --- |
| Block core, RAM disk, GPT scan | `src/block/` | none (MIT, standalone) |
| Filesystem adapter, liblp devices | `src/fs/` | `NhBase`, `NhFs`, `NhFat`, `NhExt4`, `NhF2fs`, [`NhErofs`], `NhGpt`, `NhBundle`, `NhPolicy` |
| Firmware loader, boot slot | `src/firmware/` | `NhPolicyRead`, `NhBundleRead`, `NhGptFind`, `NhSuperOpen`, `NhSnapshotCheck`, `NhBootArgsSlot` |

Public headers live in `src/include/kshim/`: `blkdev.h` (the shared driver
interface), `block.h`, `storage.h`, `fs.h` and `firmware.h`. The build glue is
in `cmake/storage.cmake`.

## Configuration and licensing

All options default to `n`.

| Option | Effect |
| --- | --- |
| `CONFIG_BLOCK` | Block registry, split/bounce reads, `KSHIM_BLKDEV_MEMORY`, GPT scan. No CrDK dependency. |
| `CONFIG_FS_FAT`, `CONFIG_FS_EXT4`, `CONFIG_FS_F2FS` | MIT readers from FirmwareLoaderLib. Any of them enables the adapter (`FS_CORE`). |
| `CONFIG_FS_EROFS` | **GPL-2.0-or-later.** `NhErofs.c` carries a U-Boot-derived decoder. Enabling it makes the linked kShimC binary GPL-2.0-or-later. CMake prints a warning. |
| `CONFIG_FIRMWARE_LOADER` | The loader and boot-slot capture. Requires `CONFIG_BLOCK` and at least one filesystem. |

When a reader is configured out, `src/fs/nh_stubs.c` (MIT) defines the entry
points that `NhFs.c` dispatches to. They return `NH_UNSUPPORTED`, so the
filesystem is recognized but refused. This is how the default build avoids
compiling or linking any GPL code. `tests/storage/check_no_gpl.py` verifies it
three ways: compile_commands.json, archive members, and the linked ELF's debug
info. All upstream SPDX headers are kept.

`kshim_storage_init(fdt)` (called from `kshim_main` when `CONFIG_BLOCK=y`)
scans GPT on every registered whole disk. With the loader enabled, it also
captures the current slot from the runtime DT. Storage drivers must register
their whole-disk devices before this call, or call `kshim_gpt_scan_all()` and
`kshim_bootslot_capture_fdt()` themselves later.

## Block layer (`kshim/blkdev.h`, `kshim/block.h`)

- `kshim_blkdev_register/find/first` maintain a single list in registration
  order. Registration rejects:
  - names that are empty or too long, and duplicate names (`KSHIM_ERR_EXISTS`);
  - block sizes that are not a power of two of at least 512;
  - a missing `ops->read`, except on a partition window, which is forwarded
    to its parent;
  - a child that is not inside a registered parent of the same block size.

  `kshim_blkdev_unregister` refuses while a child exists (`KSHIM_ERR_BUSY`).
- `kshim_blkdev_read` checks `lba + count` without overflow and splits into
  `max_transfer_blocks` driver calls. Partition windows add `start_lba` and
  use the parent's limit. A positive driver code is returned unchanged; a
  negative one becomes `ERROR_DEVICE_ERROR`.
- `kshim_blkdev_read_bytes` reads any byte range. It handles the unaligned
  head and tail through one bounce allocation. Whole blocks go straight to the
  caller's buffer when it is 64-byte aligned (`KSHIM_BLKDEV_DMA_ALIGN`);
  otherwise they bounce in chunks of at most 64 KiB.
- `kshim_memdisk_init` creates a RAM/image device with read counters and
  fault injection, for tests and RAM disks.
- `kshim_gpt_scan(disk)` applies `NhGptFind`'s rules:
  - both headers and both arrays must be present, CRC-valid and describe the
    same layout;
  - entries must lie in the usable range, without overlaps and with distinct
    non-zero unique GUIDs.

  It returns `ERROR_NOT_SUPPORTED` for a raw LUN and `KSHIM_ERR_CORRUPT` for a
  protective MBR with no headers, or for any inconsistency. It registers
  nothing unless the whole table is valid. Each partition becomes a read-only
  `KSHIM_BLKDEV_PARTITION` named `<disk>:<label>` (for example
  `ufs0:lu4:modem_a`), or `<disk>:p<N>` when the label is not plain ASCII,
  contains `:`, does not fit in 31 characters, or is a duplicate.
- `kshim_partition_find(label, disk|NULL)` matches exact labels. A label
  present twice is `KSHIM_ERR_CORRUPT`. `kshim_partition_find_slot(base, slot)`
  and `kshim_slot_name` append `_a`/`_b`.

## Filesystems (`kshim/fs.h`)

`kshim_fs_mount(blkdev)` detects the type the same way `NhFsOpen` does:

1. EROFS magic at byte 1024;
2. else F2FS magic at 1024;
3. else the ext4 magic at 1080;
4. else FAT.

A configured-out type returns `ERROR_NOT_SUPPORTED`.

The rest of the API:

- `kshim_fs_stat` and `kshim_fs_open` (regular files only), then
  `kshim_file_read` (exact, bounds-checked) and `kshim_file_close`.
- `kshim_fs_read_file(buf, cap)` reads a whole file into caller memory. It
  fails with `KSHIM_ERR_TOO_LARGE` and reports the size when the file does not
  fit.
- `kshim_fs_load_file(max)` allocates the buffer itself.

Paths are relative ASCII. A leading `/` is ignored. `.`, `..`, empty
components and symlinks are refused. Every file is bounded by 128 MiB
(`NH_MAX_FILE`).

`kshim_lp_open(super, metadata_slot, "vendor_a")` exposes a liblp logical
partition as an unregistered `KSHIM_BLKDEV_PARTITION` that can be mounted or
registered. It reads base extents only. Before using it at runtime, call
`kshim_lp_snapshot_check(misc)`. This requires a Virtual A/B message, version
2, in merge state `NONE`.

Format limits are FirmwareLoaderLib's:

- ext4: no journal replay, fscrypt, inline data or bigalloc.
- F2FS: clean checkpoint only; encrypted, compressed and casefolded files are
  refused.
- EROFS: 4 KiB flat/inline and legacy LZ4/compact layouts only.
- FAT: ASCII short and long names; clean, complete cluster chains.
- liblp: v10.0 to v10.2 with one block device.

## Firmware loader (`kshim/firmware.h`)

```c
struct kshim_fw_request req = { .kind = KSHIM_FW_PIL, .alternate = "adsp.mbn" };
struct kshim_firmware fw;
int rc = kshim_firmware_request("adsp.mdt", &req, &fw);
/* fw.data/fw.size: header; kshim_firmware_part(&fw, 2, &p, &n): adsp.b02 */
kshim_firmware_release(&fw);
```

`kshim_firmware_load(name, buf, cap, &size)` loads a single file into caller
memory from the default locations. Requests can also set:

- `locations`: a custom list;
- `max_size`: a bound on each file;
- `buffer`: caller memory, for `KSHIM_FW_FILE`;
- `allow_builtin`: permit the packaged fallback;
- `device`: a physical device prefix such as `ufs0`.

The selection itself is CrDK's `NhPolicyRead`, unchanged. kShimC implements
the adapter callbacks the way `Driver/NhlosPartitionCrDxe` does.

### Search order

Default locations (CrDK `Driver/PILCrDxe`, `kshim_firmware_default_locations`):

| # | Candidate | Meaning |
| --- | --- | --- |
| 0 | `super\|vendor{slot}\|firmware` | `firmware/<name>` in the current slot's `vendor_a`/`vendor_b` logical partition, inside GPT partition `super` |
| 1 | `super\|vendor{slot}\|firmware/image` | `firmware/image/<name>` in the same logical partition |
| 2 | `modem{slot}\|\|image` | `image/<name>` in GPT partition `modem_a`/`modem_b` (NON-HLOS, normally FAT) |
| — | builtin | only with `allow_builtin`, and only after confirmed absence from every location |

The full sequence for one request:

1. **Validate first.** Check the complete location list, file name and
   alternate before any I/O, so a malformed later entry is never hidden by an
   earlier success. A partition name may end in `{slot}`. An explicit suffix
   for the other slot (such as `modem_b` while booted from `_a`) is
   `ERROR_INVALID_PARAMETER`. Only `super` may hold a logical partition.
2. **Bind one device.** Use every registered whole disk of one physical
   device. The device is the name prefix before the first `:`, so `ufs0:lu0`
   to `ufs0:lu7` count as one. It must be the unique internal UFS, eMMC or
   memory device, or the one named in `req.device`. SD cards are never chosen
   implicitly. More than one candidate device is `KSHIM_ERR_CORRUPT`; none is
   `KSHIM_ERR_NOT_READY`. If any candidate needs a slot, take the current slot
   from the boot-slot authority below; if there is none, return
   `KSHIM_ERR_NOT_READY`.
3. **Walk the locations in order.** For each one, try the primary name and
   then `req.alternate`, both in that same location:
   1. Find the GPT partition with `NhGptFind` on every LUN. A valid GPT
      without it means absent. A raw LUN is skipped. If no LUN has a GPT, the
      result is `NOT_READY`. A label found on two LUNs is `CORRUPT`.
   2. If the candidate is a logical partition, admit Virtual A/B first. The
      64-byte message at byte 32768 of `misc` must be AOSP
      `misc_virtual_ab_message` version 2 in merge state `NONE`, and must read
      the same twice. A missing `misc` or any other state is `NOT_READY`.
      Then open the liblp metadata for the current slot with `NhSuperOpen`.
   3. Mount the filesystem with `NhFsOpen`, then open and read the file.
   4. For `KSHIM_FW_PIL`, `NhBundleRead` validates the ELF header. It then
      loads each segment that is not inside the header from `<stem>.bXX`,
      from the **same** source.

   Only "absent" moves on to the next candidate: a missing partition, a
   missing logical partition, or a missing primary file. Corrupt metadata, an
   unsupported filesystem, a non-regular file, I/O errors, an oversize file,
   a missing `.bXX` member and Virtual A/B or slot problems all stop the
   request.
4. **Recheck stability.** Before returning a result, or before treating the
   firmware as absent everywhere, recheck that:
   - the bound disks are still registered with the same geometry;
   - the current slot is unchanged;
   - the `misc` message is unchanged;
   - the selected GPT partition still has the same location.
5. **Builtin fallback.** Look in the builtin table
   (`kshim_firmware_builtin_register`) only if `allow_builtin` is set and
   every location was confirmed absent. A non-NULL empty location list means
   builtin-only, which is CrDK's package-only mode.

`NOT_READY` and every error other than `NOT_FOUND` never permit a fallback.
Consumers still authenticate executable images (PIL/PAS) and enforce their own
size and format limits; the loader only reads.

CrDK's lists do not name `persist`, a physical `vendor_a`, or other
partitions. Boards that need them pass an explicit list in the same syntax,
for example `"persist||touch"` or `"vendor{slot}||firmware"`. They are not
added to the defaults.

### Current boot slot

The slot comes only from the immutable bootloader handoff, following CrDK
AndroidBootCr:

- `androidboot.slot_suffix=_a|_b` or `androidboot.slot=a|b` in the kernel
  command line (`/chosen/bootargs`, parsed with CrDK `NhBootArgsSlot`);
- the same keys in a checksum-verified Android bootconfig;
- `slot_suffix` / `slot` under `/firmware/android` in the DT.

All present values must agree; a conflict is `KSHIM_ERR_CORRUPT` and clears
the slot. The misc `bootloader_control` block and GPT A/B attributes describe
the *next* boot, so they are never used. `kshim_bootslot_capture_fdt` cannot
read the bootconfig, because kShimC does not map the initrd. A boot that
requests bootconfig (the `bootconfig` command-line flag) and carries no other
slot source leaves the slot `NOT_READY`. A caller that has mapped the initrd
can use `kshim_bootconfig_from_initrd` and `kshim_bootslot_capture` instead.

## Tests

```sh
cmake -S tests/storage -B build/storage -G Ninja -DCMAKE_C_COMPILER=clang
cmake --build build/storage && ctest --test-dir build/storage --output-on-failure
# GPL opt-in variant:
cmake -S tests/storage -B build/storage-erofs -G Ninja -DKSHIM_STORAGE_TEST_EROFS=ON
```

`tests/storage/mkimages.py` builds the fixtures. Missing tools make the run
SKIP with exit code 77.

- FAT12/16/32 images come from `mkfs.vfat` and `mtools`; ext4 from
  `mkfs.ext4 -d`; F2FS from `mkfs.f2fs` and `sload.f2fs`.
- EROFS comes from `mkfs.erofs`. Without it, a minimal flat EROFS writer in
  the script is used.
- The 512-byte LUN is partitioned by `sgdisk`, which also serves as the
  layout oracle. The 4096-byte LUN uses an independent writer.
- The liblp `super` image has `vendor_a` (ext4, split across two extents) and
  `vendor_b` (F2FS).

All tests run under ASan and UBSan. The suite is also included from
`tests/CMakeLists.txt`.
