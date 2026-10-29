/* SPDX-License-Identifier: MIT */
/*
 * Polled UFSHCI host core and UPIU/SCSI engine for kShimC.
 *
 * Written natively from the JEDEC UFSHCI register interface (JESD223) and the
 * UFS UPIU/descriptor layouts (JESD220); SCSI CDBs follow SBC/SPC. No code
 * from Linux or from CrDK's UfsHcCrDxe is used. Only transfer request slot 0
 * is used, interrupts stay disabled and every wait is bounded.
 */
#include <kshim/ufshci.h>

#include <stdio.h>
#include <string.h>

#ifndef UFSHCI_POOL_HOSTS
#define UFSHCI_POOL_HOSTS 1
#endif

#define UFS_LOG(host, fmt, ...) printf("%s: " fmt "\n", (host)->name ? (host)->name : "ufs", ##__VA_ARGS__)

/* UTP transfer request descriptor (UTRD), 32 bytes, little endian. */
#define UTRD_CT_UFS        (1u << 28)
#define UTRD_DD_WRITE      (1u << 25)   /* system memory -> device */
#define UTRD_DD_READ       (2u << 25)   /* device -> system memory */
#define UTRD_OCS_INVALID   0x0f

/* UCD layout inside the 2 KiB command descriptor (64 PRDT entries). */
#define UCD_CMD_OFFSET     0
#define UCD_RSP_OFFSET     512
#define UCD_RSP_SIZE       512
#define UCD_PRDT_OFFSET    1024
#define UCD_SIZE           2048
#define UCD_PRDT_MAX       ((UCD_SIZE - UCD_PRDT_OFFSET) / 16)
#define PRDT_MAX_BYTES     (256u * 1024u)

/* UPIU transaction codes. */
#define UPIU_NOP_OUT       0x00
#define UPIU_COMMAND       0x01
#define UPIU_QUERY_REQ     0x16
#define UPIU_NOP_IN        0x20
#define UPIU_RESPONSE      0x21
#define UPIU_QUERY_RSP     0x36
#define UPIU_FLAG_READ     (1u << 6)
#define UPIU_FLAG_WRITE    (1u << 5)

#define QUERY_FN_READ      0x01
#define QUERY_FN_WRITE     0x81
#define QUERY_OP_READ_DESC 0x01
#define QUERY_OP_READ_FLAG 0x05
#define QUERY_OP_SET_FLAG  0x06

#define DESC_DEVICE        0x00
#define DESC_UNIT          0x02
#define DESC_GEOMETRY      0x07
#define FLAG_DEVICE_INIT   0x01

#define SCSI_GOOD          0x00
#define SCSI_CHECK         0x02

#define TIMEOUT_REG_US     100000u
#define TIMEOUT_UIC_US     500000u
#define TIMEOUT_NOP_US     50000u
#define TIMEOUT_QUERY_US   1500000u
#define TIMEOUT_SCSI_US    5000000u
#define TIMEOUT_INIT_US    5000000u

struct ufshci_pool {
    uint8_t utrl[1024];
    uint8_t utmrl[1024];
    uint8_t ucd[UCD_SIZE];
    uint8_t bounce[UFSHCI_BOUNCE_SIZE];
} __attribute__((aligned(4096)));

static struct ufshci_pool pool[UFSHCI_POOL_HOSTS];
static uintptr_t pool_owner[UFSHCI_POOL_HOSTS];

static inline void put_be16(uint8_t *p, uint16_t v) { p[0] = v >> 8; p[1] = (uint8_t)v; }
static inline void put_be32(uint8_t *p, uint32_t v)
{ p[0] = v >> 24; p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v; }
static inline uint16_t get_be16(const uint8_t *p) { return (uint16_t)(p[0] << 8 | p[1]); }
static inline uint32_t get_be32(const uint8_t *p)
{ return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }
static inline uint64_t get_be64(const uint8_t *p)
{ return (uint64_t)get_be32(p) << 32 | get_be32(p + 4); }
static inline void put_le32(uint8_t *p, uint32_t v) { memcpy(p, &v, 4); }
static inline uint32_t get_le32(const uint8_t *p) { uint32_t v; memcpy(&v, p, 4); return v; }

uint32_t ufshci_read(struct ufshci_host *h, uint32_t off)
{
    return h->io.read32(h->io.context, h->base + off);
}

void ufshci_write(struct ufshci_host *h, uint32_t off, uint32_t value)
{
    h->io.write32(h->io.context, h->base + off, value);
}

static uint64_t dma_addr(struct ufshci_host *h, const void *va)
{
    return h->dma.to_dma ? h->dma.to_dma(h->dma.context, va) : (uint64_t)(uintptr_t)va;
}

