/* SPDX-License-Identifier: MIT */
#pragma once
/*
 * SD / eMMC card layer for kShimC on top of CrDK's SdhciMsmLib core
 * (CR_SDHC: bounded, polled SDHCI PIO register engine with injectable IO).
 *
 * The CrDK core owns SDHCI register semantics (reset, clock enable, command
 * issue, PIO data port, interrupt status, Qualcomm v5 power handshake). This
 * layer owns what CrDK's UEFI driver delegated to MU MmcDxe: card
 * identification (SD v1/v2/SDHC/SDXC and eMMC), CSD/EXT_CSD capacity,
 * bus-width selection, CMD17/18 reads (CMD24/25 writes on request) and the
 * kShimC block-device registration.
 */
#include <stdbool.h>
#include <stdint.h>

#include <Library/SdhciMsmLib.h>
#include <kshim/blkdev.h>

struct sdmmc_host;

/* Platform hooks. NULL hooks are skipped. */
struct sdmmc_platform_ops {
    /* Right after CrSdhcInit, before any MMIO (e.g. install core.SyncPower). */
    int (*attach)(struct sdmmc_host *host);
    /* After RESET_ALL with card clock and bus power off (vendor POR state). */
    int (*prepare)(struct sdmmc_host *host);
    /* Card clock is stopped; select the controller input clock for hz. */
    int (*set_clock)(struct sdmmc_host *host, uint32_t hz);
    /* After bus width selection, before the card clock restarts. */
    int (*set_timing)(struct sdmmc_host *host, uint32_t hz);
    /* Bus power request issued (on) or about to be removed (off). */
    int (*power)(struct sdmmc_host *host, bool on);
    int (*card_present)(struct sdmmc_host *host, bool *present);
    /* Called from every wait loop (e.g. to service a polled power IRQ). */
    int (*poll)(struct sdmmc_host *host);
};

enum sdmmc_card_type { SDMMC_NONE, SDMMC_SD, SDMMC_EMMC };

struct sdmmc_host {
    /* Caller supplied. */
    CR_SDHC_IO io;
    const struct sdmmc_platform_ops *ops;
    void *platform;
    char name[16];                 /* "sdhc2" */
    uint32_t max_hz;               /* 0: 25 MHz default speed */
    bool allow_write;
    bool emmc_only, sd_only;       /* skip probing the other card family */
    /* State. */
    CR_SDHC core;
    enum sdmmc_card_type type;
    bool high_capacity;            /* block addressed */
    uint16_t rca;
    uint32_t ocr, cid[4], csd[4];
    uint32_t clock_hz;
    uint8_t bus_width;
    uint64_t blocks;               /* 512-byte blocks */
    uint32_t last_events, last_errors;
    struct kshim_blkdev dev;
    bool ready;
};

int sdmmc_init(struct sdmmc_host *host);   /* identify the card */
int sdmmc_probe(struct sdmmc_host *host);  /* init + register "<name>" */
int sdmmc_read(struct sdmmc_host *host, uint64_t lba, uint32_t count, void *buf);
int sdmmc_write(struct sdmmc_host *host, uint64_t lba, uint32_t count, const void *buf);
/* Stop the card clock and reset command/data lines; card stays selected. */
int sdmmc_quiesce(struct sdmmc_host *host);
/* Raw command, exported for tests: returns 0 and fills resp[4] (R2 as 4 words). */
int sdmmc_cmd(struct sdmmc_host *host, uint8_t index, uint32_t arg, CR_SDHC_RESPONSE type,
              uint32_t resp[4]);
