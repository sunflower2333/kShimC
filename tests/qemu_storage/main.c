/* SPDX-License-Identifier: MIT */
/*
 * QEMU smoke test for the kShimC storage cores on "-M virt,highmem=off":
 * the real ufshci.c against QEMU's PCI "ufs"/"ufs-lu" and the real sdmmc.c
 * plus CrDK SdhciMsm.c against "sdhci-pci"/"sd-card". Only the ECAM/BAR setup
 * and a tiny libc are test-specific. MMU and caches stay off, so DMA is
 * coherent and the platform hooks are not exercised (no Qualcomm vendor
 * registers exist in QEMU).
 */
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <kshim/sdmmc.h>
#include <kshim/ufshci.h>

#define UART 0x09000000ul
#define ECAM 0x3f000000ul
#define MMIO_WINDOW 0x10000000ul

/* --- tiny libc ----------------------------------------------------------- */
void *memcpy(void *d, const void *s, size_t n) { uint8_t *a = d; const uint8_t *b = s; while (n--) *a++ = *b++; return d; }
void *memmove(void *d, const void *s, size_t n)
{
    uint8_t *a = d; const uint8_t *b = s;
    if (a < b) while (n--) *a++ = *b++; else while (n--) a[n] = b[n];
    return d;
}
void *memset(void *d, int c, size_t n) { uint8_t *a = d; while (n--) *a++ = (uint8_t)c; return d; }
int memcmp(const void *x, const void *y, size_t n)
{
    const uint8_t *a = x, *b = y;
    for (; n; --n, ++a, ++b) if (*a != *b) return *a - *b;
    return 0;
}
size_t strlen(const char *s) { size_t n = 0; while (s[n]) ++n; return n; }
int strcmp(const char *a, const char *b) { while (*a && *a == *b) ++a, ++b; return (uint8_t)*a - (uint8_t)*b; }
int strncmp(const char *a, const char *b, size_t n)
{
    for (; n; --n, ++a, ++b) { if (*a != *b || !*a) return (uint8_t)*a - (uint8_t)*b; }
    return 0;
}

static void uart_putc(char c) { *(volatile uint32_t *)UART = (uint8_t)c; }

struct out { char *buf; size_t cap, len; };
static void emit(struct out *o, char c)
{
    if (o->buf) { if (o->len + 1 < o->cap) o->buf[o->len] = c; }
    else uart_putc(c);
    o->len++;
}

int vsnprintf(char *buf, size_t cap, const char *fmt, va_list ap)
{
    struct out o = {buf, cap, 0};
    for (; *fmt; ++fmt) {
        if (*fmt != '%') { emit(&o, *fmt); continue; }
        ++fmt;
        bool zero = false;
        if (*fmt == '#') ++fmt;
        if (*fmt == '0') { zero = true; ++fmt; }
        unsigned width = 0;
        while (*fmt >= '0' && *fmt <= '9') width = width * 10 + (unsigned)(*fmt++ - '0');
        int longs = 0;
        while (*fmt == 'l') { ++longs; ++fmt; }
        char tmp[24]; unsigned n = 0; uint64_t v; bool neg = false;
        switch (*fmt) {
        case 's': { const char *s = va_arg(ap, const char *); if (!s) s = "(null)"; while (*s) emit(&o, *s++); continue; }
        case 'c': emit(&o, (char)va_arg(ap, int)); continue;
        case '%': emit(&o, '%'); continue;
        case 'd': { int64_t x = longs ? va_arg(ap, int64_t) : va_arg(ap, int); neg = x < 0; v = neg ? (uint64_t)-x : (uint64_t)x; break; }
        case 'u': case 'x': case 'p':
            v = *fmt == 'p' ? (uint64_t)(uintptr_t)va_arg(ap, void *) : longs ? va_arg(ap, uint64_t) : va_arg(ap, unsigned);
            break;
        default: continue;
        }
        unsigned base = (*fmt == 'x' || *fmt == 'p') ? 16 : 10;
        do { tmp[n++] = "0123456789abcdef"[v % base]; v /= base; } while (v);
        if (neg) tmp[n++] = '-';
        while (width > n) { emit(&o, zero ? '0' : ' '); --width; }
        while (n) emit(&o, tmp[--n]);
    }
    if (buf && cap) buf[o.len < cap ? o.len : cap - 1] = 0;
    return (int)o.len;
}
int snprintf(char *buf, size_t cap, const char *fmt, ...)
{ va_list ap; va_start(ap, fmt); int n = vsnprintf(buf, cap, fmt, ap); va_end(ap); return n; }
int printf(const char *fmt, ...)
{ va_list ap; va_start(ap, fmt); int n = vsnprintf(NULL, 0, fmt, ap); va_end(ap); return n; }
int puts(const char *s) { while (*s) uart_putc(*s++); uart_putc('\n'); return 0; }
int putchar(int c) { uart_putc((char)c); return c; }

