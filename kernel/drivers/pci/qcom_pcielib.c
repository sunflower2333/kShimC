/* SPDX-License-Identifier: MIT */
/*
 * CrDK PcieLib adapter for the Qualcomm host (CONFIG_KSHIM_PCI_QCOM, CrDK
 * built with CRDK_PCIE). PcieLib (Library/PcieLib/pcie.c, MIT) runs the
 * cold start unmodified: PERST#, rails, GDSCs, controller clocks and BCR,
 * PARF, QMP PHY tables, root-port DBI, BDF-to-SID table, iATU, link
 * training, and the reverse at PcieShutdownPort(). This file gives it a
 * PcieIoOps onto kShimC services:
 *  - MMIO through pci_mmio_* (identity mapped), delays and time;
 *  - clocks, resets and GDSCs through kshim_clk: the DT consumer first
 *    (controller node for the controller slice, QMP PHY node for the PHY
 *    slice, by clock-names/reset-names/power-domains), then the target's
 *    Linux name through kshim_clk_get_by_name(); a rate request is applied
 *    with kshim_clk_set_rate() when the branch is off (19.2 MHz: CXO; other
 *    rates from the SoC rate rows, e.g. the SM8650 rchng 100 MHz from
 *    GPLL0_OUT_EVEN, which refuses the cold start when that PLL output is
 *    not verified running);
 *  - the PHY AUX and PIPE muxes (PcieTargetPhy Aux/PipeMux*) through
 *    kshim_clk_set_parent() on the named mux leaf;
 *  - supplies through kshim_regulator_get() on the PHY node, then the
 *    controller node; a supply the DT does not describe is left as found;
 *  - PERST# through kshim_gpio (DT perst-gpios first, else the target pin);
 *    WAKE#/CLKREQ# come from the controller's pinctrl "default" state;
 *  - interconnect and SMMU: kShimC votes no ICC path and does not program
 *    the SMMU (docs/io-stack.md): both callbacks only log that the state is
 *    assumed inherited.
 * Every resource taken here is released by PcieShutdownPort() through the
 * same callbacks (disable/deassert), and the platform restore steps put
 * anything else back to its state at entry.
 */
#include "qcom.h"
#include <dt.h>

#include <config.h>
#include <kshim/clk.h>
#include <kshim/pinctrl.h>
#include <kshim/regulator.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#define LOG(...) printf("PCIe: " __VA_ARGS__)
#define CLOCK_CACHE 64U

struct target_choice {
    const char *compatible;
    const PcieTargetContext *(*get)(void);
    const char *name;
};

static const PcieTargetContext *sm8450(void) { return KshimCrdkSm8450PcieContext(); }
static const PcieTargetContext *sm8250(void) { return KshimCrdkSm8250PcieContext(); }

/* Host compatible -> target data. Android kona DTs ("qcom,pci-msm") are
 * recognised by the machine compatible. */
static const struct target_choice Targets[] = {
    {"qcom,pcie-sm8650", kshim_pci_qcom_sm8650_target, "kShimC SM8650"},
    {"qcom,pcie-sm8450-pcie0", sm8450, "CrDK Sm8450"},
    {"qcom,pcie-sm8450-pcie1", sm8450, "CrDK Sm8450"},
    {"qcom,pcie-sm8250", sm8250, "CrDK Sm8250"},
};

struct clock_slot {
    const void *key;
    struct kshim_clk *clk;
    bool enabled;
};

static struct {
    PcieDeviceContext *lib;
    const PcieTargetContext *target;
    struct qcom_pcie *ports[PCIE_MAX_CONTROLLERS];
    struct clock_slot clocks[CLOCK_CACHE];
    unsigned clock_count;
    bool icc_logged, smmu_logged;
} Lib;

int kshim_pci_qcom_status(CR_STATUS s)
{
    switch (s) {
    case CR_SUCCESS: return 0;
    case CR_TIMEOUT: return ERROR_TIMEOUT;
    case CR_BUSY: case CR_ALREADY_STARTED: return ERROR_BUSY;
    case CR_INVALID_PARAMETER: return ERROR_INVALID_PARAMETER;
    case CR_UNSUPPORTED: return ERROR_NOT_SUPPORTED;
    case CR_NOT_FOUND: return ERROR_NOT_FOUND;
    case CR_NOT_READY: return ERROR_NOT_READY;
    case CR_OUT_OF_RESOURCES: return KSHIM_IO_ERR_NO_MEMORY;
    default: return ERROR_DEVICE_ERROR;
    }
}

