/* SPDX-License-Identifier: MIT */
/* Host fakes for the QUP/GENI tests; see qup_fakes.h. */
#include "qup_fakes.h"
#include "../irq_fake.h"

#include <kshim/pinctrl.h>
#include <pl011_uart.h>
#include <kshim/smp.h>
#include <kshim/time.h>
#include <Library/cr_geni_regs.h>
#include <dt.h>

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- Fixtures ----------------------------------------------------------- */

void *qup_load_fixture(const char *dir, const char *name)
{
    char path[1024];
    snprintf(path, sizeof(path), "%s/%s", dir, name);
    FILE *file = fopen(path, "rb");
    if (!file) fprintf(stderr, "missing fixture %s\n", path);
    assert(file);
    assert(!fseek(file, 0, SEEK_END));
    long size = ftell(file);
    assert(size > 0 && !fseek(file, 0, SEEK_SET));
    void *data = aligned_alloc(8, ((size_t)size + 16384 + 7) & ~(size_t)7);
    assert(data && fread(data, 1, (size_t)size, file) == (size_t)size);
    fclose(file);
    assert(!fdt_open_into(data, data, size + 16384));
    assert(!dt_validate(data, fdt_totalsize(data)));
    return data;
}

int qup_node_by_compatible_base(const void *fdt, const char *compatible, uint64_t base)
{
    for (int n = fdt_node_offset_by_compatible(fdt, -1, compatible); n >= 0;
         n = fdt_node_offset_by_compatible(fdt, n, compatible)) {
        struct dt_range reg;
        if (!dt_reg(fdt, n, 0, &reg) && reg.base == base) return n;
    }
    return -1;
}

/* ---- Clocks ------------------------------------------------------------- */

static struct kshim_clk Clocks[64];
unsigned FakeClkGets;

void fake_clk_reset(void)
{
    memset(Clocks, 0, sizeof(Clocks));
    FakeClkGets = 0;
}

static struct kshim_clk *clock_for(int provider, uint32_t id)
{
    for (unsigned i = 0; i < 64; ++i)
        if (Clocks[i].used && Clocks[i].provider == provider && Clocks[i].id == id) return &Clocks[i];
    for (unsigned i = 0; i < 64; ++i)
        if (!Clocks[i].used) {
            Clocks[i] = (struct kshim_clk){.provider = provider, .id = id, .used = true};
            return &Clocks[i];
        }
    assert(!"clock table full");
    return NULL;
}

int kshim_clk_get(const void *fdt, int consumer, const char *name, struct kshim_clk **clk)
{
    int index = 0;
    if (name) {
        index = dt_string_index(fdt, consumer, "clock-names", name);
        if (index < 0) return ERROR_NOT_FOUND;
    }
    struct dt_spec spec;
    if (!fdt_getprop(fdt, consumer, "clocks", NULL)) return ERROR_NOT_FOUND;
    if (dt_specifier(fdt, consumer, "clocks", "#clock-cells", (unsigned)index, &spec) || spec.count < 1)
        return ERROR_NOT_SUPPORTED;
    *clk = clock_for(spec.node, spec.args[0]);
    ++FakeClkGets;
    return 0;
}

struct kshim_clk *fake_clk_of(const void *fdt, int consumer, const char *name)
{
    struct kshim_clk *clk = NULL;
    assert(!kshim_clk_get(fdt, consumer, name, &clk));
    return clk;
}

void fake_clk_all(bool on)
{
    for (unsigned i = 0; i < 64; ++i)
        if (Clocks[i].used) Clocks[i].on = on;
}

unsigned fake_clk_enabled_by_us(void)
{
    unsigned n = 0;
    for (unsigned i = 0; i < 64; ++i)
        if (Clocks[i].used && Clocks[i].ours) ++n;
    return n;
}

bool kshim_clk_is_on(const struct kshim_clk *clk) { return clk && clk->on; }

