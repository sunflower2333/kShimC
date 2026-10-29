/* SPDX-License-Identifier: MIT */
#pragma once

/*
 * Public kernel input API: menu keys and touch, independent of the Qualcomm
 * PON/GPIO key driver and the FTM5/QUP touch driver behind them.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* ---- Keys ---------------------------------------------------------------- */

typedef enum {
    KSHIM_KEY_ACTION_NONE = 0,
    KSHIM_KEY_ACTION_UP,
    KSHIM_KEY_ACTION_DOWN,
    KSHIM_KEY_ACTION_SELECT,
} kshim_key_action_t;

typedef enum {
    KSHIM_KEY_EVENT_NONE = 0,
    KSHIM_KEY_EVENT_PRESS,
    KSHIM_KEY_EVENT_RELEASE,
} kshim_key_event_type_t;

typedef struct {
    kshim_key_event_type_t type;
    kshim_key_action_t action;
    uint32_t code;      /* Linux input code (KEY_VOLUMEUP, ...). */
    uint8_t pressed;
} kshim_key_event_t;

/* kshim_key_source_poll() results; negative values are errors. */
#define KSHIM_KEYS_NO_EVENT 0
#define KSHIM_KEYS_EVENT    1

/* Opaque, kernel-owned debounced key source. */
typedef struct kshim_key_source kshim_key_source_t;

/* Return KSHIM_KEYS_EVENT and fill *event, KSHIM_KEYS_NO_EVENT, or a negative
 * error (also for an unconfigured source). *event is written only for
 * KSHIM_KEYS_EVENT. */
int kshim_key_source_poll(kshim_key_source_t *source, kshim_key_event_t *event);

/* (Re)build the platform keys from the boot FDT; 0 on success. The source
 * returned by kshim_input_keys() is valid either way and reports errors
 * when no key could be configured. */
int kshim_input_keys_init(const void *fdt);
kshim_key_source_t *kshim_input_keys(void);

/* ---- Touch --------------------------------------------------------------- */

typedef enum kshim_touch_event_type {
    KSHIM_TOUCH_EVENT_PRESS = 0,
    KSHIM_TOUCH_EVENT_MOVE,
    KSHIM_TOUCH_EVENT_RELEASE,
    /* Discard a contact after an error/shutdown without completing a click. */
    KSHIM_TOUCH_EVENT_CANCEL,
} kshim_touch_event_type_t;

typedef struct kshim_touch_event {
    kshim_touch_event_type_t type;
    uint8_t contact_id;
    uint32_t x;
    uint32_t y;
} kshim_touch_event_t;

typedef void (*kshim_touch_event_fn)(void *context,
                                     const kshim_touch_event_t *event);

/* Start the DT-selected touch controller, scaled to an output_width x
 * output_height surface; events are delivered synchronously to emit from
 * kshim_input_touch_poll(). Returns 0 when touch is ready, 1 when the board
 * has no supported controller (nothing touched), or a negative error after
 * the controller has been quiesced again. */
int kshim_input_touch_start(uint32_t output_width, uint32_t output_height,
                            kshim_touch_event_fn emit, void *context);

/* Service the controller once. Returns 0, or a negative error after which
 * touch is disabled and the active contact has been cancelled. */
int kshim_input_touch_poll(void);
