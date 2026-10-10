/* Render reviewable full-resolution menu keyframes as binary PPM. */
#include <assert.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <lvgl.h>
#include <lvgl_port.h>
#include "../src/ui/menu_style.h"

static uint32_t Inputs[3];

static uint32_t ReadInput(void *Context, uint64_t Address)
{
    (void)Context;
    assert(Address >= 1U && Address <= 3U);
    return Inputs[Address - 1U];
}

static void Advance(kshim_lvgl_t *Ui, uint32_t Milliseconds)
{
    while (Milliseconds != 0U) {
        uint32_t Step = Milliseconds > 16U ? 16U : Milliseconds;
        kshim_lvgl_frame(Ui, Step);
        Milliseconds -= Step;
    }
}

/* 按当前 tick 导出完整关键帧，不将显示刷新定时器的相位当成视觉差异。 */
static uint64_t WritePpm(
    const char *Directory,
    const char *Stage,
    const kshim_framebuffer_t *Framebuffer
)
{
    lv_refr_now(NULL);
    char Path[1024];
    int Length = snprintf(Path, sizeof(Path), "%s/menu-%ux%u-%s.ppm",
                          Directory, Framebuffer->width,
                          Framebuffer->height, Stage);
    assert(Length > 0 && (size_t)Length < sizeof(Path));
    FILE *File = fopen(Path, "wb");
    assert(File != NULL);
    assert(fprintf(File, "P6\n%u %u\n255\n", Framebuffer->width,
                   Framebuffer->height) > 0);

    uint64_t Hash = UINT64_C(14695981039346656037);
    unsigned NonBlack = 0U;
    unsigned Different = 0U;
    uint32_t FirstPixel = 0U;
    for (uint32_t Y = 0; Y < Framebuffer->height; Y++) {
        const uint32_t *Row = (const uint32_t *)(const void *)(
            Framebuffer->render_address + (size_t)Y * Framebuffer->stride);
        for (uint32_t X = 0; X < Framebuffer->width; X++) {
            uint32_t Pixel = Row[X];
            uint8_t Rgb[3] = {
                (uint8_t)(Pixel >> 16),
                (uint8_t)(Pixel >> 8),
                (uint8_t)Pixel,
            };
            assert(fwrite(Rgb, sizeof(Rgb), 1U, File) == 1U);
            if (Y == 0U && X == 0U)
                FirstPixel = Pixel & 0xffffffU;
            NonBlack += (Pixel & 0xffffffU) != 0U;
            Different += (Pixel & 0xffffffU) != FirstPixel;
            for (size_t Byte = 0; Byte < sizeof(Rgb); Byte++)
                Hash = (Hash ^ Rgb[Byte]) * UINT64_C(1099511628211);
        }
    }
    assert(NonBlack > Framebuffer->height);
    assert(Different > Framebuffer->height);
    assert(fclose(File) == 0);
    printf("%s  %016" PRIx64 "\n", Path, Hash);
    fflush(stdout);
    return Hash;
}

/* 记录确认入口和动画状态，保留像素变化断言。 */
static void TraceConfirmation(const char *Stage, const kshim_lvgl_t *Ui)
{
    printf("CONFIRM %s: index=%d ready=%u elapsed=%u\n", Stage,
           Ui->PendingIndex, (unsigned)Ui->Ready, (unsigned)Ui->ElapsedMs);
    printf("CONFIRM %s: animation=%u scale_x=%ld scale_y=%ld running=%u\n", Stage,
           (unsigned)(lv_anim_get(Ui->BootButton, NULL) != NULL),
           (long)lv_obj_get_style_transform_scale_x(Ui->BootButton, LV_PART_MAIN),
           (long)lv_obj_get_style_transform_scale_y(Ui->BootButton, LV_PART_MAIN),
           (unsigned)lv_anim_count_running());
    fflush(stdout);
}

