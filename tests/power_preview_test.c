/* SPDX-License-Identifier: MIT
 * Same framebuffer/PPM machinery as the existing authenticity fixture. This
 * executable only simulates hardware capabilities: it never resets a machine.
 */
#define main kshim_legacy_preview_main
#include "ui_preview_test.c"
#undef main
static void press(kshim_lvgl_t *u,void *button)
{ assert(button);assert(lv_obj_send_event(button,LV_EVENT_CLICKED,NULL)==LV_RESULT_OK);Advance(u,400); }
static void power_size(const char *directory,uint32_t width,uint32_t height)
{
    size_t bytes=(size_t)width*height*4;uint32_t *pixels=calloc(1,bytes);assert(pixels);
    kshim_framebuffer_config_t cfg={.render_address=(uintptr_t)pixels,.width=width,.height=height,
        .stride=width*4,.bpp=32,.format=KSHIM_FB_FORMAT_XRGB8888,.buffer_size=bytes};
    kshim_framebuffer_t fb;assert(!kshim_fb_init(&fb,&cfg));
    static uint8_t scratch[4U<<20] __attribute__((aligned(64)));
    kshim_lvgl_set_scratch(scratch,sizeof scratch);kshim_lvgl_t ui;assert(!kshim_lvgl_init(&ui,&fb,NULL));
    const char *entries[]={"Android","UEFI 诊断","Recovery 恢复模式","Factory image","Network rescue"};
    assert(!kshim_lvgl_set_entries(&ui,entries,5,0));
    assert(!kshim_lvgl_set_power_capabilities(&ui,KSHIM_POWER_ALL));Advance(&ui,400);
    WritePpm(directory,"power-controls",&fb);
    press(&ui,ui.RestartButton);assert(ui.PowerDialog);WritePpm(directory,"restart-options",&fb);
    press(&ui,ui.PowerModeButtons[2]);assert(ui.PowerConfirmButton);WritePpm(directory,"edl-confirm",&fb);
    press(&ui,ui.PowerCancelButton);press(&ui,ui.PowerOffButton);WritePpm(directory,"shutdown-confirm",&fb);
    assert(!kshim_lvgl_take_power(&ui)&&kshim_lvgl_take_index(&ui)==-1);
    kshim_lvgl_deinit(&ui);free(pixels);
}
int main(int argc,char **argv)
{
    assert(argc==2);power_size(argv[1],1920,1080);power_size(argv[1],1080,2340);
    puts("PASS: themed power controls, restart menu and two confirmation frames; no hardware operations");return 0;
}
