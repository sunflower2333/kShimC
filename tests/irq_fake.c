/* SPDX-License-Identifier: MIT */
/* Host fakes for the Qualcomm interrupt cascades; see irq_fake.h. */
#include "irq_fake.h"

#include <assert.h>
#include <string.h>

#include <qcom_dt.h>
#include <qcom_irq.h>
#include <qcom_key_platform.h>
#include <kshim/time.h>

/* ---- GIC core --------------------------------------------------------- */

struct fake_gic_irq FakeGic[1020];
bool FakeIrqReady = true;
int FakeIrqMasked;
bool FakeInHandler;

bool kshim_irq_ready(void) { return FakeIrqReady; }
int kshim_irq_init(const void *fdt) { (void)fdt; return 0; }
int kshim_irq_cpu_init(void) { return 0; }
int kshim_irq_send_sgi(uint32_t sgi, uint64_t mpidr) { (void)sgi; (void)mpidr; return ERROR_NOT_SUPPORTED; }
int kshim_irq_teardown(void) { return 0; }

unsigned long kshim_irq_save(void) { return (unsigned long)FakeIrqMasked++; }
void kshim_irq_restore(unsigned long flags)
{
    assert(FakeIrqMasked > 0 && (unsigned long)FakeIrqMasked - 1U == flags);
    --FakeIrqMasked;
}

int kshim_irq_request(uint32_t intid, unsigned trigger, kshim_irq_handler_t handler, void *context)
{
    assert(intid < 1020 && handler);
    /* The GIC implements only level-high and rising-edge senses. */
    assert(trigger == KSHIM_IRQ_TYPE_NONE || trigger == KSHIM_IRQ_TYPE_LEVEL_HIGH ||
           trigger == KSHIM_IRQ_TYPE_EDGE_RISING);
    struct fake_gic_irq *g = &FakeGic[intid];
    if (g->requested) return ERROR_DEVICE_ERROR;
    g->requested = g->enabled = true;
    g->trigger = trigger; g->handler = handler; g->context = context;
    ++g->requests;
    return 0;
}

int kshim_irq_free(uint32_t intid)
{
    assert(intid < 1020);
    struct fake_gic_irq *g = &FakeGic[intid];
    if (!g->requested) return ERROR_NOT_FOUND;
    g->requested = g->enabled = false;
    g->handler = NULL; g->context = NULL;
    ++g->frees;
    return 0;
}

void kshim_irq_enable(uint32_t intid) { assert(intid < 1020); FakeGic[intid].enabled = FakeGic[intid].requested; }
void kshim_irq_disable(uint32_t intid) { assert(intid < 1020); FakeGic[intid].enabled = false; ++FakeGic[intid].disables; }

/* kshim_irq_from_fdt() is the core's (kernel/dt/irq.c), linked as is. */

void fake_gic_line(uint32_t intid, bool (*line)(void)) { FakeGic[intid].line = line; }

int fake_gic_run(void)
{
    int delivered = 0;
    for (unsigned round = 0; round < 5000; ++round) {
        bool progress = false;
        for (unsigned id = 0; id < 1020; ++id) {
            struct fake_gic_irq *g = &FakeGic[id];
            if (!g->requested || !g->enabled || !g->line || !g->line()) continue;
            assert(!FakeIrqMasked);
            FakeIrqMasked = 1; FakeInHandler = true;
            g->handler(id, g->context);
            FakeIrqMasked = 0; FakeInHandler = false;
            ++g->delivered; ++delivered;
            progress = true;
        }
        if (!progress) return delivered;
    }
    return -1;
}

/* ---- MMIO and time ---------------------------------------------------- */

#define STORE_SIZE (1U << 17)
static struct { uintptr_t address; uint32_t value; bool used; } Store[STORE_SIZE];
struct fake_region { uintptr_t base; size_t size; fake_read_fn read; fake_write_fn write; };
static struct fake_region Regions[16];
static unsigned RegionCount;
#define WRITE_LOG 8192U
static uintptr_t WriteLog[WRITE_LOG];
static unsigned WriteCount;
uint64_t FakeNowUs;

