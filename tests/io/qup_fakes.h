/* SPDX-License-Identifier: MIT */
/*
 * Host fakes for the QUP/GENI tests (owner: qup):
 *  - clocks (kshim/clk.h) keyed by <provider, id> specifier, with an
 *    inherited-on state, refcounts and kshim_clk_set_rate_cxo() calls;
 *  - pinctrl (kshim/pinctrl.h) recording which node/state was applied;
 *  - kshim_console_base(), time (delay, kshim_wait_event delivering the fake
 *    GIC's lines, kshim_smp_time_us) on top of tests/irq_fake.c;
 *  - a GENI serial-engine register model (M sequencer for I2C and SPI,
 *    FIFO packing, NACK, hang, cancel/abort, CS state, IRQ line).
 */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <kshim/clk.h>

/* ---- Fixtures ----------------------------------------------------------- */
void *qup_load_fixture(const char *dir, const char *name);
/* First node with `path`, or by compatible + reg base. */
int qup_node_by_compatible_base(const void *fdt, const char *compatible, uint64_t base);

/* ---- Clocks ------------------------------------------------------------- */
struct kshim_clk {
    int provider;
    uint32_t id;
    bool used, on, ours;
    unsigned refs, enables, disables, rate_calls;
    int enable_status, rate_status;
    uint8_t dfs;
};
void fake_clk_reset(void);
/* The clock behind entry `name` of the consumer's clock-names (created). */
struct kshim_clk *fake_clk_of(const void *fdt, int consumer, const char *name);
/* Every clock currently created: all on, or all off. */
void fake_clk_all(bool on);
unsigned fake_clk_enabled_by_us(void);
extern unsigned FakeClkGets;

/* ---- Pinctrl ------------------------------------------------------------ */
extern int FakePinctrlStatus;
extern unsigned FakePinctrlCalls;
extern int FakePinctrlNode;

/* ---- PL011 (kernel/drivers/pl011.c entry points) ------------------------ */
extern uint64_t FakePl011Base;
extern char FakePl011Out[64], FakePl011In;
extern unsigned FakePl011OutLen;

/* ---- Console and time --------------------------------------------------- */
extern uint64_t FakeConsoleBase;
extern unsigned FakeWaits;
/* Called once from inside the next kshim_wait_event() (re-entry tests). */
extern void (*FakeWaitHook)(void);
/* kshim_wait_event() advances FakeNowUs in steps of this many microseconds
 * and stops at the first delivered interrupt. */
#define FAKE_WAIT_STEP_US 5U

/* ---- GENI serial-engine model ------------------------------------------ */
#define GENI_MODEL_DEPTH 16U  /* FIFO words */
struct geni_cmd {
    uint8_t opcode;
    uint32_t params;
    uint32_t tx_len, rx_len;
    uint64_t at;
};
struct geni_model {
    uintptr_t base;
    uint32_t regs[0x4000 / 4];
    unsigned protocol;          /* FW_REVISION_RO PROTOCOL */
    unsigned byte_us;           /* bus time per byte */
    /* M sequencer */
    bool m_active;
    uint8_t opcode;
    uint32_t params, tx_len, rx_len, tx_got, rx_given;
    uint64_t start, done_at;
    uint32_t latched;
    bool done;                  /* CMD_DONE reached for the running command */
    /* Behaviour */
    int nack_address;           /* I2C address that NACKs (-1 none) */
    bool hang, cancel_fails, abort_fails, s_active;
    /* Logs */
    struct geni_cmd cmds[256];
    unsigned cmd_count;
    uint8_t tx_log[4096];
    unsigned tx_log_len;
    uint8_t rx_data[4096];      /* bytes returned by reads, in order */
    unsigned rx_data_pos;
    bool cs;
    unsigned cs_asserts, cs_deasserts, cancels, aborts;
};
extern struct geni_model Geni;
/* Reset the model at `base` (registers zero except FW/HW params) and hook
 * it into FakeIo. IRQ_EN starts at `irq_en` (what the boot chain left). */
void geni_model_reset(uintptr_t base, unsigned protocol, uint32_t irq_en);
/* The SE interrupt line: SE_IRQ_EN.GENI_M and pending enabled M status. */
bool geni_model_line(void);
uint32_t geni_model_reg(uint32_t offset);
