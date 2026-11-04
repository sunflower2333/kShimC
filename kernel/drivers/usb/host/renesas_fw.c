/* SPDX-License-Identifier: MIT */
/*
 * Renesas uPD720201/uPD720202 firmware download over PCI configuration
 * space. Register and sequence facts: see renesas_fw.h (Renesas user's
 * manual R19UH0078EJ). Written independently; no Linux or CrDK
 * (RenesasXhciCrLib, GPL-2.0-only) code was used.
 */
#include "renesas_fw.h"

#include <kshim/io.h>

#include <stdio.h>
#include <string.h>

/* Per data-register handshake and for the final result. The manual gives
 * no figure; the controller consumes a dword within microseconds. */
#define RENESAS_STEP_TIMEOUT_US   20000U
#define RENESAS_RESULT_TIMEOUT_US 200000U
#define RENESAS_POLL_US           1U

static int cfg_read(const struct renesas_cfg *cfg, uint16_t reg, uint32_t *value)
{
    int status = cfg->read32(cfg->context, reg, value);
    if (!status && *value == UINT32_MAX) return KSHIM_IO_ERR_NO_DEVICE;
    return status;
}

/* 0xF4 and 0xF6 share a dword. Writes keep the ROM half at zero: ROM
 * Access Enable off and no ROM command bit set (all write-one-to-act),
 * which is its idle state checked before the download starts. */
static int fw_control(const struct renesas_cfg *cfg, bool enable, uint32_t set_bits)
{
    return cfg->write32(cfg->context, RENESAS_FW_STATUS, (enable ? RENESAS_FW_ENABLE : 0) | set_bits);
}

int renesas_fw_state(const struct renesas_cfg *cfg, enum renesas_fw_state *state, uint32_t *raw)
{
    uint32_t v;
    *state = RENESAS_FW_FAILED;
    int status = cfg_read(cfg, RENESAS_FW_STATUS, &v);
    if (raw) *raw = status ? 0 : v;
    if (status) return status;
    uint32_t fw = v & 0xffffU, rom = v >> 16;
    unsigned result = RENESAS_RESULT(fw);
    if (fw & RENESAS_FW_LOCK) {
        /* Locked: usable only when the locked image runs. */
        *state = result == RENESAS_RESULT_SUCCESS ? RENESAS_FW_RUNNING : RENESAS_FW_FAILED;
        return 0;
    }
    if (fw & RENESAS_FW_ENABLE) {
        /* A download someone else started and never finished. */
        *state = RENESAS_FW_FAILED;
        return 0;
    }
    if (result == RENESAS_RESULT_SUCCESS) {
        *state = RENESAS_FW_RUNNING;
        return 0;
    }
    if (result != RENESAS_RESULT_NONE) {
        *state = RENESAS_FW_FAILED;
        return 0;
    }
    if ((rom & RENESAS_ROM_EXISTS) && RENESAS_RESULT(rom) == RENESAS_RESULT_SUCCESS) {
        *state = RENESAS_FW_FROM_ROM;
        return 0;
    }
    *state = RENESAS_FW_NEEDED;
    return 0;
}

int renesas_fw_validate(const uint8_t *image, size_t size)
{
    if (!image || size < RENESAS_FW_MIN_SIZE || size > RENESAS_FW_MAX_SIZE) return KSHIM_IO_ERR_PROTOCOL;
    if ((uint16_t)(image[0] | image[1] << 8) != RENESAS_FW_SIGNATURE) return KSHIM_IO_ERR_PROTOCOL;
    return 0;
}

static uint32_t image_dword(const uint8_t *image, size_t size, size_t index)
{
    uint8_t b[4] = {0, 0, 0, 0};
    size_t at = index * 4U;
    for (size_t i = 0; i < 4 && at + i < size; ++i) b[i] = image[at + i];
    return (uint32_t)b[0] | (uint32_t)b[1] << 8 | (uint32_t)b[2] << 16 | (uint32_t)b[3] << 24;
}

static int wait_clear(const struct renesas_cfg *cfg, uint32_t mask, uint32_t timeout_us)
{
    for (uint32_t waited = 0;; waited += RENESAS_POLL_US) {
        uint32_t v;
        int status = cfg_read(cfg, RENESAS_FW_STATUS, &v);
        if (status) return status;
        if (!(v & mask)) return 0;
        if (waited >= timeout_us) return ERROR_TIMEOUT;
        cfg->delay_us(cfg->context, RENESAS_POLL_US);
    }
}

int renesas_fw_download(const struct renesas_cfg *cfg, const uint8_t *image, size_t size)
{
    int status = renesas_fw_validate(image, size);
    if (status) return status;
    uint32_t v;
    if ((status = cfg_read(cfg, RENESAS_FW_STATUS, &v))) return status;
    if ((v & (RENESAS_FW_LOCK | RENESAS_FW_ENABLE)) || ((v >> 16) & RENESAS_ROM_ACCESS))
        return ERROR_BUSY;
    size_t dwords = (size + 3U) / 4U;

    /* 1. FW Download Enable. */
    if ((status = fw_control(cfg, true, 0))) return status;
    /* 2. First two dwords, then both Set DATA bits. */
    if ((status = cfg->write32(cfg->context, RENESAS_DATA0, image_dword(image, size, 0)))) goto abort;
    if ((status = cfg->write32(cfg->context, RENESAS_DATA1, image_dword(image, size, 1)))) goto abort;
    if ((status = fw_control(cfg, true, RENESAS_SET_DATA0 | RENESAS_SET_DATA1))) goto abort;
    /* 3. Alternate DATA0/DATA1, each after its Set bit cleared. */
    for (size_t i = 2; i < dwords; ++i) {
        bool odd = i & 1U;
        uint32_t bit = odd ? RENESAS_SET_DATA1 : RENESAS_SET_DATA0;
        if ((status = wait_clear(cfg, bit, RENESAS_STEP_TIMEOUT_US))) goto abort;
        if ((status = cfg->write32(cfg->context, odd ? RENESAS_DATA1 : RENESAS_DATA0,
                                   image_dword(image, size, i)))) goto abort;
        if ((status = fw_control(cfg, true, bit))) goto abort;
    }
    /* 4. Both consumed, then end the download. */
    if ((status = wait_clear(cfg, RENESAS_SET_DATA0 | RENESAS_SET_DATA1, RENESAS_STEP_TIMEOUT_US)))
        goto abort;
    if ((status = fw_control(cfg, false, 0))) return status;
    /* 5. Result Code. */
    for (uint32_t waited = 0;; waited += 10U) {
        if ((status = cfg_read(cfg, RENESAS_FW_STATUS, &v))) return status;
        unsigned result = RENESAS_RESULT(v);
        if (result == RENESAS_RESULT_SUCCESS) return 0;
        if (result != RENESAS_RESULT_NONE) return ERROR_FW;
        if (waited >= RENESAS_RESULT_TIMEOUT_US) return ERROR_TIMEOUT;
        cfg->delay_us(cfg->context, 10U);
    }

abort:
    (void)fw_control(cfg, false, 0);
    return status;
}
