/* SPDX-License-Identifier: MIT */
/*
 * Interrupt devicetree parsing: GICv3 node, timer PPIs, "interrupts" and
 * "interrupts-extended" specifiers. Pure FDT code, shared by the firmware
 * and the host tests.
 */
#include <kshim_irq_dt.h>
#include <dt.h>
#include <kshim/irq.h>

#include <string.h>

#define GIC_SPI 0U
#define GIC_PPI 1U
#define GIC_ESPI 2U
#define GIC_EPPI 3U
#define MAX_PARENT_HOPS 16U

static bool is_gic(const void *f, int node)
{
    return node >= 0 && !dt_node_check_compatible(f, node, "arm,gic-v3");
}

/* of_irq_find_parent(): follow interrupt-parent, else the tree parent, until
 * a node with #interrupt-cells. */
static int irq_parent(const void *f, int node)
{
    for (unsigned hop = 0; node >= 0 && hop < MAX_PARENT_HOPS; ++hop) {
        int len;
        const fdt32_t *p = fdt_getprop(f, node, "interrupt-parent", &len);
        int next;
        if (p) {
            if (len != 4) return -1;
            next = dt_node_by_phandle(f, fdt32_to_cpu(*p));
        } else {
            if (len != -FDT_ERR_NOTFOUND) return -1;
            next = dt_parent_offset(f, node);
        }
        if (next < 0) return -1;
        if (fdt_getprop(f, next, "#interrupt-cells", NULL)) return next;
        node = next;
    }
    return -1;
}

static int interrupt_cells(const void *f, int controller, uint32_t *cells)
{
    if (dt_u32(f, controller, "#interrupt-cells", cells) || !*cells || *cells > 8)
        return -1;
    return 0;
}

/* Raw specifier `index` of a node and the controller it belongs to. */
static int irq_spec(const void *f, int node, int index, int *parent,
                    const fdt32_t **spec, uint32_t *cells)
{
    int len;
    const fdt32_t *p = fdt_getprop(f, node, "interrupts-extended", &len);
    if (p) {
        unsigned total = (unsigned)len / 4U, off = 0;
        if (len <= 0 || len % 4) return ERROR_INVALID_PARAMETER;
        for (int item = 0;; ++item) {
            if (off >= total) return ERROR_NOT_FOUND;
            *parent = dt_node_by_phandle(f, fdt32_to_cpu(p[off++]));
            if (*parent < 0 || interrupt_cells(f, *parent, cells) || *cells > total - off)
                return ERROR_INVALID_PARAMETER;
            if (item == index) { *spec = p + off; return 0; }
            off += *cells;
        }
    }
    if (len != -FDT_ERR_NOTFOUND) return ERROR_INVALID_PARAMETER;
    p = fdt_getprop(f, node, "interrupts", &len);
    if (!p) return len == -FDT_ERR_NOTFOUND ? ERROR_NOT_FOUND : ERROR_INVALID_PARAMETER;
    *parent = irq_parent(f, node);
    if (*parent < 0 || interrupt_cells(f, *parent, cells) || len <= 0 ||
        len % (int)(4U * *cells))
        return ERROR_INVALID_PARAMETER;
    if ((unsigned)index >= (unsigned)len / (4U * *cells)) return ERROR_NOT_FOUND;
    *spec = p + (unsigned)index * *cells;
    return 0;
}

/* <type number flags> of the arm,gic-v3 binding. */
static int gic_spec(const fdt32_t *spec, uint32_t cells, uint32_t *intid, unsigned *trigger)
{
    if (cells < 3) return ERROR_INVALID_PARAMETER;
    uint32_t type = fdt32_to_cpu(spec[0]), number = fdt32_to_cpu(spec[1]);
    if (type == GIC_SPI && number < 988U) *intid = KSHIM_IRQ_SPI(number);
    else if (type == GIC_PPI && number < 16U) *intid = KSHIM_IRQ_PPI(number);
    else if (type == GIC_ESPI || type == GIC_EPPI) return ERROR_NOT_SUPPORTED;
    else return ERROR_INVALID_PARAMETER;
    if (trigger) *trigger = fdt32_to_cpu(spec[2]) & 0xfU;
    return 0;
}

int kshim_irq_from_fdt(const void *f, int node, int index, uint32_t *intid, unsigned *trigger)
{
    int parent;
    const fdt32_t *spec;
    uint32_t cells;
    if (!f || node < 0 || index < 0 || !intid) return ERROR_INVALID_PARAMETER;
    int status = irq_spec(f, node, index, &parent, &spec, &cells);
    if (status) return status;
    if (!is_gic(f, parent)) return ERROR_NOT_SUPPORTED;
    return gic_spec(spec, cells, intid, trigger);
}

