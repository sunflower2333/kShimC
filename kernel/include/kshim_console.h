/* SPDX-License-Identifier: MIT */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
/* kshim_console_write() and kshim_console_try_read() are public. */
#include <kshim/console.h>
struct kshim_console_resource { uintptr_t base; size_t size; bool geni; };
int kshim_console_probe(const void *fdt, struct kshim_console_resource *resource);
size_t kshim_console_size(void);
int kshim_console_quiesce(void);

/* Select the console from the boot FDT before the first printf. */
int kshim_console_init(const void *fdt);
uint64_t kshim_console_base(void);

/* GENI RX interrupt (CONFIG_KSHIM_CONSOLE_RX_IRQ, kernel/drivers/geni_rx_irq.c).
 * read: 0 when the interrupt path served the request (*received set),
 * non-zero to use the polled read. stop: handoff step 1. */
struct CrDebugUartPoll;
int kshim_geni_rx_irq_read(struct CrDebugUartPoll *uart, const void *fdt, uintptr_t base,
                           char *character, int *received);
int kshim_console_rx_irq_stop(void);
