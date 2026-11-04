/* SPDX-License-Identifier: MIT */
/*
 * TEST ONLY (CONFIG_KSHIM_USB_XHCI_TEST_PCI): a minimal kshim/pci.h for the
 * xHCI QEMU test while the PCI core (owner pci) is developed in parallel.
 *
 * It drives one "pci-host-ecam-generic" bridge (QEMU virt gpex): bus 0 only,
 * no bridges, memory BARs from the first 32-bit "ranges" window, INTx from
 * "interrupt-map" (no MSI: kShimC does not drive the GICv3 ITS), journaled
 * config writes replayed at quiesce. Every definition is weak, so the real
 * PCI core replaces all of it when both are linked; this file is not a
 * PCI implementation for hardware.
 *
 * Facts: PCI Local Bus 3.0 configuration header (vendor 0x00, command 0x04,
 * class 0x08, header type 0x0e, BARs 0x10-0x24, capabilities 0x34,
 * interrupt pin 0x3d), PCIe ECAM layout (bus << 20 | devfn << 12 | reg),
 * IEEE 1275 PCI bus binding ("ranges" phys.hi space code bits 25:24,
 * "interrupt-map"/"interrupt-map-mask").
 */
#include <kshim/pci.h>
#include <kshim/io.h>
#include <kshim/irq.h>

#include <config.h>
#include <dt.h>
#include <qcom_dt.h>
#include <stdio.h>
#include <string.h>

#define WEAK __attribute__((weak))
#define TP_JOURNAL_MAX 64U
#define TP_IRQ_SLOTS 8U

struct tp_irq {
    struct kshim_pci_dev *dev;
    kshim_irq_handler_t handler;
    void *context;
    uint32_t intid;
};

static struct {
    const void *fdt;
    int node;
    uint64_t ecam, ecam_size;
    uint64_t mem_cpu, mem_pci, mem_size, mem_next;
    struct kshim_pci_host host;
    struct kshim_pci_dev devs[KSHIM_PCI_MAX_DEVICES];
    unsigned count;
    struct kshim_pci_driver *drivers;
    struct { struct kshim_pci_dev *dev; uint16_t reg; uint32_t value; } journal[TP_JOURNAL_MAX];
    unsigned journal_count;
    struct tp_irq irqs[TP_IRQ_SLOTS];
    bool initialized, quiesced;
} Tp;

/* ---- DT --------------------------------------------------------------------- */

static int tp_find(const void *fdt)
{
    for (int n = fdt_node_offset_by_compatible(fdt, -1, "pci-host-ecam-generic"); n >= 0;
         n = fdt_node_offset_by_compatible(fdt, n, "pci-host-ecam-generic"))
        if (dt_available(fdt, n)) return n;
    return -1;
}

static uint64_t cells(const fdt32_t *p, int count)
{
    uint64_t v = 0;
    for (int i = 0; i < count; ++i) v = v << 32 | fdt32_to_cpu(p[i]);
    return v;
}

static int tp_parse(const void *fdt)
{
    int node = tp_find(fdt);
    if (node < 0) return ERROR_NOT_FOUND;
    struct dt_range reg;
    if (dt_reg(fdt, node, 0, &reg)) return ERROR_NOT_FOUND;
    Tp.fdt = fdt;
    Tp.node = node;
    Tp.ecam = reg.base;
    Tp.ecam_size = reg.size;
    int parent = fdt_parent_offset(fdt, node);
    int pac = fdt_address_cells(fdt, parent), sc = fdt_size_cells(fdt, node);
    int len = 0;
    const fdt32_t *ranges = fdt_getprop(fdt, node, "ranges", &len);
    int entry = 3 + pac + sc;
    for (int i = 0; ranges && pac > 0 && sc > 0 && (i + 1) * entry * 4 <= len; ++i) {
        const fdt32_t *r = ranges + i * entry;
        uint32_t hi = fdt32_to_cpu(r[0]);
        if (((hi >> 24) & 3U) != 2U) continue; /* 32-bit memory space */
        Tp.mem_pci = cells(r + 1, 2);
        Tp.mem_cpu = cells(r + 3, pac);
        Tp.mem_size = cells(r + 3 + pac, sc);
        Tp.mem_next = Tp.mem_pci;
        break;
    }
    return Tp.mem_size ? 0 : ERROR_NOT_FOUND;
}

