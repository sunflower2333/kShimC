/* SPDX-License-Identifier: MIT */
#include <kshim.h>
#include <platform.h>
#include <qcom_dt.h>
#include <smp.h>
#include <smp_selftest.h>
#include <psci.h>
#include <kshim_mmu.h>
#include <st_ftm5.h>
#include <st_ftm4.h>
#include <kshim_lvgl_os.h>
#include <kshim_console.h>
#include <timer.h>
#include <config.h>
#include <framebuffer.h>
#include <lvgl_port.h>
#include <qcom_key_platform.h>
#include <qcom_keys.h>
#include <qcom_keys_fdt.h>
#include <uefi_boot.h>

__attribute__((weak)) int kshim_menu_activate(kshim_menu_action_t action)
{ (void)action; return -1; }
static int SmpInitialized;
static QcomKeys Keys;
static int build_keys(const void *fdt)
{
#if CONFIG_QCOM_KEYS
    QcomKeyDescriptor descriptors[QCOM_KEYS_MAX];
    size_t count = 0;
    if (!fdt || QcomKeysBuildFromDeviceTree(fdt, descriptors, QCOM_KEYS_MAX, &count) || !count)
        return -1;
    int status = QcomKeysInitEx(&Keys, descriptors, count, kshim_qcom_key_read32,
                                kshim_qcom_key_read32_status, NULL);
    QcomKeysSetDebounce(&Keys, CONFIG_QCOM_KEY_DEBOUNCE_SAMPLES);
    return status;
#else
    (void)fdt; return -1;
#endif
}
#if CONFIG_KSHIM_FRAMEBUFFER && CONFIG_KSHIM_LVGL
static kshim_framebuffer_t Framebuffer;
static kshim_lvgl_t Lvgl;
static uint64_t LastTime;
#if CONFIG_KSHIM_TOUCH
static kshim_touch_t Touch;
static struct CrQupContext TouchPlatform;
static kshim_ftm4_t Ftm4;
static kshim_ftm5_t Ftm5;
static int TouchAttempted, TouchReady;
static struct kshim_touch_resource *TouchResource;
static int touch_transfer(void *context, const uint8_t *command, size_t length,
                          uint8_t *response, size_t count)
{
    (void)context;
    struct CrI2cMessage messages[] = {{TouchResource->address, false, (uint8_t *)(uintptr_t)command, length},
                                    {TouchResource->address, true, response, count}};
    return CrGeniI2cTransfer(&TouchPlatform.i2c, messages, count ? 2 : 1, 20000);
}
static void touch_delay(void *context, uint32_t us)
{ (void)context; TouchPlatform.io.delay_us(TouchPlatform.io.context, us); }
static int touch_quiesce(void)
{
    if (!TouchAttempted) return 0;
    int status = TouchResource->generation == 4 ? kshim_ftm4_quiesce(&Ftm4) : kshim_ftm5_quiesce(&Ftm5);
    if (status || CrQupQuiesce(&TouchPlatform)) return -1;
    TouchAttempted = TouchReady = 0;
    return 0;
}
static void start_touch(struct kshim_resources *r)
{
    if (r->touch_status) return;
    TouchResource = &r->touch;
    kshim_touch_config_t cfg = {.source_width = r->touch.width, .source_height = r->touch.height,
        .output_width = Framebuffer.width, .output_height = Framebuffer.height,
        .flip_x = r->touch.flip_x, .flip_y = r->touch.flip_y,
        .emit = kshim_lvgl_touch, .emit_context = &Lvgl};
    int status = kshim_touch_init(&Touch, &cfg);
    if (!status) status = CrQupInit(&TouchPlatform, NULL, &r->touch.qup);
    TouchAttempted = 1;
    if (!status && r->touch.generation == 4) {
        kshim_ftm4_transport_t t = {NULL, touch_transfer, touch_delay};
        status = kshim_ftm4_init(&Ftm4, &t, &Touch, false, false);
    } else if (!status) {
        kshim_ftm5_transport_t t = {NULL, touch_transfer, touch_delay, KSHIM_FTM5_BUS_I2C};
        status = kshim_ftm5_init(&Ftm5, &t, &Touch);
    }
    TouchReady = status == 0;
    printf("Touch: FTM%u status %d\n", r->touch.generation, status);
    if (!TouchReady) { (void)touch_quiesce(); kshim_lvgl_set_status(&Lvgl, "Use volume keys to choose"); }
}
#endif
static int start_menu(const void *fdt)
{
    struct kshim_resources *r = kshim_resources_get();
    kshim_framebuffer_config_t config = r->framebuffer;
#if CONFIG_KSHIM_QEMU_PLATFORM
    /* Explicit virtual test surface; physical platforms require scanout evidence. */
    config = (kshim_framebuffer_config_t){.render_address = CONFIG_KSHIM_FB_RENDER_ADDRESS,
        .width = CONFIG_KSHIM_FB_WIDTH, .height = CONFIG_KSHIM_FB_HEIGHT,
        .stride = CONFIG_KSHIM_FB_STRIDE, .format = CONFIG_KSHIM_FB_FORMAT,
        .bpp = kshim_fb_format_bpp(CONFIG_KSHIM_FB_FORMAT), .foreground = UINT32_MAX};
    r->framebuffer = config;
#endif
    if (!config.render_address || kshim_fb_init(&Framebuffer, &config)) return -1;
    int keys = build_keys(fdt);
#if CONFIG_KSHIM_SMP
    int status = kshim_smp_init(fdt);
    SmpInitialized = status == 0;
    if (SmpInitialized) status = kshim_smp_start_workers(KSHIM_SMP_DEFAULT_TIMEOUT_US);
    printf("PSCI workers: %u (status %d)\n", (unsigned)kshim_smp_available_count(), status);
#endif
    if (!kshim_mmu_is_enabled() &&
        (kshim_mmu_root_table() || kshim_mmu_configure_default(fdt) == 0))
        (void)kshim_mmu_enable_current_cpu();
    kshim_lvgl_set_scratch((void *)(uintptr_t)r->ui_scratch.base,
                           (size_t)r->ui_scratch.size);
    if (kshim_lvgl_init(&Lvgl, &Framebuffer, &Keys)) return -2;
    printf("UI: %s font, scratch %#llx+%#llx\n",
           kshim_lvgl_has_cjk_font(&Lvgl) ? "Noto Sans CJK" : "fallback Latin",
           (unsigned long long)r->ui_scratch.base, (unsigned long long)r->ui_scratch.size);
    if (keys) kshim_lvgl_set_status(&Lvgl, "Volume keys unavailable");
#if CONFIG_KSHIM_TOUCH
    start_touch(r);
#endif
    LastTime = get_current_time();
    return 0;
}
static void menu_frame(void)
{
#if CONFIG_KSHIM_TOUCH
    if (TouchReady) {
        int status = CrQupIrq(&TouchPlatform);
        if (status > 0) status = TouchResource->generation == 4 ? kshim_ftm4_poll(&Ftm4) : kshim_ftm5_poll(&Ftm5);
        if (status < 0) {
            TouchReady = 0; kshim_touch_cancel(&Touch);
            printf("Touch: input status %d\n", status);
            kshim_lvgl_set_status(&Lvgl, "Use volume keys to choose");
        }
    }
#endif
    uint64_t now = get_current_time(), elapsed = now - LastTime;
    LastTime = now;
    kshim_lvgl_frame(&Lvgl, elapsed > UINT32_MAX ? UINT32_MAX : (uint32_t)elapsed);
}
#endif
int kshim_platform_prepare_handoff(void)
{
#if CONFIG_KSHIM_FRAMEBUFFER && CONFIG_KSHIM_LVGL
#if CONFIG_KSHIM_TOUCH
    TouchReady = 0;
    if (touch_quiesce()) return -1;
#endif
    kshim_lvgl_deinit(&Lvgl);
#if CONFIG_KSHIM_SMP
    if (kshim_lvgl_os_prepare_shutdown()) return -2;
#endif
#endif
    if (SmpInitialized) {
        if (kshim_smp_prepare_handoff(KSHIM_SMP_DEFAULT_TIMEOUT_US)) return -2;
        SmpInitialized = 0;
        return 0;
    }
    return kshim_mmu_prepare_handoff();
}
static int serial_select(const void *fdt, const char *const *names, size_t count,
                          size_t initial, uint32_t timeout, size_t *selected)
{
    int keys_ready = build_keys(fdt) == 0;
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
            QcomKeyEvent event;
            if (QcomKeysPoll(&Keys, &event) == QCOM_KEYS_OK && event.Pressed) {
                if (event.Action == QCOM_KEY_ACTION_UP) current = current ? current - 1 : count - 1;
                if (event.Action == QCOM_KEY_ACTION_DOWN) current = (current + 1) % count;
                if (event.Action == QCOM_KEY_ACTION_SELECT) break;
                printf("\n> %s\n", names[current]);
            }
        }
        if (timeout && get_current_time() - start >= timeout) break;
        (void)delay(5000);
    }
    printf("\n"); *selected = current;
    return 0;
}
int kshim_platform_select(uint64_t fdt, const char *const *names, size_t count,
                           size_t initial, uint32_t timeout_ms, size_t *selected)
{
    if (!names || !selected || !count || initial >= count) return -1;
#if CONFIG_KSHIM_FRAMEBUFFER && CONFIG_KSHIM_LVGL
    int status = start_menu((const void *)(uintptr_t)fdt);
    if (!status && kshim_lvgl_set_entries(&Lvgl, names, count, initial) == 0) {
        kshim_lvgl_set_countdown(&Lvgl, timeout_ms);
        uint64_t start = get_current_time();
        for (;;) {
            menu_frame();
            int index = kshim_lvgl_take_index(&Lvgl);
            if (index >= 0 && (size_t)index < count) { *selected = index; break; }
            if (timeout_ms && get_current_time() - start >= timeout_ms) { *selected = initial; break; }
            (void)delay(5000);
        }
        return kshim_platform_prepare_handoff() ? -2 : 0;
    }
    if (kshim_platform_prepare_handoff()) return -2;
#endif
    return serial_select((const void *)(uintptr_t)fdt, names, count, initial, timeout_ms, selected);
}
int kshim_platform_main(uint64_t fdt, uint64_t arg1, uint64_t arg2, uint64_t arg3)
{
    kshim_boot_set_args(fdt, arg1, arg2, arg3);
#if CONFIG_KSHIM_SMP_SELFTEST
    int status = kshim_smp_selftest((const void *)(uintptr_t)fdt);
    printf("SMP_SELFTEST %s (%d)\n", status == 0 ? "PASS" : "FAIL", status);
    kshim_psci_system_off();
#endif
    const char *names[] = {CONFIG_KSHIM_MENU_ENTRY1, CONFIG_KSHIM_MENU_ENTRY2};
    for (;;) {
        size_t selected;
        if (kshim_platform_select(fdt, names, 2, 0, 0, &selected) == -2) {
            printf("Unsafe UI shutdown; refusing payload handoff\n");
            for (;;) (void)delay(100000);
        }
        if (kshim_menu_activate(selected ? KSHIM_MENU_RECOVERY : KSHIM_MENU_BOOT))
            printf("Unable to start selected image\n");
    }
}
