/* SPDX-License-Identifier: MIT */
#include <qcom_dt.h>
#include <Library/cr_android.h>
#include <string.h>
unsigned kshim_dt_soc(const void *f, int node)
{
    if (!fdt_node_check_compatible(f, node, "qcom,gcc-sm8150-v2") ||
        !fdt_node_check_compatible(f, node, "qcom,sm8150-pinctrl") ||
        !fdt_node_check_compatible(f, node, "qcom,gcc-sm8150")) return 8150;
    if (!fdt_node_check_compatible(f, node, "qcom,kona-pinctrl") ||
        !fdt_node_check_compatible(f, node, "qcom,sm8250-tlmm") ||
        !fdt_node_check_compatible(f, node, "qcom,gcc-kona") ||
        !fdt_node_check_compatible(f, node, "qcom,gcc-sm8250")) return 8250;
    /* Mainline SM8650 (sm8650.dtsi): linear TLMM, 210 normal pads. */
    if (!fdt_node_check_compatible(f, node, "qcom,sm8650-tlmm") ||
        !fdt_node_check_compatible(f, node, "qcom,sm8650-gcc") ||
        !fdt_node_check_compatible(f, node, "qcom,sm8650-tcsr")) return 8650;
    return 0;
}
/* SM8650 TLMM: control at base + pin * 0x1000 for the normal pads 0..209
 * (pinctrl-sm8650 REG_SIZE, CrDK Target/Sm8650 CrGpioTarget.c). Only the
 * "gpio" function (mux 0) is resolved here; kshim/pinctrl.h owns the full
 * SM8650 function table (kernel/soc/). */
static int linear_gpio(const void *f, int provider, uint32_t pin, uintptr_t *address,
                       const char *function, uint32_t *mux)
{
    struct dt_range range;
    if (pin >= 210U || dt_reg(f, provider, 0, &range) ||
        !dt_contains(range, range.base + (uint64_t)pin * 0x1000U, 0x10) ||
        range.base + (uint64_t)pin * 0x1000U > UINTPTR_MAX) return -1;
    *address = (uintptr_t)(range.base + (uint64_t)pin * 0x1000U);
    if (!function) return 0;
    if (!mux || strcmp(function, "gpio")) return -1;
    *mux = 0;
    return 0;
}
int kshim_dt_gpio(const void *f, int provider, uint32_t pin,
                  uintptr_t *address, const char *function, uint32_t *mux)
{
    struct dt_range range;
    unsigned soc = kshim_dt_soc(f, provider);
    if (!address) return -1;
    if (soc == 8650) return linear_gpio(f, provider, pin, address, function, mux);
    const struct CrAndroidPin *layout = CrAndroidPin(soc, pin);
    if (!layout || !layout->functions[0] || dt_reg(f, provider, 0, &range) ||
        !dt_contains(range, range.base + layout->offset, 0x10)) return -1;
    *address = range.base + layout->offset;
    if (function) {
        if (!mux) return -1;
        for (unsigned i = 0; i < 10; ++i)
            if (layout->functions[i] && !strcmp(layout->functions[i], function)) { *mux = i; return 0; }
        return -1;
    }
    return 0;
}
