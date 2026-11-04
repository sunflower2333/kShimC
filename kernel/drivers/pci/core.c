/* SPDX-License-Identifier: MIT */
/*
 * PCI core (CONFIG_KSHIM_PCI): host registry, configuration access with a
 * write journal, bus enumeration, BAR and bridge-window assignment that
 * keeps valid inherited assignments, INTx through the host interrupt-map,
 * driver binding and the handoff quiesce. See kshim/pci.h and docs/pci.md.
 *
 * Register facts are from the PCI Local Bus 3.0, PCI-to-PCI Bridge 1.2 and
 * PCI Express Base specifications (type 0/1 headers, BAR sizing, bridge
 * windows, capability lists, interrupt pin swizzle).
 */
#include "pci_internal.h"

#include <config.h>
#include <dt.h>
#include <kshim/irq.h>
#include <kshim/smp.h>
#include <kshim/time.h>
#include <stdio.h>
#include <string.h>

#if CONFIG_KSHIM_REGULATOR
#include <kshim/regulator.h>
#endif
#if CONFIG_KSHIM_GPIO
#include <kshim/pinctrl.h>
#endif

/* Configuration header offsets (PCI 3.0 section 6.1, bridge 1.2 3.2). */
#define CFG_STATUS_CAP_LIST     0x0010U
#define CFG_HEADER_MULTI        0x80U
#define CFG_HEADER_TYPE_MASK    0x7fU
#define CFG_SUBSYSTEM_VENDOR    0x2c
#define CFG_CAP_PTR             0x34
#define CFG_BUS_NUMBERS         0x18
#define CFG_SUBORDINATE_BUS     0x1a
#define CFG_IO_BASE             0x1c
#define CFG_IO_LIMIT            0x1d
#define CFG_MEM_BASE            0x20
#define CFG_MEM_LIMIT           0x22
#define CFG_PREF_BASE           0x24
#define CFG_PREF_LIMIT          0x26
#define CFG_PREF_BASE_UPPER     0x28
#define CFG_PREF_LIMIT_UPPER    0x2c
#define CFG_IO_BASE_UPPER       0x30
#define CFG_IO_LIMIT_UPPER      0x32
#define BAR_IO                  0x1U
#define BAR_MEM_TYPE_64         0x4U
#define BAR_MEM_PREFETCH        0x8U
#define EXP_FLAGS               0x02
#define EXP_TYPE_ROOT_PORT      0x4U
#define EXP_TYPE_DOWNSTREAM     0x6U
#define MSI_CTRL_ENABLE         0x0001U
#define MSI_CTRL_64BIT          0x0080U
#define MSI_CTRL_MME_MASK       0x0070U

#define IO_GRANULE   UINT64_C(0x1000)
#define MEM_GRANULE  UINT64_C(0x100000)
#define IO_FLOOR     UINT64_C(0x1000)   /* never hand out legacy I/O ports */
#define JOURNAL_MAX  256U
#define INTX_LINES   16U
#define POWER_NODES  16U
#define MAP_NODES    8U
#define REQUESTS_MAX 64U

struct dev_priv {
    kshim_irq_handler_t handler;
    void *context;
    unsigned line;          /* INTx line index while requested */
    bool pcie_port;         /* root/downstream port: only device 0 below */
    bool fresh;             /* bridge windows (re)assigned by kShimC */
    bool bme_on_entry;
};

struct journal_entry {
    struct kshim_pci_host *host;
    uint8_t bus, devfn, mask;
    uint16_t reg;
    uint32_t value;
};

struct intx_line {
    uint32_t intid;
    unsigned trigger, users;
};

static const void *Fdt;
static struct kshim_pci_host *Hosts;
static unsigned HostCount;
static struct kshim_pci_driver *Drivers;
static struct kshim_pci_dev Devs[KSHIM_PCI_MAX_DEVICES];
static struct dev_priv Priv[KSHIM_PCI_MAX_DEVICES];
static unsigned DevCount;
static struct journal_entry Journal[JOURNAL_MAX];
static unsigned JournalCount;
static struct intx_line Lines[INTX_LINES];
#if CONFIG_KSHIM_REGULATOR
static int PowerNodes[POWER_NODES];
static unsigned PowerCount;
#endif
struct map_node { int node; bool mapped; };
static struct map_node MapNodes[MAP_NODES];
static unsigned MapCount;
static bool Initialized, Quiesced;

#define LOG(...) printf("PCI: " __VA_ARGS__)

/* ---- small helpers ----------------------------------------------------- */

const void *kshim_pci_fdt(void) { return Fdt; }
uint64_t kshim_pci_time_us(void) { return kshim_smp_time_us(); }
void kshim_pci_delay_us(uint32_t us) { (void)delay(us); }

void kshim_pci_mark_node(int node, bool mapped)
{
    for (unsigned i = 0; i < MapCount; ++i)
        if (MapNodes[i].node == node) { MapNodes[i].mapped = mapped; return; }
    if (MapCount < MAP_NODES) MapNodes[MapCount++] = (struct map_node){node, mapped};
}

bool kshim_pci_node_unmapped(int node)
{
    for (unsigned i = 0; i < MapCount; ++i)
        if (MapNodes[i].node == node) return !MapNodes[i].mapped;
    return false;
}

static struct dev_priv *priv(const struct kshim_pci_dev *dev)
{
    return &Priv[dev - Devs];
}

static bool valid_dev(const struct kshim_pci_dev *dev)
{
    return dev && dev >= Devs && dev < Devs + DevCount;
}

static bool pow2(uint64_t v) { return v && !(v & (v - 1)); }

static uint64_t align_up(uint64_t v, uint64_t a) { return (v + a - 1) & ~(a - 1); }

/* ---- configuration access ---------------------------------------------- */

static uint32_t cfg_rd(struct kshim_pci_host *h, uint8_t bus, uint8_t devfn, uint16_t reg,
                       unsigned width)
{
    uint32_t v = 0xffffffffU;
    if (h->ops->read(h, bus, devfn, reg, width, &v)) v = 0xffffffffU;
    if (width < 4) v &= (UINT32_C(1) << (8 * width)) - 1;
    return v;
}

static int cfg_wr(struct kshim_pci_host *h, uint8_t bus, uint8_t devfn, uint16_t reg,
                  unsigned width, uint32_t v)
{
    return h->ops->write(h, bus, devfn, reg, width, v);
}

static uint32_t rd(const struct kshim_pci_dev *d, uint16_t reg, unsigned width)
{
    return cfg_rd(d->host, d->bus, d->devfn, reg, width);
}

static int raw_wr(const struct kshim_pci_dev *d, uint16_t reg, unsigned width, uint32_t v)
{
    return cfg_wr(d->host, d->bus, d->devfn, reg, width, v);
}

static bool access_ok(uint16_t reg, unsigned width)
{
    return (width == 1 || width == 2 || width == 4) && reg < 0x1000U &&
           !(reg & (width - 1U));
}

/* First write to a dword records its inherited value; quiesce writes back
 * exactly the bytes kShimC wrote, so W1C status bits next to COMMAND or the
 * bridge secondary status are never acknowledged by the replay. */
static int journal_write(struct kshim_pci_host *h, uint8_t bus, uint8_t devfn, uint16_t reg,
                         unsigned width, uint32_t v)
{
    uint16_t dword = reg & ~3U;
    uint8_t bytes = (uint8_t)(((1U << width) - 1U) << (reg & 3U));
    struct journal_entry *e = NULL;
    for (unsigned i = 0; i < JournalCount && !e; ++i)
        if (Journal[i].host == h && Journal[i].bus == bus && Journal[i].devfn == devfn &&
            Journal[i].reg == dword) e = &Journal[i];
    if (!e) {
        if (JournalCount == JOURNAL_MAX) {
            LOG("config journal full, write to %02x:%02x.%u+0x%x refused\n", bus,
                KSHIM_PCI_SLOT(devfn), KSHIM_PCI_FUNC(devfn), reg);
            return KSHIM_IO_ERR_OVERFLOW;
        }
        e = &Journal[JournalCount++];
        *e = (struct journal_entry){h, bus, devfn, 0, dword, cfg_rd(h, bus, devfn, dword, 4)};
    }
    e->mask |= bytes;
    return cfg_wr(h, bus, devfn, reg, width, v);
}

