/* SPDX-License-Identifier: MIT */
#pragma once
/*
 * Interrupt-driven keys (CONFIG_KSHIM_INPUT_IRQ, docs/interrupts.md layer
 * 3). Handlers sample key levels in IRQ context; kshim_key_source_poll()
 * keeps its contract, so event loops wake on the interrupt instead of a poll
 * period. Anything whose interrupt cannot be resolved stays on the polling
 * path. Touch controllers use the touch framework's deferred interrupt
 * (kshim/touch_driver.h).
 */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <kshim/input.h>
#include <qcom_keys.h>

/* Quiet window after an accepted key edge. Matches the PON debounce
 * (qcom,pon-dbc-delay 15625 us) and gpio-keys debounce-interval = <15> on
 * the SM8150 boards; a longer DT debounce-interval of an interrupt-driven
 * TLMM gpio-keys child raises it. */
#define KSHIM_KEYS_IRQ_DEBOUNCE_MS 15U

/* Request the interrupt of every descriptor that resolves from the DT: PON
 * and PMIC GPIO keys (4-cell SPMI specifier on the arbiter) through the SPMI
 * cascade, TLMM gpio-keys through the TLMM cascade, both edges. Returns the
 * number of interrupt-driven descriptors; the others stay polled. Before
 * kshim_irq_ready() it returns 0 and the first poll after retries. */
int kshim_keys_irq_start(QcomKeys *keys, const void *fdt);
/* Free every key interrupt; the descriptors are polled again. */
int kshim_keys_irq_stop(QcomKeys *keys);
