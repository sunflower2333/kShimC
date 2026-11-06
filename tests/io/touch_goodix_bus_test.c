/* SPDX-License-Identifier: MIT */
/*
 * Goodix touch over the real bus drivers, on the committed mainline SM8650
 * fixtures (tests/fixtures/io: the Pocket S2 DTB with the RFC touch node,
 * and the QRD DTB). Everything between the boot menu's touch callback and
 * the registers is kShimC code:
 *
 *   kernel/core/input.c (kshim_input_touch_start/poll/quiesce)
 *   -> touch framework (core.c, registry.c) -> goodix_gt911.c / gt9916.c
 *   -> I2C / SPI cores -> geni_i2c.c / geni_spi.c -> geni_engine.c
 *   -> CrDK GeniSeLib FIFO core (unmodified) -> GENI SE registers
 *   -> kernel/drivers/qup/qup.c (ownership, clocks, pinctrl, SE interrupt)
 *   -> kernel/drivers/clk, gpio (registry, pinctrl), regulator (RPMh)
 *   -> kernel/drivers/irqchip/tlmm.c (TLMM summary cascade)
 *   -> kernel/core/io.c (kshim_io_init / kshim_io_quiesce handoff order)
 *
 * What is modelled instead of hardware: the GIC core (tests/irq_fake.c),
 * the GENI serial engine with the GT911 as an I2C target and the GT9916 as
 * an SPI device on it (tests/io/qup_fakes.c), and the SM8650 TLMM, GCC and
 * RPMh RSC/CMD DB registers (tests/io/sm8650_model.c). The chip models see
 * only bus phases, pad levels and RPMh votes.
 *
 * Scenarios (one per process; the registries keep their state for a boot):
 *  s2          Pocket S2 cold: QUP clocks, RCG, pads and ldob14 off. GT911
 *              reset/INT address select through TLMM pads (0x5d), the
 *              first address phase NACKed (CrDK cancel + STOP, driver
 *              retry), register reads as WRITE + repeated START READ with
 *              the SE interrupt ending each wait, IRQ 162 falling edge ->
 *              TLMM summary SPI 208 -> deferred handle -> PRESS/MOVE/RELEASE
 *              at the menu callback; handoff (kshim_input_quiesce, then
 *              kshim_io_quiesce and kshim_irqchip_restore) leaves every
 *              TLMM pad, GCC word, GENI register, RPMh vote and GIC line as
 *              found, with RSTB released before the rail drops.
 *  s2-inherited UEFI left the QUP clocks, ldob14 and a running GT911 at 0x5d:
 *              nothing is pulsed, voted or switched on; still running after
 *              the handoff.
 *  s2-poll     No GIC (kshim_irq_ready() false): the GENI engine busy-polls
 *              and the framework polls the GT911 status at its 10 ms cadence.
 *  qrd         QRD8650 cold, spi4 GT9916: CS assert / TX / RX / deassert
 *              framing in mode 0, reset held while ldob14 comes up, level-low
 *              IRQ 162 deasserted by the event release before the unmask,
 *              handoff with reset asserted before the rail drops.
 */
#include "qup_fakes.h"
#include "sm8650_model.h"
#include "../irq_fake.h"

#include <kshim/clk.h>
#include <kshim/gpio.h>
#include <kshim/i2c.h>
#include <kshim/input.h>
#include <kshim/io.h>
#include <kshim/qup.h>
#include <kshim/regulator.h>
#include <kshim/spi.h>
#include <kshim/touch_driver.h>
#include <Library/cr_geni_regs.h>
#include <qcom_dt.h>
#include <qcom_irq.h>
#include <input_irq.h>
#include <platform.h>
#include "../../kernel/drivers/gpio/internal.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SE 0xa90000UL
#define SE_IRQ KSHIM_IRQ_SPI(357)
#define SUMMARY KSHIM_IRQ_SPI(SM_TLMM_SUMMARY_SPI)
#define RESET_PIN 161U
#define INT_PIN 162U
#define L14B 0x40d00U
#define SE_RCG 0x183c8U          /* gcc_qupv3_wrap1_s4_clk_src (kernel/soc/sm8650_tables.c) */
#define QUP_VOTE 0x52008U        /* APCS vote word of the wrap1 SE and AHB clocks */
#define INHERITED_IRQ_EN 0x55U   /* SE_IRQ_EN as the boot chain left it */

static const char *const QupClocks[] = {
    "gcc_qupv3_wrap1_s4_clk", "gcc_qupv3_wrap_1_m_ahb_clk", "gcc_qupv3_wrap_1_s_ahb_clk",
};

/* ---- Boot environment ----------------------------------------------------- */

static struct kshim_resources Resources;
struct kshim_resources *kshim_resources_get(void) { return &Resources; }
/* kshim_input_quiesce(): no platform keys are configured in this test. */
int kshim_keys_irq_stop(QcomKeys *keys) { (void)keys; return 0; }

