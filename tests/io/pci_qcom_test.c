/* SPDX-License-Identifier: MIT */
/*
 * Qualcomm PCIe host over a DWC/PARF/iATU/QMP register model and fakes of
 * the clock, regulator, GPIO and pinctrl services, with
 * tests/fixtures/io/pci-sm8650.dts (Pocket S2 pcie1 -> uPD720201). One
 * scenario per executable (the PCI core keeps static state):
 *
 *  SCENARIO 1  inherited link, bootloader iATU config + memory regions:
 *              only the config region is taken; clocks, rails, PERST, PARF
 *              untouched; endpoint BAR kept; handoff restores the iATU, the
 *              config space and every register byte as found.
 *  SCENARIO 2  inherited link without a memory iATU region: a free region
 *              is programmed for the window and restored at handoff; the
 *              untranslated I/O window is not used.
 *  SCENARIO 3  cold start through CrDK PcieLib with the kShimC SM8650
 *              target: PERST# held while the endpoint rails come up, GDSC
 *              before clocks, the 119 QMP rows programmed before the PIPE
 *              clock, link up, enumeration, and the handoff releasing every
 *              resource with PERST# asserted before endpoint power drops.
 */
#define _GNU_SOURCE
#include "../../kernel/drivers/pci/qcom.h"
#include "pci_model.h"

#include <kshim/clk.h>
#include <kshim/irq.h>
#include <kshim/pinctrl.h>
#include <kshim/regulator.h>
#include <kshim/smp.h>
#include <kshim/time.h>
#include <qcom_dt.h>
#include <stdarg.h>
#include <stdlib.h>

#ifndef SCENARIO
#error "SCENARIO 1..3"
#endif
#define COLD (SCENARIO == 3)

#define PARF   UINT64_C(0x01c08000)
#define DBI    UINT64_C(0x40000000)
#define ELBI   UINT64_C(0x40000f20)
#define ATU    UINT64_C(0x40001000)
#define CONFIG UINT64_C(0x40100000)
#define PHYB   UINT64_C(0x01c0e000)

/* ---- event log --------------------------------------------------------- */
static char Events[512][48];
static unsigned EventCount;
static void event(const char *fmt, ...)
{
    assert(EventCount < 512);
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(Events[EventCount++], sizeof(Events[0]), fmt, ap);
    va_end(ap);
}
static int first(const char *e)
{
    for (unsigned i = 0; i < EventCount; ++i) if (!strcmp(Events[i], e)) return (int)i;
    return -1;
}
static int last(const char *e)
{
    for (unsigned i = EventCount; i-- > 0;) if (!strcmp(Events[i], e)) return (int)i;
    return -1;
}
static unsigned count_prefix(const char *p)
{
    unsigned n = 0;
    for (unsigned i = 0; i < EventCount; ++i) n += !strncmp(Events[i], p, strlen(p));
    return n;
}

/* ---- environment stubs ------------------------------------------------- */
static uint64_t now_us;
uint64_t kshim_smp_time_us(void) { return now_us; }
uint64_t delay(uint64_t us) { now_us += us; return us; }
bool kshim_irq_ready(void) { return false; }
uint64_t get_current_time(void) { return now_us / 1000; }
void kshim_wait_event(uint64_t deadline_ms) { now_us = deadline_ms * 1000 + 1; }
int kshim_irq_request(uint32_t intid, unsigned t, kshim_irq_handler_t h, void *c)
{
    (void)t; (void)h; (void)c;
    event("irq+ %u", intid);
    return 0;
}
int kshim_irq_free(uint32_t intid) { event("irq- %u", intid); return 0; }
unsigned long kshim_irq_save(void) { return 0; }
void kshim_irq_restore(unsigned long f) { (void)f; }
int kshim_resource_map(struct kshim_resources *r, uint64_t base, uint64_t size,
                       kshim_mmu_memory_type_t type, uint32_t perm)
{
    (void)r; (void)type; (void)perm;
    event("map %llx+%llx", (unsigned long long)base, (unsigned long long)size);
    return 0;
}

