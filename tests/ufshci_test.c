/* SPDX-License-Identifier: MIT */
/* UFSHCI core against a register + UPIU device model. DMA addresses are host
 * pointers; the model walks UTRD -> UCD -> PRDT exactly as hardware would. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <kshim/ufshci.h>
#include "blkdev_stub.h"

#define BASE 0x1d84000u

struct model {
    uint32_t reg[0x100 / 4];
    uint64_t now;
    unsigned hce_writes, linkstartups, doorbells, nops, queries;
    bool device_init_flag;
    unsigned device_init_polls;
    bool unit_attention[4];
    bool fail_ocs, medium_error, link_fails_once, dead;
    uint32_t last_cdb_op;
    uint64_t last_lba;
};
static struct model M;

#define R(off) M.reg[(off) / 4]

static const struct { bool enabled; uint32_t bs; uint64_t blocks; } LUS[4] = {
    {true, 4096, 1024}, {true, 512, (1ULL << 33) + 5}, {false, 0, 0}, {true, 4096, 16},
};

static uint8_t pattern(unsigned lun, uint64_t lba, uint32_t off)
{
    return (uint8_t)(lun * 31 + lba * 7 + off * 3 + (lba >> 32));
}

static uint32_t be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3]; }
static void wbe32(uint8_t *p, uint32_t v) { p[0] = v >> 24; p[1] = v >> 16; p[2] = v >> 8; p[3] = v; }
static void wbe16(uint8_t *p, uint16_t v) { p[0] = v >> 8; p[1] = (uint8_t)v; }
static uint32_t le32(const uint8_t *p) { uint32_t v; memcpy(&v, p, 4); return v; }
static uint8_t *ptr(uint32_t lo, uint32_t hi) { return (uint8_t *)(uintptr_t)((uint64_t)hi << 32 | lo); }

/* Scatter/gather the device data stream through the PRDT. */
static void prdt_xfer(uint8_t *ucd, uint32_t prdt_off, uint16_t entries, uint8_t *stream,
                      uint32_t len, bool to_host)
{
    uint32_t done = 0;
    for (uint16_t i = 0; i < entries && done < len; ++i) {
        uint8_t *e = ucd + prdt_off + 16u * i;
        uint8_t *buf = ptr(le32(e), le32(e + 4));
        uint32_t n = (le32(e + 12) & 0x3ffff) + 1;
        assert(!((uintptr_t)buf & 3) && !(n & 3));
        if (n > len - done) n = len - done;
        if (to_host) memcpy(buf, stream + done, n); else memcpy(stream + done, buf, n);
        done += n;
    }
    assert(done == len);
}

static void scsi(uint8_t *cmd, uint8_t *rsp, uint8_t *ucd, uint32_t prdt_off, uint16_t entries)
{
    unsigned lun = cmd[2];
    const uint8_t *cdb = cmd + 16;
    uint32_t edtl = be32(cmd + 12);
    rsp[0] = 0x21; rsp[3] = cmd[3];
    M.last_cdb_op = cdb[0];
    assert(lun < 4 && LUS[lun].enabled);
    if (M.unit_attention[lun]) {
        M.unit_attention[lun] = false;
        rsp[7] = 2; wbe16(rsp + 10, 20); wbe16(rsp + 32, 18);
        rsp[34] = 0x70; rsp[36] = 6; rsp[46] = 0x29;
        return;
    }
    static uint8_t stream[1 << 20];
    uint32_t bs = LUS[lun].bs;
    switch (cdb[0]) {
    case 0x00: break;
    case 0x25: {
        uint64_t last = LUS[lun].blocks - 1;
        wbe32(stream, last > 0xffffffffu ? 0xffffffffu : (uint32_t)last);
        wbe32(stream + 4, bs);
        assert(edtl == 8);
        prdt_xfer(ucd, prdt_off, entries, stream, 8, true);
        break;
    }
    case 0x9e: {
        uint64_t last = LUS[lun].blocks - 1;
        memset(stream, 0, 32);
        wbe32(stream, (uint32_t)(last >> 32)); wbe32(stream + 4, (uint32_t)last);
        wbe32(stream + 8, bs);
        prdt_xfer(ucd, prdt_off, entries, stream, edtl, true);
        break;
    }
    case 0x28: case 0x88: {
        uint64_t lba; uint32_t n;
        if (cdb[0] == 0x28) { lba = be32(cdb + 2); n = (uint32_t)cdb[7] << 8 | cdb[8]; }
        else { lba = (uint64_t)be32(cdb + 2) << 32 | be32(cdb + 6); n = be32(cdb + 10); }
        M.last_lba = lba;
        assert(edtl == n * bs && edtl <= sizeof(stream));
        assert(cmd[1] & 0x40);
        if (M.medium_error) {
            rsp[7] = 2; wbe16(rsp + 10, 20); wbe16(rsp + 32, 18);
            rsp[34] = 0x70; rsp[36] = 3; rsp[46] = 0x11;
            return;
        }
        for (uint32_t b = 0; b < n; ++b)
            for (uint32_t o = 0; o < bs; ++o) stream[b * bs + o] = pattern(lun, lba + b, o);
        prdt_xfer(ucd, prdt_off, entries, stream, edtl, true);
        break;
    }
    default:
        assert(!"unexpected CDB");
    }
}

