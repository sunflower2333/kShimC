/* SPDX-License-Identifier: MIT */
/* RAM/image backed block device (KSHIM_BLKDEV_MEMORY) for tests and RAM disks. */
#include <string.h>

#include <kshim/block.h>

static struct kshim_memdisk *memdisk_of(struct kshim_blkdev *dev)
{
    return (struct kshim_memdisk *)dev->priv;
}

static bool in_range(const struct kshim_blkdev *dev, uint64_t lba, uint32_t count)
{
    return lba <= dev->num_blocks && count <= dev->num_blocks - lba;
}

static int memdisk_read(struct kshim_blkdev *dev, uint64_t lba, uint32_t count, void *buf)
{
    struct kshim_memdisk *disk = memdisk_of(dev);

    if (!disk || !in_range(dev, lba, count))
        return ERROR_INVALID_PARAMETER;
    disk->read_calls++;
    if (count > disk->largest_read)
        disk->largest_read = count;
    if (disk->fail_status && lba < disk->fail_lba + disk->fail_count &&
        disk->fail_lba < lba + count)
        return disk->fail_status;
    memcpy(buf, disk->data + lba * dev->block_size, (size_t)count * dev->block_size);
    return ERROR_NONE;
}

static int memdisk_write(struct kshim_blkdev *dev, uint64_t lba, uint32_t count, const void *buf)
{
    struct kshim_memdisk *disk = memdisk_of(dev);

    if (!disk || dev->read_only || !in_range(dev, lba, count))
        return ERROR_INVALID_PARAMETER;
    disk->write_calls++;
    memcpy(disk->data + lba * dev->block_size, buf, (size_t)count * dev->block_size);
    return ERROR_NONE;
}

static int memdisk_flush(struct kshim_blkdev *dev)
{
    (void)dev;
    return ERROR_NONE;
}

static const struct kshim_blkdev_ops memdisk_rw_ops = {
    .read = memdisk_read, .write = memdisk_write, .flush = memdisk_flush,
};
static const struct kshim_blkdev_ops memdisk_ro_ops = {
    .read = memdisk_read, .write = NULL, .flush = memdisk_flush,
};

int kshim_memdisk_init(struct kshim_memdisk *disk, const char *name,
                       void *data, uint64_t bytes, uint32_t block_size,
                       uint32_t max_transfer_blocks, bool read_only)
{
    size_t length;

    if (!disk || !name || !data || !block_size || (block_size & (block_size - 1)) ||
        block_size < 512 || !bytes || bytes % block_size || bytes > SIZE_MAX)
        return ERROR_INVALID_PARAMETER;
    length = strlen(name);
    if (!length || length >= KSHIM_BLKDEV_NAME_MAX)
        return ERROR_INVALID_PARAMETER;
    memset(disk, 0, sizeof(*disk));
    memcpy(disk->dev.name, name, length + 1);
    disk->dev.kind = KSHIM_BLKDEV_MEMORY;
    disk->dev.block_size = block_size;
    disk->dev.num_blocks = bytes / block_size;
    disk->dev.max_transfer_blocks = max_transfer_blocks;
    disk->dev.read_only = read_only;
    disk->dev.ops = read_only ? &memdisk_ro_ops : &memdisk_rw_ops;
    disk->dev.priv = disk;
    disk->data = data;
    return ERROR_NONE;
}
