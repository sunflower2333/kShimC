/* SPDX-License-Identifier: MIT */
#pragma once
/*
 * Devicetree pin configuration (CONFIG_KSHIM_GPIO).
 * Owner: sm8650 (kernel/drivers/gpio/pinctrl.c). See docs/io-stack.md.
 *
 * Applies one "pinctrl-N" state of a consumer node. Each phandle is a state
 * node; the state node itself and each of its subnodes may carry:
 *   pins = "gpioN", ...           (TLMM pad names; "sdc2_clk"-style pads are
 *                                  ERROR_NOT_SUPPORTED)
 *   function = "gpio" | "<mux>"   (mux index from the SoC function table:
 *                                  CrAndroidPin on SM8150/SM8250, a kShimC
 *                                  table derived from pinctrl-sm8650.c facts
 *                                  on SM8650)
 *   drive-strength = <2..16>      (mA, even)
 *   bias-disable | bias-pull-down | bias-pull-up | bias-bus-hold
 *   output-high | output-low      (drive as output with that level)
 *   input-enable                  (output disabled)
 * The provider is the closest ancestor with "gpio-controller". PMIC GPIO
 * states (qcom,pmXXXX-gpio providers) return ERROR_NOT_SUPPORTED and are
 * left as inherited.
 *
 * Every pin touched is snapshot through the GPIO registry first; the
 * snapshot is restored by kshim_gpio_restore_all() at handoff. Applying the
 * value a pin already has writes nothing (inherited pads stay untouched).
 */
#include <stdbool.h>
#include <stdint.h>

#include "error_no.h"

/* Apply pinctrl state `name` ("default", ...) of `node`: its index in
 * "pinctrl-names", or pinctrl-0 when "pinctrl-names" is absent and name is
 * "default". ERROR_NOT_FOUND when the node has no such state (callers treat
 * that as "nothing to do"). Thread context. */
int kshim_pinctrl_apply(const void *fdt, int node, const char *name);

/* Apply one state node directly (a phandle target). */
int kshim_pinctrl_apply_state(const void *fdt, int state);
