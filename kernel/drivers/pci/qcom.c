/* SPDX-License-Identifier: MIT */
/*
 * Qualcomm DesignWare PCIe root complex host (CONFIG_KSHIM_PCI_QCOM).
 *
 * Inherit first. When the previous bootloader left the port running (its
 * clocks verified on through kshim_clk, PARF LTSSM enabled and the root
 * port's Data Link Layer Link Active bit set), kShimC changes nothing but
 * one outbound iATU region for configuration cycles (the bootloader's own
 * config region when it has one, else a free region), plus a free region
 * for a DT memory window that no enabled iATU region translates. Those
 * regions are snapshot and restored at quiesce; PERST#, clocks, GDSCs,
 * rails and the link are left as found.
 *
 * Otherwise the port is cold-started through CrDK PcieLib with the SoC
 * target data (qcom_pcielib.c): PERST# is asserted here, before the PCI
 * core powers the DT endpoints, and PcieShutdownPort() at quiesce stops
 * LTSSM, asserts PERST# again and releases every clock, reset, GDSC and
 * rail kShimC took (the adapter's references), after which the platform
 * restore puts pads and rails back to their state at entry.
 *
 * DT: mainline "qcom,pcie-sm8650" style (reg-names parf/dbi/elbi/atu/
 * config[/mhi], "phys" -> QMP PHY node, perst-gpios) and Android
 * "qcom,pci-msm" (dm_core/iatu/conf, a "phy" region, perst-gpio).
 * Configuration access facts (DWC own-config for the root port, CFG0 for
 * device 0 of the secondary bus only, CFG1 deeper, BDF in the iATU target
 * bus[31:24] dev[23:19] fn[18:16]) follow CrDK PcieConfigAccess().
 */
#include "qcom.h"

#include <config.h>
#include <kshim/clk.h>
#include <kshim/pinctrl.h>
#include <stdio.h>
#include <string.h>

#define LOG(...) printf("PCIe: " __VA_ARGS__)

static const char *const Compatibles[] = {
    "qcom,pcie-sm8650", "qcom,pcie-sm8550", "qcom,pcie-sm8450-pcie0",
    "qcom,pcie-sm8450-pcie1", "qcom,pcie-sm8350", "qcom,pcie-sm8250",
    "qcom,pcie-sm8150", "qcom,pci-msm", NULL,
};

static struct qcom_pcie Hosts[QCOM_PCIE_MAX];
static unsigned HostCount;

static struct qcom_pcie *qcom(struct kshim_pci_host *h) { return (struct qcom_pcie *)h; }

static bool compatible(const void *f, int node)
{
    for (unsigned i = 0; Compatibles[i]; ++i)
        if (!dt_node_check_compatible(f, node, Compatibles[i])) return true;
    return false;
}

static int next_host(const void *f, int node)
{
    for (node = dt_next_node(f, node); node >= 0; node = dt_next_node(f, node))
        if (compatible(f, node) && dt_available(f, node)) return node;
    return -1;
}

/* reg-names of both DT families. */
static int region(const void *f, int node, const char *a, const char *b, struct dt_range *r)
{
    if (!dt_reg_named(f, node, a, r)) return 0;
    if (b && !dt_reg_named(f, node, b, r)) return 0;
    return ERROR_NOT_FOUND;
}

static int parse(const void *f, int node, struct qcom_pcie *qp)
{
    memset(qp, 0, sizeof(*qp));
    qp->fdt = f;
    qp->node = node;
    qp->phy_node = qp->tlmm_node = -1;
    qp->port = -1;
    qp->cfg_region = -1;
    if (region(f, node, "parf", NULL, &qp->parf) || region(f, node, "dbi", "dm_core", &qp->dbi) ||
        region(f, node, "atu", "iatu", &qp->atu) || region(f, node, "config", "conf", &qp->config))
        return ERROR_INVALID_PARAMETER;
    qp->has_elbi = !region(f, node, "elbi", NULL, &qp->elbi);
    qp->has_mhi = !region(f, node, "mhi", NULL, &qp->mhi);
    int phy = dt_reference(f, node, "phys", 0);
    if (phy >= 0 && !dt_reg(f, phy, 0, &qp->phy)) {
        qp->phy_node = phy;
        qp->has_phy = true;
    } else if (!region(f, node, "phy", NULL, &qp->phy)) {
        qp->has_phy = true;
    }
    if (qp->atu.size < QCOM_ATU_STRIDE || qp->config.size < 0x1000 || qp->dbi.size < 0x100)
        return ERROR_INVALID_PARAMETER;
    int status = kshim_pci_host_parse_dt(f, node, &qp->host);
    if (status) return status;
    qp->host.name = fdt_get_name(f, node, NULL);
    qp->atu_regions = (unsigned)(qp->atu.size / QCOM_ATU_STRIDE);
    if (qp->atu_regions > 16) qp->atu_regions = 16;
    return 0;
}

