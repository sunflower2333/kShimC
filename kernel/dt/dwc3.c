/* SPDX-License-Identifier: MIT */
/* Device-tree discovery of the DWC3 core used by the USB CDC-ACM console. */
#include <dt.h>
#include <usb_acm.h>
#include <Library/UsbAcmCrLib.h>
#include <string.h>

static bool qcom_wrapper(const void *f, int node)
{
    return !fdt_node_check_compatible(f, node, "qcom,dwc3") ||
           !fdt_node_check_compatible(f, node, "qcom,dwc-usb3-msm");
}

/* androidboot.usbcontroller=a600000.dwc3 -> 0xa600000, else 0. */
static uint64_t controller_hint(const void *f)
{
    static const char key[] = "androidboot.usbcontroller=";
    int chosen = fdt_path_offset(f, "/chosen");
    const char *args = chosen >= 0 ? dt_string(f, chosen, "bootargs") : NULL;
    for (const char *p = args; p && *p;) {
        if (!strncmp(p, key, sizeof(key) - 1)) {
            uint64_t value = 0;
            unsigned digits = 0;
            for (p += sizeof(key) - 1; *p && *p != '.' && *p != ' '; ++p, ++digits) {
                unsigned d = (*p >= '0' && *p <= '9') ? (unsigned)(*p - '0') :
                             (*p >= 'a' && *p <= 'f') ? (unsigned)(*p - 'a' + 10) :
                             (*p >= 'A' && *p <= 'F') ? (unsigned)(*p - 'A' + 10) : 16;
                if (d > 15 || digits >= 16) return 0;
                value = value * 16 + d;
            }
            return *p == '.' ? value : 0;
        }
        while (*p && *p != ' ') ++p;
        while (*p == ' ') ++p;
    }
    return 0;
}

static int interrupt_parent(const void *f, int node)
{
    for (int n = node; n >= 0; n = dt_parent_offset(f, n)) {
        uint32_t handle;
        if (fdt_getprop(f, n, "interrupt-parent", NULL))
            return dt_u32(f, n, "interrupt-parent", &handle) ? -1 : dt_phandle(f, handle);
    }
    return -1;
}

/* The first (or "dwc_usb3"/"peripheral") interrupt of the core. */
static void interrupt(const void *f, int node, struct kshim_usb_acm_resource *r)
{
    int length, parent;
    uint32_t cells;
    const fdt32_t *p = fdt_getprop(f, node, "interrupts-extended", &length);
    if (p && length >= 8) {
        parent = dt_phandle(f, fdt32_to_cpu(p[0]));
        if (parent < 0 || dt_u32(f, parent, "#interrupt-cells", &cells) || !cells ||
            cells > 4 || (unsigned)length / 4 < 1 + cells) return;
        ++p;
    } else {
        p = fdt_getprop(f, node, "interrupts", &length);
        parent = interrupt_parent(f, node);
        if (!p || parent < 0 || dt_u32(f, parent, "#interrupt-cells", &cells) || !cells ||
            cells > 4 || (unsigned)length / 4 < cells) return;
        int index = dt_string_index(f, node, "interrupt-names", "dwc_usb3");
        if (index < 0) index = dt_string_index(f, node, "interrupt-names", "peripheral");
        if (index > 0 && (unsigned)length / 4 >= (index + 1) * cells) p += index * cells;
    }
    r->has_irq = true;
    r->irq_parent = parent;
    r->irq_cells = cells;
    for (uint32_t i = 0; i < cells; ++i) r->irq_spec[i] = fdt32_to_cpu(p[i]);
    /* Android's 3-cell Qualcomm PDC sits in front of the GIC with GIC-style
     * specifiers; SPIs it has no pin for pass straight through. */
    r->pdc = cells == 3 && !strncmp(dt_string(f, parent, "compatible") ?
                                    dt_string(f, parent, "compatible") : "", "qcom,pdc", 8);
    r->gic = cells >= 3 && r->irq_spec[0] <= 1 &&
             (r->pdc || !fdt_node_check_compatible(f, parent, "arm,gic-v3") ||
              !fdt_node_check_compatible(f, parent, "arm,gic-400") ||
              !fdt_node_check_compatible(f, parent, "arm,cortex-a15-gic"));
    if (r->gic) {
        r->intid = (r->irq_spec[0] ? 16U : 32U) + r->irq_spec[1];
        r->trigger = r->irq_spec[2] & 0xF;
    }
}

int kshim_dt_usb_acm(const void *f, struct kshim_usb_acm_resource *out)
{
    if (!f || !out) return -1;
    memset(out, 0, sizeof(*out));
    out->node = out->irq_parent = -1;
    uint64_t hint = controller_hint(f);
    int chosen = -1;
    struct dt_range chosen_reg = {0, 0};
    for (int n = fdt_node_offset_by_compatible(f, -1, "snps,dwc3"); n >= 0;
         n = fdt_node_offset_by_compatible(f, n, "snps,dwc3")) {
        int parent = dt_parent_offset(f, n);
        if (parent < 0 || !qcom_wrapper(f, parent) || !dt_available(f, n)) continue;
        const char *mode = dt_string(f, n, "dr_mode");
        if (!mode) mode = dt_string(f, parent, "dr_mode");
        if (mode && !strcmp(mode, "host")) continue;
        struct dt_range reg;
        if (dt_reg(f, n, 0, &reg) || reg.size < CR_USB_ACM_DWC3_WINDOW_BYTES ||
            reg.base > UINTPTR_MAX - reg.size) continue;
        if (chosen < 0 || (hint && reg.base == hint && chosen_reg.base != hint)) {
            chosen = n;
            chosen_reg = reg;
        }
    }
    if (chosen < 0) return -1;
    out->node = chosen;
    out->base = chosen_reg.base;
    out->size = chosen_reg.size;
    interrupt(f, chosen, out);
    return 0;
}
