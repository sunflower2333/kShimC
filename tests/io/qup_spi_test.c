/* SPDX-License-Identifier: MIT */
/*
 * GENI SPI controller and SPI core over the GENI register model, SE
 * interrupt on the fake GIC: on-demand probe of the QRD8650 &spi4 (GT9916),
 * full-duplex segments under one chip select with a CS hold, zero filling,
 * reprogramming between devices of different mode/speed/CS, unsupported
 * modes, and the restore of the inherited SE configuration at handoff.
 */
#include <kshim/spi.h>
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

#define SPI4 0xa90000U
#define SPI4_IRQ KSHIM_IRQ_SPI(357)
#define FRAG SPI_GENI_M_CMD_PARAM_FRAGMENTATION

static int node_named(const void *f, const char *path)
{
    int node = fdt_path_offset(f, path);
    assert(node >= 0);
    return node;
}

static uint32_t clk_cfg(uint32_t divisor) { return (divisor << 4) | GENI_SE_SER_CLK_EN; }

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    void *qrd = qup_load_fixture(IO_FIXTURES, "sm8650-qrd.dtb");
    int spi4 = node_named(qrd, "/soc@0/geniqup@ac0000/spi@a90000");
    int ts = node_named(qrd, "/soc@0/geniqup@ac0000/spi@a90000/touchscreen@0");

    fake_reset();
    fake_clk_reset();
    kshim_qup_set_io(&FakeIo);
    FakeConsoleBase = 0x89c000;
    geni_model_reset(SPI4, 1, 0x55);
    /* What the boot chain left in the SPI registers. */
    Geni.regs[SPI_GENI_SE_CPHA / 4] = 1;
    Geni.regs[SPI_GENI_SE_DEMUX_SEL / 4] = 2;
    Geni.regs[GENI_SE_SER_M_CLK_CFG / 4] = clk_cfg(7);
    fake_gic_line(SPI4_IRQ, geni_model_line);
    assert(!kshim_qup_init(qrd));

    /* Probe from the device: GT9916 on CS 0, mode 0, 1 MHz. */
    struct kshim_spi_device gt;
    assert(!kshim_spi_device_from_node(qrd, ts, &gt));
    struct kshim_spi_controller *c = gt.controller;
    assert(c && c->node == spi4 && c->num_chipselect == 4 && c->irq_driven && !strcmp(c->name, "spi@a90000"));
    assert(gt.chip_select == 0 && gt.mode == KSHIM_SPI_MODE_0 && gt.max_speed_hz == 1000000);
    struct kshim_spi_controller *again;
    assert(!kshim_spi_controller_get(qrd, spi4, &again) && again == c);
    /* 19.2 MHz CXO / 20 = 960 kHz; mode 0; CS 0; SPI 357 requested. */
    assert(geni_model_reg(GENI_SE_SER_M_CLK_CFG) == clk_cfg(20));
    assert(geni_model_reg(SPI_GENI_SE_CPHA) == 0 && geni_model_reg(SPI_GENI_SE_CPOL) == 0);
    assert(geni_model_reg(SPI_GENI_SE_DEMUX_SEL) == 0 && geni_model_reg(SPI_GENI_SE_WORD_LEN) == 4);
    assert(FakeGic[SPI4_IRQ].requested && geni_model_reg(GENI_SE_IRQ_EN) == GENI_SE_IRQ_GENI_M_EN);
    struct kshim_qup_se *se = kshim_qup_se_by_base(SPI4);
    assert(se->owner == KSHIM_QUP_OWNER_SPI && se->owner_node == spi4);

    /* A Berlin-style read: full-duplex header, then data, CS held between
     * the segments and for 50 us after the last one. */
    uint8_t header[4] = {0xf1, 0x10, 0x00, 0x00}, echo[4], data[8];
    memcpy(Geni.rx_data, "\xa0\xa1\xa2\xa3\x10\x11\x12\x13\x14\x15\x16\x17", 12);
    struct kshim_spi_xfer xfers[3] = {
        {header, echo, 4, 0}, {NULL, data, 8, 50}, {NULL, NULL, 3, 0},
    };
    unsigned waits = FakeWaits;
    assert(!kshim_spi_sync(&gt, xfers, 2, 0));
    assert(!memcmp(echo, "\xa0\xa1\xa2\xa3", 4) && !memcmp(data, "\x10\x11\x12\x13\x14\x15\x16\x17", 8));
    assert(Geni.cmd_count == 4);
    assert(Geni.cmds[0].opcode == SPI_GENI_M_CMD_OPCODE_CS_ASSERT);
    assert(Geni.cmds[1].opcode == 3 && (Geni.cmds[1].params & FRAG) && Geni.cmds[1].tx_len == 4 &&
           Geni.cmds[1].rx_len == 4);
    assert(Geni.cmds[2].opcode == SPI_GENI_M_CMD_OPCODE_RX_ONLY && Geni.cmds[2].rx_len == 8);
    assert(Geni.cmds[3].opcode == SPI_GENI_M_CMD_OPCODE_CS_DEASSERT);
    assert(Geni.cmds[3].at - Geni.cmds[2].at >= 50);
    assert(Geni.cs_asserts == 1 && Geni.cs_deasserts == 1 && !Geni.cs);
    assert(Geni.tx_log_len == 4 && !memcmp(Geni.tx_log, header, 4));
    assert(FakeWaits > waits && FakeGic[SPI4_IRQ].delivered);

    /* Neither buffer: zeros are clocked out by a TX-only command. */
    Geni.cmd_count = Geni.tx_log_len = 0;
    assert(!kshim_spi_sync(&gt, &xfers[2], 1, 0));
    assert(Geni.cmds[1].opcode == SPI_GENI_M_CMD_OPCODE_TX_ONLY && Geni.tx_log_len == 3);
    assert(!memcmp(Geni.tx_log, "\0\0\0", 3));
    assert(!kshim_spi_write_then_read(&gt, header, 2, data, 2, 0));
    assert(Geni.cmds[Geni.cmd_count - 3].opcode == SPI_GENI_M_CMD_OPCODE_TX_ONLY);
    assert(Geni.cmds[Geni.cmd_count - 2].opcode == SPI_GENI_M_CMD_OPCODE_RX_ONLY);

    /* Another device on CS 1, mode 3, 4 MHz: the engine is reprogrammed,
     * and back again for the GT9916. */
    struct kshim_spi_device other = {c, -1, 1, KSHIM_SPI_MODE_3, 8, 4000000};
    assert(!kshim_spi_sync(&other, xfers, 1, 0));
    assert(geni_model_reg(SPI_GENI_SE_CPHA) == 1 && geni_model_reg(SPI_GENI_SE_CPOL) == SPI_GENI_CPOL_BIT);
    assert(geni_model_reg(SPI_GENI_SE_DEMUX_SEL) == 1 && geni_model_reg(GENI_SE_SER_M_CLK_CFG) == clk_cfg(5));
    assert(geni_model_reg(GENI_SE_IRQ_EN) == GENI_SE_IRQ_GENI_M_EN && FakeGic[SPI4_IRQ].requested);
    assert(!kshim_spi_sync(&gt, xfers, 1, 0));
    assert(geni_model_reg(SPI_GENI_SE_CPHA) == 0 && geni_model_reg(SPI_GENI_SE_DEMUX_SEL) == 0);
    assert(geni_model_reg(GENI_SE_SER_M_CLK_CFG) == clk_cfg(20));

    /* Modes and sizes GENI does not do; rejected without bus activity. */
    unsigned commands = Geni.cmd_count;
    struct kshim_spi_device bad = gt;
    bad.mode |= KSHIM_SPI_CS_HIGH;
    assert(kshim_spi_sync(&bad, xfers, 1, 0) == ERROR_NOT_SUPPORTED);
    bad = gt;
    bad.bits_per_word = 16;
    assert(kshim_spi_sync(&bad, xfers, 1, 0) == ERROR_NOT_SUPPORTED);
    bad = gt;
    bad.chip_select = 4;
    assert(kshim_spi_sync(&bad, xfers, 1, 0) == ERROR_INVALID_PARAMETER);
    struct kshim_spi_xfer many[KSHIM_SPI_GENI_MAX_SEGMENTS + 1];
    for (unsigned i = 0; i <= KSHIM_SPI_GENI_MAX_SEGMENTS; ++i) many[i] = (struct kshim_spi_xfer){header, NULL, 1, 0};
    assert(kshim_spi_sync(&gt, many, KSHIM_SPI_GENI_MAX_SEGMENTS + 1, 0) == ERROR_INVALID_PARAMETER);
    struct kshim_spi_xfer empty = {header, NULL, 0, 0}, long_zero = {NULL, NULL, 257, 0};
    assert(kshim_spi_sync(&gt, &empty, 1, 0) == ERROR_INVALID_PARAMETER);
    assert(kshim_spi_sync(&gt, &long_zero, 1, 0) == ERROR_NOT_SUPPORTED);
    assert(Geni.cmd_count == commands);

    /* Handoff: the inherited SE configuration is back, interrupt freed,
     * clocks released, ownership dropped; later messages are refused. */
    assert(!kshim_spi_quiesce_all() && !kshim_spi_quiesce_all());
    assert(geni_model_reg(SPI_GENI_SE_CPHA) == 1 && geni_model_reg(SPI_GENI_SE_DEMUX_SEL) == 2);
    assert(geni_model_reg(GENI_SE_SER_M_CLK_CFG) == clk_cfg(7) && geni_model_reg(GENI_SE_IRQ_EN) == 0x55);
    assert(!FakeGic[SPI4_IRQ].requested && !fake_clk_enabled_by_us());
    assert(!se->acquired && se->owner == KSHIM_QUP_OWNER_NONE && !se->intid);
    assert(kshim_spi_sync(&gt, xfers, 1, 0) == KSHIM_IO_ERR_QUIESCED);
    assert(kshim_spi_controller_get(qrd, spi4, &again) == KSHIM_IO_ERR_QUIESCED);
    assert(!kshim_qup_quiesce_all());
    free(qrd);
    puts("qup_spi_test: ok");
    return 0;
}