/* ---- map --------------------------------------------------------------- */

static int map_one(struct kshim_resources *r, void *context)
{
    struct qcom_pcie *qp = context;
    const struct dt_range *regs[] = {&qp->parf, &qp->dbi, &qp->atu, &qp->config,
                                     qp->has_elbi ? &qp->elbi : NULL,
                                     qp->has_mhi ? &qp->mhi : NULL,
                                     qp->has_phy ? &qp->phy : NULL};
    for (unsigned i = 0; i < sizeof(regs) / sizeof(regs[0]); ++i) {
        if (!regs[i]) continue;
        int status = kshim_pci_map_device(r, regs[i]->base, regs[i]->size);
        if (status) return status;
    }
    return kshim_pci_map_windows(r, &qp->host);
}

int kshim_pci_qcom_map(const void *f, struct kshim_resources *r)
{
    static struct qcom_pcie scratch;
    for (int n = next_host(f, -1); n >= 0; n = next_host(f, n)) {
        if (parse(f, n, &scratch)) {
            kshim_pci_mark_node(n, false);
            continue;
        }
        if (kshim_pci_map_guarded(r, n, map_one, &scratch))
            LOG("%s: windows not mapped, host skipped\n", scratch.host.name);
    }
    return 0;
}

/* ---- register access --------------------------------------------------- */

static uint32_t rd32(uint64_t a) { return pci_read32((uintptr_t)a); }
static void wr32(uint64_t a, uint32_t v) { pci_write32((uintptr_t)a, v); }

static uint8_t dbi_cap(struct qcom_pcie *qp, uint8_t id)
{
    uint8_t ptr = (uint8_t)(rd32(qp->dbi.base + 0x34) & 0xfcU);
    for (unsigned guard = 0; ptr >= 0x40U && guard < 48U; ++guard) {
        uint32_t header = rd32(qp->dbi.base + ptr);
        if ((header & 0xffU) == id) return ptr;
        ptr = (uint8_t)((header >> 8) & 0xfcU);
    }
    return 0;
}

static bool link_active(struct qcom_pcie *qp)
{
    if (!qp->exp_cap) qp->exp_cap = dbi_cap(qp, KSHIM_PCI_CAP_ID_EXP);
    if (!qp->exp_cap) return false;
    uint32_t v = rd32(qp->dbi.base + ((qp->exp_cap + QCOM_EXP_LINK_STATUS) & ~3U));
    return ((v >> 16) & QCOM_EXP_LINK_ACTIVE) != 0;
}

static uint64_t atu_reg(const struct qcom_pcie *qp, unsigned region, unsigned reg)
{
    return qp->atu.base + (uint64_t)region * QCOM_ATU_STRIDE + reg;
}

static const unsigned AtuRegs[QCOM_ATU_SAVED_REGS] = {
    QCOM_ATU_CTRL1, QCOM_ATU_CTRL2, QCOM_ATU_LOWER_BASE, QCOM_ATU_UPPER_BASE,
    QCOM_ATU_LIMIT, QCOM_ATU_LOWER_TARGET, QCOM_ATU_UPPER_TARGET,
};

static bool atu_enabled(const struct qcom_pcie *qp, unsigned region)
{
    return (rd32(atu_reg(qp, region, QCOM_ATU_CTRL2)) & QCOM_ATU_ENABLE) != 0;
}

