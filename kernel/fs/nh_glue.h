/* SPDX-License-Identifier: MIT */
#pragma once
/* Private glue between kShimC block devices and CrDK FirmwareLoaderLib. */
#include <Library/FirmwareLoaderLib/NhFs.h>

#include <kshim/block.h>

/* A byte window [base, base + bytes) on a block device, as an NH_READER. */
struct kshim_nh_window {
    struct kshim_blkdev *dev;
    uint64_t base;
    uint64_t bytes;
    int last_error; /* last kShimC status seen by the reader, 0 if none */
};

/* Fill `reader` for `window`.  The window must outlive the reader. */
void kshim_nh_reader(struct kshim_nh_window *window, NH_READER *reader);

/* Heap callbacks for NH_READER/NH_POLICY_IO (context ignored). */
void *kshim_nh_alloc(void *context, size_t bytes);
void kshim_nh_free(void *context, void *pointer);

/* Map an NH_STATUS to a kShimC status.  NH_IO reports `io_error` when the
 * reader recorded one, else ERROR_DEVICE_ERROR. */
int kshim_nh_status(NH_STATUS status, int io_error);

/* Validate and normalise a caller path: leading '/' are skipped. */
int kshim_nh_path(const char *path, const char **out);
