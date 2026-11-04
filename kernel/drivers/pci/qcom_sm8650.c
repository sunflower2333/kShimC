/* SPDX-License-Identifier: MIT */
/*
 * SM8650 PCIe1 target data for CrDK PcieLib (kShimC side; CrDK has no
 * Target/Sm8650 PCIe description). Layout follows CrDK Target/Sm8450
 * CrPcieTarget.c. This is hardware data only: register addresses, clock,
 * reset and GDSC identities, pins and the QMP PHY init values. The PCIe
 * host adapter (qcom_pcielib.c) resolves clocks, resets, GDSCs, supplies
 * and PERST through the runtime DT first (kshim_clk_get(), kshim_regulator_get(),
 * kshim_gpio_from_dt()); the names here are the DT clock-names/reset-names/
 * *-supply names, and the Id fields are the Linux/CrDK resource names used
 * only as a fallback (kshim_clk_get_by_name()).
 *
 * Provenance:
 *  - Addresses, interrupts, clocks, resets, power domains, iommu-map, pins
 *    and ranges: Linux arch/arm64/boot/dts/qcom/sm8650.dtsi pcie1 and
 *    pcie1_phy nodes, pcie1_default_state (BSD-3-Clause DT facts);
 *    sm8650-ayaneo-pocket-s2.dts for the PHY supplies (l3e, l3i, l1i);
 *    pinctrl-sm8650.c PINGROUP(98, pcie1_clk_req_n) gives mux 1 for CLKREQ#.
 *  - QMP v6.20 gen4x2 PHY: block offsets qmp_pcie_offsets_v6_20 (serdes
 *    0x1000, pcs 0x1200, pcs_misc 0x1400, tx 0x0, rx 0x200, tx2 0x800, rx2
 *    0xa00, ln_shrd 0xe00), pciephy_v6_regs_layout (SW_RESET 0x00,
 *    PCS_STATUS1 0x14, POWER_DOWN_CONTROL 0x40, START_CONTROL 0x44),
 *    PHYSTATUS_4_20 = bit 7, pwrdn_ctrl SW_PWRDN | REFCLK_DRV_DSBL; register
 *    values of sm8650_qmp_gen4x2_pciephy_cfg: the sm8550 gen4x2 serdes, tx,
 *    pcs, pcs_misc and ln_shrd tables and the sm8650 gen4x2 rx table, in the
 *    order qmp_pcie_init_registers() programs them. Offsets were resolved
 *    from the phy-qcom-qmp-qserdes-com-v6.h, -qserdes-txrx-v6_20.h,
 *    -qserdes-ln-shrd-v6.h, -pcs-v6_20.h and -pcs-pcie-v6_20.h register
 *    definitions (Linux drivers/phy/qualcomm), symbol named per row. No
 *    Linux code is used; only these register/value facts.
 *  - CrDK PcieTargetPhy has no lane-shared block: like CrDK Target/Sm8550
 *    (flattened rows anchored at the zero-offset block), the ln_shrd rows
 *    are TX-block, lane-0-only rows at absolute PHY offset 0xe00 + register.
 */
#include "qcom.h"

#include <oskal/common.h>

#define SM8650_PCIE1_PARF   0x01c08000ULL
#define SM8650_PCIE1_DBI    0x40000000ULL
#define SM8650_PCIE1_ELBI   0x40000f20ULL
#define SM8650_PCIE1_ATU    0x40001000ULL
#define SM8650_PCIE1_CONFIG 0x40100000ULL
#define SM8650_PCIE1_PHY    0x01c0e000ULL

/* qmp_pcie_offsets_v6_20 */
#define V6_20_SERDES   0x1000
#define V6_20_PCS      0x1200
#define V6_20_PCS_MISC 0x1400
#define V6_20_TX       0x0000
#define V6_20_RX       0x0200
#define V6_20_TX2      0x0800
#define V6_20_RX2      0x0a00

#define QMP_SW_RESET          0x000
#define QMP_PCS_STATUS1       0x014
#define QMP_POWER_DOWN        0x040
#define QMP_START             0x044
#define QMP_SW_PWRDN          BIT(0)
#define QMP_REFCLK_DRV_DSBL   BIT(1)
#define QMP_PHYSTATUS_4_20    BIT(7)

