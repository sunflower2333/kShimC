/* SPDX-License-Identifier: MIT */
#pragma once
/*
 * Private glue shared by kernel/drivers/gpio (GPIO, pinctrl),
 * kernel/drivers/clk and kernel/drivers/regulator, and their host
 * register-model tests (tests/io/platform_*). Not a public interface.
 */
#include <kshim/gpio.h>
#include <Library/cr_geni.h>
#include <Library/pmic_gpio.h>

/* MMIO used by the platform drivers: CrIoDefault() unless a host register
 * model is injected. NULL resets to the default. */
void kshim_platform_set_io(const struct CrIo *io);
const struct CrIo *kshim_platform_io(void);

static inline uint32_t kshim_platform_read(uintptr_t address)
{
    const struct CrIo *io = kshim_platform_io();
    return io->read32(io->context, address);
}
static inline void kshim_platform_write(uintptr_t address, uint32_t value)
{
    const struct CrIo *io = kshim_platform_io();
    io->write32(io->context, address, value);
}

/* PMIC GPIO register transport (one byte at a time). The default goes over
 * the SPMI arbiter transport of kernel/drivers/qcom_keys.c; the controller
 * index is passed as the bus context (uintptr_t). */
void kshim_gpio_set_pmic_bus(const PmicGpioBusOps *bus);

/* Pinctrl entry into the registry: snapshot `gpio` (taking no consumer
 * reference) and apply a full TLMM CFG value plus, when out_level >= 0, the
 * output level first. Equal values are not written. The snapshot is kept
 * until kshim_gpio_restore_all(). */
int kshim_gpio_pinctrl_apply(struct kshim_gpio *gpio, uint32_t cfg, uint32_t cfg_mask,
                             int out_level);

/* TLMM provider facts for pinctrl: SoC number (kshim_dt_soc) and whether
 * `pin` is reserved by the provider's gpio-reserved-ranges. */
unsigned kshim_gpio_tlmm_soc(const void *fdt, int provider);
bool kshim_gpio_pin_reserved(const void *fdt, int provider, uint32_t pin);

/* Registry introspection for tests and logs. */
unsigned kshim_gpio_snapshot_count(void);