static bool atu_owned(const struct qcom_pcie *qp, unsigned region)
{
    for (unsigned i = 0; i < qp->owned_count; ++i)
        if (qp->owned[i].region == region) return true;
    return false;
}

static int atu_take(struct qcom_pcie *qp, unsigned region)
{
    if (qp->owned_count == QCOM_ATU_OWNED_MAX) return KSHIM_IO_ERR_NO_MEMORY;
    struct qcom_atu_owned *o = &qp->owned[qp->owned_count];
    o->region = region;
    for (unsigned i = 0; i < QCOM_ATU_SAVED_REGS; ++i)
        o->saved[i] = rd32(atu_reg(qp, region, AtuRegs[i]));
    return (int)qp->owned_count++;
}

/* A disabled region that answers (its base register reads back). */
static int atu_free_region(struct qcom_pcie *qp)
{
    for (unsigned r = 0; r < qp->atu_regions; ++r) {
        if (atu_owned(qp, r) || atu_enabled(qp, r)) continue;
        uint64_t base = atu_reg(qp, r, QCOM_ATU_LOWER_BASE);
        uint32_t saved = rd32(base);
        wr32(base, 0x5a5a0000U);
        bool present = rd32(base) == 0x5a5a0000U;
        wr32(base, saved);
        if (present) return (int)r;
    }
    return -1;
}

static int atu_program(struct qcom_pcie *qp, unsigned region, uint32_t type, uint64_t cpu,
                       uint64_t pci, uint64_t size)
{
    uint64_t limit = cpu + size - 1;
    if (!size || (cpu >> 32) != (limit >> 32)) return ERROR_NOT_SUPPORTED;
    wr32(atu_reg(qp, region, QCOM_ATU_LOWER_BASE), (uint32_t)cpu);
    wr32(atu_reg(qp, region, QCOM_ATU_UPPER_BASE), (uint32_t)(cpu >> 32));
    wr32(atu_reg(qp, region, QCOM_ATU_LIMIT), (uint32_t)limit);
    wr32(atu_reg(qp, region, QCOM_ATU_LOWER_TARGET), (uint32_t)pci);
    wr32(atu_reg(qp, region, QCOM_ATU_UPPER_TARGET), (uint32_t)(pci >> 32));
    wr32(atu_reg(qp, region, QCOM_ATU_CTRL1), type);
    wr32(atu_reg(qp, region, QCOM_ATU_CTRL2), QCOM_ATU_ENABLE);
    for (unsigned retry = 0; retry < 5; ++retry) {
        if (atu_enabled(qp, region)) return 0;
        kshim_pci_delay_us(9);
    }
    return ERROR_TIMEOUT;
}

static void atu_restore_all(struct qcom_pcie *qp)
{
    while (qp->owned_count) {
        const struct qcom_atu_owned *o = &qp->owned[--qp->owned_count];
        wr32(atu_reg(qp, o->region, QCOM_ATU_CTRL2), 0);
        for (unsigned i = QCOM_ATU_SAVED_REGS; i-- > 0;)
            if (AtuRegs[i] != QCOM_ATU_CTRL2) wr32(atu_reg(qp, o->region, AtuRegs[i]), o->saved[i]);
        wr32(atu_reg(qp, o->region, QCOM_ATU_CTRL2), o->saved[1]);
    }
    qp->cfg_region = -1;
}

/* Whether an enabled iATU region already translates `w` as found. */
static bool window_translated(struct qcom_pcie *qp, const struct kshim_pci_window *w)
{
    uint32_t want = w->type == KSHIM_PCI_WINDOW_IO ? QCOM_ATU_TYPE_IO : QCOM_ATU_TYPE_MEM;
    for (unsigned r = 0; r < qp->atu_regions; ++r) {
        if (!atu_enabled(qp, r)) continue;
        if ((rd32(atu_reg(qp, r, QCOM_ATU_CTRL1)) & QCOM_ATU_TYPE_MASK) != want) continue;
        uint64_t hi = (uint64_t)rd32(atu_reg(qp, r, QCOM_ATU_UPPER_BASE)) << 32;
        uint64_t base = hi | rd32(atu_reg(qp, r, QCOM_ATU_LOWER_BASE));
        uint64_t limit = hi | rd32(atu_reg(qp, r, QCOM_ATU_LIMIT));
        uint64_t target = (uint64_t)rd32(atu_reg(qp, r, QCOM_ATU_UPPER_TARGET)) << 32 |
                          rd32(atu_reg(qp, r, QCOM_ATU_LOWER_TARGET));
        if (base <= w->cpu_base && limit >= w->cpu_base + w->size - 1 &&
            target + (w->cpu_base - base) == w->pci_base) return true;
    }
    return false;
}