static CR_STATUS cr(int status)
{
    switch (status) {
    case 0: return CR_SUCCESS;
    case ERROR_TIMEOUT: return CR_TIMEOUT;
    case ERROR_BUSY: return CR_BUSY;
    case ERROR_INVALID_PARAMETER: return CR_INVALID_PARAMETER;
    case ERROR_NOT_SUPPORTED: return CR_UNSUPPORTED;
    case ERROR_NOT_FOUND: return CR_NOT_FOUND;
    case ERROR_NOT_READY: return CR_NOT_READY;
    default: return CR_DEVICE_ERROR;
    }
}

void KshimPcieCrLog(const char *format, ...)
{
    char fmt[256];
    size_t o = 0;
    for (const char *p = format; *p && o + 3 < sizeof(fmt); ++p) {
        if (p[0] == '%' && p[1] == '%') { fmt[o++] = *p++; fmt[o++] = *p; continue; }
        fmt[o++] = *p;
        if (p[0] == '%' && p[1] == 'a') { fmt[o++] = 's'; ++p; }
    }
    if (o && fmt[o - 1] != '\n') fmt[o++] = '\n';
    fmt[o] = 0;
    va_list args;
    va_start(args, format);
    vprintf(fmt, args);
    va_end(args);
}

/* ---- target selection -------------------------------------------------- */

static const PcieTargetRegion *find_region(const PcieTargetContext *t,
                                           const PcieTargetController *c, const char *name)
{
    for (unsigned i = 0; i < c->RegionCount; ++i)
        if (!strcmp(t->Regions[c->RegionOffset + i].Name, name))
            return &t->Regions[c->RegionOffset + i];
    return NULL;
}

int kshim_pci_qcom_lib_select(struct qcom_pcie *qp)
{
    const struct target_choice *choice = NULL;
    for (unsigned i = 0; i < sizeof(Targets) / sizeof(Targets[0]) && !choice; ++i)
        if (!dt_node_check_compatible(qp->fdt, qp->node, Targets[i].compatible))
            choice = &Targets[i];
    if (!choice && !dt_node_check_compatible(qp->fdt, qp->node, "qcom,pci-msm") &&
        (!dt_node_check_compatible(qp->fdt, 0, "qcom,kona") ||
         !dt_node_check_compatible(qp->fdt, 0, "qcom,sm8250")))
        choice = &Targets[3];
    if (!choice) return ERROR_NOT_SUPPORTED;
    const PcieTargetContext *t = choice->get();
    if (!t) return ERROR_NOT_SUPPORTED;
    for (unsigned i = 0; i < t->ControllerCount; ++i) {
        const PcieTargetController *c = &t->Controllers[i];
        const PcieTargetRegion *parf = find_region(t, c, "parf"), *dbi = find_region(t, c, "dbi"),
                               *atu = find_region(t, c, "atu"), *cfg = find_region(t, c, "config");
        if (!parf || parf->Base != qp->parf.base) continue;
        if (!dbi || !atu || !cfg || dbi->Base != qp->dbi.base || atu->Base != qp->atu.base ||
            cfg->Base != qp->config.base) {
            LOG("%s: DT registers differ from %s target data, no cold start\n", qp->host.name,
                choice->name);
            return ERROR_NOT_SUPPORTED;
        }
        const PcieTargetPhy *phy = &t->Phys[c->PhyIndex];
        if (qp->has_phy && phy->Base != qp->phy.base) {
            LOG("%s: DT PHY differs from %s target data, no cold start\n", qp->host.name,
                choice->name);
            return ERROR_NOT_SUPPORTED;
        }
        if (Lib.target && Lib.target != t) return ERROR_NOT_SUPPORTED; /* one SoC per boot */
        qp->target = t;
        qp->port = (int)i;
        return 0;
    }
    return ERROR_NOT_SUPPORTED;
}

