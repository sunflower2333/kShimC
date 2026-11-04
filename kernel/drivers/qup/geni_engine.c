/* SPDX-License-Identifier: MIT */
/*
 * GENI FIFO engine glue: CrDK GeniSeLib (Library/GeniSeLib/geni_fifo.c,
 * unmodified) driven through CrIo hooks that sleep on the SE interrupt.
 * See geni_engine.h for the model.
 *
 * Register facts (GENI SE common block; Linux include/linux/soc/qcom/
 * geni-se.h, CrDK Include/Library/cr_geni_regs.h):
 *   GENI_STATUS +0x40 (M_GENI_CMD_ACTIVE bit 0), M_IRQ_STATUS +0x610,
 *   M_IRQ_EN +0x614, SE_IRQ_EN +0xe1c (GENI_M_IRQ_EN bit 2).
 */
#include "geni_engine.h"
#include <kshim/io.h>
#include <kshim/irq.h>
#include <kshim/time.h>
#include <Library/cr_geni_regs.h>
#include <config.h>
#include <stdio.h>
#include <string.h>

static uint32_t rd(const struct kshim_geni_engine *e, uint32_t offset)
{
    return e->mmio->read32(e->mmio->context, e->config.base + offset);
}

static void wr(const struct kshim_geni_engine *e, uint32_t offset, uint32_t value)
{
    e->mmio->write32(e->mmio->context, e->config.base + offset, value);
}

static uint32_t io_read(void *context, uintptr_t address)
{
    const struct kshim_geni_engine *e = context;
    return e->mmio->read32(e->mmio->context, address);
}

static void io_write(void *context, uintptr_t address, uint32_t value)
{
    const struct kshim_geni_engine *e = context;
    e->mmio->write32(e->mmio->context, address, value);
}

static uint64_t io_now(void *context)
{
    const struct kshim_geni_engine *e = context;
    return e->mmio->now_us(e->mmio->context);
}

/* IRQ context: mask the SPI, leave the SE status latched for the FIFO core. */
static void engine_irq(uint32_t intid, void *context)
{
    struct kshim_geni_engine *e = context;
    kshim_irq_disable(intid);
    e->masked = true;
    ++e->irq_count;
}

static void set_m_irq(struct kshim_geni_engine *e, bool enabled)
{
    uint32_t value = rd(e, GENI_SE_IRQ_EN);
    uint32_t next = enabled ? value | GENI_SE_IRQ_GENI_M_EN : value & ~GENI_SE_IRQ_GENI_M_EN;
    if (next != value) wr(e, GENI_SE_IRQ_EN, next);
}

static void stop_irq(struct kshim_geni_engine *e)
{
    if (!e->irq_requested) return;
    e->irq_live = false;
    /* Source first, so the level line is low when the GIC lets go. */
    unsigned long flags = kshim_irq_save();
    if (e->bus.initialized) set_m_irq(e, false);
    kshim_irq_restore(flags);
    if (!kshim_qup_se_irq_free(e->se)) e->irq_requested = false;
}

static void fall_back_to_polling(struct kshim_geni_engine *e)
{
    printf("QUP: engine %#llx interrupt %u not delivered, polling\n",
           (unsigned long long)e->se->base, (unsigned)e->intid);
    stop_irq(e);
}

/* The FIFO core's poll step (and its protocol delays). */
static void io_delay(void *context, uint32_t us)
{
    struct kshim_geni_engine *e = context;
    e->mmio->delay_us(e->mmio->context, us);
    if (!e->irq_live) return;
    /* Nothing to wait for, or status the core has not read yet. */
    if (!(rd(e, GENI_SE_STATUS) & GENI_SE_STATUS_M_GENI_CMD_ACTIVE)) return;
    if (rd(e, GENI_SE_M_IRQ_STATUS) & e->m_irq_en) return;
    uint32_t seen = e->irq_count;
    if (e->masked) {
        e->masked = false;
        kshim_irq_enable(e->intid);
    }
    ++e->sleeps;
    kshim_wait_event(get_current_time() + 1);
    if (e->irq_count != seen) {
        e->misses = 0;
        return;
    }
    /* The sleep ran out with status pending and the line enabled, and no
     * interrupt came. A delivered interrupt clears the count. */
    if ((rd(e, GENI_SE_M_IRQ_STATUS) & e->m_irq_en) &&
        ++e->misses >= KSHIM_GENI_MISSED_IRQ_LIMIT)
        fall_back_to_polling(e);
}

