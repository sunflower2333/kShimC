/* SPDX-License-Identifier: MIT */
/* Test-only stand-in for the blkdev core registry. */
#include <kshim/blkdev.h>
#include <string.h>

#include "blkdev_stub.h"

struct kshim_blkdev *blkdev_stub_devices[BLKDEV_STUB_MAX];
unsigned blkdev_stub_count;

void blkdev_stub_reset(void)
{
    memset(blkdev_stub_devices, 0, sizeof(blkdev_stub_devices));
    blkdev_stub_count = 0;
}

int kshim_blkdev_register(struct kshim_blkdev *dev)
{
    if (!dev || !dev->ops || !dev->ops->read || blkdev_stub_count == BLKDEV_STUB_MAX)
        return ERROR_INVALID_PARAMETER;
    for (unsigned i = 0; i < blkdev_stub_count; ++i)
        if (!strcmp(blkdev_stub_devices[i]->name, dev->name)) return ERROR_INVALID_PARAMETER;
    blkdev_stub_devices[blkdev_stub_count++] = dev;
    return 0;
}

struct kshim_blkdev *kshim_blkdev_find(const char *name)
{
    for (unsigned i = 0; i < blkdev_stub_count; ++i)
        if (!strcmp(blkdev_stub_devices[i]->name, name)) return blkdev_stub_devices[i];
    return NULL;
}