/* ---- resource lookup --------------------------------------------------- */

static int port_of_clock(const PcieTargetContext *t, unsigned index, bool *phy_slice)
{
    for (unsigned p = 0; p < t->ControllerCount; ++p) {
        const PcieTargetController *c = &t->Controllers[p];
        const PcieTargetPhy *phy = &t->Phys[c->PhyIndex];
        if (index >= c->ClockOffset && index < (unsigned)c->ClockOffset + c->ClockCount) {
            *phy_slice = false;
            return (int)p;
        }
        if (index >= phy->ClockOffset && index < (unsigned)phy->ClockOffset + phy->ClockCount) {
            *phy_slice = true;
            return (int)p;
        }
    }
    return -1;
}

static int port_of_reset(const PcieTargetContext *t, unsigned index, bool *phy_slice)
{
    for (unsigned p = 0; p < t->ControllerCount; ++p) {
        const PcieTargetController *c = &t->Controllers[p];
        const PcieTargetPhy *phy = &t->Phys[c->PhyIndex];
        *phy_slice = false;
        if (index >= c->ResetOffset && index < (unsigned)c->ResetOffset + c->ResetCount)
            return (int)p;
        *phy_slice = true;
        if ((index >= phy->ResetOffset && index < (unsigned)phy->ResetOffset + phy->ResetCount) ||
            (index >= phy->PipeResetOffset &&
             index < (unsigned)phy->PipeResetOffset + phy->PipeResetCount))
            return (int)p;
    }
    return -1;
}

static int consumer(const struct qcom_pcie *qp, bool phy_slice)
{
    return phy_slice && qp->phy_node >= 0 ? qp->phy_node : qp->node;
}

static int clock_handle(const PcieTargetClock *clock, struct kshim_clk **out, bool **enabled)
{
    for (unsigned i = 0; i < Lib.clock_count; ++i)
        if (Lib.clocks[i].key == clock) {
            *out = Lib.clocks[i].clk;
            *enabled = &Lib.clocks[i].enabled;
            return 0;
        }
    bool phy_slice;
    int port = port_of_clock(Lib.target, (unsigned)(clock - Lib.target->Clocks), &phy_slice);
    struct qcom_pcie *qp = port >= 0 ? Lib.ports[port] : NULL;
    struct kshim_clk *clk = NULL;
    int status = qp ? kshim_clk_get(qp->fdt, consumer(qp, phy_slice), clock->Name, &clk)
                    : ERROR_NOT_FOUND;
    if (status) status = kshim_clk_get_by_name(clock->Controller, clock->Id, &clk);
    if (status) return status;
    if (Lib.clock_count == CLOCK_CACHE) return KSHIM_IO_ERR_NO_MEMORY;
    Lib.clocks[Lib.clock_count] = (struct clock_slot){clock, clk, false};
    *out = clk;
    *enabled = &Lib.clocks[Lib.clock_count++].enabled;
    return 0;
}

/* ---- PcieIoOps --------------------------------------------------------- */

static UINT32 io_read32(VOID *c, UINT64 a) { (void)c; return pci_mmio_read((uintptr_t)a, 4); }
static UINT16 io_read16(VOID *c, UINT64 a) { (void)c; return (UINT16)pci_mmio_read((uintptr_t)a, 2); }
static UINT8 io_read8(VOID *c, UINT64 a) { (void)c; return (UINT8)pci_mmio_read((uintptr_t)a, 1); }
static VOID io_write32(VOID *c, UINT64 a, UINT32 v) { (void)c; pci_mmio_write((uintptr_t)a, 4, v); }
static VOID io_write16(VOID *c, UINT64 a, UINT16 v) { (void)c; pci_mmio_write((uintptr_t)a, 2, v); }
static VOID io_write8(VOID *c, UINT64 a, UINT8 v) { (void)c; pci_mmio_write((uintptr_t)a, 1, v); }
static VOID io_delay(VOID *c, UINT32 us) { (void)c; kshim_pci_delay_us(us); }
static UINT64 io_time(VOID *c) { (void)c; return kshim_pci_time_us(); }