static void RenderSize(const char *Directory, uint32_t Width, uint32_t Height)
{
    size_t Bytes = (size_t)Width * Height * sizeof(uint32_t);
    uint32_t *Pixels = malloc(Bytes);
    assert(Pixels != NULL);
    memset(Pixels, 0, Bytes);

    kshim_framebuffer_config_t Config = {
        .render_address = (uintptr_t)Pixels,
        .width = Width,
        .height = Height,
        .stride = Width * sizeof(uint32_t),
        .bpp = 32U,
        .format = KSHIM_FB_FORMAT_XRGB8888,
        .buffer_size = Bytes,
    };
    kshim_framebuffer_t Framebuffer;
    assert(kshim_fb_init(&Framebuffer, &Config) == KSHIM_FB_OK);

    QcomKeyDescriptor Descriptors[3];
    const uint32_t Codes[] = {
        QCOM_KEYCODE_VOLUMEUP, QCOM_KEYCODE_VOLUMEDOWN,
        QCOM_KEYCODE_POWER,
    };
    for (size_t Index = 0; Index < 3U; Index++)
        assert(QcomKeyMakeGpioAt(&Descriptors[Index], Index + 1U, 1U,
                                 Codes[Index], 0U) == 0);
    QcomKeys Keys;
    memset(Inputs, 0, sizeof(Inputs));
    assert(QcomKeysInit(&Keys, Descriptors, 3U, ReadInput, NULL) == 0);

    static uint8_t Scratch[4U << 20] __attribute__((aligned(64)));
    kshim_lvgl_set_scratch(Scratch, sizeof(Scratch));
    kshim_lvgl_t Ui;
    assert(kshim_lvgl_init(&Ui, &Framebuffer, &Keys) == 0);
    assert(kshim_lvgl_has_cjk_font(&Ui));
    static const char *const Entries[] = {
        "Android", "UEFI 诊断", "Recovery 恢复模式", "Factory image",
        "Copied test payload", "Network rescue", "关机",
    };
    assert(kshim_lvgl_set_entries(&Ui, Entries,
                                  sizeof(Entries) / sizeof(Entries[0]), 0U) == 0);

    Advance(&Ui, 120U);
    uint64_t EntryHash = WritePpm(Directory, "entry-120ms", &Framebuffer);
    Advance(&Ui, 280U);
    uint64_t ReadyHash = WritePpm(Directory, "ready-400ms", &Framebuffer);
    /* Native LVGL has no custom entry fade: these two frames should be stable. */
    assert(KSHIM_MENU_IS_NATIVE ? EntryHash == ReadyHash : EntryHash != ReadyHash);

    lv_obj_t *Third = lv_group_get_obj_by_index(Ui.Group, 2U);
    assert(Third != NULL);
    lv_group_focus_obj(Third);
    Advance(&Ui, KSHIM_UI_FOCUS_DURATION_MS / 2U);
    uint64_t FocusHash = WritePpm(Directory, "focus-130ms", &Framebuffer);
    assert(FocusHash != ReadyHash);

    if (KSHIM_MENU_IS_NATIVE) {
        /* Sample actual native pressed feedback, not a synthetic kShim pulse. */
        lv_area_t area; lv_obj_get_coords(Ui.BootButton, &area);
        kshim_touch_event_t touch = {.type=KSHIM_TOUCH_EVENT_PRESS,
            .x=(uint32_t)((area.x1+area.x2)/2), .y=(uint32_t)((area.y1+area.y2)/2)};
        kshim_lvgl_touch(&Ui, &touch);
        assert(kshim_lvgl_take_index(&Ui) == -1);
        Advance(&Ui, KSHIM_UI_CONFIRM_DURATION_MS / 2U);
        uint64_t ConfirmHash = WritePpm(Directory, "confirm-90ms", &Framebuffer);
        assert(ConfirmHash != FocusHash && lv_obj_has_state(Ui.BootButton,LV_STATE_PRESSED));
        touch.type=KSHIM_TOUCH_EVENT_RELEASE;
        kshim_lvgl_touch(&Ui, &touch);
    } else {
        assert(lv_obj_send_event(Ui.BootButton, LV_EVENT_CLICKED, NULL) == LV_RESULT_OK);
        TraceConfirmation("requested", &Ui);
        Advance(&Ui, KSHIM_UI_CONFIRM_DURATION_MS / 2U);
        uint64_t ConfirmHash = WritePpm(Directory, "confirm-90ms", &Framebuffer);
        TraceConfirmation("sampled", &Ui);
        assert(ConfirmHash != FocusHash);
    }
    assert(kshim_lvgl_take_index(&Ui) == 2);

    kshim_lvgl_deinit(&Ui);
    free(Pixels);
}

int main(int ArgumentCount, char **Arguments)
{
    assert(ArgumentCount == 2);
    RenderSize(Arguments[1], 1080U, 2340U);
    RenderSize(Arguments[1], 1920U, 1080U);
    puts("full-resolution menu keyframes rendered");
    return 0;
}
