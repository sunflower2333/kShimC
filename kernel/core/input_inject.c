/* SPDX-License-Identifier: MIT */
/*
 * Kernel input injection (kshim/input_inject.h, docs/io-stack.md 3.9).
 *
 * Producers (the USB HID class driver today) queue menu key events and
 * pointer events; the existing consumers (kshim_key_source_poll() and
 * kshim_input_touch_poll()) drain them. Producers may run in IRQ context or
 * on another CPU, so both queues sit behind one short lock taken with local
 * interrupts masked. Before the MMU is on only the boot CPU runs and
 * exclusives on Device memory are not guaranteed, so the lock is then a plain
 * flag (the console uses the same rule).
 *
 * Pointer events are coalesced while queued: a move with the same button
 * state merges into the newest queued move, so a fast mouse never fills the
 * queue and press/release edges are never merged or dropped for room taken
 * by moves.
 */
#include <kshim/input_inject.h>
#include <kshim/io.h>
#include <kshim/irq.h>
#include <config.h>
#include <stdio.h>
#include <string.h>
#if !defined(KSHIM_HOST_TEST)
#include <kshim_mmu.h>
#endif

#define INJECT_KEY_SLOTS 32U
#define INJECT_POINTER_SLOTS 32U

static struct {
    kshim_key_event_t keys[INJECT_KEY_SLOTS];
    unsigned key_head, key_count;
    struct kshim_input_pointer_event pointers[INJECT_POINTER_SLOTS];
    unsigned pointer_head, pointer_count;
    uint32_t dropped_keys, dropped_pointers;
    bool quiesced;
} Queue;

static struct kshim_input_source *Sources;
static bool Servicing;

/* Pointer state of the touch-stream conversion (consumer side only). */
static struct {
    uint32_t width, height;
    uint32_t x, y;
    bool pressed, placed;
} Pointer;

static volatile uint32_t LockWord;

static bool shared_memory(void)
{
#if defined(__aarch64__) && !defined(KSHIM_HOST_TEST)
    return kshim_mmu_is_enabled() != 0;
#else
    return true;
#endif
}

static unsigned long inject_lock(void)
{
    unsigned long flags = kshim_irq_save();
    if (!shared_memory()) {
        LockWord = 1;
        return flags;
    }
    while (__atomic_exchange_n(&LockWord, 1U, __ATOMIC_ACQUIRE)) {
        while (__atomic_load_n(&LockWord, __ATOMIC_RELAXED)) {
#if defined(__aarch64__)
            __asm__ volatile("yield" ::: "memory");
#endif
        }
    }
    return flags;
}

static void inject_unlock(unsigned long flags)
{
    if (shared_memory()) __atomic_store_n(&LockWord, 0U, __ATOMIC_RELEASE);
    else LockWord = 0;
    kshim_irq_restore(flags);
}

int kshim_input_source_register(struct kshim_input_source *source)
{
    if (!source) return ERROR_INVALID_PARAMETER;
    for (struct kshim_input_source *s = Sources; s; s = s->next)
        if (s == source) return KSHIM_IO_ERR_EXISTS;
    source->next = NULL;
    struct kshim_input_source **tail = &Sources;
    while (*tail) tail = &(*tail)->next;
    *tail = source;
    return 0;
}

int kshim_input_source_unregister(struct kshim_input_source *source)
{
    for (struct kshim_input_source **s = &Sources; *s; s = &(*s)->next) {
        if (*s != source) continue;
        *s = source->next;
        source->next = NULL;
        return 0;
    }
    return ERROR_NOT_FOUND;
}

bool kshim_input_inject_has(unsigned caps)
{
    unsigned found = 0;
    for (struct kshim_input_source *s = Sources; s; s = s->next) found |= s->caps;
    return caps && (found & caps) == caps;
}

int kshim_input_inject_key(const kshim_key_event_t *event)
{
    if (!event) return ERROR_INVALID_PARAMETER;
    int status = 0;
    unsigned long flags = inject_lock();
    if (Queue.quiesced) {
        status = ERROR_NOT_READY;
    } else if (Queue.key_count == INJECT_KEY_SLOTS) {
        ++Queue.dropped_keys;
        status = ERROR_BUSY;
    } else {
        Queue.keys[(Queue.key_head + Queue.key_count++) % INJECT_KEY_SLOTS] = *event;
    }
    inject_unlock(flags);
    return status;
}

static bool pointer_mergeable(const struct kshim_input_pointer_event *last,
                              const struct kshim_input_pointer_event *next)
{
    if (last->mode != next->mode || last->pressed != next->pressed) return false;
    if (next->mode == KSHIM_INPUT_POINTER_RELATIVE) return true;
    return last->range_x == next->range_x && last->range_y == next->range_y;
}

static int32_t add_clamped(int32_t a, int32_t b)
{
    int64_t sum = (int64_t)a + b;
    if (sum > INT32_MAX) return INT32_MAX;
    if (sum < INT32_MIN) return INT32_MIN;
    return (int32_t)sum;
}

