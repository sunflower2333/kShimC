/* SPDX-License-Identifier: MIT */
#pragma once

/* Public kernel boot-handoff API. */

typedef enum {
  KSHIM_MENU_NONE = 0,
  KSHIM_MENU_BOOT,
  KSHIM_MENU_RECOVERY,
  KSHIM_MENU_POWER_OFF,
} kshim_menu_action_t;

/* Board hook called by the main loop. Return zero if the action was accepted.
 * The weak default reports unavailable until a board supplies a boot/power
 * handler; menu confirmation never pretends to perform a missing action. */
int kshim_menu_activate(kshim_menu_action_t Action);
