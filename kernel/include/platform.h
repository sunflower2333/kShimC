/* SPDX-License-Identifier: MIT */
#pragma once
#include <stdint.h>
#include <stddef.h>

/* kshim_platform_accelerate() and kshim_platform_prepare_handoff() are public. */
#include <kshim/platform.h>

/* Standalone/no-Manifest entry: run the self-test if configured, then
 * hand over to kshim_user_main(). */
int kshim_platform_main(uint64_t fdt, uint64_t arg1, uint64_t arg2, uint64_t arg3);

/* Handoff steps 1-2: quiesce touch, tear down userspace (LVGL) and stop the
 * SMP workers. kshim_platform_prepare_handoff() runs this, then restores the
 * interrupt state and turns the MMU off. Idempotent. */
int kshim_platform_quiesce(void);
/* Quiesce the touch controller before handoff (kernel/core/input.c). */
int kshim_input_touch_stop(void);