static void query(uint8_t *cmd, uint8_t *rsp)
{
    ++M.queries;
    rsp[0] = 0x36; rsp[3] = cmd[3]; rsp[5] = cmd[5];
    memcpy(rsp + 12, cmd + 12, 16);
    uint8_t op = cmd[12], idn = cmd[13], index = cmd[14];
    if (op == 0x01) {
        uint16_t want = (uint16_t)(cmd[18] << 8 | cmd[19]);
        uint8_t d[64] = {0};
        uint16_t n;
        if (idn == 0x00) { n = 0x40; d[0] = n; d[1] = 0; d[6] = 3; }
        else if (idn == 0x02) {
            if (index >= 8) { rsp[6] = 0xf7; return; } /* invalid index */
            n = 0x2d; d[0] = n; d[1] = 2; d[2] = index;
            d[3] = index < 4 && LUS[index].enabled;
        } else { rsp[6] = 0xf6; return; }
        if (n > want) n = want;
        memcpy(rsp + 32, d, n);
        wbe16(rsp + 10, n); wbe16(rsp + 18, n);
    } else if (op == 0x05 || op == 0x06) {
        assert(idn == 0x01);
        if (op == 0x06) { M.device_init_flag = true; M.device_init_polls = 0; }
        else if (M.device_init_flag && ++M.device_init_polls >= 3) M.device_init_flag = false;
        rsp[23] = op == 0x06 ? 1 : M.device_init_flag;
    } else assert(!"query op");
}

static void doorbell(void)
{
    ++M.doorbells;
    uint8_t *utrd = ptr(R(UFSHCI_UTRLBA), R(UFSHCI_UTRLBAU));
    assert((le32(utrd) >> 28) == 1);
    assert((le32(utrd + 8) & 0xff) == 0x0f);
    uint8_t *ucd = ptr(le32(utrd + 16), le32(utrd + 20));
    uint32_t rsp_off = (le32(utrd + 24) >> 16) * 4, prdt_off = (le32(utrd + 28) >> 16) * 4;
    uint16_t entries = le32(utrd + 28) & 0xffff;
    uint8_t *cmd = ucd, *rsp = ucd + rsp_off;
    memset(rsp, 0, (le32(utrd + 24) & 0xffff) * 4);
    uint32_t dd = (le32(utrd) >> 25) & 3;
    switch (cmd[0]) {
    case 0x00: ++M.nops; rsp[0] = 0x20; rsp[3] = cmd[3]; assert(dd == 0); break;
    case 0x16: query(cmd, rsp); break;
    case 0x01: scsi(cmd, rsp, ucd, prdt_off, entries); break;
    default: assert(!"UPIU type");
    }
    uint32_t ocs = M.fail_ocs && cmd[0] == 0x01 ? 0x02 : 0;
    memcpy(utrd + 8, &ocs, 4);
    R(UFSHCI_IS) |= UFSHCI_IS_UTRCS;
}

static void uic(uint32_t cmd)
{
    uint32_t attr = R(UFSHCI_UCMDARG1) >> 16;
    R(UFSHCI_UCMDARG2) = 0;
    if (cmd == UIC_DME_LINKSTARTUP) {
        ++M.linkstartups;
        if (M.link_fails_once) { M.link_fails_once = false; R(UFSHCI_UCMDARG2) = 1; }
        else R(UFSHCI_HCS) |= UFSHCI_HCS_DP | UFSHCI_HCS_UTRLRDY | UFSHCI_HCS_UTMRLRDY;
    } else if (cmd == UIC_DME_GET) {
        if (attr == UNIPRO_PA_CONNECTED_TX_LANES)
            R(UFSHCI_UCMDARG3) = (R(UFSHCI_HCS) & UFSHCI_HCS_DP) ? 2 : 0; /* stale even if dead */
        else R(UFSHCI_UCMDARG3) = 0;
    }
    R(UFSHCI_IS) |= UFSHCI_IS_UCCS;
}