WEAK int kshim_pci_map_resources(struct kshim_resources *resources)
{
    if (!resources || !resources->fdt || tp_parse(resources->fdt)) return 0;
    /* Bus 0 configuration space and the 32-bit memory window. */
    if (kshim_resource_map(resources, Tp.ecam, 1U << 20, KSHIM_MMU_DEVICE, KSHIM_MMU_READ | KSHIM_MMU_WRITE) ||
        kshim_resource_map(resources, Tp.mem_cpu, Tp.mem_size, KSHIM_MMU_DEVICE, KSHIM_MMU_READ | KSHIM_MMU_WRITE))
        return ERROR_NOT_SUPPORTED;
    return 0;
}

/* ---- Config space ------------------------------------------------------------ */

static volatile uint8_t *cfg_addr(uint8_t bus, uint8_t devfn, uint16_t reg)
{
    return (volatile uint8_t *)(uintptr_t)(Tp.ecam + ((uint64_t)bus << 20) + ((uint64_t)devfn << 12) + reg);
}

static int ecam_read(struct kshim_pci_host *host, uint8_t bus, uint8_t devfn, uint16_t reg,
                     unsigned width, uint32_t *value)
{
    (void)host;
    if (bus || reg >= 0x1000 || (reg & (width - 1U))) return ERROR_INVALID_PARAMETER;
    volatile uint8_t *a = cfg_addr(bus, devfn, reg);
    *value = width == 4 ? *(volatile uint32_t *)a : width == 2 ? *(volatile uint16_t *)a : *a;
    return 0;
}

static int ecam_write(struct kshim_pci_host *host, uint8_t bus, uint8_t devfn, uint16_t reg,
                      unsigned width, uint32_t value)
{
    (void)host;
    if (bus || reg >= 0x1000 || (reg & (width - 1U))) return ERROR_INVALID_PARAMETER;
    volatile uint8_t *a = cfg_addr(bus, devfn, reg);
    if (width == 4) *(volatile uint32_t *)a = value;
    else if (width == 2) *(volatile uint16_t *)a = (uint16_t)value;
    else *a = (uint8_t)value;
    return 0;
}

static const struct kshim_pci_host_ops TpOps = {.read = ecam_read, .write = ecam_write};

static void journal(struct kshim_pci_dev *dev, uint16_t reg)
{
    reg &= (uint16_t)~3U;
    for (unsigned i = 0; i < Tp.journal_count; ++i)
        if (Tp.journal[i].dev == dev && Tp.journal[i].reg == reg) return;
    if (Tp.journal_count >= TP_JOURNAL_MAX) return;
    uint32_t value = 0;
    (void)ecam_read(&Tp.host, dev->bus, dev->devfn, reg, 4, &value);
    Tp.journal[Tp.journal_count].dev = dev;
    Tp.journal[Tp.journal_count].reg = reg;
    Tp.journal[Tp.journal_count].value = value;
    ++Tp.journal_count;
}

WEAK int kshim_pci_read_config8(struct kshim_pci_dev *dev, uint16_t reg, uint8_t *value)
{
    uint32_t v = 0;
    int status = ecam_read(&Tp.host, dev->bus, dev->devfn, reg, 1, &v);
    *value = (uint8_t)v;
    return status;
}
WEAK int kshim_pci_read_config16(struct kshim_pci_dev *dev, uint16_t reg, uint16_t *value)
{
    uint32_t v = 0;
    int status = ecam_read(&Tp.host, dev->bus, dev->devfn, reg, 2, &v);
    *value = (uint16_t)v;
    return status;
}
WEAK int kshim_pci_read_config32(struct kshim_pci_dev *dev, uint16_t reg, uint32_t *value)
{
    return ecam_read(&Tp.host, dev->bus, dev->devfn, reg, 4, value);
}
WEAK int kshim_pci_write_config8(struct kshim_pci_dev *dev, uint16_t reg, uint8_t value)
{
    journal(dev, reg);
    return ecam_write(&Tp.host, dev->bus, dev->devfn, reg, 1, value);
}
WEAK int kshim_pci_write_config16(struct kshim_pci_dev *dev, uint16_t reg, uint16_t value)
{
    journal(dev, reg);
    return ecam_write(&Tp.host, dev->bus, dev->devfn, reg, 2, value);
}
WEAK int kshim_pci_write_config32(struct kshim_pci_dev *dev, uint16_t reg, uint32_t value)
{
    journal(dev, reg);
    return ecam_write(&Tp.host, dev->bus, dev->devfn, reg, 4, value);
}
WEAK int kshim_pci_write_config_raw32(struct kshim_pci_dev *dev, uint16_t reg, uint32_t value)
{
    return ecam_write(&Tp.host, dev->bus, dev->devfn, reg, 4, value);
}