static void dma_clean(struct ufshci_host *h, const void *va, size_t len)
{
    if (h->dma.clean && len) h->dma.clean(h->dma.context, va, len);
}

static void dma_invalidate(struct ufshci_host *h, void *va, size_t len)
{
    if (h->dma.invalidate && len) h->dma.invalidate(h->dma.context, va, len);
}

static int wait_reg(struct ufshci_host *h, uint32_t off, uint32_t mask, uint32_t want,
                    uint32_t timeout_us)
{
    uint64_t start = h->io.now_us(h->io.context);
    for (;;) {
        if ((ufshci_read(h, off) & mask) == want) return 0;
        if (h->io.now_us(h->io.context) - start >= timeout_us) break;
        h->io.delay_us(h->io.context, 1);
    }
    return (ufshci_read(h, off) & mask) == want ? 0 : ERROR_TIMEOUT;
}

const char *ufshci_path_name(enum ufshci_init_path path)
{
    switch (path) {
    case UFSHCI_PATH_INHERITED: return "inherited";
    case UFSHCI_PATH_FULL: return "full-init";
    default: return "none";
    }
}

int ufshci_uic(struct ufshci_host *h, uint32_t cmd, uint16_t attr, uint16_t sel, uint32_t *value)
{
    if (wait_reg(h, UFSHCI_HCS, UFSHCI_HCS_UCRDY, UFSHCI_HCS_UCRDY, TIMEOUT_UIC_US))
        return ERROR_TIMEOUT;
    ufshci_write(h, UFSHCI_IS, UFSHCI_IS_UCCS);
    ufshci_write(h, UFSHCI_UCMDARG1, (uint32_t)attr << 16 | sel);
    ufshci_write(h, UFSHCI_UCMDARG2, 0);
    ufshci_write(h, UFSHCI_UCMDARG3, value && (cmd == UIC_DME_SET) ? *value : 0);
    ufshci_write(h, UFSHCI_UICCMD, cmd);
    int status = wait_reg(h, UFSHCI_IS, UFSHCI_IS_UCCS, UFSHCI_IS_UCCS, TIMEOUT_UIC_US);
    uint32_t result = ufshci_read(h, UFSHCI_UCMDARG2) & 0xff;
    if (!status && value && cmd != UIC_DME_SET) *value = ufshci_read(h, UFSHCI_UCMDARG3);
    ufshci_write(h, UFSHCI_IS, UFSHCI_IS_UCCS);
    if (status) return status;
    return result ? ERROR_DEVICE_ERROR : 0;
}

/* --- memory ------------------------------------------------------------- */

static int attach_memory(struct ufshci_host *h)
{
    unsigned free_slot = UFSHCI_POOL_HOSTS;
    for (unsigned i = 0; i < UFSHCI_POOL_HOSTS; ++i) {
        if (pool_owner[i] == h->base) { free_slot = i; break; }
        if (!pool_owner[i] && free_slot == UFSHCI_POOL_HOSTS) free_slot = i;
    }
    if (free_slot == UFSHCI_POOL_HOSTS) return ERROR_NOT_SUPPORTED;
    struct ufshci_pool *p = &pool[free_slot];
    pool_owner[free_slot] = h->base;
    h->utrl = p->utrl; h->utmrl = p->utmrl; h->ucd = p->ucd; h->bounce = p->bounce;
    memset(p->utrl, 0, sizeof(p->utrl));
    memset(p->utmrl, 0, sizeof(p->utmrl));
    memset(p->ucd, 0, sizeof(p->ucd));
    dma_clean(h, p->utrl, sizeof(p->utrl));
    dma_clean(h, p->utmrl, sizeof(p->utmrl));
    dma_clean(h, p->ucd, sizeof(p->ucd));
    uint64_t a = dma_addr(h, p->utrl), b = dma_addr(h, p->utmrl), c = dma_addr(h, p->ucd);
    if (!(h->cap & UFSHCI_CAP_64AS) &&
        ((a >> 32) || (b >> 32) || ((c + UCD_SIZE) >> 32) ||
         ((dma_addr(h, p->bounce) + UFSHCI_BOUNCE_SIZE) >> 32))) {
        UFS_LOG(h, "descriptor memory above 4 GiB without 64-bit addressing");
        return ERROR_NOT_SUPPORTED;
    }
    return 0;
}