/* ---- clock / reset / GDSC fake ----------------------------------------- */
struct kshim_clk { int node; char name[24]; bool on, inherited_on; int refs; };
static struct kshim_clk Clks[48];
static unsigned ClkCount;
static bool ClocksOnAtEntry = !COLD;

static struct kshim_clk *clk_entry(int node, const char *name)
{
    for (unsigned i = 0; i < ClkCount; ++i)
        if (Clks[i].node == node && !strcmp(Clks[i].name, name)) return &Clks[i];
    assert(ClkCount < 48);
    struct kshim_clk *c = &Clks[ClkCount++];
    c->node = node;
    snprintf(c->name, sizeof(c->name), "%s", name);
    c->on = c->inherited_on = ClocksOnAtEntry;
    return c;
}
static const void *Fdt;
static const char *short_node(int node)
{
    const char *n = fdt_get_name(Fdt, node, NULL);
    return !strncmp(n, "phy", 3) ? "phy" : !strncmp(n, "pcie@1c", 7) ? "ctl" : n;
}
int kshim_clk_get(const void *f, int consumer, const char *name, struct kshim_clk **clk)
{
    if (!name || fdt_stringlist_search(f, consumer, "clock-names", name) < 0) return ERROR_NOT_FOUND;
    *clk = clk_entry(consumer, name);
    return 0;
}
int kshim_clk_get_power_domain(const void *f, int consumer, unsigned index, struct kshim_clk **g)
{
    if (index || !fdt_getprop(f, consumer, "power-domains", NULL)) return ERROR_NOT_FOUND;
    *g = clk_entry(consumer, "gdsc");
    return 0;
}
int kshim_clk_get_reset(const void *f, int consumer, const char *name, struct kshim_clk **r)
{
    if (fdt_stringlist_search(f, consumer, "reset-names", name) < 0) return ERROR_NOT_FOUND;
    char key[24];
    snprintf(key, sizeof(key), "rst:%s", name);
    *r = clk_entry(consumer, key);
    return 0;
}
/* GCC muxes by name: the PIPE and PHY AUX muxes (source field, 2 = XO
 * after a clean shutdown, as Linux clk-regmap-phy-mux parks it). */
static struct kshim_clk PipeMux = {-1, "pipe_mux", false, false, 0};
static struct kshim_clk AuxMux = {-1, "aux_mux", false, false, 0};
static uint32_t PipeMuxSrc = 2, AuxMuxSrc = 2;
int kshim_clk_get_by_name(const char *controller, const char *name, struct kshim_clk **clk)
{
    if (!strcmp(controller, "gcc") && !strcmp(name, "gcc_pcie_1_pipe_clk_src")) { *clk = &PipeMux; return 0; }
    if (!strcmp(controller, "gcc") && !strcmp(name, "gcc_pcie_1_phy_aux_clk_src")) { *clk = &AuxMux; return 0; }
    return ERROR_NOT_SUPPORTED;
}
int kshim_clk_set_parent(struct kshim_clk *mux, uint32_t source)
{
    assert(mux == &PipeMux || mux == &AuxMux);
    uint32_t *src = mux == &PipeMux ? &PipeMuxSrc : &AuxMuxSrc;
    event("%s=%u", mux->name, source);
    *src = source;
    return 0;
}
bool kshim_clk_is_on(const struct kshim_clk *clk) { return clk->on; }
int kshim_clk_enable(struct kshim_clk *clk)
{
    if (!clk->refs++) event("on %s.%s", short_node(clk->node), clk->name);
    clk->on = true;
    return 0;
}
int kshim_clk_disable(struct kshim_clk *clk)
{
    assert(clk->refs > 0);
    if (!--clk->refs) {
        event("off %s.%s", short_node(clk->node), clk->name);
        clk->on = clk->inherited_on;
    }
    return 0;
}
int kshim_clk_reset(struct kshim_clk *r, bool asserted)
{
    event("%s %s.%s", asserted ? "assert" : "deassert", short_node(r->node), r->name + 4);
    return 0;
}
int kshim_clk_set_rate(struct kshim_clk *clk, uint32_t hz)
{
    if (hz == 19200000U) event("cxo %s.%s", short_node(clk->node), clk->name);
    else event("rate %s.%s %u", short_node(clk->node), clk->name, (unsigned)hz);
    return 0;
}

