/* SPDX-License-Identifier: MIT */
/* liblp (Android dynamic partition) logical devices over CrDK NhSuperOpen. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <Library/FirmwareLoaderLib/NhPolicy.h>
#include <kshim/fs.h>

#include "nh_glue.h"

#define MISC_VIRTUAL_AB_OFFSET 32768U

struct kshim_lp_dev {
    struct kshim_blkdev dev;
    struct kshim_nh_window window; /* whole super device */
    NH_VIEW view;
};

static int lp_read(struct kshim_blkdev *dev, uint64_t lba, uint32_t count, void *buf)
{
    struct kshim_lp_dev *lp = dev->priv;
    NH_STATUS status;

    if (lba > dev->num_blocks || count > dev->num_blocks - lba)
        return ERROR_INVALID_PARAMETER;
    lp->window.last_error = 0;
    status = NhRead(&lp->view.Reader, lba * dev->block_size, buf,
                    (size_t)count * dev->block_size);
    return kshim_nh_status(status, lp->window.last_error);
}

static const struct kshim_blkdev_ops lp_ops = { .read = lp_read };

int kshim_lp_open(struct kshim_blkdev *super, uint32_t metadata_slot,
                  const char *name, struct kshim_lp_dev **out)
{
    struct kshim_lp_dev *lp;
    NH_READER reader;
    int status, length;

    if (!out)
        return ERROR_INVALID_PARAMETER;
    *out = NULL;
    if (!super || !name || !kshim_blkdev_bytes(super))
        return ERROR_INVALID_PARAMETER;
    lp = calloc(1, sizeof(*lp));
    if (!lp)
        return KSHIM_ERR_NO_MEMORY;
    lp->window.dev = super;
    lp->window.bytes = kshim_blkdev_bytes(super);
    kshim_nh_reader(&lp->window, &reader);
    status = kshim_nh_status(NhSuperOpen(&reader, metadata_slot, name, &lp->view),
                             lp->window.last_error);
    if (!status && lp->view.Reader.Bytes % super->block_size)
        status = ERROR_NOT_SUPPORTED; /* not representable in device blocks */
    if (!status && !lp->view.Reader.Bytes)
        status = KSHIM_ERR_CORRUPT;
    if (status) {
        NhViewClose(&lp->view);
        free(lp);
        return status;
    }
    length = snprintf(lp->dev.name, sizeof(lp->dev.name), "%s:%s", super->name, name);
    if (length < 0 || (size_t)length >= sizeof(lp->dev.name))
        snprintf(lp->dev.name, sizeof(lp->dev.name), "lp:%.28s", name);
    lp->dev.kind = KSHIM_BLKDEV_PARTITION;
    lp->dev.block_size = super->block_size;
    lp->dev.num_blocks = lp->view.Reader.Bytes / super->block_size;
    lp->dev.max_transfer_blocks = super->max_transfer_blocks;
    lp->dev.read_only = true;
    lp->dev.ops = &lp_ops;
    lp->dev.parent = super;
    lp->dev.start_lba = 0; /* extents are mapped by lp_read, not a window */
    lp->dev.priv = lp;
    *out = lp;
    return ERROR_NONE;
}

struct kshim_blkdev *kshim_lp_blkdev(struct kshim_lp_dev *lp)
{
    return lp ? &lp->dev : NULL;
}

int kshim_lp_close(struct kshim_lp_dev *lp)
{
    int status;

    if (!lp)
        return ERROR_NONE;
    status = kshim_blkdev_unregister(&lp->dev);
    if (status == KSHIM_ERR_BUSY)
        return status;
    NhViewClose(&lp->view);
    free(lp);
    return ERROR_NONE;
}

int kshim_lp_snapshot_check(struct kshim_blkdev *misc, uint8_t message[64])
{
    uint8_t first[64], again[64];
    int status;

    if (!misc)
        return ERROR_INVALID_PARAMETER;
    status = kshim_blkdev_read_bytes(misc, MISC_VIRTUAL_AB_OFFSET, sizeof(first), first);
    if (status)
        return status;
    status = kshim_nh_status(NhSnapshotCheck(first), 0);
    if (status)
        return status;
    status = kshim_blkdev_read_bytes(misc, MISC_VIRTUAL_AB_OFFSET, sizeof(again), again);
    if (status)
        return status;
    if (memcmp(first, again, sizeof(first)))
        return KSHIM_ERR_NOT_READY;
    if (message)
        memcpy(message, first, sizeof(first));
    return ERROR_NONE;
}
