/* SPDX-License-Identifier: MIT */
#include <qcom_dt.h>
#include <power.h>
#include <config.h>
#include <qcom_keys_fdt.h>
#include <qcom_key_platform.h>
#include <kshim_console.h>
#include <string.h>
#include <error_no.h>
#if defined(CONFIG_BLK_UFS) || defined(CONFIG_BLK_SDHCI)
#include <kshim/storage_probe.h>
#endif
#include <kshim/io.h>
#include <boot_trace.h>
#if CONFIG_QCOM_IRQCHIP
#include <qcom_irq.h>
/* The TLMM cascade writes INTR_CFG/INTR_STATUS on the key pin's page. */
#define KEY_GPIO_PERMISSIONS (KSHIM_MMU_READ | KSHIM_MMU_WRITE)
#else
#define KEY_GPIO_PERMISSIONS KSHIM_MMU_READ
#endif
static struct kshim_resources resources;
static bool initialized;
struct kshim_resources *kshim_resources_get(void) { return &resources; }
int kshim_resource_map(struct kshim_resources *r, uint64_t base, uint64_t size,
                       kshim_mmu_memory_type_t type, uint32_t permissions)
{
    if (!base || !size || base > UINT64_MAX - size) return -1;
    /* Coalesce device pages before page-table construction. A GPIO input and
     * a pinmux register on the same page must have one memory type/permission. */
    if (type == KSHIM_MMU_DEVICE) {
        if (base + size > UINT64_MAX - 4095) return -1;
        uint64_t end = (base + size + 4095) & ~UINT64_C(4095);
        base &= ~UINT64_C(4095); size = end - base;
    }
    for (size_t i = 0; i < r->mapping_count;) {
        kshim_mmu_region_t *m = &r->mappings[i];
        if (m->type == type && type == KSHIM_MMU_DEVICE &&
            base < m->base + m->size && m->base < base + size) {
            uint64_t end = base + size > m->base + m->size ? base + size : m->base + m->size;
            if (m->base < base) base = m->base;
            size = end - base; permissions |= m->permissions;
            r->mappings[i] = r->mappings[--r->mapping_count];
            i = 0;
            continue;
        }
        if (m->type == type && m->permissions == permissions &&
            dt_contains((struct dt_range){m->base, m->size}, base, size)) return 0;
        ++i;
    }
    if (r->mapping_count == KSHIM_RESOURCE_MAX) return -1;
    r->mappings[r->mapping_count++] = (kshim_mmu_region_t){base, size, type, permissions};
    return 0;
}
#if CONFIG_KSHIM_IRQ
/* The GIC is optional: without it the runtime polls. A failed probe leaves
 * no partial mapping behind. */
static int gic_maps(struct kshim_resources *r)
{
    int status = kshim_dt_gic(r->fdt, &r->gic);
    if (status) return status;
    if (kshim_resource_map(r, r->gic.dist.base, r->gic.dist.size, KSHIM_MMU_DEVICE,
                           KSHIM_MMU_READ | KSHIM_MMU_WRITE)) return ERROR_NOT_SUPPORTED;
    for (unsigned i = 0; i < r->gic.redist_count; ++i)
        if (kshim_resource_map(r, r->gic.redist[i].base, r->gic.redist[i].size, KSHIM_MMU_DEVICE,
                               KSHIM_MMU_READ | KSHIM_MMU_WRITE)) return ERROR_NOT_SUPPORTED;
    return 0;
}
#endif
#if CONFIG_KSHIM_FRAMEBUFFER
/* The display reservation past the scanout frame is owned by the display and
 * unused while kShimC runs; the UI keeps its inflated font and background
 * textures there instead of in the runtime slot. */
static void ui_scratch_from_splash(const void *f, struct kshim_resources *r)
{
    struct dt_range reservation;
    uint64_t start = r->framebuffer.render_address + r->framebuffer.buffer_size;
    if (kshim_dt_splash_reservation(f, &reservation, NULL) ||
        start < r->framebuffer.render_address || start > UINT64_MAX - 4095) return;
    start = (start + 4095) & ~UINT64_C(4095);
    uint64_t end = reservation.base + reservation.size;
    if (start >= reservation.base && start < end)
        r->ui_scratch = (struct dt_range){start, end - start};
}
/* Write-back unless a no-map reservation covers it: those may only be
 * mapped non-cacheable. */
