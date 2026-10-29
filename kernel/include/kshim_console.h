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
