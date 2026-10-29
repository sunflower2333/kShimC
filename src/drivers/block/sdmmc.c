/* SPDX-License-Identifier: MIT */
/*
 * SD / eMMC card identification and block access over CrDK's CR_SDHC core.
 *
 * Written natively from the SD Physical Layer Simplified Specification and
 * JEDEC eMMC (JESD84) command sets; CrDK supplies the SDHCI register engine.
 * PIO only (no DMA addresses are ever given to the controller), default
 * speed (<= 25 MHz), 1/4-bit bus, polled with bounded waits.
 */
#include <kshim/sdmmc.h>

#include <stdio.h>
#include <string.h>

#define SD_LOG(h, fmt, ...) printf("%s: " fmt "\n", (h)->name[0] ? (h)->name : "sdhc", ##__VA_ARGS__)

#define EV_CMD   CR_SDHC_EVENT_COMMAND
#define EV_XFER  CR_SDHC_EVENT_TRANSFER
#define EV_RREADY CR_SDHC_EVENT_READ_READY
#define EV_WREADY CR_SDHC_EVENT_WRITE_READY
#define EV_ENABLE (EV_CMD | EV_XFER | EV_RREADY | EV_WREADY | 0x8000u)

#define R1_ERRORS 0xfdf98008u     /* card status error bits (SD/JESD84 R1) */
#define OCR_BUSY  (1u << 31)
#define OCR_HCS   (1u << 30)
#define SD_OCR_VDD 0x00ff8000u    /* 2.7-3.6 V */
#define MMC_OCR_SECTOR (2u << 29)
#define BLOCK 512u
#define MAX_BLOCKS 256u

struct sdmmc_data {
    uint8_t *buf;
    uint16_t blocks, block_size;
    bool write;
};

static int map(CR_STATUS s)
{
    switch (s) {
    case CR_SUCCESS: return 0;
    case CR_TIMEOUT: return ERROR_TIMEOUT;
    case CR_INVALID_PARAMETER: return ERROR_INVALID_PARAMETER;
    case CR_UNSUPPORTED: return ERROR_NOT_SUPPORTED;
    default: return ERROR_DEVICE_ERROR;
    }
}

static void stall(struct sdmmc_host *h, uint32_t us)
{
    (void)h->core.Io.StallUs(h->core.Io.Context, us);
}

static void recover(struct sdmmc_host *h)
{
    (void)CrSdhcReset(&h->core, 6);
    (void)CrSdhcClearEvents(&h->core, 0xffff);
    h->core.DataActive = false;
}

/* Wait for every bit in `required`, accumulating events seen earlier. */
static int wait_events(struct sdmmc_host *h, uint32_t required, uint32_t *seen, uint32_t limit_us)
{
    for (uint32_t t = 0;; t += 10) {
        if (h->ops && h->ops->poll) {
            int s = h->ops->poll(h);
            if (s) return s;
        }
        UINT32 ev = 0, err = 0;
        CR_STATUS s = CrSdhcInterrupt(&h->core, &ev, &err);
        if (s != CR_SUCCESS) return map(s);
        *seen |= ev;
        h->last_events = *seen;
        if (err) {
            h->last_errors = err;
            /* Command or data timeout versus CRC/end-bit/index/ADMA errors. */
            return err & ((1u << 0) | (1u << 4)) ? ERROR_TIMEOUT : ERROR_DEVICE_ERROR;
        }
        if ((*seen & required) == required) {
            *seen &= ~required;
            return 0;
        }
        if (t >= limit_us) return ERROR_TIMEOUT;
        stall(h, 10);
    }
}

static bool r1_type(uint8_t index, CR_SDHC_RESPONSE type, bool app)
{
    if (type != CrSdResponse48 && type != CrSdResponse48Busy) return false;
    if (app && index == 41) return false;          /* R3 */
    if (index == 3 && type == CrSdResponse48 && !app) return false; /* SD R6 checked separately */
    if (index == 8 && type == CrSdResponse48 && !app) return false; /* SD R7 / MMC R1 handled by caller */
    return true;
}