static unsigned slot(uintptr_t address)
{
    unsigned h = (unsigned)((address >> 2) * 2654435761U) & (STORE_SIZE - 1U);
    while (Store[h].used && Store[h].address != address) h = (h + 1U) & (STORE_SIZE - 1U);
    return h;
}
uint32_t fake_peek(uintptr_t address) { unsigned h = slot(address); return Store[h].used ? Store[h].value : 0; }
void fake_poke(uintptr_t address, uint32_t value)
{
    unsigned h = slot(address);
    Store[h].used = true; Store[h].address = address; Store[h].value = value;
}
void fake_mmio_region(uintptr_t base, size_t size, fake_read_fn read, fake_write_fn write)
{
    assert(RegionCount < 16);
    Regions[RegionCount++] = (struct fake_region){base, size, read, write};
}
static int region(uintptr_t a)
{
    for (unsigned i = 0; i < RegionCount; ++i)
        if (a >= Regions[i].base && a - Regions[i].base < Regions[i].size) return (int)i;
    return -1;
}
static uint32_t io_read(void *context, uintptr_t address)
{
    (void)context;
    assert((address & 3U) == 0);
    int r = region(address);
    return r >= 0 && Regions[r].read ? Regions[r].read(address) : fake_peek(address);
}
static void io_write(void *context, uintptr_t address, uint32_t value)
{
    (void)context;
    assert((address & 3U) == 0);
    WriteLog[WriteCount++ % WRITE_LOG] = address;
    int r = region(address);
    if (r >= 0 && Regions[r].write) Regions[r].write(address, value);
    else fake_poke(address, value);
}
static uint64_t io_now(void *context) { (void)context; return FakeNowUs; }
static void io_delay(void *context, uint32_t us) { (void)context; FakeNowUs += us; }
const struct CrIo FakeIo = {NULL, io_read, io_write, io_now, io_delay};
unsigned fake_writes_in(uintptr_t base, size_t size)
{
    unsigned n = 0, count = WriteCount < WRITE_LOG ? WriteCount : WRITE_LOG;
    for (unsigned i = 0; i < count; ++i)
        if (WriteLog[i] >= base && WriteLog[i] - base < size) ++n;
    return n;
}
uint64_t get_current_time(void) { return FakeNowUs / 1000U; }

static struct { uintptr_t control; bool level, used; } TPins[16];
void fake_reset(void)
{
    memset(Store, 0, sizeof(Store));
    memset(Regions, 0, sizeof(Regions)); RegionCount = 0;
    memset(FakeGic, 0, sizeof(FakeGic));
    memset(TPins, 0, sizeof(TPins));
    WriteCount = 0;
    FakeIrqReady = true; FakeIrqMasked = 0; FakeInHandler = false;
    kshim_irqchip_set_io(&FakeIo);
}

/* kshim_resources_init's mapper, for kshim_irqchip_map_resources(). */
int kshim_resource_map(struct kshim_resources *r, uint64_t base, uint64_t size,
                       kshim_mmu_memory_type_t type, uint32_t permissions)
{
    if (r->mapping_count == KSHIM_RESOURCE_MAX) return -1;
    r->mappings[r->mapping_count++] = (kshim_mmu_region_t){base, size, type, permissions};
    return 0;
}

/* ---- TLMM: INTR_CFG EN 0, POL 1, DET 3:2, RAW 4, TARGET 7:5 ------------ */