static uint32_t mread(void *c, uintptr_t a) { (void)c; assert(a >= BASE && a < BASE + 0x100); return M.reg[(a - BASE) / 4]; }
static void mwrite(void *c, uintptr_t a, uint32_t v)
{
    (void)c;
    uint32_t off = (uint32_t)(a - BASE);
    switch (off) {
    case UFSHCI_IS: R(off) &= ~v; return;
    case UFSHCI_HCE:
        ++M.hce_writes;
        R(off) = v & 1;
        R(UFSHCI_HCS) = v & 1 ? UFSHCI_HCS_UCRDY : 0;
        if (!(v & 1)) { R(UFSHCI_UTRLRSR) = R(UFSHCI_UTMRLRSR) = R(UFSHCI_UTRLDBR) = 0; M.dead = false; }
        return;
    case UFSHCI_UICCMD: uic(v); return;
    case UFSHCI_UTRLDBR:
        assert(R(UFSHCI_UTRLRSR) & 1);
        if (M.dead) { R(off) |= v; return; } /* link down: never completes */
        if (v & 1) doorbell();
        return;
    case UFSHCI_UTRLCLR: R(UFSHCI_UTRLDBR) &= v; return;
    default: R(off) = v;
    }
}
static uint64_t mnow(void *c) { (void)c; return M.now++; }
static void mdelay(void *c, uint32_t us) { (void)c; M.now += us; }

static void reset_model(void)
{
    memset(&M, 0, sizeof(M));
    R(UFSHCI_CAP) = UFSHCI_CAP_64AS | (7u << 16) | 31;
    R(UFSHCI_VER) = 0x300;
    for (unsigned i = 0; i < 4; ++i) M.unit_attention[i] = true;
    blkdev_stub_reset();
}

static void make_live(void)
{
    R(UFSHCI_HCE) = 1;
    R(UFSHCI_HCS) = UFSHCI_HCS_DP | UFSHCI_HCS_UTRLRDY | UFSHCI_HCS_UTMRLRDY | UFSHCI_HCS_UCRDY;
    R(UFSHCI_UTRLRSR) = R(UFSHCI_UTMRLRSR) = 1;
    R(UFSHCI_UTRLBA) = 0xdead0000; /* previous owner's list */
    R(UFSHCI_IE) = 0x30c7f;
}

static void host_init(struct ufshci_host *h, enum ufshci_policy policy)
{
    memset(h, 0, sizeof(*h));
    h->base = BASE;
    h->io = (struct CrIo){NULL, mread, mwrite, mnow, mdelay};
    h->name = "ufs0";
    h->policy = policy;
}

static void check_reads(void)
{
    struct kshim_blkdev *lu0 = kshim_blkdev_find("ufs0:lu0");
    struct kshim_blkdev *lu1 = kshim_blkdev_find("ufs0:lu1");
    assert(lu0 && lu1 && !kshim_blkdev_find("ufs0:lu2") && kshim_blkdev_find("ufs0:lu3"));
    assert(lu0->kind == KSHIM_BLKDEV_UFS_LU && lu0->block_size == 4096 && lu0->num_blocks == 1024);
    assert(lu1->block_size == 512 && lu1->num_blocks == (1ULL << 33) + 5);
    assert(lu0->read_only && !lu0->ops->write);

    /* Aligned buffer: direct DMA, multi-block READ(10). */
    static uint8_t aligned[8 * 4096] __attribute__((aligned(64)));
    assert(lu0->ops->read(lu0, 100, 8, aligned) == 0);
    assert(M.last_cdb_op == 0x28 && M.last_lba == 100);
    for (uint32_t b = 0; b < 8; ++b)
        for (uint32_t o = 0; o < 4096; ++o) assert(aligned[b * 4096 + o] == pattern(0, 100 + b, o));

    /* Misaligned buffer: bounce path, split into 64 KiB requests. */
    uint8_t *raw = malloc(40 * 4096 + 3), *odd = raw + 3;
    assert(lu0->ops->read(lu0, 0, 40, odd) == 0);
    for (uint32_t b = 0; b < 40; ++b)
        for (uint32_t o = 0; o < 4096; o += 511) assert(odd[b * 4096 + o] == pattern(0, b, o));
    free(raw);

    /* LBA above 2^32 forces READ(16). */
    uint8_t blk[512] __attribute__((aligned(64)));
    uint64_t high = (1ULL << 33) + 1;
    assert(lu1->ops->read(lu1, high, 1, blk) == 0);
    assert(M.last_cdb_op == 0x88 && M.last_lba == high);
    for (uint32_t o = 0; o < 512; ++o) assert(blk[o] == pattern(1, high, o));

    /* Bounds. */
    assert(lu0->ops->read(lu0, 1020, 8, aligned) == ERROR_INVALID_PARAMETER);
}