static int command(struct sdmmc_host *h, uint8_t index, uint32_t arg, CR_SDHC_RESPONSE type,
                   bool app, struct sdmmc_data *data, uint32_t resp[4])
{
    CR_SDHC_COMMAND d;
    memset(&d, 0, sizeof(d));
    d.Index = index;
    d.Argument = arg;
    d.Response = type;
    if (data) {
        d.Data = true;
        d.Read = !data->write;
        d.BlockSize = data->block_size;
        d.BlockCount = data->blocks;
        d.Buffer = data->buf;
        d.Length = (UINT32)data->block_size * data->blocks;
        d.AutoCmd12 = data->blocks > 1;
    }
    uint32_t seen = 0, required = 0;
    int status = map(CrSdhcClearEvents(&h->core, 0xffff));
    if (!status) status = map(CrSdhcIssue(&h->core, &d, &required));
    if (!status)
        status = wait_events(h, EV_CMD | (type == CrSdResponse48Busy ? EV_XFER : 0), &seen, 100000);
    UINT8 raw[16] = {0};
    if (!status) status = map(CrSdhcResponse(&h->core, type, raw));
    if (!status) {
        uint32_t w[4];
        memcpy(w, raw, sizeof(w));
        if (type == CrSdResponse136) {
            /* SDHCI stores R2 bits [127:8]; restore the CRC byte position. */
            uint32_t prev = 0;
            for (unsigned i = 0; i < 4; ++i) { resp[i] = w[i] << 8 | prev >> 24; prev = w[i]; }
        } else {
            resp[0] = w[0]; resp[1] = resp[2] = resp[3] = 0;
        }
        if (r1_type(index, type, app) && (resp[0] & R1_ERRORS)) {
            SD_LOG(h, "CMD%u status %08x", index, (unsigned)resp[0]);
            status = ERROR_DEVICE_ERROR;
        }
    }
    if (!status && data) {
        uint32_t want = data->write ? EV_WREADY : EV_RREADY;
        for (uint16_t b = 0; b < data->blocks && !status; ++b) {
            status = wait_events(h, want, &seen, 1000000);
            UINT16 left = 0;
            if (!status)
                status = map(CrSdhcPioBlock(&h->core, data->buf + (size_t)b * data->block_size,
                                            data->block_size, &left));
        }
        if (!status) status = wait_events(h, EV_XFER, &seen, 2000000);
        if (!status) h->core.DataActive = false;
    }
    if (status) {
        if (index != 8 && !(app && index == 41))
            SD_LOG(h, "%sCMD%u arg %08x failed (%d) events %04x errors %04x", app ? "A" : "", index,
                   (unsigned)arg, status, (unsigned)h->last_events, (unsigned)h->last_errors);
        recover(h);
    }
    return status;
}

int sdmmc_cmd(struct sdmmc_host *h, uint8_t index, uint32_t arg, CR_SDHC_RESPONSE type, uint32_t resp[4])
{
    return command(h, index, arg, type, false, NULL, resp);
}

static int app_command(struct sdmmc_host *h, uint8_t index, uint32_t arg, CR_SDHC_RESPONSE type,
                       uint32_t resp[4])
{
    uint32_t r[4];
    int status = command(h, 55, (uint32_t)h->rca << 16, CrSdResponse48, false, NULL, r);
    if (status) return status;
    if (!(r[0] & (1u << 5))) return ERROR_NOT_SUPPORTED; /* APP_CMD not accepted */
    return command(h, index, arg, type, true, NULL, resp);
}

static uint32_t bits(const uint32_t r[4], unsigned start, unsigned len)
{
    uint32_t v = 0;
    for (unsigned i = 0; i < len; ++i) {
        unsigned b = start + i;
        v |= ((r[b / 32] >> (b % 32)) & 1u) << i;
    }
    return v;
}

