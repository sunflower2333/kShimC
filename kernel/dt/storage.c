/* SPDX-License-Identifier: MIT */
/* Device-tree discovery for Qualcomm UFS ("qcom,ufshc") and SDHCI
 * ("qcom,sdhci-msm-v5") controllers, plus the apps RSC / CMD DB used to
 * vote their rails. Accepts the Android vendor and mainline bindings. */
#include <kshim/storage_probe.h>
#include <qcom_dt.h>

#include <stdio.h>
#include <string.h>

#ifndef KSHIM_STORAGE_ANDROID_PINS
#define KSHIM_STORAGE_ANDROID_PINS 0
#endif
/* Mainline GDSC power domains resolve through the kernel/soc clock tables
 * when they are built (CONFIG_KSHIM_GPIO/CLK, cmake/io/platform.cmake). */
#if __has_include(<config.h>)
#include <config.h>
#endif
#if defined(CONFIG_KSHIM_GPIO) || defined(CONFIG_KSHIM_CLK)
#define KSHIM_STORAGE_SOC_GDSC 1
#include <kshim/soc_tables.h>
#else
#define KSHIM_STORAGE_SOC_GDSC 0
#endif

#if KSHIM_STORAGE_SOC_GDSC
/* Mainline: power-domains = <&gcc UFS_PHY_GDSC>; the GDSCR (PWR_ON bit 31)
 * from the SoC table (SM8650: ufs_phy_gdsc at GCC + 0x77004). */
static uint64_t soc_gdsc(const void *f, int node)
{
    struct dt_spec spec;
    struct dt_range reg;
    struct kshim_soc_clk leaf;
    if (dt_specifier(f, node, "power-domains", "#power-domain-cells", 0, &spec) || spec.count != 1)
        return 0;
    const char *compatible = dt_string(f, spec.node, "compatible");
    const struct kshim_soc_clock_controller *c = kshim_soc_clock_controller(compatible);
    if (!c || !kshim_soc_clock_leaf(c, KSHIM_SOC_CLK_SPACE_GDSC, spec.args[0], &leaf) ||
        dt_reg(f, spec.node, 0, &reg) || leaf.halt_reg + 4U > reg.size) return 0;
    return reg.base + leaf.halt_reg;
}
#endif

unsigned kshim_dt_storage_soc(const void *f)
{
    static const struct { const char *compatible; unsigned soc; } socs[] = {
        {"qcom,sm8150", 8150}, {"qcom,msmnile", 8150},
        {"qcom,sm8250", 8250}, {"qcom,kona", 8250},
        {"qcom,sm8450", 8450}, {"qcom,waipio", 8450},
    };
    for (unsigned i = 0; i < sizeof(socs) / sizeof(socs[0]); ++i)
        if (!dt_node_check_compatible(f, 0, socs[i].compatible)) return socs[i].soc;
    return 0;
}

/* "sdhc2" / "mmc1" style alias index for a node, or -1. */
static int alias_index(const void *f, int node, const char *prefix)
{
    int aliases = fdt_path_offset(f, "/aliases");
    if (aliases < 0) return -1;
    size_t plen = strlen(prefix);
    for (int p = fdt_first_property_offset(f, aliases); p >= 0; p = fdt_next_property_offset(f, p)) {
        const char *name; int len;
        const char *path = fdt_getprop_by_offset(f, p, &name, &len);
        if (!path || len < 2 || path[len - 1] || strncmp(name, prefix, plen)) continue;
        const char *digits = name + plen;
        if (!*digits) continue;
        int value = 0;
        for (const char *d = digits; *d; ++d) {
            if (*d < '0' || *d > '9' || value > 99) { value = -1; break; }
            value = value * 10 + (*d - '0');
        }
        if (value >= 0 && fdt_path_offset(f, path) == node) return value;
    }
    return -1;
}

static int find_ufs(const void *f, struct kshim_storage_dt *out)
{
    for (int n = dt_node_by_compatible(f, -1, "qcom,ufshc"); n >= 0;
         n = dt_node_by_compatible(f, n, "qcom,ufshc")) {
        if (!dt_available(f, n)) continue;
        if (out->ufs_count == KSHIM_STORAGE_MAX_UFS) return -1; /* ambiguous */
        struct kshim_dt_ufs *u = &out->ufs[out->ufs_count];
        memset(u, 0, sizeof(*u));
        u->node = n;
        if (dt_reg_named(f, n, "std", &u->hci) && dt_reg(f, n, 0, &u->hci)) return -1;
        if (u->hci.size < 0x100) return -1;
        int phy = dt_reference(f, n, "phys", 0);
        if (phy >= 0 && dt_available(f, phy) && !dt_reg(f, phy, 0, &u->phy)) u->has_phy = true;
        /* Android: vdd-hba-supply is the "qcom,gdsc" node with its register. */
        int hba = dt_reference(f, n, "vdd-hba-supply", 0);
        struct dt_range gdsc;
        if (hba >= 0 && !dt_node_check_compatible(f, hba, "qcom,gdsc") &&
            !dt_reg(f, hba, 0, &gdsc) && gdsc.size >= 4)
            u->gdsc = gdsc.base;
#if KSHIM_STORAGE_SOC_GDSC
        if (!u->gdsc) u->gdsc = soc_gdsc(f, n);
#endif
        u->index = out->ufs_count++;
    }
    return 0;
}