static void drop_window(struct kshim_pci_host *h, unsigned i)
{
    memmove(&h->windows[i], &h->windows[i + 1], (h->window_count - i - 1) * sizeof(h->windows[0]));
    --h->window_count;
}

static int setup_inherited(struct qcom_pcie *qp)
{
    /* Configuration region: the bootloader's own when it has one. */
    int region = -1;
    for (unsigned r = 0; r < qp->atu_regions && region < 0; ++r) {
        if (!atu_enabled(qp, r)) continue;
        uint32_t type = rd32(atu_reg(qp, r, QCOM_ATU_CTRL1)) & QCOM_ATU_TYPE_MASK;
        uint64_t base = (uint64_t)rd32(atu_reg(qp, r, QCOM_ATU_UPPER_BASE)) << 32 |
                        rd32(atu_reg(qp, r, QCOM_ATU_LOWER_BASE));
        if ((type == QCOM_ATU_TYPE_CFG0 || type == QCOM_ATU_TYPE_CFG1) && base == qp->config.base)
            region = (int)r;
    }
    if (region < 0) region = atu_free_region(qp);
    if (region < 0) {
        LOG("%s: no iATU region for configuration cycles\n", qp->host.name);
        return ERROR_NOT_SUPPORTED;
    }
    qp->cfg_region = atu_take(qp, (unsigned)region);
    if (qp->cfg_region < 0) return ERROR_NOT_SUPPORTED;
    qp->cfg_type = 0;
    /* Memory windows the bootloader did not translate get a free region;
     * a window that cannot be translated is not used for BARs. */
    for (unsigned i = 0; i < qp->host.window_count;) {
        struct kshim_pci_window *w = &qp->host.windows[i];
        if (window_translated(qp, w)) { ++i; continue; }
        int free_region = w->type == KSHIM_PCI_WINDOW_IO ? -1 : atu_free_region(qp);
        int slot = free_region >= 0 ? atu_take(qp, (unsigned)free_region) : -1;
        if (slot >= 0 && !atu_program(qp, (unsigned)free_region, QCOM_ATU_TYPE_MEM, w->cpu_base,
                                      w->pci_base, w->size)) {
            LOG("%s: iATU region %d maps window 0x%llx+0x%llx\n", qp->host.name, free_region,
                (unsigned long long)w->cpu_base, (unsigned long long)w->size);
            ++i;
            continue;
        }
        LOG("%s: %s window 0x%llx not translated, not used\n", qp->host.name,
            w->type == KSHIM_PCI_WINDOW_IO ? "I/O" : "memory", (unsigned long long)w->cpu_base);
        drop_window(&qp->host, i);
    }
    return 0;
}

static int inherited_cfg(struct qcom_pcie *qp, uint8_t bus, uint8_t devfn, uint16_t reg,
                         unsigned width, bool write, uint32_t *value)
{
    uint64_t address;
    if (bus == qp->host.bus_start) {
        if (devfn) goto absent;
        if (reg + width > qp->dbi.size) goto absent;
        address = qp->dbi.base + reg;
    } else {
        uint32_t numbers = rd32(qp->dbi.base + 0x18);
        unsigned secondary = (numbers >> 8) & 0xffU, subordinate = (numbers >> 16) & 0xffU;
        if (bus < secondary || bus > subordinate || !secondary || !link_active(qp)) goto absent;
        if (bus == secondary && KSHIM_PCI_SLOT(devfn)) goto absent; /* one device below a port */
        if (qp->cfg_region < 0 || reg + width > qp->config.size) goto absent;
        uint32_t type = bus == secondary ? QCOM_ATU_TYPE_CFG0 : QCOM_ATU_TYPE_CFG1;
        uint32_t target = (uint32_t)bus << 24 | (uint32_t)KSHIM_PCI_SLOT(devfn) << 19 |
                          (uint32_t)KSHIM_PCI_FUNC(devfn) << 16;
        if (qp->cfg_type != type || qp->cfg_target != target) {
            unsigned region = qp->owned[qp->cfg_region].region;
            int status = atu_program(qp, region, type, qp->config.base, target,
                                     qp->config.size);
            if (status) return status;
            qp->cfg_type = type;
            qp->cfg_target = target;
        }
        address = qp->config.base + reg;
    }
    if (write) pci_mmio_write((uintptr_t)address, width, *value);
    else *value = pci_mmio_read((uintptr_t)address, width);
    return 0;
absent:
    if (!write) *value = width == 4 ? 0xffffffffU : (1U << (8 * width)) - 1;
    return 0;
}