static bool rail_on(void) { return sm_rpmh_get(L14B + 4) & 1U; }

/* The boot menu's touch callback (user/bootmenu passes kshim_lvgl_touch). */
static kshim_touch_event_t Events[32];
static unsigned EventCount;
static int MenuContext;
static void menu_touch(void *context, const kshim_touch_event_t *event)
{
    assert(context == &MenuContext && EventCount < 32);
    Events[EventCount++] = *event;
}

static void le16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }

static void *Fdt;
static void world(const char *fixture, unsigned protocol)
{
    fake_reset();                   /* MMIO bus, GIC model, irqchip I/O */
    sm_model_reset();               /* TLMM, GCC, RSC regions */
    kshim_platform_set_io(&FakeIo);
    kshim_qup_set_io(&FakeIo);
    FakeConsoleBase = 0x89c000;     /* uart15, the debug console */
    static const char *const names[] = {"ldob14"};
    static const uint32_t addresses[] = {L14B};
    sm_cmd_db(names, addresses, 1);
    geni_model_reset(SE, protocol, INHERITED_IRQ_EN);
    if (protocol == 1) Geni.byte_us = 8;    /* 1 MHz SPI */
    fake_gic_line(SE_IRQ, geni_model_line);
    fake_gic_line(SUMMARY, sm_tlmm_summary_line);
    /* The SE clock's RCG as found: not on CXO (SRC_SEL 1, divider 2). */
    fake_poke(SM_GCC + SE_RCG + 4, (1U << 8) | 3U);
    Fdt = qup_load_fixture(IO_FIXTURES, fixture);
    Resources.fdt = Fdt;
    EventCount = 0;
}

/* ---- Handoff comparison --------------------------------------------------- */

struct snapshot {
    uint32_t tlmm[SM_TLMM_PINS][4];
    uint32_t gcc[8];
    uint32_t geni[0x4000 / 4];
    uint32_t votes[3];
};

static void take(struct snapshot *s)
{
    for (unsigned pin = 0; pin < SM_TLMM_PINS; ++pin)
        for (unsigned r = 0; r < 4; ++r) s->tlmm[pin][r] = FakeIo.read32(NULL, sm_tlmm_cfg(pin) + 4U * r);
    const uint32_t gcc[8] = {QUP_VOTE, SE_RCG, SE_RCG + 4, SE_RCG + 0x14, 0x183bc, 0x2313c, 0x23140, 0};
    for (unsigned i = 0; i < 8; ++i) s->gcc[i] = gcc[i] ? FakeIo.read32(NULL, SM_GCC + gcc[i]) : 0;
    memcpy(s->geni, Geni.regs, sizeof(s->geni));
    for (unsigned i = 0; i < 3; ++i) s->votes[i] = sm_rpmh_get(L14B + 4U * i);
}

/* Registers a handoff does not restore by design: command and length
 * registers (written per command), FORCE_DEFAULT (a pulse) and the clear
 * registers. Everything else must read as found. */
static bool volatile_geni(uint32_t offset)
{
    switch (offset) {
    case GENI_SE_FORCE_DEFAULT_REG: case GENI_SE_M_CMD0: case GENI_SE_M_CMD_CTRL_REG:
    case GENI_SE_M_IRQ_CLEAR: case GENI_SE_S_IRQ_CLEAR: case I2C_GENI_SE_TX_TRANS_LEN:
    case I2C_GENI_SE_RX_TRANS_LEN:
        return true;
    default:
        return false;
    }
}

static void expect_as_found(const struct snapshot *before)
{
    struct snapshot now;
    take(&now);
    bool same = true;
    for (unsigned pin = 0; pin < SM_TLMM_PINS; ++pin)
        for (unsigned r = 0; r < 4; ++r)
            if (now.tlmm[pin][r] != before->tlmm[pin][r]) {
                fprintf(stderr, "TLMM pad %u +0x%x: 0x%x, found 0x%x\n", pin, 4 * r, now.tlmm[pin][r],
                        before->tlmm[pin][r]);
                same = false;
            }
    for (unsigned i = 0; i < 8; ++i)
        if (now.gcc[i] != before->gcc[i]) {
            fprintf(stderr, "GCC word %u: 0x%x, found 0x%x\n", i, now.gcc[i], before->gcc[i]);
            same = false;
        }
    for (unsigned i = 0; i < 0x4000 / 4; ++i)
        if (!volatile_geni(4 * i) && now.geni[i] != before->geni[i]) {
            fprintf(stderr, "GENI +0x%x: 0x%x, found 0x%x\n", 4 * i, now.geni[i], before->geni[i]);
            same = false;
        }
    for (unsigned i = 0; i < 3; ++i)
        if (now.votes[i] != before->votes[i]) {
            fprintf(stderr, "ldob14 +%u: %u, found %u\n", 4 * i, now.votes[i], before->votes[i]);
            same = false;
        }
    assert(same);
    /* No interrupt kShimC requested is left at the GIC. */
    assert(!FakeGic[SE_IRQ].requested && !FakeGic[SUMMARY].requested);
}