static int tpin(uintptr_t control)
{
    for (unsigned i = 0; i < 16; ++i) if (TPins[i].used && TPins[i].control == control) return (int)i;
    return -1;
}
static void tlmm_relatch(unsigned i)
{
    uint32_t cfg = fake_peek(TPins[i].control + 8);
    bool pol = (cfg >> 1) & 1U, det = (cfg >> 2) & 3U;
    if ((cfg & 0x10U) && !det && TPins[i].level == pol) fake_poke(TPins[i].control + 0xc, 1);
}
static uint32_t tlmm_read(uintptr_t a)
{
    int i = tpin(a & ~(uintptr_t)0xfff);
    if (i >= 0 && (a & 0xfff) == 4) return (fake_peek(a) & ~1U) | TPins[i].level;
    return fake_peek(a);
}
static void tlmm_write(uintptr_t a, uint32_t v)
{
    int i = tpin(a & ~(uintptr_t)0xfff);
    fake_poke(a, (a & 0xfff) == 0xc ? (v & 1U) : v);
    if (i >= 0 && ((a & 0xfff) == 8 || (a & 0xfff) == 0xc)) tlmm_relatch((unsigned)i);
}
void fake_tlmm_pin(uintptr_t control, bool level)
{
    if (!RegionCount || region(0x03000000) < 0) fake_mmio_region(0x03000000, 0xdc2000, tlmm_read, tlmm_write);
    for (unsigned i = 0; i < 16; ++i)
        if (!TPins[i].used) { TPins[i].used = true; TPins[i].control = control; TPins[i].level = level; return; }
    assert(0);
}
void fake_tlmm_set(uintptr_t control, bool level)
{
    int i = tpin(control);
    assert(i >= 0);
    bool old = TPins[i].level;
    TPins[i].level = level;
    uint32_t cfg = fake_peek(control + 8), det = (cfg >> 2) & 3U;
    if ((cfg & 0x10U) && det && old != level && ((level && (det & 1U)) || (!level && (det & 2U))))
        fake_poke(control + 0xc, 1);
    tlmm_relatch((unsigned)i);
}
bool fake_tlmm_line(void)
{
    for (unsigned i = 0; i < 16; ++i) {
        if (!TPins[i].used) continue;
        uint32_t cfg = fake_peek(TPins[i].control + 8);
        if ((cfg & 1U) && (fake_peek(TPins[i].control + 0xc) & 1U) && ((cfg >> 5) & 7U) == 3U) return true;
    }
    return false;
}

/* ---- SPMI arbiter v5 and PMIC ----------------------------------------- */

#define APIDS 16U
static SpmiDeviceContext Spmi;
static bool SpmiUp;
static uint16_t Ppid[APIDS];
static unsigned ApidCount;
static uint32_t AccEnable[APIDS], IrqStatus[APIDS], ChannelWord[APIDS][8], ObsWord[APIDS][8];
static uint8_t Pmic[16][0x10000], Sent[16][256];
static struct { uint8_t sid, peripheral, references; } Grants[16];
unsigned FakeSpmiGrantRefs, FakeSpmiBadWrites;
static uintptr_t Core, Chnls, Obs, Intr, Cnfg;

uint8_t *fake_pmic(uint8_t sid, uint16_t address) { return &Pmic[sid][address]; }
static int apid_of(uint8_t sid, uint8_t peripheral)
{
    for (unsigned a = 0; a < ApidCount; ++a) if (Ppid[a] == ((sid << 8) | peripheral)) return (int)a;
    return -1;
}
static bool granted(unsigned apid)
{
    for (unsigned i = 0; i < 16; ++i)
        if (Grants[i].references && ((Grants[i].sid << 8) | Grants[i].peripheral) == Ppid[apid]) return true;
    return false;
}
/* Latch and message: level types relatch while asserted; a new
 * latched-and-enabled bit sends one interrupt message to the arbiter. */