/* ---- host ops ---------------------------------------------------------- */

static int qcom_read(struct kshim_pci_host *h, uint8_t bus, uint8_t devfn, uint16_t reg,
                     unsigned width, uint32_t *value)
{
    struct qcom_pcie *qp = qcom(h);
    if (bus < h->bus_start || bus > h->bus_end) return ERROR_NOT_FOUND;
    if (qp->live) return inherited_cfg(qp, bus, devfn, reg, width, false, value);
    if (qp->cold) return kshim_pci_qcom_lib_config(qp, bus, devfn, reg, width, false, value);
    *value = width == 4 ? 0xffffffffU : (1U << (8 * width)) - 1;
    return 0;
}

static int qcom_write(struct kshim_pci_host *h, uint8_t bus, uint8_t devfn, uint16_t reg,
                      unsigned width, uint32_t value)
{
    struct qcom_pcie *qp = qcom(h);
    if (bus < h->bus_start || bus > h->bus_end) return ERROR_NOT_FOUND;
    if (qp->live) return inherited_cfg(qp, bus, devfn, reg, width, true, &value);
    if (qp->cold) return kshim_pci_qcom_lib_config(qp, bus, devfn, reg, width, true, &value);
    return 0;
}

static int qcom_start(struct kshim_pci_host *h)
{
    struct qcom_pcie *qp = qcom(h);
    if (qp->live) {
        int status = setup_inherited(qp);
        if (status) atu_restore_all(qp);
        return status;
    }
    int status = kshim_pci_qcom_lib_start(qp);
    if (status && status != ERROR_NOT_READY) return status;
    qp->cold = true;
    return status;
}

static int qcom_quiesce(struct kshim_pci_host *h)
{
    struct qcom_pcie *qp = qcom(h);
    if (qp->live) {
        atu_restore_all(qp);
        return 0;
    }
    int status = 0;
    if (qp->cold) {
        status = kshim_pci_qcom_lib_stop(qp);
        if (status) return status;
        qp->cold = false;
    }
    kshim_pci_qcom_perst_release(qp);
    return 0;
}

static const struct kshim_pci_host_ops QcomOps = {
    .start = qcom_start,
    .read = qcom_read,
    .write = qcom_write,
    .quiesce = qcom_quiesce,
};

/* ---- probe ------------------------------------------------------------- */

enum { CLOCKS_OFF, CLOCKS_ON, CLOCKS_UNKNOWN };

/* clock-names of the mainline and Android bindings, by role. */
static int find_clock(const void *f, int node, const char *role, const char *suffix,
                      struct kshim_clk **clk)
{
    int status = kshim_clk_get(f, node, role, clk);
    if (status != ERROR_NOT_FOUND) return status;
    int count = fdt_stringlist_count(f, node, "clock-names");
    size_t sl = strlen(suffix);
    for (int i = 0; i < count; ++i) {
        const char *name = fdt_stringlist_get(f, node, "clock-names", i, NULL);
        size_t nl = name ? strlen(name) : 0;
        if (nl > sl && !strcmp(name + nl - sl, suffix)) return kshim_clk_get(f, node, name, clk);
    }
    return ERROR_NOT_FOUND;
}