static int start_lists(struct ufshci_host *h)
{
    uint64_t utrl = dma_addr(h, h->utrl), utmrl = dma_addr(h, h->utmrl);
    if (wait_reg(h, UFSHCI_HCS, UFSHCI_HCS_UTRLRDY | UFSHCI_HCS_UTMRLRDY,
                 UFSHCI_HCS_UTRLRDY | UFSHCI_HCS_UTMRLRDY, TIMEOUT_REG_US)) {
        UFS_LOG(h, "transfer lists not ready HCS=%08x", (unsigned)ufshci_read(h, UFSHCI_HCS));
        return ERROR_DEVICE_ERROR;
    }
    ufshci_write(h, UFSHCI_UTRLBA, (uint32_t)utrl);
    ufshci_write(h, UFSHCI_UTRLBAU, (uint32_t)(utrl >> 32));
    ufshci_write(h, UFSHCI_UTMRLBA, (uint32_t)utmrl);
    ufshci_write(h, UFSHCI_UTMRLBAU, (uint32_t)(utmrl >> 32));
    ufshci_write(h, UFSHCI_UTRLRSR, 1);
    ufshci_write(h, UFSHCI_UTMRLRSR, 1);
    /* Only transfer requests are issued; a host without a task management
     * list (QEMU 8.2) is acceptable as long as the transfer list runs. */
    if (!(ufshci_read(h, UFSHCI_UTMRLRSR) & 1))
        UFS_LOG(h, "task management list not running (unused)");
    if (!(ufshci_read(h, UFSHCI_UTRLRSR) & 1) || ufshci_read(h, UFSHCI_UTRLBA) != (uint32_t)utrl) {
        UFS_LOG(h, "transfer list run-stop did not latch (RSR %x/%x BA %08x)",
                (unsigned)ufshci_read(h, UFSHCI_UTRLRSR), (unsigned)ufshci_read(h, UFSHCI_UTMRLRSR),
                (unsigned)ufshci_read(h, UFSHCI_UTRLBA));
        return ERROR_DEVICE_ERROR;
    }
    return 0;
}

static int stop_lists(struct ufshci_host *h)
{
    ufshci_write(h, UFSHCI_UTRLRSR, 0);
    ufshci_write(h, UFSHCI_UTMRLRSR, 0);
    if (wait_reg(h, UFSHCI_UTRLRSR, 1, 0, TIMEOUT_REG_US) ||
        wait_reg(h, UFSHCI_UTMRLRSR, 1, 0, TIMEOUT_REG_US))
        return ERROR_TIMEOUT;
    return 0;
}

/* --- request engine ------------------------------------------------------ */

struct xfer {
    uint8_t dd;            /* 0 none, 1 write, 2 read */
    void *data;            /* DMA-visible buffer, or NULL */
    uint32_t len;
    uint32_t timeout_us;
};

static int build_prdt(struct ufshci_host *h, const struct xfer *x, uint16_t *entries)
{
    *entries = 0;
    if (!x->len) return 0;
    if (x->len & 3 || ((uintptr_t)x->data & 3)) return ERROR_INVALID_PARAMETER;
    uint8_t *prdt = h->ucd + UCD_PRDT_OFFSET;
    uint32_t left = x->len;
    uint64_t addr = dma_addr(h, x->data);
    uint16_t n = 0;
    while (left) {
        if (n == UCD_PRDT_MAX) return ERROR_INVALID_PARAMETER;
        uint32_t chunk = left > PRDT_MAX_BYTES ? PRDT_MAX_BYTES : left;
        uint8_t *e = prdt + 16u * n++;
        put_le32(e, (uint32_t)addr);
        put_le32(e + 4, (uint32_t)(addr >> 32));
        put_le32(e + 8, 0);
        put_le32(e + 12, chunk - 1);
        addr += chunk; left -= chunk;
    }
    *entries = n;
    return 0;
}

