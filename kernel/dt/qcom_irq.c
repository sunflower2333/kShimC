/* SPDX-License-Identifier: MIT */
/*
 * Devicetree interrupt specifiers for the Qualcomm cascades. Android SM8150
 * runtime DTs set interrupt-parent = <&pdc> at the root, so the SPMI
 * arbiter, the TLMM summary and the QUP engines are PDC children; PMIC
 * peripherals inherit the arbiter (4 cells) and TLMM consumers name the TLMM
 * (2 cells). interrupt-map nexus nodes are not used by these boards.
 */
#include <qcom_irq.h>
#include <dt.h>
#include <string.h>

static int irq_cells(const void *f, int controller, unsigned *count)
{
    uint32_t cells;
    if (controller < 0 || dt_u32(f, controller, "#interrupt-cells", &cells) ||
        !cells || cells > KSHIM_DT_IRQ_CELLS) return -1;
    *count = cells;
    return 0;
}

int kshim_dt_irq_parent(const void *f, int node)
{
    if (!f || node < 0) return -1;
    for (unsigned depth = 0; depth < 64; ++depth) {
        int len, parent;
        const fdt32_t *handle = fdt_getprop(f, node, "interrupt-parent", &len);
        if (handle) {
            if (len != 4) return -1;
            parent = dt_phandle(f, fdt32_to_cpu(*handle));
        } else {
            if (len != -FDT_ERR_NOTFOUND) return -1;
            parent = dt_parent_offset(f, node);
        }
        if (parent < 0) return -1;
        if (fdt_getprop(f, parent, "#interrupt-cells", NULL)) return parent;
        node = parent;
    }
    return -1;
}

int kshim_dt_irq(const void *f, int node, unsigned index, struct kshim_dt_irq *irq)
{
    int len;
    if (!f || !irq || node < 0 || !dt_available(f, node)) return -1;
    const fdt32_t *p = fdt_getprop(f, node, "interrupts-extended", &len);
    if (p) {
        if (len <= 0 || len % 4) return -1;
        for (unsigned off = 0, item = 0; off < (unsigned)len / 4; ++item) {
            unsigned count;
            int parent = dt_phandle(f, fdt32_to_cpu(p[off++]));
            if (irq_cells(f, parent, &count) || count > (unsigned)len / 4 - off) return -1;
            if (item == index) {
                irq->parent = parent; irq->count = count;
                for (unsigned i = 0; i < count; ++i) irq->cells[i] = fdt32_to_cpu(p[off + i]);
                return 0;
            }
            off += count;
        }
        return -1;
    }
    p = fdt_getprop(f, node, "interrupts", &len);
    if (!p || len <= 0 || len % 4) return -1;
    unsigned count;
    int parent = kshim_dt_irq_parent(f, node);
    if (irq_cells(f, parent, &count) || (unsigned)len / 4 % count ||
        index >= (unsigned)len / 4 / count) return -1;
    irq->parent = parent; irq->count = count;
    for (unsigned i = 0; i < count; ++i) irq->cells[i] = fdt32_to_cpu(p[index * count + i]);
    return 0;
}

int kshim_dt_irq_named(const void *f, int node, const char *name, struct kshim_dt_irq *irq)
{
    int index = name ? dt_string_index(f, node, "interrupt-names", name) : -1;
    return index < 0 ? -1 : kshim_dt_irq(f, node, (unsigned)index, irq);
}

static bool compatible_prefix(const void *f, int node, const char *prefix, const char *suffix)
{
    int len;
    const char *list = fdt_getprop(f, node, "compatible", &len);
    if (!list || len <= 0 || list[len - 1]) return false;
    size_t plen = strlen(prefix), slen = suffix ? strlen(suffix) : 0;
    for (int off = 0; off < len; off += (int)strlen(list + off) + 1) {
        const char *c = list + off;
        size_t n = strlen(c);
        if (n >= plen + slen && !strncmp(c, prefix, plen) &&
            (!slen || !strcmp(c + n - slen, suffix))) return true;
    }
    return false;
}

