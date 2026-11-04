/* SPDX-License-Identifier: MIT */
#pragma once
/*
 * Qualcomm QUPv3 / GENI serial engines (CONFIG_KSHIM_QUP).
 * Owner: qup (kernel/drivers/qup/, kernel/dt/qup.c). See docs/io-stack.md.
 *
 * Discovery (pure DT, no MMIO), on both binding families:
 *   wrapper:  mainline "qcom,geni-se-qup" (clocks "m-ahb", "s-ahb"; SEs are
 *             children), Android "qcom,qupv3-geni-se" (SEs are elsewhere and
 *             point back with "qcom,wrapper-core").
 *   engine:   mainline "qcom,geni-i2c", "qcom,geni-spi", "qcom,geni-uart",
 *             "qcom,geni-debug-uart"; Android "qcom,i2c-geni", "qcom,spi-geni",
 *             "qcom,msm-geni-serial-hs", "qcom,msm-geni-console".
 *   resources: reg (SE window, >= 0x4000 mainline / 0x1000 Android),
 *             clocks "se" (mainline) or "se-clk"/"m-ahb"/"s-ahb" (Android)
 *             through kshim/clk.h, pinctrl-0, interrupts[0] (GIC or PDC
 *             parent, kshim_qcom_irq_request_spec).
 * One SE register window can have several DT nodes (i2c4 and spi4 share
 * 0xa90000 on SM8650); ownership is per window, not per node.
 *
 * Policy: kShimC never loads SE firmware. The protocol the boot chain
 * loaded (GENI_FW_REVISION_RO, SE + 0x68, PROTOCOL [15:8]: 1 SPI, 2 UART,
 * 3 I2C) must equal the protocol requested, else ERROR_NOT_SUPPORTED.
 * Interconnect (ICC/BCM) votes and the CX power domain are assumed
 * inherited. Clocks: an SE whose clocks already run is used as is; otherwise
 * the wrapper AHB clocks are voted and the SE clock is put on CXO undivided
 * (kshim_clk_set_rate_cxo), exactly CrDK ClockLib clock_poll's policy; an
 * active SE (GENI_STATUS M/S_GENI_CMD_ACTIVE) is never retuned.
 * Transfers use CrDK's GENI FIFO core (Library/GeniSeLib/geni_fifo.c,
 * CrGeniInit/CrGeniI2cTransfer/CrGeniSpiTransfer/CrGeniQuiesce), which
 * snapshots and restores the SE configuration; GPI DMA is not used.
 */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "error_no.h"
#include <kshim/irq.h>

#define KSHIM_QUP_MAX_WRAPPERS 4U
#define KSHIM_QUP_MAX_SE       40U

/* GENI firmware protocol numbers (GENI_FW_REVISION_RO PROTOCOL field). */
enum kshim_qup_protocol {
    KSHIM_QUP_PROTOCOL_NONE = 0,
    KSHIM_QUP_PROTOCOL_SPI = 1,
    KSHIM_QUP_PROTOCOL_UART = 2,
    KSHIM_QUP_PROTOCOL_I2C = 3,
};

enum kshim_qup_owner {
    KSHIM_QUP_OWNER_NONE = 0,
    KSHIM_QUP_OWNER_CONSOLE, /* the DT console; quiesced last by kshim_console_quiesce() */
    KSHIM_QUP_OWNER_I2C,
    KSHIM_QUP_OWNER_SPI,
    KSHIM_QUP_OWNER_UART,
};

struct kshim_clk;

struct kshim_qup_wrapper {
    int node;
    uint64_t base, size;          /* wrapper common registers, 0 when the DT has none */
    struct kshim_clk *m_ahb, *s_ahb;
    unsigned index;
};

struct kshim_qup_se {
    unsigned index;               /* registry index */
    int node;                     /* first available DT node for this window */
    int nodes[3];                 /* every DT node naming this window (-1 = unused) */
    const struct kshim_qup_wrapper *wrapper;
    uint64_t base, size;          /* SE register window */
    enum kshim_qup_protocol dt_protocol; /* from the compatible of `node` */
    uint32_t clock_frequency;     /* DT "clock-frequency" (I2C), 0 if absent */
    struct kshim_clk *se_clk;
    /* Runtime, owned by the QUP core. */
    enum kshim_qup_owner owner;
    const char *owner_name;
    int owner_node;               /* DT node the owner bound (i2c4 vs spi4) */
    bool acquired;                /* clocks/pins taken by kshim_qup_se_acquire() */
    bool inherited_active;        /* clocks were running and FW loaded at acquire */
    uint32_t intid;               /* SE interrupt once requested, 0 otherwise */
};

/* Bind every wrapper and SE in the FDT and claim the console's SE
 * (kshim_console_base()) for KSHIM_QUP_OWNER_CONSOLE. Idempotent, no MMIO. */
int kshim_qup_init(const void *fdt);
struct kshim_resources;
/* Map the SE window (device, RW) of every SE with a user: an available I2C
 * or SPI controller node with an available child, or an available UART node
 * (kshim_dt_qup_window_used). The wrapper common window is never accessed
 * and the pins are in the TLMM window kshim_gpio_map_resources() maps. */
int kshim_qup_map_resources(struct kshim_resources *resources);

unsigned kshim_qup_se_count(void);
struct kshim_qup_se *kshim_qup_se_get(unsigned index);
/* By any of its DT nodes, or by SE window base. NULL when unknown. */
struct kshim_qup_se *kshim_qup_se_by_node(int node);
struct kshim_qup_se *kshim_qup_se_by_base(uint64_t base);

