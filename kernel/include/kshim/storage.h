/* SPDX-License-Identifier: MIT */
#pragma once
/*
 * Storage-layer status codes shared by the block core, the filesystem adapter
 * and the firmware loader.
 *
 * Every storage API returns 0 on success or a positive code.  Codes below
 * KSHIM_ERR_STORAGE_BASE are the generic ERROR_* values from error_no.h; the
 * values here extend them for conditions error_no.h cannot express.  They
 * live in their own header (and numeric range) so error_no.h stays shared
 * and unchanged.
 */
#include "error_no.h"

#define KSHIM_ERR_STORAGE_BASE 64

enum {
    /* On-disk metadata is malformed or inconsistent.  Never means "absent". */
    KSHIM_ERR_CORRUPT = KSHIM_ERR_STORAGE_BASE,
    /* Heap allocation failed. */
    KSHIM_ERR_NO_MEMORY,
    /* Object exceeds a caller or library size bound. */
    KSHIM_ERR_TOO_LARGE,
    /* Required evidence (storage, boot slot, Virtual A/B state) is not
     * available yet; the caller may retry later.  Never means "absent". */
    KSHIM_ERR_NOT_READY,
    /* Name or object already exists. */
    KSHIM_ERR_EXISTS,
    /* Object is still referenced (for example a disk with partitions). */
    KSHIM_ERR_BUSY,
};

/* Static, never-NULL description of any storage or ERROR_* code. */
const char *kshim_storage_strerror(int status);

/*
 * Boot-time storage bring-up, called once on the boot CPU after storage
 * drivers have registered their whole-disk block devices.
 *  - scans GPT on every registered whole disk without partitions;
 *  - with CONFIG_FIRMWARE_LOADER, captures the current Android boot slot from
 *    the runtime device tree (fdt may be NULL).
 * Returns the number of partitions registered.  Errors are logged only.
 */
unsigned kshim_storage_init(const void *fdt);
