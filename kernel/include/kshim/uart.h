/* SPDX-License-Identifier: MIT */
#pragma once
/*
 * UART instances (GENI and PL011) under the console (CONFIG_KSHIM_QUP for
 * GENI instances bound to a QUP SE; PL011 always).
 * Owner: qup (kernel/drivers/qup/geni_uart.c, kernel/drivers/console.c,
 * kernel/drivers/geni_rx_irq.c). See docs/io-stack.md.
 *
 * The public console API (include/kshim/console.h) and kshim_console.h stay
 * unchanged; console.c keeps one struct kshim_uart for the DT console and
 * routes its UART calls here. Rules that carry over unchanged:
 *  - TX is polled (CrDK DebugUartLib), under kshim_irq_save so a handler
 *    that prints cannot interleave a multi-register GENI sequence;
 *  - RX may be interrupt-driven (S-sequencer RX watermark/last only), the
 *    handler queues bytes and never takes the console ticket lock;
 *  - the console instance is opened before the MMU, QUP discovery and
 *    kshim_io_init(), from its MMIO window only (inherited configuration),
 *    and is quiesced last, by kshim_console_quiesce() in prepare_boot().
 */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "error_no.h"
#include <Library/cr_debug_uart_poll.h>

enum kshim_uart_kind {
    KSHIM_UART_NONE = 0,
    KSHIM_UART_PL011,
    KSHIM_UART_GENI,
};

#define KSHIM_UART_RX_QUEUE 64U

struct kshim_qup_se;
struct kshim_uart {
    enum kshim_uart_kind kind;
    uintptr_t base;
    size_t size;
    uint32_t baud;               /* 0: inherited configuration */
    struct kshim_qup_se *se;     /* GENI instance opened through kshim_uart_open_dt */
    struct CrDebugUartPoll geni; /* GENI state (CrDK) */
    /* RX interrupt queue (single producer IRQ, single consumer). */
    uint32_t intid;
    bool rx_irq;
    uint8_t rx_queue[KSHIM_UART_RX_QUEUE];
    volatile uint32_t rx_head, rx_tail;
    bool open;
};

/* Early console: inherited configuration of the window at base/size. */
int kshim_uart_open_mmio(struct kshim_uart *uart, enum kshim_uart_kind kind,
                         uintptr_t base, size_t size);
/* A DT UART node (GENI through its QUP SE: claim, acquire clocks/pins when
 * not inherited; baud 0 keeps the inherited divider). */
int kshim_uart_open_dt(struct kshim_uart *uart, const void *fdt, int node, uint32_t baud);

/* Polled TX of all bytes; IRQ-safe (kshim_irq_save inside). */
int kshim_uart_write(struct kshim_uart *uart, const void *data, size_t length);
/* Non-blocking: 1 and *byte when a byte was available, 0 when none, else an
 * error. Drains the RX interrupt queue first, else polls the FIFO. */
int kshim_uart_read_byte(struct kshim_uart *uart, uint8_t *byte);

/* RX interrupt from interrupts[0] of `node` (CONFIG_KSHIM_CONSOLE_RX_IRQ
 * keeps gating it for the console). ERROR_NOT_SUPPORTED: keep polling. */
int kshim_uart_rx_irq_start(struct kshim_uart *uart, const void *fdt, int node);
int kshim_uart_rx_irq_stop(struct kshim_uart *uart);

/* Stop RX interrupts, wait for both sequencers idle, restore the inherited
 * configuration (CrDebugUartPollQuiesce) and release the SE (DT instances).
 * A timeout keeps ownership and must block handoff. */
int kshim_uart_quiesce(struct kshim_uart *uart);
