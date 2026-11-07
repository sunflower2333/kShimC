/* SPDX-License-Identifier: MIT */
/*
 * QUPv3 / GENI serial-engine discovery from the runtime DT (kshim/qup.h).
 * Pure FDT: no MMIO, no clocks.
 *
 * Binding facts (Linux Documentation/devicetree/bindings/soc/qcom/
 * qcom,geni-se.yaml and the msm-4.14/4.19 Android QUPv3 bindings, as seen in
 * the andromeda, hdk8150, kona and SM8150/SM8250/SM8650 mainline DTs):
 *  - mainline wrappers are "qcom,geni-se-qup" nodes whose children are the
 *    serial engines; Android wrappers are "qcom,qupv3-geni-se" nodes and the
 *    serial engines are elsewhere under /soc with a "qcom,wrapper-core"
 *    phandle back to their wrapper;
 *  - an engine's first "reg" entry is its register window (0x4000 bytes on
 *    every SoC here); several protocol nodes can name one window (SM8650
 *    i2c4/spi4 at 0xa90000, kona SE12 uart/i2c at 0xa90000).
 */
#include <kshim/qup.h>
#include <kshim/io.h>
#include <dt.h>
#include <string.h>

#define SE_MIN_WINDOW 0x1000U

static const struct {
    const char *compatible;
    enum kshim_qup_protocol protocol;
    bool android;
} SeCompatibles[] = {
    {"qcom,geni-i2c", KSHIM_QUP_PROTOCOL_I2C, false},
    {"qcom,geni-spi", KSHIM_QUP_PROTOCOL_SPI, false},
    {"qcom,geni-uart", KSHIM_QUP_PROTOCOL_UART, false},
    {"qcom,geni-debug-uart", KSHIM_QUP_PROTOCOL_UART, false},
    {"qcom,i2c-geni", KSHIM_QUP_PROTOCOL_I2C, true},
    {"qcom,spi-geni", KSHIM_QUP_PROTOCOL_SPI, true},
    {"qcom,msm-geni-serial-hs", KSHIM_QUP_PROTOCOL_UART, true},
    {"qcom,msm-geni-console", KSHIM_QUP_PROTOCOL_UART, true},
};
#define SE_COMPATIBLES (sizeof(SeCompatibles) / sizeof(SeCompatibles[0]))

static int se_kind(const void *f, int node)
{
    if (!f || node < 0) return -1;
    for (unsigned i = 0; i < SE_COMPATIBLES; ++i)
        if (!dt_node_check_compatible(f, node, SeCompatibles[i].compatible)) return (int)i;
    return -1;
}

enum kshim_qup_protocol kshim_dt_qup_protocol(const void *f, int node)
{
    int kind = se_kind(f, node);
    return kind < 0 ? KSHIM_QUP_PROTOCOL_NONE : SeCompatibles[kind].protocol;
}

bool kshim_dt_qup_android(const void *f, int node)
{
    int kind = se_kind(f, node);
    return kind >= 0 && SeCompatibles[kind].android;
}

uint32_t kshim_dt_qup_i2c_bus_hz(const void *f, int node)
{
    uint32_t hz = 0;
    if (fdt_getprop(f, node, "clock-frequency", NULL)) {
        if (!dt_u32(f, node, "clock-frequency", &hz) && hz) return hz;
    } else if (fdt_getprop(f, node, "qcom,clk-freq-out", NULL)) {
        if (!dt_u32(f, node, "qcom,clk-freq-out", &hz) && hz) return hz;
    }
    return kshim_dt_qup_android(f, node) ? 400000U : 100000U;
}

/* The wrapper of an engine node: its "qcom,geni-se-qup" parent (mainline) or
 * its "qcom,wrapper-core" target (Android; the focused fixtures may omit the
 * wrapper compatible). -1 when neither exists. */
static int wrapper_node(const void *f, int node)
{
    int parent = dt_parent_offset(f, node);
    if (parent >= 0 && !dt_node_check_compatible(f, parent, "qcom,geni-se-qup")) return parent;
    if (fdt_getprop(f, node, "qcom,wrapper-core", NULL)) return dt_reference(f, node, "qcom,wrapper-core", 0);
    return -1;
}

static bool has_available_child(const void *f, int node)
{
    int child;
    fdt_for_each_subnode(child, f, node)
        if (dt_available(f, child)) return true;
    return false;
}

bool kshim_dt_qup_window_used(const void *f, const struct kshim_qup_se *se)
{
    if (!f || !se) return false;
    for (unsigned i = 0; i < 3; ++i) {
        int n = se->nodes[i];
        if (n < 0 || !dt_available(f, n)) continue;
        switch (kshim_dt_qup_protocol(f, n)) {
        case KSHIM_QUP_PROTOCOL_UART: return true;
        case KSHIM_QUP_PROTOCOL_I2C:
        case KSHIM_QUP_PROTOCOL_SPI:
            if (has_available_child(f, n)) return true;
            break;
        default: break;
        }
    }
    return false;
}

static void set_owner_facts(const void *f, struct kshim_qup_se *se, int node)
{
    se->node = node;
    se->dt_protocol = kshim_dt_qup_protocol(f, node);
    se->clock_frequency = 0;
    uint32_t hz;
    if (se->dt_protocol == KSHIM_QUP_PROTOCOL_I2C &&
        ((fdt_getprop(f, node, "clock-frequency", NULL) && !dt_u32(f, node, "clock-frequency", &hz)) ||
         (fdt_getprop(f, node, "qcom,clk-freq-out", NULL) && !dt_u32(f, node, "qcom,clk-freq-out", &hz))))
        se->clock_frequency = hz;
}

