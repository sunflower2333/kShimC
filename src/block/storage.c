/* SPDX-License-Identifier: MIT */
#include <stdio.h>

#include <config.h>
#include <kshim/block.h>
#ifdef CONFIG_FIRMWARE_LOADER
#include <kshim/firmware.h>
#endif

const char *kshim_storage_strerror(int status)
{
    switch (status) {
    case ERROR_NONE: return "success";
    case ERROR_FW: return "firmware error";
    case ERROR_DEVICE_ERROR: return "device I/O error";
    case ERROR_INVALID_PARAMETER: return "invalid parameter";
    case ERROR_NOT_SUPPORTED: return "not supported";
    case ERROR_NOT_FOUND: return "not found";
    case ERROR_TIMEOUT: return "timeout";
    case KSHIM_ERR_CORRUPT: return "corrupt metadata";
    case KSHIM_ERR_NO_MEMORY: return "out of memory";
    case KSHIM_ERR_TOO_LARGE: return "too large";
    case KSHIM_ERR_NOT_READY: return "not ready";
    case KSHIM_ERR_EXISTS: return "already exists";
    case KSHIM_ERR_BUSY: return "busy";
    default: return "unknown storage error";
    }
}

unsigned kshim_storage_init(const void *fdt)
{
    unsigned partitions = kshim_gpt_scan_all();

#ifdef CONFIG_FIRMWARE_LOADER
    {
        uint32_t slot;
        int status = kshim_bootslot_capture_fdt(fdt);

        if (status == ERROR_NONE && kshim_bootslot_get(&slot) == ERROR_NONE)
            printf("firmware: current Android slot _%c\n", 'a' + (int)slot);
        else
            printf("firmware: current Android slot unavailable (%s)\n",
                   kshim_storage_strerror(status));
    }
#else
    (void)fdt;
#endif
    return partitions;
}
