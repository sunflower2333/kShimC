/* SPDX-License-Identifier: MIT */
/*
 * UART instances (kshim/uart.h): PL011 and GENI serial engines in UART mode
 * behind one byte-stream API. The console (kernel/drivers/console.c) keeps
 * one instance; it is built in every profile.
 *
 * GENI instances run CrDK DebugUartLib's polled FIFO engine
 * (Library/DebugUartLib/debug_uart_poll.c): TX is a bounded polled command,
 * RX uses the S sequencer's UART_START_READ that DebugUartLib keeps running,
 * and quiesce restores the configuration the boot chain left. The baud
 * divider is never changed. RX can be interrupt-driven through
 * kernel/drivers/geni_rx_irq.c.
 */
#include <kshim/uart.h>
#include <kshim/io.h>
#include <kshim/irq.h>
#include <Library/cr_debug_uart_poll.h>
#include <config.h>
#include <dt.h>
#include <pl011_uart.h>
#include <string.h>
#if CONFIG_KSHIM_QUP
#include <kshim/qup.h>
#endif

#define GENI_UART_MIN_WINDOW 0xe2cU /* DebugUartLib's highest register + 4 */
#define PL011_MIN_WINDOW 0x100U

#define UART_DT_MAX 4U

static const struct CrIo *Io;
/* Instances opened through kshim_uart_open_dt(), for kshim_uart_quiesce_all(). */
static struct kshim_uart *DtInstances[UART_DT_MAX];

void kshim_uart_set_io(const struct CrIo *io) { Io = io; }

static const struct CrIo *uart_io(void)
{
    const struct CrIo *io = Io ? Io : CrIoDefault();
    return CrIoValid(io) ? io : NULL;
}

int kshim_uart_open_mmio(struct kshim_uart *uart, enum kshim_uart_kind kind,
                         uintptr_t base, size_t size)
{
    if (!uart || !base || (base & 3U)) return ERROR_INVALID_PARAMETER;
    if (uart->open) return ERROR_BUSY;
    memset(uart, 0, sizeof(*uart));
    uart->kind = kind;
    uart->base = base;
    uart->size = size;
    switch (kind) {
    case KSHIM_UART_PL011:
        if (size < PL011_MIN_WINDOW) return ERROR_INVALID_PARAMETER;
        pl011_init(base);
        break;
    case KSHIM_UART_GENI: {
        const struct CrIo *io = uart_io();
        if (size < GENI_UART_MIN_WINDOW) return ERROR_INVALID_PARAMETER;
        if (!io) return ERROR_NOT_READY;
        uart->geni = (struct CrDebugUartPoll){.io = *io, .base = base, .size = size};
        int status = CrDebugUartPollInit(&uart->geni);
        if (status) {
            /* A claimed-then-failed init (RX abort timeout) keeps its saved
             * state for quiesce; anything else leaves the SE untouched. */
            uart->open = uart->geni.claimed;
            return kshim_io_err_from_bus(status);
        }
        break;
    }
    default:
        return ERROR_INVALID_PARAMETER;
    }
    uart->open = true;
    return 0;
}

int kshim_uart_open_dt(struct kshim_uart *uart, const void *fdt, int node, uint32_t baud)
{
    if (!uart || !fdt || node < 0) return ERROR_INVALID_PARAMETER;
    if (baud) return ERROR_NOT_SUPPORTED; /* the inherited divider only */
    if (!dt_available(fdt, node)) return ERROR_NOT_FOUND;
    struct dt_range reg;
    if (dt_reg(fdt, node, 0, &reg) || reg.base > UINTPTR_MAX || reg.size > SIZE_MAX)
        return ERROR_INVALID_PARAMETER;
    if (!fdt_node_check_compatible(fdt, node, "arm,pl011"))
        return kshim_uart_open_mmio(uart, KSHIM_UART_PL011, (uintptr_t)reg.base, (size_t)reg.size);
#if CONFIG_KSHIM_QUP
    if (kshim_dt_qup_protocol(fdt, node) != KSHIM_QUP_PROTOCOL_UART) return ERROR_NOT_SUPPORTED;
    if (uart->open) return ERROR_BUSY;
    struct kshim_qup_se *se = kshim_qup_se_by_node(node);
    if (!se) return ERROR_NOT_FOUND;
    int status = kshim_qup_se_claim(se, KSHIM_QUP_OWNER_UART, node, "uart");
    if (status) return status;
    status = kshim_qup_se_acquire(se, KSHIM_QUP_PROTOCOL_UART, NULL, NULL);
    if (!status) {
        status = kshim_uart_open_mmio(uart, KSHIM_UART_GENI, (uintptr_t)se->base, (size_t)se->size);
        if (status && !uart->open) (void)kshim_qup_se_release(se);
    }
    if (status && !uart->open) {
        (void)kshim_qup_se_unclaim(se);
        return status;
    }
    uart->se = se;
    for (unsigned i = 0; i < UART_DT_MAX; ++i)
        if (!DtInstances[i]) { DtInstances[i] = uart; break; }
    return status;
#else
    return ERROR_NOT_SUPPORTED;
#endif
}

