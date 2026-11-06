/* SPDX-License-Identifier: MIT */
/*
 * GENI I2C controller and I2C core over the GENI register model, with the
 * SE interrupt on the fake GIC: on-demand probe of the Pocket S2 &i2c4 (GT911
 * bus) and the andromeda FTM5 bus, repeated START and STOP runs, NACK and
 * timeout recovery, a poisoned engine recovered, interrupt-driven waits vs
 * the polling fallback (no GIC, interrupt never delivered), re-entry, and
 * the restore of the inherited SE state at handoff.
 */
#include <kshim/i2c.h>
#include <kshim/qup.h>
#include <kshim/io.h>
#include <qcom_dt.h>
#include <Library/cr_geni_regs.h>

#include "qup_fakes.h"
#include "../irq_fake.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define I2C4 0xa90000U
#define I2C4_IRQ KSHIM_IRQ_SPI(357)
#define INHERITED_IRQ_EN 0x55U  /* what the boot chain left in SE_IRQ_EN */
#define STOP_STRETCH I2C_GENI_M_CMD_PARAM_STOP_STRETCH

static const void *S2;
static int I2c4Node, TouchNode;

static int node_named(const void *f, const char *path)
{
    int node = fdt_path_offset(f, path);
    assert(node >= 0);
    return node;
}

static void world(uintptr_t base, unsigned irq)
{
    fake_reset();
    fake_clk_reset();
    kshim_qup_set_io(&FakeIo);
    FakeConsoleBase = 0x89c000;
    geni_model_reset(base, 3, INHERITED_IRQ_EN);
    if (irq) fake_gic_line(irq, geni_model_line);
}

static unsigned address_of(const struct geni_cmd *c) { return (c->params >> 9) & 0x7fU; }

static void test_probe_and_transfer(void)
{
    world(I2C4, I2C4_IRQ);
    assert(!kshim_qup_init(S2));
    struct kshim_i2c_adapter *a = NULL;
    assert(!kshim_i2c_adapter_get(S2, I2c4Node, &a) && a);
    assert(a->node == I2c4Node && a->bus_hz == 400000 && a->irq_driven && !strcmp(a->name, "i2c@a90000"));
    struct kshim_i2c_adapter *again;
    assert(!kshim_i2c_adapter_get(S2, I2c4Node, &again) && again == a);
    struct kshim_qup_se *se = kshim_qup_se_by_base(I2C4);
    assert(se->owner == KSHIM_QUP_OWNER_I2C && se->acquired && se->intid == I2C4_IRQ);
    /* CrGeniInit: 400 kHz SCL timing, FIFO mode, M status enabled; the
     * engine adds SE_IRQ_EN.GENI_M so that status reaches SPI 357. */
    assert(geni_model_reg(I2C_GENI_SE_SCL_COUNTERS) == ((5U << 20) | (12U << 10) | 24U));
    assert(geni_model_reg(GENI_SE_IRQ_EN) == GENI_SE_IRQ_GENI_M_EN);
    assert(geni_model_reg(GENI_SE_M_IRQ_EN) & GENI_SE_M_IRQ_CMD_DONE);
    assert(FakeGic[I2C4_IRQ].requested && FakeGic[I2C4_IRQ].enabled);

    /* GT911 register read: write the 16-bit register, repeated START, read. */
    struct kshim_i2c_client client;
    assert(!kshim_i2c_client_from_node(S2, TouchNode, &client) && client.addr == 0x5d && client.adapter == a);
    static const uint8_t reg[2] = {0x81, 0x40};
    uint8_t data[4] = {0};
    memcpy(Geni.rx_data, "\x39\x31\x31\x00", 4);
    unsigned waits = FakeWaits;
    assert(!kshim_i2c_write_read(&client, reg, 2, data, 4, 0));
    assert(!memcmp(data, "911", 4) && Geni.tx_log_len == 2 && !memcmp(Geni.tx_log, reg, 2));
    assert(Geni.cmd_count == 2);
    assert(Geni.cmds[0].opcode == I2C_GENI_M_CMD_OPCODE_WRITE && Geni.cmds[0].tx_len == 2);
    assert(address_of(&Geni.cmds[0]) == 0x5d && (Geni.cmds[0].params & STOP_STRETCH));
    assert(Geni.cmds[1].opcode == I2C_GENI_M_CMD_OPCODE_READ && Geni.cmds[1].rx_len == 4);
    assert(address_of(&Geni.cmds[1]) == 0x5d && !(Geni.cmds[1].params & STOP_STRETCH));
    /* Interrupt-driven: the CPU slept and the SE interrupt woke it. */
    assert(FakeWaits > waits && FakeGic[I2C4_IRQ].delivered >= 2);
    assert(!FakeGic[I2C4_IRQ].enabled || !geni_model_line());

    /* KSHIM_I2C_M_STOP splits the list into runs, each ending with STOP. */
    uint8_t x = 1, y = 2;
    struct kshim_i2c_msg runs[3] = {
        {0x5d, KSHIM_I2C_M_STOP, 1, &x}, {0x5d, 0, 1, &y}, {0x14, KSHIM_I2C_M_RD, 1, &y},
    };
    Geni.cmd_count = 0;
    assert(!kshim_i2c_transfer(a, runs, 3, 0));
    assert(Geni.cmd_count == 3 && !(Geni.cmds[0].params & STOP_STRETCH));
    assert((Geni.cmds[1].params & STOP_STRETCH) && !(Geni.cmds[2].params & STOP_STRETCH));
    assert(address_of(&Geni.cmds[2]) == 0x14);

    /* Unsupported by GENI, rejected before the first START. */
    struct kshim_i2c_msg ten = {0x5d, KSHIM_I2C_M_TEN, 1, &x}, empty = {0x5d, 0, 0, &x};
    Geni.cmd_count = 0;
    assert(kshim_i2c_transfer(a, &ten, 1, 0) == ERROR_NOT_SUPPORTED);
    assert(kshim_i2c_transfer(a, &empty, 1, 0) == ERROR_NOT_SUPPORTED);
    struct kshim_i2c_msg many[KSHIM_I2C_GENI_MAX_RUN + 1];
    for (unsigned i = 0; i <= KSHIM_I2C_GENI_MAX_RUN; ++i) many[i] = (struct kshim_i2c_msg){0x5d, 0, 1, &x};
    assert(kshim_i2c_transfer(a, many, KSHIM_I2C_GENI_MAX_RUN + 1, 0) == ERROR_INVALID_PARAMETER);
    many[KSHIM_I2C_GENI_MAX_RUN - 1].flags = KSHIM_I2C_M_STOP;
    assert(!kshim_i2c_transfer(a, many, KSHIM_I2C_GENI_MAX_RUN + 1, 0));
    assert(Geni.cmd_count == KSHIM_I2C_GENI_MAX_RUN + 1);
    assert(kshim_i2c_transfer(a, NULL, 1, 0) == ERROR_INVALID_PARAMETER);
    printf("probe and transfer: ok\n");
}

