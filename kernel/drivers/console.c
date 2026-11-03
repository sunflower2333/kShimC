/* SPDX-License-Identifier: MIT */
#include <kshim_console.h>
#include <dt.h>
#include <pl011_uart.h>
#include <Library/cr_debug_uart_poll.h>
#if !defined(KSHIM_HOST_TEST)
#include <config.h>
#endif
static struct kshim_console_resource console;
static struct CrDebugUartPoll uart;
static bool initialized;
#if CONFIG_KSHIM_CONSOLE_RX_IRQ
static const void *ConsoleFdt;
#endif
int kshim_console_init(const void *fdt)
{
    if (initialized) return 0;
    if (!fdt || dt_validate(fdt, fdt_totalsize(fdt)) || kshim_console_probe(fdt, &console)) return -1;
#if CONFIG_KSHIM_CONSOLE_RX_IRQ
    ConsoleFdt = fdt;
#endif
    if (console.geni) {
        const struct CrIo *io = CrIoDefault();
        if (!io) return -1;
        uart = (struct CrDebugUartPoll){.io = *io, .base = console.base, .size = console.size};
        if (CrDebugUartPollInit(&uart)) return -1;
    } else pl011_init(console.base);
    initialized = true;
    return 0;
}
void kshim_console_write(const char *buffer, uint32_t length)
{
    if (!initialized || !buffer || !length) return;
    if (console.geni) (void)CrDebugUartPollWrite(&uart, buffer, length);
    else pl011_write(console.base, (char *)buffer, (int)length);
}
int kshim_console_try_read(char *character)
{
    if (!initialized || !character) return 0;
#if CONFIG_KSHIM_CONSOLE_RX_IRQ
    /* RX interrupt (kernel/drivers/geni_rx_irq.c) when it is live. */
    int received;
    if (console.geni && !kshim_geni_rx_irq_read(&uart, ConsoleFdt, console.base, character, &received))
        return received;
#endif
    if (console.geni) return CrDebugUartPollRead(&uart, (uint8_t *)character) > 0;
    return pl011_try_read(console.base, character);
}
uint64_t kshim_console_base(void) { return initialized ? console.base : 0; }
size_t kshim_console_size(void) { return initialized ? console.size : 0; }
int kshim_console_quiesce(void)
{
#if CONFIG_KSHIM_CONSOLE_RX_IRQ
    if (kshim_console_rx_irq_stop()) return -1;
#endif
    int status = console.geni ? CrDebugUartPollQuiesce(&uart) : 0;
    if (!status) initialized = false;
    return status;
}
