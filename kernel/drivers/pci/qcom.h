/* SPDX-License-Identifier: MIT */
#pragma once
/*
 * Qualcomm DesignWare PCIe root complex (CONFIG_KSHIM_PCI_QCOM): shared
 * state of the host (qcom.c) and its CrDK PcieLib adapter (qcom_pcielib.c).
 * See docs/pci.md.
 */
#include "pci_internal.h"

#include <Library/pcie.h>
#include <dt.h>
#include <kshim/gpio.h>

#define QCOM_PCIE_MAX       2U
#define QCOM_ATU_SAVED_REGS 7U
#define QCOM_ATU_OWNED_MAX  4U

/* DesignWare register facts (Synopsys DWC PCIe 4.x/5.x as used by CrDK
 * PcieLib and Linux pcie-designware): PCIe capability link status, port
 * debug LTSSM state, unrolled outbound iATU regions. */
#define QCOM_PARF_LTSSM            0x1b0
#define QCOM_PARF_LTSSM_ENABLE     (1U << 8)
#define QCOM_DBI_PORT_DEBUG0       0x728
#define QCOM_LTSSM_L0              0x11U
#define QCOM_EXP_LINK_STATUS       0x12
#define QCOM_EXP_LINK_ACTIVE       (1U << 13)
#define QCOM_ATU_STRIDE            0x200U
#define QCOM_ATU_CTRL1             0x00
#define QCOM_ATU_CTRL2             0x04
#define QCOM_ATU_LOWER_BASE        0x08
#define QCOM_ATU_UPPER_BASE        0x0c
#define QCOM_ATU_LIMIT             0x10
#define QCOM_ATU_LOWER_TARGET      0x14
#define QCOM_ATU_UPPER_TARGET      0x18
#define QCOM_ATU_ENABLE            (1U << 31)
#define QCOM_ATU_TYPE_MASK         0x1fU
#define QCOM_ATU_TYPE_MEM          0x0U
#define QCOM_ATU_TYPE_IO           0x2U
#define QCOM_ATU_TYPE_CFG0         0x4U
#define QCOM_ATU_TYPE_CFG1         0x5U

struct qcom_atu_owned {
    unsigned region;
    uint32_t saved[QCOM_ATU_SAVED_REGS];
};

struct qcom_pcie {
    struct kshim_pci_host host;
    const void *fdt;
    int node, phy_node, tlmm_node;
    struct dt_range parf, dbi, elbi, atu, config, mhi, phy;
    bool has_elbi, has_mhi, has_phy;
    /* Inherited link: kShimC owns only iATU regions (config + any missing
     * memory window), restored newest first at quiesce. */
    bool live;
    unsigned atu_regions;
    struct qcom_atu_owned owned[QCOM_ATU_OWNED_MAX];
    unsigned owned_count;
    int cfg_region;                 /* index into owned[], -1 = none */
    uint32_t cfg_target, cfg_type;  /* last programmed BDF target and type */
    uint8_t exp_cap;                /* root port PCIe capability in DBI */
    /* Cold start through CrDK PcieLib. */
    const PcieTargetContext *target;
    int port;                       /* controller index in target, -1 = none */
    bool cold;                      /* PcieInitializePort succeeded */
    /* PERST# (DT perst-gpios/perst-gpio/reset-gpios, else target pin). */
    struct kshim_gpio perst;
    bool has_perst, perst_requested, pinctrl_applied;
};

/* qcom_pcielib.c */
/* Pick the SoC target data and controller for a host (by compatible and
 * PARF base); 0 when a cold start is possible. Checks the DT regions
 * against the target's. */
int kshim_pci_qcom_lib_select(struct qcom_pcie *qp);
/* Cold start: PcieLibInit() once, then PcieInitializePort(). 0 = link up,
 * ERROR_NOT_READY = port ready but no link, else the failure. */
int kshim_pci_qcom_lib_start(struct qcom_pcie *qp);
int kshim_pci_qcom_lib_config(struct qcom_pcie *qp, uint8_t bus, uint8_t devfn, uint16_t reg,
                              unsigned width, bool write, uint32_t *value);
int kshim_pci_qcom_lib_stop(struct qcom_pcie *qp);
/* Drive PERST# (logical: asserted = endpoint held in reset). Requests the
 * pin on first use; the request is dropped by kshim_pci_qcom_perst_release(). */
int kshim_pci_qcom_perst(struct qcom_pcie *qp, bool asserted);
void kshim_pci_qcom_perst_release(struct qcom_pcie *qp);
/* CrDK PcieLib logging (pcie.c is built with CrOsLog=KshimPcieCrLog): EDK2
 * "%a" strings are printed as "%s" and a newline is added. */
void KshimPcieCrLog(const char *format, ...);
/* Status of a CR_STATUS as a kShimC code. */
int kshim_pci_qcom_status(CR_STATUS status);

/* Target data: kShimC SM8650 table (qcom_sm8650.c) and the unmodified CrDK
 * Target/Sm8450 and Target/Sm8250 CrPcieTarget.c, built with
 * CrTargetGetPcieContext renamed (ports/crdk/io/pci.cmake). */
const PcieTargetContext *kshim_pci_qcom_sm8650_target(void);
PcieTargetContext *KshimCrdkSm8450PcieContext(void);
PcieTargetContext *KshimCrdkSm8250PcieContext(void);