/* Issue the UPIU already written at UCD_CMD_OFFSET on slot 0 and wait. */
static int issue(struct ufshci_host *h, const struct xfer *x)
{
    uint8_t *utrd = h->utrl;
    uint16_t prdt_entries;
    int status = build_prdt(h, x, &prdt_entries);
    if (status) return status;
    if (ufshci_read(h, UFSHCI_UTRLDBR) & 1) return ERROR_DEVICE_ERROR;

    uint64_t ucd = dma_addr(h, h->ucd);
    uint32_t dw0 = UTRD_CT_UFS | (x->dd == 1 ? UTRD_DD_WRITE : x->dd == 2 ? UTRD_DD_READ : 0);
    memset(utrd, 0, 32);
    put_le32(utrd + 0, dw0);
    put_le32(utrd + 8, UTRD_OCS_INVALID);
    put_le32(utrd + 16, (uint32_t)ucd);
    put_le32(utrd + 20, (uint32_t)(ucd >> 32));
    put_le32(utrd + 24, (uint32_t)(UCD_RSP_OFFSET / 4) << 16 | (UCD_RSP_SIZE / 4));
    put_le32(utrd + 28, (uint32_t)(UCD_PRDT_OFFSET / 4) << 16 | prdt_entries);
    memset(h->ucd + UCD_RSP_OFFSET, 0, UCD_RSP_SIZE);

    dma_clean(h, utrd, 32);
    dma_clean(h, h->ucd, UCD_PRDT_OFFSET + 16u * prdt_entries);
    if (x->len) {
        if (x->dd == 1) dma_clean(h, x->data, x->len);
        else dma_invalidate(h, x->data, x->len); /* drop dirty lines before DMA */
    }

    ufshci_write(h, UFSHCI_IS, UFSHCI_IS_UTRCS);
    ufshci_write(h, UFSHCI_UTRLDBR, 1);
    status = wait_reg(h, UFSHCI_UTRLDBR, 1, 0, x->timeout_us);
    uint32_t is = ufshci_read(h, UFSHCI_IS);
    ufshci_write(h, UFSHCI_IS, UFSHCI_IS_UTRCS);
    if (status) {
        /* Abort the slot so it can be reused; UTRLCLR uses 0 to clear. */
        ufshci_write(h, UFSHCI_UTRLCLR, ~1u);
        (void)wait_reg(h, UFSHCI_UTRLDBR, 1, 0, TIMEOUT_REG_US);
        UFS_LOG(h, "request timeout IS=%08x", (unsigned)is);
        return ERROR_TIMEOUT;
    }
    if (is & UFSHCI_IS_FATAL) {
        UFS_LOG(h, "fatal error IS=%08x", (unsigned)is);
        h->ready = false;
        return ERROR_DEVICE_ERROR;
    }
    dma_invalidate(h, utrd, 32);
    dma_invalidate(h, h->ucd + UCD_RSP_OFFSET, UCD_RSP_SIZE);
    if (x->len && x->dd == 2) dma_invalidate(h, x->data, x->len);
    uint32_t ocs = get_le32(utrd + 8) & 0xff;
    if (ocs) {
        if (!h->quiet) UFS_LOG(h, "OCS %02x for UPIU %02x", (unsigned)ocs, h->ucd[UCD_CMD_OFFSET]);
        return ERROR_DEVICE_ERROR;
    }
    return 0;
}

static uint8_t *new_upiu(struct ufshci_host *h, uint8_t type, uint8_t flags, uint8_t lun)
{
    uint8_t *u = h->ucd + UCD_CMD_OFFSET;
    memset(u, 0, UCD_RSP_OFFSET);
    u[0] = type; u[1] = flags; u[2] = lun; u[3] = h->task_tag++;
    return u;
}

static int check_response(struct ufshci_host *h, uint8_t want_type, uint8_t tag)
{
    const uint8_t *r = h->ucd + UCD_RSP_OFFSET;
    if (r[0] != want_type || r[3] != tag) {
        UFS_LOG(h, "unexpected response %02x tag %u (want %02x tag %u)", r[0], r[3], want_type, tag);
        return ERROR_DEVICE_ERROR;
    }
    return 0;
}

int ufshci_nop(struct ufshci_host *h)
{
    uint8_t *u = new_upiu(h, UPIU_NOP_OUT, 0, 0);
    uint8_t tag = u[3];
    struct xfer x = { .timeout_us = TIMEOUT_NOP_US };
    int status = issue(h, &x);
    if (status) return status;
    status = check_response(h, UPIU_NOP_IN, tag);
    if (!status && h->ucd[UCD_RSP_OFFSET + 6]) status = ERROR_DEVICE_ERROR;
    return status;
}

static int query(struct ufshci_host *h, uint8_t fn, uint8_t op, uint8_t idn, uint8_t index,
                 uint8_t selector, uint16_t length, uint32_t *value, void *data, uint16_t *data_len)
{
    uint8_t *u = new_upiu(h, UPIU_QUERY_REQ, 0, 0);
    uint8_t tag = u[3];
    u[5] = fn;
    u[12] = op; u[13] = idn; u[14] = index; u[15] = selector;
    put_be16(u + 18, length);
    if (value) put_be32(u + 20, *value);
    struct xfer x = { .timeout_us = TIMEOUT_QUERY_US };
    int status = issue(h, &x);
    if (status) return status;
    if ((status = check_response(h, UPIU_QUERY_RSP, tag))) return status;
    const uint8_t *r = h->ucd + UCD_RSP_OFFSET;
    if (r[6]) return r[6] == 0xf6 || r[6] == 0xf7 ? ERROR_NOT_FOUND : ERROR_DEVICE_ERROR;
    if (value) *value = get_be32(r + 20);
    if (data && data_len) {
        uint16_t got = get_be16(r + 10);
        if (got > *data_len) got = *data_len;
        if (got > UCD_RSP_SIZE - 32) return ERROR_DEVICE_ERROR;
        memcpy(data, r + 32, got);
        *data_len = got;
    }
    return 0;
}