/* Untranslated first "reg" address and its parent. dt_reg() only accepts
 * available nodes; disabled nodes that name a known window are matched as
 * siblings by this raw address (every DT here keeps them siblings). */
static int raw_base(const void *f, int node, int *parent, uint64_t *base)
{
    uint32_t ac = 2, sc = 1;
    int len;
    *parent = dt_parent_offset(f, node);
    if (*parent < 0) return -1;
    if (fdt_getprop(f, *parent, "#address-cells", NULL) && dt_u32(f, *parent, "#address-cells", &ac)) return -1;
    if (fdt_getprop(f, *parent, "#size-cells", NULL) && dt_u32(f, *parent, "#size-cells", &sc)) return -1;
    const fdt32_t *p = fdt_getprop(f, node, "reg", &len);
    if (!p || !ac || ac > 2 || sc > 2 || len < (int)((ac + sc) * 4U)) return -1;
    *base = ac == 2 ? ((uint64_t)fdt32_to_cpu(p[0]) << 32) | fdt32_to_cpu(p[1]) : fdt32_to_cpu(p[0]);
    return 0;
}

static bool add_node(struct kshim_qup_se *se, int node)
{
    for (unsigned k = 0; k < 3; ++k) {
        if (se->nodes[k] == node) return true;
        if (se->nodes[k] < 0) { se->nodes[k] = node; return true; }
    }
    return false;
}

int kshim_dt_qup_discover(const void *f, struct kshim_qup_wrapper *wrappers,
                          unsigned max_wrappers, unsigned *wrapper_count,
                          struct kshim_qup_se *ses, unsigned max_ses, unsigned *se_count)
{
    /* Scratch per window: its wrapper node (from the first node naming one)
     * and the untranslated address of its first available node. */
    int wrapper_of[KSHIM_QUP_MAX_SE], raw_parent[KSHIM_QUP_MAX_SE];
    uint64_t raw[KSHIM_QUP_MAX_SE];
    unsigned count = 0, wcount = 0;
    int status = 0;
    if (!f || !wrappers || !wrapper_count || !ses || !se_count) return ERROR_INVALID_PARAMETER;
    if (max_ses > KSHIM_QUP_MAX_SE) max_ses = KSHIM_QUP_MAX_SE;
    *wrapper_count = *se_count = 0;
    /* Pass 1 creates a window per available node, so the many disabled
     * engines of an Android DT never take a slot; pass 2 adds the disabled
     * nodes that name a known window. */
    for (unsigned pass = 0; pass < 2; ++pass) {
        for (int n = dt_next_node(f, -1); n >= 0; n = dt_next_node(f, n)) {
            if (se_kind(f, n) < 0) continue;
            bool available = dt_available(f, n);
            if ((pass != 0) == available) continue;
            int parent;
            uint64_t address;
            if (raw_base(f, n, &parent, &address)) continue;
            unsigned i;
            for (i = 0; i < count && (raw_parent[i] != parent || raw[i] != address); ++i) {}
            if (!pass) {
                struct dt_range reg;
                if (dt_reg(f, n, 0, &reg) || !reg.base || reg.size < SE_MIN_WINDOW ||
                    reg.base > UINTPTR_MAX - reg.size) continue;
                if (i == count) {
                    if (count == max_ses) { status = KSHIM_IO_ERR_OVERFLOW; continue; }
                    memset(&ses[i], 0, sizeof(ses[i]));
                    ses[i].owner_node = -1;
                    for (unsigned k = 0; k < 3; ++k) ses[i].nodes[k] = -1;
                    ses[i].base = reg.base;
                    ses[i].size = reg.size;
                    set_owner_facts(f, &ses[i], n);
                    wrapper_of[i] = -1;
                    raw_parent[i] = parent;
                    raw[i] = address;
                    ++count;
                } else if (reg.size < ses[i].size) {
                    ses[i].size = reg.size;
                }
            } else if (i == count) {
                continue;
            }
            if (!add_node(&ses[i], n)) continue; /* more than three nodes: ignored */
            if (wrapper_of[i] < 0) wrapper_of[i] = wrapper_node(f, n);
        }
    }
    for (unsigned i = 0; i < count; ++i) {
        struct kshim_qup_se *se = &ses[i];
        se->index = i;
        se->wrapper = NULL;
        int w = wrapper_of[i];
        if (w < 0) continue;
        unsigned j;
        for (j = 0; j < wcount && wrappers[j].node != w; ++j) {}
        if (j == wcount) {
            if (wcount == max_wrappers) { status = KSHIM_IO_ERR_OVERFLOW; continue; }
            struct dt_range reg = {0, 0};
            memset(&wrappers[j], 0, sizeof(wrappers[j]));
            wrappers[j].node = w;
            if (fdt_getprop(f, w, "reg", NULL) && !dt_reg(f, w, 0, &reg)) {
                wrappers[j].base = reg.base;
                wrappers[j].size = reg.size;
            }
            wrappers[j].index = j;
            ++wcount;
        }
        se->wrapper = &wrappers[j];
    }
    *se_count = count;
    *wrapper_count = wcount;
    return status;
}