/* --- time and MMIO --------------------------------------------------------- */
static uint64_t now_us(void *c)
{
    (void)c;
    uint64_t t, f;
    __asm__ volatile("isb; mrs %0, cntpct_el0" : "=r"(t));
    __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(f));
    return t / f * 1000000u + (t % f) * 1000000u / f;
}
static void delay_us(void *c, uint32_t us) { uint64_t s = now_us(c); while (now_us(c) - s < us) {} }
static uint32_t rd32(void *c, uintptr_t a) { (void)c; return *(volatile uint32_t *)a; }
static void wr32(void *c, uintptr_t a, uint32_t v) { (void)c; *(volatile uint32_t *)a = v; }

/* --- blkdev registry stand-in ------------------------------------------- */
static struct kshim_blkdev *devs[16];
static unsigned dev_count;
int kshim_blkdev_register(struct kshim_blkdev *dev)
{
    if (dev_count == 16) return ERROR_NOT_SUPPORTED;
    devs[dev_count++] = dev;
    return 0;
}

/* --- PCI ----------------------------------------------------------------- */
static uintptr_t next_mmio = MMIO_WINDOW;

static uintptr_t pci_setup(uint32_t class16, unsigned *slot_out)
{
    for (unsigned dev = 0; dev < 32; ++dev) {
        volatile uint32_t *cfg = (volatile uint32_t *)(ECAM + ((uintptr_t)dev << 15));
        if ((cfg[0] & 0xffff) == 0xffff || (cfg[2] >> 16) != class16) continue;
        cfg[4] = 0xffffffffu;
        uint32_t bar = cfg[4];
        bool is64 = ((bar >> 1) & 3) == 2;
        uint32_t size = ~(bar & ~0xfu) + 1;
        uintptr_t addr = (next_mmio + size - 1) & ~(uintptr_t)(size - 1);
        next_mmio = addr + size;
        cfg[4] = (uint32_t)addr | (bar & 0xf);
        if (is64) cfg[5] = 0;
        cfg[1] |= 0x6; /* memory space + bus master */
        *slot_out = dev;
        printf("pci: 00:%02u.0 class %06x BAR0 %lx size %x\n", dev, (unsigned)(cfg[2] >> 8), (uint64_t)addr, size);
        return addr;
    }
    return 0;
}

/* Patterns written by tests/storage_qemu.py. */
static uint8_t ufs_pattern(uint64_t lba, uint32_t i) { return (uint8_t)(lba * 131 + i * 7 + 0x5a); }
static uint8_t sd_pattern(uint64_t lba, uint32_t i) { return (uint8_t)(lba * 29 + i * 3 + 0xa5); }

static int failures;
#define CHECK(c) do { if (!(c)) { printf("CHECK FAILED %s:%d: %s\n", __FILE__, __LINE__, #c); ++failures; } } while (0)

static uint8_t buf[64 * 4096] __attribute__((aligned(64)));

static bool verify(uint8_t (*pat)(uint64_t, uint32_t), const uint8_t *p, uint64_t lba, uint32_t count, uint32_t bs)
{
    for (uint32_t b = 0; b < count; ++b)
        for (uint32_t i = 0; i < bs; ++i)
            if (p[(size_t)b * bs + i] != pat(lba + b, i)) {
                printf("mismatch lba %lu byte %u: %02x != %02x\n", lba + b, i, p[(size_t)b * bs + i], pat(lba + b, i));
                return false;
            }
    return true;
}