#define PHY(block, offset, value, lanes) \
  { PCIE_PHY_PHASE_BASE, PCIE_PHY_BLOCK_##block, (offset), (value), (lanes) }

STATIC CONST PciePhyInitEntry mPhyInit[] = {
    /* sm8550_qmp_gen4x2_pcie_serdes_tbl */
    PHY(SERDES, 0x000, 0x26, 1), /* QSERDES_V6_COM_SSC_STEP_SIZE1_MODE1 */
    PHY(SERDES, 0x004, 0x03, 1), /* QSERDES_V6_COM_SSC_STEP_SIZE2_MODE1 */
    PHY(SERDES, 0x010, 0x06, 1), /* QSERDES_V6_COM_CP_CTRL_MODE1 */
    PHY(SERDES, 0x014, 0x16, 1), /* QSERDES_V6_COM_PLL_RCTRL_MODE1 */
    PHY(SERDES, 0x018, 0x36, 1), /* QSERDES_V6_COM_PLL_CCTRL_MODE1 */
    PHY(SERDES, 0x01c, 0x04, 1), /* QSERDES_V6_COM_CORECLK_DIV_MODE1 */
    PHY(SERDES, 0x020, 0x0a, 1), /* QSERDES_V6_COM_LOCK_CMP1_MODE1 */
    PHY(SERDES, 0x024, 0x1a, 1), /* QSERDES_V6_COM_LOCK_CMP2_MODE1 */
    PHY(SERDES, 0x028, 0x68, 1), /* QSERDES_V6_COM_DEC_START_MODE1 */
    PHY(SERDES, 0x030, 0xab, 1), /* QSERDES_V6_COM_DIV_FRAC_START1_MODE1 */
    PHY(SERDES, 0x034, 0xaa, 1), /* QSERDES_V6_COM_DIV_FRAC_START2_MODE1 */
    PHY(SERDES, 0x038, 0x02, 1), /* QSERDES_V6_COM_DIV_FRAC_START3_MODE1 */
    PHY(SERDES, 0x03c, 0x12, 1), /* QSERDES_V6_COM_HSCLK_SEL_1 */
    PHY(SERDES, 0x060, 0xf8, 1), /* QSERDES_V6_COM_SSC_STEP_SIZE1_MODE0 */
    PHY(SERDES, 0x064, 0x01, 1), /* QSERDES_V6_COM_SSC_STEP_SIZE2_MODE0 */
    PHY(SERDES, 0x070, 0x06, 1), /* QSERDES_V6_COM_CP_CTRL_MODE0 */
    PHY(SERDES, 0x074, 0x16, 1), /* QSERDES_V6_COM_PLL_RCTRL_MODE0 */
    PHY(SERDES, 0x078, 0x36, 1), /* QSERDES_V6_COM_PLL_CCTRL_MODE0 */
    PHY(SERDES, 0x07c, 0x0a, 1), /* QSERDES_V6_COM_PLL_CORE_CLK_DIV_MODE0 */
    PHY(SERDES, 0x080, 0x04, 1), /* QSERDES_V6_COM_LOCK_CMP1_MODE0 */
    PHY(SERDES, 0x084, 0x0d, 1), /* QSERDES_V6_COM_LOCK_CMP2_MODE0 */
    PHY(SERDES, 0x088, 0x41, 1), /* QSERDES_V6_COM_DEC_START_MODE0 */
    PHY(SERDES, 0x090, 0xab, 1), /* QSERDES_V6_COM_DIV_FRAC_START1_MODE0 */
    PHY(SERDES, 0x094, 0xaa, 1), /* QSERDES_V6_COM_DIV_FRAC_START2_MODE0 */
    PHY(SERDES, 0x098, 0x01, 1), /* QSERDES_V6_COM_DIV_FRAC_START3_MODE0 */
    PHY(SERDES, 0x09c, 0x00, 1), /* QSERDES_V6_COM_HSCLK_HS_SWITCH_SEL_1 */
    PHY(SERDES, 0x0bc, 0x0a, 1), /* QSERDES_V6_COM_BG_TIMER */
    PHY(SERDES, 0x0c0, 0x01, 1), /* QSERDES_V6_COM_SSC_EN_CENTER */
    PHY(SERDES, 0x0cc, 0x62, 1), /* QSERDES_V6_COM_SSC_PER1 */
    PHY(SERDES, 0x0d0, 0x02, 1), /* QSERDES_V6_COM_SSC_PER2 */
    PHY(SERDES, 0x0d8, 0x40, 1), /* QSERDES_V6_COM_PLL_POST_DIV_MUX */
    PHY(SERDES, 0x0dc, 0x14, 1), /* QSERDES_V6_COM_PLL_BIAS_EN_CLK_BUFLR_EN */
    PHY(SERDES, 0x0e0, 0x90, 1), /* QSERDES_V6_COM_CLK_ENABLE1 */
    PHY(SERDES, 0x0e4, 0x82, 1), /* QSERDES_V6_COM_SYS_CLK_CTRL */
    PHY(SERDES, 0x0f4, 0x0f, 1), /* QSERDES_V6_COM_PLL_IVCO */
    PHY(SERDES, 0x110, 0x08, 1), /* QSERDES_V6_COM_SYSCLK_EN_SEL */
    PHY(SERDES, 0x120, 0x46, 1), /* QSERDES_V6_COM_LOCK_CMP_EN */
    PHY(SERDES, 0x124, 0x04, 1), /* QSERDES_V6_COM_LOCK_CMP_CFG */
    PHY(SERDES, 0x140, 0x14, 1), /* QSERDES_V6_COM_VCO_TUNE_MAP */
    PHY(SERDES, 0x164, 0x34, 1), /* QSERDES_V6_COM_CLK_SELECT */
    PHY(SERDES, 0x170, 0xa0, 1), /* QSERDES_V6_COM_CORE_CLK_EN */
    PHY(SERDES, 0x174, 0x06, 1), /* QSERDES_V6_COM_CMN_CONFIG_1 */
    PHY(SERDES, 0x184, 0x88, 1), /* QSERDES_V6_COM_CMN_MISC_1 */
    PHY(SERDES, 0x188, 0x14, 1), /* QSERDES_V6_COM_CMN_MODE */
    PHY(SERDES, 0x198, 0x0f, 1), /* QSERDES_V6_COM_PLL_VCO_DC_LEVEL_CTRL */
    /* sm8550_qmp_gen4x2_pcie_tx_tbl */
    PHY(TX, 0x030, 0x1d, 3), /* QSERDES_V6_20_TX_RES_CODE_LANE_OFFSET_TX */
    PHY(TX, 0x034, 0x03, 3), /* QSERDES_V6_20_TX_RES_CODE_LANE_OFFSET_RX */
    PHY(TX, 0x078, 0x01, 3), /* QSERDES_V6_20_TX_LANE_MODE_1 */
    PHY(TX, 0x07c, 0x00, 3), /* QSERDES_V6_20_TX_LANE_MODE_2 */
    PHY(TX, 0x080, 0x51, 3), /* QSERDES_V6_20_TX_LANE_MODE_3 */
    PHY(TX, 0x0ac, 0x34, 3), /* QSERDES_V6_20_TX_TRAN_DRVR_EMP_EN */
    /* sm8650_qmp_gen4x2_pcie_rx_tbl */
    PHY(RX, 0x008, 0x0a, 3), /* QSERDES_V6_20_RX_UCDR_FO_GAIN_RATE_2 */
    PHY(RX, 0x00c, 0x0a, 3), /* QSERDES_V6_20_RX_UCDR_FO_GAIN_RATE_3 */
    PHY(RX, 0x020, 0x16, 3), /* QSERDES_V6_20_RX_UCDR_PI_CONTROLS */
    PHY(RX, 0x034, 0x00, 3), /* QSERDES_V6_20_RX_UCDR_SO_ACC_DEFAULT_VAL_RATE3 */
    PHY(RX, 0x09c, 0x82, 3), /* QSERDES_V6_20_RX_IVCM_CAL_CTRL2 */
    PHY(RX, 0x0b4, 0x05, 3), /* QSERDES_V6_20_RX_DFE_3 */
    PHY(RX, 0x0e8, 0x0a, 3), /* QSERDES_V6_20_RX_VGA_CAL_MAN_VAL */
    PHY(RX, 0x10c, 0x0d, 3), /* QSERDES_V6_20_RX_GM_CAL */
    PHY(RX, 0x120, 0x0b, 3), /* QSERDES_V6_20_RX_EQU_ADAPTOR_CNTRL4 */
    PHY(RX, 0x148, 0x1c, 3), /* QSERDES_V6_20_RX_SIGDET_ENABLES */
    PHY(RX, 0x188, 0x20, 3), /* QSERDES_V6_20_RX_PHPRE_CTRL */
    PHY(RX, 0x194, 0x38, 3), /* QSERDES_V6_20_RX_DFE_CTLE_POST_CAL_OFFSET */
    PHY(RX, 0x1f4, 0xd3, 3), /* QSERDES_V6_20_RX_MODE_RATE2_B0 */
    PHY(RX, 0x1f8, 0xd3, 3), /* QSERDES_V6_20_RX_MODE_RATE2_B1 */
    PHY(RX, 0x1fc, 0x00, 3), /* QSERDES_V6_20_RX_MODE_RATE2_B2 */
    PHY(RX, 0x200, 0x9a, 3), /* QSERDES_V6_20_RX_MODE_RATE2_B3 */
    PHY(RX, 0x204, 0x06, 3), /* QSERDES_V6_20_RX_MODE_RATE2_B4 */
    PHY(RX, 0x208, 0xb6, 3), /* QSERDES_V6_20_RX_MODE_RATE2_B5 */
    PHY(RX, 0x20c, 0xee, 3), /* QSERDES_V6_20_RX_MODE_RATE2_B6 */
    PHY(RX, 0x210, 0x23, 3), /* QSERDES_V6_20_RX_MODE_RATE3_B0 */
    PHY(RX, 0x214, 0x9b, 3), /* QSERDES_V6_20_RX_MODE_RATE3_B1 */
    PHY(RX, 0x218, 0x60, 3), /* QSERDES_V6_20_RX_MODE_RATE3_B2 */
    PHY(RX, 0x21c, 0xdf, 3), /* QSERDES_V6_20_RX_MODE_RATE3_B3 */
    PHY(RX, 0x220, 0x43, 3), /* QSERDES_V6_20_RX_MODE_RATE3_B4 */
    PHY(RX, 0x224, 0x76, 3), /* QSERDES_V6_20_RX_MODE_RATE3_B5 */
    PHY(RX, 0x228, 0xff, 3), /* QSERDES_V6_20_RX_MODE_RATE3_B6 */
    /* sm8550_qmp_gen4x2_pcie_pcs_tbl */
    PHY(PCS, 0x170, 0x17, 1), /* QPHY_V6_20_PCS_G12S1_TXDEEMPH_M6DB */
    PHY(PCS, 0x178, 0x2e, 1), /* QPHY_V6_20_PCS_G3S2_PRE_GAIN */
    PHY(PCS, 0x190, 0xcc, 1), /* QPHY_V6_20_PCS_RX_SIGDET_LVL */
    PHY(PCS, 0x1f8, 0x00, 1), /* QPHY_V6_20_PCS_EQ_CONFIG4 */
    PHY(PCS, 0x1fc, 0x22, 1), /* QPHY_V6_20_PCS_EQ_CONFIG5 */
    PHY(PCS, 0x1dc, 0x04, 1), /* QPHY_V6_20_PCS_TX_RX_CONFIG1 */
    PHY(PCS, 0x1e0, 0x02, 1), /* QPHY_V6_20_PCS_TX_RX_CONFIG2 */
    /* sm8550_qmp_gen4x2_pcie_pcs_misc_tbl */
    PHY(PCS_MISC, 0x01c, 0xc1, 1), /* QPHY_PCIE_V6_20_PCS_ENDPOINT_REFCLK_DRIVE */
    PHY(PCS_MISC, 0x090, 0x00, 1), /* QPHY_PCIE_V6_20_PCS_OSC_DTCT_ATCIONS */
    PHY(PCS_MISC, 0x0a0, 0x16, 1), /* QPHY_PCIE_V6_20_PCS_EQ_CONFIG1 */
    PHY(PCS_MISC, 0x0f0, 0x27, 1), /* QPHY_PCIE_V6_20_PCS_G3_RXEQEVAL_TIME */
    PHY(PCS_MISC, 0x0f4, 0x27, 1), /* QPHY_PCIE_V6_20_PCS_G4_RXEQEVAL_TIME */
    PHY(PCS_MISC, 0x108, 0x02, 1), /* QPHY_PCIE_V6_20_PCS_EQ_CONFIG5 */
    PHY(PCS_MISC, 0x15c, 0x2e, 1), /* QPHY_PCIE_V6_20_PCS_G4_PRE_GAIN */
    PHY(PCS_MISC, 0x17c, 0x03, 1), /* QPHY_PCIE_V6_20_PCS_RX_MARGINING_CONFIG1 */
    PHY(PCS_MISC, 0x184, 0x28, 1), /* QPHY_PCIE_V6_20_PCS_RX_MARGINING_CONFIG3 */
    PHY(PCS_MISC, 0x018, 0xc0, 1), /* QPHY_PCIE_V6_20_PCS_TX_RX_CONFIG */
    PHY(PCS_MISC, 0x00c, 0x1d, 1), /* QPHY_PCIE_V6_20_PCS_POWER_STATE_CONFIG2 */
    PHY(PCS_MISC, 0x18c, 0x0f, 1), /* QPHY_PCIE_V6_20_PCS_RX_MARGINING_CONFIG5 */
    PHY(PCS_MISC, 0x1ac, 0xf2, 1), /* QPHY_PCIE_V6_20_PCS_G3_FOM_EQ_CONFIG5 */
    PHY(PCS_MISC, 0x1c0, 0xf2, 1), /* QPHY_PCIE_V6_20_PCS_G4_FOM_EQ_CONFIG5 */
    /* sm8550_qmp_gen4x2_pcie_ln_shrd_tbl */
    PHY(TX, 0xea0, 0x01, 1), /* QSERDES_V6_LN_SHRD_RXCLK_DIV2_CTRL */
    PHY(TX, 0xeb4, 0x00, 1), /* QSERDES_V6_LN_SHRD_DFE_DAC_ENABLE1 */
    PHY(TX, 0xec4, 0x02, 1), /* QSERDES_V6_LN_SHRD_TX_ADAPT_POST_THRESH1 */
    PHY(TX, 0xec8, 0x0d, 1), /* QSERDES_V6_LN_SHRD_TX_ADAPT_POST_THRESH2 */
    PHY(TX, 0xed4, 0x12, 1), /* QSERDES_V6_LN_SHRD_RX_MODE_RATE_0_1_B0 */
    PHY(TX, 0xed8, 0x12, 1), /* QSERDES_V6_LN_SHRD_RX_MODE_RATE_0_1_B1 */
    PHY(TX, 0xedc, 0xdb, 1), /* QSERDES_V6_LN_SHRD_RX_MODE_RATE_0_1_B2 */
    PHY(TX, 0xee0, 0x9a, 1), /* QSERDES_V6_LN_SHRD_RX_MODE_RATE_0_1_B3 */
    PHY(TX, 0xee4, 0x38, 1), /* QSERDES_V6_LN_SHRD_RX_MODE_RATE_0_1_B4 */
    PHY(TX, 0xee8, 0xb6, 1), /* QSERDES_V6_LN_SHRD_RX_MODE_RATE_0_1_B5 */
    PHY(TX, 0xeec, 0x64, 1), /* QSERDES_V6_LN_SHRD_RX_MODE_RATE_0_1_B6 */
    PHY(TX, 0xef0, 0x1f, 1), /* QSERDES_V6_LN_SHRD_RX_MARG_COARSE_THRESH1_RATE210 */
    PHY(TX, 0xef4, 0x1f, 1), /* QSERDES_V6_LN_SHRD_RX_MARG_COARSE_THRESH1_RATE3 */
    PHY(TX, 0xef8, 0x1f, 1), /* QSERDES_V6_LN_SHRD_RX_MARG_COARSE_THRESH2_RATE210 */
    PHY(TX, 0xefc, 0x1f, 1), /* QSERDES_V6_LN_SHRD_RX_MARG_COARSE_THRESH2_RATE3 */
    PHY(TX, 0xf00, 0x1f, 1), /* QSERDES_V6_LN_SHRD_RX_MARG_COARSE_THRESH3_RATE210 */
    PHY(TX, 0xf04, 0x1f, 1), /* QSERDES_V6_LN_SHRD_RX_MARG_COARSE_THRESH3_RATE3 */
    PHY(TX, 0xf0c, 0x1f, 1), /* QSERDES_V6_LN_SHRD_RX_MARG_COARSE_THRESH4_RATE3 */
    PHY(TX, 0xf14, 0x1f, 1), /* QSERDES_V6_LN_SHRD_RX_MARG_COARSE_THRESH5_RATE3 */
    PHY(TX, 0xf1c, 0x1f, 1), /* QSERDES_V6_LN_SHRD_RX_MARG_COARSE_THRESH6_RATE3 */
    PHY(TX, 0xf28, 0x5b, 1), /* QSERDES_V6_LN_SHRD_RX_SUMMER_CAL_SPD_MODE */
};

STATIC CONST PcieTargetRegion mRegions[] = {
    { "parf", SM8650_PCIE1_PARF, 0x3000 },
    { "dbi", SM8650_PCIE1_DBI, 0xf1d },
    { "elbi", SM8650_PCIE1_ELBI, 0xa8 },
    { "atu", SM8650_PCIE1_ATU, 0x1000 },
    { "config", SM8650_PCIE1_CONFIG, 0x100000 },
};

/* ranges: I/O PCI 0 at 0x40200000 (1 MiB), MEM 0x40300000 (509 MiB). */
STATIC CONST PcieTargetRange mRanges[] = {
    { PCIE_RANGE_IO, 0x00000000ULL, 0x40200000ULL, 0x00100000ULL },
    { PCIE_RANGE_MEM32, 0x40300000ULL, 0x40300000ULL, 0x1fd00000ULL },
};

STATIC CONST PcieTargetInterrupt mInterrupts[] = {
    { "msi0", GIC_SPI(307), CR_INTERRUPT_TRIGGER_LEVEL_HIGH },
    { "msi1", GIC_SPI(308), CR_INTERRUPT_TRIGGER_LEVEL_HIGH },
    { "msi2", GIC_SPI(309), CR_INTERRUPT_TRIGGER_LEVEL_HIGH },
    { "msi3", GIC_SPI(312), CR_INTERRUPT_TRIGGER_LEVEL_HIGH },
    { "msi4", GIC_SPI(313), CR_INTERRUPT_TRIGGER_LEVEL_HIGH },
    { "msi5", GIC_SPI(314), CR_INTERRUPT_TRIGGER_LEVEL_HIGH },
    { "msi6", GIC_SPI(374), CR_INTERRUPT_TRIGGER_LEVEL_HIGH },
    { "msi7", GIC_SPI(375), CR_INTERRUPT_TRIGGER_LEVEL_HIGH },
    { "global", GIC_SPI(306), CR_INTERRUPT_TRIGGER_LEVEL_HIGH },
    { "int_a", GIC_SPI(434), CR_INTERRUPT_TRIGGER_LEVEL_HIGH },
    { "int_b", GIC_SPI(435), CR_INTERRUPT_TRIGGER_LEVEL_HIGH },
    { "int_c", GIC_SPI(438), CR_INTERRUPT_TRIGGER_LEVEL_HIGH },
    { "int_d", GIC_SPI(439), CR_INTERRUPT_TRIGGER_LEVEL_HIGH },
};

enum {
  SM8650_PCIE1_CLOCK_OFFSET = 0,
  SM8650_PCIE1_CLOCK_COUNT = 8,
  SM8650_PCIE1_PHY_CLOCK_OFFSET = SM8650_PCIE1_CLOCK_OFFSET + SM8650_PCIE1_CLOCK_COUNT,
  SM8650_PCIE1_PHY_CLOCK_COUNT = 5,
  SM8650_PCIE_CLOCK_TOTAL = SM8650_PCIE1_PHY_CLOCK_OFFSET + SM8650_PCIE1_PHY_CLOCK_COUNT,

  SM8650_PCIE1_RESET_OFFSET = 0,
  SM8650_PCIE1_RESET_COUNT = 1,
  SM8650_PCIE1_PHY_RESET_OFFSET = 1,
  SM8650_PCIE1_PHY_RESET_COUNT = 1,
  SM8650_PCIE1_PHY_NOCSR_RESET_OFFSET = 2,
  SM8650_PCIE1_PHY_NOCSR_RESET_COUNT = 1,
  SM8650_PCIE_RESET_TOTAL = 3,

  SM8650_PCIE1_SUPPLY_COUNT = 3,
  SM8650_PCIE1_GPIO_COUNT = 3,
  SM8650_PCIE1_IOMMU_COUNT = 2,
  SM8650_PCIE1_INTERRUPT_COUNT = 9,
  SM8650_PCIE1_INTX_COUNT = 4,
};

/* Controller clocks in sm8650.dtsi clock-names order, then the PHY's. */
STATIC CONST PcieTargetClock mClocks[] = {
    { "aux", "gcc", "gcc_pcie_1_aux_clk", 19200000, PCIE_CLOCK_PROVIDER_GCC },
    { "cfg", "gcc", "gcc_pcie_1_cfg_ahb_clk", 0, PCIE_CLOCK_PROVIDER_GCC },
    { "bus_master", "gcc", "gcc_pcie_1_mstr_axi_clk", 0, PCIE_CLOCK_PROVIDER_GCC },
    { "bus_slave", "gcc", "gcc_pcie_1_slv_axi_clk", 0, PCIE_CLOCK_PROVIDER_GCC },
    { "slave_q2a", "gcc", "gcc_pcie_1_slv_q2a_axi_clk", 0, PCIE_CLOCK_PROVIDER_GCC },
    { "ddrss_sf_tbu", "gcc", "gcc_ddrss_pcie_sf_qtb_clk", 0, PCIE_CLOCK_PROVIDER_GCC },
    { "noc_aggr", "gcc", "gcc_aggre_noc_pcie_axi_clk", 0, PCIE_CLOCK_PROVIDER_GCC },
    { "cnoc_sf_axi", "gcc", "gcc_cnoc_pcie_sf_axi_clk", 0, PCIE_CLOCK_PROVIDER_GCC },

    { "aux", "gcc", "gcc_pcie_1_phy_aux_clk", 0, PCIE_CLOCK_PROVIDER_GCC },
    { "cfg_ahb", "gcc", "gcc_pcie_1_cfg_ahb_clk", 0, PCIE_CLOCK_PROVIDER_GCC },
    { "ref", "tcsr", "tcsr_pcie_1_clkref_en", 0, PCIE_CLOCK_PROVIDER_TCSR },
    { "rchng", "gcc", "gcc_pcie_1_phy_rchng_clk", 100000000, PCIE_CLOCK_PROVIDER_GCC },
    { "pipe", "gcc", "gcc_pcie_1_pipe_clk", 0, PCIE_CLOCK_PROVIDER_GCC },
};

STATIC CONST PcieTargetReset mResets[] = {
    { "pci", "gcc", "gcc_pcie_1_bcr", 0, PCIE_CLOCK_PROVIDER_GCC },
    { "phy", "gcc", "gcc_pcie_1_phy_bcr", 0, PCIE_CLOCK_PROVIDER_GCC },
    { "phy_nocsr", "gcc", "gcc_pcie_1_nocsr_com_phy_bcr", 0, PCIE_CLOCK_PROVIDER_GCC },
};

/* pcie1_phy *-supply names; Ids are the Pocket S2 rails (vreg_l3e_0p9,
 * vreg_l3i_1p2, vreg_l1i_0p88). Zero voltage keeps the DT constraints. */
STATIC CONST PcieTargetSupply mSupplies[] = {
    { "vdda-phy", "rpmh", "ldoe3", 0, 0, 0 },
    { "vdda-pll", "rpmh", "ldoi3", 0, 0, 0 },
    { "vdda-qref", "rpmh", "ldoi1", 0, 0, 0 },
};

/* pcie1_default_state: PERST# gpio97 pull-down, CLKREQ# gpio98 function
 * pcie1_clk_req_n pull-up, WAKE# gpio99 pull-up, all 2 mA. */
STATIC CONST PcieTargetGpio mGpios[] = {
    { "perst", "tlmm", 97, "GPIO_ACTIVE_LOW", 0, 2, PCIE_GPIO_PULL_DOWN },
    { "clkreq", "tlmm", 98, "GPIO_ACTIVE_LOW", 1, 2, PCIE_GPIO_PULL_UP },
    { "wake", "tlmm", 99, "GPIO_ACTIVE_HIGH", 0, 2, PCIE_GPIO_PULL_UP },
};

/* iommu-map = <0 &apps_smmu 0x1480 0 1>, <0x100 &apps_smmu 0x1481 0 1>. */
STATIC CONST PcieTargetIommuMap mIommuMaps[] = {
    { 0x0000, "apps_smmu", 0x1480, 1 },
    { 0x0100, "apps_smmu", 0x1481, 1 },
};

STATIC CONST PcieTargetPhy mPhys[] = {
    {
        .Label = "pcie1_phy",
        .Compatible = "qcom,sm8650-qmp-gen4x2-pcie-phy",
        .Base = SM8650_PCIE1_PHY,
        .Size = 0x2000,
        .Lanes = 2,
        .BlockOffsets = { V6_20_SERDES, V6_20_TX, V6_20_RX, V6_20_TX2, V6_20_RX2,
                          V6_20_PCS, V6_20_PCS_MISC },
        .SwReset = QMP_SW_RESET,
        .StartControl = QMP_START,
        .Status = QMP_PCS_STATUS1,
        .PowerDownControl = QMP_POWER_DOWN,
        .PowerDownControlValue = QMP_SW_PWRDN | QMP_REFCLK_DRV_DSBL,
        .StatusMask = QMP_PHYSTATUS_4_20,
        .InitOffset = 0,
        .InitCount = ARRAY_SIZE(mPhyInit),
        .ClockOffset = SM8650_PCIE1_PHY_CLOCK_OFFSET,
        .ClockCount = SM8650_PCIE1_PHY_CLOCK_COUNT,
        .ResetOffset = SM8650_PCIE1_PHY_RESET_OFFSET,
        .ResetCount = SM8650_PCIE1_PHY_RESET_COUNT,
        /* No-CSR reset: held through the tables, released after PIPE. */
        .PipeResetOffset = SM8650_PCIE1_PHY_NOCSR_RESET_OFFSET,
        .PipeResetCount = SM8650_PCIE1_PHY_NOCSR_RESET_COUNT,
        .PowerDomainController = "gcc",
        .PowerDomainId = "pcie_1_phy_gdsc",
    },
};

STATIC CONST PcieTargetController mControllers[] = {
    {
        .Label = "pcie1",
        .Compatible = "qcom,pcie-sm8650",
        .LinuxConfig = "cfg_1_9_0",
        .LinuxOps = "ops_1_9_0",
        .Enabled = TRUE,
        .DmaAbove4G = TRUE,
        .Domain = 1,
        .BusStart = 0,
        .BusEnd = 0xff,
        .Lanes = 2,
        .MaxLinkSpeed = 0,           /* keep the controller's Gen4 capability */
        .RegionOffset = 0,
        .RegionCount = ARRAY_SIZE(mRegions),
        .RangeOffset = 0,
        .RangeCount = ARRAY_SIZE(mRanges),
        .InterruptOffset = 0,
        .InterruptCount = SM8650_PCIE1_INTERRUPT_COUNT,
        .IntxOffset = SM8650_PCIE1_INTERRUPT_COUNT,
        .IntxCount = SM8650_PCIE1_INTX_COUNT,
        .ClockOffset = SM8650_PCIE1_CLOCK_OFFSET,
        .ClockCount = SM8650_PCIE1_CLOCK_COUNT,
        .ResetOffset = SM8650_PCIE1_RESET_OFFSET,
        .ResetCount = SM8650_PCIE1_RESET_COUNT,
        .SupplyOffset = 0,
        .SupplyCount = SM8650_PCIE1_SUPPLY_COUNT,
        .PowerDomainController = "gcc",
        .PowerDomainId = "pcie_1_gdsc",
        .GpioOffset = 0,
        .GpioCount = SM8650_PCIE1_GPIO_COUNT,
        .IommuMapOffset = 0,
        .IommuMapCount = SM8650_PCIE1_IOMMU_COUNT,
        .PhyIndex = 0,
        /* dt-bindings/interconnect/qcom,sm8650-rpmh.h: pcie_noc MASTER_PCIE_1
         * (2) -> mc_virt SLAVE_EBI1 (1); gem_noc MASTER_APPSS_PROC (3) ->
         * cnoc_main SLAVE_PCIE_1 (12). Not voted by kShimC (inherited). */
        .Interconnect = { "qcom,sm8650-pcie", 2, 1, 3, 12, 0, 0, 0, 0 },
        .ParfSysControlSetMask = 0,  /* Linux pcie-qcom: PARF_SYS_CTRL kept */
    },
};

CR_STATIC_ASSERT(ARRAY_SIZE(mClocks) == SM8650_PCIE_CLOCK_TOTAL, "PCIe clock slices");
CR_STATIC_ASSERT(ARRAY_SIZE(mResets) == SM8650_PCIE_RESET_TOTAL, "PCIe reset slices");
CR_STATIC_ASSERT(ARRAY_SIZE(mInterrupts) ==
                 SM8650_PCIE1_INTERRUPT_COUNT + SM8650_PCIE1_INTX_COUNT, "PCIe interrupts");
CR_STATIC_ASSERT(ARRAY_SIZE(mSupplies) == SM8650_PCIE1_SUPPLY_COUNT, "PCIe supplies");
CR_STATIC_ASSERT(ARRAY_SIZE(mGpios) == SM8650_PCIE1_GPIO_COUNT, "PCIe GPIOs");
CR_STATIC_ASSERT(ARRAY_SIZE(mIommuMaps) == SM8650_PCIE1_IOMMU_COUNT, "PCIe IOMMU map");
CR_STATIC_ASSERT(ARRAY_SIZE(mPhyInit) == 119, "sm8650 gen4x2: 45+6+26+7+14+21 rows");

STATIC CONST PcieTargetContext mSm8650Target = {
    .Controllers = mControllers,
    .ControllerCount = ARRAY_SIZE(mControllers),
    .Regions = mRegions,
    .RegionCount = ARRAY_SIZE(mRegions),
    .Ranges = mRanges,
    .RangeCount = ARRAY_SIZE(mRanges),
    .Interrupts = mInterrupts,
    .InterruptCount = ARRAY_SIZE(mInterrupts),
    .Clocks = mClocks,
    .ClockCount = ARRAY_SIZE(mClocks),
    .Resets = mResets,
    .ResetCount = ARRAY_SIZE(mResets),
    .Supplies = mSupplies,
    .SupplyCount = ARRAY_SIZE(mSupplies),
    .Gpios = mGpios,
    .GpioCount = ARRAY_SIZE(mGpios),
    .IommuMaps = mIommuMaps,
    .IommuMapCount = ARRAY_SIZE(mIommuMaps),
    .Phys = mPhys,
    .PhyCount = ARRAY_SIZE(mPhys),
    .PhyInit = mPhyInit,
    .PhyInitCount = ARRAY_SIZE(mPhyInit),
};

const PcieTargetContext *kshim_pci_qcom_sm8650_target(void)
{
    return &mSm8650Target;
}