/* ---- regulator fake ---------------------------------------------------- */
struct kshim_regulator { int provider; int refs; char name[24]; };
static struct kshim_regulator Regs[8];
static unsigned RegCount;
int kshim_regulator_get(const void *f, int consumer, const char *supply,
                        struct kshim_regulator **r)
{
    char prop[48];
    snprintf(prop, sizeof(prop), "%s-supply", supply);
    int provider = dt_reference(f, consumer, prop, 0);
    if (provider < 0) return ERROR_NOT_FOUND;
    for (unsigned i = 0; i < RegCount; ++i)
        if (Regs[i].provider == provider) { *r = &Regs[i]; return 0; }
    Regs[RegCount] = (struct kshim_regulator){provider, 0, ""};
    snprintf(Regs[RegCount].name, sizeof(Regs[0].name), "%s", supply);
    *r = &Regs[RegCount++];
    return 0;
}
int kshim_regulator_enable(struct kshim_regulator *r)
{
    if (!r->refs++) event("rail+ %s", r->name);
    return 0;
}
int kshim_regulator_disable(struct kshim_regulator *r)
{
    assert(r->refs > 0);
    if (!--r->refs) event("rail- %s", r->name);
    return 0;
}
int kshim_regulator_set_voltage(struct kshim_regulator *r, uint32_t a, uint32_t b)
{
    (void)r; (void)a; (void)b;
    return 0;
}
int kshim_regulator_bulk_enable(const void *f, int node)
{
    event("bulk+ %s", fdt_get_name(f, node, NULL));
    return 0;
}
int kshim_regulator_bulk_disable(const void *f, int node)
{
    event("bulk- %s", fdt_get_name(f, node, NULL));
    return 0;
}

/* ---- GPIO / pinctrl fake ----------------------------------------------- */
static int PerstLevel = -1; /* logical: 1 asserted */
int kshim_gpio_from_dt(const void *f, int node, const char *prop, unsigned index,
                       struct kshim_gpio *g)
{
    int len;
    const fdt32_t *p = fdt_getprop(f, node, prop, &len);
    if (!p || index || len != 12) return ERROR_NOT_FOUND;
    memset(g, 0, sizeof(*g));
    g->chip = KSHIM_GPIO_CHIP_TLMM;
    g->provider = fdt_node_offset_by_phandle(f, fdt32_to_cpu(p[0]));
    g->pin = fdt32_to_cpu(p[1]);
    g->dt_flags = fdt32_to_cpu(p[2]);
    g->active_low = g->dt_flags & 1;
    g->slot = -1;
    return 0;
}
int kshim_gpio_from_tlmm_pin(const void *f, int tlmm, uint32_t pin, bool low, struct kshim_gpio *g)
{
    (void)f;
    memset(g, 0, sizeof(*g));
    g->chip = KSHIM_GPIO_CHIP_TLMM;
    g->provider = tlmm;
    g->pin = pin;
    g->active_low = low;
    g->slot = -1;
    return 0;
}
int kshim_gpio_request(struct kshim_gpio *g, const char *owner)
{
    event("gpio+ %u %s", g->pin, owner);
    g->slot = 1;
    return 0;
}
int kshim_gpio_direction_output(struct kshim_gpio *g, bool active)
{
    assert(g->slot == 1);
    if (g->pin == 97) {
        assert(g->active_low);
        PerstLevel = active;
        event("perst=%d", active);
    }
    return 0;
}
int kshim_gpio_free(struct kshim_gpio *g)
{
    event("gpio- %u", g->pin);
    g->slot = -1;
    return 0;
}
int kshim_pinctrl_apply(const void *f, int node, const char *name)
{
    assert(!strcmp(name, "default"));
    if (!fdt_getprop(f, node, "pinctrl-0", NULL)) return ERROR_NOT_FOUND;
    event("pinctrl %s", fdt_get_name(f, node, NULL));
    return 0;
}