static int set_clock(struct sdmmc_host *h, uint32_t hz)
{
    int status = map(CrSdhcClock(&h->core, false));
    if (!status && h->ops && h->ops->set_clock) status = h->ops->set_clock(h, hz);
    if (!status && h->ops && h->ops->set_timing) status = h->ops->set_timing(h, hz);
    if (!status) status = map(CrSdhcClock(&h->core, true));
    if (!status) h->clock_hz = hz;
    return status;
}

static int sd_identify(struct sdmmc_host *h)
{
    uint32_t r[4];
    bool v2 = command(h, 8, 0x1aa, CrSdResponse48, false, NULL, r) == 0 && (r[0] & 0xfff) == 0x1aa;
    uint32_t arg = SD_OCR_VDD | (v2 ? OCR_HCS : 0);
    for (unsigned t = 0; t < 1000; ++t) {
        int status = app_command(h, 41, arg, CrSdResponse48NoCrc, r);
        if (status) return status;
        if (r[0] & OCR_BUSY) {
            h->ocr = r[0];
            h->high_capacity = v2 && (r[0] & OCR_HCS);
            h->type = SDMMC_SD;
            return 0;
        }
        stall(h, 1000);
    }
    return ERROR_TIMEOUT;
}

static int mmc_identify(struct sdmmc_host *h)
{
    uint32_t r[4];
    (void)command(h, 0, 0, CrSdResponseNone, false, NULL, r);
    for (unsigned t = 0; t < 1000; ++t) {
        int status = command(h, 1, SD_OCR_VDD | MMC_OCR_SECTOR, CrSdResponse48NoCrc, false, NULL, r);
        if (status) return status;
        if (r[0] & OCR_BUSY) {
            h->ocr = r[0];
            h->high_capacity = (r[0] & (3u << 29)) == MMC_OCR_SECTOR;
            h->type = SDMMC_EMMC;
            return 0;
        }
        stall(h, 1000);
    }
    return ERROR_TIMEOUT;
}

static uint64_t csd_blocks(const uint32_t csd[4])
{
    if (bits(csd, 126, 2) == 1) /* SD CSD 2.0 */
        return ((uint64_t)bits(csd, 48, 22) + 1) * 1024;
    uint32_t c_size = bits(csd, 62, 12), mult = bits(csd, 47, 3), bl_len = bits(csd, 80, 4);
    uint64_t bytes = ((uint64_t)c_size + 1) << (mult + 2) << bl_len;
    return bytes / BLOCK;
}

static int wait_ready(struct sdmmc_host *h)
{
    uint32_t r[4];
    for (unsigned t = 0; t < 5000; ++t) {
        int status = command(h, 13, (uint32_t)h->rca << 16, CrSdResponse48, false, NULL, r);
        if (status) return status;
        if ((r[0] & (1u << 8)) && ((r[0] >> 9) & 0xf) == 4) return 0; /* READY, tran */
        stall(h, 100);
    }
    return ERROR_TIMEOUT;
}

static int mmc_switch(struct sdmmc_host *h, uint8_t index, uint8_t value)
{
    uint32_t r[4];
    uint32_t arg = 3u << 24 | (uint32_t)index << 16 | (uint32_t)value << 8; /* write byte */
    int status = command(h, 6, arg, CrSdResponse48Busy, false, NULL, r);
    if (!status) status = wait_ready(h);
    if (!status) {
        status = command(h, 13, (uint32_t)h->rca << 16, CrSdResponse48, false, NULL, r);
        if (!status && (r[0] & (1u << 7))) status = ERROR_NOT_SUPPORTED; /* SWITCH_ERROR */
    }
    return status;
}

