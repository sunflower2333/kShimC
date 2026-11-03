/* SPDX-License-Identifier: MIT */
#pragma once
#include <stdint.h>
#include <stddef.h>

/* kshim_platform_accelerate() and kshim_platform_prepare_handoff() are public. */
#include <kshim/platform.h>

/* Standalone/no-Manifest entry: run the self-test if configured, then
 * hand over to kshim_user_main(). */
int kshim_platform_main(uint64_t fdt, uint64_t arg1, uint64_t arg2, uint64_t arg3);

/* Handoff steps 1-3: quiesce input/touch/console RX interrupts, tear down
 * userspace (LVGL), stop the SMP workers and restore the Qualcomm cascades
 * (TLMM, SPMI, PDC). kshim_platform_prepare_handoff() runs this, then
 * restores the interrupt state and turns the MMU off. Idempotent. */
int kshim_platform_quiesce(void);
/* Quiesce the touch controller before handoff (kernel/core/input.c). */
int kshim_input_touch_stop(void);
/* Handoff step 1 (docs/interrupts.md): stop touch and every input interrupt
 * (keys; touch) so no handler runs while the UI and controllers go down. */
int kshim_input_quiesce(void);