/* The platform handoff (kernel/core/platform.c kshim_platform_quiesce):
 * step 1 input, step 3a I/O in the documented order, step 3b irqchips. */
static void handoff(void)
{
    assert(!kshim_input_quiesce());
    assert(!kshim_io_quiesce());
    assert(!kshim_irqchip_restore());
    /* Idempotent. */
    assert(!kshim_io_quiesce() && !kshim_irqchip_restore());
}

/* ---- GT911 (I2C target on the GENI model) --------------------------------- */

static struct {
    uint8_t mem[0x200];         /* registers 0x8000-0x81ff */
    uint8_t address;            /* latched at the RSTB rising edge; 0 = off or in reset */
    bool rstb;
    unsigned latches, nacks_left, nacks;
    uint16_t pointer;
    unsigned index;
    unsigned status_clears, frames, transactions;
    unsigned rail_drops_with_rstb_driven_high, rstb_rises_unpowered;
    bool rstb_released_with_rail_on;
} G;

static uint8_t *gt911_reg(uint16_t reg)
{
    assert(reg >= 0x8000 && reg < 0x8200);
    return &G.mem[reg - 0x8000];
}

/* INT is the chip's output once it runs (falling-edge mode: idle high). */
static void gt911_int_idle(void) { sm_tlmm_drive(INT_PIN, G.address ? 1 : -1); }

static void gt911_pad(unsigned pin, bool high)
{
    if (pin != RESET_PIN || high == G.rstb) return;
    G.rstb = high;
    if (!high) {
        if (!sm_tlmm_output(RESET_PIN) && rail_on()) G.rstb_released_with_rail_on = true;
        G.address = 0;
    } else if (!rail_on()) {
        ++G.rstb_rises_unpowered;
    } else {
        /* Datasheet address select: INT low at the RSTB rising edge -> 0x5d. */
        G.address = sm_tlmm_level(INT_PIN) ? 0x14 : 0x5d;
        ++G.latches;
    }
    gt911_int_idle();
}

static void gt911_rail(uint32_t address, uint32_t value)
{
    if (address != L14B + 4) return;
    if (value & 1U) return;
    if (sm_tlmm_output(RESET_PIN) && sm_tlmm_level(RESET_PIN)) ++G.rail_drops_with_rstb_driven_high;
    G.address = 0;
    gt911_int_idle();
}

static bool gt911_start(void *context, bool read, bool repeated)
{
    (void)read; (void)repeated;
    uint8_t address = (uint8_t)(uintptr_t)context;
    if (!rail_on() || !G.rstb || address != G.address) return false;
    if (G.nacks_left) { --G.nacks_left; ++G.nacks; return false; }
    G.index = 0;
    ++G.transactions;
    return true;
}

static bool gt911_write(void *context, uint8_t byte)
{
    (void)context;
    if (G.index == 0) G.pointer = (uint16_t)(byte << 8);
    else if (G.index == 1) G.pointer |= byte;
    else {
        if (G.pointer == 0x814e && byte == 0) ++G.status_clears;
        *gt911_reg(G.pointer++) = byte;
    }
    ++G.index;
    return true;
}

static uint8_t gt911_read(void *context)
{
    (void)context;
    if (G.pointer == 0x814e) ++G.frames;
    return *gt911_reg(G.pointer++);
}

static const struct geni_i2c_target Gt911At5d = {0x5d, (void *)(uintptr_t)0x5d, gt911_start, gt911_write,
                                                 gt911_read, NULL};
static const struct geni_i2c_target Gt911At14 = {0x14, (void *)(uintptr_t)0x14, gt911_start, gt911_write,
                                                 gt911_read, NULL};

static void gt911_init(void)
{
    memset(&G, 0, sizeof(G));
    memcpy(gt911_reg(0x8140), "911\0", 4);
    le16(gt911_reg(0x8144), 0x1060);         /* firmware version */
    le16(gt911_reg(0x8146), 720);
    le16(gt911_reg(0x8148), 1280);
    uint8_t *config = gt911_reg(0x8047);
    config[0] = 0x41;
    le16(config + 1, 1440);
    le16(config + 3, 2560);
    config[5] = 5;
    config[6] = 0x01;                        /* Module_Switch1: INT falling, as the DT */
    uint8_t sum = 0;
    for (unsigned i = 0; i < 184; ++i) sum = (uint8_t)(sum + config[i]);
    config[184] = (uint8_t)(0U - sum);
    geni_model_i2c_target(&Gt911At5d);
    geni_model_i2c_target(&Gt911At14);
    SmPadWatch = gt911_pad;
    SmRpmhWatch = gt911_rail;
}

/* A frame: status (ready | count), then 8-byte points from 0x814F, and the
 * INT pulse of the falling-edge mode. */
