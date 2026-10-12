/* Clover's boot entries are the activation targets; utilities form row two. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <lvgl.h>
#include <lvgl_port.h>
static void settle(kshim_lvgl_t *ui)
{ for (unsigned i=0; i<40; i++) kshim_lvgl_frame(ui,16); lv_refr_now(NULL); }
static void touch(kshim_lvgl_t *ui, lv_obj_t *obj, int cancel)
{
    lv_area_t r; lv_obj_get_coords(obj,&r);
    kshim_touch_event_t e={.type=KSHIM_TOUCH_EVENT_PRESS,
        .x=(uint32_t)((r.x1+r.x2)/2),.y=(uint32_t)((r.y1+r.y2)/2)};
    kshim_lvgl_touch(ui,&e);
    assert(kshim_lvgl_take_index(ui)==-1);
    e.type=cancel?KSHIM_TOUCH_EVENT_CANCEL:KSHIM_TOUCH_EVENT_RELEASE;
    kshim_lvgl_touch(ui,&e); settle(ui);
}
static uint32_t key_value;
static lv_indev_state_t key_state;
static void read_key(lv_indev_t *in, lv_indev_data_t *d)
{ (void)in; d->key=key_value; d->state=key_state; d->continue_reading=false; }
static void key(kshim_lvgl_t *ui, uint32_t value)
{
    lv_indev_set_read_cb(ui->Input,read_key); lv_indev_set_mode(ui->Input,LV_INDEV_MODE_EVENT);
    key_value=value; key_state=LV_INDEV_STATE_PRESSED; lv_indev_read(ui->Input);
    key_state=LV_INDEV_STATE_RELEASED; lv_indev_read(ui->Input); settle(ui);
}
static void click(kshim_lvgl_t *ui, lv_obj_t *b)
{ assert(b); assert(lv_obj_send_event(b,LV_EVENT_CLICKED,NULL)==LV_RESULT_OK); settle(ui); }
static void check(unsigned w,unsigned h)
{
    uint32_t *pixels=calloc((size_t)w*h,sizeof *pixels); assert(pixels);
    kshim_framebuffer_config_t c={.render_address=(uintptr_t)pixels,.width=w,.height=h,
      .stride=w*4,.bpp=32,.format=KSHIM_FB_FORMAT_XRGB8888,.buffer_size=(size_t)w*h*4};
    kshim_framebuffer_t fb; assert(!kshim_fb_init(&fb,&c));
    kshim_lvgl_t ui; assert(!kshim_lvgl_init(&ui,&fb,NULL));
    const char *names[]={"Android","Recovery","Linux"};
    assert(!kshim_lvgl_set_entries(&ui,names,3,0)); settle(&ui);
    assert(ui.BootButton==NULL && "Clover must not create a separate Boot button");
    assert(kshim_lvgl_take_index(&ui)==-1);
    lv_obj_t *second=lv_group_get_obj_by_index(ui.Group,1);
    lv_group_focus_obj(second); settle(&ui);
    assert(kshim_lvgl_take_index(&ui)==-1 && "focus is not activation");
    touch(&ui,second,1); assert(kshim_lvgl_take_index(&ui)==-1);
    touch(&ui,second,0); assert(kshim_lvgl_take_index(&ui)==1);
    assert(kshim_lvgl_take_index(&ui)==-1 && !ui.PowerDialog);
    key(&ui,LV_KEY_ENTER); assert(kshim_lvgl_take_index(&ui)==1);
    assert(!kshim_lvgl_set_power_capabilities(&ui,KSHIM_POWER_ALL)); settle(&ui);
    lv_obj_t *panel=lv_obj_get_parent(ui.List), *status=lv_obj_get_child(panel,1);
    lv_area_t list,r,o,s; lv_obj_get_coords(ui.List,&list);
    lv_obj_get_coords(ui.RestartButton,&r); lv_obj_get_coords(ui.PowerOffButton,&o);
    assert(r.y1>list.y2 && o.y1==r.y1 && r.x2<o.x1);
    assert(r.x1>=0 && o.x2<(int)w && o.y2<(int)h);
    assert(lv_obj_get_parent(ui.RestartButton)==panel && lv_obj_get_parent(ui.PowerOffButton)==panel);
    if(w>=720 && h>=720) {
        lv_obj_get_coords(status,&s); assert(s.y1>r.y2 && s.y1>o.y2);
        assert(lv_obj_has_flag(lv_obj_get_child(ui.RestartButton,0),LV_OBJ_FLAG_HIDDEN));
        assert(lv_obj_has_flag(lv_obj_get_child(ui.PowerOffButton,0),LV_OBJ_FLAG_HIDDEN));
    }
    key(&ui,LV_KEY_NEXT); assert(kshim_lvgl_take_index(&ui)==-1);
    click(&ui,ui.RestartButton); assert(ui.PowerDialog);
    assert(lv_group_get_focused(lv_indev_get_group(ui.Input))==ui.PowerCancelButton);
    second=lv_group_get_obj_by_index(ui.Group,1);
    click(&ui,second); assert(kshim_lvgl_take_index(&ui)==-1);
    click(&ui,ui.PowerCancelButton); assert(!ui.PowerDialog);
    click(&ui,ui.PowerOffButton); assert(ui.PowerConfirmButton);
    touch(&ui,ui.PowerConfirmButton,1); assert(!kshim_lvgl_take_power(&ui));
    click(&ui,ui.PowerCancelButton); assert(!ui.PowerDialog);
    const char *many[49]; for(unsigned i=0;i<49;i++)many[i]="Boot image";
    assert(!kshim_lvgl_set_entries(&ui,many,49,0)); settle(&ui);
    assert(!ui.BootButton && lv_group_get_obj_count(ui.Group)==51);
    lv_obj_t *first=lv_group_get_obj_by_index(ui.Group,0);
    lv_area_t f;lv_obj_get_coords(first,&f);
    kshim_touch_event_t drag={.type=KSHIM_TOUCH_EVENT_PRESS,
        .x=(uint32_t)((f.x1+f.x2)/2),.y=(uint32_t)((f.y1+f.y2)/2)};
    kshim_lvgl_touch(&ui,&drag);
    for(unsigned i=0;i<5;i++) {
        drag.type=KSHIM_TOUCH_EVENT_MOVE;
        if(lv_obj_get_scroll_dir(ui.List)==LV_DIR_HOR)drag.x=drag.x>15?drag.x-15:0;
        else drag.y=drag.y>15?drag.y-15:0;
        kshim_lvgl_touch(&ui,&drag); kshim_lvgl_frame(&ui,16);
    }
    drag.type=KSHIM_TOUCH_EVENT_RELEASE;kshim_lvgl_touch(&ui,&drag);settle(&ui);
    assert(kshim_lvgl_take_index(&ui)==-1 && "scrolling must never activate a boot entry");
    lv_obj_get_coords(ui.PowerOffButton,&o);
    lv_group_focus_obj(lv_group_get_obj_by_index(ui.Group,48));settle(&ui);
    lv_obj_get_coords(ui.PowerOffButton,&s);assert(!memcmp(&s,&o,sizeof s));
    assert(!kshim_lvgl_set_entries(&ui,names,3,0));settle(&ui);
    assert(!ui.BootButton && kshim_lvgl_take_index(&ui)==-1);
    kshim_lvgl_deinit(&ui);free(pixels);
}
int main(void)
{
    check(240,160);check(320,240);check(360,800);check(1080,2340);check(1920,1080);
    puts("PASS: Clover direct entry activation, separate utility row, no Boot button, safe focus/scroll/cancel and modal isolation");
}
