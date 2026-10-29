/* SPDX-License-Identifier: MIT */
/*
 * Block device core: registry, bounds-checked block reads split by the
 * device's max_transfer_blocks, and byte-granular reads through a bounce
 * buffer.  Polling only; callers serialize access (boot CPU).
 */
#include <stdlib.h>
#include <string.h>

#include <kshim/block.h>

static struct kshim_blkdev *registry_head;

static bool name_valid(const char *name)
{
    size_t length = 0;

    while (length < KSHIM_BLKDEV_NAME_MAX && name[length])
        ++length;
    return length > 0 && length < KSHIM_BLKDEV_NAME_MAX;
}

static bool power_of_two(uint32_t value)
{
    return value && !(value & (value - 1));
}

static bool registered(const struct kshim_blkdev *dev)
{
    for (const struct kshim_blkdev *it = registry_head; it; it = it->next)
        if (it == dev)
            return true;
    return false;
}

uint64_t kshim_blkdev_bytes(const struct kshim_blkdev *dev)
{
    if (!dev || !dev->block_size || dev->num_blocks > UINT64_MAX / dev->block_size)
        return 0;
    return dev->num_blocks * dev->block_size;
}

int kshim_blkdev_register(struct kshim_blkdev *dev)
{
    struct kshim_blkdev **tail = &registry_head;

    if (!dev || !name_valid(dev->name) || !power_of_two(dev->block_size) ||
        dev->block_size < 512 || !dev->num_blocks ||
        dev->num_blocks > UINT64_MAX / dev->block_size)
        return ERROR_INVALID_PARAMETER;
    if (dev->parent) {
        /* A child is a window onto a registered parent of the same geometry. */
        if (!registered(dev->parent) || dev->parent->block_size != dev->block_size ||
            dev->start_lba > dev->parent->num_blocks ||
            dev->num_blocks > dev->parent->num_blocks - dev->start_lba)
            return ERROR_INVALID_PARAMETER;
    } else if (dev->kind == KSHIM_BLKDEV_PARTITION || dev->start_lba) {
        return ERROR_INVALID_PARAMETER;
    }
    if ((!dev->ops || !dev->ops->read) && !dev->parent)
        return ERROR_INVALID_PARAMETER;
    for (; *tail; tail = &(*tail)->next) {
        if (*tail == dev || !strncmp((*tail)->name, dev->name, KSHIM_BLKDEV_NAME_MAX))
            return KSHIM_ERR_EXISTS;
    }
    dev->next = NULL;
    *tail = dev;
    return ERROR_NONE;
}

int kshim_blkdev_unregister(struct kshim_blkdev *dev)
{
    struct kshim_blkdev **link;

    if (!dev)
        return ERROR_INVALID_PARAMETER;
    for (struct kshim_blkdev *it = registry_head; it; it = it->next)
        if (it->parent == dev)
            return KSHIM_ERR_BUSY;
    for (link = &registry_head; *link; link = &(*link)->next) {
        if (*link == dev) {
            *link = dev->next;
            dev->next = NULL;
            return ERROR_NONE;
        }
    }
    return ERROR_NOT_FOUND;
}

struct kshim_blkdev *kshim_blkdev_find(const char *name)
{
    if (!name)
        return NULL;
    for (struct kshim_blkdev *it = registry_head; it; it = it->next)
        if (!strncmp(it->name, name, KSHIM_BLKDEV_NAME_MAX))
            return it;
    return NULL;
}

struct kshim_blkdev *kshim_blkdev_first(void)
{
    return registry_head;
}

static int driver_status(int status)
{
    if (status == 0)
        return ERROR_NONE;
    return status > 0 ? status : ERROR_DEVICE_ERROR;
}

