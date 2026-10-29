/* SPDX-License-Identifier: MIT */
#pragma once

/* Public kernel power API: capability-gated reset/power-off actions. */

#include <stdint.h>

typedef enum {
    KSHIM_POWER_NONE = 0, KSHIM_POWER_OFF, KSHIM_POWER_WARM,
    KSHIM_POWER_HARD, KSHIM_POWER_EDL
} kshim_power_action_t;
#define KSHIM_POWER_BIT(a) (1U << (unsigned)(a))
#define KSHIM_POWER_ALL 0x1eU

/* Probe the platform (read-only) and return a KSHIM_POWER_BIT() mask. */
uint32_t kshim_platform_power_init(const void *fdt);

/* Perform an action. Call kshim_platform_prepare_handoff() first; never call
 * from inside a UI callback. Returning at all means the action failed. */
int kshim_platform_power_execute(kshim_power_action_t action);