static void gt911_frame(unsigned count, uint16_t x, uint16_t y)
{
    uint8_t *s = gt911_reg(0x814e);
    s[0] = (uint8_t)(0x80U | count);
    s[1] = 0;
    le16(s + 2, x);
    le16(s + 4, y);
    le16(s + 6, 30);
    assert(G.address && !sm_tlmm_output(INT_PIN));
    sm_tlmm_drive(INT_PIN, 0);
    sm_tlmm_drive(INT_PIN, 1);
}

/* The menu loop: an interrupt arrives (WFI ends), then the next poll. */
static void irq_then_poll(void)
{
    unsigned commands = Geni.cmd_count, taken = FakeGic[SUMMARY].delivered;
    assert(fake_gic_run() >= 1);
    /* The TLMM handler only masked the pin: no bus traffic in IRQ context. */
    assert(FakeGic[SUMMARY].delivered == taken + 1 && Geni.cmd_count == commands);
    assert(!kshim_input_touch_poll());
}

static void s2_common_checks(const struct kshim_touch_dev *dev)
{
    assert(dev && dev->driver == &kshim_touch_driver_gt911 && dev->bus == KSHIM_TOUCH_BUS_I2C);
    assert(dev->i2c.addr == 0x5d && dev->i2c.adapter && !strcmp(dev->i2c.adapter->name, "i2c@a90000"));
    assert(dev->i2c.adapter->bus_hz == 400000);
    struct kshim_qup_se *se = kshim_qup_se_by_base(SE);
    assert(se && se->owner == KSHIM_QUP_OWNER_I2C && se->acquired);
    /* CrGeniInit at 400 kHz from CXO, FIFO mode. */
    assert(geni_model_reg(I2C_GENI_SE_SCL_COUNTERS) == ((5U << 20) | (12U << 10) | 24U));
    assert(geni_model_reg(GENI_SE_SER_M_CLK_CFG) == ((2U << 4) | GENI_SE_SER_CLK_EN));
}

/* Probe traffic: every register read is WRITE(2-byte register, STOP_STRETCH)
 * then a repeated START READ ending with STOP; writes are one WRITE + STOP. */
static void check_register_reads(void)
{
    assert(strstr(Geni.bus_log, "S 5d/W A 81 40 Sr 5d/R A [11] P "));     /* product block */
    assert(strstr(Geni.bus_log, "S 5d/W A 80 47 Sr 5d/R A [185] P "));    /* configuration */
    assert(strstr(Geni.bus_log, "S 5d/W A 81 4e 00 P "));                 /* buffer release */
    assert(Geni.repeated_starts >= 2 && Geni.stops >= Geni.starts);
    for (unsigned i = 0; i + 1 < Geni.cmd_count; ++i) {
        const struct geni_cmd *c = &Geni.cmds[i];
        if (c->opcode != I2C_GENI_M_CMD_OPCODE_WRITE || c->tx_len != 2) continue;
        /* A register address: STOP_STRETCH, then the READ ending the run
         * with STOP, or STOP_ON_BUS after a NACK. */
        const struct geni_cmd *next = &Geni.cmds[i + 1];
        assert(c->params & I2C_GENI_M_CMD_PARAM_STOP_STRETCH);
        assert((next->opcode == I2C_GENI_M_CMD_OPCODE_READ &&
                !(next->params & I2C_GENI_M_CMD_PARAM_STOP_STRETCH)) ||
               next->opcode == I2C_GENI_M_CMD_OPCODE_STOP_ON_BUS);
    }
}

