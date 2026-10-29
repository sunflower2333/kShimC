/* SPDX-License-Identifier: MIT */
/* SD/eMMC card layer over CrDK's SdhciMsmLib core, against an SDHCI register
 * model with an SD (v2 SDHC, v1 SDSC) or eMMC card behind it. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <kshim/sdmmc.h>
#include "blkdev_stub.h"

enum card_kind { SDHC, SDSC, EMMC };

static struct {
    uint8_t r8[0x300];
    enum card_kind kind;
    bool app, selected;
    unsigned acmd41, cmd1, state;
    uint16_t rca;
    uint8_t width;
    /* data phase */
    uint8_t buf[256 * 512];
    uint32_t pos, len, block;
    bool reading, writing;
    uint64_t start_block;
    uint8_t written[8 * 512];
    uint64_t written_lba;
    unsigned cmds[64];
    uint32_t last_arg[64];
    bool fail_crc_once;
    bool present;
} M;

#define BLOCKS_SDHC (16384u * 1024u)
#define BLOCKS_SDSC 8192u
#define BLOCKS_EMMC 30535680u

static uint8_t pattern(uint64_t lba, uint32_t off) { return (uint8_t)(lba * 13 + off * 5 + M.kind); }

static uint16_t rd16(unsigned o) { return (uint16_t)(M.r8[o] | M.r8[o + 1] << 8); }
static uint32_t rd32(unsigned o) { return rd16(o) | (uint32_t)rd16(o + 2) << 16; }
static void wr16(unsigned o, uint16_t v) { M.r8[o] = (uint8_t)v; M.r8[o + 1] = v >> 8; }
static void wr32(unsigned o, uint32_t v) { wr16(o, (uint16_t)v); wr16(o + 2, v >> 16); }

static void raise(uint16_t normal)
{
    wr16(0x30, rd16(0x30) | (normal & rd16(0x34)));
}

static void raise_error(uint16_t err)
{
    wr16(0x32, rd16(0x32) | (err & rd16(0x36)));
    if (rd16(0x32)) wr16(0x30, rd16(0x30) | 0x8000);
}

static void set_bits(uint32_t w[4], unsigned start, unsigned len, uint32_t v)
{
    for (unsigned i = 0; i < len; ++i) {
        unsigned b = start + i;
        if (v >> i & 1) w[b / 32] |= 1u << (b % 32);
    }
}

static void respond136(const uint32_t full[4])
{
    /* Registers hold response bits [127:8]. */
    for (unsigned i = 0; i < 4; ++i)
        wr32(0x10 + 4 * i, full[i] >> 8 | (i < 3 ? full[i + 1] << 24 : 0));
}

static uint64_t blocks(void) { return M.kind == SDHC ? BLOCKS_SDHC : M.kind == SDSC ? BLOCKS_SDSC : BLOCKS_EMMC; }

static uint32_t r1(void) { return (uint32_t)M.state << 9 | 1u << 8 | (M.app ? 1u << 5 : 0); }

static void start_data(bool read, uint64_t lba, uint32_t count)
{
    M.reading = read; M.writing = !read;
    M.pos = 0; M.block = 0;
    M.len = count * 512;
    M.start_block = lba;
    assert(M.len <= sizeof(M.buf));
    if (read) {
        for (uint32_t b = 0; b < count; ++b)
            for (uint32_t o = 0; o < 512; ++o) M.buf[b * 512 + o] = pattern(lba + b, o);
        wr32(0x24, rd32(0x24) | 1u << 11);
        raise(0x20);
    } else {
        wr32(0x24, rd32(0x24) | 1u << 10);
        raise(0x10);
    }
}

