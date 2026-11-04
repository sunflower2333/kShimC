/* SPDX-License-Identifier: MIT */
#pragma once
/*
 * Renesas uPD720201 / uPD720202 xHCI firmware download (owner xhci).
 *
 * Clean-room implementation from the Renesas uPD720201/uPD720202 User's
 * Manual: Hardware (R19UH0078EJ, the "FW Download" and "External ROM"
 * PCI configuration registers and the download sequence described there):
 *
 *   0x6C  Firmware Version (RO)
 *   0xF4  FW Download Control and Status (16-bit)
 *           bit 0 FW Download Enable, bit 1 FW Download Lock,
 *           bits 6:4 Result Code (000 none, 001 success, 010 error),
 *           bit 8 Set DATA0, bit 9 Set DATA1 (set by software, cleared by
 *           the controller once it consumed the data register)
 *   0xF6  External ROM Access Control and Status (16-bit)
 *           bit 0 ROM Access Enable, bits 6:4 Result Code,
 *           bit 15 External ROM Exists (RO)
 *   0xF8  DATA0, 0xFC DATA1 (32-bit firmware data)
 *
 * Download sequence: set FW Download Enable; write the first two firmware
 * dwords to DATA0/DATA1 and set both Set DATA bits; then alternately wait
 * for Set DATA0 (DATA1) to clear, write the next dword and set the bit;
 * after the last dword wait for both bits to clear, clear FW Download
 * Enable and check that the Result Code reads success.
 *
 * Image format (the manual's "Data Format" section): little-endian dwords,
 * the first two bytes are AA 55 (0x55AA read as a 16-bit LE value); images
 * are between 4 KiB and 64 KiB.
 *
 * The firmware lives in controller RAM until the next PCI reset or power
 * loss. kShimC never asserts either, so a downloaded image stays for the
 * payload, which then finds "already running".
 *
 * Config access goes through struct renesas_cfg so the sequence can be run
 * against a register model (tests/io/renesas_fw_test.c). On hardware it is
 * kshim_pci_read_config32 / kshim_pci_write_config_raw32 (never journaled:
 * replaying 0xF4-0xFC would have side effects).
 */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define RENESAS_FW_VERSION      0x6c
#define RENESAS_FW_STATUS       0xf4
#define RENESAS_ROM_STATUS      0xf6
#define RENESAS_DATA0           0xf8
#define RENESAS_DATA1           0xfc

#define RENESAS_FW_ENABLE       (1U << 0)
#define RENESAS_FW_LOCK         (1U << 1)
#define RENESAS_RESULT(v)       (((v) >> 4) & 7U)
#define RENESAS_RESULT_NONE     0U
#define RENESAS_RESULT_SUCCESS  1U
#define RENESAS_RESULT_ERROR    2U
#define RENESAS_SET_DATA0       (1U << 8)
#define RENESAS_SET_DATA1       (1U << 9)
#define RENESAS_ROM_ACCESS      (1U << 0)
#define RENESAS_ROM_EXISTS      (1U << 15)

#define RENESAS_FW_MIN_SIZE     0x1000U
#define RENESAS_FW_MAX_SIZE     0x10000U
#define RENESAS_FW_SIGNATURE    0x55aaU

struct renesas_cfg {
    void *context;
    int (*read32)(void *context, uint16_t reg, uint32_t *value);
    /* Raw (not journaled) config write. */
    int (*write32)(void *context, uint16_t reg, uint32_t value);
    /* Bounded wait step in microseconds (delay()). */
    void (*delay_us)(void *context, uint32_t us);
};

enum renesas_fw_state {
    RENESAS_FW_RUNNING = 0,   /* downloaded earlier (UEFI XhciDxe, a previous boot stage) */
    RENESAS_FW_FROM_ROM,      /* external ROM present and its load succeeded */
    RENESAS_FW_NEEDED,        /* no firmware: download required */
    RENESAS_FW_FAILED,        /* error/stale state that only a reset clears */
};

/* Read the controller's firmware state. *raw receives the 0xF4 dword. */
int renesas_fw_state(const struct renesas_cfg *cfg, enum renesas_fw_state *state, uint32_t *raw);
/* 0 or KSHIM_IO_ERR_PROTOCOL for a malformed image. */
int renesas_fw_validate(const uint8_t *image, size_t size);
/* Run the download sequence; 0 when the controller reports success. A
 * trailing partial dword is zero-padded. */
int renesas_fw_download(const struct renesas_cfg *cfg, const uint8_t *image, size_t size);