int main(void)
{
    struct ufshci_host h;

    /* 1. Cold controller: full init, link startup retry, fDeviceInit, scan. */
    reset_model();
    M.link_fails_once = true;
    host_init(&h, UFSHCI_PREFER_INHERIT);
    assert(ufshci_probe(&h) == 0);
    assert(h.path == UFSHCI_PATH_FULL && h.inherit_reject);
    assert(M.linkstartups == 2 && M.hce_writes == 1 && !M.device_init_flag);
    assert(R(UFSHCI_IE) == 0 && h.lu_count == 3 && blkdev_stub_count == 3);
    check_reads();

    /* 2. Live controller left by the previous loader: take over, no reset. */
    reset_model();
    make_live();
    host_init(&h, UFSHCI_PREFER_INHERIT);
    assert(ufshci_probe(&h) == 0);
    assert(h.path == UFSHCI_PATH_INHERITED && !h.inherit_reject);
    assert(M.hce_writes == 0 && M.linkstartups == 0);
    assert(R(UFSHCI_UTRLBA) != 0xdead0000 && R(UFSHCI_IE) == 0);
    check_reads();

    /* 3. Live but busy (outstanding doorbell): rejected, full init. */
    reset_model();
    make_live();
    R(UFSHCI_UTRLDBR) = 4;
    host_init(&h, UFSHCI_PREFER_INHERIT);
    assert(ufshci_init(&h) == 0 && h.path == UFSHCI_PATH_FULL);
    assert(!strcmp(h.inherit_reject, "requests outstanding") && M.hce_writes == 2);

    /* 4. Inherit-only on a dead controller never resets it. */
    reset_model();
    host_init(&h, UFSHCI_INHERIT_ONLY);
    assert(ufshci_init(&h) == ERROR_NOT_SUPPORTED && M.hce_writes == 0);

    /* 5. Forced full init on a live controller resets it. */
    reset_model();
    make_live();
    host_init(&h, UFSHCI_FORCE_FULL);
    assert(ufshci_init(&h) == 0 && h.path == UFSHCI_PATH_FULL && M.hce_writes == 2);

    /* 6. Error reporting: OCS failure and CHECK CONDITION medium error. */
    reset_model();
    host_init(&h, UFSHCI_PREFER_INHERIT);
    assert(ufshci_probe(&h) == 0);
    struct kshim_blkdev *lu0 = kshim_blkdev_find("ufs0:lu0");
    static uint8_t buf[4096] __attribute__((aligned(64)));
    M.fail_ocs = true;
    assert(lu0->ops->read(lu0, 0, 1, buf) == ERROR_DEVICE_ERROR);
    M.fail_ocs = false;
    M.medium_error = true;
    assert(lu0->ops->read(lu0, 0, 1, buf) == ERROR_DEVICE_ERROR && h.last_sense_key == 3);
    M.medium_error = false;
    assert(lu0->ops->read(lu0, 0, 1, buf) == 0);

    /* 7. Writes are only exposed on request. */
    reset_model();
    host_init(&h, UFSHCI_PREFER_INHERIT);
    h.allow_write = true;
    assert(ufshci_probe(&h) == 0);
    lu0 = kshim_blkdev_find("ufs0:lu0");
    assert(!lu0->read_only && lu0->ops->write && lu0->ops->flush);

    /* 8. Quiesce stops the lists without disabling the controller. */
    assert(ufshci_quiesce(&h) == 0);
    assert(R(UFSHCI_UTRLRSR) == 0 && R(UFSHCI_HCE) == 1);

    /* 9. Registers look live but the link is dead: NOP OUT never completes,
     *    takeover is rejected and the controller is reinitialized. */
    reset_model();
    make_live();
    M.dead = true;
    host_init(&h, UFSHCI_PREFER_INHERIT);
    assert(ufshci_init(&h) == 0 && h.path == UFSHCI_PATH_FULL && M.hce_writes == 2);
    assert(!strcmp(h.inherit_reject, "takeover NOP failed") && M.linkstartups == 1);

    printf("ufshci_test: PASS\n");
    return 0;
}
