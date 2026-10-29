/* SPDX-License-Identifier: MIT */
#pragma once
/*
 * Read-only filesystem access over kShimC block devices, built on CrDK
 * FirmwareLoaderLib's bounded Nh* readers (FAT12/16/32, ext4, F2FS and, only
 * with CONFIG_FS_EROFS, EROFS).  Paths are relative to the filesystem root
 * ("firmware/adsp.mdt"; a leading '/' is accepted and ignored), ASCII, at
 * most 511 bytes, without "." / ".." components.  Symlinks are refused.
 * Nothing is ever written.  Not thread safe: use from one CPU at a time.
 *
 * Status codes: 0, ERROR_NOT_FOUND (absent), ERROR_NOT_SUPPORTED (unknown or
 * disabled filesystem, unsupported feature, symlink), KSHIM_ERR_CORRUPT,
 * KSHIM_ERR_TOO_LARGE, KSHIM_ERR_NO_MEMORY, ERROR_DEVICE_ERROR,
 * ERROR_INVALID_PARAMETER.
 */
#include <kshim/block.h>

enum kshim_fs_type {
    KSHIM_FS_NONE = 0,
    KSHIM_FS_EXT4 = 1,
    KSHIM_FS_FAT = 2,
    KSHIM_FS_EROFS = 3,
    KSHIM_FS_F2FS = 4,
};

/* Library bound on a single file (CrDK NH_MAX_FILE, 128 MiB). */
#define KSHIM_FS_FILE_MAX (128U * 1024U * 1024U)
#define KSHIM_FS_PATH_MAX 512U

#define KSHIM_FS_MODE_TYPE 0170000U
#define KSHIM_FS_MODE_DIR 0040000U
#define KSHIM_FS_MODE_REG 0100000U

struct kshim_fs;
struct kshim_file;

struct kshim_fs_stat {
    uint64_t size;  /* bytes (FAT directories report the library bound) */
    uint32_t mode;  /* KSHIM_FS_MODE_* type bits (+ permissions where stored) */
    uint64_t inode; /* filesystem-specific identifier */
};

/* True when support for `type` is compiled in. */
bool kshim_fs_supported(enum kshim_fs_type type);
const char *kshim_fs_type_name(enum kshim_fs_type type);

/* Detect and mount the filesystem on a whole block device (usually a GPT
 * partition).  The device must stay registered until unmount. */
int kshim_fs_mount(struct kshim_blkdev *dev, struct kshim_fs **out);
void kshim_fs_unmount(struct kshim_fs *fs);
enum kshim_fs_type kshim_fs_get_type(const struct kshim_fs *fs);

int kshim_fs_stat(struct kshim_fs *fs, const char *path, struct kshim_fs_stat *st);

/* Open a regular file (directories: ERROR_NOT_SUPPORTED). */
int kshim_fs_open(struct kshim_fs *fs, const char *path, struct kshim_file **out);
uint64_t kshim_file_size(const struct kshim_file *file);
/* Exact read of [offset, offset + len); out-of-range is INVALID_PARAMETER. */
int kshim_file_read(struct kshim_file *file, uint64_t offset, void *buf, size_t len);
void kshim_file_close(struct kshim_file *file);

/* Read a whole regular file into `buf`.  When the file is larger than `cap`
 * nothing is read, *size (optional) receives the file size and
 * KSHIM_ERR_TOO_LARGE is returned. */
int kshim_fs_read_file(struct kshim_fs *fs, const char *path,
                       void *buf, size_t cap, size_t *size);
/* Read a whole regular file into a malloc() buffer (free() it).  Files above
 * `max_size` (0 = KSHIM_FS_FILE_MAX) fail with KSHIM_ERR_TOO_LARGE. */
int kshim_fs_load_file(struct kshim_fs *fs, const char *path, size_t max_size,
                       void **data, size_t *size);

/* ---- Android dynamic partitions (liblp "super") ------------------------- */

/*
 * Open logical partition `name` (e.g. "vendor_a") from the metadata slot
 * `metadata_slot` (0 = _a, 1 = _b) of a super block device, using CrDK's
 * NhSuperOpen (liblp v10.0-10.2, one block device, LINEAR/ZERO extents).
 * The result is an unregistered KSHIM_BLKDEV_PARTITION device named
 * "<super>:<name>" (shortened if needed) whose parent is `super`; it can be
 * mounted directly or registered with kshim_blkdev_register().
 *
 * This reads base extents only.  It does not resolve Virtual A/B snapshots:
 * callers must first admit the misc Virtual A/B state with
 * kshim_lp_snapshot_check(), as the firmware loader does.
 * ERROR_NOT_FOUND: valid metadata without `name`.  KSHIM_ERR_NOT_READY:
 * metadata advertises an update in progress.
 */
struct kshim_lp_dev;
int kshim_lp_open(struct kshim_blkdev *super, uint32_t metadata_slot,
                  const char *name, struct kshim_lp_dev **out);
struct kshim_blkdev *kshim_lp_blkdev(struct kshim_lp_dev *lp);
/* Unregisters (if registered) and frees.  KSHIM_ERR_BUSY, with nothing
 * freed, while a registered device names it as parent. */
int kshim_lp_close(struct kshim_lp_dev *lp);

/* Admit runtime reads of super base extents: the 64-byte AOSP
 * misc_virtual_ab_message at misc offset 32768 must be version 2 with merge
 * state NONE (CrDK NhSnapshotCheck).  The message is read twice and must be
 * stable.  `message` (optional) receives the bytes for later comparison. */
int kshim_lp_snapshot_check(struct kshim_blkdev *misc, uint8_t message[64]);
