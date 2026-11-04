/* SPDX-License-Identifier: MIT */
/*
 * GENI UART RX interrupt (kshim/uart.h kshim_geni_rx_irq_*), used by the
 * console with CONFIG_KSHIM_CONSOLE_RX_IRQ and by other GENI UART instances.
 *
 * Coexistence with CrDK DebugUartLib's polled engine:
 *  - only GENI_SE_IRQ_EN.GENI_S and S_IRQ_EN.{RX_FIFO_WATERMARK,
 *    RX_FIFO_LAST} are set; the M sequencer (TX) keeps its masked, polled
 *    path, so printf is unaffected;
 *  - while the interrupt is live the handler is the only RX reader and
 *    drains through CrDebugUartPollRead, keeping DebugUartLib's word
 *    unpacking, error recovery and latch clearing;
 *  - CrDebugUartPollQuiesce restores its own pre-kShimC snapshot of all three
 *    enable registers; kShimC restores its bits and frees the SPI before it.
 * The queue (struct kshim_uart rx_queue) is read with IRQs masked; on
 * overflow the oldest bytes are dropped. The handler never takes the console
 * ticket lock.
 */
#include <kshim/uart.h>
#include <qcom_irq.h>
#include <dt.h>
#include <Library/cr_debug_uart_poll.h>
#include <Library/cr_geni_regs.h>

#define RX_S_BITS (GENI_SE_S_IRQ_RX_FIFO_WATERMARK | GENI_SE_S_IRQ_RX_FIFO_LAST)

static uint32_t rd(struct kshim_uart *u, unsigned offset)
{ return u->geni.io.read32(u->geni.io.context, u->geni.base + offset); }
static void wr(struct kshim_uart *u, unsigned offset, uint32_t value)
{ u->geni.io.write32(u->geni.io.context, u->geni.base + offset, value); }

static void push(struct kshim_uart *u, uint8_t byte)
{
    if (u->rx_tail - u->rx_head == KSHIM_UART_RX_QUEUE) { ++u->rx_head; ++u->rx_dropped; }
    u->rx_queue[u->rx_tail % KSHIM_UART_RX_QUEUE] = byte;
    ++u->rx_tail;
}

/* IRQs masked. Bounded drain into the queue; the newest bytes win. */
static void drain(struct kshim_uart *u)
{
    for (unsigned budget = 4U * KSHIM_UART_RX_QUEUE; budget; --budget) {
        uint8_t byte;
        int status = CrDebugUartPollRead(&u->geni, &byte);
        if (status > 0) {
            push(u, byte);
            continue;
        }
        if (u->geni.poisoned) break;
        if (status < 0) continue; /* DebugUartLib recovered RX; read again. */
        /* Empty: drop the latch, then recheck so a word that landed in
         * between is read now rather than left without an interrupt. */
        wr(u, GENI_SE_S_IRQ_CLEAR, RX_S_BITS);
        if (!(rd(u, GENI_SE_RX_FIFO_STATUS) & GENI_SE_RX_FIFO_WC_MSK)) return;
    }
    if (u->geni.poisoned) {
        /* Nothing can clear the latch any more: mask kShimC's sources. */
        wr(u, GENI_SE_S_IRQ_EN, (rd(u, GENI_SE_S_IRQ_EN) & ~RX_S_BITS) | (u->rx_saved_s_irq_en & RX_S_BITS));
        kshim_irq_disable(u->intid);
    }
}

static void isr(uint32_t intid, void *context)
{
    (void)intid;
    drain(context);
}

/* The available GENI UART node whose window starts at the instance base. */
static int uart_node(const void *f, uintptr_t base)
{
    static const char *const compatibles[] = {
        "qcom,msm-geni-console", "qcom,geni-debug-uart", "qcom,geni-uart", "qcom,msm-geni-serial-hs",
    };
    for (unsigned i = 0; i < sizeof(compatibles) / sizeof(compatibles[0]); ++i)
        for (int n = fdt_node_offset_by_compatible(f, -1, compatibles[i]); n >= 0;
             n = fdt_node_offset_by_compatible(f, n, compatibles[i])) {
            struct dt_range reg;
            if (dt_available(f, n) && !dt_reg(f, n, 0, &reg) && reg.base == base) return n;
        }
    return -1;
}

int kshim_geni_rx_irq_start(struct kshim_uart *u, const void *fdt, int node)
{
    if (!u || !fdt) return ERROR_INVALID_PARAMETER;
    if (u->kind != KSHIM_UART_GENI || !u->open || !u->geni.initialized) return ERROR_NOT_SUPPORTED;
    if (u->rx_irq) return 0;
    if (!kshim_irq_ready()) return ERROR_NOT_READY;
    if (node < 0) node = uart_node(fdt, u->base);
    if (node < 0) return ERROR_NOT_SUPPORTED;
    u->rx_saved_irq_en = rd(u, GENI_SE_IRQ_EN);
    u->rx_saved_s_irq_en = rd(u, GENI_SE_S_IRQ_EN);
    /* Android: <0 357 0> under the PDC root, a pass-through; QUP serial
     * engines are level high. */
    uint32_t intid;
    int status = kshim_qcom_irq_request(fdt, node, 0, KSHIM_IRQ_TYPE_LEVEL_HIGH, isr, u, &intid);
    if (status) return status == ERROR_NOT_FOUND ? ERROR_NOT_SUPPORTED : status;
    u->intid = intid;
    unsigned long flags = kshim_irq_save();
    wr(u, GENI_SE_S_IRQ_EN, u->rx_saved_s_irq_en | RX_S_BITS);
    wr(u, GENI_SE_IRQ_EN, u->rx_saved_irq_en | GENI_SE_IRQ_GENI_S_EN);
    u->rx_irq = true;
    drain(u); /* bytes received before the interrupt existed */
    kshim_irq_restore(flags);
    return 0;
}

int kshim_geni_rx_irq_read(struct kshim_uart *u, uint8_t *byte)
{
    if (!u || !byte) return ERROR_INVALID_PARAMETER;
    if (!u->rx_irq) return ERROR_NOT_READY;
    unsigned long flags = kshim_irq_save();
    if (u->rx_head == u->rx_tail) drain(u); /* never strand a byte behind a lost latch */
    int got = u->rx_head != u->rx_tail;
    if (got) {
        *byte = u->rx_queue[u->rx_head % KSHIM_UART_RX_QUEUE];
        ++u->rx_head;
    }
    kshim_irq_restore(flags);
    return got;
}

int kshim_geni_rx_irq_stop(struct kshim_uart *u)
{
    if (!u) return ERROR_INVALID_PARAMETER;
    if (!u->rx_irq) return 0;
    unsigned long flags = kshim_irq_save();
    wr(u, GENI_SE_IRQ_EN, (rd(u, GENI_SE_IRQ_EN) & ~GENI_SE_IRQ_GENI_S_EN) |
                          (u->rx_saved_irq_en & GENI_SE_IRQ_GENI_S_EN));
    wr(u, GENI_SE_S_IRQ_EN, (rd(u, GENI_SE_S_IRQ_EN) & ~RX_S_BITS) | (u->rx_saved_s_irq_en & RX_S_BITS));
    kshim_irq_restore(flags);
    /* Source masked first, so the level line is low when the GIC lets go. */
    int status = kshim_qcom_irq_free(u->intid);
    if (status) return status;
    u->rx_irq = false;
    u->intid = 0;
    return 0;
}
