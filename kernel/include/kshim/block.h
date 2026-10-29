/* SPDX-License-Identifier: MIT */
#pragma once
/*
 * Block layer extensions on top of the shared kshim/blkdev.h interface:
 * registry removal, RAM-backed devices, GPT partition scanning and
 * partition-name lookup.  All functions are synchronous, polling and must be
 * called from one CPU at a time (the boot CPU); the registry has no lock.
 */
#include <kshim/blkdev.h>
#include <kshim/storage.h>

/* Buffers handed to kshim_blkdev_read_bytes() are passed straight to the
 * driver only when aligned to this many bytes; otherwise the core bounces. */
#define KSHIM_BLKDEV_DMA_ALIGN 64U
/* Upper bound for one bounce allocation (rounded up to one block). */
#define KSHIM_BLKDEV_BOUNCE_MAX (64U * 1024U)

/* Remove a device from the registry.  Fails with KSHIM_ERR_BUSY while any
 * registered device names it as parent, ERROR_NOT_FOUND if not registered. */
int kshim_blkdev_unregister(struct kshim_blkdev *dev);

/* Total device size in bytes (0 on overflow or NULL). */
uint64_t kshim_blkdev_bytes(const struct kshim_blkdev *dev);

/* ---- RAM/image backed device (KSHIM_BLKDEV_MEMORY) ---------------------- */

struct kshim_memdisk {
    struct kshim_blkdev dev;
    uint8_t *data;              /* backing store, bytes = num_blocks * block_size */
    /* Test instrumentation, maintained by the memory driver. */
    uint32_t read_calls;        /* ops->read invocations */
    uint32_t largest_read;      /* largest block count seen by ops->read */
    uint32_t write_calls;
    /* Fault injection: when fail_status != 0, any ops->read touching
     * [fail_lba, fail_lba + fail_count) returns fail_status. */
    int fail_status;
    uint64_t fail_lba;
    uint64_t fail_count;
};

/* Initialise (does not register) a memory disk over `data`.  `bytes` must be
 * a non-zero multiple of `block_size`.  A NULL write op is installed when
 * read_only is true. */
int kshim_memdisk_init(struct kshim_memdisk *disk, const char *name,
                       void *data, uint64_t bytes, uint32_t block_size,
                       uint32_t max_transfer_blocks, bool read_only);

/* ---- GPT ----------------------------------------------------------------- */

#define KSHIM_GPT_NAME_MAX 36   /* UTF-16 code units in a GPT entry name */

/* A registered GPT partition.  `dev` is a KSHIM_BLKDEV_PARTITION whose name is
 * "<parent>:<label>" when that fits and is unique, else "<parent>:p<index>".
 * Look partitions up by `label`, never by the possibly shortened dev.name. */
struct kshim_partition {
    struct kshim_blkdev dev;
    char label[KSHIM_GPT_NAME_MAX + 1]; /* ASCII; non-ASCII units become '?' */
    bool label_ascii;                   /* label is exact printable ASCII */
    uint32_t index;                     /* 1-based GPT entry index */
    uint8_t type_guid[16];
    uint8_t unique_guid[16];
    uint64_t attributes;
    struct kshim_partition *scan_next;  /* owned by the GPT scanner */
};

/*
 * Validate the GPT of a whole disk with the same rules as CrDK's NhGptFind
 * (both headers and arrays, CRCs, identical layout, usable ranges, overlaps
 * and duplicate unique GUIDs) and register one KSHIM_BLKDEV_PARTITION child
 * per used entry.  `registered` (optional) receives the child count.
 *   ERROR_NOT_SUPPORTED  no GPT and no protective MBR (raw LUN)
 *   KSHIM_ERR_CORRUPT    damaged/inconsistent GPT, or protective MBR only
 *   KSHIM_ERR_EXISTS     the disk already has scanned partitions
 * Nothing is registered unless the whole table validates.
 */
int kshim_gpt_scan(struct kshim_blkdev *disk, unsigned *registered);

/* Scan every registered whole disk (parent == NULL, not yet scanned).
 * Returns the number of partitions registered; per-disk errors are logged. */
unsigned kshim_gpt_scan_all(void);

/* Unregister and free the partitions created by kshim_gpt_scan(disk).
 * KSHIM_ERR_BUSY (nothing released) while a registered device uses one of
 * them as parent; ERROR_NOT_FOUND when the disk was never scanned. */
int kshim_gpt_release(struct kshim_blkdev *disk);

/* Partition record for a device registered by the GPT scanner, else NULL. */
struct kshim_partition *kshim_partition_of(struct kshim_blkdev *dev);

/*
 * Find a scanned partition by exact GPT label.  With disk == NULL every
 * scanned disk is searched.  Returns ERROR_NOT_FOUND when absent and
 * KSHIM_ERR_CORRUPT when the label is ambiguous (present more than once),
 * mirroring CrDK's refusal of duplicate partition names across LUNs.
 */
int kshim_partition_find(const char *label, struct kshim_blkdev *disk,
                         struct kshim_partition **out);

/* A/B helpers.  Slot 0 is "_a", slot 1 is "_b". */
int kshim_slot_name(char *out, size_t cap, const char *base, uint32_t slot);
int kshim_partition_find_slot(const char *base, uint32_t slot,
                              struct kshim_blkdev *disk,
                              struct kshim_partition **out);
