/* SPDX-License-Identifier: MIT */
#pragma once
/*
 * Polled JEDEC UFSHCI (JESD223) host core with a minimal UPIU/SCSI engine.
 *
 * Written from the UFSHCI register map and the UFS (JESD220) UPIU layouts.
 * One transfer request slot is used; no interrupts are enabled. All MMIO and
 * time goes through the injectable CrIo, all DMA addresses through
 * struct ufshci_dma, so the same code runs on hardware, QEMU and host models.
 */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <Library/cr_geni.h> /* struct CrIo */
#include <kshim/blkdev.h>

/* Host controller registers (offsets from the HCI base). */
#define UFSHCI_CAP       0x00
#define UFSHCI_VER       0x08
#define UFSHCI_IS        0x20
#define UFSHCI_IE        0x24
#define UFSHCI_HCS       0x30
#define UFSHCI_HCE       0x34
#define UFSHCI_UECPA     0x38
#define UFSHCI_UECDL     0x3c
#define UFSHCI_UTRLBA    0x50
#define UFSHCI_UTRLBAU   0x54
#define UFSHCI_UTRLDBR   0x58
#define UFSHCI_UTRLCLR   0x5c
#define UFSHCI_UTRLRSR   0x60
#define UFSHCI_UTMRLBA   0x70
#define UFSHCI_UTMRLBAU  0x74
#define UFSHCI_UTMRLDBR  0x78
#define UFSHCI_UTMRLCLR  0x7c
#define UFSHCI_UTMRLRSR  0x80
#define UFSHCI_UICCMD    0x90
#define UFSHCI_UCMDARG1  0x94
#define UFSHCI_UCMDARG2  0x98
#define UFSHCI_UCMDARG3  0x9c

#define UFSHCI_CAP_64AS        (1u << 24)
#define UFSHCI_IS_UTRCS        (1u << 0)
#define UFSHCI_IS_UE           (1u << 2)
#define UFSHCI_IS_UPMS         (1u << 4)
#define UFSHCI_IS_UCCS         (1u << 10)
/* Fatal causes: device fatal, UTP error, host controller/system bus fatal. */
#define UFSHCI_IS_FATAL        ((1u << 11) | (1u << 12) | (1u << 16) | (1u << 17))
#define UFSHCI_HCS_DP          (1u << 0)
#define UFSHCI_HCS_UTRLRDY     (1u << 1)
#define UFSHCI_HCS_UTMRLRDY    (1u << 2)
#define UFSHCI_HCS_UCRDY       (1u << 3)
#define UFSHCI_HCS_UPMCRS(v)   (((v) >> 8) & 7u)

/* UIC (DME) commands. */
#define UIC_DME_GET            0x01
#define UIC_DME_SET            0x02
#define UIC_DME_PEER_GET       0x03
#define UIC_DME_LINKSTARTUP    0x16
/* UniPro PA attributes used for link liveness. */
#define UNIPRO_PA_CONNECTED_TX_LANES 0x1561
#define UNIPRO_PA_CONNECTED_RX_LANES 0x1581
#define UNIPRO_PA_PWRMODE            0x1571

#define UFSHCI_MAX_LUS     8
#define UFSHCI_BOUNCE_SIZE (8u * 1024u)  /* unaligned buffers only */

struct ufshci_host;

/* DMA visibility. kShimC runs identity mapped, so to_dma returns the
 * physical address of a runtime buffer. clean() publishes CPU writes to the
 * device; invalidate() discards stale lines before the CPU reads device
 * writes. Both may be NULL on coherent hosts (QEMU, host tests). */
struct ufshci_dma {
    void *context;
    uint64_t (*to_dma)(void *context, const void *va);
    void (*clean)(void *context, const void *va, size_t len);
    void (*invalidate)(void *context, void *va, size_t len);
};

/* Vendor hooks around the standard bring-up. All optional. */
struct ufshci_platform_ops {
    int (*pre_hce)(struct ufshci_host *host);
    int (*post_hce)(struct ufshci_host *host);
    int (*pre_link)(struct ufshci_host *host);
    int (*post_link)(struct ufshci_host *host);
};

enum ufshci_init_path {
    UFSHCI_PATH_NONE,
    UFSHCI_PATH_INHERITED,   /* live controller/link taken over, no reset */
    UFSHCI_PATH_FULL,        /* HCE reset, link startup, fDeviceInit */
};

enum ufshci_policy {
    UFSHCI_PREFER_INHERIT,   /* take over if verifiably live, else full init */
    UFSHCI_FORCE_FULL,
    UFSHCI_INHERIT_ONLY,     /* never reset the controller */
};

struct ufshci_lu {
    struct kshim_blkdev dev;
    struct ufshci_host *host;
    uint8_t lun;
};

struct ufshci_host {
    /* Caller supplied. */
    uintptr_t base;
    struct CrIo io;
    struct ufshci_dma dma;
    const struct ufshci_platform_ops *ops;
    void *platform;            /* vendor context for ops */
    const char *name;          /* "ufs0" */
    enum ufshci_policy policy;
    bool allow_write;          /* expose ops->write on registered LUs */
    /* Driver state. */
    uint32_t cap, version;
    uint8_t nutrs;
    uint8_t task_tag;
    enum ufshci_init_path path;
    const char *inherit_reject; /* why the inherited state was not used */
    uint8_t *utrl, *utmrl, *ucd, *bounce;
    struct ufshci_lu lus[UFSHCI_MAX_LUS];
    unsigned lu_count;
    uint8_t last_sense_key, last_asc, last_ascq;
    bool ready;
    bool quiet;                /* suppress logs for expected probe failures */
};

/* Bring the controller up (inherit or full init per policy) and verify the
 * device answers NOP OUT. Does not enumerate LUs. */
int ufshci_init(struct ufshci_host *host);
/* Enumerate enabled LUs, READ CAPACITY each, and register "<name>:lu<N>". */
int ufshci_scan(struct ufshci_host *host);
/* Convenience: init + scan. */
int ufshci_probe(struct ufshci_host *host);
/* Stop the transfer lists (no HCE reset) so the next owner can take over. */
int ufshci_quiesce(struct ufshci_host *host);

/* Lower level, exported for vendor hooks and tests. */
uint32_t ufshci_read(struct ufshci_host *host, uint32_t offset);
void ufshci_write(struct ufshci_host *host, uint32_t offset, uint32_t value);
int ufshci_uic(struct ufshci_host *host, uint32_t cmd, uint16_t attr, uint16_t sel,
               uint32_t *value);
int ufshci_nop(struct ufshci_host *host);
int ufshci_query_flag(struct ufshci_host *host, uint8_t opcode, uint8_t idn, bool *value);
int ufshci_read_descriptor(struct ufshci_host *host, uint8_t idn, uint8_t index,
                           void *buf, uint16_t *len);
int ufshci_scsi(struct ufshci_host *host, uint8_t lun, const uint8_t *cdb, uint8_t cdb_len,
                void *buf, uint32_t len, bool write);
int ufshci_read_blocks(struct ufshci_lu *lu, uint64_t lba, uint32_t count, void *buf);
const char *ufshci_path_name(enum ufshci_init_path path);