int ufshci_query_flag(struct ufshci_host *h, uint8_t opcode, uint8_t idn, bool *flag)
{
    uint32_t v = 0;
    uint8_t fn = opcode == QUERY_OP_READ_FLAG ? QUERY_FN_READ : QUERY_FN_WRITE;
    int status = query(h, fn, opcode, idn, 0, 0, 0, &v, NULL, NULL);
    if (!status && flag) *flag = v & 1;
    return status;
}

int ufshci_read_descriptor(struct ufshci_host *h, uint8_t idn, uint8_t index, void *buf, uint16_t *len)
{
    if (!buf || !len || !*len) return ERROR_INVALID_PARAMETER;
    return query(h, QUERY_FN_READ, QUERY_OP_READ_DESC, idn, index, 0, *len, NULL, buf, len);
}

/* Data path: DMA into the caller buffer when it is cache-line clean,
 * otherwise go through the bounce buffer. */
static bool dma_direct_ok(const void *buf, uint32_t len)
{
    return !((uintptr_t)buf & 63) && !(len & 63);
}

static int scsi_once(struct ufshci_host *h, uint8_t lun, const uint8_t *cdb, uint8_t cdb_len,
                     void *buf, uint32_t len, bool write)
{
    if (cdb_len > 16) return ERROR_INVALID_PARAMETER;
    void *dma_buf = buf;
    bool bounce = len && !dma_direct_ok(buf, len);
    if (bounce) {
        if (len > UFSHCI_BOUNCE_SIZE) return ERROR_INVALID_PARAMETER;
        dma_buf = h->bounce;
        if (write) memcpy(dma_buf, buf, len);
    }
    uint8_t *u = new_upiu(h, UPIU_COMMAND, len ? (write ? UPIU_FLAG_WRITE : UPIU_FLAG_READ) : 0, lun);
    uint8_t tag = u[3];
    put_be32(u + 12, len);
    memcpy(u + 16, cdb, cdb_len);
    struct xfer x = { .dd = len ? (write ? 1 : 2) : 0, .data = dma_buf, .len = len,
                      .timeout_us = TIMEOUT_SCSI_US };
    int status = issue(h, &x);
    if (status) return status;
    if ((status = check_response(h, UPIU_RESPONSE, tag))) return status;
    const uint8_t *r = h->ucd + UCD_RSP_OFFSET;
    h->last_sense_key = h->last_asc = h->last_ascq = 0;
    if (r[6]) return ERROR_DEVICE_ERROR;   /* target failure */
    if (r[7] == SCSI_CHECK) {
        uint16_t seg = get_be16(r + 10);
        if (seg >= 2 + 14 && seg <= UCD_RSP_SIZE - 32) {
            const uint8_t *sense = r + 34;
            h->last_sense_key = sense[2] & 0xf;
            h->last_asc = sense[12];
            h->last_ascq = sense[13];
        }
        return ERROR_FW; /* distinguishable: CHECK CONDITION with sense */
    }
    if (r[7] != SCSI_GOOD) return ERROR_DEVICE_ERROR;
    if (bounce && !write) memcpy(buf, dma_buf, len);
    return 0;
}

int ufshci_scsi(struct ufshci_host *h, uint8_t lun, const uint8_t *cdb, uint8_t cdb_len,
                void *buf, uint32_t len, bool write)
{
    if (!h || !h->ready) return ERROR_DEVICE_ERROR;
    int status = ERROR_DEVICE_ERROR;
    for (unsigned attempt = 0; attempt < 4; ++attempt) {
        status = scsi_once(h, lun, cdb, cdb_len, buf, len, write);
        if (status != ERROR_FW) return status;
        /* UNIT ATTENTION after reset/power-on, or NOT READY while becoming ready. */
        if (h->last_sense_key == 6) continue;
        if (h->last_sense_key == 2 && h->last_asc == 4) { h->io.delay_us(h->io.context, 10000); continue; }
        UFS_LOG(h, "LU%u CDB %02x sense %x/%02x/%02x", lun, cdb[0], h->last_sense_key,
                h->last_asc, h->last_ascq);
        return ERROR_DEVICE_ERROR;
    }
    return status == ERROR_FW ? ERROR_DEVICE_ERROR : status;
}

/* --- bring-up ------------------------------------------------------------ */