static int jwr(const struct kshim_pci_dev *d, uint16_t reg, unsigned width, uint32_t v)
{
    return journal_write(d->host, d->bus, d->devfn, reg, width, v);
}

/* Write a value only when it differs (inherit first). */
static int jwr_changed(const struct kshim_pci_dev *d, uint16_t reg, unsigned width, uint32_t v)
{
    return rd(d, reg, width) == v ? 0 : jwr(d, reg, width, v);
}

static void journal_replay(void)
{
    for (unsigned i = JournalCount; i-- > 0;) {
        struct journal_entry *e = &Journal[i];
        if (e->mask == 0xfU) {
            (void)cfg_wr(e->host, e->bus, e->devfn, e->reg, 4, e->value);
            continue;
        }
        for (unsigned half = 0; half < 2; ++half) {
            unsigned m = (e->mask >> (2 * half)) & 3U;
            uint16_t reg = (uint16_t)(e->reg + 2 * half);
            uint32_t v = e->value >> (16 * half);
            if (m == 3U) (void)cfg_wr(e->host, e->bus, e->devfn, reg, 2, v & 0xffffU);
            else if (m) {
                for (unsigned b = 0; b < 2; ++b)
                    if (m & (1U << b))
                        (void)cfg_wr(e->host, e->bus, e->devfn, (uint16_t)(reg + b), 1,
                                     (v >> (8 * b)) & 0xffU);
            }
        }
    }
    JournalCount = 0;
}

static int check_access(struct kshim_pci_dev *dev, uint16_t reg, unsigned width)
{
    if (Quiesced) return KSHIM_IO_ERR_QUIESCED;
    if (!valid_dev(dev) || !access_ok(reg, width)) return ERROR_INVALID_PARAMETER;
    return 0;
}

static int read_config(struct kshim_pci_dev *dev, uint16_t reg, unsigned width, uint32_t *v)
{
    if (!v) return ERROR_INVALID_PARAMETER;
    *v = width == 4 ? 0xffffffffU : (UINT32_C(1) << (8 * width)) - 1;
    int status = check_access(dev, reg, width);
    if (status) return status;
    status = dev->host->ops->read(dev->host, dev->bus, dev->devfn, reg, width, v);
    if (status) *v = width == 4 ? 0xffffffffU : (UINT32_C(1) << (8 * width)) - 1;
    return status;
}

int kshim_pci_read_config8(struct kshim_pci_dev *dev, uint16_t reg, uint8_t *value)
{
    uint32_t v = 0xffU;
    int status = value ? read_config(dev, reg, 1, &v) : ERROR_INVALID_PARAMETER;
    if (value) *value = (uint8_t)v;
    return status;
}

int kshim_pci_read_config16(struct kshim_pci_dev *dev, uint16_t reg, uint16_t *value)
{
    uint32_t v = 0xffffU;
    int status = value ? read_config(dev, reg, 2, &v) : ERROR_INVALID_PARAMETER;
    if (value) *value = (uint16_t)v;
    return status;
}

int kshim_pci_read_config32(struct kshim_pci_dev *dev, uint16_t reg, uint32_t *value)
{
    return read_config(dev, reg, 4, value);
}

static int write_config(struct kshim_pci_dev *dev, uint16_t reg, unsigned width, uint32_t v)
{
    int status = check_access(dev, reg, width);
    return status ? status : jwr(dev, reg, width, v);
}

int kshim_pci_write_config8(struct kshim_pci_dev *dev, uint16_t reg, uint8_t value)
{
    return write_config(dev, reg, 1, value);
}

int kshim_pci_write_config16(struct kshim_pci_dev *dev, uint16_t reg, uint16_t value)
{
    return write_config(dev, reg, 2, value);
}

int kshim_pci_write_config32(struct kshim_pci_dev *dev, uint16_t reg, uint32_t value)
{
    return write_config(dev, reg, 4, value);
}

int kshim_pci_write_config_raw32(struct kshim_pci_dev *dev, uint16_t reg, uint32_t value)
{
    int status = check_access(dev, reg, 4);
    return status ? status : raw_wr(dev, reg, 4, value);
}

/* ---- capabilities ------------------------------------------------------ */

static uint8_t find_cap(const struct kshim_pci_dev *d, uint8_t id)
{
    if (!(rd(d, KSHIM_PCI_STATUS, 2) & CFG_STATUS_CAP_LIST)) return 0;
    uint8_t ptr = (uint8_t)(rd(d, CFG_CAP_PTR, 1) & 0xfcU);
    for (unsigned guard = 0; ptr >= 0x40U && guard < 48U; ++guard) {
        uint32_t header = rd(d, ptr, 2);
        if ((header & 0xffU) == 0xffU) break;
        if ((header & 0xffU) == id) return ptr;
        ptr = (uint8_t)((header >> 8) & 0xfcU);
    }
    return 0;
}

uint16_t kshim_pci_find_capability(struct kshim_pci_dev *dev, uint8_t id)
{
    return valid_dev(dev) && !Quiesced ? find_cap(dev, id) : 0;
}

uint16_t kshim_pci_find_ext_capability(struct kshim_pci_dev *dev, uint16_t id)
{
    if (!valid_dev(dev) || Quiesced || !dev->cap_exp) return 0;
    uint16_t ptr = 0x100;
    for (unsigned guard = 0; ptr >= 0x100U && guard < 480U; ++guard) {
        uint32_t header = rd(dev, ptr, 4);
        if (header == 0 || header == 0xffffffffU) return 0;
        if ((header & 0xffffU) == id) return ptr;
        ptr = (uint16_t)((header >> 20) & 0xffcU);
    }
    return 0;
}

/* ---- enumeration ------------------------------------------------------- */

static int match_node(int parent, uint8_t devfn)
{
    if (!Fdt || parent < 0) return -1;
    int child;
    fdt_for_each_subnode(child, Fdt, parent) {
        int len;
        const fdt32_t *reg = fdt_getprop(Fdt, child, "reg", &len);
        if (!reg || len < 4 || !dt_available(Fdt, child)) continue;
        if (((fdt32_to_cpu(reg[0]) >> 8) & 0xffU) == devfn) return child;
    }
    return -1;
}

static void size_bars(struct kshim_pci_dev *d)
{
    unsigned type = d->header_type & CFG_HEADER_TYPE_MASK;
    unsigned count = type == 0 ? 6U : type == 1 ? 2U : 0U;
    if (!count) return;
    uint32_t cmd = rd(d, KSHIM_PCI_COMMAND, 2);
    uint32_t decode = KSHIM_PCI_COMMAND_IO | KSHIM_PCI_COMMAND_MEMORY;
    /* Decoding off while a BAR holds the sizing pattern (raw: undone below). */
    if (cmd & decode) (void)raw_wr(d, KSHIM_PCI_COMMAND, 2, cmd & ~decode);
    for (unsigned i = 0; i < count; ++i) {
        uint16_t reg = (uint16_t)(KSHIM_PCI_BAR0 + 4U * i);
        uint32_t orig = rd(d, reg, 4);
        (void)raw_wr(d, reg, 4, 0xffffffffU);
        uint32_t mask = rd(d, reg, 4);
        (void)raw_wr(d, reg, 4, orig);
        struct kshim_pci_bar *bar = &d->bars[i];
        memset(bar, 0, sizeof(*bar));
        if (!mask || mask == 0xffffffffU) continue;
        if (orig & BAR_IO) {
            uint32_t m = mask & ~3U;
            if (!(m & 0xffff0000U)) m |= 0xffff0000U;
            bar->flags = KSHIM_PCI_BAR_IO;
            bar->size = (uint32_t)(~m + 1U);
            bar->bus_addr = orig & ~3U;
        } else if ((orig & 0x6U) == BAR_MEM_TYPE_64 && i + 1 < count) {
            uint16_t hi_reg = (uint16_t)(reg + 4);
            uint32_t hi = rd(d, hi_reg, 4);
            (void)raw_wr(d, hi_reg, 4, 0xffffffffU);
            uint32_t hi_mask = rd(d, hi_reg, 4);
            (void)raw_wr(d, hi_reg, 4, hi);
            uint64_t m = ((uint64_t)hi_mask << 32) | (mask & ~0xfU);
            bar->flags = KSHIM_PCI_BAR_MEM64 | ((orig & BAR_MEM_PREFETCH) ? KSHIM_PCI_BAR_PREFETCH : 0);
            bar->size = ~m + 1U;
            bar->bus_addr = ((uint64_t)hi << 32) | (orig & ~0xfU);
            memset(&d->bars[i + 1], 0, sizeof(d->bars[0]));
            ++i;
        } else {
            bar->flags = (orig & BAR_MEM_PREFETCH) ? KSHIM_PCI_BAR_PREFETCH : 0;
            bar->size = (uint32_t)(~(mask & ~0xfU) + 1U);
            bar->bus_addr = orig & ~0xfU;
        }
        if (!pow2(bar->size)) memset(bar, 0, sizeof(*bar));
    }
    if (cmd & decode) (void)raw_wr(d, KSHIM_PCI_COMMAND, 2, cmd);
}

