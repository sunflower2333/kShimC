/* SPDX-License-Identifier: MIT */
#pragma once
/*
 * GPIO consumer API for TLMM and SPMI PMIC GPIOs (CONFIG_KSHIM_GPIO).
 * Owner: sm8650 (kernel/drivers/gpio/). See docs/io-stack.md.
 *
 * Interrupts on TLMM pins stay with the cascade in qcom_irq.h
 * (kshim_gpio_irq_request(pin, trigger, handler, ctx)); this API only
 * resolves the pin number for it (kshim_gpio_tlmm_pin). PMIC GPIO
 * interrupts are not offered here.
 *
 * Register facts (TLMM, per pin; pinctrl-msm layout, confirmed by CrDK
 * GpioLib RegCfg[0] for SM8150/SM8250/SM8450/SM8650):
 *   CFG  @ +0: PULL [1:0] (0 none, 1 down, 2 keeper, 3 up), FUNC [5:2],
 *              DRV [8:6] ((mA / 2) - 1), OE bit 9
 *   IO   @ +4: IN bit 0, OUT bit 1
 * The pin's control address comes from the SoC layout: CrDK Android tables
 * (CrAndroidPin) on SM8150/SM8250 (kshim_dt_gpio), base + pin * 0x1000 on
 * SM8450/SM8650.
 *
 * Every pin is snapshot (CFG and IO for TLMM; MODE_CTL, DIG_VIN_CTL,
 * DIG_PULL_CTL, DIG_OUT_SRC_CTL, DIG_OUT_CTL, EN_CTL for PMIC GPIO) before
 * kShimC first writes it, whether through this API or kshim_pinctrl_apply().
 * kshim_gpio_restore_all() writes the snapshots back in reverse order.
 */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "error_no.h"

enum kshim_gpio_chip {
    KSHIM_GPIO_CHIP_NONE = 0,
    KSHIM_GPIO_CHIP_TLMM,
    KSHIM_GPIO_CHIP_PMIC,
};

/* Devicetree GPIO flags (dt-bindings/gpio/gpio.h). */
#define KSHIM_GPIO_DT_ACTIVE_LOW  0x1U
#define KSHIM_GPIO_DT_SINGLE_ENDED 0x2U
#define KSHIM_GPIO_DT_OPEN_DRAIN  0x4U
#define KSHIM_GPIO_DT_PULL_UP     0x10U
#define KSHIM_GPIO_DT_PULL_DOWN   0x20U

enum kshim_gpio_pull {
    KSHIM_GPIO_PULL_NONE = 0,
    KSHIM_GPIO_PULL_DOWN = 1,
    KSHIM_GPIO_PULL_KEEPER = 2,
    KSHIM_GPIO_PULL_UP = 3,
    KSHIM_GPIO_PULL_KEEP = 0xff, /* leave the current pull */
};

/* A resolved GPIO descriptor; plain data, owned by the consumer. */
struct kshim_gpio {
    enum kshim_gpio_chip chip;
    int provider;          /* DT node of the gpio-controller */
    uint32_t pin;          /* TLMM: pin number; PMIC: DT number (1-based) */
    uint32_t dt_flags;     /* KSHIM_GPIO_DT_* from the specifier */
    bool active_low;       /* logical value = !physical level */
    uintptr_t ctl;         /* TLMM: CFG register address (IO at ctl + 4) */
    uint8_t spmi_controller; /* PMIC: arbiter index as kshim_dt_spmi() numbers it */
    uint8_t sid;           /* PMIC: slave ID */
    uint16_t base;         /* PMIC: peripheral base address (for example 0x8800 + (n-1)*0x100) */
    int slot;              /* registry slot while requested, -1 otherwise */
};

/* Bind the TLMM and PMIC GPIO providers of the runtime FDT. Idempotent.
 * kshim_io_init() calls it first. */
int kshim_gpio_init(const void *fdt);

/* Map every TLMM region (whole "reg" 0, device, RW). kshim_io_map_resources. */
struct kshim_resources;
int kshim_gpio_map_resources(struct kshim_resources *resources);

/* Resolve entry `index` of `property` ("reset-gpios", "irq-gpios",
 * "perst-gpios", "gpios", "gpio", legacy Android names such as
 * "fts,reset-gpio"). No MMIO. ERROR_NOT_FOUND when the property is absent,
 * ERROR_NOT_SUPPORTED for an unknown controller. gpio->slot is set to -1. */
int kshim_gpio_from_dt(const void *fdt, int node, const char *property,
                       unsigned index, struct kshim_gpio *gpio);
/* Descriptor for TLMM pin `pin` of TLMM node `tlmm` (for an interrupt
 * specifier <&tlmm 162 IRQ_TYPE_LEVEL_LOW> used as the IRQ GPIO). */
int kshim_gpio_from_tlmm_pin(const void *fdt, int tlmm, uint32_t pin, bool active_low,
                             struct kshim_gpio *gpio);

/* Take a reference on the pin for `owner` (static string, for logs),
 * snapshotting it on the first reference. ERROR_BUSY when another owner
 * already drives it as an output. Thread context. */
int kshim_gpio_request(struct kshim_gpio *gpio, const char *owner);
/* GPIO function (mux 0), output enabled, logical value `active` (the level
 * is written before OE is set, so there is no glitch). */
int kshim_gpio_direction_output(struct kshim_gpio *gpio, bool active);
/* GPIO function, output disabled, pull applied unless KSHIM_GPIO_PULL_KEEP. */
int kshim_gpio_direction_input(struct kshim_gpio *gpio, enum kshim_gpio_pull pull);
/* Set the logical output value of a requested pin. IRQ-safe for TLMM. */
int kshim_gpio_set(struct kshim_gpio *gpio, bool active);
/* Read the logical input value (TLMM IN bit / PMIC STATUS). IRQ-safe for
 * TLMM; does not require a request. */
int kshim_gpio_get(const struct kshim_gpio *gpio, bool *active);
/* Drop the reference; the last one restores the snapshot. */
int kshim_gpio_free(struct kshim_gpio *gpio);

/* TLMM pin number for kshim_gpio_irq_request(); ERROR_NOT_SUPPORTED for a
 * PMIC GPIO. */
int kshim_gpio_tlmm_pin(const struct kshim_gpio *gpio, uint32_t *pin);

/* Handoff (kshim_io_quiesce, last step): restore every snapshot taken by
 * this API and by kshim_pinctrl_apply(), newest first, including pins whose
 * consumers never freed them. Idempotent. */
int kshim_gpio_restore_all(void);