/* PARF needs cfg_ahb; DBI and config space go through the AXI slave and
 * master ports. All of them must be verified running before any MMIO. */
static int clocks_state(struct qcom_pcie *qp)
{
    static const struct { const char *role, *suffix; } roles[] = {
        {"cfg", "_cfg_ahb_clk"}, {"bus_master", "_mstr_axi_clk"}, {"bus_slave", "_slv_axi_clk"},
    };
    struct kshim_clk *gdsc;
    int status = kshim_clk_get_power_domain(qp->fdt, qp->node, 0, &gdsc);
    if (!status && !kshim_clk_is_on(gdsc)) return CLOCKS_OFF;
    if (status && status != ERROR_NOT_FOUND) return CLOCKS_UNKNOWN;
    for (unsigned i = 0; i < sizeof(roles) / sizeof(roles[0]); ++i) {
        struct kshim_clk *clk;
        if (find_clock(qp->fdt, qp->node, roles[i].role, roles[i].suffix, &clk))
            return CLOCKS_UNKNOWN;
        if (!kshim_clk_is_on(clk)) return CLOCKS_OFF;
    }
    return CLOCKS_ON;
}

static void find_perst(struct qcom_pcie *qp)
{
    static const char *const props[] = {"perst-gpios", "perst-gpio", NULL};
    for (unsigned i = 0; props[i]; ++i)
        if (!kshim_gpio_from_dt(qp->fdt, qp->node, props[i], 0, &qp->perst)) {
            qp->has_perst = true;
            break;
        }
    int child;
    fdt_for_each_subnode(child, qp->fdt, qp->node) {
        if (qp->has_perst) break;
        if (!kshim_gpio_from_dt(qp->fdt, child, "reset-gpios", 0, &qp->perst)) qp->has_perst = true;
    }
    if (qp->has_perst) qp->tlmm_node = qp->perst.provider;
}

static int probe_one(const void *f, int node, struct qcom_pcie *qp)
{
    int status = parse(f, node, qp);
    if (status) return status;
    qp->host.ops = &QcomOps;
    find_perst(qp);
    bool cold_ok = !kshim_pci_qcom_lib_select(qp);
    switch (clocks_state(qp)) {
    case CLOCKS_UNKNOWN:
        LOG("%s: clock state cannot be verified, host not touched\n", qp->host.name);
        return ERROR_NOT_SUPPORTED;
    case CLOCKS_ON: {
        uint32_t ltssm = rd32(qp->parf.base + QCOM_PARF_LTSSM);
        uint32_t state = rd32(qp->dbi.base + QCOM_DBI_PORT_DEBUG0) & 0x3fU;
        qp->live = (ltssm & QCOM_PARF_LTSSM_ENABLE) && link_active(qp);
        LOG("%s: clocks on, LTSSM %s state 0x%02x, link %s\n", qp->host.name,
            ltssm & QCOM_PARF_LTSSM_ENABLE ? "enabled" : "off", (unsigned)state,
            qp->live ? "up: inherited" : "down");
        break;
    }
    default:
        LOG("%s: clocks off\n", qp->host.name);
        break;
    }
    qp->host.inherited = qp->live;
    if (qp->live) return 0;
    if (!cold_ok) {
        LOG("%s: link down and no cold-start target data\n", qp->host.name);
        return ERROR_NOT_SUPPORTED;
    }
    /* Endpoints are powered by the core next: hold them in reset. */
    status = kshim_pci_qcom_perst(qp, true);
    if (status) LOG("%s: PERST# not asserted: %s\n", qp->host.name, kshim_io_strerror(status));
    return 0;
}

int kshim_pci_qcom_probe(const void *f)
{
    int result = 0;
    for (int n = next_host(f, -1); n >= 0; n = next_host(f, n)) {
        if (kshim_pci_node_unmapped(n)) continue;
        if (HostCount == QCOM_PCIE_MAX) break;
        struct qcom_pcie *qp = &Hosts[HostCount];
        int status = probe_one(f, n, qp);
        if (!status) status = kshim_pci_host_register(&qp->host);
        if (status) {
            kshim_pci_qcom_perst_release(qp);
            result = status;
            continue;
        }
        ++HostCount;
    }
    return result;
}
