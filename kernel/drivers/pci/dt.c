/* SPDX-License-Identifier: MIT */
/*
 * Devicetree side of the PCI core: host bridge "bus-range",
 * "linux,pci-domain", "ranges" windows, "dma-coherent" and INTx through
 * "interrupt-map". No hardware access.
 *
 * Binding facts (IEEE 1275 PCI bus binding, Linux
 * Documentation/devicetree/bindings/pci/pci.txt and host-generic-pci.yaml):
 *  - a PCI address is 3 cells, phys.hi = npt000ss bbbbbbbb dddddfff rrrrrrrr:
 *    ss (bits 25:24) is the space (01 I/O, 10 32-bit memory, 11 64-bit
 *    memory), p (bit 30) prefetchable, b/d/f the bus, device and function;
 *  - "ranges" entries are <pci-address(3) cpu-address(parent #address-cells)
 *    size(#size-cells of the bridge, 2)>;
 *  - "interrupt-map" entries are <child-unit-address(3) child-pin(1)
 *    parent-phandle parent-unit-address(parent #address-cells, default 0)
 *    parent-specifier(parent #interrupt-cells)>, matched after applying
 *    "interrupt-map-mask" to the child unit address and pin.
 *  - arm,gic-v3 specifiers are <type number flags [ppi-partition]>, type 0
 *    SPI (INTID 32 + number), 1 PPI (INTID 16 + number).
 */
#include "pci_internal.h"

#include <dt.h>
#include <kshim/irq.h>
#include <string.h>

#define PCI_SPACE(hi)        (((hi) >> 24) & 3U)
#define PCI_SPACE_IO         1U
#define PCI_SPACE_MEM32      2U
#define PCI_SPACE_MEM64      3U
#define PCI_PREFETCHABLE     (1U << 30)
#define MAX_TRANSLATE_DEPTH  16

static int cells(const void *f, int node, const char *prop, int fallback)
{
    int len;
    const fdt32_t *p = fdt_getprop(f, node, prop, &len);
    if (!p) return len == -FDT_ERR_NOTFOUND ? fallback : -1;
    if (len != 4 || fdt32_to_cpu(*p) > 4) return -1;
    return (int)fdt32_to_cpu(*p);
}

static uint64_t value(const fdt32_t *p, int n)
{
    uint64_t v = 0;
    while (n-- > 0) v = (v << 32) | fdt32_to_cpu(*p++);
    return v;
}

int kshim_pci_dt_translate(const void *f, int node, uint64_t address, uint64_t size,
                           uint64_t *cpu)
{
    if (!f || !cpu || node < 0) return ERROR_INVALID_PARAMETER;
    int depth = 0;
    /* `bus` is the node whose address space `address` belongs to. */
    for (int bus = dt_parent_offset(f, node); bus > 0; bus = dt_parent_offset(f, bus)) {
        if (++depth > MAX_TRANSLATE_DEPTH) return ERROR_INVALID_PARAMETER;
        int parent = dt_parent_offset(f, bus), len;
        if (parent < 0) return ERROR_INVALID_PARAMETER;
        int ac = cells(f, bus, "#address-cells", 2), sc = cells(f, bus, "#size-cells", 1);
        int pc = cells(f, parent, "#address-cells", 2);
        if (ac < 1 || ac > 2 || sc < 0 || sc > 2 || pc < 1 || pc > 2)
            return ERROR_INVALID_PARAMETER;
        const fdt32_t *p = fdt_getprop(f, bus, "ranges", &len);
        if (!p) return ERROR_NOT_FOUND; /* no "ranges": not memory mapped */
        if (!len) continue;             /* empty "ranges": identity */
        int tuple = ac + pc + sc;
        if (len < 0 || len % (tuple * 4)) return ERROR_INVALID_PARAMETER;
        bool found = false;
        for (int off = 0; off < len / 4; off += tuple) {
            struct dt_range window = {value(p + off, ac), value(p + off + ac + pc, sc)};
            uint64_t target = value(p + off + ac, pc);
            if (!dt_contains(window, address, size ? size : 1)) continue;
            if (target > UINT64_MAX - window.size) return ERROR_INVALID_PARAMETER;
            address = target + (address - window.base);
            found = true;
            break;
        }
        if (!found) return ERROR_NOT_FOUND;
    }
    *cpu = address;
    return 0;
}

int kshim_pci_dt_windows(const void *f, int node, struct kshim_pci_host *host)
{
    int len;
    const fdt32_t *p = fdt_getprop(f, node, "ranges", &len);
    int parent = dt_parent_offset(f, node);
    int ac = cells(f, node, "#address-cells", 3), sc = cells(f, node, "#size-cells", 2);
    int pc = parent >= 0 ? cells(f, parent, "#address-cells", 2) : -1;
    host->window_count = 0;
    if (!p) return len == -FDT_ERR_NOTFOUND ? 0 : ERROR_INVALID_PARAMETER;
    if (ac != 3 || sc < 1 || sc > 2 || pc < 1 || pc > 2) return ERROR_INVALID_PARAMETER;
    int tuple = ac + pc + sc;
    if (len <= 0 || len % (tuple * 4)) return ERROR_INVALID_PARAMETER;
    for (int off = 0; off < len / 4; off += tuple) {
        uint32_t hi = fdt32_to_cpu(p[off]);
        uint64_t pci = value(p + off + 1, 2);
        uint64_t cpu_local = value(p + off + ac, pc);
        uint64_t size = value(p + off + ac + pc, sc);
        uint8_t type;
        switch (PCI_SPACE(hi)) {
        case PCI_SPACE_IO: type = KSHIM_PCI_WINDOW_IO; break;
        case PCI_SPACE_MEM32: type = KSHIM_PCI_WINDOW_MEM32; break;
        case PCI_SPACE_MEM64: type = KSHIM_PCI_WINDOW_MEM64; break;
        default: continue; /* configuration space entries are not windows */
        }
        if (!size || pci > UINT64_MAX - size) return ERROR_INVALID_PARAMETER;
        if (size > KSHIM_PCI_WINDOW_CAP) size = KSHIM_PCI_WINDOW_CAP;
        uint64_t cpu;
        int status = kshim_pci_dt_translate(f, node, cpu_local, size, &cpu);
        if (status) return status;
        if (host->window_count == KSHIM_PCI_MAX_WINDOWS) break;
        struct kshim_pci_window *w = &host->windows[host->window_count++];
        memset(w, 0, sizeof(*w));
        w->type = type;
        w->prefetchable = (hi & PCI_PREFETCHABLE) != 0;
        w->cpu_base = cpu;
        w->pci_base = pci;
        w->size = size;
        w->next = pci;
    }
    return 0;
}

