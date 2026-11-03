/* SPDX-License-Identifier: MIT */
#pragma once
/*
 * Kernel input injection (CONFIG_KSHIM_INPUT_INJECT, selected by
 * KSHIM_USB_HID). Owner: usbhid (kernel/core/input_inject.c).
 * See docs/io-stack.md.
 *
 * Producers (USB HID now; other sources later) inject menu key actions and
 * pointer events. The existing consumer API in include/kshim/input.h is
 * unchanged and sees them:
 *  - kshim_key_source_poll(kshim_input_keys(), &event) returns injected key
 *    events before the platform (Qualcomm PON/GPIO) keys
 *    (hook in kernel/drivers/keys.c);
 *  - kshim_input_touch_poll() delivers injected pointer events to the emit
 *    callback given to kshim_input_touch_start(), scaled to its output
 *    surface (hook in kernel/core/input.c);
 *  - kshim_input_keys_init() succeeds when a key-capable source is
 *    registered even without platform keys (QEMU);
 *  - kshim_input_quiesce() calls kshim_input_inject_quiesce() (handoff
 *    step 1).
 * Both consumers first run kshim_input_service(), which pumps every
 * registered source (kshim_usb_poll), so hotplug and the polling fallback
 * advance while the menu runs.
 */
#include <stdbool.h>
#include <stdint.h>

#include <kshim/input.h>
#include "error_no.h"

#define KSHIM_INPUT_CAP_KEYS    0x1U
#define KSHIM_INPUT_CAP_POINTER 0x2U

/* Contact id carried by injected pointer events (outside the controller
 * contact ids 0..9 the touch tracker uses). */
#define KSHIM_INPUT_POINTER_CONTACT 15U

/* A registered source. Static storage owned by the producer. */
struct kshim_input_source {
    const char *name;
    unsigned caps;                   /* KSHIM_INPUT_CAP_*; 0 for a service-only pump */
    /* Thread context, boot CPU, never re-entered; may be NULL. */
    void (*service)(void *context);
    void *context;
    struct kshim_input_source *next; /* owned by the input core */
};

/* Thread context. KSHIM_IO_ERR_EXISTS when already registered. */
int kshim_input_source_register(struct kshim_input_source *source);
int kshim_input_source_unregister(struct kshim_input_source *source);
/* True when a registered source has every bit of caps. */
bool kshim_input_inject_has(unsigned caps);

/* ---- Producers: IRQ-safe, any CPU. ---------------------------------------
 * 0, ERROR_BUSY when the queue is full (the event is dropped and counted),
 * or ERROR_NOT_READY after kshim_input_inject_quiesce(). */

/* type PRESS/RELEASE, action UP/DOWN/SELECT/BACK (NONE is queued for code
 * only listeners), code = Linux input code (KEY_UP, BTN_SOUTH, ...),
 * pressed = 1 for PRESS. */
int kshim_input_inject_key(const kshim_key_event_t *event);

enum kshim_input_pointer_mode {
    KSHIM_INPUT_POINTER_ABSOLUTE = 0,
    KSHIM_INPUT_POINTER_RELATIVE,
};

struct kshim_input_pointer_event {
    enum kshim_input_pointer_mode mode;
    int32_t x, y;               /* absolute: 0..range; relative: delta in surface pixels */
    uint32_t range_x, range_y;  /* absolute only: inclusive logical maximum (> 0) */
    bool pressed;               /* primary button / tip switch */
};
int kshim_input_inject_pointer(const struct kshim_input_pointer_event *event);

/* ---- Consumers (kernel input core) --------------------------------------- */

/* Pump every source's service hook. Thread context on the boot CPU; a
 * no-op elsewhere and when re-entered. */
void kshim_input_service(void);
/* Pop one queued key event; false when empty. */
bool kshim_input_inject_poll_key(kshim_key_event_t *event);
/* Output surface for pointer scaling (kshim_input_touch_start). Relative
 * pointers start centred and are clamped to it. */
void kshim_input_inject_set_surface(uint32_t width, uint32_t height);
/* Pop one pointer event converted to the touch stream: press -> PRESS,
 * move while pressed -> MOVE, release -> RELEASE, move while released ->
 * HOVER; contact_id KSHIM_INPUT_POINTER_CONTACT; x/y in surface pixels. */
bool kshim_input_inject_poll_touch(kshim_touch_event_t *event);
/* Handoff step 1 (kshim_input_quiesce): refuse new events and drop queued
 * ones; the UI is torn down next, so no CANCEL is delivered. Idempotent. */
void kshim_input_inject_quiesce(void);