static void pocket_s2_cold(bool interrupts)
{
    world("sm8650-ayaneo-pocket-s2.dtb", 3);
    FakeIrqReady = interrupts;
    gt911_init();
    G.nacks_left = 1;  /* still booting at the first address phase */
    /* As found: RSTB an input on its pull-down, INT an input, rail and QUP
     * clocks off, the SE clock's RCG not on CXO. */
    fake_poke(sm_tlmm_cfg(RESET_PIN), 0x1);
    struct snapshot before;
    take(&before);
    assert(!sm_tlmm_level(RESET_PIN) && !rail_on());

    assert(!kshim_io_init(Fdt));
    assert(!kshim_input_touch_start(1440, 2560, menu_touch, &MenuContext));
    const struct kshim_touch_dev *dev = kshim_touch_active();
    s2_common_checks(dev);
    /* Cold bring-up through the clock driver: AHB and SE votes, RCG on CXO. */
    for (unsigned i = 0; i < 3; ++i) assert(sm_gcc_on(QupClocks[i]));
    assert((fake_peek(SM_GCC + SE_RCG + 4) & 0x371fU) == 1U);
    /* ldob14 at 3200 mV in HPM (regulator-initial-mode), enabled. */
    assert(rail_on() && sm_rpmh_get(L14B) == 3200 && sm_rpmh_get(L14B + 8) == 7);
    /* Pinctrl through the GPIO registry: i2c4 SDA/SCL (gpio48/49) muxed to
     * qup1_se4 with pull-up at 2 mA; RSTB driven high over its pull-down
     * and INT an input without bias, both at 8 mA. */
    for (unsigned pin = 48; pin <= 49; ++pin) {
        uint32_t c = fake_peek(sm_tlmm_cfg(pin));
        assert(((c >> 2) & 0xfU) != 0 && (c & 3U) == 3U && !((c >> 6) & 7U) && !(c & (1U << 9)));
    }
    uint32_t c161 = fake_peek(sm_tlmm_cfg(RESET_PIN)), c162 = fake_peek(sm_tlmm_cfg(INT_PIN));
    assert(!((c161 >> 2) & 0xfU) && (c161 & 3U) == 1U && ((c161 >> 6) & 7U) == 3U && (c161 & (1U << 9)));
    assert(sm_tlmm_level(RESET_PIN));
    assert(!((c162 >> 2) & 0xfU) && !(c162 & 3U) && ((c162 >> 6) & 7U) == 3U && !(c162 & (1U << 9)));
    /* Address select through the TLMM pads: 0x5d, once, never unpowered. */
    assert(G.latches == 1 && G.address == 0x5d && !G.rstb_rises_unpowered);
    /* The first address phase was NACKed: CrDK cancelled the command and
     * sent STOP, the driver retried. */
    assert(G.nacks == 1 && Geni.address_nacks == 1 && Geni.cancels >= 1);
    assert(strstr(Geni.bus_log, "S 5d/W N P "));
    bool stop_on_bus = false;
    for (unsigned i = 0; i < Geni.cmd_count; ++i)
        stop_on_bus |= Geni.cmds[i].opcode == I2C_GENI_M_CMD_OPCODE_STOP_ON_BUS;
    assert(stop_on_bus);
    check_register_reads();

    struct kshim_i2c_adapter *adapter = dev->i2c.adapter;
    if (interrupts) {
        /* The SE interrupt ended the transfer waits (geni_engine.c): one
         * per command, the CPU asleep in WFI in between. */
        assert(adapter->irq_driven && FakeGic[SE_IRQ].requested);
        assert(FakeGic[SE_IRQ].delivered + 1 >= Geni.cmd_count && FakeWaits >= Geni.cmd_count);
        assert(geni_model_reg(GENI_SE_IRQ_EN) == GENI_SE_IRQ_GENI_M_EN);
        /* TLMM 162 falling edge, apps target, summary SPI 208. */
        assert(dev->irq_live && FakeGic[SUMMARY].requested);
        uint32_t intr = FakeIo.read32(NULL, sm_tlmm_cfg(INT_PIN) + 8);
        assert((intr & 1U) && ((intr >> 2) & 3U) == 2U && ((intr >> 8) & 7U) == 3U && (intr & 0x10U));
        assert(!kshim_input_touch_poll());  /* the pass for anything latched before */

        unsigned delivered = FakeGic[SE_IRQ].delivered;
        gt911_frame(1, 700, 1200);
        irq_then_poll();
        assert(EventCount == 1 && Events[0].type == KSHIM_TOUCH_EVENT_PRESS);
        assert(Events[0].x == 700 && Events[0].y == 1200);
        assert(FakeGic[SE_IRQ].delivered > delivered && G.status_clears >= 2);
        /* Unmasked again after the handle; the edge pad keeps RAW_STATUS_EN. */
        assert(FakeIo.read32(NULL, sm_tlmm_cfg(INT_PIN) + 8) & 1U);

        gt911_frame(1, 710, 1190);
        irq_then_poll();
        assert(EventCount == 2 && Events[1].type == KSHIM_TOUCH_EVENT_MOVE && Events[1].x == 710);
        gt911_frame(0, 0, 0);
        irq_then_poll();
        assert(EventCount == 3 && Events[2].type == KSHIM_TOUCH_EVENT_RELEASE);
        /* No spurious pass: nothing latched, nothing read. */
        unsigned frames = G.frames;
        assert(fake_gic_run() == 0 && !kshim_input_touch_poll() && G.frames == frames);
    } else {
        /* Polled: no GIC, so no SE or TLMM interrupt was requested. */
        assert(!adapter->irq_driven && !FakeGic[SE_IRQ].requested && !dev->irq_live);
        assert(geni_model_reg(GENI_SE_IRQ_EN) == 0);
        gt911_frame(1, 700, 1200);
        /* The 10 ms status cadence finds the frame within two polls. */
        for (unsigned i = 0; i < 3 && !EventCount; ++i) {
            FakeNowUs += 11000;
            assert(!kshim_input_touch_poll());
        }
        assert(EventCount == 1 && Events[0].type == KSHIM_TOUCH_EVENT_PRESS && Events[0].x == 700);
        gt911_frame(0, 0, 0);
        FakeNowUs += 11000;
        assert(!kshim_input_touch_poll() && EventCount == 2 && Events[1].type == KSHIM_TOUCH_EVENT_RELEASE);
    }

    /* Handoff: RSTB is released to its pull-down while the rail is still
     * on (no high level into the unpowered chip) and never driven low;
     * then everything reads as found. */
    handoff();
    assert(!rail_on() && G.rstb_released_with_rail_on && !G.rail_drops_with_rstb_driven_high);
    assert(!sm_tlmm_output(RESET_PIN) && !sm_tlmm_level(RESET_PIN));
    for (unsigned i = 0; i < 3; ++i) assert(!sm_gcc_on(QupClocks[i]));
    assert(kshim_qup_se_by_base(SE)->owner == KSHIM_QUP_OWNER_NONE && !kshim_qup_se_by_base(SE)->acquired);
    expect_as_found(&before);
    free(Fdt);
    printf("pocket-s2 GT911 %s over GENI I2C: ok (%u GENI commands, %u SE interrupts)\n",
           interrupts ? "interrupt-driven" : "polled", Geni.cmd_count, FakeGic[SE_IRQ].delivered);
}

