/* SPDX-License-Identifier: MIT */
#pragma once
/*
 * Read-mostly block device interface shared by storage drivers (UFS, SDHCI)
 * and consumers (partition tables, filesystems, firmware loader).
 *
 * All calls are synchronous and polling; no interrupts are assumed.
 * Return values: 0 on success, otherwise a positive ERROR_* code from
 * error_no.h.
 */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "error_no.h"

#define KSHIM_BLKDEV_NAME_MAX 32

enum kshim_blkdev_kind {
    KSHIM_BLKDEV_UFS_LU,     /* one UFS logical unit */
    KSHIM_BLKDEV_EMMC,       /* eMMC user area or boot partition */
    KSHIM_BLKDEV_SD,         /* removable SD card */
    KSHIM_BLKDEV_PARTITION,  /* GPT partition view on a parent device */
    KSHIM_BLKDEV_MEMORY,     /* RAM/image backed, used by tests */
};

struct kshim_blkdev;

struct kshim_blkdev_ops {
    /* Read `count` blocks starting at `lba` into `buf` (count * block_size bytes). */
    int (*read)(struct kshim_blkdev *dev, uint64_t lba, uint32_t count, void *buf);
    /* Optional; NULL for read-only devices. */
    int (*write)(struct kshim_blkdev *dev, uint64_t lba, uint32_t count, const void *buf);
    /* Optional; flush volatile caches. */
    int (*flush)(struct kshim_blkdev *dev);
};

struct kshim_blkdev {
    char name[KSHIM_BLKDEV_NAME_MAX]; /* e.g. "ufs0:lu0", "sdhc1", "ufs0:lu4:modem_a" */
    enum kshim_blkdev_kind kind;
    uint32_t block_size;              /* bytes, power of two, >= 512 */
    uint64_t num_blocks;
    uint32_t max_transfer_blocks;     /* 0 = no limit; callers split larger reads */
    bool read_only;
    const struct kshim_blkdev_ops *ops;
    struct kshim_blkdev *parent;      /* set for KSHIM_BLKDEV_PARTITION */
    uint64_t start_lba;               /* offset into parent, partition only */
    void *priv;                       /* driver private data */
    struct kshim_blkdev *next;        /* registry link, owned by blkdev core */
};

/* Registry (implemented by the blkdev core). */
int kshim_blkdev_register(struct kshim_blkdev *dev);
struct kshim_blkdev *kshim_blkdev_find(const char *name);
struct kshim_blkdev *kshim_blkdev_first(void);

/* Bounds-checked helpers that split by max_transfer_blocks. */
int kshim_blkdev_read(struct kshim_blkdev *dev, uint64_t lba, uint32_t count, void *buf);
/* Byte-granular read through a bounce buffer; offset/len need not be aligned. */
int kshim_blkdev_read_bytes(struct kshim_blkdev *dev, uint64_t offset, size_t len, void *buf);
