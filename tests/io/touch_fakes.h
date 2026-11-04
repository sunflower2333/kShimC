/* SPDX-License-Identifier: MIT */
#pragma once
/*
 * Host fakes for the touch framework's dependencies until the sm8650 and qup
 * branches are merged: kshim/gpio.h (TLMM pins by number, DT-resolved),
 * kshim/pinctrl.h, kshim/regulator.h (one object per provider node),
 * kshim/i2c.h and kshim/spi.h (adapters probed from "qcom,geni-i2c" /
 * "qcom,i2c-geni" / "qcom,geni-spi" parents, transfers handed to a device
 * model), plus time and, with TOUCH_FAKE_TLMM, the TLMM interrupt API of
 * qcom_irq.h. Every side effect is appended to FakeLog in order.
 */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <kshim/gpio.h>
#include <kshim/i2c.h>
#include <kshim/io.h>
#include <kshim/pinctrl.h>
#include <kshim/regulator.h>
#include <kshim/spi.h>
#include <kshim/touch_driver.h>
#include <qcom_irq.h>

#ifndef TOUCH_FAKE_TLMM
#define TOUCH_FAKE_TLMM 1
#endif

/* ---- Log ------------------------------------------------------------------ */
extern char FakeLog[16384];
void fake_log(const char *format, ...) __attribute__((format(printf, 1, 2)));
void fake_log_clear(void);
bool fake_log_has(const char *needle);
/* Every needle occurs, each after the previous one. */
bool fake_log_order(const char *const *needles);
unsigned fake_log_count(const char *needle);

/* ---- Time ----------------------------------------------------------------- */
uint64_t fake_now_us(void);
void fake_advance_us(uint64_t us);

/* ---- GPIO ----------------------------------------------------------------- */
struct fake_pin {
    bool used;
    uint32_t pin;
    int refs;
    bool output, out_level;     /* physical */
    bool in_level;              /* physical level driven by the outside */
    bool snapshot_output, snapshot_level;
};
struct fake_pin *fake_pin(uint32_t pin);
void fake_pin_input(uint32_t pin, bool level);
bool fake_pin_level(uint32_t pin);
/* Called on every physical output change (device models watch reset). */
extern void (*FakePinHook)(uint32_t pin, bool level);
/* Overrides the input level source (real TLMM register model). */
extern bool (*FakePinRead)(const void *fdt, const struct kshim_gpio *gpio, bool *level);
extern int FakeGpioFailRequest;  /* pin number whose request fails, or -1 */

/* ---- Regulators ----------------------------------------------------------- */
struct fake_regulator {
    bool used;
    int node;
    char name[48];
    int refs, enables, disables;
};
struct fake_regulator *fake_regulator_named(const char *name);
extern const char *FakeRegulatorFail;   /* regulator-name whose enable fails */

/* ---- Buses ---------------------------------------------------------------- */
typedef int (*fake_i2c_fn)(uint16_t address, const uint8_t *tx, size_t tx_length,
                           uint8_t *rx, size_t rx_length);
extern fake_i2c_fn FakeI2cDevice;
extern int FakeI2cGetStatus;      /* returned by kshim_i2c_adapter_get when non-zero */
extern unsigned FakeI2cProbes, FakeI2cTransfers;
extern bool FakeBusForbidden;     /* assert no transfer (IRQ context checks) */
typedef int (*fake_spi_fn)(const struct kshim_spi_device *device,
                           const struct kshim_spi_xfer *xfers, size_t count);
extern fake_spi_fn FakeSpiDevice;

/* ---- TLMM interrupt (TOUCH_FAKE_TLMM) ------------------------------------- */
#if TOUCH_FAKE_TLMM
extern bool FakeIrqReady;
struct fake_tlmm_irq {
    bool requested, masked, sealed;
    uint32_t pin;
    unsigned trigger;
    kshim_gpio_irq_handler_t handler;
    void *context;
    unsigned requests, frees, masks, unmasks, delivered;
    int fail;
};
extern struct fake_tlmm_irq FakeTlmmIrq;
/* Deliver the interrupt if requested and unmasked; bus access asserts. */
bool fake_irq_fire(void);
#endif

/* Reset every fake (not the DT). */
void fake_touch_reset(void);
