/* SPDX-License-Identifier: MIT */
#include <kshim_console.h>
#include <dt.h>
#include <pl011_uart.h>
#include <Library/cr_debug_uart_poll.h>
#include <kshim/irq.h>
static struct kshim_console_resource console;
static struct CrDebugUartPoll uart;
static bool initialized;
int kshim_console_init(const void *fdt)
{
    if (initialized) return 0;
    if (!fdt || dt_validate(fdt, fdt_totalsize(fdt)) || kshim_console_probe(fdt, &console)) return -1;
    if (console.geni) {
        const struct CrIo *io = CrIoDefault();
        if (!io) return -1;
        uart = (struct CrDebugUartPoll){.io = *io, .base = console.base, .size = console.size};
        if (CrDebugUartPollInit(&uart)) return -1;
    } else pl011_init(console.base);
    initialized = true;
    return 0;
}
/* A GENI transfer is a multi-register sequence; an interrupt handler that
 * prints must not start another one in the middle (printf from handlers). */
void kshim_console_write(const char *buffer, uint32_t length)
{
    if (!initialized || !buffer || !length) return;
    unsigned long flags = kshim_irq_save();
    if (console.geni) (void)CrDebugUartPollWrite(&uart, buffer, length);
    else pl011_write(console.base, (char *)buffer, (int)length);
    kshim_irq_restore(flags);
}
int kshim_console_try_read(char *character)
{
    if (!initialized || !character) return 0;
    unsigned long flags = kshim_irq_save();
    int status = console.geni ? CrDebugUartPollRead(&uart, (uint8_t *)character) > 0 :
                                pl011_try_read(console.base, character);
    kshim_irq_restore(flags);
    return status;
}
uint64_t kshim_console_base(void) { return initialized ? console.base : 0; }
size_t kshim_console_size(void) { return initialized ? console.size : 0; }
int kshim_console_quiesce(void)
{
    int status = console.geni ? CrDebugUartPollQuiesce(&uart) : 0;
    if (!status) initialized = false;
    return status;
}