static void command(uint16_t cmdreg)
{
    unsigned idx = cmdreg >> 8;
    uint32_t arg = rd32(0x08);
    bool data = cmdreg & 0x20;
    bool app = M.app;
    M.app = false;
    M.cmds[idx]++;
    M.last_arg[idx] = arg;
    bool emmc = M.kind == EMMC;
    bool timeout = false;
    memset(&M.r8[0x10], 0, 16);
    switch (idx) {
    case 0: M.state = 0; M.rca = 0; M.selected = false; M.acmd41 = M.cmd1 = 0; break;
    case 1:
        if (!emmc) { timeout = true; break; }
        wr32(0x10, ++M.cmd1 >= 3 ? 0xc0ff8080u : 0x40ff8080u);
        if (M.cmd1 >= 3) M.state = 1;
        break;
    case 2: { uint32_t cid[4] = {0x11223300, 0x44556677, 0x8899aabb, 0x03534453}; respond136(cid); M.state = 2; break; }
    case 3:
        if (emmc) { M.rca = arg >> 16; wr32(0x10, r1()); }
        else { M.rca = 0x1234; wr32(0x10, (uint32_t)M.rca << 16 | 0x0500); }
        M.state = 3;
        break;
    case 6:
        if (app) { assert(arg == 2); M.width = 4; wr32(0x10, r1()); }
        else { assert(emmc && arg == (3u << 24 | 183u << 16 | 1u << 8)); M.width = 4; wr32(0x10, r1()); }
        break;
    case 7: assert(arg >> 16 == M.rca); M.selected = true; M.state = 4; wr32(0x10, r1()); break;
    case 8:
        if (emmc) {
            if (!data) { timeout = true; break; }
            wr32(0x10, r1());
            memset(M.buf, 0, 512);
            M.buf[212] = (uint8_t)BLOCKS_EMMC; M.buf[213] = (uint8_t)(BLOCKS_EMMC >> 8);
            M.buf[214] = (uint8_t)(BLOCKS_EMMC >> 16); M.buf[215] = (uint8_t)(BLOCKS_EMMC >> 24);
            M.reading = true; M.writing = false; M.pos = 0; M.block = 0; M.len = 512;
            wr32(0x24, rd32(0x24) | 1u << 11); raise(0x20);
            break;
        }
        if (M.kind == SDSC) { timeout = true; break; }
        wr32(0x10, arg & 0xfff);
        break;
    case 9: {
        uint32_t csd[4] = {0};
        if (M.kind == SDHC) { set_bits(csd, 126, 2, 1); set_bits(csd, 48, 22, BLOCKS_SDHC / 1024 - 1); }
        else if (M.kind == SDSC) { set_bits(csd, 80, 4, 9); set_bits(csd, 62, 12, 255); set_bits(csd, 47, 3, 3); }
        else { set_bits(csd, 126, 2, 3); set_bits(csd, 62, 12, 0xfff); set_bits(csd, 47, 3, 7); set_bits(csd, 80, 4, 9); }
        respond136(csd);
        break;
    }
    case 13: wr32(0x10, r1()); break;
    case 16: assert(arg == 512); wr32(0x10, r1()); break;
    case 17: case 18: case 24: case 25: {
        assert(M.selected && data);
        uint64_t lba = M.kind == SDSC ? arg / 512 : arg;
        if (M.kind == SDSC) assert(arg % 512 == 0);
        uint32_t count = rd16(0x06);
        if (idx == 17 || idx == 24) assert(count == 1);
        else assert(count > 1 && (rd16(0x0c) & 4)); /* auto CMD12 */
        assert(lba + count <= blocks());
        wr32(0x10, r1());
        if (M.fail_crc_once) { M.fail_crc_once = false; raise_error(1u << 5); return; } /* data CRC */
        start_data(idx < 20, lba, count);
        if (idx >= 24) M.written_lba = lba;
        break;
    }
    case 41:
        if (!app || emmc) { timeout = true; break; }
        if (M.kind == SDHC) assert(arg & (1u << 30));
        else assert(!(arg & (1u << 30)));
        wr32(0x10, ++M.acmd41 >= 3 ? (0x80ff8000u | (M.kind == SDHC ? 1u << 30 : 0)) : 0x00ff8000u);
        if (M.acmd41 >= 3) M.state = 1;
        break;
    case 55:
        if (emmc) { timeout = true; break; }
        M.app = true; wr32(0x10, r1());
        break;
    default:
        fprintf(stderr, "unexpected CMD%u\n", idx);
        assert(0);
    }
    if (timeout) { raise_error(1u << 0); return; }
    raise(0x01);
    if ((cmdreg & 3) == 3) raise(0x02);
}