static void try_irq(struct kshim_geni_engine *e)
{
    if (e->irq_tried || e->irq_requested || !kshim_irq_ready()) return;
    e->irq_tried = true;
    uint32_t intid = 0;
    int status = kshim_qup_se_irq_request(e->se, engine_irq, e, &intid);
    if (status) {
        printf("QUP: engine %#llx polled (%s)\n", (unsigned long long)e->se->base,
               kshim_io_strerror(status));
        return;
    }
    e->intid = intid;
    e->irq_requested = true;
    e->masked = false;
    e->misses = 0;
    unsigned long flags = kshim_irq_save();
    set_m_irq(e, true);
    kshim_irq_restore(flags);
    e->irq_live = true;
}

int kshim_geni_engine_start(struct kshim_geni_engine *e, struct kshim_qup_se *se,
                            enum kshim_qup_protocol protocol, uint32_t speed_hz,
                            uint8_t spi_mode, uint8_t chip_select)
{
    if (!e || !se || (protocol != KSHIM_QUP_PROTOCOL_I2C && protocol != KSHIM_QUP_PROTOCOL_SPI))
        return ERROR_INVALID_PARAMETER;
    if (e->started) return ERROR_BUSY;
    memset(e, 0, sizeof(*e));
    e->se = se;
    e->mmio = kshim_qup_io();
    if (!e->mmio) return ERROR_NOT_READY;
    e->io = (struct CrIo){e, io_read, io_write, io_now, io_delay};
    uint32_t source_hz = 0;
    uint8_t dfs_index = 0;
    int status = kshim_qup_se_acquire(se, protocol, &source_hz, &dfs_index);
    if (status) return status;
    e->config = (struct CrGeniConfig){
        .base = kshim_qup_se_mmio(se), .size = (size_t)se->size,
        .protocol = (enum CrGeniProtocol)protocol, .source_hz = source_hz,
        .speed_hz = speed_hz, .clock_index = dfs_index,
        .spi_mode = spi_mode, .chip_select = chip_select,
    };
    status = CrGeniInit(&e->bus, &e->config, &e->io);
    if (status) {
        (void)kshim_qup_se_release(se);
        return kshim_io_err_from_bus(status);
    }
    e->m_irq_en = rd(e, GENI_SE_M_IRQ_EN);
    e->started = true;
    try_irq(e);
    return 0;
}

static int reinit(struct kshim_geni_engine *e, const struct CrGeniConfig *next)
{
    /* An engine that cannot even be stopped stays poisoned: report it as a
     * device error rather than the recovery's own timeout. */
    if (e->bus.initialized && CrGeniQuiesce(&e->bus)) return ERROR_DEVICE_ERROR;
    int status = CrGeniInit(&e->bus, next, &e->io);
    if (status) {
        /* Keep the previous working configuration when the new one is
         * refused (for example an SPI rate the divider cannot reach). */
        if (CrGeniInit(&e->bus, &e->config, &e->io)) e->bus.initialized = false;
    } else {
        e->config = *next;
    }
    if (e->bus.initialized) {
        e->m_irq_en = rd(e, GENI_SE_M_IRQ_EN);
        if (e->irq_live) set_m_irq(e, true);
    }
    return status ? kshim_io_err_from_bus(status) : 0;
}

int kshim_geni_engine_prepare(struct kshim_geni_engine *e, uint32_t speed_hz,
                              uint8_t spi_mode, uint8_t chip_select)
{
    if (!e || !e->started) return ERROR_NOT_READY;
    try_irq(e);
    struct CrGeniConfig next = e->config;
    if (next.protocol == CR_GENI_SPI) {
        next.speed_hz = speed_hz;
        next.spi_mode = spi_mode;
        next.chip_select = chip_select;
    }
    bool changed = next.speed_hz != e->config.speed_hz || next.spi_mode != e->config.spi_mode ||
                   next.chip_select != e->config.chip_select;
    if (!changed && e->bus.initialized && !e->bus.poisoned) return 0;
    if (e->bus.poisoned) printf("QUP: engine %#llx recovering\n", (unsigned long long)e->se->base);
    return reinit(e, &next);
}

int kshim_geni_engine_stop(struct kshim_geni_engine *e)
{
    if (!e || !e->started) return 0;
    stop_irq(e);
    if (e->irq_requested) return ERROR_DEVICE_ERROR;
    int status = CrGeniQuiesce(&e->bus);
    if (status) return kshim_io_err_from_bus(status);
    status = kshim_qup_se_release(e->se);
    if (status) return status;
    e->started = false;
    return 0;
}