static void pocket_s2_inherited(void)
{
    world("sm8650-ayaneo-pocket-s2.dtb", 3);
    gt911_init();
    /* UEFI left: ldob14 on at 3200 mV HPM, RSTB driven high over its
     * pull-down with the pinctrl state applied, the GT911 running at 0x5d,
     * the QUP wrapper and SE clocks running with the SE RCG on CXO. */
    sm_rpmh_set(L14B + 0, 3200);
    sm_rpmh_set(L14B + 4, 1);
    sm_rpmh_set(L14B + 8, 7);
    fake_poke(sm_tlmm_cfg(RESET_PIN), 0x1 | (3U << 6) | (1U << 9));
    fake_poke(sm_tlmm_cfg(RESET_PIN) + 4, 2);
    fake_poke(sm_tlmm_cfg(INT_PIN), 3U << 6);
    G.rstb = true;
    G.address = 0x5d;
    sm_tlmm_drive(INT_PIN, 1);
    for (unsigned i = 0; i < 3; ++i) sm_gcc_inherit_on(QupClocks[i]);
    fake_poke(SM_GCC + SE_RCG + 4, 0);
    struct snapshot before;
    take(&before);
    unsigned gcc_writes = fake_writes_in(SM_GCC, SM_GCC_SIZE);

    assert(!kshim_io_init(Fdt));
    assert(!kshim_input_touch_start(1440, 2560, menu_touch, &MenuContext));
    const struct kshim_touch_dev *dev = kshim_touch_active();
    s2_common_checks(dev);
    assert(kshim_qup_se_by_base(SE)->inherited_active);
    /* Inherit first: no clock write, no RPMh vote, no reset pulse. */
    assert(fake_writes_in(SM_GCC, SM_GCC_SIZE) == gcc_writes && sm_rpmh_writes() == 0);
    assert(G.latches == 0 && G.rstb && G.address == 0x5d && Geni.address_nacks == 0);
    assert(dev->irq_live && dev->i2c.adapter->irq_driven);
    assert(!kshim_input_touch_poll());
    gt911_frame(1, 10, 20);
    irq_then_poll();
    assert(EventCount == 1 && Events[0].type == KSHIM_TOUCH_EVENT_PRESS && Events[0].x == 10);

    handoff();
    /* Still running at 0x5d for the payload, on its inherited rail and clocks. */
    assert(G.rstb && G.address == 0x5d && G.latches == 0 && rail_on() && sm_rpmh_writes() == 0);
    for (unsigned i = 0; i < 3; ++i) assert(sm_gcc_on(QupClocks[i]));
    assert(fake_writes_in(SM_GCC, SM_GCC_SIZE) == gcc_writes);
    expect_as_found(&before);
    free(Fdt);
    puts("pocket-s2 GT911 inherited over GENI I2C: ok");
}

/* ---- GT9916 (Berlin-D, SPI device on the GENI model) --------------------- */

#define BRL_TOUCH 0x10308U
static struct {
    bool rstb;                  /* physical, high = running */
    uint64_t release_us;
    uint8_t bootoption[0x14], version[28], info[1024], touch[128];
    unsigned resets, releases_with_rail_off, rail_drops, rail_drops_with_reset_high;
    /* SPI frame */
    unsigned index, frames, bad_mode, mosi_dummy_errors;
    uint8_t cmd;
    uint32_t addr;
} B;

static void brl_checksum(uint8_t *data, size_t length)
{
    uint16_t sum = 0;
    for (size_t i = 0; i + 2 < length; ++i) sum = (uint16_t)(sum + data[i]);
    le16(&data[length - 2], sum);
}

