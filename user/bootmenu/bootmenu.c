/* SPDX-License-Identifier: MIT */
/*
 * Boot-entry selection (userspace): the LVGL framebuffer menu, the serial
 * fallback, and the standalone two-entry loop. Hardware is reached only
 * through the public kernel API in include/kshim/.
 */
#include <kshim/boot.h>
#include <kshim/console.h>
#include <kshim/display.h>
#include <kshim/input.h>
#include <kshim/platform.h>
#include <kshim/power.h>
#include <kshim/time.h>
#include <kshim/user.h>
#include <config.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#if CONFIG_KSHIM_FRAMEBUFFER && CONFIG_KSHIM_LVGL
#include <framebuffer.h>
#include <lvgl_port.h>
#include <kshim_lvgl_os.h>
#endif

/* Input that has no interrupt is polled at least this often; the menus
 * sleep in kshim_wait_event() until then, an interrupt, or their deadline. */
#define MENU_POLL_MS 5U
/* Upper bound on one LVGL-loop sleep, should LVGL report no pending timer. */
#define MENU_MAX_SLEEP_MS 100U

static uint64_t earlier(uint64_t a, uint64_t b) { return a < b ? a : b; }

#if CONFIG_KSHIM_FRAMEBUFFER && CONFIG_KSHIM_LVGL
static kshim_framebuffer_t Framebuffer;
static kshim_lvgl_t Lvgl;
static uint64_t LastTime, SleptMs;
static int TouchPolled;
static int start_menu(const void *fdt)
{
    kshim_framebuffer_config_t config;
    if (kshim_display_get(&config) || kshim_fb_init(&Framebuffer, &config)) return -1;
    int keys = kshim_input_keys_init(fdt);
    kshim_platform_accelerate(fdt);
    uint64_t scratch_base, scratch_size;
    kshim_display_scratch(&scratch_base, &scratch_size);
    kshim_lvgl_set_scratch((void *)(uintptr_t)scratch_base, (size_t)scratch_size);
    if (kshim_lvgl_init(&Lvgl, &Framebuffer, kshim_input_keys())) return -2;
    printf("UI: %s font, scratch %#llx+%#llx\n",
           kshim_lvgl_has_cjk_font(&Lvgl) ? "Noto Sans CJK" : "fallback Latin",
           (unsigned long long)scratch_base, (unsigned long long)scratch_size);
    if (keys) kshim_lvgl_set_status(&Lvgl, "Volume keys unavailable");
    TouchPolled = 0;
#if CONFIG_KSHIM_TOUCH
    if (kshim_input_touch_start(Framebuffer.width, Framebuffer.height, kshim_lvgl_touch, &Lvgl) < 0)
        kshim_lvgl_set_status(&Lvgl, "Use volume keys to choose");
    else TouchPolled = 1;
#endif
    LastTime = get_current_time();
    SleptMs = 0;
    return 0;
}
/* One frame; returns the milliseconds until LVGL next needs one. */
static uint32_t menu_frame(void)
{
#if CONFIG_KSHIM_TOUCH
    if (kshim_input_touch_poll() < 0)
        kshim_lvgl_set_status(&Lvgl, "Use volume keys to choose");
#endif
    uint64_t now = get_current_time(), elapsed = now - LastTime;
    /* The old loop slept MENU_POLL_MS between frames; only waiting beyond
     * that is idle time for the slow-frame heuristic. */
    uint64_t idle = SleptMs > MENU_POLL_MS ? SleptMs - MENU_POLL_MS : 0;
    LastTime = now;
    SleptMs = 0;
    if (elapsed > UINT32_MAX) elapsed = UINT32_MAX;
    return kshim_lvgl_frame_idle(&Lvgl, (uint32_t)elapsed, (uint32_t)earlier(idle, elapsed));
}
#endif
int kshim_user_prepare_handoff(void)
{
#if CONFIG_KSHIM_FRAMEBUFFER && CONFIG_KSHIM_LVGL
    kshim_lvgl_deinit(&Lvgl);
#if CONFIG_KSHIM_SMP
    if (kshim_lvgl_os_prepare_shutdown()) return -2;
#endif
#endif
    return 0;
}
static int serial_select(const void *fdt, const char *const *names, size_t count,
                          size_t initial, uint32_t timeout, size_t *selected)
{
    int keys_ready = kshim_input_keys_init(fdt) == 0;
    size_t current = initial, numeric = 0;
    printf("\nKShim boot menu\n");
    for (size_t i = 0; i < count; ++i) printf("  %u. %s%s\n", (unsigned)i + 1, names[i], i == initial ? " [default]" : "");
    printf("Select kernel and press Enter: ");
    uint64_t start = get_current_time();
    for (;;) {
        char c;
        if (kshim_console_try_read(&c)) {
            if (c >= '0' && c <= '9') {
                size_t next = numeric * 10 + c - '0';
                timeout = 0; /* the user is choosing: stop autoboot */
                if (next <= count) { numeric = next; kshim_console_write(&c, 1); }
            } else if (c == '\r' || c == '\n') {
                if (numeric && numeric <= count) { current = numeric - 1; break; }
                numeric = 0;
            } else if (c == 27) { current = initial; break; }
        }
        if (keys_ready) {
            kshim_key_event_t event;
            /* *event is only written for KSHIM_KEYS_EVENT. Like the LVGL
             * menu, act on press edges and cancel autoboot on the first one. */
            if (kshim_key_source_poll(kshim_input_keys(), &event) == KSHIM_KEYS_EVENT &&
                event.type == KSHIM_KEY_EVENT_PRESS) {
                timeout = 0;
                if (event.action == KSHIM_KEY_ACTION_UP) current = current ? current - 1 : count - 1;
                if (event.action == KSHIM_KEY_ACTION_DOWN) current = (current + 1) % count;
                if (event.action == KSHIM_KEY_ACTION_SELECT) break;
                printf("\n> %s\n", names[current]);
            }
        }
        uint64_t now = get_current_time();
        if (timeout && now - start >= timeout) break;
        /* Console and keys are polled: wake at the next poll at the latest;
         * an input interrupt ends the wait sooner. */
        uint64_t deadline = now + MENU_POLL_MS;
        if (timeout) deadline = earlier(deadline, start + timeout);
        kshim_wait_event(deadline);
    }
    printf("\n"); *selected = current;
    return 0;
}
int kshim_user_select(uint64_t fdt, const char *const *names, size_t count,
                      size_t initial, uint32_t timeout_ms, size_t *selected)
{
    if (!names || !selected || !count || initial >= count) return -1;
#if CONFIG_KSHIM_FRAMEBUFFER && CONFIG_KSHIM_LVGL
    int status = start_menu((const void *)(uintptr_t)fdt);
    if (!status && kshim_lvgl_set_entries(&Lvgl, names, count, initial) == 0) {
        uint32_t power_caps = kshim_platform_power_init((const void *)(uintptr_t)fdt);
        if (kshim_lvgl_set_power_capabilities(&Lvgl, power_caps))
            kshim_lvgl_set_status(&Lvgl, "Power controls unavailable");
        kshim_lvgl_set_countdown(&Lvgl, timeout_ms);
        uint64_t start = get_current_time();
        for (;;) {
            uint32_t next_ms = menu_frame();
            kshim_power_action_t action = kshim_lvgl_take_power(&Lvgl);
            if (action != KSHIM_POWER_NONE) {
                /* Never reset from an LVGL callback or leave active bus/render
                 * workers accessing MMIO after the PMIC arbiter is stopped. */
                int power_status = kshim_platform_prepare_handoff();
                if (!power_status) power_status = kshim_platform_power_execute(action);
                printf("Power action %u returned (%d); refusing payload handoff\n",
                       (unsigned)action, power_status);
                for (;;) (void)delay(100000);
            }
            int index = kshim_lvgl_take_index(&Lvgl);
            if (index >= 0 && (size_t)index < count) { *selected = index; break; }
            uint64_t now = get_current_time();
            if (timeout_ms && !Lvgl.AutobootCancelled && now - start >= timeout_ms) { *selected = initial; break; }
            /* Sleep until the next LVGL timer, the autoboot deadline, or the
             * next touch poll; input interrupts wake the loop sooner. */
            uint64_t deadline = now + earlier(next_ms, MENU_MAX_SLEEP_MS);
            if (TouchPolled) deadline = earlier(deadline, now + MENU_POLL_MS);
            if (timeout_ms && !Lvgl.AutobootCancelled) deadline = earlier(deadline, start + timeout_ms);
            uint64_t before = get_current_time();
            kshim_wait_event(deadline);
            SleptMs = get_current_time() - before;
        }
        return kshim_platform_prepare_handoff() ? -2 : 0;
    }
    if (kshim_platform_prepare_handoff()) return -2;
#endif
    return serial_select((const void *)(uintptr_t)fdt, names, count, initial, timeout_ms, selected);
}
int kshim_user_main(uint64_t fdt)
{
    const char *names[] = {CONFIG_KSHIM_MENU_ENTRY1, CONFIG_KSHIM_MENU_ENTRY2};
    for (;;) {
        size_t selected;
        if (kshim_user_select(fdt, names, 2, 0, 0, &selected) == -2) {
            printf("Unsafe UI shutdown; refusing payload handoff\n");
            for (;;) (void)delay(100000);
        }
        if (kshim_menu_activate(selected ? KSHIM_MENU_RECOVERY : KSHIM_MENU_BOOT))
            printf("Unable to start selected image\n");
    }
}