static void pmic_eval(uint8_t sid, uint8_t peripheral)
{
    uint8_t *r = &Pmic[sid][peripheral << 8];
    uint8_t level = (uint8_t)(~r[0x11] & ((r[0x12] & r[0x10]) | (r[0x13] & ~r[0x10])));
    r[0x18] |= level;
    uint8_t live = r[0x18] & r[0x15], fresh = live & ~Sent[sid][peripheral];
    Sent[sid][peripheral] = live;
    int apid = apid_of(sid, peripheral);
    if (fresh && apid >= 0) IrqStatus[apid] |= fresh;
}
void fake_pmic_rt(uint8_t sid, uint8_t peripheral, uint8_t irq, bool level)
{
    uint8_t *r = &Pmic[sid][peripheral << 8], bit = (uint8_t)(1U << irq);
    bool old = (r[0x10] & bit) != 0;
    r[0x10] = level ? (r[0x10] | bit) : (r[0x10] & ~bit);
    if ((r[0x11] & bit) && old != level && ((level && (r[0x12] & bit)) || (!level && (r[0x13] & bit))))
        r[0x18] |= bit;
    pmic_eval(sid, peripheral);
}
static uint8_t pmic_read(uint8_t sid, uint16_t a)
{
    if ((a & 0xff) == 0x16) return Pmic[sid][(a & 0xff00) | 0x15];
    return Pmic[sid][a];
}
static void pmic_write(uint8_t sid, uint16_t a, uint8_t v)
{
    uint8_t *r = &Pmic[sid][a & 0xff00];
    switch (a & 0xff) {
    case 0x10: case 0x18: break;             /* read-only */
    case 0x14: r[0x18] &= (uint8_t)~v; Sent[sid][a >> 8] &= (uint8_t)~v; break;
    case 0x15: r[0x15] |= v; break;
    case 0x16: r[0x15] &= (uint8_t)~v; break;
    default: Pmic[sid][a] = v; break;
    }
    pmic_eval(sid, (uint8_t)(a >> 8));
}
static void execute(unsigned apid, uint32_t command, bool write, uint32_t *words)
{
    unsigned opcode = command >> 27, count = (command & 7U) + 1U;
    uint8_t sid = (uint8_t)(Ppid[apid] >> 8), peripheral = (uint8_t)Ppid[apid];
    uint16_t address = (uint16_t)((peripheral << 8) | ((command >> 4) & 0xffU));
    assert(opcode == (write ? 0U : 1U));
    for (unsigned i = 0; i < count; ++i) {
        uint32_t *word = &words[(write ? 0x10U : 0x18U) / 4U + i / 4U];
        if (write) pmic_write(sid, (uint16_t)(address + i), (uint8_t)(*word >> (8U * (i % 4U))));
        else {
            if (!(i % 4U)) *word = 0;
            *word |= (uint32_t)pmic_read(sid, (uint16_t)(address + i)) << (8U * (i % 4U));
        }
    }
    words[2] = 1U; /* STATUS: done */
}
static uint32_t spmi_read(void *context, UINTN a)
{
    (void)context;
    if (a >= Core && a < Core + 0x1100) {
        unsigned off = (unsigned)(a - Core);
        if (off == 0) return 0x50000000U;
        if (off == 4) return ApidCount;
        if (off >= 0x900 && off < 0x900 + 4 * APIDS) {
            unsigned apid = (off - 0x900) / 4;
            return apid < ApidCount ? (uint32_t)Ppid[apid] << 8 | (1U << 24) : 0;
        }
        return 0;
    }
    if (a >= Cnfg && a < Cnfg + 0x26000) return 0; /* every APID owned by EE 0 */
    if (a >= Chnls && a < Chnls + 0x2000000) {
        unsigned apid = (unsigned)((a - Chnls) >> 16), off = (unsigned)((a - Chnls) & 0xffff);
        assert(apid < APIDS);
        if (off == 0x100) return AccEnable[apid];
        if (off == 0x104) return IrqStatus[apid];
        if (off < 0x20) return ChannelWord[apid][off / 4];
        return 0;
    }
    if (a >= Obs && a < Obs + 0x100000) {
        unsigned off = (unsigned)(a - Obs) - 0x10000U * FAKE_SPMI_EE, apid = off / 0x80U;
        assert(apid < APIDS && (off % 0x80U) < 0x20U);
        return ObsWord[apid][(off % 0x80U) / 4];
    }
    if (a >= Intr && a < Intr + 0xa0000) {
        unsigned off = (unsigned)(a - Intr), group = (off - 0x10000U * FAKE_SPMI_EE) / 4U;
        uint32_t bits = 0;
        for (unsigned apid = group * 32; apid < group * 32 + 32 && apid < ApidCount; ++apid)
            if (IrqStatus[apid] && (AccEnable[apid] & 1U)) bits |= 1U << (apid % 32);
        return bits;
    }
    assert(0 && "SPMI read outside the arbiter");
    return 0;
}
static void spmi_write(void *context, UINTN a, UINT32 v)
{
    (void)context;
    if (a >= Chnls && a < Chnls + 0x2000000) {
        unsigned apid = (unsigned)((a - Chnls) >> 16), off = (unsigned)((a - Chnls) & 0xffff);
        assert(apid < APIDS);
        /* kShimC's transport: only claimed peripherals' words are writable. */
        if (!granted(apid)) { ++FakeSpmiBadWrites; return; }
        if (off == 0x100) { AccEnable[apid] = v; return; }
        if (off == 0x108) { IrqStatus[apid] &= ~v; return; }
        assert(off < 0x20);
        ChannelWord[apid][off / 4] = v;
        if (off == 0) execute(apid, v, true, ChannelWord[apid]);
        return;
    }
    if (a >= Obs && a < Obs + 0x100000) {
        unsigned off = (unsigned)(a - Obs) - 0x10000U * FAKE_SPMI_EE, apid = off / 0x80U;
        if (!granted(apid)) { ++FakeSpmiBadWrites; return; }
        assert((off % 0x80U) == 0);
        execute(apid, v, false, ObsWord[apid]);
        return;
    }
    ++FakeSpmiBadWrites;
}
static void spmi_stall(void *context, UINT32 us) { (void)context; FakeNowUs += us; }