static struct kshim_pci_dev *probe_function(struct kshim_pci_host *h, struct kshim_pci_dev *bridge,
                                            uint8_t bus, uint8_t devfn, uint32_t id)
{
    if (DevCount == KSHIM_PCI_MAX_DEVICES) {
        LOG("more than %u functions, %02x:%02x.%u ignored\n", KSHIM_PCI_MAX_DEVICES, bus,
            KSHIM_PCI_SLOT(devfn), KSHIM_PCI_FUNC(devfn));
        return NULL;
    }
    struct kshim_pci_dev *d = &Devs[DevCount];
    memset(d, 0, sizeof(*d));
    memset(&Priv[DevCount], 0, sizeof(Priv[0]));
    ++DevCount;
    d->host = h;
    d->bridge = bridge;
    d->bus = bus;
    d->devfn = devfn;
    d->vendor = (uint16_t)id;
    d->device = (uint16_t)(id >> 16);
    uint32_t class_rev = rd(d, KSHIM_PCI_CLASS_REVISION, 4);
    d->class_code = class_rev >> 8;
    d->revision = (uint8_t)class_rev;
    d->header_type = (uint8_t)rd(d, KSHIM_PCI_HEADER_TYPE, 1);
    if ((d->header_type & CFG_HEADER_TYPE_MASK) == 0) {
        uint32_t sub = rd(d, CFG_SUBSYSTEM_VENDOR, 4);
        d->subsystem_vendor = (uint16_t)sub;
        d->subsystem_device = (uint16_t)(sub >> 16);
    }
    d->irq_pin = (uint8_t)rd(d, KSHIM_PCI_INTERRUPT_PIN, 1);
    if (d->irq_pin > 4) d->irq_pin = 0;
    d->cap_pm = find_cap(d, KSHIM_PCI_CAP_ID_PM);
    d->cap_msi = find_cap(d, KSHIM_PCI_CAP_ID_MSI);
    d->cap_msix = find_cap(d, KSHIM_PCI_CAP_ID_MSIX);
    d->cap_exp = find_cap(d, KSHIM_PCI_CAP_ID_EXP);
    if (d->cap_exp) {
        unsigned type = (rd(d, (uint16_t)(d->cap_exp + EXP_FLAGS), 2) >> 4) & 0xfU;
        priv(d)->pcie_port = type == EXP_TYPE_ROOT_PORT || type == EXP_TYPE_DOWNSTREAM;
    }
    d->node = match_node(bridge ? bridge->node : h->node, devfn);
    priv(d)->bme_on_entry = (rd(d, KSHIM_PCI_COMMAND, 2) & KSHIM_PCI_COMMAND_MASTER) != 0;
    size_bars(d);
    return d;
}

/* Depth first, Linux-style: an inherited bridge numbering that is ordered
 * and inside the parent's range is kept, otherwise the next free bus is
 * assigned with the parent's limit as a temporary subordinate. Returns the
 * highest bus number below `bus`. */
static unsigned scan_bus(struct kshim_pci_host *h, struct kshim_pci_dev *bridge, unsigned bus,
                         unsigned limit, unsigned *next)
{
    unsigned max = bus;
    unsigned slots = bridge && priv(bridge)->pcie_port ? 1U : 32U;
    for (unsigned slot = 0; slot < slots; ++slot) {
        bool multi = false;
        for (unsigned fn = 0; fn < 8U; ++fn) {
            if (fn && !multi) break;
            uint8_t devfn = KSHIM_PCI_DEVFN(slot, fn);
            uint32_t id = cfg_rd(h, (uint8_t)bus, devfn, KSHIM_PCI_VENDOR_ID, 4);
            if ((id & 0xffffU) == 0xffffU || (id & 0xffffU) == 0) {
                if (!fn) break;
                continue;
            }
            if (!fn)
                multi = (cfg_rd(h, (uint8_t)bus, devfn, KSHIM_PCI_HEADER_TYPE, 1) &
                         CFG_HEADER_MULTI) != 0;
            struct kshim_pci_dev *d = probe_function(h, bridge, (uint8_t)bus, devfn, id);
            if (!d || (d->header_type & CFG_HEADER_TYPE_MASK) != 1) continue;
            uint32_t numbers = rd(d, CFG_BUS_NUMBERS, 4);
            unsigned pri = numbers & 0xffU, sec = (numbers >> 8) & 0xffU,
                     sub = (numbers >> 16) & 0xffU;
            if (pri == bus && sec > bus && sec >= *next && sub >= sec && sub <= limit) {
                d->secondary_bus = (uint8_t)sec;
                d->subordinate_bus = (uint8_t)sub;
                *next = sec + 1;
                (void)scan_bus(h, d, sec, sub, next);
                *next = sub + 1;
                if (sub > max) max = sub;
                continue;
            }
            if (*next > limit) {
                LOG("%02x:%02x.%u: no bus number left for the bridge\n", bus, slot, fn);
                continue;
            }
            sec = *next;
            *next = sec + 1;
            if (jwr(d, CFG_BUS_NUMBERS, 4,
                    (numbers & 0xff000000U) | (limit << 16) | (sec << 8) | bus)) continue;
            d->secondary_bus = (uint8_t)sec;
            sub = scan_bus(h, d, sec, limit, next);
            (void)jwr(d, CFG_SUBORDINATE_BUS, 1, sub);
            d->subordinate_bus = (uint8_t)sub;
            if (sub > max) max = sub;
        }
    }
    return max;
}

/* ---- resource assignment ----------------------------------------------- */

struct res_ctx {
    struct kshim_pci_aperture io, mem, mem64;
    bool mem64_pref;         /* mem64 only for prefetchable BARs (bridge pref window) */
};

enum { KIND_IO, KIND_MEM };

struct request {
    struct kshim_pci_dev *dev;
    int bar;                 /* 0..5, or WINDOW_IO / WINDOW_MEM */
    uint8_t kind;
    bool placed;
    uint64_t size, align, base;
};
#define WINDOW_IO  (-1)
#define WINDOW_MEM (-2)

struct windows {
    bool io_on, mem_on, pref_on;
    uint64_t io_base, io_limit, mem_base, mem_limit, pref_base, pref_limit;
};

static bool on_bus(const struct kshim_pci_dev *d, struct kshim_pci_host *h,
                   const struct kshim_pci_dev *bridge)
{
    return d->host == h && d->bridge == bridge;
}

static bool is_bridge(const struct kshim_pci_dev *d)
{
    return (d->header_type & CFG_HEADER_TYPE_MASK) == 1 && d->secondary_bus;
}