int kshim_clk_enable(struct kshim_clk *clk)
{
    assert(clk);
    if (clk->enable_status) return clk->enable_status;
    ++clk->enables;
    ++clk->refs;
    if (!clk->on) { clk->on = true; clk->ours = true; }
    return 0;
}

int kshim_clk_disable(struct kshim_clk *clk)
{
    assert(clk && clk->refs);
    ++clk->disables;
    if (!--clk->refs && clk->ours) { clk->on = false; clk->ours = false; }
    return 0;
}

int kshim_clk_set_rate_cxo(struct kshim_clk *clk, uint8_t *dfs_index)
{
    assert(clk && dfs_index);
    ++clk->rate_calls;
    if (clk->rate_status) return clk->rate_status;
    *dfs_index = clk->dfs;
    return 0;
}

const struct kshim_clk_desc *kshim_clk_desc(const struct kshim_clk *clk) { (void)clk; return NULL; }

/* ---- Pinctrl ------------------------------------------------------------ */

int FakePinctrlStatus;
unsigned FakePinctrlCalls;
int FakePinctrlNode = -1;

int kshim_pinctrl_apply(const void *fdt, int node, const char *name)
{
    ++FakePinctrlCalls;
    FakePinctrlNode = node;
    assert(name && !strcmp(name, "default"));
    if (!fdt_getprop(fdt, node, "pinctrl-0", NULL)) return ERROR_NOT_FOUND;
    return FakePinctrlStatus;
}

/* ---- PL011 --------------------------------------------------------------- */

uint64_t FakePl011Base;
char FakePl011Out[64], FakePl011In;
unsigned FakePl011OutLen;

void pl011_init(uint64_t base) { FakePl011Base = base; }
void pl011_write(uint64_t base, char *s, int n)
{
    assert(base == FakePl011Base && n >= 0 && FakePl011OutLen + (unsigned)n <= sizeof(FakePl011Out));
    memcpy(FakePl011Out + FakePl011OutLen, s, (size_t)n);
    FakePl011OutLen += (unsigned)n;
}
int pl011_try_read(uint64_t base, char *c)
{
    assert(base == FakePl011Base);
    if (!FakePl011In) return 0;
    *c = FakePl011In;
    FakePl011In = 0;
    return 1;
}

/* ---- Console and time --------------------------------------------------- */

uint64_t FakeConsoleBase;
unsigned FakeWaits;
void (*FakeWaitHook)(void);

uint64_t kshim_console_base(void) { return FakeConsoleBase; }
uint64_t kshim_smp_time_us(void) { return FakeNowUs; }
uint64_t delay(uint64_t micro_seconds)
{
    FakeNowUs += micro_seconds;
    return micro_seconds;
}

void kshim_wait_event(uint64_t deadline_ms)
{
    uint64_t deadline = deadline_ms * 1000U;
    ++FakeWaits;
    assert(!FakeIrqMasked);
    if (FakeWaitHook) {
        void (*hook)(void) = FakeWaitHook;
        FakeWaitHook = NULL;
        hook();
    }
    do {
        if (fake_gic_run() > 0) return;
        FakeNowUs += FAKE_WAIT_STEP_US;
    } while (FakeNowUs < deadline);
    (void)fake_gic_run();
}

/* ---- GENI serial-engine model ------------------------------------------ */

struct geni_model Geni;

static uint64_t command_time(uint32_t bytes) { return (uint64_t)(bytes + 1U) * Geni.byte_us; }

static uint32_t rx_available(void)
{
    if (Geni.rx_given >= Geni.rx_len || FakeNowUs < Geni.done_at) return 0;
    uint32_t left = Geni.rx_len - Geni.rx_given;
    return left > GENI_MODEL_DEPTH * 4U ? GENI_MODEL_DEPTH * 4U : left;
}