int kshim_uart_write(struct kshim_uart *uart, const void *data, size_t length)
{
    if (!uart || (!data && length)) return ERROR_INVALID_PARAMETER;
    if (!uart->open) return ERROR_NOT_READY;
    if (!length) return 0;
    int status = 0;
    /* A GENI transfer is a multi-register sequence: a handler that prints
     * must not start another one in the middle. */
    unsigned long flags = kshim_irq_save();
    if (uart->kind == KSHIM_UART_GENI)
        status = kshim_io_err_from_bus(CrDebugUartPollWrite(&uart->geni, data, length));
    else
        pl011_write(uart->base, (char *)(uintptr_t)data, (int)length);
    kshim_irq_restore(flags);
    return status;
}

int kshim_uart_read_byte(struct kshim_uart *uart, uint8_t *byte)
{
    if (!uart || !byte) return ERROR_INVALID_PARAMETER;
    if (!uart->open) return ERROR_NOT_READY;
#if CONFIG_QCOM_IRQCHIP
    if (uart->rx_irq) {
        int status = kshim_geni_rx_irq_read(uart, byte);
        if (status != ERROR_NOT_READY) return status;
    }
#endif
    unsigned long flags = kshim_irq_save();
    /* Bytes the interrupt queued before it was stopped come first. */
    if (uart->rx_head != uart->rx_tail) {
        *byte = uart->rx_queue[uart->rx_head % KSHIM_UART_RX_QUEUE];
        ++uart->rx_head;
        kshim_irq_restore(flags);
        return 1;
    }
    int status;
    if (uart->kind == KSHIM_UART_GENI) {
        status = CrDebugUartPollRead(&uart->geni, byte);
        /* DebugUartLib recovered RX after an error: nothing was read. */
        if (status < 0) status = uart->geni.poisoned ? ERROR_DEVICE_ERROR : 0;
    } else {
        char c;
        status = pl011_try_read(uart->base, &c);
        if (status > 0) { *byte = (uint8_t)c; status = 1; }
        else status = 0;
    }
    kshim_irq_restore(flags);
    return status;
}

int kshim_uart_rx_irq_start(struct kshim_uart *uart, const void *fdt, int node)
{
    if (!uart || !fdt) return ERROR_INVALID_PARAMETER;
    if (!uart->open || uart->kind != KSHIM_UART_GENI) return ERROR_NOT_SUPPORTED;
#if CONFIG_QCOM_IRQCHIP
    return kshim_geni_rx_irq_start(uart, fdt, node);
#else
    (void)node;
    return ERROR_NOT_SUPPORTED;
#endif
}

int kshim_uart_rx_irq_stop(struct kshim_uart *uart)
{
    if (!uart) return ERROR_INVALID_PARAMETER;
#if CONFIG_QCOM_IRQCHIP
    return kshim_geni_rx_irq_stop(uart);
#else
    return 0;
#endif
}

int kshim_uart_quiesce(struct kshim_uart *uart)
{
    if (!uart) return ERROR_INVALID_PARAMETER;
    if (!uart->open) return 0;
    int status = kshim_uart_rx_irq_stop(uart);
    if (status) return status;
    if (uart->kind == KSHIM_UART_GENI) {
        status = CrDebugUartPollQuiesce(&uart->geni);
        if (status) return kshim_io_err_from_bus(status); /* ownership kept */
    }
#if CONFIG_KSHIM_QUP
    if (uart->se) {
        status = kshim_qup_se_release(uart->se);
        if (!status) status = kshim_qup_se_unclaim(uart->se);
        if (status) return status;
        uart->se = NULL;
    }
#endif
    for (unsigned i = 0; i < UART_DT_MAX; ++i)
        if (DtInstances[i] == uart) DtInstances[i] = NULL;
    uart->open = false;
    return 0;
}

int kshim_uart_quiesce_all(void)
{
    int result = 0;
    for (unsigned i = UART_DT_MAX; i > 0; --i) {
        struct kshim_uart *uart = DtInstances[i - 1];
        if (!uart) continue;
        int status = kshim_uart_quiesce(uart);
        if (status && !result) result = status;
    }
    return result;
}