/* ---- register model ---------------------------------------------------- */
static uint8_t Parf[0x3000], Atu[0x1000], Phy[0x2000], Elbi[0xa8];
static int Root, Ep;

static uint32_t rd_arr(const uint8_t *a, uint64_t off, unsigned w) { return get(a, (uint16_t)off, w); }
static void wr_arr(uint8_t *a, uint64_t off, unsigned w, uint32_t v) { put(a, (uint16_t)off, w, v); }

/* The enabled CFG region covering a config-window address: BDF and type. */
static bool cfg_decode(uint64_t a, unsigned *bus, uint8_t *devfn, uint32_t *type)
{
    for (unsigned r = 0; r < 8; ++r) {
        uint32_t c2 = get(Atu, (uint16_t)(r * 0x200 + 4), 4);
        uint32_t c1 = get(Atu, (uint16_t)(r * 0x200), 4) & 0x1f;
        if (!(c2 & 0x80000000U) || (c1 != 4 && c1 != 5)) continue;
        uint64_t base = get(Atu, (uint16_t)(r * 0x200 + 8), 4) |
                        (uint64_t)get(Atu, (uint16_t)(r * 0x200 + 0xc), 4) << 32;
        uint64_t limit = get(Atu, (uint16_t)(r * 0x200 + 0x10), 4) | (base & ~0xffffffffULL);
        if (a < base || a > limit) continue;
        uint32_t target = get(Atu, (uint16_t)(r * 0x200 + 0x14), 4);
        *bus = target >> 24;
        *devfn = (uint8_t)(target >> 16);
        *type = c1;
        return true;
    }
    return false;
}

static void link_train(void)
{
    /* LTSSM enable trains the link: Data Link Layer Link Active, L0. */
    put(Model[Root].cfg, 0x52, 2, (uint16_t)(get(Model[Root].cfg, 0x52, 2) | 0x2000));
    put(Model[Root].cfg, 0x728, 4, 0x11);
}

static uint32_t mmio_read(uintptr_t a, unsigned w)
{
    if (a >= PARF && a < PARF + sizeof(Parf)) return rd_arr(Parf, a - PARF, w);
    if (a >= ELBI && a < ELBI + sizeof(Elbi)) return rd_arr(Elbi, a - ELBI, w);
    if (a >= DBI && a < DBI + 0xf1d) return get(Model[Root].cfg, (uint16_t)(a - DBI), w);
    if (a >= ATU && a < ATU + sizeof(Atu)) return rd_arr(Atu, a - ATU, w);
    if (a >= PHYB && a < PHYB + sizeof(Phy)) return rd_arr(Phy, a - PHYB, w);
    if (a >= CONFIG && a < CONFIG + 0x100000) {
        unsigned bus; uint8_t devfn; uint32_t type;
        if (!cfg_decode(a, &bus, &devfn, &type)) return 0xffffffffU;
        unsigned secondary = get(Model[Root].cfg, 0x19, 1);
        if ((type == 4) != (bus == secondary)) return 0xffffffffU;
        return model_cfg_read(bus, devfn, (uint16_t)((a - CONFIG) & 0xfff), w);
    }
    printf("unexpected read 0x%lx\n", (unsigned long)a);
    assert(!"unexpected MMIO read");
    return 0;
}

