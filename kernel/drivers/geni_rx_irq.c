/* SPDX-License-Identifier: MIT */
/*
 * GENI UART console RX interrupt (CONFIG_KSHIM_CONSOLE_RX_IRQ).
 *
 * Coexistence with CrDK DebugUartLib's polled console:
 *  - only GENI_SE_IRQ_EN.GENI_S and S_IRQ_EN.{RX_FIFO_WATERMARK,
 *    RX_FIFO_LAST} are set; the M sequencer (TX) keeps its masked, polled
 *    path, so printf is unaffected;
 *  - while the interrupt is live the handler is the only RX reader and
 *    drains through CrDebugUartPollRead, keeping DebugUartLib's word
 *    unpacking, error recovery and latch clearing;
 *  - CrDebugUartPollQuiesce restores its own pre-kShimC snapshot of all three
 *    enable registers; kShimC restores its bits and frees the SPI before it.
 * The ring is read by kshim_console_try_read() with IRQs masked.
 */
#include <kshim_console.h>
#include <qcom_irq.h>
#include <dt.h>
#include <Library/cr_debug_uart_poll.h>
#include <Library/cr_geni_regs.h>

#define RX_RING 256U
#define RX_S_BITS (GENI_SE_S_IRQ_RX_FIFO_WATERMARK | GENI_SE_S_IRQ_RX_FIFO_LAST)

static struct {
    struct CrDebugUartPoll *uart;
    char data[RX_RING];
    unsigned head, count, dropped;
    bool attempted, active;
    uint32_t intid, saved_se, saved_s;
} Rx;

static uint32_t rd(unsigned offset)
{ return Rx.uart->io.read32(Rx.uart->io.context, Rx.uart->base + offset); }
static void wr(unsigned offset, uint32_t value)
{ Rx.uart->io.write32(Rx.uart->io.context, Rx.uart->base + offset, value); }

/* IRQs masked. Bounded drain into the ring; the newest bytes win. */
static void drain(void)
{
    for (unsigned budget = 4U * RX_RING; budget; --budget) {
        uint8_t byte;
        int status = CrDebugUartPollRead(Rx.uart, &byte);
        if (status > 0) {
            if (Rx.count == RX_RING) { Rx.head = (Rx.head + 1U) % RX_RING; --Rx.count; ++Rx.dropped; }
            Rx.data[(Rx.head + Rx.count++) % RX_RING] = (char)byte;
            continue;
        }
        if (Rx.uart->poisoned) break;
        if (status < 0) continue; /* DebugUartLib recovered RX; read again. */
        /* Empty: drop the latch, then recheck so a word that landed in
         * between is read now rather than left without an interrupt. */
        wr(GENI_SE_S_IRQ_CLEAR, RX_S_BITS);
        if (!(rd(GENI_SE_RX_FIFO_STATUS) & GENI_SE_RX_FIFO_WC_MSK)) return;
    }
    if (Rx.uart->poisoned) {
        /* Nothing can clear the latch any more: mask kShimC's sources. */
        wr(GENI_SE_S_IRQ_EN, (rd(GENI_SE_S_IRQ_EN) & ~RX_S_BITS) | (Rx.saved_s & RX_S_BITS));
        kshim_irq_disable(Rx.intid);
    }
}

static void isr(uint32_t intid, void *context) { (void)intid; (void)context; drain(); }

static int console_node(const void *f, uintptr_t base)
{
    static const char *const compatibles[] = {"qcom,msm-geni-console", "qcom,geni-debug-uart"};
    for (unsigned i = 0; i < 2; ++i)
        for (int n = fdt_node_offset_by_compatible(f, -1, compatibles[i]); n >= 0;
             n = fdt_node_offset_by_compatible(f, n, compatibles[i])) {
            struct dt_range reg;
            if (dt_available(f, n) && !dt_reg(f, n, 0, &reg) && reg.base == base) return n;
        }
    return -1;
}

static void start(struct CrDebugUartPoll *uart, const void *fdt, uintptr_t base)
{
    Rx.attempted = true;
    int node = fdt ? console_node(fdt, base) : -1;
    if (node < 0 || !uart->initialized || uart->base != base) return;
    Rx.uart = uart;
    Rx.saved_se = rd(GENI_SE_IRQ_EN);
    Rx.saved_s = rd(GENI_SE_S_IRQ_EN);
    /* Android: <0 357 0> under the PDC root, a pass-through; QUP serial
     * engines are level high. */
    if (kshim_qcom_irq_request(fdt, node, 0, KSHIM_IRQ_TYPE_LEVEL_HIGH, isr, NULL, &Rx.intid))
        return;
    unsigned long flags = kshim_irq_save();
    wr(GENI_SE_S_IRQ_EN, Rx.saved_s | RX_S_BITS);
    wr(GENI_SE_IRQ_EN, Rx.saved_se | GENI_SE_IRQ_GENI_S_EN);
    Rx.active = true;
    drain(); /* bytes received before the interrupt existed */
    kshim_irq_restore(flags);
}

int kshim_geni_rx_irq_read(struct CrDebugUartPoll *uart, const void *fdt, uintptr_t base,
                           char *character, int *received)
{
    /* The console comes up before the GIC: start on the first read after. */
    if (!Rx.attempted && kshim_irq_ready()) start(uart, fdt, base);
    if (!Rx.active || !character || !received) return 1;
    unsigned long flags = kshim_irq_save();
    if (!Rx.count) drain(); /* never strand a byte behind a lost latch */
    *received = Rx.count != 0;
    if (*received) {
        *character = Rx.data[Rx.head];
        Rx.head = (Rx.head + 1U) % RX_RING;
        --Rx.count;
    }
    kshim_irq_restore(flags);
    return 0;
}

int kshim_console_rx_irq_stop(void)
{
    if (!Rx.active) return 0;
    unsigned long flags = kshim_irq_save();
    wr(GENI_SE_IRQ_EN, (rd(GENI_SE_IRQ_EN) & ~GENI_SE_IRQ_GENI_S_EN) |
                       (Rx.saved_se & GENI_SE_IRQ_GENI_S_EN));
    wr(GENI_SE_S_IRQ_EN, (rd(GENI_SE_S_IRQ_EN) & ~RX_S_BITS) | (Rx.saved_s & RX_S_BITS));
    kshim_irq_restore(flags);
    /* Source masked first, so the level line is low when the GIC lets go. */
    if (kshim_qcom_irq_free(Rx.intid)) return -1;
    Rx.active = false;
    return 0;
}