static void test_recovery(struct kshim_i2c_adapter *a)
{
    uint8_t x = 0x55;
    struct kshim_i2c_msg write = {0x5d, 0, 1, &x};

    /* Address NACK: reported, then cancel and STOP; the bus works again. */
    Geni.nack_address = 0x5d;
    Geni.cmd_count = Geni.cancels = 0;
    assert(kshim_i2c_transfer(a, &write, 1, 0) == KSHIM_IO_ERR_NACK);
    assert(Geni.cancels == 1 && Geni.cmds[Geni.cmd_count - 1].opcode == I2C_GENI_M_CMD_OPCODE_STOP_ON_BUS);
    Geni.nack_address = -1;
    assert(!kshim_i2c_transfer(a, &write, 1, 0));

    /* A stuck transfer: ERROR_TIMEOUT within the bound, controller idle. */
    Geni.hang = true;
    uint64_t start = FakeNowUs;
    assert(kshim_i2c_transfer(a, &write, 1, 2000) == ERROR_TIMEOUT);
    assert(FakeNowUs - start >= 2000 && FakeNowUs - start < 6000);
    assert(!Geni.m_active && Geni.cmds[Geni.cmd_count - 1].opcode == I2C_GENI_M_CMD_OPCODE_STOP_ON_BUS);
    assert(!kshim_i2c_transfer(a, &write, 1, 0));

    /* Cancel and abort both fail: the engine is poisoned. Once the hardware
     * lets go, the next transfer recovers it (quiesce + init). */
    Geni.hang = Geni.cancel_fails = Geni.abort_fails = true;
    assert(kshim_i2c_transfer(a, &write, 1, 1000) == ERROR_TIMEOUT);
    assert(kshim_i2c_transfer(a, &write, 1, 1000) == ERROR_DEVICE_ERROR); /* still stuck */
    Geni.cancel_fails = Geni.abort_fails = false;
    assert(!kshim_i2c_transfer(a, &write, 1, 0));
    assert(geni_model_reg(GENI_SE_IRQ_EN) == GENI_SE_IRQ_GENI_M_EN && a->irq_driven);
    assert(!kshim_i2c_transfer(a, &write, 1, 0));
    printf("recovery: ok\n");
}

/* A 256-byte register device at 0x50 on the bus model (geni_i2c_target):
 * a write sets the pointer, further bytes are stored, reads stream from the
 * pointer. It NACKs the data byte at nack_at (0 = never). */
