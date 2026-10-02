/* SPDX-License-Identifier: MIT */
#pragma once
#include <dt.h>
#include <Library/spmi.h>
#include <Library/cr_resources.h>
#include <framebuffer.h>
#include <kshim_mmu.h>
#define KSHIM_SPMI_MAX 4
#define KSHIM_RESOURCE_MAX 96
struct kshim_spmi_resource { int node; SpmiDeviceContext controller; };
struct kshim_touch_resource {
    int node;
    uint16_t address;
    uint8_t generation;
    uint32_t width, height;
    bool flip_x, flip_y;
    char rail_names[2][9];
    struct CrQupResources qup;
};
struct kshim_resources {
    const void *fdt;
    struct kshim_spmi_resource spmi[KSHIM_SPMI_MAX];
    unsigned spmi_count;
    struct kshim_touch_resource touch;
    kshim_framebuffer_config_t framebuffer;
    kshim_mmu_region_t mappings[KSHIM_RESOURCE_MAX];
    size_t mapping_count;
    int spmi_status, touch_status, splash_status;
};
int kshim_resources_init(const void *fdt, size_t available);
struct kshim_resources *kshim_resources_get(void);
int kshim_dt_spmi(const void *fdt, struct kshim_resources *resources);
int kshim_dt_spmi_identity(const void *fdt, int node, uint8_t *controller,
                           uint8_t *bus, uint8_t *sid);
int kshim_dt_touch(const void *fdt, struct kshim_touch_resource *touch);
int kshim_dt_gpio(const void *fdt, int provider, uint32_t pin,
                  uintptr_t *address, const char *function, uint32_t *mux);
int kshim_dt_splash(const void *fdt, const struct CrIo *io,
                    kshim_framebuffer_config_t *framebuffer,
                    kshim_mmu_region_t *mmio);
int kshim_resource_map(struct kshim_resources *r, uint64_t base, uint64_t size,
                       kshim_mmu_memory_type_t type, uint32_t permissions);

int kshim_dt_spmi_reg(const void *fdt, int node, uint16_t *offset);

unsigned kshim_dt_soc(const void *fdt, int provider);