static int bus_to_cpu(const struct kshim_pci_host *h, bool io, uint64_t bus, uint64_t size,
                      uint64_t *cpu)
{
    for (unsigned i = 0; i < h->window_count; ++i) {
        const struct kshim_pci_window *w = &h->windows[i];
        if ((w->type == KSHIM_PCI_WINDOW_IO) != io) continue;
        if (bus >= w->pci_base && bus - w->pci_base < w->size &&
            size <= w->size - (bus - w->pci_base)) {
            *cpu = w->cpu_base + (bus - w->pci_base);
            return 0;
        }
    }
    return ERROR_NOT_FOUND;
}

static void read_windows(const struct kshim_pci_dev *d, struct windows *w)
{
    memset(w, 0, sizeof(*w));
    uint32_t io = rd(d, CFG_IO_BASE, 2);
    uint32_t iob = io & 0xffU, iol = (io >> 8) & 0xffU;
    if (iob || iol) {
        uint64_t upper_b = 0, upper_l = 0;
        if ((iob & 0xfU) == 1U) {
            uint32_t u = rd(d, CFG_IO_BASE_UPPER, 4);
            upper_b = (uint64_t)(u & 0xffffU) << 16;
            upper_l = (uint64_t)(u >> 16) << 16;
        }
        w->io_base = upper_b | ((uint64_t)(iob & 0xf0U) << 8);
        w->io_limit = upper_l | ((uint64_t)(iol & 0xf0U) << 8) | 0xfffU;
        w->io_on = w->io_limit > w->io_base;
    }
    uint32_t mem = rd(d, CFG_MEM_BASE, 4);
    if (mem) {
        w->mem_base = (uint64_t)(mem & 0xfff0U) << 16;
        w->mem_limit = ((uint64_t)((mem >> 16) & 0xfff0U) << 16) | 0xfffffU;
        w->mem_on = w->mem_limit > w->mem_base;
    }
    uint32_t pref = rd(d, CFG_PREF_BASE, 4);
    if (pref) {
        w->pref_base = (uint64_t)(pref & 0xfff0U) << 16;
        w->pref_limit = ((uint64_t)((pref >> 16) & 0xfff0U) << 16) | 0xfffffU;
        if ((pref & 0xfU) == 1U) {
            w->pref_base |= (uint64_t)rd(d, CFG_PREF_BASE_UPPER, 4) << 32;
            w->pref_limit |= (uint64_t)rd(d, CFG_PREF_LIMIT_UPPER, 4) << 32;
        }
        w->pref_on = w->pref_limit > w->pref_base;
    }
}

static struct kshim_pci_aperture *bar_aperture(struct res_ctx *ctx, const struct kshim_pci_bar *b,
                                               bool second)
{
    if (b->flags & KSHIM_PCI_BAR_IO) return second ? NULL : &ctx->io;
    if (!second) return &ctx->mem;
    if (!ctx->mem64.size) return NULL;
    if (ctx->mem64_pref && !(b->flags & KSHIM_PCI_BAR_PREFETCH)) return NULL;
    bool low = ctx->mem64.base + ctx->mem64.size <= UINT64_C(0x100000000);
    return (b->flags & KSHIM_PCI_BAR_MEM64) || low ? &ctx->mem64 : NULL;
}

static void reset_subtree(struct kshim_pci_dev *bridge)
{
    for (unsigned i = 0; i < DevCount; ++i) {
        struct kshim_pci_dev *d = &Devs[i];
        if (d->bridge != bridge) continue;
        for (unsigned b = 0; b < 6; ++b) {
            d->bars[b].inherited = false;
            d->bars[b].cpu_addr = 0;
        }
        if (is_bridge(d)) reset_subtree(d);
    }
}

static void sort_requests(struct request *r, unsigned n)
{
    for (unsigned i = 1; i < n; ++i) {
        struct request key = r[i];
        unsigned j = i;
        while (j > 0 && (r[j - 1].align < key.align ||
                         (r[j - 1].align == key.align && r[j - 1].size < key.size))) {
            r[j] = r[j - 1];
            --j;
        }
        r[j] = key;
    }
}

/* Packed size of `n` requests sorted by alignment (what assign_fresh uses). */
static uint64_t pack(const struct request *r, unsigned n, uint64_t *align)
{
    uint64_t cursor = 0;
    *align = 1;
    for (unsigned i = 0; i < n; ++i) {
        cursor = align_up(cursor, r[i].align) + r[i].size;
        if (r[i].align > *align) *align = r[i].align;
    }
    return cursor;
}

static unsigned collect(struct kshim_pci_host *h, struct kshim_pci_dev *bridge,
                        struct request *out, bool fresh_only);

/* Window sizes a bridge needs for its whole subtree, assigned afresh. */
static void measure(struct kshim_pci_host *h, struct kshim_pci_dev *bridge, uint64_t *io,
                    uint64_t *mem, uint64_t *mem_align)
{
    struct request req[REQUESTS_MAX], io_req[REQUESTS_MAX], mem_req[REQUESTS_MAX];
    unsigned n = collect(h, bridge, req, false), ni = 0, nm = 0;
    for (unsigned i = 0; i < n; ++i) {
        if (req[i].kind == KIND_IO) io_req[ni++] = req[i];
        else mem_req[nm++] = req[i];
    }
    sort_requests(io_req, ni);
    sort_requests(mem_req, nm);
    uint64_t io_align;
    *io = ni ? align_up(pack(io_req, ni, &io_align), IO_GRANULE) : 0;
    *mem = nm ? align_up(pack(mem_req, nm, mem_align), MEM_GRANULE) : 0;
    if (!nm || *mem_align < MEM_GRANULE) *mem_align = MEM_GRANULE;
}

/* Requests of the devices on `bridge`'s secondary bus (root bus: NULL):
 * every BAR not kept, and the windows of every bridge to (re)assign. */
static unsigned collect(struct kshim_pci_host *h, struct kshim_pci_dev *bridge,
                        struct request *out, bool fresh_only)
{
    unsigned n = 0;
    for (unsigned i = 0; i < DevCount; ++i) {
        struct kshim_pci_dev *d = &Devs[i];
        if (!on_bus(d, h, bridge)) continue;
        for (unsigned b = 0; b < 6 && n < REQUESTS_MAX; ++b) {
            const struct kshim_pci_bar *bar = &d->bars[b];
            if (!bar->size || (fresh_only && bar->inherited)) continue;
            bool io = bar->flags & KSHIM_PCI_BAR_IO;
            out[n++] = (struct request){d, (int)b, io ? KIND_IO : KIND_MEM, false, bar->size,
                                        io && bar->size < 4 ? 4 : bar->size, 0};
        }
        if (!is_bridge(d) || (fresh_only && !priv(d)->fresh)) continue;
        uint64_t io, mem, mem_align;
        measure(h, d, &io, &mem, &mem_align);
        if (io && n < REQUESTS_MAX)
            out[n++] = (struct request){d, WINDOW_IO, KIND_IO, false, io, IO_GRANULE, 0};
        if (mem && n < REQUESTS_MAX)
            out[n++] = (struct request){d, WINDOW_MEM, KIND_MEM, false, mem, mem_align, 0};
    }
    return n;
}

static bool keep_bar(struct kshim_pci_host *h, struct kshim_pci_dev *d, unsigned b,
                     struct res_ctx *ctx)
{
    struct kshim_pci_bar *bar = &d->bars[b];
    uint64_t align = (bar->flags & KSHIM_PCI_BAR_IO) && bar->size < 4 ? 4 : bar->size;
    if (!bar->bus_addr || (bar->bus_addr & (align - 1))) return false;
    for (unsigned second = 0; second < 2; ++second) {
        struct kshim_pci_aperture *a = bar_aperture(ctx, bar, second);
        if (!a || !kshim_pci_aperture_reserve(a, bar->bus_addr, bar->size)) continue;
        uint64_t cpu;
        if (bus_to_cpu(h, bar->flags & KSHIM_PCI_BAR_IO, bar->bus_addr, bar->size, &cpu)) {
            kshim_pci_aperture_release(a, bar->bus_addr, bar->size);
            return false;
        }
        bar->inherited = true;
        bar->cpu_addr = cpu;
        return true;
    }
    return false;
}

