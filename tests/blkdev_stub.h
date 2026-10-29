/* SPDX-License-Identifier: MIT */
#pragma once
#include <kshim/blkdev.h>
#define BLKDEV_STUB_MAX 16
extern struct kshim_blkdev *blkdev_stub_devices[BLKDEV_STUB_MAX];
extern unsigned blkdev_stub_count;
void blkdev_stub_reset(void);