static CR_STATUS set_clock(VOID *context, CONST PcieTargetClock *clock, BOOLEAN enable)
{
    (void)context;
    if (clock->Provider == PCIE_CLOCK_PROVIDER_PHY_OUTPUT_ALIAS ||
        clock->Provider == PCIE_CLOCK_PROVIDER_RPMH_ALWAYS_ON)
        return CR_SUCCESS; /* produced by the PHY / always-on CXO: nothing to vote */
    struct kshim_clk *clk;
    bool *enabled;
    int status = clock_handle(clock, &clk, &enabled);
    if (status) {
        LOG("clock %s (%s) unavailable: %s\n", clock->Name, clock->Id, kshim_io_strerror(status));
        return cr(status);
    }
    if (enable) {
        if (*enabled) return CR_SUCCESS;
        if (clock->RateHz && !kshim_clk_is_on(clk)) {
            int rate = kshim_clk_set_rate(clk, clock->RateHz);
            if (rate == ERROR_NOT_READY) {
                LOG("clock %s (%s): %u Hz needs its parent PLL running; cold start refused\n",
                    clock->Name, clock->Id, (unsigned)clock->RateHz);
                return cr(rate);
            }
            if (rate == ERROR_NOT_SUPPORTED && clock->RateHz != 19200000U)
                LOG("clock %s (%s): no rate facts for %u Hz, left as found\n", clock->Name,
                    clock->Id, (unsigned)clock->RateHz);
            else if (rate && rate != ERROR_NOT_SUPPORTED && rate != ERROR_BUSY)
                return cr(rate);
        }
        status = kshim_clk_enable(clk);
        if (!status) *enabled = true;
        return cr(status);
    }
    if (!*enabled) return CR_SUCCESS;
    status = kshim_clk_disable(clk);
    if (!status) *enabled = false;
    return cr(status);
}

static CR_STATUS set_reset(VOID *context, CONST PcieTargetReset *reset, BOOLEAN assert)
{
    (void)context;
    bool phy_slice;
    int port = port_of_reset(Lib.target, (unsigned)(reset - Lib.target->Resets), &phy_slice);
    struct qcom_pcie *qp = port >= 0 ? Lib.ports[port] : NULL;
    struct kshim_clk *r = NULL;
    int status = qp ? kshim_clk_get_reset(qp->fdt, consumer(qp, phy_slice), reset->Name, &r)
                    : ERROR_NOT_FOUND;
    if (status) status = kshim_clk_get_by_name(reset->Controller, reset->Id, &r);
    if (!status) status = kshim_clk_reset(r, assert);
    if (status) LOG("reset %s (%s) %s failed: %s\n", reset->Name, reset->Id,
                    assert ? "assert" : "deassert", kshim_io_strerror(status));
    return cr(status);
}

static int gdsc(struct qcom_pcie *qp, int node, const char *controller, const char *id,
                struct kshim_clk **out)
{
    int status = node >= 0 ? kshim_clk_get_power_domain(qp->fdt, node, 0, out) : ERROR_NOT_FOUND;
    if (status && id) status = kshim_clk_get_by_name(controller, id, out);
    return status;
}

static CR_STATUS set_power_domain(VOID *context, CONST PcieTargetController *c, BOOLEAN enable)
{
    (void)context;
    unsigned port = (unsigned)(c - Lib.target->Controllers);
    struct qcom_pcie *qp = Lib.ports[port];
    const PcieTargetPhy *phy = &Lib.target->Phys[c->PhyIndex];
    struct kshim_clk *ctrl = NULL, *phy_gdsc = NULL;
    int status = 0;
    if (c->PowerDomainId) status = gdsc(qp, qp->node, c->PowerDomainController, c->PowerDomainId, &ctrl);
    if (!status && phy->PowerDomainId)
        status = gdsc(qp, qp->phy_node, phy->PowerDomainController, phy->PowerDomainId, &phy_gdsc);
    if (status) {
        LOG("%s: power domain unavailable: %s\n", qp->host.name, kshim_io_strerror(status));
        return cr(status);
    }
    if (enable) {
        if (ctrl) status = kshim_clk_enable(ctrl);
        if (!status && phy_gdsc) {
            status = kshim_clk_enable(phy_gdsc);
            if (status && ctrl) (void)kshim_clk_disable(ctrl);
        }
    } else {
        if (phy_gdsc) status = kshim_clk_disable(phy_gdsc);
        if (ctrl) {
            int s = kshim_clk_disable(ctrl);
            if (!status) status = s;
        }
    }
    return cr(status);
}