int kshim_blkdev_read(struct kshim_blkdev *dev, uint64_t lba, uint32_t count, void *buf)
{
    uint8_t *out = buf;

    if (!dev || (!buf && count) || !dev->block_size)
        return ERROR_INVALID_PARAMETER;
    if (lba > dev->num_blocks || count > dev->num_blocks - lba)
        return ERROR_INVALID_PARAMETER;
    if (!count)
        return ERROR_NONE;
    if ((uint64_t)count * dev->block_size > SIZE_MAX)
        return ERROR_INVALID_PARAMETER;
    if (!dev->ops || !dev->ops->read) {
        /* Driverless partition window: the parent applies its own limits. */
        if (!dev->parent || dev->parent->block_size != dev->block_size ||
            dev->start_lba > UINT64_MAX - lba)
            return ERROR_INVALID_PARAMETER;
        return kshim_blkdev_read(dev->parent, dev->start_lba + lba, count, buf);
    }
    while (count) {
        uint32_t take = count;
        int status;

        if (dev->max_transfer_blocks && take > dev->max_transfer_blocks)
            take = dev->max_transfer_blocks;
        status = driver_status(dev->ops->read(dev, lba, take, out));
        if (status)
            return status;
        lba += take;
        count -= take;
        out += (size_t)take * dev->block_size;
    }
    return ERROR_NONE;
}

int kshim_blkdev_read_bytes(struct kshim_blkdev *dev, uint64_t offset, size_t len, void *buf)
{
    uint8_t *out = buf, *bounce = NULL;
    size_t bounce_size = 0, need = 0;
    uint64_t total, lba, middle_blocks;
    uint32_t bs, within;
    int status = ERROR_NONE;

    if (!dev || (!buf && len) || !power_of_two(dev->block_size))
        return ERROR_INVALID_PARAMETER;
    total = kshim_blkdev_bytes(dev);
    if (offset > total || len > total - offset)
        return ERROR_INVALID_PARAMETER;
    if (!len)
        return ERROR_NONE;

    bs = dev->block_size;
    within = (uint32_t)(offset % bs);
    lba = offset / bs;

    /* Size one bounce allocation for every phase of this call. */
    {
        size_t head = within ? (len < bs - within ? len : bs - within) : 0;
        size_t rest = len - head;
        bool middle_aligned = !(((uintptr_t)(out + head)) % KSHIM_BLKDEV_DMA_ALIGN);

        middle_blocks = rest / bs;
        if (head || rest % bs)
            need = bs;
        if (middle_blocks && !middle_aligned) {
            uint64_t want = middle_blocks * bs;
            size_t cap = KSHIM_BLKDEV_BOUNCE_MAX / bs * bs;

            if (cap < bs)
                cap = bs;
            if (want > cap)
                want = cap;
            if (want > need)
                need = (size_t)want;
        }
    }
    if (need) {
        bounce_size = (need + KSHIM_BLKDEV_DMA_ALIGN - 1) / KSHIM_BLKDEV_DMA_ALIGN *
                      KSHIM_BLKDEV_DMA_ALIGN;
        bounce = aligned_alloc(KSHIM_BLKDEV_DMA_ALIGN, bounce_size);
        if (!bounce)
            return KSHIM_ERR_NO_MEMORY;
    }

    if (within) {
        size_t take = len < bs - within ? len : bs - within;

        status = kshim_blkdev_read(dev, lba, 1, bounce);
        if (status)
            goto done;
        memcpy(out, bounce + within, take);
        out += take;
        len -= take;
        ++lba;
    }
    while (len >= bs) {
        uint64_t blocks = len / bs;
        uint32_t take;

        if (!(((uintptr_t)out) % KSHIM_BLKDEV_DMA_ALIGN)) {
            take = blocks > UINT32_MAX ? UINT32_MAX : (uint32_t)blocks;
            status = kshim_blkdev_read(dev, lba, take, out);
        } else {
            uint64_t room = bounce_size / bs;

            take = (uint32_t)(blocks < room ? blocks : room);
            status = kshim_blkdev_read(dev, lba, take, bounce);
            if (!status)
                memcpy(out, bounce, (size_t)take * bs);
        }
        if (status)
            goto done;
        out += (size_t)take * bs;
        len -= (size_t)take * bs;
        lba += take;
    }
    if (len) {
        status = kshim_blkdev_read(dev, lba, 1, bounce);
        if (!status)
            memcpy(out, bounce, len);
    }
done:
    free(bounce);
    return status;
}
