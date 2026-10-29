/* SPDX-License-Identifier: MIT */
#pragma once

/*
 * Upcalls: entry points implemented by userspace (user/) and called by the
 * kernel. This is the only way the kernel reaches userspace code.
 */

#include <stddef.h>
#include <stdint.h>

/* Let the user pick one of count boot entries. Names must be in
 * runtime-owned memory. Returns 0 with *selected set, -1 on invalid
 * arguments, or -2 on unsafe UI shutdown (the caller must not boot). On
 * return the platform has been prepared for handoff. */
int kshim_user_select(uint64_t fdt, const char *const *names, size_t count,
                      size_t initial, uint32_t timeout_ms, size_t *selected);

/* Standalone main loop (no Manifest): offer the configured entries and
 * hand the choice to kshim_menu_activate(). Does not return. */
int kshim_user_main(uint64_t fdt);

/* Release userspace resources before handoff (LVGL and its workers).
 * Called by kshim_platform_prepare_handoff(); non-zero aborts the handoff. */
int kshim_user_prepare_handoff(void);

/* LVGL stage of the QEMU SMP self-test (CONFIG_KSHIM_SMP_SELFTEST), run
 * while worker_count PSCI workers are online. 0 on success. */
int kshim_user_smp_selftest(size_t worker_count);