static kshim_mmu_memory_type_t ui_scratch_type(const void *f, struct dt_range scratch)
{
    struct dt_range ranges[128]; bool no_map[128]; size_t count;
    if (dt_reservations(f, ranges, no_map, 128, &count)) return KSHIM_MMU_NORMAL_NC;
    for (size_t i = 0; i < count; ++i)
        if (no_map[i] && scratch.base < ranges[i].base + ranges[i].size &&
            ranges[i].base < scratch.base + scratch.size) return KSHIM_MMU_NORMAL_NC;
    return KSHIM_MMU_NORMAL_WB;
}
#endif
int kshim_resources_init(const void *f, size_t available)
{
    if (dt_validate(f, available)) return -1;
    if (initialized && resources.fdt == f) return 0;
    initialized = false;
    memset(&resources, 0, sizeof(resources));
    resources.fdt = f;
    resources.spmi_status = resources.splash_status = -1;
    resources.gic_status = ERROR_NOT_SUPPORTED;
#if CONFIG_QCOM_SPMI
    resources.spmi_status = kshim_dt_spmi(f, &resources);
    SpmiDeviceContext *contexts[KSHIM_SPMI_MAX];
    unsigned count = resources.spmi_status ? 0 : resources.spmi_count;
    for (unsigned i = 0; i < count; ++i) {
        contexts[i] = &resources.spmi[i].controller;
        for (unsigned j = 0; j < SPMI_MEMORY_REGION_TYPE_MAX; ++j) {
            SpmiMemoryRegion *m = &contexts[i]->Regions[j];
            if (m->Size && kshim_resource_map(&resources, m->BaseAddress, m->Size,
                    KSHIM_MMU_DEVICE, KSHIM_MMU_READ | KSHIM_MMU_WRITE)) return -1;
        }
    }
    kshim_qcom_spmi_bind(contexts, count);
    kshim_boot_trace("  spmi");
#endif
#if CONFIG_QCOM_KEYS
    QcomKeyDescriptor keys[QCOM_KEYS_MAX]; size_t key_count;
    if (!QcomKeysBuildFromDeviceTree(f, keys, QCOM_KEYS_MAX, &key_count)) {
        for (size_t i = 0; i < key_count; ++i)
            if (!QCOM_KEY_IS_SPMI_TOKEN(keys[i].Base) &&
                kshim_resource_map(&resources, keys[i].Base + keys[i].RegisterOffset,
                    4, KSHIM_MMU_DEVICE, KEY_GPIO_PERMISSIONS)) return -1;
    }
    kshim_boot_trace("  keys");
#endif
    /* Touch controllers (CONFIG_KSHIM_TOUCH_FRAMEWORK) use the GPIO, QUP and
     * regulator windows that kshim_io_map_resources() adds below. */
#if CONFIG_KSHIM_FRAMEBUFFER
    kshim_mmu_region_t display;
    resources.splash_status = kshim_dt_splash(f, CrIoDefault(), &resources.framebuffer, &display);
    if (!resources.splash_status && kshim_resource_map(&resources, display.base, display.size,
            display.type, display.permissions)) return -1;
    if (!resources.splash_status) ui_scratch_from_splash(f, &resources);
#if CONFIG_KSHIM_UI_SCRATCH_SIZE
    if (!resources.ui_scratch.size)
        resources.ui_scratch = (struct dt_range){CONFIG_KSHIM_UI_SCRATCH_BASE,
                                                 CONFIG_KSHIM_UI_SCRATCH_SIZE};
#endif
    if (resources.ui_scratch.size && kshim_resource_map(&resources,
            resources.ui_scratch.base, resources.ui_scratch.size,
            ui_scratch_type(f, resources.ui_scratch), KSHIM_MMU_READ | KSHIM_MMU_WRITE))
        resources.ui_scratch = (struct dt_range){0};
    kshim_boot_trace("  splash");
#endif
#if CONFIG_QCOM_IRQCHIP
    kshim_irqchip_map_resources(&resources); /* optional PDC routes */
    kshim_boot_trace("  irqchip");
#endif
    if (kshim_power_map_resources(&resources)) return -1;
    kshim_boot_trace("  power");
#if CONFIG_KSHIM_CONSOLE_USB_ACM
    (void)kshim_console_map_resources(&resources); /* optional; stops the USB console on failure */
#endif
#if defined(CONFIG_BLK_UFS) || defined(CONFIG_BLK_SDHCI)
    (void)kshim_storage_map_resources(&resources); /* optional; restores on failure */
    kshim_boot_trace("  storage");
#endif
    /* I/O stack windows (docs/io-stack.md); each subsystem is optional and
     * rolled back on its own failure. No-op when not configured. */
    (void)kshim_io_map_resources(&resources);
    kshim_boot_trace("  io");
#if CONFIG_KSHIM_IRQ
    /* Last, so a full table costs interrupts, never a mandatory region. */
    kshim_mmu_region_t saved[KSHIM_RESOURCE_MAX];
    size_t saved_count = resources.mapping_count;
    memcpy(saved, resources.mappings, saved_count * sizeof(*saved));
    resources.gic_status = gic_maps(&resources);
    if (resources.gic_status) {
        memcpy(resources.mappings, saved, saved_count * sizeof(*saved));
        resources.mapping_count = saved_count;
    }
#endif
    initialized = true;
    return 0;
}