static CR_STATUS set_supply(VOID *context, CONST PcieTargetSupply *supply, BOOLEAN enable)
{
    (void)context;
    struct qcom_pcie *qp = NULL;
    unsigned index = (unsigned)(supply - Lib.target->Supplies);
    for (unsigned p = 0; p < Lib.target->ControllerCount && !qp; ++p) {
        const PcieTargetController *c = &Lib.target->Controllers[p];
        if (index >= c->SupplyOffset && index < (unsigned)c->SupplyOffset + c->SupplyCount)
            qp = Lib.ports[p];
    }
    if (!qp) return CR_INVALID_PARAMETER;
    struct kshim_regulator *r;
    int status = qp->phy_node >= 0 ? kshim_regulator_get(qp->fdt, qp->phy_node, supply->Name, &r)
                                   : ERROR_NOT_FOUND;
    if (status == ERROR_NOT_FOUND) status = kshim_regulator_get(qp->fdt, qp->node, supply->Name, &r);
    if (status == ERROR_NOT_FOUND) {
        if (enable) LOG("%s: %s-supply not in DT, left as found\n", qp->host.name, supply->Name);
        return CR_SUCCESS;
    }
    if (status) return cr(status);
    if (!enable) return cr(kshim_regulator_disable(r));
    if (supply->VoltageMv) {
        int v = kshim_regulator_set_voltage(r, supply->VoltageMv * 1000U, supply->VoltageMv * 1000U);
        if (v && v != ERROR_NOT_SUPPORTED) return cr(v);
    }
    return cr(kshim_regulator_enable(r));
}

static CR_STATUS set_interconnect(VOID *context, CONST PcieTargetController *c, BOOLEAN enable)
{
    (void)context;
    (void)c;
    if (enable && !Lib.icc_logged) {
        Lib.icc_logged = true;
        LOG("interconnect paths not voted (assumed inherited)\n");
    }
    return CR_SUCCESS;
}

static CR_STATUS set_iommu(VOID *context, CONST PcieTargetController *c, BOOLEAN enable)
{
    (void)context;
    (void)c;
    if (enable && !Lib.smmu_logged) {
        Lib.smmu_logged = true;
        LOG("SMMU streams not programmed (bypass assumed inherited)\n");
    }
    return CR_SUCCESS;
}

static CR_STATUS set_mux(const PcieTargetPhy *phy, const char *what, const char *controller,
                         const char *id, UINT32 source)
{
    struct kshim_clk *mux = NULL;
    int status = controller && id ? kshim_clk_get_by_name(controller, id, &mux) : ERROR_NOT_SUPPORTED;
    if (!status) status = kshim_clk_set_parent(mux, source);
    if (status)
        LOG("%s: %s mux %s source %u: %s\n", phy->Label, what, id ? id : "?", (unsigned)source,
            kshim_io_strerror(status));
    return cr(status);
}

/* PcieIoOps.SetClockParent: the PHY AUX mux (reference during the cold PHY
 * init, PHY output once ready, reference again before power-down). */
static CR_STATUS set_aux_parent(VOID *context, CONST PcieTargetPhy *phy, UINT32 source)
{
    (void)context;
    return set_mux(phy, "AUX", phy->AuxMuxController, phy->AuxMuxId, source);
}

/* PcieIoOps.SetPipeClockParent: the PIPE mux (PHY output with the GDSCs
 * up, reference after the PIPE gates stop). */
static CR_STATUS set_pipe_parent(VOID *context, CONST PcieTargetPhy *phy, UINT32 source)
{
    (void)context;
    return set_mux(phy, "PIPE", phy->PipeMuxController, phy->PipeMuxId, source);
}