static int program_bar(struct kshim_pci_host *h, struct kshim_pci_dev *d, unsigned b,
                       uint64_t addr)
{
    struct kshim_pci_bar *bar = &d->bars[b];
    uint16_t reg = (uint16_t)(KSHIM_PCI_BAR0 + 4U * b);
    uint64_t cpu;
    int status = bus_to_cpu(h, bar->flags & KSHIM_PCI_BAR_IO, addr, bar->size, &cpu);
    if (status) return status;
    status = jwr(d, reg, 4, (uint32_t)addr);
    if (!status && (bar->flags & KSHIM_PCI_BAR_MEM64))
        status = jwr(d, (uint16_t)(reg + 4), 4, (uint32_t)(addr >> 32));
    if (status) return status;
    bar->bus_addr = addr;
    bar->cpu_addr = cpu;
    bar->inherited = false;
    return 0;
}

/* Fresh bridge windows; an empty window is closed (base above limit). */
static int program_windows(struct kshim_pci_dev *d, uint64_t io_base, uint64_t io_size,
                           uint64_t mem_base, uint64_t mem_size, struct windows *w)
{
    memset(w, 0, sizeof(*w));
    int status = 0;
    if (io_size) {
        uint64_t limit = io_base + io_size - 1;
        uint32_t cur = rd(d, CFG_IO_BASE, 1);
        bool io32 = (cur & 0xfU) == 1U;
        if (!io32 && limit > 0xffffU) return KSHIM_IO_ERR_NO_MEMORY;
        status |= jwr_changed(d, CFG_IO_BASE, 2,
                              (uint32_t)(((io_base >> 8) & 0xf0U) | (cur & 0xfU)) |
                              (uint32_t)(((limit >> 8) & 0xf0U) | (cur & 0xfU)) << 8);
        if (io32)
            status |= jwr_changed(d, CFG_IO_BASE_UPPER, 4,
                                  (uint32_t)((io_base >> 16) & 0xffffU) |
                                  (uint32_t)((limit >> 16) & 0xffffU) << 16);
        /* A bridge without an I/O window keeps the registers at zero. */
        uint32_t back = rd(d, CFG_IO_BASE, 2);
        w->io_on = ((back & 0xf0U) << 8) == (io_base & 0xf000U) &&
                   ((back >> 8) & 0xf0U) << 8 == (limit & 0xf000U);
        if (!w->io_on)
            LOG("%02x:%02x.%u: bridge has no I/O window\n", d->bus, KSHIM_PCI_SLOT(d->devfn),
                KSHIM_PCI_FUNC(d->devfn));
        w->io_base = io_base;
        w->io_limit = limit;
    } else if (rd(d, CFG_IO_BASE, 2)) {
        status |= jwr(d, CFG_IO_BASE, 2, 0x00f0U);
    }
    if (mem_size) {
        uint64_t limit = mem_base + mem_size - 1;
        status |= jwr_changed(d, CFG_MEM_BASE, 4,
                              (uint32_t)((mem_base >> 16) & 0xfff0U) |
                              (uint32_t)((limit >> 16) & 0xfff0U) << 16);
        w->mem_on = true;
        w->mem_base = mem_base;
        w->mem_limit = limit;
    } else if (rd(d, CFG_MEM_BASE, 4)) {
        status |= jwr(d, CFG_MEM_BASE, 4, 0x0000fff0U);
    }
    /* The prefetchable window is not used for fresh assignments. */
    uint32_t pref = rd(d, CFG_PREF_BASE, 4);
    if (pref && (pref & 0xfff0fff0U) != 0x0000fff0U) {
        status |= jwr(d, CFG_PREF_BASE, 4, (pref & 0x000f000fU) | 0x0000fff0U);
        if ((pref & 0xfU) == 1U) {
            status |= jwr_changed(d, CFG_PREF_BASE_UPPER, 4, 0);
            status |= jwr_changed(d, CFG_PREF_LIMIT_UPPER, 4, 0);
        }
    }
    uint32_t cmd = rd(d, KSHIM_PCI_COMMAND, 2);
    uint32_t want = cmd | (w->io_on ? KSHIM_PCI_COMMAND_IO : 0) |
                    (w->mem_on ? KSHIM_PCI_COMMAND_MEMORY : 0);
    if (want != cmd) status |= jwr(d, KSHIM_PCI_COMMAND, 2, want);
    return status ? ERROR_DEVICE_ERROR : 0;
}

static void child_ctx(const struct windows *w, struct res_ctx *c)
{
    kshim_pci_aperture_init(&c->io, w->io_base, w->io_on ? w->io_limit - w->io_base + 1 : 0);
    kshim_pci_aperture_init(&c->mem, w->mem_base,
                            w->mem_on ? w->mem_limit - w->mem_base + 1 : 0);
    kshim_pci_aperture_init(&c->mem64, w->pref_base,
                            w->pref_on ? w->pref_limit - w->pref_base + 1 : 0);
    c->mem64_pref = true;
}

static int assign_bus(struct kshim_pci_host *h, struct kshim_pci_dev *bridge,
                      struct res_ctx *ctx, bool keep);

static bool intersects(const struct kshim_pci_aperture *a, uint64_t base, uint64_t size)
{
    return a->size && base <= a->base + (a->size - 1) && a->base <= base + (size - 1);
}

/* Keep an inherited bridge when each open window sits inside the parent's
 * apertures and its whole subtree fits in it. An open window that does not
 * touch any parent aperture (the reset value 0/0 decodes [0, 1 MiB)) claims
 * nothing kShimC uses and is left alone. */
static bool keep_bridge(struct kshim_pci_host *h, struct kshim_pci_dev *d, struct res_ctx *ctx)
{
    struct windows w;
    read_windows(d, &w);
    struct { bool *on; uint64_t base, size; struct kshim_pci_aperture *a; } win[3] = {
        {&w.io_on, w.io_base, w.io_limit - w.io_base + 1, NULL},
        {&w.mem_on, w.mem_base, w.mem_limit - w.mem_base + 1, NULL},
        {&w.pref_on, w.pref_base, w.pref_limit - w.pref_base + 1, NULL},
    };
    struct kshim_pci_aperture *candidates[3][2] = {
        {&ctx->io, NULL}, {&ctx->mem, NULL}, {&ctx->mem, &ctx->mem64}};
    bool ok = true;
    for (unsigned i = 0; i < 3 && ok; ++i) {
        if (!*win[i].on) continue;
        bool touches = false;
        for (unsigned k = 0; k < 2 && candidates[i][k] && !win[i].a; ++k) {
            struct kshim_pci_aperture *a = candidates[i][k];
            touches |= intersects(a, win[i].base, win[i].size);
            if (kshim_pci_aperture_reserve(a, win[i].base, win[i].size)) win[i].a = a;
        }
        if (!win[i].a && touches) ok = false;
        if (!win[i].a) *win[i].on = false; /* outside every aperture: ignored */
    }
    if (ok && (w.io_on || w.mem_on || w.pref_on)) {
        struct res_ctx child;
        child_ctx(&w, &child);
        if (!assign_bus(h, d, &child, true)) return true;
        reset_subtree(d);
    }
    for (unsigned i = 0; i < 3; ++i)
        if (win[i].a) kshim_pci_aperture_release(win[i].a, win[i].base, win[i].size);
    return false;
}