int sdmmc_init(struct sdmmc_host *h)
{
    if (!h) return ERROR_INVALID_PARAMETER;
    h->ready = false;
    h->type = SDMMC_NONE;
    h->rca = 0;
    int status = map(CrSdhcInit(&h->core, &h->io));
    if (status) return status;
    if (h->ops && h->ops->attach && (status = h->ops->attach(h))) return status;
    if (h->ops && h->ops->card_present) {
        bool present = true;
        if ((status = h->ops->card_present(h, &present))) return status;
        if (!present) { SD_LOG(h, "no card"); return ERROR_NOT_FOUND; }
    }
    /* RESET_ALL also retires any DMA left running by the previous owner. */
    if ((status = map(CrSdhcReset(&h->core, 1)))) { SD_LOG(h, "reset failed"); return status; }
    if ((status = map(CrSdhcClock(&h->core, false)))) return status;
    if (h->ops && h->ops->prepare && (status = h->ops->prepare(h))) return status;
    if ((status = map(CrSdhcWrite(&h->core, 0x2e, 1, 0x0e)))) return status;
    if ((status = map(CrSdhcClearEvents(&h->core, 0xffff)))) return status;
    if ((status = map(CrSdhcEvents(&h->core, EV_ENABLE, true)))) return status;
    if ((status = map(CrSdhcPowerRequest(&h->core, true)))) return status;
    if (h->ops && h->ops->power && (status = h->ops->power(h, true))) return status;
    if ((status = map(CrSdhcBusWidth(&h->core, 1)))) return status;
    h->bus_width = 1;
    if ((status = set_clock(h, 400000))) { SD_LOG(h, "400 kHz clock failed"); return status; }
    stall(h, 1000); /* >= 74 identification clocks */

    uint32_t r[4];
    (void)command(h, 0, 0, CrSdResponseNone, false, NULL, r);
    status = h->emmc_only ? ERROR_NOT_SUPPORTED : sd_identify(h);
    if (status && !h->sd_only) status = mmc_identify(h);
    if (status) { SD_LOG(h, "no SD or eMMC card answered (%d)", status); return ERROR_NOT_FOUND; }

    if ((status = command(h, 2, 0, CrSdResponse136, false, NULL, h->cid))) return status;
    if (h->type == SDMMC_SD) {
        if ((status = command(h, 3, 0, CrSdResponse48, false, NULL, r))) return status;
        if (r[0] & 0xe000u || !(r[0] >> 16)) return ERROR_DEVICE_ERROR;
        h->rca = (uint16_t)(r[0] >> 16);
    } else {
        h->rca = 1;
        if ((status = command(h, 3, (uint32_t)h->rca << 16, CrSdResponse48, false, NULL, r))) return status;
    }
    if ((status = command(h, 9, (uint32_t)h->rca << 16, CrSdResponse136, false, NULL, h->csd))) return status;
    if ((status = command(h, 7, (uint32_t)h->rca << 16, CrSdResponse48Busy, false, NULL, r))) return status;

    h->blocks = csd_blocks(h->csd);
    if (h->type == SDMMC_SD) {
        if (!h->high_capacity && (status = command(h, 16, BLOCK, CrSdResponse48, false, NULL, r)))
            return status;
        if (!app_command(h, 6, 2, CrSdResponse48, r) && !CrSdhcBusWidth(&h->core, 4)) h->bus_width = 4;
    } else {
        static uint8_t ext_csd[512] __attribute__((aligned(8)));
        struct sdmmc_data d = {ext_csd, 1, 512, false};
        if ((status = command(h, 8, 0, CrSdResponse48, false, &d, r))) return status;
        uint64_t sectors = (uint32_t)ext_csd[212] | (uint32_t)ext_csd[213] << 8 |
                           (uint32_t)ext_csd[214] << 16 | (uint32_t)ext_csd[215] << 24;
        if (h->high_capacity && sectors) h->blocks = sectors;
        if (!mmc_switch(h, 183, 1) && !CrSdhcBusWidth(&h->core, 4)) h->bus_width = 4; /* BUS_WIDTH */
    }
    uint32_t hz = h->max_hz && h->max_hz < 25000000 ? h->max_hz : 25000000;
    if ((status = set_clock(h, hz))) { SD_LOG(h, "%u Hz clock failed", (unsigned)hz); return status; }
    if (!h->blocks) return ERROR_DEVICE_ERROR;
    h->ready = true;
    SD_LOG(h, "%s %s, %llu blocks, %u-bit, %u kHz", h->type == SDMMC_SD ? "SD" : "eMMC",
           h->high_capacity ? "block-addressed" : "byte-addressed",
           (unsigned long long)h->blocks, h->bus_width, (unsigned)(hz / 1000));
    return 0;
}