static struct qcom_pcie *port_of_gpio(const PcieTargetGpio *gpio)
{
    unsigned index = (unsigned)(gpio - Lib.target->Gpios);
    for (unsigned p = 0; p < Lib.target->ControllerCount; ++p) {
        const PcieTargetController *c = &Lib.target->Controllers[p];
        if (index >= c->GpioOffset && index < (unsigned)c->GpioOffset + c->GpioCount)
            return Lib.ports[p];
    }
    return NULL;
}

static CR_STATUS set_gpio(VOID *context, CONST PcieTargetGpio *gpio, PCIE_GPIO_DIRECTION dir,
                          BOOLEAN active)
{
    (void)context;
    struct qcom_pcie *qp = port_of_gpio(gpio);
    if (!qp) return CR_INVALID_PARAMETER;
    if (!strcmp(gpio->Role, "perst")) return cr(kshim_pci_qcom_perst(qp, active));
    if (dir == PCIE_GPIO_OUTPUT) {
        /* Board enable lines of the target ("enable"): its pin, its polarity. */
        struct kshim_gpio g;
        if (qp->tlmm_node < 0) return CR_UNSUPPORTED;
        int status = kshim_gpio_from_tlmm_pin(qp->fdt, qp->tlmm_node, gpio->Pin,
                                              !strcmp(gpio->Polarity, "GPIO_ACTIVE_LOW"), &g);
        if (!status) status = kshim_gpio_request(&g, "pcie");
        if (!status) status = kshim_gpio_direction_output(&g, active);
        return cr(status);
    }
    /* WAKE#/CLKREQ#: muxed and biased by the controller's pinctrl state. */
    if (!qp->pinctrl_applied)
        LOG("%s: %s pin %u left as found (no pinctrl state)\n", qp->host.name, gpio->Role,
            gpio->Pin);
    return CR_SUCCESS;
}

static CR_STATUS set_endpoint_power(CONST struct _PcieIoOps *io, CONST PcieTargetController *c,
                                    BOOLEAN enable)
{
    (void)io;
    (void)enable;
    /* Endpoint rails come from the DT (kshim_pci_init powers every endpoint
     * node under the root ports before the link starts). */
    return c->Ntn3 ? CR_UNSUPPORTED : CR_SUCCESS;
}

static CR_STATUS set_endpoint_stage(CONST struct _PcieIoOps *io, CONST PcieTargetController *c,
                                    PCIE_ENDPOINT_STAGE stage)
{
    (void)io;
    (void)stage;
    return c->Ntn3 ? CR_UNSUPPORTED : CR_SUCCESS;
}

static const PcieIoOps Ops = {
    .Read32 = io_read32,
    .Write32 = io_write32,
    .DelayUs = io_delay,
    .SetClock = set_clock,
    .SetReset = set_reset,
    .SetPowerDomain = set_power_domain,
    .SetSupply = set_supply,
    .SetInterconnect = set_interconnect,
    .SetIommu = set_iommu,
    .SetGpio = set_gpio,
    .Context = NULL,
    .Read8 = io_read8,
    .Read16 = io_read16,
    .Write8 = io_write8,
    .Write16 = io_write16,
    .SetClockParent = set_aux_parent,
    .DiagnoseLink = NULL,
    .SetEndpointPower = set_endpoint_power,
    .GetTimeUs = io_time,
    .SetPipeClockParent = set_pipe_parent,
    .SetEndpointStage = set_endpoint_stage,
};

/* ---- PERST# ------------------------------------------------------------ */

static int find_tlmm(struct qcom_pcie *qp)
{
    static const char *const tlmm[] = {"qcom,sm8650-tlmm", "qcom,sm8550-tlmm", "qcom,sm8450-tlmm",
                                       "qcom,sm8250-tlmm", "qcom,kona-pinctrl",
                                       "qcom,sm8150-pinctrl", NULL};
    for (unsigned i = 0; tlmm[i]; ++i) {
        int n = dt_node_by_compatible(qp->fdt, -1, tlmm[i]);
        if (n >= 0) return n;
    }
    return -1;
}