static struct { uint8_t mem[256], pointer; unsigned index, nack_at, stops; bool reading; } Eeprom;
static bool eeprom_start(void *c, bool read, bool repeated)
{
    (void)c; (void)repeated;
    Eeprom.index = 0;
    Eeprom.reading = read;
    return true;
}
static bool eeprom_write(void *c, uint8_t byte)
{
    (void)c;
    if (++Eeprom.index == Eeprom.nack_at) return false;
    if (Eeprom.index == 1) Eeprom.pointer = byte;
    else Eeprom.mem[Eeprom.pointer++] = byte;
    return true;
}
static uint8_t eeprom_read(void *c) { (void)c; return Eeprom.mem[Eeprom.pointer++]; }
static void eeprom_stop(void *c) { (void)c; ++Eeprom.stops; }
static const struct geni_i2c_target EepromTarget = {0x50, NULL, eeprom_start, eeprom_write, eeprom_read,
                                                    eeprom_stop};

static void test_devices(struct kshim_i2c_adapter *a)
{
    /* Bus devices instead of canned data: the device sees START, repeated
     * START, data and STOP as the controller issues them. */
    memset(&Eeprom, 0, sizeof(Eeprom));
    for (unsigned i = 0; i < 256; ++i) Eeprom.mem[i] = (uint8_t)(i ^ 0xa5);
    geni_model_i2c_target(&EepromTarget);
    geni_model_log_clear();
    struct kshim_i2c_client client = {.adapter = a, .node = -1, .addr = 0x50};
    static const uint8_t reg = 0x10;
    uint8_t data[4];
    assert(!kshim_i2c_write_read(&client, &reg, 1, data, 4, 0));
    assert(data[0] == (0x10 ^ 0xa5) && data[3] == (0x13 ^ 0xa5) && Eeprom.stops == 1);
    assert(!strcmp(Geni.bus_log, "S 50/W A 10 Sr 50/R A [4] P "));

    /* Data NACK on the second byte of a write (0x01 is clocked, then
     * NACKed): NACK reported, the rest not clocked, STOP sent by the
     * recovery; the bus works again. */
    static const uint8_t page[4] = {0x20, 1, 2, 3};
    Eeprom.nack_at = 2;
    geni_model_log_clear();
    unsigned stops = Geni.stops;
    assert(kshim_i2c_write(&client, page, 4, 0) == KSHIM_IO_ERR_NACK);
    assert(Geni.data_nacks == 1 && Geni.stops == stops + 1 && Eeprom.stops == 2);
    assert(!strcmp(Geni.bus_log, "S 50/W A 20 01 N P "));
    assert(Geni.cmds[Geni.cmd_count - 1].opcode == I2C_GENI_M_CMD_OPCODE_STOP_ON_BUS);
    Eeprom.nack_at = 0;
    assert(!kshim_i2c_write(&client, page, 4, 0) && Eeprom.mem[0x20] == 1 && Eeprom.mem[0x22] == 3);

    /* Nobody at 0x51: address NACK, no data clocked. */
    struct kshim_i2c_client absent = {.adapter = a, .node = -1, .addr = 0x51};
    geni_model_log_clear();
    assert(kshim_i2c_write_read(&absent, &reg, 1, data, 4, 0) == KSHIM_IO_ERR_NACK);
    assert(!strcmp(Geni.bus_log, "S 51/W N P "));
    assert(!kshim_i2c_write_read(&client, &reg, 1, data, 1, 0) && data[0] == (0x10 ^ 0xa5));
    /* Back to canned data for the tests below. */
    Geni.i2c_count = 0;
    printf("bus devices (repeated START, data NACK, address NACK): ok\n");
}

static struct kshim_i2c_adapter *Reentrant;
static int ReentryStatus = -1;
static void reenter(void)
{
    uint8_t x = 0;
    struct kshim_i2c_msg m = {0x5d, 0, 1, &x};
    ReentryStatus = kshim_i2c_transfer(Reentrant, &m, 1, 0);
}

static void test_reentry(struct kshim_i2c_adapter *a)
{
    uint8_t x = 0;
    struct kshim_i2c_msg m = {0x5d, 0, 1, &x};
    Reentrant = a;
    FakeWaitHook = reenter;
    assert(!kshim_i2c_transfer(a, &m, 1, 0));
    assert(ReentryStatus == ERROR_BUSY && !a->busy);
    printf("re-entry: ok\n");
}

static bool never(void) { return false; }