static const char *inherit_check(struct ufshci_host *h)
{
    uint32_t hce = ufshci_read(h, UFSHCI_HCE);
    if (hce == 0xffffffffu) return "HCI reads all-ones";
    if (!(hce & 1)) return "HCE clear";
    uint32_t hcs = ufshci_read(h, UFSHCI_HCS);
    uint32_t need = UFSHCI_HCS_DP | UFSHCI_HCS_UTRLRDY | UFSHCI_HCS_UCRDY;
    if ((hcs & need) != need) return "device/list/UIC not ready";
    if (ufshci_read(h, UFSHCI_IS) & UFSHCI_IS_FATAL) return "fatal interrupt status";
    if (ufshci_read(h, UFSHCI_UTRLDBR) || ufshci_read(h, UFSHCI_UTMRLDBR))
        return "requests outstanding";
    /* The link itself is proven by the NOP OUT/NOP IN round trip after
     * takeover; a UPIU cannot cross a dead UniPro link. The connected-lane
     * count is logged when the host implements DME_GET (QEMU does not). */
    uint32_t lanes = 0;
    if (!ufshci_uic(h, UIC_DME_GET, UNIPRO_PA_CONNECTED_TX_LANES, 0, &lanes) && !lanes)
        return "UniPro reports no connected lanes";
    return NULL;
}

static int takeover(struct ufshci_host *h)
{
    ufshci_write(h, UFSHCI_IE, 0);
    if (stop_lists(h)) return ERROR_TIMEOUT;
    int status = start_lists(h);
    if (status) return status;
    for (unsigned i = 0; i < 3; ++i)
        if (!(status = ufshci_nop(h))) break;
    if (status) (void)stop_lists(h); /* leave it as found for the full init */
    return status;
}

static int link_startup(struct ufshci_host *h)
{
    int status = ERROR_DEVICE_ERROR;
    for (unsigned attempt = 0; attempt < 4; ++attempt) {
        if (h->ops && h->ops->pre_link && (status = h->ops->pre_link(h))) return status;
        ufshci_write(h, UFSHCI_IS, 0xffffffffu);
        status = ufshci_uic(h, UIC_DME_LINKSTARTUP, 0, 0, NULL);
        if (!status && !wait_reg(h, UFSHCI_HCS, UFSHCI_HCS_DP, UFSHCI_HCS_DP, TIMEOUT_REG_US)) {
            if (h->ops && h->ops->post_link && (status = h->ops->post_link(h))) return status;
            return 0;
        }
        UFS_LOG(h, "link startup attempt %u failed (HCS=%08x)", attempt,
                (unsigned)ufshci_read(h, UFSHCI_HCS));
        status = ERROR_DEVICE_ERROR;
    }
    return status;
}

static int device_init(struct ufshci_host *h)
{
    bool pending = true;
    int status = ufshci_query_flag(h, QUERY_OP_READ_FLAG, FLAG_DEVICE_INIT, &pending);
    if (status) return status;
    if (pending) {
        /* Already in progress (inherited) - only poll. */
    } else if (h->path == UFSHCI_PATH_FULL) {
        if ((status = ufshci_query_flag(h, QUERY_OP_SET_FLAG, FLAG_DEVICE_INIT, NULL))) return status;
        pending = true;
    }
    uint64_t start = h->io.now_us(h->io.context);
    while (pending) {
        if (h->io.now_us(h->io.context) - start > TIMEOUT_INIT_US) return ERROR_TIMEOUT;
        h->io.delay_us(h->io.context, 1000);
        if ((status = ufshci_query_flag(h, QUERY_OP_READ_FLAG, FLAG_DEVICE_INIT, &pending)))
            return status;
    }
    return 0;
}

static int full_init(struct ufshci_host *h)
{
    int status;
    ufshci_write(h, UFSHCI_IE, 0);
    if (ufshci_read(h, UFSHCI_HCE) & 1) {
        (void)stop_lists(h);
        ufshci_write(h, UFSHCI_HCE, 0);
        if (wait_reg(h, UFSHCI_HCE, 1, 0, TIMEOUT_REG_US)) {
            UFS_LOG(h, "HCE did not clear");
            return ERROR_TIMEOUT;
        }
    }
    if (h->ops && h->ops->pre_hce && (status = h->ops->pre_hce(h))) return status;
    ufshci_write(h, UFSHCI_HCE, 1);
    if (wait_reg(h, UFSHCI_HCE, 1, 1, TIMEOUT_REG_US)) {
        UFS_LOG(h, "HCE did not set");
        return ERROR_TIMEOUT;
    }
    if (h->ops && h->ops->post_hce && (status = h->ops->post_hce(h))) return status;
    if ((status = link_startup(h))) return status;
    ufshci_write(h, UFSHCI_IE, 0);
    ufshci_write(h, UFSHCI_IS, 0xffffffffu);
    if ((status = start_lists(h))) return status;
    uint64_t start = h->io.now_us(h->io.context);
    while ((status = ufshci_nop(h))) {
        if (h->io.now_us(h->io.context) - start > TIMEOUT_INIT_US) {
            UFS_LOG(h, "device does not answer NOP OUT");
            return status;
        }
        h->io.delay_us(h->io.context, 1000);
    }
    return 0;
}