/* Level conditions plus latched events, as the hardware reports them. */
static uint32_t m_status(void)
{
    uint32_t level = 0;
    if (Geni.m_active && !Geni.hang) {
        bool i2c = Geni.protocol == 3;
        uint8_t address = (uint8_t)((Geni.params >> 9) & 0x7fU);
        bool data = Geni.opcode >= 1 && Geni.opcode <= 3;
        if (i2c && data && (int)address == Geni.nack_address &&
            FakeNowUs >= Geni.start + Geni.byte_us) {
            /* Address NACK: GP_IRQ_1 with the command failing; it stays
             * active until cancelled (CrDK then sends STOP). */
            Geni.latched |= I2C_GENI_M_IRQ_NACK | GENI_SE_M_IRQ_CMD_FAILURE;
        } else {
            if (Geni.tx_got < Geni.tx_len && Geni.regs[GENI_SE_TX_WATERMARK_REG / 4])
                level |= GENI_SE_M_IRQ_TX_FIFO_WATERMARK;
            if (rx_available()) {
                level |= GENI_SE_M_IRQ_RX_FIFO_WATERMARK;
                if (Geni.rx_len - Geni.rx_given <= GENI_MODEL_DEPTH * 4U) level |= GENI_SE_M_IRQ_RX_FIFO_LAST;
            }
            if (!Geni.done && FakeNowUs >= Geni.done_at && Geni.tx_got == Geni.tx_len &&
                Geni.rx_len - Geni.rx_given <= GENI_MODEL_DEPTH * 4U) {
                /* Bus phase over; the rest of the read data is in the FIFO. */
                Geni.done = true;
                Geni.latched |= GENI_SE_M_IRQ_CMD_DONE;
                if (Geni.opcode == 8 && Geni.protocol == 1) { Geni.cs = true; ++Geni.cs_asserts; }
                if (Geni.opcode == 9 && Geni.protocol == 1) { Geni.cs = false; ++Geni.cs_deasserts; }
            }
            if (Geni.done && Geni.rx_given == Geni.rx_len) Geni.m_active = false;
        }
    }
    return Geni.latched | level;
}

bool geni_model_line(void)
{
    return (Geni.regs[GENI_SE_IRQ_EN / 4] & GENI_SE_IRQ_GENI_M_EN) &&
           (m_status() & Geni.regs[GENI_SE_M_IRQ_EN / 4]);
}

uint32_t geni_model_reg(uint32_t offset) { return Geni.regs[offset / 4]; }

static uint32_t geni_read(uintptr_t address)
{
    uint32_t reg = (uint32_t)(address - Geni.base);
    switch (reg) {
    case GENI_SE_STATUS:
        (void)m_status();
        return (Geni.m_active ? GENI_SE_STATUS_M_GENI_CMD_ACTIVE : 0) |
               (Geni.s_active ? GENI_SE_STATUS_S_GENI_CMD_ACTIVE : 0);
    case GENI_SE_M_IRQ_STATUS: return m_status();
    case GENI_SE_TX_FIFO_STATUS: return 0; /* drained at once */
    case GENI_SE_RX_FIFO_STATUS: {
        uint32_t bytes = rx_available();
        uint32_t value = (bytes + 3U) / 4U;
        if (bytes && bytes == Geni.rx_len - Geni.rx_given)
            value |= (UINT32_C(1) << 31) | ((bytes % 4U) << 28);
        return value;
    }
    case GENI_SE_RX_FIFOn: {
        assert(rx_available());
        uint32_t word = 0;
        for (unsigned b = 0; b < 4 && Geni.rx_given < Geni.rx_len; ++b) {
            uint8_t byte = Geni.rx_data_pos < sizeof(Geni.rx_data) ? Geni.rx_data[Geni.rx_data_pos] : 0;
            ++Geni.rx_data_pos;
            word |= (uint32_t)byte << (8U * b);
            ++Geni.rx_given;
        }
        return word;
    }
    default:
        return Geni.regs[reg / 4];
    }
}

