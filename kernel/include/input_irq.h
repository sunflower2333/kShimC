/* SPDX-License-Identifier: MIT */
#pragma once
/*
 * Interrupt-driven input (CONFIG_KSHIM_INPUT_IRQ, docs/interrupts.md layer
 * 3). Handlers queue events in IRQ context; kshim_key_source_poll() and
 * kshim_input_touch_poll() keep their contracts and only drain the queues,
 * so event loops wake on the interrupt instead of a poll period. Anything
 * whose interrupt cannot be resolved stays on the polling path.
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

/* Single-producer (IRQ) / single-consumer (poll) touch event queue. A move
 * replaces a queued move of the same contact; when full, a move is dropped
 * and a press/release replaces a queued trailing move. */
#define KSHIM_TOUCH_QUEUE_DEPTH 32U
struct kshim_touch_queue {
    kshim_touch_event_t events[KSHIM_TOUCH_QUEUE_DEPTH];
    unsigned head, count, dropped;
};
void kshim_touch_queue_push(struct kshim_touch_queue *queue, const kshim_touch_event_t *event);
bool kshim_touch_queue_pop(struct kshim_touch_queue *queue, kshim_touch_event_t *event);
void kshim_touch_queue_reset(struct kshim_touch_queue *queue);

/* TLMM pin and trigger of a touch controller's `interrupts` entry. The pin
 * must be the controller IRQ GPIO kShimC power-sequences (control register
 * irq_control); a NONE type falls back to the GPIO's active level. */
int kshim_touch_irq_resolve(const void *fdt, int node, uintptr_t irq_control,
                            bool active_low, uint32_t *pin, unsigned *trigger);
