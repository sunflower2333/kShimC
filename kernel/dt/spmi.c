/* SPDX-License-Identifier: MIT */
#include <qcom_dt.h>
#include <string.h>
static int controller_node(const void *f, int n)
{ return fdt_node_check_compatible(f, n, "qcom,spmi-pmic-arb") == 0 && dt_available(f, n); }
int kshim_dt_spmi(const void *f, struct kshim_resources *r)
{
    static const char *names[SPMI_MEMORY_REGION_TYPE_MAX] = {
        "core", "chnls", "obsrvr", "intr", "cnfg", "chnls-map", "chnls-owner"
    };
    r->spmi_count = 0;
    for (int n = fdt_next_node(f, -1, NULL); n >= 0; n = fdt_next_node(f, n, NULL)) {
        if (!controller_node(f, n)) continue;
        if (r->spmi_count == KSHIM_SPMI_MAX) return -1;
        struct kshim_spmi_resource *s = &r->spmi[r->spmi_count];
        memset(s, 0, sizeof(*s)); s->node = n;
        for (unsigned i = 0; i < SPMI_MEMORY_REGION_TYPE_MAX; ++i) {
            struct dt_range range;
            if (dt_reg_named(f, n, names[i], &range)) {
                if (i == SPMI_MEMORY_REGION_TYPE_CORE || i == SPMI_MEMORY_REGION_TYPE_CH_SLAVES ||
                    i == SPMI_MEMORY_REGION_TYPE_OBSERVER) return -1;
                continue;
            }
            if ((range.base & 3) || range.base > UINTPTR_MAX || range.size > SIZE_MAX) return -1;
            s->controller.Regions[i] = (SpmiMemoryRegion){range.base, range.size};
        }
        uint32_t ee, channel = 0, bus = 0;
        if (dt_u32(f, n, "qcom,ee", &ee) || ee > 5) return -1;
        if (fdt_getprop(f, n, "qcom,channel", NULL) && dt_u32(f, n, "qcom,channel", &channel)) return -1;
        if (fdt_getprop(f, n, "qcom,bus-id", NULL) && dt_u32(f, n, "qcom,bus-id", &bus)) return -1;
        if (channel > 5 || bus > 3) return -1;
        s->controller.ActiveEE = ee; s->controller.Channel = channel; s->controller.BusId = bus;
        ++r->spmi_count;
    }
    return r->spmi_count ? 0 : -1;
}
int kshim_dt_spmi_identity(const void *f, int node, uint8_t *controller,
                           uint8_t *bus, uint8_t *sid)
{
    int pmic = node, arb = -1;
    for (; pmic > 0; pmic = fdt_parent_offset(f, pmic)) {
        int parent = fdt_parent_offset(f, pmic);
        if (parent >= 0 && controller_node(f, parent)) { arb = parent; break; }
    }
    if (arb < 0 || !dt_available(f, node)) return -1;
    int len;
    const fdt32_t *reg = fdt_getprop(f, pmic, "reg", &len);
    if (!reg || len != 8 || fdt32_to_cpu(reg[0]) > 15 || fdt32_to_cpu(reg[1])) return -1;
    uint32_t id = 0;
    if (fdt_getprop(f, arb, "qcom,bus-id", NULL) && dt_u32(f, arb, "qcom,bus-id", &id)) return -1;
    if (id > 3) return -1;
    unsigned index = 0;
    for (int n = fdt_next_node(f, -1, NULL); n >= 0; n = fdt_next_node(f, n, NULL)) {
        if (!controller_node(f, n)) continue;
        if (n == arb) {
            if (index >= KSHIM_SPMI_MAX) return -1;
            *controller = index; *bus = id; *sid = fdt32_to_cpu(reg[0]);
            return 0;
        }
        ++index;
    }
    return -1;
}

/* Downstream Qualcomm encodes PMIC peripheral reg as <offset length> with
 * address-cells=2, size-cells=0. Mainline uses <offset>, size-cells=0.
 * This is a bus-specific tuple, not a 64-bit physical MMIO address. */
int kshim_dt_spmi_reg(const void *f, int node, uint16_t *offset)
{
    uint8_t controller, bus, sid;
    if (!offset || kshim_dt_spmi_identity(f, node, &controller, &bus, &sid)) return -1;
    int parent = fdt_parent_offset(f, node), len;
    uint32_t ac, sc;
    const fdt32_t *p = fdt_getprop(f, node, "reg", &len);
    if (!p || dt_u32(f, parent, "#address-cells", &ac) ||
        dt_u32(f, parent, "#size-cells", &sc) || sc ||
        (ac != 1 && ac != 2) || len != (int)ac * 4) return -1;
    uint32_t a = fdt32_to_cpu(p[0]), size = ac == 2 ? fdt32_to_cpu(p[1]) : 0;
    if ((a & 0xffU) || a > UINT16_MAX || size > 0x10000U - a) return -1;
    *offset = a;
    return 0;
}