int kshim_pci_qcom_perst(struct qcom_pcie *qp, bool asserted)
{
    if (!qp->has_perst && qp->target && qp->port >= 0) {
        const PcieTargetController *c = &qp->target->Controllers[qp->port];
        for (unsigned i = 0; i < c->GpioCount && !qp->has_perst; ++i) {
            const PcieTargetGpio *g = &qp->target->Gpios[c->GpioOffset + i];
            if (strcmp(g->Role, "perst")) continue;
            if (qp->tlmm_node < 0) qp->tlmm_node = find_tlmm(qp);
            if (qp->tlmm_node >= 0 &&
                !kshim_gpio_from_tlmm_pin(qp->fdt, qp->tlmm_node, g->Pin,
                                          !strcmp(g->Polarity, "GPIO_ACTIVE_LOW"), &qp->perst))
                qp->has_perst = true;
        }
    }
    if (!qp->has_perst) return ERROR_NOT_FOUND;
    if (!qp->perst_requested) {
        int status = kshim_gpio_request(&qp->perst, "pcie-perst");
        if (status) return status;
        qp->perst_requested = true;
    }
    return kshim_gpio_direction_output(&qp->perst, asserted);
}

void kshim_pci_qcom_perst_release(struct qcom_pcie *qp)
{
    if (!qp->perst_requested) return;
    (void)kshim_gpio_free(&qp->perst);
    qp->perst_requested = false;
}

/* ---- lifecycle --------------------------------------------------------- */

int kshim_pci_qcom_lib_start(struct qcom_pcie *qp)
{
    if (!qp->target || qp->port < 0) return ERROR_NOT_SUPPORTED;
    if (Lib.target && Lib.target != qp->target) return ERROR_NOT_SUPPORTED;
    Lib.ports[qp->port] = qp;
    int pinctrl = kshim_pinctrl_apply(qp->fdt, qp->node, "default");
    qp->pinctrl_applied = !pinctrl;
    if (pinctrl && pinctrl != ERROR_NOT_FOUND)
        LOG("%s: pinctrl default: %s\n", qp->host.name, kshim_io_strerror(pinctrl));
    if (!Lib.lib) {
        CR_STATUS s = PcieLibInit(&Lib.lib, qp->target, &Ops);
        if (CR_ERROR(s)) {
            LOG("%s: PcieLibInit failed (%d)\n", qp->host.name, s);
            Lib.lib = NULL;
            return kshim_pci_qcom_status(s);
        }
        Lib.target = qp->target;
    }
    LOG("%s: cold start of %s\n", qp->host.name, qp->target->Controllers[qp->port].Label);
    CR_STATUS s = PcieInitializePort(Lib.lib, (UINT16)qp->port);
    if (CR_ERROR(s)) return kshim_pci_qcom_status(s);
    return PcieIsLinkUp(Lib.lib, (UINT16)qp->port) ? 0 : ERROR_NOT_READY;
}

int kshim_pci_qcom_lib_config(struct qcom_pcie *qp, uint8_t bus, uint8_t devfn, uint16_t reg,
                              unsigned width, bool write, uint32_t *value)
{
    UINT32 v = write ? *value : 0;
    CR_STATUS s = PcieConfigAccess(Lib.lib, (UINT16)qp->port, bus, (UINT8)KSHIM_PCI_SLOT(devfn),
                                   (UINT8)KSHIM_PCI_FUNC(devfn), reg, width, write, &v);
    if (s == CR_NOT_FOUND) {
        if (!write) *value = width == 4 ? 0xffffffffU : (1U << (8 * width)) - 1;
        return 0;
    }
    if (CR_ERROR(s)) return kshim_pci_qcom_status(s);
    if (!write) *value = v;
    return 0;
}

int kshim_pci_qcom_lib_stop(struct qcom_pcie *qp)
{
    if (!Lib.lib || qp->port < 0) return 0;
    CR_STATUS s = PcieShutdownPort(Lib.lib, (UINT16)qp->port);
    if (CR_ERROR(s)) {
        LOG("%s: PcieShutdownPort failed (%d)\n", qp->host.name, s);
        return kshim_pci_qcom_status(s);
    }
    return 0;
}
