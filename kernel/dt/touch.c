/* SPDX-License-Identifier: MIT */
/*
 * Android touch board selection for the touch framework's ST drivers
 * (kshim/touch_driver.h): qcom,i2c-touch-active on the bus and the panel
 * list against the active DSI display.
 */
#include <dt.h>
#include <kshim/touch_driver.h>
#include <string.h>

static bool selected_panel(const void *f, int node)
{
    int length;
    const fdt32_t *panels = fdt_getprop(f, node, "panel", &length);
    if (!panels) return length == -FDT_ERR_NOTFOUND;
    if (length <= 0 || length % 4) return false;
    for (int n = fdt_next_node(f, -1, NULL); n >= 0; n = fdt_next_node(f, n, NULL)) {
        bool active;
        if (dt_bool(f, n, "qcom,dsi-display-active", &active)) return false;
        if (!active || !dt_available(f, n)) continue;
        int panel = dt_reference(f, n, "qcom,dsi-panel", 0);
        for (int i = 0; i < length / 4; ++i)
            if (panel >= 0 && panel == dt_phandle(f, fdt32_to_cpu(panels[i]))) return true;
    }
    return false;
}
bool kshim_touch_dt_selected(const void *f, int node, const char *compatible)
{
    if (!f || node < 0) return false;
    int bus = dt_parent_offset(f, node);
    /* Android QRD/HDK DTs name the one populated controller on the bus; an
     * empty selection (stock kona-hdk.dtsi) selects none. */
    if (bus >= 0 && fdt_getprop(f, bus, "qcom,i2c-touch-active", NULL)) {
        const char *selection = dt_string(f, bus, "qcom,i2c-touch-active");
        if (!selection || !compatible || strcmp(selection, compatible)) return false;
    }
    return selected_panel(f, node);
}