static uint8_t *brl_byte(uint32_t addr)
{
    struct { uint32_t base; uint8_t *bytes; size_t span; } map[] = {
        {0x10000, B.bootoption, sizeof(B.bootoption)}, {0x10014, B.version, sizeof(B.version)},
        {0x10070, B.info, BRL_TOUCH - 0x10070}, {BRL_TOUCH, B.touch, sizeof(B.touch)},
    };
    for (unsigned i = 0; i < 4; ++i)
        if (addr >= map[i].base && addr < map[i].base + map[i].span) return map[i].bytes + (addr - map[i].base);
    assert(!"GT9916 model: access outside the map");
    return NULL;
}

/* INT (level low) is the chip's output while it runs. */
static void brl_int(void)
{
    sm_tlmm_drive(INT_PIN, rail_on() && B.rstb ? !B.touch[0] : -1);
}

static void brl_pad(unsigned pin, bool high)
{
    if (pin != RESET_PIN || high == B.rstb) return;
    B.rstb = high;
    if (high) {
        if (!rail_on()) ++B.releases_with_rail_off;
        B.release_us = FakeNowUs;
        ++B.resets;
        B.touch[0] = 0;
    }
    brl_int();
}

static void brl_rail(uint32_t address, uint32_t value)
{
    if (address != L14B + 4) return;
    if (!(value & 1U)) {
        ++B.rail_drops;
        if (!sm_tlmm_output(RESET_PIN) || sm_tlmm_level(RESET_PIN)) ++B.rail_drops_with_reset_high;
    }
    brl_int();
}

static void brl_select(void *context, bool asserted, unsigned mode)
{
    (void)context;
    if (!asserted) { brl_int(); return; }
    if (mode != 0) ++B.bad_mode;
    B.index = 0;
    B.cmd = 0;
    B.addr = 0;
    ++B.frames;
}

/* Berlin SPI framing: write 0xF0 addr[31:0] data; read 0xF1 addr[31:0],
 * three dummy bytes, then data. */
static uint8_t brl_exchange(void *context, uint8_t mosi)
{
    (void)context;
    unsigned i = B.index++;
    bool up = rail_on() && B.rstb && FakeNowUs - B.release_us >= 4000U;
    if (i == 0) { B.cmd = mosi; return 0xff; }
    if (i <= 4) { B.addr = B.addr << 8 | mosi; return 0xff; }
    if (!up) return 0xff;
    if (B.cmd == 0xf0) {
        *brl_byte(B.addr + i - 5) = mosi;
        return 0xff;
    }
    if (B.cmd != 0xf1) return 0xff;
    if (i < 8) {
        if (mosi != 0xff) ++B.mosi_dummy_errors;
        return 0xff;
    }
    bool booting = B.addr != 0x10000 && B.addr != BRL_TOUCH && FakeNowUs - B.release_us < 100000U;
    return booting ? 0 : *brl_byte(B.addr + i - 8);
}

static const struct geni_spi_target Gt9916 = {NULL, brl_select, brl_exchange};

static void brl_init(void)
{
    memset(&B, 0, sizeof(B));
    memcpy(B.version, "BERLIN", 6);
    B.version[6] = 1; B.version[7] = 2; B.version[8] = 3;
    memcpy(&B.version[10], "9916R\0\0\0", 8);
    B.version[18] = 0x01; B.version[20] = 0x0b; B.version[21] = 0x03; B.version[23] = 2;
    brl_checksum(B.version, sizeof(B.version));
    uint8_t *info = B.info;
    size_t o = 2;
    for (unsigned i = 0; i < 16; ++i) info[o++] = (uint8_t)(0x10 + i);
    for (unsigned i = 0; i < 10; ++i) info[o++] = (uint8_t)(0x20 + i);
    info[o++] = 17; info[o++] = 38; info[o++] = 0; info[o++] = 0;
    const uint8_t lists[5] = {2, 3, 1, 1, 0};
    for (unsigned l = 0; l < 5; ++l) {
        info[o++] = lists[l];
        for (unsigned i = 0; i < lists[l]; ++i, o += 2) le16(&info[o], (uint16_t)(1000 + i));
    }
    le16(&info[o], 0x0174); le16(&info[o + 2], 0x0001);
    le16(&info[o + 44], (uint16_t)BRL_TOUCH); le16(&info[o + 46], (uint16_t)(BRL_TOUCH >> 16));
    le16(&info[o + 48], 8);
    le16(&info[o + 50], 8);
    o += 102;
    le16(info, (uint16_t)o);
    brl_checksum(info, o);
    geni_model_spi_target(0, &Gt9916);
    SmPadWatch = brl_pad;
    SmRpmhWatch = brl_rail;
}

static void brl_touch(uint8_t id, uint16_t x, uint16_t y)
{
    uint8_t *e = B.touch;
    memset(e, 0, sizeof(B.touch));
    e[0] = 0x80;
    e[2] = 1;
    brl_checksum(e, 8);
    uint8_t *p = &e[8];
    p[0] = (uint8_t)(id << 4);
    le16(&p[2], x);
    le16(&p[4], y);
    le16(&p[6], 40);
    brl_checksum(&e[8], 8 + 2);
    brl_int();
}