static void test_undelivered(struct kshim_i2c_adapter *a)
{
    /* The line resolves but never fires: after a few missed sleeps the
     * engine polls and frees the interrupt; transfers keep working. */
    fake_gic_line(I2C4_IRQ, never);
    uint8_t data[64];
    struct kshim_i2c_msg read = {0x5d, KSHIM_I2C_M_RD, sizeof(data), data};
    for (unsigned i = 0; i < 10 && a->irq_driven; ++i) assert(!kshim_i2c_transfer(a, &read, 1, 0));
    assert(!a->irq_driven && !FakeGic[I2C4_IRQ].requested);
    assert(!(geni_model_reg(GENI_SE_IRQ_EN) & GENI_SE_IRQ_GENI_M_EN));
    unsigned waits = FakeWaits;
    assert(!kshim_i2c_transfer(a, &read, 1, 0) && FakeWaits == waits); /* busy-wait poll */
    assert(!kshim_qup_se_by_base(I2C4)->intid);
    printf("undelivered interrupt: ok\n");
}

static void test_android_polled_first(const void *andromeda)
{
    /* FTM5 bus on andromeda before the GIC is up: polled, then the
     * interrupt (PDC pass-through SPI 373) is taken on a later transfer. */
    world(0xc80000, KSHIM_IRQ_SPI(0x175));
    FakeConsoleBase = 0xa90000;
    assert(!kshim_qup_init(andromeda));
    struct kshim_qup_se *se = kshim_qup_se_by_base(0xc80000);
    FakeIrqReady = false;
    struct kshim_i2c_adapter *a;
    assert(!kshim_i2c_adapter_get(andromeda, se->node, &a) && !a->irq_driven && a->bus_hz == 400000);
    uint8_t cmd = 0x87, id[3];
    struct kshim_i2c_msg msgs[2] = {{0x49, 0, 1, &cmd}, {0x49, KSHIM_I2C_M_RD, 3, id}};
    unsigned waits = FakeWaits;
    assert(!kshim_i2c_transfer(a, msgs, 2, 0) && FakeWaits == waits && !a->irq_driven);
    FakeIrqReady = true;
    assert(!kshim_i2c_transfer(a, msgs, 2, 0) && a->irq_driven && FakeWaits > waits);
    assert(FakeGic[KSHIM_IRQ_SPI(0x175)].requested && FakeGic[KSHIM_IRQ_SPI(0x175)].delivered);
    printf("android, polled then interrupt-driven: ok\n");
}

static void test_quiesce(void)
{
    assert(!kshim_i2c_quiesce_all());
    /* Every adapter's engine: inherited registers back, interrupt freed,
     * clocks released, ownership dropped. */
    assert(geni_model_reg(GENI_SE_IRQ_EN) == INHERITED_IRQ_EN);
    assert(geni_model_reg(I2C_GENI_SE_SCL_COUNTERS) == 0x1234U && geni_model_reg(GENI_SE_CLK_SEL) == 5);
    struct kshim_qup_se *se = kshim_qup_se_by_base(0xc80000);
    assert(!se->acquired && !se->intid && se->owner == KSHIM_QUP_OWNER_NONE);
    assert(!FakeGic[KSHIM_IRQ_SPI(0x175)].requested && !fake_clk_enabled_by_us());
    struct kshim_i2c_adapter *a;
    assert(kshim_i2c_adapter_get(NULL, se->node, &a) == KSHIM_IO_ERR_QUIESCED);
    uint8_t x = 0;
    struct kshim_i2c_msg m = {0x49, 0, 1, &x};
    assert(kshim_i2c_transfer(a, &m, 1, 0) == KSHIM_IO_ERR_QUIESCED);
    assert(!kshim_i2c_quiesce_all() && !kshim_qup_quiesce_all());
    printf("quiesce: ok\n");
}

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    void *s2 = qup_load_fixture(IO_FIXTURES, "sm8650-ayaneo-pocket-s2.dtb");
    void *andromeda = qup_load_fixture(FIXTURES, "andromeda-fdt.dtb");
    S2 = s2;
    I2c4Node = node_named(s2, "/soc@0/geniqup@ac0000/i2c@a90000");
    TouchNode = node_named(s2, "/soc@0/geniqup@ac0000/i2c@a90000/touchscreen@5d");
    test_probe_and_transfer();
    struct kshim_i2c_adapter *a;
    assert(!kshim_i2c_adapter_get(s2, I2c4Node, &a));
    test_recovery(a);
    test_devices(a);
    test_reentry(a);
    test_undelivered(a);
    /* The Pocket S2 adapter stays registered (and quiesced below). Its
     * engine is no longer modelled once the model moves to andromeda. */
    assert(!a->ops->quiesce(a));
    a->quiesced = true;
    test_android_polled_first(andromeda);
    test_quiesce();
    free(s2);
    free(andromeda);
    puts("qup_i2c_test: ok");
    return 0;
}