WEAK uint16_t kshim_pci_find_capability(struct kshim_pci_dev *dev, uint8_t id)
{
    uint16_t status = 0;
    uint8_t ptr = 0;
    if (kshim_pci_read_config16(dev, KSHIM_PCI_STATUS, &status) || !(status & 0x10U)) return 0;
    (void)kshim_pci_read_config8(dev, 0x34, &ptr);
    for (unsigned guard = 0; ptr >= 0x40 && guard < 48; ++guard) {
        uint8_t cap = 0, next = 0;
        ptr &= (uint8_t)~3U;
        (void)kshim_pci_read_config8(dev, ptr, &cap);
        if (cap == id) return ptr;
        (void)kshim_pci_read_config8(dev, (uint16_t)(ptr + 1), &next);
        ptr = next;
    }
    return 0;
}

WEAK uint16_t kshim_pci_find_ext_capability(struct kshim_pci_dev *dev, uint16_t id)
{
    uint16_t offset = 0x100;
    for (unsigned guard = 0; offset && guard < 96; ++guard) {
        uint32_t header = 0;
        if (kshim_pci_read_config32(dev, offset, &header) || !header || header == UINT32_MAX) return 0;
        if ((header & 0xffffU) == id) return offset;
        offset = (uint16_t)((header >> 20) & 0xffcU);
        if (offset < 0x100) return 0;
    }
    return 0;
}

/* ---- Enumeration ------------------------------------------------------------- */

static void resolve_intx(struct kshim_pci_dev *dev)
{
    int len = 0, mask_len = 0;
    const fdt32_t *map = fdt_getprop(Tp.fdt, Tp.node, "interrupt-map", &len);
    const fdt32_t *mask = fdt_getprop(Tp.fdt, Tp.node, "interrupt-map-mask", &mask_len);
    if (!map || !mask || mask_len < 16 || !dev->irq_pin) return;
    uint32_t child[4] = {(uint32_t)dev->bus << 16 | (uint32_t)dev->devfn << 8, 0, 0, dev->irq_pin};
    for (unsigned i = 0; i < 4; ++i) child[i] &= fdt32_to_cpu(mask[i]);
    int at = 0, cells_total = len / 4;
    while (at + 5 <= cells_total) {
        const fdt32_t *e = map + at;
        int parent = dt_phandle(Tp.fdt, fdt32_to_cpu(e[4]));
        if (parent < 0) return;
        uint32_t pac = 0, pic = 0;
        (void)dt_u32(Tp.fdt, parent, "#address-cells", &pac);
        if (dt_u32(Tp.fdt, parent, "#interrupt-cells", &pic) || pic < 3) return;
        int size = 5 + (int)pac + (int)pic;
        if (at + size > cells_total) return;
        bool match = true;
        for (unsigned i = 0; i < 4; ++i)
            if (fdt32_to_cpu(e[i]) != child[i]) match = false;
        if (match) {
            const fdt32_t *spec = e + 5 + pac;
            uint32_t type = fdt32_to_cpu(spec[0]), num = fdt32_to_cpu(spec[1]);
            dev->intx_intid = type == 0 ? KSHIM_IRQ_SPI(num) : KSHIM_IRQ_PPI(num);
            dev->intx_trigger = fdt32_to_cpu(spec[2]) & 0xfU;
            return;
        }
        at += size;
    }
}

