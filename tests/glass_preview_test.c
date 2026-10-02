/* Render reviewable full-resolution liquid-glass keyframes as binary PPM. */
#include <assert.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <lvgl.h>
#include <lvgl_port.h>

#ifndef CONFIG_KSHIM_UI_DARK
#define CONFIG_KSHIM_UI_DARK 1
#endif

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

static uint64_t WritePpm(const char *Directory, const char *Stage,
                         const kshim_framebuffer_t *Framebuffer)
{
    char Path[1024];
    const char *Theme = CONFIG_KSHIM_UI_DARK ? "dark" : "light";
    int Length = snprintf(Path, sizeof(Path), "%s/glass-%s-%ux%u-%s.ppm",
                          Directory, Theme, Framebuffer->width,
                          Framebuffer->height, Stage);
    assert(Length > 0 && (size_t)Length < sizeof(Path));
    FILE *File = fopen(Path, "wb");
    assert(File != NULL);
    assert(fprintf(File, "P6\n%u %u\n255\n", Framebuffer->width,
                   Framebuffer->height) > 0);

    uint64_t Hash = UINT64_C(14695981039346656037);
    unsigned NonBlack = 0U;
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
            NonBlack += (Pixel & 0xffffffU) != 0U;
            for (size_t Byte = 0; Byte < sizeof(Rgb); Byte++)
                Hash = (Hash ^ Rgb[Byte]) * UINT64_C(1099511628211);
        }
    }
    assert(NonBlack > Framebuffer->width * Framebuffer->height / 2U);
    assert(fclose(File) == 0);
    printf("%s  %016" PRIx64 "\n", Path, Hash);
    return Hash;
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

    kshim_lvgl_t Ui;
    assert(kshim_lvgl_init(&Ui, &Framebuffer, &Keys) == 0);
    static const char *const Entries[] = {
        "Android", "UEFI diagnostics", "Recovery", "Factory image",
        "Copied test payload", "Network rescue", "Power off",
    };
    assert(kshim_lvgl_set_entries(&Ui, Entries,
                                  sizeof(Entries) / sizeof(Entries[0]), 0U) == 0);

    Advance(&Ui, 120U);
    uint64_t EntryHash = WritePpm(Directory, "entry-120ms", &Framebuffer);
    Advance(&Ui, 280U);
    uint64_t ReadyHash = WritePpm(Directory, "ready-400ms", &Framebuffer);
    assert(EntryHash != ReadyHash);

    lv_obj_t *Third = lv_group_get_obj_by_index(Ui.Group, 2U);
    assert(Third != NULL);
    lv_group_focus_obj(Third);
    Advance(&Ui, KSHIM_UI_FOCUS_DURATION_MS / 2U);
    uint64_t FocusHash = WritePpm(Directory, "focus-130ms", &Framebuffer);
    assert(FocusHash != ReadyHash);

    assert(lv_obj_send_event(Ui.BootButton, LV_EVENT_CLICKED, NULL) ==
           LV_RESULT_OK);
    Advance(&Ui, KSHIM_UI_CONFIRM_DURATION_MS / 2U);
    uint64_t ConfirmHash = WritePpm(Directory, "confirm-90ms", &Framebuffer);
    assert(ConfirmHash != FocusHash);
    assert(kshim_lvgl_take_index(&Ui) == 2);

    kshim_lvgl_deinit(&Ui);
    free(Pixels);
}

int main(int ArgumentCount, char **Arguments)
{
    assert(ArgumentCount == 2);
    RenderSize(Arguments[1], 1080U, 2340U);
    RenderSize(Arguments[1], 1920U, 1080U);
    puts("full-resolution glass keyframes rendered");
    return 0;
}
