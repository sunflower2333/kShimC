/* SPDX-License-Identifier: MIT */
#include <assert.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <lvgl.h>
#include <lvgl_port.h>
#include "../src/ui/menu_design.h"
static int integrated(void) {
    return mMenuDesign.kind==KSHIM_DESIGN_FLUENT2 || mMenuDesign.kind==KSHIM_DESIGN_MATERIAL3 ||
           mMenuDesign.kind==KSHIM_DESIGN_TERMINAL || mMenuDesign.kind==KSHIM_DESIGN_CLOVER;
}
static int separate(lv_area_t a,lv_area_t b) {
    return a.x2<b.x1 || b.x2<a.x1 || a.y2<b.y1 || b.y2<a.y1;
}
static void frame(kshim_lvgl_t *u){for(unsigned i=0;i<32;i++)kshim_lvgl_frame(u,16);lv_refr_now(NULL);}
static uint32_t key_value;
static lv_indev_state_t key_state;
static void read_key(lv_indev_t *i,lv_indev_data_t *d)
{(void)i;d->key=key_value;d->state=key_state;d->continue_reading=false;}
static void key(kshim_lvgl_t *u,uint32_t value)
{
    lv_indev_set_read_cb(u->Input,read_key);lv_indev_set_mode(u->Input,LV_INDEV_MODE_EVENT);
    key_value=value;key_state=LV_INDEV_STATE_PRESSED;lv_indev_read(u->Input);
    key_state=LV_INDEV_STATE_RELEASED;lv_indev_read(u->Input);frame(u);
}
static void click(kshim_lvgl_t *u,void *b){assert(b);lv_obj_send_event(b,LV_EVENT_CLICKED,NULL);frame(u);}
#ifdef POWER_RED
int kshim_lvgl_set_power_capabilities(kshim_lvgl_t *u,uint32_t c){(void)u;(void)c;return -1;}
kshim_power_action_t kshim_lvgl_take_power(kshim_lvgl_t *u){(void)u;return 0;}
#endif
static void check(unsigned w,unsigned h){
    uint32_t *pixels=calloc((size_t)w*h,4);assert(pixels);
    kshim_framebuffer_t f; kshim_framebuffer_config_t c={.render_address=(uintptr_t)pixels,.width=w,.height=h,.stride=w*4,.format=KSHIM_FB_FORMAT_XRGB8888,.bpp=32,.buffer_size=(size_t)w*h*4};
    assert(!kshim_fb_init(&f,&c));kshim_lvgl_t u;assert(!kshim_lvgl_init(&u,&f,NULL));
    const char *names[]={"Android","Recovery","Linux"};assert(!kshim_lvgl_set_entries(&u,names,3,1));
    assert(!kshim_lvgl_set_power_capabilities(&u,KSHIM_POWER_ALL));frame(&u);
    lv_area_t a,b,p;lv_obj_get_coords(u.RestartButton,&a);lv_obj_get_coords(u.PowerOffButton,&b);lv_obj_get_coords(lv_obj_get_parent(u.List),&p);
    assert(a.x1>=0&&b.x2<(int)w&&a.y1>=0&&b.y2<(int)h);
    if(integrated()) {
        assert(lv_obj_get_parent(u.RestartButton)==lv_obj_get_parent(u.List));
        assert(lv_obj_get_parent(u.PowerOffButton)==lv_obj_get_parent(u.List));
        lv_area_t list,boot;lv_obj_get_coords(u.List,&list);
        if(u.BootButton)lv_obj_get_coords(u.BootButton,&boot);
        if(!(a.x1>=p.x1&&a.y1>=p.y1&&a.x2<=p.x2&&a.y2<=p.y2))
            fprintf(stderr,"%ux%u panel=%d,%d,%d,%d restart=%d,%d,%d,%d\n",w,h,p.x1,p.y1,p.x2,p.y2,a.x1,a.y1,a.x2,a.y2);
        assert(a.x1>=p.x1&&a.y1>=p.y1&&a.x2<=p.x2&&a.y2<=p.y2);
        assert(b.x1>=p.x1&&b.y1>=p.y1&&b.x2<=p.x2&&b.y2<=p.y2);
        assert(separate(a,b)&&separate(a,list)&&separate(b,list));
        if(mMenuDesign.kind==KSHIM_DESIGN_CLOVER) {
            assert(!u.BootButton && a.y1>list.y2 && b.y1==a.y1);
        }else assert(separate(a,boot)&&separate(b,boot));
        if(w>=720)assert(lv_area_get_width(&a)<(int)w/3&&lv_area_get_width(&b)<(int)w/3);
    } else assert(p.y2<a.y1);
    assert(lv_group_get_obj_count(u.Group)==5);
    kshim_lvgl_set_countdown(&u,500);click(&u,u.RestartButton);
    assert(u.PowerDialog&&u.AutobootCancelled&&!u.CountdownEnabled);assert(kshim_lvgl_take_index(&u)==-1);
    assert(lv_indev_get_group(u.Input)!=u.Group);assert(!kshim_lvgl_take_power(&u));
    for(unsigned i=0;i<3;i++){
        assert(u.PowerModeButtons[i]);
        if(w>=720&&h>=720){
            lv_area_t row,visible;lv_obj_get_coords(u.PowerModeButtons[i],&row);
            lv_obj_get_content_coords(lv_obj_get_parent(u.PowerModeButtons[i]),&visible);
            assert(row.y1>=visible.y1&&row.y2<=visible.y2);
            lv_obj_t *label=lv_obj_get_child(u.PowerModeButtons[i],0);
            assert(lv_obj_get_width(label)>=80 && "mode label is clipped before the layout resolves");
        }
    }
    click(&u,u.PowerModeButtons[2]);assert(u.PowerConfirmButton&&u.PowerCancelButton);
    assert(lv_group_get_focused(lv_indev_get_group(u.Input))==u.PowerCancelButton);
    if(integrated())assert(lv_obj_get_style_outline_width(u.PowerCancelButton,LV_PART_MAIN)>0 &&
        "Cancel-first keyboard focus must have a visible cue");
    click(&u,u.PowerCancelButton);assert(!u.PowerDialog&&!kshim_lvgl_take_power(&u));
    if(integrated()) {
        click(&u,u.RestartButton);
        click(&u,u.PowerDialog); /* Outside dismiss never reaches the boot list. */
        assert(!u.PowerDialog&&!kshim_lvgl_take_power(&u)&&kshim_lvgl_take_index(&u)==-1);
    }
    click(&u,u.PowerOffButton);assert(!kshim_lvgl_take_power(&u));
    /* Real pointer cancel must not confirm. */
    lv_area_t confirm;lv_obj_get_coords(u.PowerConfirmButton,&confirm);
    kshim_touch_event_t ev={.type=KSHIM_TOUCH_EVENT_PRESS,.x=(uint32_t)(confirm.x1+confirm.x2)/2,.y=(uint32_t)(confirm.y1+confirm.y2)/2};
    kshim_lvgl_touch(&u,&ev);ev.type=KSHIM_TOUCH_EVENT_CANCEL;kshim_lvgl_touch(&u,&ev);frame(&u);assert(!kshim_lvgl_take_power(&u));
    click(&u,u.PowerCancelButton);
    for(unsigned i=0;i<3;i++) {
        click(&u,u.RestartButton);click(&u,u.PowerModeButtons[i]);click(&u,u.PowerConfirmButton);
        assert(kshim_lvgl_take_power(&u)==(kshim_power_action_t)(KSHIM_POWER_WARM+i));
        assert(!kshim_lvgl_take_power(&u)&&kshim_lvgl_take_index(&u)==-1);
    }
    /* Volume-key navigation reaches the controls and defaults every modal to Cancel. */
    lv_group_focus_obj(u.RestartButton);
    key(&u,LV_KEY_ENTER);
    assert(u.PowerDialog);lv_group_t *modal=lv_indev_get_group(u.Input);
    assert(lv_group_get_focused(modal)==u.PowerCancelButton);
    key(&u,LV_KEY_ENTER);
    assert(!u.PowerDialog&&!kshim_lvgl_take_power(&u));
    /* Long lists keep the power bar fixed and restore the selected boot item. */
    const char *many[49];for(unsigned i=0;i<49;i++)many[i]="A long operating system image name";
    assert(!kshim_lvgl_set_entries(&u,many,49,48));frame(&u);
    assert(lv_group_get_obj_count(u.Group)==51);lv_area_t fixed;lv_obj_get_coords(u.PowerOffButton,&fixed);
    if(!integrated())assert(fixed.y1==b.y1&&fixed.y2==b.y2);
    /* Selection changes must not move the tools. Rebuilds may resize their panel. */
    lv_group_focus_obj(lv_group_get_obj_by_index(u.Group,0));frame(&u);
    lv_area_t stable;lv_obj_get_coords(u.PowerOffButton,&stable);
    assert(memcmp(&fixed,&stable,sizeof fixed)==0);
    click(&u,u.PowerOffButton);click(&u,u.PowerConfirmButton);assert(kshim_lvgl_take_power(&u)==KSHIM_POWER_OFF);
    assert(!kshim_lvgl_set_power_capabilities(&u,KSHIM_POWER_BIT(KSHIM_POWER_OFF)));frame(&u);
    assert(lv_obj_has_state(u.RestartButton,LV_STATE_DISABLED));click(&u,u.RestartButton);assert(!u.PowerDialog);
    assert(!kshim_lvgl_set_entries(&u,names,3,0));frame(&u);
    click(&u,u.BootButton?u.BootButton:lv_group_get_obj_by_index(u.Group,0));
    assert(kshim_lvgl_take_index(&u)==0);
    kshim_lvgl_deinit(&u);free(pixels);
}
int main(void){check(240,160);check(320,240);check(360,800);check(1080,2340);check(1920,1080);puts("PASS: power menu capability gate, safe modal focus, confirmation/cancel, countdown and boot independence");}
