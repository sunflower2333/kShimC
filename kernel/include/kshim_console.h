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

/* Select the console from the boot FDT before the first printf. With
 * CONFIG_KSHIM_CONSOLE_USB_ACM this also starts the USB CDC-ACM mirror when
 * the DT has a usable DWC3 core; init succeeds if either backend works. */
int kshim_console_init(const void *fdt);
uint64_t kshim_console_base(void);

/* Console RX interrupt stop (CONFIG_KSHIM_CONSOLE_RX_IRQ starts it on the
 * first read after the GIC is up, kshim_uart_rx_irq_start): handoff step 1.
 * The RX interrupt engine itself is kshim/uart.h kshim_geni_rx_irq_*. */
int kshim_console_rx_irq_stop(void);

struct kshim_resources;
/* Add the USB console's controller window to the MMU resources; on failure
 * the USB console is stopped before the MMU comes up. */
int kshim_console_map_resources(struct kshim_resources *resources);
/* Service the USB console from an idle wait. Never waits for another CPU. */
void kshim_console_service(void);