/* Exclusive ownership of the SE window for `owner` through DT node `node`
 * (which selects the pinctrl/IRQ/clock-frequency to use). ERROR_BUSY when
 * owned by someone else; claiming again with the same owner and node is a
 * no-op. Thread context. */
int kshim_qup_se_claim(struct kshim_qup_se *se, enum kshim_qup_owner owner,
                       int node, const char *owner_name);
int kshim_qup_se_unclaim(struct kshim_qup_se *se);

/* Clock and pin bring-up for `protocol` (see the policy above), checks the
 * loaded firmware protocol, applies the owner node's pinctrl "default".
 * *source_hz and *dfs_index receive what CrGeniConfig needs (source_hz,
 * clock_index). Thread context; snapshot for kshim_qup_se_release(). */
int kshim_qup_se_acquire(struct kshim_qup_se *se, enum kshim_qup_protocol protocol,
                         uint32_t *source_hz, uint8_t *dfs_index);
/* Undo kshim_qup_se_acquire() (clocks, RCG, pins back to the snapshot).
 * The protocol driver must have run CrGeniQuiesce() first. */
int kshim_qup_se_release(struct kshim_qup_se *se);

/* Request interrupts[0] of the owner node (GIC or PDC parent). The handler
 * runs in IRQ context: acknowledge/mask the SE (M_IRQ_EN/S_IRQ_EN or
 * kshim_irq_disable(intid)) and complete; never run the FIFO there.
 * ERROR_NOT_SUPPORTED when the IRQ does not resolve: poll. */
int kshim_qup_se_irq_request(struct kshim_qup_se *se, kshim_irq_handler_t handler,
                             void *context, uint32_t *intid);
int kshim_qup_se_irq_free(struct kshim_qup_se *se);

/* SE base address of a QUP SE window for CrDK (CrGeniConfig.base/.size). */
static inline uintptr_t kshim_qup_se_mmio(const struct kshim_qup_se *se) { return (uintptr_t)se->base; }

/* Handoff: release every acquired SE except the console's, newest first.
 * The I2C/SPI/UART owners have quiesced their engines before this. */
int kshim_qup_quiesce_all(void);

/* ---- Additions by the qup owner (additive, docs/qup.md) ----------------- */

/* Acquire details (kernel/drivers/qup/qup.c):
 *  - the owner node's clocks are "se" (mainline) or "se-clk" (Android); the
 *    AHB clocks "m-ahb"/"s-ahb" come from the wrapper node when it has them
 *    (mainline), else from the owner node (Android);
 *  - source_hz and dfs_index may be NULL for a protocol that keeps the
 *    inherited rate (UART with baud 0): the RCG is then never touched and
 *    an inherited active sequencer (a UART RX command) is accepted;
 *  - kshim_qup_se_release() drops kShimC's clock enables; the RCG selection
 *    and the pin snapshots stay with kshim_clk_restore_all() and
 *    kshim_gpio_restore_all(), which run later in kshim_io_quiesce();
 *  - a window that kshim_qup_map_resources() did not map is refused with
 *    ERROR_NOT_SUPPORTED once a mapping pass has run (the MMU would fault). */

struct CrIo;
/* Register access and time for the QUP core and the GENI engines:
 * CrIoDefault() unless a host register model is injected. */
void kshim_qup_set_io(const struct CrIo *io);
const struct CrIo *kshim_qup_io(void);
/* FDT bound by kshim_qup_init(), NULL before. */
const void *kshim_qup_fdt(void);

/* DT helpers (kernel/dt/qup.c), pure FDT, no MMIO. */
/* GENI protocol named by an SE node's compatible, NONE for other nodes. */
enum kshim_qup_protocol kshim_dt_qup_protocol(const void *fdt, int node);
/* True for the Android binding family ("qcom,i2c-geni", "qcom,spi-geni",
 * "qcom,msm-geni-serial-hs", "qcom,msm-geni-console"). */
bool kshim_dt_qup_android(const void *fdt, int node);
/* Every SE register window with at least one available node, in DT order,
 * and the wrappers they reference (parent "qcom,geni-se-qup" or
 * "qcom,wrapper-core"). Malformed nodes are skipped; windows past the
 * capacity are dropped and reported with KSHIM_IO_ERR_OVERFLOW (the tables
 * are still filled). Wrapper pointers point into `wrappers`. */
int kshim_dt_qup_discover(const void *fdt, struct kshim_qup_wrapper *wrappers,
                          unsigned max_wrappers, unsigned *wrapper_count,
                          struct kshim_qup_se *ses, unsigned max_ses, unsigned *se_count);
/* I2C bus rate of a controller node: "clock-frequency", else Android
 * "qcom,clk-freq-out", else the binding family's default: 100 kHz for
 * "qcom,geni-i2c" (Linux i2c-qcom-geni), 400 kHz for "qcom,i2c-geni" (msm
 * i2c-qcom-geni, and kShimC's former FTM5 binding). */
uint32_t kshim_dt_qup_i2c_bus_hz(const void *fdt, int node);
/* Whether kshim_qup_map_resources() maps this window: an available I2C or
 * SPI controller node with an available child, or an available UART node. */
bool kshim_dt_qup_window_used(const void *fdt, const struct kshim_qup_se *se);