static void size_bars(struct kshim_pci_dev *dev)
{
    for (unsigned i = 0; i < 6; ++i) {
        uint16_t reg = (uint16_t)(KSHIM_PCI_BAR0 + 4U * i);
        uint32_t orig = 0, probe = 0, orig_hi = 0, probe_hi = 0;
        (void)ecam_read(&Tp.host, 0, dev->devfn, reg, 4, &orig);
        if (orig & 1U) continue; /* I/O BARs are not assigned here */
        bool is64 = ((orig >> 1) & 3U) == 2U;
        /* Sizing writes are restored at once (memory decode is off). */
        (void)ecam_write(&Tp.host, 0, dev->devfn, reg, 4, UINT32_MAX);
        (void)ecam_read(&Tp.host, 0, dev->devfn, reg, 4, &probe);
        (void)ecam_write(&Tp.host, 0, dev->devfn, reg, 4, orig);
        if (is64 && i < 5) {
            (void)ecam_read(&Tp.host, 0, dev->devfn, (uint16_t)(reg + 4), 4, &orig_hi);
            (void)ecam_write(&Tp.host, 0, dev->devfn, (uint16_t)(reg + 4), 4, UINT32_MAX);
            (void)ecam_read(&Tp.host, 0, dev->devfn, (uint16_t)(reg + 4), 4, &probe_hi);
            (void)ecam_write(&Tp.host, 0, dev->devfn, (uint16_t)(reg + 4), 4, orig_hi);
        }
        uint64_t m = (uint64_t)(is64 ? probe_hi : UINT32_MAX) << 32 | (probe & ~0xfU);
        if (!(probe & ~0xfU)) {
            if (is64) ++i;
            continue;
        }
        uint64_t size = ~m + 1U;
        struct kshim_pci_bar *bar = &dev->bars[i];
        bar->size = size;
        bar->flags = (uint8_t)((is64 ? KSHIM_PCI_BAR_MEM64 : 0) | ((orig & 8U) ? KSHIM_PCI_BAR_PREFETCH : 0));
        uint64_t current = (uint64_t)orig_hi << 32 | (orig & ~0xfU);
        if (current && current >= Tp.mem_pci && current + size <= Tp.mem_pci + Tp.mem_size) {
            bar->bus_addr = current;
            bar->inherited = true;
        } else {
            uint64_t addr = (Tp.mem_next + size - 1U) & ~(size - 1U);
            if (addr + size > Tp.mem_pci + Tp.mem_size) {
                if (is64) ++i;
                continue;
            }
            Tp.mem_next = addr + size;
            (void)kshim_pci_write_config32(dev, reg, (uint32_t)addr | (orig & 0xfU));
            if (is64) (void)kshim_pci_write_config32(dev, (uint16_t)(reg + 4), (uint32_t)(addr >> 32));
            bar->bus_addr = addr;
        }
        bar->cpu_addr = bar->bus_addr - Tp.mem_pci + Tp.mem_cpu;
        if (is64) ++i;
    }
}

static bool id_matches(const struct kshim_pci_device_id *id, const struct kshim_pci_dev *dev)
{
    if (id->vendor != KSHIM_PCI_ANY_ID && id->vendor != dev->vendor) return false;
    if (id->device != KSHIM_PCI_ANY_ID && id->device != dev->device) return false;
    return !id->class_mask || (dev->class_code & id->class_mask) == (id->class_code & id->class_mask);
}

static void bind(struct kshim_pci_dev *dev)
{
    if (dev->driver) return;
    for (struct kshim_pci_driver *drv = Tp.drivers; drv; drv = drv->next) {
        for (const struct kshim_pci_device_id *id = drv->ids; id && (id->vendor || id->device || id->class_mask); ++id) {
            if (!id_matches(id, dev)) continue;
            dev->driver = drv;
            if (!drv->probe(dev, id)) return;
            dev->driver = NULL;
            break;
        }
    }
}

WEAK int kshim_pci_driver_register(struct kshim_pci_driver *driver)
{
    if (!driver || !driver->probe) return ERROR_INVALID_PARAMETER;
    struct kshim_pci_driver **tail = &Tp.drivers;
    for (; *tail; tail = &(*tail)->next)
        if (*tail == driver) return KSHIM_IO_ERR_EXISTS;
    driver->next = NULL;
    *tail = driver;
    if (Tp.initialized)
        for (unsigned i = 0; i < Tp.count; ++i) bind(&Tp.devs[i]);
    return 0;
}

WEAK int kshim_pci_host_parse_dt(const void *fdt, int node, struct kshim_pci_host *host)
{
    (void)fdt; (void)node; (void)host;
    return ERROR_NOT_SUPPORTED;
}

WEAK int kshim_pci_host_register(struct kshim_pci_host *host)
{
    (void)host;
    return ERROR_NOT_SUPPORTED;
}