int kshim_input_inject_pointer(const struct kshim_input_pointer_event *event)
{
    if (!event) return ERROR_INVALID_PARAMETER;
    if (event->mode == KSHIM_INPUT_POINTER_ABSOLUTE && (!event->range_x || !event->range_y))
        return ERROR_INVALID_PARAMETER;
    int status = 0;
    unsigned long flags = inject_lock();
    if (Queue.quiesced) {
        status = ERROR_NOT_READY;
    } else {
        struct kshim_input_pointer_event *last = NULL;
        if (Queue.pointer_count)
            last = &Queue.pointers[(Queue.pointer_head + Queue.pointer_count - 1U) %
                                   INJECT_POINTER_SLOTS];
        /* Merge a move into the newest queued event of the same button
         * state, unless that event is the button edge itself (its position
         * belongs to the edge). Only a queued move is merged into. */
        bool merge = last && Queue.pointer_count > 1U && pointer_mergeable(last, event);
        if (merge) {
            const struct kshim_input_pointer_event *before =
                &Queue.pointers[(Queue.pointer_head + Queue.pointer_count - 2U) %
                                INJECT_POINTER_SLOTS];
            merge = before->pressed == last->pressed;
        }
        if (merge) {
            if (event->mode == KSHIM_INPUT_POINTER_RELATIVE) {
                last->x = add_clamped(last->x, event->x);
                last->y = add_clamped(last->y, event->y);
            } else {
                last->x = event->x;
                last->y = event->y;
            }
        } else if (Queue.pointer_count == INJECT_POINTER_SLOTS) {
            ++Queue.dropped_pointers;
            status = ERROR_BUSY;
        } else {
            Queue.pointers[(Queue.pointer_head + Queue.pointer_count++) % INJECT_POINTER_SLOTS] =
                *event;
        }
    }
    inject_unlock(flags);
    return status;
}

void kshim_input_service(void)
{
    if (Servicing || Queue.quiesced || !kshim_io_on_boot_cpu()) return;
    Servicing = true;
    for (struct kshim_input_source *s = Sources; s; s = s->next)
        if (s->service) s->service(s->context);
    Servicing = false;
}

bool kshim_input_inject_poll_key(kshim_key_event_t *event)
{
    if (!event) return false;
    bool found = false;
    unsigned long flags = inject_lock();
    if (Queue.key_count) {
        *event = Queue.keys[Queue.key_head];
        Queue.key_head = (Queue.key_head + 1U) % INJECT_KEY_SLOTS;
        --Queue.key_count;
        found = true;
    }
    inject_unlock(flags);
    return found;
}

void kshim_input_inject_set_surface(uint32_t width, uint32_t height)
{
    Pointer.width = width;
    Pointer.height = height;
    /* A relative pointer starts centred on the new surface. */
    Pointer.x = width / 2U;
    Pointer.y = height / 2U;
    Pointer.placed = false;
}

static bool pointer_pop(struct kshim_input_pointer_event *event)
{
    bool found = false;
    unsigned long flags = inject_lock();
    if (Queue.pointer_count) {
        *event = Queue.pointers[Queue.pointer_head];
        Queue.pointer_head = (Queue.pointer_head + 1U) % INJECT_POINTER_SLOTS;
        --Queue.pointer_count;
        found = true;
    }
    inject_unlock(flags);
    return found;
}

static uint32_t scale_absolute(int32_t value, uint32_t range, uint32_t size)
{
    if (value <= 0 || !size) return 0;
    if ((uint32_t)value >= range) return size - 1U;
    return (uint32_t)(((uint64_t)(uint32_t)value * (size - 1U) + range / 2U) / range);
}

static uint32_t move_relative(uint32_t position, int32_t delta, uint32_t size)
{
    int64_t next = (int64_t)position + delta;
    if (next < 0) return 0;
    if (!size) return 0;
    if (next >= (int64_t)size) return size - 1U;
    return (uint32_t)next;
}

bool kshim_input_inject_poll_touch(kshim_touch_event_t *event)
{
    struct kshim_input_pointer_event p;
    if (!event) return false;
    /* Without a surface (no kshim_input_touch_start yet) nothing can be
     * scaled: queued events stay queued (and coalesce). */
    if (!Pointer.width || !Pointer.height) return false;
    while (pointer_pop(&p)) {
        uint32_t x, y;
        if (p.mode == KSHIM_INPUT_POINTER_ABSOLUTE) {
            x = scale_absolute(p.x, p.range_x, Pointer.width);
            y = scale_absolute(p.y, p.range_y, Pointer.height);
        } else {
            x = move_relative(Pointer.x, p.x, Pointer.width);
            y = move_relative(Pointer.y, p.y, Pointer.height);
        }
        bool moved = !Pointer.placed || x != Pointer.x || y != Pointer.y;
        bool was_pressed = Pointer.pressed;
        Pointer.x = x;
        Pointer.y = y;
        Pointer.placed = true;
        Pointer.pressed = p.pressed;
        kshim_touch_event_type_t type;
        if (p.pressed && !was_pressed) type = KSHIM_TOUCH_EVENT_PRESS;
        else if (!p.pressed && was_pressed) type = KSHIM_TOUCH_EVENT_RELEASE;
        else if (!moved) continue;
        else type = p.pressed ? KSHIM_TOUCH_EVENT_MOVE : KSHIM_TOUCH_EVENT_HOVER;
        *event = (kshim_touch_event_t){.type = type, .contact_id = KSHIM_INPUT_POINTER_CONTACT,
                                       .x = x, .y = y};
        return true;
    }
    return false;
}

void kshim_input_inject_quiesce(void)
{
    unsigned long flags = inject_lock();
    bool first = !Queue.quiesced;
    uint32_t dropped_keys = Queue.dropped_keys, dropped_pointers = Queue.dropped_pointers;
    Queue.quiesced = true;
    Queue.key_count = Queue.pointer_count = 0;
    Queue.key_head = Queue.pointer_head = 0;
    inject_unlock(flags);
    Pointer.pressed = false;
    if (first && (dropped_keys || dropped_pointers))
        printf("Input: %u injected key and %u pointer events dropped (queue full)\n",
               (unsigned)dropped_keys, (unsigned)dropped_pointers);
}