static uint32_t data_read(unsigned width)
{
    assert(M.reading && (rd32(0x24) & (1u << 11)));
    uint32_t v = 0;
    for (unsigned i = 0; i < width; ++i) v |= (uint32_t)M.buf[M.pos++] << (8 * i);
    if (M.pos % 512 == 0) {
        wr32(0x24, rd32(0x24) & ~(1u << 11));
        if (M.pos == M.len) { M.reading = false; raise(0x02); }
        else { wr32(0x24, rd32(0x24) | 1u << 11); raise(0x20); }
    }
    return v;
}

static void data_write(unsigned width, uint32_t v)
{
    assert(M.writing && (rd32(0x24) & (1u << 10)));
    for (unsigned i = 0; i < width; ++i) M.buf[M.pos++] = (uint8_t)(v >> (8 * i));
    if (M.pos % 512 == 0) {
        wr32(0x24, rd32(0x24) & ~(1u << 10));
        if (M.pos == M.len) {
            M.writing = false;
            memcpy(M.written, M.buf, M.len < sizeof(M.written) ? M.len : sizeof(M.written));
            raise(0x02);
        } else { wr32(0x24, rd32(0x24) | 1u << 10); raise(0x10); }
    }
}

static CR_STATUS io_ready(void *c) { (void)c; return CR_SUCCESS; }
static CR_STATUS io_read(void *c, UINT32 o, UINT8 w, UINT32 *v)
{
    (void)c;
    assert(o + w <= sizeof(M.r8));
    if (o == 0x20) { *v = data_read(w); return CR_SUCCESS; }
    *v = w == 1 ? M.r8[o] : w == 2 ? rd16(o) : rd32(o);
    return CR_SUCCESS;
}
static CR_STATUS io_write(void *c, UINT32 o, UINT8 w, UINT32 v)
{
    (void)c;
    switch (o) {
    case 0x20: data_write(w, v); return CR_SUCCESS;
    case 0x0e: assert(w == 2); wr16(0x0e, (uint16_t)v); command((uint16_t)v); return CR_SUCCESS;
    case 0x2c:
        assert(w == 2);
        wr16(0x2c, (uint16_t)((v & 1) ? (v | 2) : 0));
        return CR_SUCCESS;
    case 0x2f:
        if (v & 1) { memset(M.r8, 0, 0x100); M.reading = M.writing = false; }
        if (v & 6) { M.reading = M.writing = false; wr32(0x24, 0); }
        M.r8[0x2f] = 0;
        return CR_SUCCESS;
    case 0x30:
        if (w == 4) { wr16(0x30, rd16(0x30) & ~(uint16_t)v); wr16(0x32, rd16(0x32) & ~(uint16_t)(v >> 16)); }
        else wr16(0x30, rd16(0x30) & ~(uint16_t)v);
        if (!rd16(0x32)) wr16(0x30, rd16(0x30) & 0x7fff);
        return CR_SUCCESS;
    }
    if (w == 1) M.r8[o] = (uint8_t)v; else if (w == 2) wr16(o, (uint16_t)v); else wr32(o, v);
    return CR_SUCCESS;
}
static CR_STATUS io_stall(void *c, UINT32 us) { (void)c; (void)us; return CR_SUCCESS; }
static CR_STATUS io_barrier(void *c) { (void)c; return CR_SUCCESS; }

static int present(struct sdmmc_host *h, bool *p) { (void)h; *p = M.present; return 0; }
static const struct sdmmc_platform_ops ops = { .card_present = present };