static void mmio_write(uintptr_t a, unsigned w, uint32_t v)
{
    if (a >= PARF && a < PARF + sizeof(Parf)) {
        wr_arr(Parf, a - PARF, w, v);
        if (a - PARF == 0x1b0 && (v & 0x100)) link_train();
        return;
    }
    if (a >= ELBI && a < ELBI + sizeof(Elbi)) { wr_arr(Elbi, a - ELBI, w, v); return; }
    if (a >= DBI && a < DBI + 0xf1d) { put(Model[Root].cfg, (uint16_t)(a - DBI), w, v); return; }
    if (a >= ATU && a < ATU + sizeof(Atu)) { wr_arr(Atu, a - ATU, w, v); return; }
    if (a >= PHYB && a < PHYB + sizeof(Phy)) {
        wr_arr(Phy, a - PHYB, w, v);
        if (a - PHYB == 0x1244 && (v & 3) == 3) put(Phy, 0x1214, 4, 0); /* PHY ready */
        return;
    }
    if (a >= CONFIG && a < CONFIG + 0x100000) {
        unsigned bus; uint8_t devfn; uint32_t type;
        if (cfg_decode(a, &bus, &devfn, &type))
            model_cfg_write(bus, devfn, (uint16_t)((a - CONFIG) & 0xfff), w, v);
        return;
    }
    printf("unexpected write 0x%lx\n", (unsigned long)a);
    assert(!"unexpected MMIO write");
}
static const struct kshim_pci_test_mmio Mmio = {mmio_read, mmio_write};
const struct kshim_pci_test_mmio *kshim_pci_test_mmio = &Mmio;

static void *load(const char *path)
{
    FILE *f = fopen(path, "rb");
    assert(f);
    static char blob[65536] __attribute__((aligned(8)));
    size_t n = fread(blob, 1, sizeof(blob), f);
    fclose(f);
    assert(n > 0 && dt_validate(blob, n) == 0);
    return blob;
}

static void atu_region(unsigned r, uint32_t type, uint64_t base, uint64_t size, uint64_t target)
{
    put(Atu, (uint16_t)(r * 0x200 + 0x0), 4, type);
    put(Atu, (uint16_t)(r * 0x200 + 0x4), 4, 0x80000000U);
    put(Atu, (uint16_t)(r * 0x200 + 0x8), 4, (uint32_t)base);
    put(Atu, (uint16_t)(r * 0x200 + 0xc), 4, (uint32_t)(base >> 32));
    put(Atu, (uint16_t)(r * 0x200 + 0x10), 4, (uint32_t)(base + size - 1));
    put(Atu, (uint16_t)(r * 0x200 + 0x14), 4, (uint32_t)target);
}