enum kshim_dt_irq_kind kshim_dt_irq_kind(const void *f, int n)
{
    if (!f || n < 0 || !fdt_getprop(f, n, "interrupt-controller", NULL)) return KSHIM_DT_IRQ_OTHER;
    if (!dt_node_check_compatible(f, n, "arm,gic-v3") ||
        !dt_node_check_compatible(f, n, "arm,gic-400") ||
        !dt_node_check_compatible(f, n, "arm,cortex-a15-gic")) return KSHIM_DT_IRQ_GIC;
    if (!dt_node_check_compatible(f, n, "qcom,pdc") || compatible_prefix(f, n, "qcom,pdc-", NULL) ||
        compatible_prefix(f, n, "qcom,", "-pdc")) return KSHIM_DT_IRQ_PDC;
    if (!dt_node_check_compatible(f, n, "qcom,spmi-pmic-arb")) return KSHIM_DT_IRQ_SPMI;
    if (fdt_getprop(f, n, "gpio-controller", NULL) &&
        (compatible_prefix(f, n, "qcom,", "-pinctrl") || compatible_prefix(f, n, "qcom,", "-tlmm")))
        return KSHIM_DT_IRQ_TLMM;
    return KSHIM_DT_IRQ_OTHER;
}

int kshim_dt_spmi_controller(const void *f, unsigned controller)
{
    unsigned index = 0;
    for (int n = dt_next_node(f, -1); n >= 0; n = dt_next_node(f, n)) {
        if (dt_node_check_compatible(f, n, "qcom,spmi-pmic-arb") || !dt_available(f, n)) continue;
        if (index++ == controller) return n;
    }
    return -1;
}

/* One depth-first pass over the arbiter subtree, carrying availability and
 * the inherited interrupt parent per depth: libfdt parent lookups are linear
 * in the blob, so per-node kshim_dt_irq() calls would rescan it each time. */
int kshim_dt_spmi_irq_find(const void *f, unsigned controller, uint8_t sid,
                           uint8_t peripheral, uint8_t irq, unsigned *trigger)
{
    enum { DEPTH = 16 };
    int arb = kshim_dt_spmi_controller(f, controller), found = -1;
    if (arb < 0 || !trigger || !dt_available(f, arb)) return -1;
    /* nodes[d]: the current node at depth d; cells[d]: it has
     * #interrupt-cells; parent[d]: its interrupt parent. Without its own
     * interrupt-parent a node uses its tree parent if that is a controller,
     * else the tree parent's interrupt parent (of_irq_find_parent). */
    int nodes[DEPTH], parent[DEPTH];
    bool cells[DEPTH];
    nodes[0] = arb; cells[0] = true; parent[0] = kshim_dt_irq_parent(f, arb);
    int depth = 0, skip = DEPTH;
    for (int n = fdt_next_node(f, arb, &depth); n >= 0 && depth > 0; n = fdt_next_node(f, n, &depth)) {
        if (depth >= DEPTH) return -1;
        if (depth > skip) continue;      /* inside a disabled subtree */
        skip = DEPTH;
        int len;
        if (fdt_getprop(f, n, "status", &len)) {
            const char *status = dt_string(f, n, "status");
            if (!status || (strcmp(status, "okay") && strcmp(status, "ok"))) { skip = depth; continue; }
        } else if (len != -FDT_ERR_NOTFOUND) return -1;
        nodes[depth] = n;
        cells[depth] = fdt_getprop(f, n, "#interrupt-cells", NULL) != NULL;
        /* This node's own interrupt parent. */
        int own;
        if (fdt_getprop(f, n, "interrupt-parent", NULL)) own = kshim_dt_irq_parent(f, n);
        else own = cells[depth - 1] ? nodes[depth - 1] : parent[depth - 1];
        parent[depth] = own;
        const fdt32_t *p = fdt_getprop(f, n, "interrupts", &len);
        if (fdt_getprop(f, n, "interrupts-extended", NULL)) {
            struct kshim_dt_irq spec;
            for (unsigned i = 0; kshim_dt_irq(f, n, i, &spec) == 0; ++i) {
                if (spec.parent != arb || spec.count != 4 || spec.cells[0] != sid ||
                    spec.cells[1] != peripheral || spec.cells[2] != irq) continue;
                if (found >= 0 && found != n) return -1;
                found = n; *trigger = kshim_dt_irq_trigger(spec.cells[3]);
            }
            continue;
        }
        if (!p || own != arb || len <= 0 || len % 16) continue;
        for (int i = 0; i < len / 4; i += 4) {
            if (fdt32_to_cpu(p[i]) != sid || fdt32_to_cpu(p[i + 1]) != peripheral ||
                fdt32_to_cpu(p[i + 2]) != irq) continue;
            /* Two nodes describing one PMIC bit is a DT error, not a choice. */
            if (found >= 0 && found != n) return -1;
            found = n; *trigger = kshim_dt_irq_trigger(fdt32_to_cpu(p[i + 3]));
        }
    }
    return found;
}
