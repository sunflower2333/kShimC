/* SPDX-License-Identifier: MIT */
/*
 * Devicetree pinctrl states (kshim/pinctrl.h) for TLMM pads.
 *
 * Generic pin configuration properties (Linux pinctrl bindings,
 * pincfg-node.yaml / qcom,tlmm-common.yaml) map onto the TLMM CFG word:
 * function -> FUNC (mux slot from the SoC table: CrAndroidPin on
 * SM8150/SM8250, kernel/soc/sm8650_tables.c on SM8650), drive-strength ->
 * DRV ((mA / 2) - 1), bias-disable/-pull-down/-bus-hold/-pull-up -> PULL
 * 0/1/2/3, output-high/-low -> OE plus the IO OUT level, input-enable and
 * output-disable -> OE cleared, output-enable -> OE set. A state node may
 * carry the properties itself (mainline) or in its subnodes (mainline
 * "perst-pins", Android "mux"/"config"); each node changes only the fields
 * it names. Every pad goes through the GPIO registry, which snapshots it
 * before the first write and skips equal values.
 */
#include <kshim/pinctrl.h>
#include <kshim/soc_tables.h>
#include "internal.h"

#include <qcom_dt.h>
#include <stdio.h>
#include <string.h>

#define STATE_PINS 16U

static int provider_of(const void *f, int state)
{
    for (int n = fdt_parent_offset(f, state); n >= 0; n = fdt_parent_offset(f, n))
        if (fdt_getprop(f, n, "gpio-controller", NULL)) return n;
    return -1;
}

static bool has(const void *f, int node, const char *property)
{
    return fdt_getprop(f, node, property, NULL) != NULL;
}

/* Parse "gpioN" entries; pads without a GPIO number (sdc2_clk, ufs_reset)
 * are not supported. */
static int pins_of(const void *f, int node, uint32_t *pins, unsigned *count)
{
    int len;
    const char *p = fdt_getprop(f, node, "pins", &len);
    *count = 0;
    if (!p) return 0;
    for (const char *end = p + len; p < end; p += strlen(p) + 1) {
        if (strncmp(p, "gpio", 4) || !p[4]) return ERROR_NOT_SUPPORTED;
        uint32_t pin = 0;
        for (const char *d = p + 4; *d; ++d) {
            if (*d < '0' || *d > '9' || pin > 1000) return ERROR_NOT_SUPPORTED;
            pin = pin * 10 + (uint32_t)(*d - '0');
        }
        if (*count == STATE_PINS) return ERROR_NOT_SUPPORTED;
        pins[(*count)++] = pin;
    }
    return 0;
}

static int apply_node(const void *f, int provider, unsigned soc, int node)
{
    uint32_t pins[STATE_PINS], value = 0, mask = 0, drive;
    unsigned count;
    int out_level = -1, status = pins_of(f, node, pins, &count);
    if (status || !count) return status;
    if (has(f, node, "drive-strength")) {
        if (dt_u32(f, node, "drive-strength", &drive) || drive < 2 || drive > 16 || (drive & 1))
            return ERROR_INVALID_PARAMETER;
        mask |= KSHIM_TLMM_CFG_DRV_MASK;
        value |= ((drive / 2U) - 1U) << KSHIM_TLMM_CFG_DRV_SHIFT;
    }
    static const char *const bias[] = {"bias-disable", "bias-pull-down", "bias-bus-hold", "bias-pull-up"};
    for (uint32_t i = 0; i < 4; ++i) {
        if (!has(f, node, bias[i])) continue;
        mask |= KSHIM_TLMM_CFG_PULL_MASK;
        value = (value & ~KSHIM_TLMM_CFG_PULL_MASK) | i;
    }
    if (has(f, node, "output-high") || has(f, node, "output-low")) {
        mask |= KSHIM_TLMM_CFG_OE;
        value |= KSHIM_TLMM_CFG_OE;
        out_level = has(f, node, "output-high");
    } else if (has(f, node, "input-enable") || has(f, node, "output-disable")) {
        mask |= KSHIM_TLMM_CFG_OE;
    } else if (has(f, node, "output-enable")) {
        mask |= KSHIM_TLMM_CFG_OE;
        value |= KSHIM_TLMM_CFG_OE;
    }
    const char *function = dt_string(f, node, "function");
    for (unsigned i = 0; i < count; ++i) {
        uint32_t pin_value = value, pin_mask = mask, mux;
        if (function) {
            if (!kshim_soc_pin_function(soc, pins[i], function, &mux)) {
                printf("pinctrl: gpio%u has no function %s\n", (unsigned)pins[i], function);
                return ERROR_NOT_SUPPORTED;
            }
            pin_mask |= KSHIM_TLMM_CFG_FUNC_MASK;
            pin_value |= mux << KSHIM_TLMM_CFG_FUNC_SHIFT;
        }
        struct kshim_gpio g;
        status = kshim_gpio_from_tlmm_pin(f, provider, pins[i], false, &g);
        if (!status) status = kshim_gpio_pinctrl_apply(&g, pin_value, pin_mask, out_level);
        if (status) return status;
    }
    return 0;
}

int kshim_pinctrl_apply_state(const void *f, int state)
{
    if (!f || state < 0) return ERROR_INVALID_PARAMETER;
    int provider = provider_of(f, state);
    if (provider < 0) return ERROR_NOT_SUPPORTED;
    unsigned soc = kshim_gpio_tlmm_soc(f, provider);
    if (!soc) return ERROR_NOT_SUPPORTED; /* PMIC GPIO/MPP states stay inherited */
    int status = apply_node(f, provider, soc, state);
    for (int n = fdt_first_subnode(f, state); !status && n >= 0; n = fdt_next_subnode(f, n))
        status = apply_node(f, provider, soc, n);
    return status;
}

int kshim_pinctrl_apply(const void *f, int node, const char *name)
{
    if (!f || node < 0 || !name) return ERROR_INVALID_PARAMETER;
    int index = 0;
    if (has(f, node, "pinctrl-names")) {
        index = dt_string_index(f, node, "pinctrl-names", name);
        if (index < 0) return ERROR_NOT_FOUND;
    } else if (strcmp(name, "default")) {
        return ERROR_NOT_FOUND;
    }
    char property[24];
    snprintf(property, sizeof(property), "pinctrl-%d", index);
    int len;
    const fdt32_t *p = fdt_getprop(f, node, property, &len);
    if (!p) return ERROR_NOT_FOUND;
    if (len % 4) return ERROR_INVALID_PARAMETER;
    int result = 0;
    for (int i = 0; i < len / 4; ++i) {
        int state = dt_phandle(f, fdt32_to_cpu(p[i]));
        int status = state < 0 ? ERROR_INVALID_PARAMETER : kshim_pinctrl_apply_state(f, state);
        if (status && !result) result = status;
    }
    return result;
}
