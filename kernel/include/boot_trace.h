/* SPDX-License-Identifier: MIT */
#ifndef KSHIM_BOOT_TRACE_H
#define KSHIM_BOOT_TRACE_H
#include <config.h>

/* CONFIG_KSHIM_BOOT_TRACE: print "kShimC: <stage> +<ms>" relative to
 * kshim_main() entry. Compiles away otherwise. */
#if CONFIG_KSHIM_BOOT_TRACE
void kshim_boot_trace(const char *stage);
#else
static inline void kshim_boot_trace(const char *stage) { (void)stage; }
#endif

#endif