static int transfer(struct sdmmc_host *h, uint64_t lba, uint32_t count, uint8_t *buf, bool write)
{
    if (!h->ready) return ERROR_DEVICE_ERROR;
    if (!count) return 0;
    if (!buf || lba >= h->blocks || count > h->blocks - lba) return ERROR_INVALID_PARAMETER;
    while (count) {
        uint16_t n = count > MAX_BLOCKS ? MAX_BLOCKS : (uint16_t)count;
        uint64_t addr = h->high_capacity ? lba : lba * BLOCK;
        if (addr > 0xffffffffu) return ERROR_INVALID_PARAMETER;
        struct sdmmc_data d = {buf, n, BLOCK, write};
        uint8_t index = write ? (n > 1 ? 25 : 24) : (n > 1 ? 18 : 17);
        uint32_t r[4];
        int status = command(h, index, (uint32_t)addr, CrSdResponse48, false, &d, r);
        if (!status && write) status = wait_ready(h);
        if (status) return status;
        buf += (size_t)n * BLOCK; lba += n; count -= n;
    }
    return 0;
}

int sdmmc_read(struct sdmmc_host *h, uint64_t lba, uint32_t count, void *buf)
{
    return transfer(h, lba, count, buf, false);
}

int sdmmc_write(struct sdmmc_host *h, uint64_t lba, uint32_t count, const void *buf)
{
    if (!h->allow_write) return ERROR_NOT_SUPPORTED;
    return transfer(h, lba, count, (uint8_t *)buf, true);
}

static int blk_read(struct kshim_blkdev *dev, uint64_t lba, uint32_t count, void *buf)
{
    return sdmmc_read(dev->priv, lba, count, buf);
}

static int blk_write(struct kshim_blkdev *dev, uint64_t lba, uint32_t count, const void *buf)
{
    return sdmmc_write(dev->priv, lba, count, buf);
}

static const struct kshim_blkdev_ops ro_ops = { .read = blk_read };
static const struct kshim_blkdev_ops rw_ops = { .read = blk_read, .write = blk_write };

int sdmmc_probe(struct sdmmc_host *h)
{
    int status = sdmmc_init(h);
    if (status) return status;
    memset(&h->dev, 0, sizeof(h->dev));
    snprintf(h->dev.name, sizeof(h->dev.name), "%s", h->name[0] ? h->name : "sdhc0");
    h->dev.kind = h->type == SDMMC_SD ? KSHIM_BLKDEV_SD : KSHIM_BLKDEV_EMMC;
    h->dev.block_size = BLOCK;
    h->dev.num_blocks = h->blocks;
    h->dev.max_transfer_blocks = MAX_BLOCKS;
    h->dev.read_only = !h->allow_write;
    h->dev.ops = h->allow_write ? &rw_ops : &ro_ops;
    h->dev.priv = h;
    return kshim_blkdev_register(&h->dev);
}

int sdmmc_quiesce(struct sdmmc_host *h)
{
    if (!h || !h->core.Initialized) return 0;
    h->ready = false;
    int status = map(CrSdhcEvents(&h->core, 0xffff, false));
    if (!status) status = map(CrSdhcClock(&h->core, false));
    if (!status) status = map(CrSdhcReset(&h->core, 6));
    return status;
}
