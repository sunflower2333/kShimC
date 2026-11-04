/* SPDX-License-Identifier: MIT */
#pragma once
/*
 * GENI FIFO engine shared by the I2C and SPI controllers (private to
 * kernel/drivers/qup/). It runs CrDK's GeniSeLib FIFO core unmodified and
 * makes it interrupt-driven through its CrIo hooks:
 *
 *  - CrGeniInit() leaves SE_IRQ_EN at 0 and M_IRQ_EN at the FIFO core's
 *    status set (done, cancel, abort, errors, TX/RX watermarks); the engine
 *    then sets SE_IRQ_EN.GENI_M_EN so that status reaches the SE's GIC SPI.
 *    CrGeniQuiesce() restores SE_IRQ_EN from its own snapshot.
 *  - The interrupt handler only masks the SPI at the GIC and counts; the
 *    status stays latched for the FIFO core, which reads and clears it in
 *    thread context.
 *  - CrIo.delay_us is the FIFO core's poll step. It first waits the
 *    requested time (the protocol minimum, e.g. a CS hold), then, while an
 *    M command is active and no enabled status is pending, unmasks the SPI
 *    and sleeps in kshim_wait_event() (WFI) for at most about 1 ms. The SE
 *    interrupt ends the sleep at once.
 *  - Polling fallback: no GIC, an interrupt that does not resolve, or eight
 *    sleeps that end with enabled status pending and no interrupt taken,
 *    with no interrupt delivered in between (it is not delivered), switch
 *    the engine to the plain busy-wait poll step and free the interrupt.
 */
#include <kshim/qup.h>
#include <Library/cr_geni.h>

#define KSHIM_GENI_MISSED_IRQ_LIMIT 8U

struct kshim_geni_engine {
    struct kshim_qup_se *se;
    struct CrGeni bus;
    struct CrGeniConfig config;
    struct CrIo io;               /* hooks handed to CrDK; context = this engine */
    const struct CrIo *mmio;      /* register/time backend (kshim_qup_io()) */
    uint32_t m_irq_en;            /* M_IRQ_EN programmed by CrGeniInit */
    uint32_t intid;
    bool started, irq_tried, irq_requested;
    volatile bool irq_live, masked;
    volatile uint32_t irq_count;
    uint32_t misses, sleeps;
};

/* Claimed SE: acquire it for `protocol`, CrGeniInit with speed/mode/cs, try
 * the interrupt. Returns a kShimC code; on failure nothing stays acquired. */
int kshim_geni_engine_start(struct kshim_geni_engine *engine, struct kshim_qup_se *se,
                            enum kshim_qup_protocol protocol, uint32_t speed_hz,
                            uint8_t spi_mode, uint8_t chip_select);
/* Before a transfer: retry the interrupt once the GIC is up, recover a
 * poisoned FIFO core (CrGeniQuiesce + CrGeniInit), reprogram a different
 * SPI speed/mode/chip select. */
int kshim_geni_engine_prepare(struct kshim_geni_engine *engine, uint32_t speed_hz,
                              uint8_t spi_mode, uint8_t chip_select);
/* Mask and free the interrupt, CrGeniQuiesce, release the SE. A quiesce
 * failure keeps the SE acquired (and must block handoff). Idempotent. */
int kshim_geni_engine_stop(struct kshim_geni_engine *engine);