static int assign_bus(struct kshim_pci_host *h, struct kshim_pci_dev *bridge,
                      struct res_ctx *ctx, bool keep)
{
    int result = 0;
    for (unsigned i = 0; i < DevCount; ++i) {
        struct kshim_pci_dev *d = &Devs[i];
        if (!on_bus(d, h, bridge)) continue;
        for (unsigned b = 0; b < 6; ++b)
            if (d->bars[b].size && keep) (void)keep_bar(h, d, b, ctx);
    }
    for (unsigned i = 0; i < DevCount; ++i) {
        struct kshim_pci_dev *d = &Devs[i];
        if (!on_bus(d, h, bridge) || !is_bridge(d)) continue;
        priv(d)->fresh = !(keep && keep_bridge(h, d, ctx));
    }
    struct request req[REQUESTS_MAX];
    unsigned n = collect(h, bridge, req, true);
    sort_requests(req, n);
    for (unsigned i = 0; i < n; ++i) {
        struct request *r = &req[i];
        uint64_t floor = r->kind == KIND_IO ? IO_FLOOR : 0;
        if (r->bar >= 0) {
            const struct kshim_pci_bar *bar = &r->dev->bars[r->bar];
            for (unsigned second = 0; second < 2 && !r->placed; ++second) {
                struct kshim_pci_aperture *a = bar_aperture(ctx, bar, second);
                r->placed = a && kshim_pci_aperture_alloc(a, r->size, r->align, floor, &r->base);
            }
            if (r->placed && program_bar(h, r->dev, (unsigned)r->bar, r->base)) r->placed = false;
        } else {
            struct kshim_pci_aperture *a = r->kind == KIND_IO ? &ctx->io : &ctx->mem;
            r->placed = kshim_pci_aperture_alloc(a, r->size, r->align, floor, &r->base);
        }
        if (!r->placed) {
            /* A kept bridge that overflows is retried afresh: no noise then. */
            if (!keep || !bridge)
                LOG("%02x:%02x.%u: no space for %s %s (0x%llx bytes)\n", r->dev->bus,
                    KSHIM_PCI_SLOT(r->dev->devfn), KSHIM_PCI_FUNC(r->dev->devfn),
                    r->bar >= 0 ? "BAR" : "window", r->kind == KIND_IO ? "I/O" : "memory",
                    (unsigned long long)r->size);
            result = KSHIM_IO_ERR_NO_MEMORY;
        }
    }
    /* Fresh bridges: program both windows, then their subtree. */
    for (unsigned i = 0; i < DevCount; ++i) {
        struct kshim_pci_dev *d = &Devs[i];
        if (!on_bus(d, h, bridge) || !is_bridge(d) || !priv(d)->fresh) continue;
        uint64_t io_base = 0, io_size = 0, mem_base = 0, mem_size = 0;
        for (unsigned k = 0; k < n; ++k) {
            if (req[k].dev != d || req[k].bar >= 0 || !req[k].placed) continue;
            if (req[k].bar == WINDOW_IO) { io_base = req[k].base; io_size = req[k].size; }
            else { mem_base = req[k].base; mem_size = req[k].size; }
        }
        struct windows w;
        if (program_windows(d, io_base, io_size, mem_base, mem_size, &w)) {
            result = ERROR_DEVICE_ERROR;
            continue;
        }
        struct res_ctx child;
        child_ctx(&w, &child);
        int status = assign_bus(h, d, &child, false);
        if (status && !result) result = status;
    }
    return result;
}

static void assign_host(struct kshim_pci_host *h)
{
    struct res_ctx root;
    memset(&root, 0, sizeof(root));
    for (unsigned i = 0; i < h->window_count; ++i) {
        const struct kshim_pci_window *w = &h->windows[i];
        struct kshim_pci_aperture *a =
            w->type == KSHIM_PCI_WINDOW_IO ? &root.io :
            w->type == KSHIM_PCI_WINDOW_MEM32 && !w->prefetchable ? &root.mem : &root.mem64;
        if (a->size) continue; /* first window of each kind */
        kshim_pci_aperture_init(a, w->pci_base, w->size);
        if (a == &root.mem64) root.mem64_pref = w->prefetchable;
    }
    if (!root.mem.size && root.mem64.size && !root.mem64_pref &&
        root.mem64.base + root.mem64.size <= UINT64_C(0x100000000)) {
        root.mem = root.mem64;
        kshim_pci_aperture_init(&root.mem64, 0, 0);
    }
    (void)assign_bus(h, NULL, &root, true);
}

/* ---- INTx -------------------------------------------------------------- */

static void resolve_intx(struct kshim_pci_dev *dev)
{
    if (!dev->irq_pin || !Fdt) return;
    uint8_t pin = dev->irq_pin;
    const struct kshim_pci_dev *d = dev;
    while (d->bridge) {
        pin = (uint8_t)(((pin - 1U + KSHIM_PCI_SLOT(d->devfn)) & 3U) + 1U);
        d = d->bridge;
    }
    uint32_t intid;
    unsigned trigger = KSHIM_IRQ_TYPE_LEVEL_HIGH;
    uint32_t unit = ((uint32_t)d->bus << 16) | ((uint32_t)d->devfn << 8);
    if (kshim_pci_dt_intx(Fdt, dev->host->node, unit, pin, &intid, &trigger)) return;
    dev->intx_intid = intid;
    dev->intx_trigger = trigger;
}

static void intx_dispatch(uint32_t intid, void *context)
{
    unsigned line = (unsigned)(uintptr_t)context;
    for (unsigned i = 0; i < DevCount; ++i) {
        struct dev_priv *p = &Priv[i];
        if (p->handler && !Devs[i].msi && p->line == line) p->handler(intid, p->context);
    }
}

int kshim_pci_request_irq(struct kshim_pci_dev *dev, unsigned allowed,
                          kshim_irq_handler_t handler, void *context, uint32_t *intid)
{
    if (Quiesced) return KSHIM_IO_ERR_QUIESCED;
    if (!valid_dev(dev) || !handler || !allowed) return ERROR_INVALID_PARAMETER;
    if (dev->irq_requested) return ERROR_BUSY;
    struct dev_priv *p = priv(dev);
    const struct kshim_pci_host_ops *ops = dev->host->ops;
    if ((allowed & KSHIM_PCI_IRQ_MSI) && dev->cap_msi && ops->msi_setup) {
        uint64_t address;
        uint32_t data;
        if (!ops->msi_setup(dev->host, dev, handler, context, &address, &data)) {
            uint16_t cap = dev->cap_msi;
            uint32_t ctrl = rd(dev, (uint16_t)(cap + 2), 2);
            int status = jwr(dev, (uint16_t)(cap + 4), 4, (uint32_t)address);
            if (ctrl & MSI_CTRL_64BIT) {
                status |= jwr(dev, (uint16_t)(cap + 8), 4, (uint32_t)(address >> 32));
                status |= jwr(dev, (uint16_t)(cap + 12), 2, data & 0xffffU);
            } else {
                status |= jwr(dev, (uint16_t)(cap + 8), 2, data & 0xffffU);
            }
            status |= jwr(dev, (uint16_t)(cap + 2), 2,
                          (ctrl & ~MSI_CTRL_MME_MASK) | MSI_CTRL_ENABLE);
            uint32_t cmd = rd(dev, KSHIM_PCI_COMMAND, 2);
            status |= jwr_changed(dev, KSHIM_PCI_COMMAND, 2, cmd | KSHIM_PCI_COMMAND_INTX_DISABLE);
            if (!status) {
                dev->msi = true;
                dev->irq_requested = true;
                if (intid) *intid = 0;
                return 0;
            }
            if (ops->msi_teardown) ops->msi_teardown(dev->host, dev);
        }
    }
    if (!(allowed & KSHIM_PCI_IRQ_INTX) || !dev->intx_intid) return ERROR_NOT_SUPPORTED;
    unsigned line = INTX_LINES, free_line = INTX_LINES;
    for (unsigned i = 0; i < INTX_LINES; ++i) {
        if (Lines[i].users && Lines[i].intid == dev->intx_intid) line = i;
        if (!Lines[i].users && free_line == INTX_LINES) free_line = i;
    }
    if (line == INTX_LINES) {
        if (free_line == INTX_LINES) return ERROR_NOT_SUPPORTED;
        line = free_line;
        unsigned trigger = dev->intx_trigger ? dev->intx_trigger : KSHIM_IRQ_TYPE_LEVEL_HIGH;
        if (kshim_irq_request(dev->intx_intid, trigger, intx_dispatch,
                              (void *)(uintptr_t)line))
            return ERROR_NOT_SUPPORTED;
        Lines[line] = (struct intx_line){dev->intx_intid, trigger, 0};
    }
    unsigned long flags = kshim_irq_save();
    p->handler = handler;
    p->context = context;
    p->line = line;
    ++Lines[line].users;
    kshim_irq_restore(flags);
    uint32_t cmd = rd(dev, KSHIM_PCI_COMMAND, 2);
    if (cmd & KSHIM_PCI_COMMAND_INTX_DISABLE)
        (void)jwr(dev, KSHIM_PCI_COMMAND, 2, cmd & ~KSHIM_PCI_COMMAND_INTX_DISABLE);
    dev->irq_requested = true;
    if (intid) *intid = dev->intx_intid;
    return 0;
}