static uint8_t Saved[MODEL_FUNCS][4096];
static uint8_t SavedAtu[0x1000], SavedParf[0x3000];

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    Fdt = load(FIXTURES "/pci-sm8650.dtb");

    /* DWC root port (DBI) and the uPD720201 behind it. */
    Root = model_add(-1, 0, 0, 0x17cb, 0x0111, 0x060400, 0x01, 1);
    model_pcie_port(Root, 4);
    Ep = model_add(Root, 0, 0, 0x1912, 0x0014, 0x0c0330, 0x00, 1);
    model_bar(Ep, 0, 0x2000, 4);
    if (!COLD) {
        /* What UEFI (XhciDxe on a live link) leaves behind. */
        model_set(Root, 0x18, 4, 0x00010100);
        model_set(Root, 0x20, 4, 0x40304030);
        model_set(Root, 0x04, 2, 0x0006);
        model_set(Ep, 0x10, 4, 0x40300004);
        model_set(Ep, 0x04, 2, 0x0006);
        link_train();
        put(Parf, 0x1b0, 4, 0x100);
        atu_region(0, 4, CONFIG, 0x100000, 0x01000000);
        if (SCENARIO == 1) atu_region(1, 0, 0x40300000, 0x1fd00000, 0x40300000);
    } else {
        put(Phy, 0x1214, 4, 0x80); /* PCS_STATUS1 PHYSTATUS until started */
    }
    model_snapshot(Saved);
    memcpy(SavedAtu, Atu, sizeof(Atu));
    memcpy(SavedParf, Parf, sizeof(Parf));

    /* The SM8650 target passes CrDK's own validation. */
    {
        const PcieTargetContext *t = kshim_pci_qcom_sm8650_target();
        assert(t->ControllerCount == 1 && t->PhyInitCount == 119);
        const PciePhyInitEntry *ln = &t->PhyInit[t->PhyInitCount - 1];
        assert(ln->Block == PCIE_PHY_BLOCK_TX && ln->LaneMask == 1 && ln->Offset >= 0xe00 &&
               ln->Offset < 0x1000);
    }

    struct kshim_resources res;
    memset(&res, 0, sizeof(res));
    res.fdt = Fdt;
    assert(!kshim_pci_map_resources(&res));
    assert(first("map 1c08000+3000") >= 0 && first("map 40000000+f1d") >= 0 &&
           first("map 40001000+1000") >= 0 && first("map 40100000+100000") >= 0 &&
           first("map 1c0e000+2000") >= 0 && first("map 40300000+1fd00000") >= 0 &&
           first("map 40200000+100000") >= 0 && first("map 40000f20+a8") >= 0);

    assert(!kshim_pci_init(Fdt));
    struct kshim_pci_dev *root = kshim_pci_find_device(0x17cb, 0x0111, NULL);
    struct kshim_pci_dev *ep = kshim_pci_find_device(0x1912, 0x0014, NULL);
    assert(root && ep);
    struct kshim_pci_host *host = root->host;
    assert(host->domain == 1 && host->started && host->inherited == !COLD);
    assert(root->bus == 0 && root->devfn == 0 && root->secondary_bus == 1);
    assert(ep->bus == 1 && ep->devfn == 0 && ep->bridge == root);
    assert(!strcmp(fdt_get_name(Fdt, ep->node, NULL), "usb-controller@0"));
    assert(!strcmp(fdt_get_name(Fdt, root->node, NULL), "pcie@0"));
    assert(ep->intx_intid == 32 + 434 && ep->intx_trigger == 4);
    assert(ep->bars[0].size == 0x2000 && ep->bars[0].cpu_addr);
    assert(ep->bars[0].bus_addr >= 0x40300000 && ep->bars[0].bus_addr + 0x2000 <= 0x60000000);
    /* Endpoint power: rails and pinctrl of usb-controller@0, before the link. */
    int bulk = first("bulk+ usb-controller@0"), pin = first("pinctrl usb-controller@0");
    assert(bulk >= 0 && pin > bulk);

    struct qcom_pcie *qp = (struct qcom_pcie *)host;
    if (!COLD) {
        assert(qp->live && !qp->cold);
        assert(ep->bars[0].inherited && ep->bars[0].bus_addr == 0x40300000);
        assert(!count_prefix("on ") && !count_prefix("assert") && !count_prefix("rail+") &&
               !count_prefix("gpio+") && first("pinctrl pcie@1c08000") < 0);
        assert(!memcmp(Parf, SavedParf, sizeof(Parf)));
        assert(host->window_count == 1 && host->windows[0].type == KSHIM_PCI_WINDOW_MEM32);
        assert(qp->owned_count == (SCENARIO == 1 ? 1U : 2U));
        assert(qp->owned[0].region == 0);
        if (SCENARIO == 2) {
            assert(qp->owned[1].region == 1);
            assert(get(Atu, 0x200, 4) == 0 && get(Atu, 0x204, 4) == 0x80000000U);
            assert(get(Atu, 0x208, 4) == 0x40300000 && get(Atu, 0x210, 4) == 0x5fffffff);
        }
        /* Region 0 now targets the endpoint (CFG0, bus 1). */
        assert(get(Atu, 0x14, 4) == 0x01000000 && (get(Atu, 0, 4) & 0x1f) == 4);
    } else {
        assert(!qp->live && qp->cold);
        /* Order: PERST# asserted, endpoint rails, controller pinctrl, PHY
         * rails, GDSCs, clocks, BCR pulse, PHY tables, PIPE, PERST# released. */
        int perst_on = first("perst=1"), perst_off = first("perst=0");
        assert(perst_on >= 0 && perst_on < bulk && perst_off > bulk);
        assert(first("pinctrl pcie@1c08000") > bulk);
        int rails = first("rail+ vdda-phy"), gdsc = first("on ctl.gdsc"), pgdsc = first("on phy.gdsc");
        assert(rails > perst_on && gdsc > rails && pgdsc > gdsc);
        assert(first("rail+ vdda-pll") > rails && first("rail+ vdda-qref") > rails);
        int aux = first("on ctl.aux"), cfg = first("on ctl.cfg");
        assert(aux > pgdsc && cfg > aux && first("cxo ctl.aux") < aux);
        assert(first("assert ctl.pci") > cfg && first("deassert ctl.pci") > first("assert ctl.pci"));
        assert(first("assert phy.phy_nocsr") > first("deassert ctl.pci"));
        int pipe = first("on phy.pipe");
        assert(pipe > first("on phy.rchng") && first("deassert phy.phy_nocsr") > pipe);
        assert(perst_off > pipe);
        /* rchng at the DT's 100 MHz before its gate (CXO cannot give it). */
        int rchng = first("rate phy.rchng 100000000");
        assert(rchng >= 0 && rchng < first("on phy.rchng"));
        /* PIPE mux on the PHY output once the GDSCs are up, before the
         * PIPE gate; AUX mux on XO before the halt-checked PHY aux gate and
         * on the PHY output once the PHY reports ready (after PCS start). */
        int pipe_phy = first("pipe_mux=0"), aux_ref = first("aux_mux=2"), aux_phy = first("aux_mux=0");
        assert(pipe_phy > pgdsc && pipe_phy < pipe);
        assert(aux_ref >= 0 && aux_ref < first("on phy.aux") && aux_phy > first("on phy.aux"));
        assert(aux_phy > pipe && PipeMuxSrc == 0 && AuxMuxSrc == 0);
        /* QMP tables: serdes, both lanes of TX/RX, PCS, PCS misc, lane shared. */
        assert(get(Phy, 0x1000, 4) == 0x26);            /* SSC_STEP_SIZE1_MODE1 */
        assert(get(Phy, 0x0000 + 0x34, 4) == get(Phy, 0x0800 + 0x34, 4));
        assert(get(Phy, 0x1244, 4) == 3);               /* SERDES_START | PCS_START */
        assert(get(Phy, 0x1240, 4) == 3);               /* SW_PWRDN | REFCLK_DRV_DSBL */
        assert(get(Parf, 0x1b0, 4) & 0x100);            /* LTSSM enabled */
        assert(!(get(Parf, 0x2c00, 4) & 1));            /* BDF-to-SID table in use */
        assert(model_get(Root, 0x18, 4) == 0x00ff0100);
        assert(!ep->bars[0].inherited);
    }

    /* Handoff. */
    assert(!kshim_pci_quiesce());
    int bulk_off = last("bulk- usb-controller@0");
    assert(bulk_off >= 0);
    if (!COLD) {
        assert(!memcmp(Atu, SavedAtu, sizeof(Atu)));
        assert(!memcmp(Parf, SavedParf, sizeof(Parf)));
        model_expect(Saved);
        assert(!count_prefix("off ") && !count_prefix("gpio"));
    } else {
        /* PERST# asserted again before the endpoint rails are dropped, every
         * clock/GDSC/rail released, BCRs left asserted, pin returned. */
        assert(PerstLevel == 1 && last("perst=1") < bulk_off);
        assert(!(get(Parf, 0x1b0, 4) & 0x100));
        for (unsigned i = 0; i < ClkCount; ++i) assert(!Clks[i].refs);
        for (unsigned i = 0; i < RegCount; ++i) assert(!Regs[i].refs);
        assert(last("assert ctl.pci") > last("deassert ctl.pci"));
        assert(last("off ctl.gdsc") > last("off ctl.aux"));
        assert(last("gpio- 97") > last("perst=1"));
        assert(last("rail- vdda-phy") > last("perst=1"));
        /* Both muxes back on XO: PIPE after its gate stopped, AUX before the
         * PHY gates and GDSC go (the clock layer then restores the
         * inherited values with its own snapshot). */
        assert(PipeMuxSrc == 2 && AuxMuxSrc == 2);
        assert(last("pipe_mux=2") > last("off phy.pipe"));
        assert(last("aux_mux=2") < last("off phy.aux") && last("aux_mux=2") < last("off phy.gdsc"));
    }
    assert(!kshim_pci_quiesce());
    printf("pci_qcom_test scenario %d: ok\n", SCENARIO);
    return 0;
}