static void ufs_test(void)
{
    unsigned slot;
    uintptr_t bar = pci_setup(0x0109, &slot); /* mass storage / UFS */
    CHECK(bar);
    if (!bar) return;
    static struct ufshci_host h;
    memset(&h, 0, sizeof(h));
    h.base = bar;
    h.io = (struct CrIo){NULL, rd32, wr32, now_us, delay_us};
    h.name = "ufs0";
    h.policy = UFSHCI_PREFER_INHERIT;
    unsigned first = dev_count;
    CHECK(ufshci_probe(&h) == 0);
    printf("UFS_PATH first=%s\n", ufshci_path_name(h.path));
    CHECK(h.path == UFSHCI_PATH_FULL && h.lu_count == 1 && dev_count == first + 1);
    if (dev_count != first + 1) return;
    struct kshim_blkdev *lu = devs[first];
    CHECK(!strcmp(lu->name, "ufs0:lu0") && lu->block_size == 4096 && lu->num_blocks == 4096);
    CHECK(lu->ops->read(lu, 0, 64, buf) == 0 && verify(ufs_pattern, buf, 0, 64, 4096));
    CHECK(lu->ops->read(lu, 4095, 1, buf) == 0 && verify(ufs_pattern, buf, 4095, 1, 4096));
    CHECK(lu->ops->read(lu, 1000, 3, buf + 4) == 0 && verify(ufs_pattern, buf + 4, 1000, 3, 4096)); /* bounce */
    CHECK(lu->ops->read(lu, 4095, 2, buf) == ERROR_INVALID_PARAMETER);

    /* A second owner finds the controller live and takes it over without
     * resetting it (the path kShimC prefers after XBL/ABL). */
    dev_count = first;
    CHECK(ufshci_probe(&h) == 0);
    printf("UFS_PATH second=%s\n", ufshci_path_name(h.path));
    CHECK(h.path == UFSHCI_PATH_INHERITED && dev_count == first + 1);
    lu = devs[first];
    CHECK(lu->ops->read(lu, 2048, 16, buf) == 0 && verify(ufs_pattern, buf, 2048, 16, 4096));
    CHECK(ufshci_quiesce(&h) == 0);
}

/* sdhci-pci exposes the 256-byte standard window; CrDK's core also
 * describes the v5 vendor window. Reads there return 0, writes are dropped. */
static uintptr_t sd_base;
static CR_STATUS sd_ready(void *c) { (void)c; return CR_SUCCESS; }
static CR_STATUS sd_read(void *c, UINT32 o, UINT8 w, UINT32 *v)
{
    (void)c;
    if (o >= 0x100) { *v = 0; return CR_SUCCESS; }
    volatile void *p = (volatile void *)(sd_base + o);
    *v = w == 1 ? *(volatile uint8_t *)p : w == 2 ? *(volatile uint16_t *)p : *(volatile uint32_t *)p;
    return CR_SUCCESS;
}
static CR_STATUS sd_write(void *c, UINT32 o, UINT8 w, UINT32 v)
{
    (void)c;
    if (o >= 0x100) return CR_SUCCESS;
    volatile void *p = (volatile void *)(sd_base + o);
    if (w == 1) *(volatile uint8_t *)p = (uint8_t)v;
    else if (w == 2) *(volatile uint16_t *)p = (uint16_t)v;
    else *(volatile uint32_t *)p = v;
    return CR_SUCCESS;
}
static CR_STATUS sd_stall(void *c, UINT32 us) { delay_us(c, us); return CR_SUCCESS; }
static CR_STATUS sd_barrier(void *c) { (void)c; __asm__ volatile("dsb sy" ::: "memory"); return CR_SUCCESS; }

static void sd_test(void)
{
    unsigned slot;
    sd_base = pci_setup(0x0805, &slot); /* base system peripheral / SD host */
    CHECK(sd_base);
    if (!sd_base) return;
    static struct sdmmc_host h;
    memset(&h, 0, sizeof(h));
    h.io = (CR_SDHC_IO){NULL, sd_ready, sd_read, sd_write, sd_stall, sd_barrier, 0x1000, CR_SDHC_WINDOW_ALL};
    snprintf(h.name, sizeof(h.name), "sdhc%u", slot);
    unsigned first = dev_count;
    CHECK(sdmmc_probe(&h) == 0);
    CHECK(h.type == SDMMC_SD && dev_count == first + 1);
    if (dev_count != first + 1) return;
    struct kshim_blkdev *d = devs[first];
    printf("SD %s blocks=%lu high_capacity=%u width=%u\n", d->name, d->num_blocks, h.high_capacity, h.bus_width);
    CHECK(d->kind == KSHIM_BLKDEV_SD && d->block_size == 512 && d->num_blocks == 8192);
    CHECK(d->ops->read(d, 7, 1, buf) == 0 && verify(sd_pattern, buf, 7, 1, 512));         /* CMD17 */
    CHECK(d->ops->read(d, 100, 300, buf) == 0 && verify(sd_pattern, buf, 100, 300, 512));  /* CMD18 */
    CHECK(d->ops->read(d, 8191, 1, buf + 1) == 0 && verify(sd_pattern, buf + 1, 8191, 1, 512));
    CHECK(sdmmc_quiesce(&h) == 0);
}

void smoke_main(void)
{
    printf("kShimC storage smoke test\n");
    ufs_test();
    sd_test();
    printf(failures ? "STORAGE_SMOKE_FAIL %d\n" : "STORAGE_SMOKE_PASS\n", failures);
}