static void free_irq(struct kshim_pci_dev *dev)
{
    struct dev_priv *p = priv(dev);
    if (!dev->irq_requested) return;
    if (dev->msi) {
        uint16_t cap = dev->cap_msi;
        uint32_t ctrl = rd(dev, (uint16_t)(cap + 2), 2);
        (void)jwr_changed(dev, (uint16_t)(cap + 2), 2, ctrl & ~MSI_CTRL_ENABLE);
        if (dev->host->ops->msi_teardown) dev->host->ops->msi_teardown(dev->host, dev);
        dev->msi = false;
    } else {
        uint32_t cmd = rd(dev, KSHIM_PCI_COMMAND, 2);
        /* Silence the pin first, so a level line cannot storm meanwhile. */
        (void)jwr_changed(dev, KSHIM_PCI_COMMAND, 2, cmd | KSHIM_PCI_COMMAND_INTX_DISABLE);
        unsigned line = p->line;
        unsigned long flags = kshim_irq_save();
        p->handler = NULL;
        p->context = NULL;
        bool last = --Lines[line].users == 0;
        kshim_irq_restore(flags);
        if (last) (void)kshim_irq_free(Lines[line].intid);
    }
    dev->irq_requested = false;
}

int kshim_pci_free_irq(struct kshim_pci_dev *dev)
{
    if (Quiesced) return KSHIM_IO_ERR_QUIESCED;
    if (!valid_dev(dev)) return ERROR_INVALID_PARAMETER;
    free_irq(dev);
    return 0;
}

/* ---- device services --------------------------------------------------- */

int kshim_pci_enable_device(struct kshim_pci_dev *dev, bool bus_master)
{
    if (Quiesced) return KSHIM_IO_ERR_QUIESCED;
    if (!valid_dev(dev)) return ERROR_INVALID_PARAMETER;
    uint32_t want = 0;
    for (unsigned b = 0; b < 6; ++b) {
        const struct kshim_pci_bar *bar = &dev->bars[b];
        if (!bar->size || !bar->cpu_addr) continue;
        want |= bar->flags & KSHIM_PCI_BAR_IO ? KSHIM_PCI_COMMAND_IO : KSHIM_PCI_COMMAND_MEMORY;
    }
    if (bus_master) want |= KSHIM_PCI_COMMAND_MASTER;
    int status = 0;
    for (struct kshim_pci_dev *d = dev; d; d = d->bridge) {
        uint32_t cmd = rd(d, KSHIM_PCI_COMMAND, 2);
        if ((cmd & want) != want) status |= jwr(d, KSHIM_PCI_COMMAND, 2, cmd | want);
    }
    if (status) return ERROR_DEVICE_ERROR;
    dev->enabled = true;
    return 0;
}

volatile void *kshim_pci_bar_map(struct kshim_pci_dev *dev, unsigned bar)
{
    if (!valid_dev(dev) || Quiesced || bar >= 6) return NULL;
    const struct kshim_pci_bar *b = &dev->bars[bar];
    if (!b->size || !b->cpu_addr || (b->flags & KSHIM_PCI_BAR_IO)) return NULL;
    return (volatile void *)(uintptr_t)b->cpu_addr;
}

uint64_t kshim_pci_dma_addr(const struct kshim_pci_dev *dev, uint64_t bus)
{
    (void)dev;
    return bus; /* no "dma-ranges" on the supported hosts: identity */
}

struct kshim_pci_dev *kshim_pci_find_device(uint16_t vendor, uint16_t device,
                                            struct kshim_pci_dev *from)
{
    unsigned start = valid_dev(from) ? (unsigned)(from - Devs) + 1U : 0U;
    for (unsigned i = start; i < DevCount; ++i)
        if ((vendor == KSHIM_PCI_ANY_ID || Devs[i].vendor == vendor) &&
            (device == KSHIM_PCI_ANY_ID || Devs[i].device == device)) return &Devs[i];
    return NULL;
}

struct kshim_pci_dev *kshim_pci_find_class(uint32_t class_code, uint32_t class_mask,
                                           struct kshim_pci_dev *from)
{
    unsigned start = valid_dev(from) ? (unsigned)(from - Devs) + 1U : 0U;
    for (unsigned i = start; i < DevCount; ++i)
        if ((Devs[i].class_code & class_mask) == (class_code & class_mask)) return &Devs[i];
    return NULL;
}

/* ---- drivers ----------------------------------------------------------- */

static bool id_end(const struct kshim_pci_device_id *id)
{
    return !id->vendor && !id->device && !id->class_code && !id->class_mask;
}

static const struct kshim_pci_device_id *match(const struct kshim_pci_driver *drv,
                                               const struct kshim_pci_dev *d)
{
    for (const struct kshim_pci_device_id *id = drv->ids; id && !id_end(id); ++id) {
        if (id->vendor != KSHIM_PCI_ANY_ID && id->vendor != d->vendor) continue;
        if (id->device != KSHIM_PCI_ANY_ID && id->device != d->device) continue;
        if ((d->class_code & id->class_mask) != (id->class_code & id->class_mask)) continue;
        return id;
    }
    return NULL;
}

static void bind(struct kshim_pci_dev *d)
{
    if (d->driver) return;
    for (struct kshim_pci_driver *drv = Drivers; drv; drv = drv->next) {
        const struct kshim_pci_device_id *id = match(drv, d);
        if (!id || !drv->probe) continue;
        d->driver = drv;
        int status = drv->probe(d, id);
        if (!status) {
            LOG("%04x:%02x:%02x.%u bound to %s\n", d->host->domain, d->bus,
                KSHIM_PCI_SLOT(d->devfn), KSHIM_PCI_FUNC(d->devfn), drv->name);
            return;
        }
        d->driver = NULL;
        d->driver_data = NULL;
        if (d->irq_requested) free_irq(d);
    }
}

int kshim_pci_driver_register(struct kshim_pci_driver *driver)
{
    if (!driver || !driver->ids) return ERROR_INVALID_PARAMETER;
    if (Quiesced) return KSHIM_IO_ERR_QUIESCED;
    struct kshim_pci_driver **tail = &Drivers;
    for (; *tail; tail = &(*tail)->next)
        if (*tail == driver) return KSHIM_IO_ERR_EXISTS;
    driver->next = NULL;
    *tail = driver;
    if (Initialized)
        for (unsigned i = 0; i < DevCount; ++i) bind(&Devs[i]);
    return 0;
}

/* ---- hosts ------------------------------------------------------------- */

int kshim_pci_host_register(struct kshim_pci_host *host)
{
    if (!host || !host->ops || !host->ops->read || !host->ops->write)
        return ERROR_INVALID_PARAMETER;
    if (Quiesced) return KSHIM_IO_ERR_QUIESCED;
    if (HostCount == KSHIM_PCI_MAX_HOSTS) return KSHIM_IO_ERR_NO_MEMORY;
    struct kshim_pci_host **tail = &Hosts;
    for (; *tail; tail = &(*tail)->next)
        if (*tail == host || (*tail)->node == host->node) return KSHIM_IO_ERR_EXISTS;
    if (host->domain == 0xffffU) host->domain = (uint16_t)HostCount;
    host->next = NULL;
    host->started = host->quiesced = false;
    *tail = host;
    ++HostCount;
    return 0;
}

static bool has_supply(int node)
{
    int offset;
    fdt_for_each_property_offset(offset, Fdt, node) {
        const char *name;
        if (!fdt_getprop_by_offset(Fdt, offset, &name, NULL) || !name) continue;
        size_t n = strlen(name);
        if (n > 7 && !strcmp(name + n - 7, "-supply")) return true;
    }
    return false;
}