static void setup(struct sdmmc_host *h, enum card_kind kind)
{
    memset(&M, 0, sizeof(M));
    M.kind = kind;
    M.present = true;
    memset(h, 0, sizeof(*h));
    h->io = (CR_SDHC_IO){NULL, io_ready, io_read, io_write, io_stall, io_barrier, 0x1000, CR_SDHC_WINDOW_ALL};
    h->ops = &ops;
    snprintf(h->name, sizeof(h->name), "sdhc2");
    blkdev_stub_reset();
}

static void check_reads(struct sdmmc_host *h)
{
    struct kshim_blkdev *dev = kshim_blkdev_find("sdhc2");
    assert(dev && dev->block_size == 512 && dev->num_blocks == blocks() && dev->read_only);
    assert(!dev->ops->write);
    static uint8_t buf[300 * 512];
    /* Single block (CMD17) at an odd address. */
    assert(dev->ops->read(dev, 77, 1, buf + 1) == 0 && M.cmds[17] == 1);
    for (uint32_t o = 0; o < 512; ++o) assert(buf[1 + o] == pattern(77, o));
    /* Multi block (CMD18 + auto CMD12), split above 256 blocks. */
    assert(dev->ops->read(dev, 1000, 300, buf) == 0 && M.cmds[18] == 2);
    for (uint32_t b = 0; b < 300; ++b)
        for (uint32_t o = 0; o < 512; o += 97) assert(buf[b * 512 + o] == pattern(1000 + b, o));
    if (M.kind == SDSC) assert(M.last_arg[18] == (1000 + 256) * 512);
    else assert(M.last_arg[18] == 1000 + 256);
    /* Last block and bounds. */
    assert(dev->ops->read(dev, blocks() - 1, 1, buf) == 0);
    assert(dev->ops->read(dev, blocks() - 1, 2, buf) == ERROR_INVALID_PARAMETER);
    /* A data CRC error is reported and the lines are recovered. */
    M.fail_crc_once = true;
    assert(dev->ops->read(dev, 5, 1, buf) == ERROR_DEVICE_ERROR);
    assert(dev->ops->read(dev, 5, 1, buf) == 0 && buf[3] == pattern(5, 3));
    (void)h;
}

int main(void)
{
    struct sdmmc_host h;

    setup(&h, SDHC);
    assert(sdmmc_probe(&h) == 0);
    assert(h.type == SDMMC_SD && h.high_capacity && h.rca == 0x1234 && h.bus_width == 4);
    assert(M.width == 4 && M.cmds[16] == 0 && h.clock_hz == 25000000);
    assert(h.dev.kind == KSHIM_BLKDEV_SD);
    check_reads(&h);

    setup(&h, SDSC);
    assert(sdmmc_probe(&h) == 0);
    assert(h.type == SDMMC_SD && !h.high_capacity && M.cmds[16] == 1 && M.cmds[8] == 1);
    check_reads(&h);

    setup(&h, EMMC);
    assert(sdmmc_probe(&h) == 0);
    assert(h.type == SDMMC_EMMC && h.high_capacity && h.rca == 1 && h.bus_width == 4);
    assert(h.dev.kind == KSHIM_BLKDEV_EMMC && M.cmds[1] >= 3);
    check_reads(&h);

    /* No card: nothing is registered, no commands issued. */
    setup(&h, SDHC);
    M.present = false;
    assert(sdmmc_probe(&h) == ERROR_NOT_FOUND && blkdev_stub_count == 0 && M.cmds[0] == 0);

    /* Writes only when requested. */
    setup(&h, SDHC);
    h.allow_write = true;
    assert(sdmmc_probe(&h) == 0 && h.dev.ops->write && !h.dev.read_only);
    uint8_t out[2 * 512];
    for (unsigned i = 0; i < sizeof(out); ++i) out[i] = (uint8_t)(i * 3);
    assert(h.dev.ops->write(&h.dev, 42, 2, out) == 0 && M.cmds[25] == 1);
    assert(M.written_lba == 42 && !memcmp(M.written, out, sizeof(out)));
    assert(sdmmc_quiesce(&h) == 0 && !(rd16(0x2c) & 4));

    printf("sdmmc_test: PASS\n");
    return 0;
}