static void qrd8650_cold(void)
{
    world("sm8650-qrd.dtb", 1);
    brl_init();
    /* As found: rail off, reset (161) and INT (162) inputs on their pull-ups. */
    fake_poke(sm_tlmm_cfg(RESET_PIN), 0x3);
    fake_poke(sm_tlmm_cfg(INT_PIN), 0x3);
    B.rstb = sm_tlmm_level(RESET_PIN);
    struct snapshot before;
    take(&before);

    assert(!kshim_io_init(Fdt));
    assert(!kshim_input_touch_start(1080, 2400, menu_touch, &MenuContext));
    const struct kshim_touch_dev *dev = kshim_touch_active();
    assert(dev && dev->driver == &kshim_touch_driver_gt9916 && dev->bus == KSHIM_TOUCH_BUS_SPI);
    assert(dev->spi.chip_select == 0 && dev->spi.max_speed_hz == 1000000);
    struct kshim_spi_controller *controller = dev->spi.controller;
    assert(controller && !strcmp(controller->name, "spi@a90000") && controller->irq_driven);
    assert(kshim_qup_se_by_base(SE)->owner == KSHIM_QUP_OWNER_SPI);
    /* CrGeniInit: 8-bit words, mode 0, CS 0, 1 MHz from CXO (divider 20). */
    assert(geni_model_reg(SPI_GENI_SE_WORD_LEN) == 8U - SPI_GENI_MIN_WORD_LEN);
    assert(!geni_model_reg(SPI_GENI_SE_CPHA) && !geni_model_reg(SPI_GENI_SE_CPOL));
    assert(geni_model_reg(SPI_GENI_SE_DEMUX_SEL) == 0);
    assert(geni_model_reg(GENI_SE_SER_M_CLK_CFG) == ((20U << 4) | GENI_SE_SER_CLK_EN));
    /* Reset (active low) held while ldob14 came up, released once after. */
    assert(rail_on() && B.resets == 1 && B.releases_with_rail_off == 0 && B.rstb);
    assert(B.bootoption[0] == 0xaa && !B.bad_mode && !B.mosi_dummy_errors);
    /* Each access is one CS assertion: CS_ASSERT, TX, TX or RX, CS_DEASSERT. */
    assert(Geni.cs_asserts == Geni.cs_deasserts && Geni.cs_asserts == B.frames && !Geni.cs);
    assert(FakeGic[SE_IRQ].delivered > 10);
    assert(dev->irq_live && dev->irq_trigger == KSHIM_IRQ_TYPE_LEVEL_LOW);
    uint32_t intr = FakeIo.read32(NULL, sm_tlmm_cfg(INT_PIN) + 8);
    assert((intr & 1U) && !((intr >> 2) & 3U) && !(intr & 2U) && ((intr >> 8) & 7U) == 3U);
    assert(!kshim_input_touch_poll());

    unsigned frames = B.frames;
    brl_touch(2, 500, 1800);
    assert(!sm_tlmm_level(INT_PIN) && sm_tlmm_summary_line());
    assert(fake_gic_run() >= 1 && B.frames == frames);   /* masked, nothing on the bus */
    assert(!sm_tlmm_summary_line());                      /* level pad masked: RAW off */
    assert(!kshim_input_touch_poll());
    /* Released by the event write before the unmask: the line stayed low. */
    assert(!B.touch[0] && sm_tlmm_level(INT_PIN) && !sm_tlmm_summary_line());
    assert(FakeIo.read32(NULL, sm_tlmm_cfg(INT_PIN) + 8) & 1U);
    assert(EventCount == 1 && Events[0].type == KSHIM_TOUCH_EVENT_PRESS);
    assert(Events[0].x == 500 && Events[0].y == 1800);
    assert(fake_gic_run() == 0);

    /* Handoff: reset asserted (pad low) while the rail drops, then every
     * register as found. */
    handoff();
    assert(!rail_on() && B.rail_drops == 1 && !B.rail_drops_with_reset_high);
    for (unsigned i = 0; i < 3; ++i) assert(!sm_gcc_on(QupClocks[i]));
    expect_as_found(&before);
    free(Fdt);
    printf("qrd8650 GT9916 over GENI SPI: ok (%u SPI frames, %u SE interrupts)\n", B.frames,
           FakeGic[SE_IRQ].delivered);
}

int main(int argc, char **argv)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    const char *which = argc > 1 ? argv[1] : "s2";
    if (!strcmp(which, "s2")) pocket_s2_cold(true);
    else if (!strcmp(which, "s2-poll")) pocket_s2_cold(false);
    else if (!strcmp(which, "s2-inherited")) pocket_s2_inherited();
    else if (!strcmp(which, "qrd")) qrd8650_cold();
    else return 2;
    printf("touch_goodix_bus_test %s: ok\n", which);
    return 0;
}
