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

#if CONFIG_KSHIM_FRAMEBUFFER && CONFIG_KSHIM_LVGL
static kshim_framebuffer_t Framebuffer;
static kshim_lvgl_t Lvgl;
static uint64_t LastTime;
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
#if CONFIG_KSHIM_TOUCH
    if (kshim_input_touch_start(Framebuffer.width, Framebuffer.height, kshim_lvgl_touch, &Lvgl) < 0)
        kshim_lvgl_set_status(&Lvgl, "Use volume keys to choose");
#endif
    LastTime = get_current_time();
    return 0;
}
static void menu_frame(void)
{
#if CONFIG_KSHIM_TOUCH
    if (kshim_input_touch_poll() < 0)
        kshim_lvgl_set_status(&Lvgl, "Use volume keys to choose");
#endif
    uint64_t now = get_current_time(), elapsed = now - LastTime;
    LastTime = now;
    kshim_lvgl_frame(&Lvgl, elapsed > UINT32_MAX ? UINT32_MAX : (uint32_t)elapsed);
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
                if (next <= count) { numeric = next; kshim_console_write(&c, 1); }
            } else if (c == '\r' || c == '\n') {
                if (numeric && numeric <= count) { current = numeric - 1; break; }
                numeric = 0;
            } else if (c == 27) { current = initial; break; }
        }
        if (keys_ready) {
            kshim_key_event_t event;
            /* Kept as before the kernel/user split: this compares against
             * NO_EVENT (QCOM_KEYS_OK), so delivered key events are ignored. */
            if (kshim_key_source_poll(kshim_input_keys(), &event) == KSHIM_KEYS_NO_EVENT && event.pressed) {
                if (event.action == KSHIM_KEY_ACTION_UP) current = current ? current - 1 : count - 1;
                if (event.action == KSHIM_KEY_ACTION_DOWN) current = (current + 1) % count;
                if (event.action == KSHIM_KEY_ACTION_SELECT) break;
                printf("\n> %s\n", names[current]);
            }
        }
        if (timeout && get_current_time() - start >= timeout) break;
        (void)delay(5000);
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
            menu_frame();
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
            if (timeout_ms && !Lvgl.AutobootCancelled && get_current_time() - start >= timeout_ms) { *selected = initial; break; }
            (void)delay(5000);
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