/* Endpoints described under the host's root ports get their supplies and
 * pinctrl "default" before the link is started (the Qualcomm host holds
 * PERST asserted meanwhile on a cold start). Inherited-on rails and pads
 * are not written by those services. */
static void power_endpoints(struct kshim_pci_host *h)
{
#if !CONFIG_KSHIM_REGULATOR
    (void)has_supply;
#endif
#if CONFIG_KSHIM_REGULATOR || CONFIG_KSHIM_GPIO
    int depth = 0;
    for (int n = fdt_next_node(Fdt, h->node, &depth); n >= 0 && depth > 0;
         n = fdt_next_node(Fdt, n, &depth)) {
        if (!dt_available(Fdt, n)) continue;
#if CONFIG_KSHIM_REGULATOR
        if (has_supply(n)) {
            int status = kshim_regulator_bulk_enable(Fdt, n);
            if (status) LOG("%s: endpoint supplies: %s\n", fdt_get_name(Fdt, n, NULL),
                            kshim_io_strerror(status));
            else if (PowerCount < POWER_NODES) PowerNodes[PowerCount++] = n;
            else (void)kshim_regulator_bulk_disable(Fdt, n);
        }
#endif
#if CONFIG_KSHIM_GPIO
        if (fdt_getprop(Fdt, n, "pinctrl-0", NULL)) {
            int status = kshim_pinctrl_apply(Fdt, n, "default");
            if (status && status != ERROR_NOT_FOUND)
                LOG("%s: endpoint pinctrl: %s\n", fdt_get_name(Fdt, n, NULL),
                    kshim_io_strerror(status));
        }
#endif
    }
#else
    (void)h;
#endif
}

static void log_device(const struct kshim_pci_dev *d)
{
    LOG("%04x:%02x:%02x.%u [%04x:%04x] class 0x%06x%s", d->host->domain, d->bus,
        KSHIM_PCI_SLOT(d->devfn), KSHIM_PCI_FUNC(d->devfn), d->vendor, d->device,
        (unsigned)d->class_code, is_bridge(d) ? " bridge" : "");
    if (is_bridge(d)) printf(" buses %02x-%02x", d->secondary_bus, d->subordinate_bus);
    for (unsigned b = 0; b < 6; ++b) {
        const struct kshim_pci_bar *bar = &d->bars[b];
        if (!bar->size) continue;
        printf(" BAR%u %s%s 0x%llx+0x%llx%s", b, bar->flags & KSHIM_PCI_BAR_IO ? "io" : "mem",
               bar->flags & KSHIM_PCI_BAR_MEM64 ? "64" : "",
               (unsigned long long)bar->bus_addr, (unsigned long long)bar->size,
               !bar->cpu_addr ? " unassigned" : bar->inherited ? " inherited" : "");
    }
    if (d->irq_pin)
        printf(" INT%c->%u", 'A' + d->irq_pin - 1, (unsigned)d->intx_intid);
    printf("\n");
}

static void bring_up(struct kshim_pci_host *h)
{
    power_endpoints(h);
    int status = h->ops->start ? h->ops->start(h) : 0;
    if (status == ERROR_NOT_READY) {
        LOG("%s: link down, root bus only\n", h->name ? h->name : "host");
    } else if (status) {
        LOG("%s: start failed: %s\n", h->name ? h->name : "host", kshim_io_strerror(status));
        return;
    }
    h->started = true;
    unsigned first = DevCount, next = h->bus_start + 1U;
    (void)scan_bus(h, NULL, h->bus_start, h->bus_end, &next);
    assign_host(h);
    for (unsigned i = first; i < DevCount; ++i) {
        resolve_intx(&Devs[i]);
        log_device(&Devs[i]);
    }
}

int kshim_pci_init(const void *fdt)
{
    if (!fdt) return ERROR_INVALID_PARAMETER;
    if (Quiesced) return KSHIM_IO_ERR_QUIESCED;
    if (Initialized) return 0;
    Fdt = fdt;
#if CONFIG_KSHIM_PCI_SELFTEST
    (void)kshim_pci_selftest_register();
#endif
#if CONFIG_KSHIM_PCI_ECAM
    (void)kshim_pci_ecam_probe(fdt);
#endif
#if CONFIG_KSHIM_PCI_QCOM
    (void)kshim_pci_qcom_probe(fdt);
#endif
    for (struct kshim_pci_host *h = Hosts; h; h = h->next) bring_up(h);
    Initialized = true;
    for (unsigned i = 0; i < DevCount; ++i) bind(&Devs[i]);
    if (!Hosts) LOG("no host bridge\n");
    return 0;
}

/* ---- handoff ----------------------------------------------------------- */

/* Devices are stopped once (drivers, IRQs, bus-master fence, journal);
 * hosts and endpoint power are retried on a later call until they succeed.
 * A failed driver quiesce is sticky: the handoff stays refused. */
static bool DevicesQuiesced;
static int DeviceResult;

static void quiesce_devices(void)
{
    /* 1. Drivers, newest binding first (reverse enumeration order). */
    for (unsigned i = DevCount; i-- > 0;) {
        struct kshim_pci_dev *d = &Devs[i];
        if (!d->driver || !d->driver->quiesce) continue;
        int status = d->driver->quiesce(d);
        if (status) {
            LOG("%02x:%02x.%u: %s quiesce failed: %s\n", d->bus, KSHIM_PCI_SLOT(d->devfn),
                KSHIM_PCI_FUNC(d->devfn), d->driver->name, kshim_io_strerror(status));
            if (!DeviceResult) DeviceResult = status;
        }
    }
    /* 2. Interrupts (INTx disabled at the function, GIC lines freed). */
    for (unsigned i = 0; i < DevCount; ++i) free_irq(&Devs[i]);
    /* 3. DMA fence: bus mastering off everywhere, deepest first. */
    for (unsigned i = DevCount; i-- > 0;) {
        uint32_t cmd = rd(&Devs[i], KSHIM_PCI_COMMAND, 2);
        if (cmd & KSHIM_PCI_COMMAND_MASTER)
            (void)raw_wr(&Devs[i], KSHIM_PCI_COMMAND, 2, cmd & ~KSHIM_PCI_COMMAND_MASTER);
    }
    /* 4. Inherited configuration, newest first, then bus mastering exactly
     * as found at entry. After a failed driver quiesce it stays off. */
    journal_replay();
    for (unsigned i = 0; i < DevCount; ++i) {
        uint32_t cmd = rd(&Devs[i], KSHIM_PCI_COMMAND, 2);
        bool on = Priv[i].bme_on_entry && !DeviceResult;
        bool now = (cmd & KSHIM_PCI_COMMAND_MASTER) != 0;
        if (on != now)
            (void)raw_wr(&Devs[i], KSHIM_PCI_COMMAND, 2,
                         on ? cmd | KSHIM_PCI_COMMAND_MASTER : cmd & ~KSHIM_PCI_COMMAND_MASTER);
    }
}

int kshim_pci_quiesce(void)
{
    Quiesced = true;
    if (!DevicesQuiesced) {
        DevicesQuiesced = true;
        quiesce_devices();
    }
    int result = DeviceResult;
    /* 5. Hosts (Qualcomm cold start: LTSSM off, PERST asserted, resources
     * released through PcieShutdownPort). */
    bool hosts_done = true;
    for (struct kshim_pci_host *h = Hosts; h; h = h->next) {
        if (h->quiesced) continue;
        int status = h->ops->quiesce ? h->ops->quiesce(h) : 0;
        if (status) {
            LOG("%s: quiesce failed: %s\n", h->name ? h->name : "host", kshim_io_strerror(status));
            if (!result) result = status;
            hosts_done = false;
            continue;
        }
        h->quiesced = true;
    }
    /* 6. Endpoint power, once PERST is asserted: each supply reference is
     * dropped, so rails return to their inherited state. */
#if CONFIG_KSHIM_REGULATOR
    while (hosts_done && PowerCount) {
        int status = kshim_regulator_bulk_disable(Fdt, PowerNodes[PowerCount - 1]);
        if (status) {
            if (!result) result = status;
            break;
        }
        --PowerCount;
    }
#else
    (void)hosts_done;
#endif
    return result;
}