static void card_detect(const void *f, struct kshim_dt_sdhc *s)
{
    struct dt_spec spec;
    if (dt_specifier(f, s->node, "cd-gpios", "#gpio-cells", 0, &spec) || spec.count < 1) return;
#if KSHIM_STORAGE_ANDROID_PINS
    uintptr_t address;
    if (kshim_dt_gpio(f, spec.node, spec.args[0], &address, NULL, NULL)) return;
    s->cd_io = address + 4; /* GPIO_IN_OUT: bit0 is the input level */
    s->cd_active_low = spec.count > 1 && (spec.args[1] & 1);
    s->has_cd = true;
#endif
}

static int find_sdhc(const void *f, struct kshim_storage_dt *out)
{
    for (int n = dt_node_by_compatible(f, -1, "qcom,sdhci-msm-v5"); n >= 0;
         n = dt_node_by_compatible(f, n, "qcom,sdhci-msm-v5")) {
        if (!dt_available(f, n)) continue;
        if (out->sdhc_count == KSHIM_STORAGE_MAX_SDHC) break;
        struct kshim_dt_sdhc *s = &out->sdhc[out->sdhc_count];
        memset(s, 0, sizeof(*s));
        s->node = n;
        if (dt_reg_named(f, n, "hc_mem", &s->regs) && dt_reg_named(f, n, "hc", &s->regs) &&
            dt_reg(f, n, 0, &s->regs)) return -1;
        if (s->regs.size < 0x1000) continue; /* the v5 vendor window must be inside */
        bool flag = false;
        s->non_removable = !dt_bool(f, n, "non-removable", &flag) && flag;
        int idx = alias_index(f, n, "sdhc");
        if (idx < 0) idx = alias_index(f, n, "mmc");
        s->index = idx >= 0 ? (unsigned)idx : out->sdhc_count;
        uint32_t hz = 0;
        if (fdt_getprop(f, n, "max-frequency", NULL) && !dt_u32(f, n, "max-frequency", &hz))
            s->max_hz = hz;
        card_detect(f, s);
        ++out->sdhc_count;
    }
    return 0;
}

/* Same RSC/TCS rules as the touch path: the apps DRV, ACTIVE TCS window. */
static int rsc(const void *f, int n, struct CrQupResources *r)
{
    uint32_t drv;
    struct dt_range region, tcs;
    if (!dt_available(f, n) || dt_u32(f, n, "qcom,drv-id", &drv) || drv > 4) return -1;
    if (!dt_node_check_compatible(f, n, "qcom,tcs-drv")) {
        if (dt_reg(f, n, 0, &region) || dt_reg(f, n, 1, &tcs) ||
            tcs.base <= region.base || tcs.base - region.base > UINT32_MAX) return -1;
        r->tcs_offset = (uint32_t)(tcs.base - region.base);
        r->rsc_size = r->tcs_offset + tcs.size;
    } else if (!dt_node_check_compatible(f, n, "qcom,rpmh-rsc")) {
        char name[16];
        snprintf(name, sizeof(name), "drv-%u", (unsigned)drv);
        if (dt_reg_named(f, n, name, &region) || dt_u32(f, n, "qcom,tcs-offset", &r->tcs_offset)) return -1;
        r->rsc_size = region.size;
    } else return -1;
    r->rsc_base = region.base;
    r->drv_id = drv;
    int len;
    const fdt32_t *p = fdt_getprop(f, n, "qcom,tcs-config", &len);
    if (!p || len != 32) return -1;
    unsigned offset = 0, seen = 0;
    for (int i = 0; i < 8; i += 2) {
        unsigned type = fdt32_to_cpu(p[i]), count = fdt32_to_cpu(p[i + 1]);
        if (type > 3 || (seen & (1U << type)) || count > 32 - offset) return -1;
        seen |= 1U << type;
        if (type == 2) { r->active_offset = offset; r->active_count = count; }
        offset += count;
    }
    return r->active_count && r->tcs_offset <= r->rsc_size &&
        offset * 0x2a0U <= r->rsc_size - r->tcs_offset ? 0 : -1;
}

static int find_rpmh(const void *f, struct kshim_storage_dt *out)
{
    int controller = -1, db = -1;
    for (int p = dt_next_node(f, -1); p >= 0; p = dt_next_node(f, p)) {
        const char *label = dt_string(f, p, "label");
        /* Android also labels msm-bus client nodes "apps_rsc"; only RSCs count. */
        if (label && !strcmp(label, "apps_rsc") && dt_available(f, p) &&
            (!dt_node_check_compatible(f, p, "qcom,tcs-drv") ||
             !dt_node_check_compatible(f, p, "qcom,rpmh-rsc"))) {
            if (controller >= 0) return -1;
            controller = p;
        }
    }
    if (controller < 0 || rsc(f, controller, &out->rpmh)) return -1;
    struct dt_range range;
    for (int p = dt_node_by_compatible(f, -1, "qcom,cmd-db"); p >= 0;
         p = dt_node_by_compatible(f, p, "qcom,cmd-db")) {
        if (!dt_available(f, p)) continue;
        if (db >= 0 || dt_reg(f, p, 0, &range)) return -1;
        db = p;
    }
    if (db < 0) return -1;
    if (range.size == 8) out->rpmh.cmd_db_dictionary = range.base;
    else { out->rpmh.cmd_db_base = range.base; out->rpmh.cmd_db_size = range.size; }
    out->has_rpmh = true;
    return 0;
}

int kshim_dt_storage(const void *f, struct kshim_storage_dt *out)
{
    if (!f || !out) return -1;
    memset(out, 0, sizeof(*out));
    out->soc = kshim_dt_storage_soc(f);
    if (find_ufs(f, out) || find_sdhc(f, out)) return -1;
    if (find_rpmh(f, out)) {
        out->has_rpmh = false;
        memset(&out->rpmh, 0, sizeof(out->rpmh));
    }
    return 0;
}