int ufshci_init(struct ufshci_host *h)
{
    if (!h || !h->base || !h->io.read32 || !h->io.write32 || !h->io.now_us || !h->io.delay_us)
        return ERROR_INVALID_PARAMETER;
    h->ready = false;
    h->path = UFSHCI_PATH_NONE;
    h->inherit_reject = NULL;
    h->lu_count = 0;
    h->cap = ufshci_read(h, UFSHCI_CAP);
    h->version = ufshci_read(h, UFSHCI_VER);
    if (h->cap == 0xffffffffu || h->version == 0xffffffffu || !h->version) {
        UFS_LOG(h, "no UFSHCI at %lx", (unsigned long)h->base);
        return ERROR_NOT_FOUND;
    }
    h->nutrs = (uint8_t)((h->cap & 0x1f) + 1);
    int status = attach_memory(h);
    if (status) return status;

    if (h->policy != UFSHCI_FORCE_FULL) {
        h->inherit_reject = inherit_check(h);
        if (!h->inherit_reject) {
            h->path = UFSHCI_PATH_INHERITED;
            status = takeover(h);
            if (status) h->inherit_reject = "takeover NOP failed";
        }
        if (h->inherit_reject) {
            UFS_LOG(h, "inherited state rejected: %s", h->inherit_reject);
            if (h->policy == UFSHCI_INHERIT_ONLY) return ERROR_NOT_SUPPORTED;
        }
    }
    if (h->path != UFSHCI_PATH_INHERITED || h->inherit_reject) {
        h->path = UFSHCI_PATH_FULL;
        if ((status = full_init(h))) return status;
    }
    h->ready = true;
    if ((status = device_init(h))) {
        h->ready = false;
        UFS_LOG(h, "fDeviceInit failed (%d)", status);
        return status;
    }
    UFS_LOG(h, "UFSHCI %x.%x ready via %s path, %u slots", (unsigned)(h->version >> 8) & 0xff,
            (unsigned)(h->version >> 4) & 0xf, ufshci_path_name(h->path), h->nutrs);
    return 0;
}

int ufshci_quiesce(struct ufshci_host *h)
{
    if (!h || !h->utrl) return 0;
    h->ready = false;
    if (wait_reg(h, UFSHCI_UTRLDBR, 0xffffffffu, 0, TIMEOUT_SCSI_US)) return ERROR_TIMEOUT;
    return stop_lists(h);
}

/* --- block layer --------------------------------------------------------- */

static int rw_blocks(struct ufshci_lu *lu, uint64_t lba, uint32_t count, void *buf, bool write)
{
    struct ufshci_host *h = lu->host;
    uint32_t bs = lu->dev.block_size;
    if (!count) return 0;
    if (!buf || lba >= lu->dev.num_blocks || count > lu->dev.num_blocks - lba)
        return ERROR_INVALID_PARAMETER;
    uint8_t *p = buf;
    bool direct = dma_direct_ok(buf, count * bs);
    uint32_t per = direct ? lu->dev.max_transfer_blocks : UFSHCI_BOUNCE_SIZE / bs;
    while (count) {
        uint32_t n = count < per ? count : per;
        uint8_t cdb[16] = {0};
        uint8_t len;
        if (lba + n - 1 <= 0xffffffffu && n <= 0xffff) {
            cdb[0] = write ? 0x2a : 0x28;
            put_be32(cdb + 2, (uint32_t)lba);
            put_be16(cdb + 7, (uint16_t)n);
            len = 10;
        } else {
            cdb[0] = write ? 0x8a : 0x88;
            put_be32(cdb + 2, (uint32_t)(lba >> 32));
            put_be32(cdb + 6, (uint32_t)lba);
            put_be32(cdb + 10, n);
            len = 16;
        }
        int status = ufshci_scsi(h, lu->lun, cdb, len, p, n * bs, write);
        if (status) return status;
        p += (size_t)n * bs; lba += n; count -= n;
    }
    return 0;
}

int ufshci_read_blocks(struct ufshci_lu *lu, uint64_t lba, uint32_t count, void *buf)
{
    return rw_blocks(lu, lba, count, buf, false);
}

static int blk_read(struct kshim_blkdev *dev, uint64_t lba, uint32_t count, void *buf)
{
    return rw_blocks(dev->priv, lba, count, buf, false);
}

static int blk_write(struct kshim_blkdev *dev, uint64_t lba, uint32_t count, const void *buf)
{
    return rw_blocks(dev->priv, lba, count, (void *)buf, true);
}