static int find_compatible(const void *f, const char *compatible)
{
    for (int n = dt_node_by_compatible(f, -1, compatible); n >= 0;
         n = dt_node_by_compatible(f, n, compatible))
        if (dt_available(f, n)) return n;
    return -1;
}

int kshim_dt_gic(const void *f, struct kshim_gic_desc *g)
{
    if (!f || !g) return ERROR_INVALID_PARAMETER;
    memset(g, 0, sizeof(*g));
    g->node = -1;
    int node = find_compatible(f, "arm,gic-v3"), len;
    if (node < 0 || !fdt_getprop(f, node, "interrupt-controller", NULL)) return ERROR_NOT_FOUND;
    uint32_t regions = 1;
    if (fdt_getprop(f, node, "#redistributor-regions", &len) &&
        (dt_u32(f, node, "#redistributor-regions", &regions) || !regions ||
         regions > KSHIM_GIC_MAX_REDIST_REGIONS))
        return ERROR_INVALID_PARAMETER;
    if (dt_reg(f, node, 0, &g->dist) || g->dist.size < KSHIM_GIC_FRAME_SIZE)
        return ERROR_INVALID_PARAMETER;
    for (unsigned i = 0; i < regions; ++i)
        if (dt_reg(f, node, 1 + i, &g->redist[i]) || g->redist[i].size < 2 * KSHIM_GIC_FRAME_SIZE)
            return ERROR_INVALID_PARAMETER;
    const fdt32_t *stride = fdt_getprop(f, node, "redistributor-stride", &len);
    if (stride) {
        if (len == 4) g->redist_stride = fdt32_to_cpu(stride[0]);
        else if (len == 8) g->redist_stride = (uint64_t)fdt32_to_cpu(stride[0]) << 32 | fdt32_to_cpu(stride[1]);
        else return ERROR_INVALID_PARAMETER;
        if (g->redist_stride < 2 * KSHIM_GIC_FRAME_SIZE || g->redist_stride % KSHIM_GIC_FRAME_SIZE)
            return ERROR_INVALID_PARAMETER;
    } else if (len != -FDT_ERR_NOTFOUND) return ERROR_INVALID_PARAMETER;
    g->node = node;
    g->redist_count = regions;
    return 0;
}

/* A PPI specifier whose 3-cell controller forwards to the GIC. */
static int ppi_through_parent(const void *f, int parent, const fdt32_t *spec, uint32_t cells,
                              uint32_t *intid)
{
    if (cells != 3 || fdt32_to_cpu(spec[0]) != GIC_PPI) return ERROR_NOT_SUPPORTED;
    for (unsigned hop = 0; hop < MAX_PARENT_HOPS; ++hop) {
        uint32_t c;
        if (interrupt_cells(f, parent, &c) || c != 3) return ERROR_NOT_SUPPORTED;
        if (is_gic(f, parent)) return gic_spec(spec, cells, intid, NULL);
        int len;
        const fdt32_t *p = fdt_getprop(f, parent, "interrupt-parent", &len);
        if (!p || len != 4) return ERROR_NOT_SUPPORTED;
        int next = dt_node_by_phandle(f, fdt32_to_cpu(*p));
        if (next < 0 || next == parent) return ERROR_NOT_SUPPORTED;
        parent = next;
    }
    return ERROR_NOT_SUPPORTED;
}

int kshim_dt_arch_timer(const void *f, struct kshim_timer_desc *t)
{
    static const char *const names[KSHIM_TIMER_COUNT] = {
        "sec-phys", "phys", "virt", "hyp-phys", "hyp-virt"};
    if (!f || !t) return ERROR_INVALID_PARAMETER;
    memset(t, 0, sizeof(*t));
    t->node = -1;
    int node = find_compatible(f, "arm,armv8-timer");
    if (node < 0) return ERROR_NOT_FOUND;
    bool named = fdt_getprop(f, node, "interrupt-names", NULL) != NULL;
    unsigned found = 0;
    for (unsigned i = 0; i < KSHIM_TIMER_COUNT; ++i) {
        int index = named ? dt_string_index(f, node, "interrupt-names", names[i]) : (int)i;
        if (index < 0) continue;
        int parent;
        const fdt32_t *spec;
        uint32_t cells, intid;
        int status = irq_spec(f, node, index, &parent, &spec, &cells);
        if (status == ERROR_NOT_FOUND && !named) break;
        if (!status)
            status = is_gic(f, parent) ? gic_spec(spec, cells, &intid, NULL) :
                     ppi_through_parent(f, parent, spec, cells, &intid);
        if (status) return status;
        if (intid < KSHIM_IRQ_PPI(0) || intid >= KSHIM_IRQ_SPI(0)) return ERROR_INVALID_PARAMETER;
        t->intid[i] = intid;
        ++found;
    }
    if (!found) return ERROR_NOT_FOUND;
    t->node = node;
    return 0;
}