WEAK int kshim_pci_init(const void *fdt)
{
    if (Tp.initialized) return 0;
    Tp.initialized = true;
    if (!Tp.mem_size && tp_parse(fdt)) return 0;
    Tp.host.name = "pci-test-ecam";
    Tp.host.node = Tp.node;
    Tp.host.ops = &TpOps;
    Tp.host.started = true;
    printf("pci-test: ECAM 0x%llx, MEM32 0x%llx+0x%llx (test-only bus 0 enumerator)\n",
           (unsigned long long)Tp.ecam, (unsigned long long)Tp.mem_cpu, (unsigned long long)Tp.mem_size);
    for (uint8_t slot = 0; slot < 32 && Tp.count < KSHIM_PCI_MAX_DEVICES; ++slot) {
        for (uint8_t fn = 0; fn < 8 && Tp.count < KSHIM_PCI_MAX_DEVICES; ++fn) {
            uint8_t devfn = KSHIM_PCI_DEVFN(slot, fn);
            uint32_t id = 0, class_rev = 0, header = 0;
            (void)ecam_read(&Tp.host, 0, devfn, 0, 4, &id);
            if ((id & 0xffffU) == 0xffffU || !(id & 0xffffU)) {
                if (!fn) break;
                continue;
            }
            struct kshim_pci_dev *dev = &Tp.devs[Tp.count++];
            memset(dev, 0, sizeof(*dev));
            dev->host = &Tp.host;
            dev->devfn = devfn;
            dev->vendor = (uint16_t)id;
            dev->device = (uint16_t)(id >> 16);
            (void)ecam_read(&Tp.host, 0, devfn, KSHIM_PCI_CLASS_REVISION, 4, &class_rev);
            (void)ecam_read(&Tp.host, 0, devfn, 0x0c, 4, &header);
            dev->class_code = class_rev >> 8;
            dev->revision = (uint8_t)class_rev;
            dev->header_type = (uint8_t)((header >> 16) & 0x7fU);
            dev->node = -1;
            if (!dev->header_type) {
                size_bars(dev);
                uint8_t pin = 0;
                (void)kshim_pci_read_config8(dev, KSHIM_PCI_INTERRUPT_PIN, &pin);
                dev->irq_pin = pin <= 4 ? pin : 0;
                resolve_intx(dev);
                dev->cap_msi = (uint8_t)kshim_pci_find_capability(dev, KSHIM_PCI_CAP_ID_MSI);
                dev->cap_msix = (uint8_t)kshim_pci_find_capability(dev, KSHIM_PCI_CAP_ID_MSIX);
                dev->cap_pm = (uint8_t)kshim_pci_find_capability(dev, KSHIM_PCI_CAP_ID_PM);
                dev->cap_exp = (uint8_t)kshim_pci_find_capability(dev, KSHIM_PCI_CAP_ID_EXP);
            }
            printf("pci-test: 00:%02x.%u %04x:%04x class %06x", slot, fn, dev->vendor, dev->device,
                   (unsigned)dev->class_code);
            for (unsigned b = 0; b < 6; ++b)
                if (dev->bars[b].size)
                    printf(" BAR%u 0x%llx+0x%llx%s", b, (unsigned long long)dev->bars[b].bus_addr,
                           (unsigned long long)dev->bars[b].size, dev->bars[b].inherited ? " (inherited)" : "");
            if (dev->intx_intid) printf(" INT%c INTID %u", 'A' + dev->irq_pin - 1, (unsigned)dev->intx_intid);
            printf("\n");
            if (!fn && !(header & 0x00800000U)) break; /* single function */
        }
    }
    for (unsigned i = 0; i < Tp.count; ++i) bind(&Tp.devs[i]);
    return 0;
}

WEAK struct kshim_pci_dev *kshim_pci_find_device(uint16_t vendor, uint16_t device, struct kshim_pci_dev *from)
{
    unsigned start = from ? (unsigned)(from - Tp.devs) + 1U : 0;
    for (unsigned i = start; i < Tp.count; ++i)
        if ((vendor == KSHIM_PCI_ANY_ID || Tp.devs[i].vendor == vendor) &&
            (device == KSHIM_PCI_ANY_ID || Tp.devs[i].device == device)) return &Tp.devs[i];
    return NULL;
}

WEAK struct kshim_pci_dev *kshim_pci_find_class(uint32_t class_code, uint32_t class_mask, struct kshim_pci_dev *from)
{
    unsigned start = from ? (unsigned)(from - Tp.devs) + 1U : 0;
    for (unsigned i = start; i < Tp.count; ++i)
        if ((Tp.devs[i].class_code & class_mask) == (class_code & class_mask)) return &Tp.devs[i];
    return NULL;
}

WEAK int kshim_pci_enable_device(struct kshim_pci_dev *dev, bool bus_master)
{
    uint16_t cmd = 0;
    int status = kshim_pci_read_config16(dev, KSHIM_PCI_COMMAND, &cmd);
    if (status) return status;
    uint16_t want = (uint16_t)(cmd | KSHIM_PCI_COMMAND_MEMORY | (bus_master ? KSHIM_PCI_COMMAND_MASTER : 0));
    if (want != cmd) status = kshim_pci_write_config16(dev, KSHIM_PCI_COMMAND, want);
    if (!status) dev->enabled = true;
    return status;
}