static void geni_write(uintptr_t address, uint32_t value)
{
    uint32_t reg = (uint32_t)(address - Geni.base);
    switch (reg) {
    case GENI_SE_M_IRQ_CLEAR:
        Geni.latched &= ~value;
        return;
    case GENI_SE_M_CMD_CTRL_REG:
        if (value & GENI_SE_CMD_CTRL_GENI_CMD_CANCEL) {
            ++Geni.cancels;
            if (!Geni.cancel_fails) {
                Geni.m_active = false;
                Geni.hang = false;
                Geni.latched |= GENI_SE_M_IRQ_CMD_CANCEL;
            }
        }
        if (value & GENI_SE_CMD_CTRL_GENI_CMD_ABORT) {
            ++Geni.aborts;
            if (!Geni.abort_fails) {
                Geni.m_active = false;
                Geni.hang = false;
                Geni.latched |= GENI_SE_M_IRQ_CMD_ABORT;
            }
        }
        return;
    case GENI_SE_TX_FIFOn:
        assert(Geni.m_active);
        for (unsigned b = 0; b < 4 && Geni.tx_got < Geni.tx_len; ++b) {
            assert(Geni.tx_log_len < sizeof(Geni.tx_log));
            Geni.tx_log[Geni.tx_log_len++] = (uint8_t)(value >> (8U * b));
            ++Geni.tx_got;
        }
        if (Geni.tx_got == Geni.tx_len && Geni.done_at < FakeNowUs + Geni.byte_us)
            Geni.done_at = FakeNowUs + Geni.byte_us;
        return;
    case GENI_SE_M_CMD0: {
        assert(!Geni.m_active);
        Geni.regs[reg / 4] = value;
        Geni.opcode = (uint8_t)(value >> 27);
        Geni.params = value & 0x07ffffffU;
        bool data = Geni.opcode >= 1 && Geni.opcode <= 3;
        Geni.tx_len = data && (Geni.opcode & 1U) ? Geni.regs[I2C_GENI_SE_TX_TRANS_LEN / 4] : 0;
        Geni.rx_len = data && (Geni.opcode & 2U) ? Geni.regs[I2C_GENI_SE_RX_TRANS_LEN / 4] : 0;
        if (Geni.protocol == 1 && data) assert(Geni.cs && (Geni.params & SPI_GENI_M_CMD_PARAM_FRAGMENTATION));
        Geni.tx_got = Geni.rx_given = 0;
        Geni.done = false;
        Geni.start = FakeNowUs;
        Geni.done_at = FakeNowUs + command_time(Geni.tx_len > Geni.rx_len ? Geni.tx_len : Geni.rx_len);
        Geni.m_active = true;
        assert(Geni.cmd_count < 256);
        Geni.cmds[Geni.cmd_count++] = (struct geni_cmd){Geni.opcode, Geni.params, Geni.tx_len, Geni.rx_len, FakeNowUs};
        return;
    }
    default:
        Geni.regs[reg / 4] = value;
        return;
    }
}

void geni_model_reset(uintptr_t base, unsigned protocol, uint32_t irq_en)
{
    memset(&Geni, 0, sizeof(Geni));
    Geni.base = base;
    Geni.protocol = protocol;
    Geni.byte_us = 25; /* 400 kHz: 9 bits per byte */
    Geni.nack_address = -1;
    Geni.regs[GENI_SE_FW_REVISION_RO / 4] = protocol << 8;
    Geni.regs[GENI_SE_HW_PARAM_0 / 4] = (32U << 24) | (GENI_MODEL_DEPTH << 16);
    Geni.regs[GENI_SE_HW_PARAM_1 / 4] = (32U << 24) | (GENI_MODEL_DEPTH << 16);
    Geni.regs[GENI_SE_IRQ_EN / 4] = irq_en;
    Geni.regs[I2C_GENI_SE_SCL_COUNTERS / 4] = 0x1234U;
    Geni.regs[GENI_SE_CLK_SEL / 4] = 5;
    fake_mmio_region(base, 0x4000, geni_read, geni_write);
}
