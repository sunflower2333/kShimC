/* SPDX-License-Identifier: MIT */
#pragma once

/* Public kernel platform lifecycle API. */

/* Prepare the boot CPU for heavy rendering: start the configured PSCI
 * workers (CONFIG_KSHIM_SMP) and enable the MMU and caches. */
void kshim_platform_accelerate(const void *fdt);

/* Quiesce touch, tear down userspace (kshim_user_prepare_handoff()), park
 * the SMP workers, restore the inherited interrupt state, then turn the MMU
 * off. Must succeed before any payload handoff or power action; non-zero
 * means the caller must not boot. */
int kshim_platform_prepare_handoff(void);