static int blk_flush(struct kshim_blkdev *dev)
{
    struct ufshci_lu *lu = dev->priv;
    const uint8_t cdb[10] = {0x35};
    return ufshci_scsi(lu->host, lu->lun, cdb, sizeof(cdb), NULL, 0, false);
}

static const struct kshim_blkdev_ops ro_ops = { .read = blk_read };
static const struct kshim_blkdev_ops rw_ops = { .read = blk_read, .write = blk_write, .flush = blk_flush };

static int read_capacity(struct ufshci_host *h, uint8_t lun, uint64_t *blocks, uint32_t *bs)
{
    uint8_t cdb[16] = {0x25}, data[32];
    int status = ufshci_scsi(h, lun, cdb, 10, data, 8, false);
    if (status) return status;
    uint64_t last = get_be32(data);
    *bs = get_be32(data + 4);
    if (last == 0xffffffffu) {
        memset(cdb, 0, sizeof(cdb));
        cdb[0] = 0x9e; cdb[1] = 0x10;
        put_be32(cdb + 10, sizeof(data));
        if ((status = ufshci_scsi(h, lun, cdb, 16, data, sizeof(data), false))) return status;
        last = get_be64(data);
        *bs = get_be32(data + 8);
    }
    *blocks = last + 1;
    return 0;
}

int ufshci_scan(struct ufshci_host *h)
{
    if (!h || !h->ready) return ERROR_DEVICE_ERROR;
    uint8_t desc[256];
    uint16_t len = sizeof(desc);
    int status = ufshci_read_descriptor(h, DESC_DEVICE, 0, desc, &len);
    if (status || len < 7) {
        UFS_LOG(h, "device descriptor read failed (%d)", status);
        return status ? status : ERROR_DEVICE_ERROR;
    }
    unsigned expected = desc[6], max_lu = 8;
    len = sizeof(desc);
    /* Geometry bMaxNumberLU (offset 0x0c): 0 = 8 LUs, 1 = 32 LUs. */
    if (!ufshci_read_descriptor(h, DESC_GEOMETRY, 0, desc, &len) && len > 0x0c && desc[0x0c] == 1)
        max_lu = 32;
    h->lu_count = 0;
    for (unsigned lun = 0; lun < max_lu && h->lu_count < UFSHCI_MAX_LUS; ++lun) {
        len = sizeof(desc);
        h->quiet = true; /* absent LUs answer with a query/OCS error */
        int miss = ufshci_read_descriptor(h, DESC_UNIT, (uint8_t)lun, desc, &len);
        h->quiet = false;
        if (miss || len < 4 || !desc[3])
            continue;
        const uint8_t tur[6] = {0};
        (void)ufshci_scsi(h, (uint8_t)lun, tur, sizeof(tur), NULL, 0, false);
        uint64_t blocks; uint32_t bs;
        if ((status = read_capacity(h, (uint8_t)lun, &blocks, &bs)) || bs < 512 || (bs & (bs - 1)) ||
            !blocks) {
            UFS_LOG(h, "LU%u READ CAPACITY failed (%d)", lun, status);
            continue;
        }
        struct ufshci_lu *lu = &h->lus[h->lu_count];
        memset(lu, 0, sizeof(*lu));
        lu->host = h; lu->lun = (uint8_t)lun;
        snprintf(lu->dev.name, sizeof(lu->dev.name), "%s:lu%u", h->name ? h->name : "ufs0", lun);
        lu->dev.kind = KSHIM_BLKDEV_UFS_LU;
        lu->dev.block_size = bs;
        lu->dev.num_blocks = blocks;
        /* 8 MiB per request = 32 PRDT entries of 256 KiB. */
        uint32_t max = (8u * 1024u * 1024u) / bs;
        lu->dev.max_transfer_blocks = max > 0xffff ? 0xffff : max;
        lu->dev.read_only = !h->allow_write;
        lu->dev.ops = h->allow_write ? &rw_ops : &ro_ops;
        lu->dev.priv = lu;
        ++h->lu_count;
        status = kshim_blkdev_register(&lu->dev);
        UFS_LOG(h, "%s: %llu x %u bytes%s", lu->dev.name, (unsigned long long)blocks, bs,
                status ? " (register failed)" : "");
    }
    if (expected && h->lu_count != expected && h->lu_count < UFSHCI_MAX_LUS)
        UFS_LOG(h, "device reports %u LUs, %u usable", expected, h->lu_count);
    return h->lu_count ? 0 : ERROR_NOT_FOUND;
}

int ufshci_probe(struct ufshci_host *h)
{
    int status = ufshci_init(h);
    return status ? status : ufshci_scan(h);
}