SpmiDeviceContext *fake_spmi_init(const void *fdt, const uint16_t *ppids, unsigned count)
{
    static struct kshim_resources r;
    assert(count <= APIDS);
    memset(&r, 0, sizeof(r));
    assert(kshim_dt_spmi(fdt, &r) == 0 && r.spmi_count >= 1);
    memset(Pmic, 0, sizeof(Pmic)); memset(Sent, 0, sizeof(Sent));
    memset(AccEnable, 0, sizeof(AccEnable)); memset(IrqStatus, 0, sizeof(IrqStatus));
    memset(ChannelWord, 0, sizeof(ChannelWord)); memset(ObsWord, 0, sizeof(ObsWord));
    memset(Grants, 0, sizeof(Grants));
    FakeSpmiGrantRefs = FakeSpmiBadWrites = 0;
    memcpy(Ppid, ppids, count * sizeof(*ppids)); ApidCount = count;
    Spmi = r.spmi[0].controller;
    Core = Spmi.Regions[SPMI_MEMORY_REGION_TYPE_CORE].BaseAddress;
    Chnls = Spmi.Regions[SPMI_MEMORY_REGION_TYPE_CH_SLAVES].BaseAddress;
    Obs = Spmi.Regions[SPMI_MEMORY_REGION_TYPE_OBSERVER].BaseAddress;
    Intr = Spmi.Regions[SPMI_MEMORY_REGION_TYPE_INTERRUPT].BaseAddress;
    Cnfg = Spmi.Regions[SPMI_MEMORY_REGION_TYPE_CONFIG].BaseAddress;
    Spmi.Io = (SpmiIoOps){spmi_read, spmi_write, spmi_stall, NULL};
    SpmiDeviceContext *p = &Spmi;
    assert(SpmiLibInit(&p) == CR_SUCCESS && Spmi.Version == SPMI_ARB_VERSION_5);
    SpmiUp = true;
    return &Spmi;
}
uint32_t fake_spmi_acc_enable(uint16_t apid) { return AccEnable[apid]; }
void fake_spmi_set_acc_enable(uint16_t apid, uint32_t value) { AccEnable[apid] = value; }
void fake_spmi_raise(uint16_t apid, uint8_t bits) { IrqStatus[apid] |= bits; }
bool fake_spmi_line(void)
{
    /* periph_irq reaches the GIC only through PDC pin 1. */
    if (!(fake_peek(FAKE_PDC_ENABLE(1)) & 2U)) return false;
    for (unsigned apid = 0; apid < ApidCount; ++apid)
        if (IrqStatus[apid] && (AccEnable[apid] & 1U)) return true;
    return false;
}

SpmiDeviceContext *kshim_qcom_spmi_context(unsigned controller)
{
    return controller == 0 && SpmiUp ? &Spmi : NULL;
}
int kshim_qcom_spmi_grant(unsigned controller, uint8_t sid, uint8_t peripheral)
{
    assert(controller == 0 && FakeIrqMasked);
    for (unsigned i = 0; i < 16; ++i)
        if (Grants[i].references && Grants[i].sid == sid && Grants[i].peripheral == peripheral) {
            ++Grants[i].references; ++FakeSpmiGrantRefs; return 0;
        }
    for (unsigned i = 0; i < 16; ++i)
        if (!Grants[i].references) {
            Grants[i].sid = sid; Grants[i].peripheral = peripheral; Grants[i].references = 1;
            ++FakeSpmiGrantRefs; return 0;
        }
    return -1;
}
void kshim_qcom_spmi_revoke(unsigned controller, uint8_t sid, uint8_t peripheral)
{
    assert(controller == 0);
    for (unsigned i = 0; i < 16; ++i)
        if (Grants[i].references && Grants[i].sid == sid && Grants[i].peripheral == peripheral) {
            --Grants[i].references; --FakeSpmiGrantRefs; return;
        }
    assert(0 && "revoke without grant");
}