WEAK volatile void *kshim_pci_bar_map(struct kshim_pci_dev *dev, unsigned bar)
{
    if (!dev || bar >= 6 || !dev->bars[bar].size || (dev->bars[bar].flags & KSHIM_PCI_BAR_IO)) return NULL;
    return (volatile void *)(uintptr_t)dev->bars[bar].cpu_addr;
}

WEAK uint64_t kshim_pci_dma_addr(const struct kshim_pci_dev *dev, uint64_t bus)
{
    (void)dev;
    return bus;
}

static void tp_dispatch(uint32_t intid, void *context)
{
    (void)context;
    for (unsigned i = 0; i < TP_IRQ_SLOTS; ++i)
        if (Tp.irqs[i].handler && Tp.irqs[i].intid == intid)
            Tp.irqs[i].handler(intid, Tp.irqs[i].context);
}

WEAK int kshim_pci_request_irq(struct kshim_pci_dev *dev, unsigned allowed, kshim_irq_handler_t handler,
                               void *context, uint32_t *intid)
{
    if (!dev || !handler || !(allowed & KSHIM_PCI_IRQ_INTX) || !dev->intx_intid || !kshim_irq_ready())
        return ERROR_NOT_SUPPORTED;
    struct tp_irq *slot = NULL;
    bool line_registered = false;
    for (unsigned i = 0; i < TP_IRQ_SLOTS; ++i) {
        if (Tp.irqs[i].handler && Tp.irqs[i].intid == dev->intx_intid) line_registered = true;
        if (!Tp.irqs[i].handler && !slot) slot = &Tp.irqs[i];
    }
    if (!slot) return KSHIM_IO_ERR_NO_MEMORY;
    *slot = (struct tp_irq){dev, handler, context, dev->intx_intid};
    if (!line_registered) {
        int status = kshim_irq_request(dev->intx_intid, dev->intx_trigger ? dev->intx_trigger
                                       : KSHIM_IRQ_TYPE_LEVEL_HIGH, tp_dispatch, NULL);
        if (status) {
            slot->handler = NULL;
            return status;
        }
    }
    uint16_t cmd = 0;
    (void)kshim_pci_read_config16(dev, KSHIM_PCI_COMMAND, &cmd);
    if (cmd & KSHIM_PCI_COMMAND_INTX_DISABLE)
        (void)kshim_pci_write_config16(dev, KSHIM_PCI_COMMAND, (uint16_t)(cmd & ~KSHIM_PCI_COMMAND_INTX_DISABLE));
    dev->irq_requested = true;
    if (intid) *intid = dev->intx_intid;
    return 0;
}

WEAK int kshim_pci_free_irq(struct kshim_pci_dev *dev)
{
    if (!dev || !dev->irq_requested) return 0;
    uint32_t line = 0;
    for (unsigned i = 0; i < TP_IRQ_SLOTS; ++i)
        if (Tp.irqs[i].handler && Tp.irqs[i].dev == dev) {
            line = Tp.irqs[i].intid;
            Tp.irqs[i].handler = NULL;
        }
    bool still_used = false;
    for (unsigned i = 0; i < TP_IRQ_SLOTS; ++i)
        if (Tp.irqs[i].handler && Tp.irqs[i].intid == line) still_used = true;
    if (line && !still_used) (void)kshim_irq_free(line);
    dev->irq_requested = false;
    return 0;
}

WEAK int kshim_pci_quiesce(void)
{
    if (Tp.quiesced) return 0;
    int result = 0;
    for (unsigned i = Tp.count; i > 0; --i) {
        struct kshim_pci_dev *dev = &Tp.devs[i - 1];
        if (dev->driver && dev->driver->quiesce) {
            int status = dev->driver->quiesce(dev);
            if (status && !result) result = status;
        }
    }
    if (result) return result; /* a DMA master may still run: keep everything */
    for (unsigned i = 0; i < Tp.count; ++i) (void)kshim_pci_free_irq(&Tp.devs[i]);
    for (unsigned i = Tp.journal_count; i > 0; --i)
        (void)ecam_write(&Tp.host, 0, Tp.journal[i - 1].dev->devfn, Tp.journal[i - 1].reg, 4,
                         Tp.journal[i - 1].value);
    Tp.journal_count = 0;
    Tp.quiesced = true;
    printf("pci-test: quiesced, config journal replayed\n");
    return 0;
}