int kshim_pci_host_parse_dt(const void *f, int node, struct kshim_pci_host *host)
{
    if (!f || node < 0 || !host) return ERROR_INVALID_PARAMETER;
    int len;
    host->node = node;
    host->bus_start = 0;
    host->bus_end = 0xff;
    const fdt32_t *range = fdt_getprop(f, node, "bus-range", &len);
    if (range) {
        if (len != 8) return ERROR_INVALID_PARAMETER;
        uint32_t start = fdt32_to_cpu(range[0]), end = fdt32_to_cpu(range[1]);
        if (start > end || end > 0xff) return ERROR_INVALID_PARAMETER;
        host->bus_start = (uint8_t)start;
        host->bus_end = (uint8_t)end;
    } else if (len != -FDT_ERR_NOTFOUND) {
        return ERROR_INVALID_PARAMETER;
    }
    uint32_t domain;
    host->domain = dt_u32(f, node, "linux,pci-domain", &domain) ? 0xffffU : (uint16_t)domain;
    bool coherent = false;
    (void)dt_bool(f, node, "dma-coherent", &coherent);
    host->dma_coherent = coherent;
    return kshim_pci_dt_windows(f, node, host);
}

static bool is_gic(const void *f, int node)
{
    return node >= 0 && !fdt_node_check_compatible(f, node, "arm,gic-v3");
}

int kshim_pci_dt_intx(const void *f, int host_node, uint32_t unit_hi, uint8_t pin,
                      uint32_t *intid, unsigned *trigger)
{
    int len, mlen;
    if (!f || host_node < 0 || pin < 1 || pin > 4 || !intid) return ERROR_INVALID_PARAMETER;
    const fdt32_t *map = fdt_getprop(f, host_node, "interrupt-map", &len);
    if (!map) return ERROR_NOT_FOUND;
    if (len <= 0 || len % 4) return ERROR_INVALID_PARAMETER;
    uint32_t icells;
    if (dt_u32(f, host_node, "#interrupt-cells", &icells) || icells != 1)
        return ERROR_INVALID_PARAMETER;
    int ac = cells(f, host_node, "#address-cells", 3);
    if (ac != 3) return ERROR_INVALID_PARAMETER;
    uint32_t mask[4] = {0xffffffffU, 0xffffffffU, 0xffffffffU, 0xffffffffU};
    const fdt32_t *m = fdt_getprop(f, host_node, "interrupt-map-mask", &mlen);
    if (m) {
        if (mlen != 16) return ERROR_INVALID_PARAMETER;
        for (unsigned i = 0; i < 4; ++i) mask[i] = fdt32_to_cpu(m[i]);
    }
    const uint32_t key[4] = {unit_hi & mask[0], 0, 0, pin & mask[3]};
    unsigned total = (unsigned)len / 4U;
    for (unsigned off = 0; off < total;) {
        if (total - off < 5) return ERROR_INVALID_PARAMETER;
        bool match = true;
        for (unsigned i = 0; i < 4; ++i)
            if ((fdt32_to_cpu(map[off + i]) & mask[i]) != key[i]) match = false;
        int parent = dt_node_by_phandle(f, fdt32_to_cpu(map[off + 4]));
        if (parent < 0) return ERROR_INVALID_PARAMETER;
        int pac = cells(f, parent, "#address-cells", 0);
        uint32_t pic;
        if (pac < 0 || dt_u32(f, parent, "#interrupt-cells", &pic) || !pic || pic > 8)
            return ERROR_INVALID_PARAMETER;
        off += 5;
        if (total - off < (unsigned)pac + pic) return ERROR_INVALID_PARAMETER;
        const fdt32_t *spec = map + off + pac;
        off += (unsigned)pac + pic;
        if (!match) continue;
        if (!is_gic(f, parent) || pic < 3) return ERROR_NOT_SUPPORTED;
        uint32_t type = fdt32_to_cpu(spec[0]), number = fdt32_to_cpu(spec[1]);
        if (type == 0 && number < 988U) *intid = KSHIM_IRQ_SPI(number);
        else if (type == 1 && number < 16U) *intid = KSHIM_IRQ_PPI(number);
        else return ERROR_NOT_SUPPORTED;
        if (trigger) *trigger = fdt32_to_cpu(spec[2]) & 0xfU;
        return 0;
    }
    return ERROR_NOT_FOUND;
}
