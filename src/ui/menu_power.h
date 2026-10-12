#ifndef KSHIM_MENU_POWER_H
#define KSHIM_MENU_POWER_H
/* Presentation only: callbacks enqueue intentions; firmware calls are owned by
 * the platform loop after quiescing input and rendering. */
static lv_obj_t *mPowerOverlay, *mPowerReturnFocus;
static lv_group_t *mPowerGroup;
static unsigned mPowerCommand;
static kshim_power_action_t mPowerChoice;
enum { POWER_RESTART=1, POWER_OFF, POWER_WARM, POWER_HARD, POWER_EDL, POWER_CANCEL, POWER_CONFIRM };
#include "menu_power_design.h"
static void KshimCancelAutoboot(void)
{
    if(!mContext)return;
    mContext->AutobootCancelled=1;mContext->CountdownEnabled=0;
}
static void KshimPowerEvent(lv_event_t *e)
{
    if(!mContext||!mContext->PowerUiEnabled)return;
    lv_obj_t *button=lv_event_get_target_obj(e);
    lv_event_code_t code=lv_event_get_code(e);
    if(code==LV_EVENT_KEY && lv_event_get_key(e)==LV_KEY_ESC){mPowerCommand=POWER_CANCEL;return;}
    if(code==LV_EVENT_FOCUSED && KSHIM_MENU_IS_NATIVE)lv_obj_add_state(button,LV_STATE_FOCUS_KEY);
    if(code==LV_EVENT_DEFOCUSED && KSHIM_MENU_IS_NATIVE)lv_obj_remove_state(button,LV_STATE_FOCUS_KEY);
    if(mMenuDesign.kind==KSHIM_DESIGN_CLOVER && !mPowerOverlay && mStatus) {
        unsigned command=(unsigned)(uintptr_t)lv_event_get_user_data(e);
        if(code==LV_EVENT_FOCUSED && (command==POWER_RESTART || command==POWER_OFF))
            lv_label_set_text(mStatus,command==POWER_RESTART?"Restart this device":"Power off this device");
        if(code==LV_EVENT_DEFOCUSED) KshimSelectionStatus(mContext->SelectedIndex);
    }
    if(code!=LV_EVENT_CLICKED||lv_obj_has_state(button,LV_STATE_DISABLED))return;
    KshimCancelAutoboot();
    mPowerCommand=(unsigned)(uintptr_t)lv_event_get_user_data(e);
}
static lv_obj_t *KshimPowerButton(lv_obj_t *parent,const char *text,unsigned command,lv_group_t *group)
{
    lv_obj_t *b=lv_button_create(parent);if(!b)return NULL;
    lv_group_remove_obj(b);
    lv_obj_t *label=lv_label_create(b);if(!label){lv_obj_delete(b);return NULL;}
    lv_label_set_text(label,text);lv_label_set_long_mode(label,LV_LABEL_LONG_DOT);
    lv_obj_set_width(label,LV_PCT(100));lv_obj_set_style_text_align(label,LV_TEXT_ALIGN_CENTER,0);lv_obj_center(label);
    if(!KSHIM_MENU_IS_NATIVE) {
        /* Always use paired role colors. Never inherit white text onto a white
         * button (the unreadable initial prototype's failure). */
        lv_obj_set_style_bg_color(b,lv_color_hex(mMenuStyle.Boot),0);
        lv_obj_set_style_bg_opa(b,LV_OPA_COVER,0);
        lv_obj_set_style_text_color(b,lv_color_hex(mMenuStyle.BootText),0);
        lv_obj_set_style_text_font(b,mStatusFont,0);
        lv_obj_set_style_border_color(b,lv_color_hex(mMenuStyle.Border),0);
        lv_obj_set_style_border_width(b,1,0);lv_obj_set_style_shadow_width(b,0,0);
        lv_obj_set_style_radius(b,mMenuDesign.enabled?mMenuDesign.boot_radius==255?LV_RADIUS_CIRCLE:mMenuDesign.boot_radius:8,0);
        lv_obj_set_style_pad_all(b,6,0);
        const lv_style_selector_t states[]={LV_STATE_FOCUSED,LV_STATE_PRESSED};
        for(unsigned i=0;i<2;i++){
            lv_obj_set_style_bg_color(b,lv_color_hex(mMenuStyle.Focus),states[i]);
            lv_obj_set_style_text_color(b,lv_color_hex(mMenuStyle.FocusText),states[i]);
            lv_obj_set_style_outline_width(b,2,states[i]);
            lv_obj_set_style_outline_color(b,lv_color_hex(mMenuStyle.Border),states[i]);
        }
        lv_obj_set_style_opa(b,LV_OPA_50,LV_STATE_DISABLED);
    }
    lv_obj_add_event_cb(b,KshimPowerEvent,LV_EVENT_ALL,(void *)(uintptr_t)command);
    if(group)lv_group_add_obj(group,b);
    return b;
}
static void KshimPowerDestroy(int restore_focus)
{
    if(mKeyInput&&mContext)lv_indev_set_group(mKeyInput,mContext->Group);
    if(mPowerOverlay)lv_obj_delete(mPowerOverlay);
    if(mPowerGroup)lv_group_delete(mPowerGroup);
    mPowerOverlay=NULL;mPowerGroup=NULL;
    if(mContext){mContext->PowerDialog=NULL;mContext->PowerConfirmButton=NULL;mContext->PowerCancelButton=NULL;
        for(unsigned i=0;i<3;i++)mContext->PowerModeButtons[i]=NULL;}
    if(restore_focus&&mPowerReturnFocus)lv_group_focus_obj(mPowerReturnFocus);
    if(restore_focus)mPowerReturnFocus=NULL;
}
static void KshimPowerClose(void){KshimPowerDestroy(1);mPowerChoice=KSHIM_POWER_NONE;mPowerCommand=0;}
static void KshimPowerReattach(void)
{
    if(!mContext||!mContext->PowerUiEnabled)return;
    lv_obj_t *buttons[]={mContext->RestartButton,mContext->PowerOffButton};
    unsigned masks[]={KSHIM_POWER_ALL&~KSHIM_POWER_BIT(KSHIM_POWER_OFF),KSHIM_POWER_BIT(KSHIM_POWER_OFF)};
    int32_t w=mFramebuffer->width,h=mFramebuffer->height;
    int32_t reserve=(int32_t)KshimPowerReserve(),gap=6;
    for(unsigned i=0;i<2;i++) {
        lv_obj_t *b=buttons[i];if(!b)continue;
        lv_group_remove_obj(b);
        if(mContext->PowerCapabilities&masks[i]){lv_obj_remove_state(b,LV_STATE_DISABLED);lv_group_add_obj(mContext->Group,b);}
        else lv_obj_add_state(b,LV_STATE_DISABLED);
        if(!KshimPowerIntegrated()) {
            lv_obj_set_pos(b,gap+(int32_t)i*(w-gap)/2,h-reserve+gap);
            lv_obj_set_size(b,(w-3*gap)/2,reserve-2*gap);
        }
    }
    if(KshimPowerIntegrated())KshimPowerThemeReattach();
}
static int KshimPowerDialog(kshim_power_action_t action)
{
    if(!mPowerOverlay)mPowerReturnFocus=lv_group_get_focused(mContext->Group);
    KshimPowerDestroy(0);
    if(KshimPowerIntegrated())return KshimPowerThemeDialog(action);
    mPowerGroup=lv_group_create();if(!mPowerGroup)return -1;
    mPowerOverlay=lv_obj_create(lv_layer_top());if(!mPowerOverlay)return -1;
    lv_obj_set_size(mPowerOverlay,LV_PCT(100),LV_PCT(100));lv_obj_set_pos(mPowerOverlay,0,0);
    lv_obj_remove_flag(mPowerOverlay,LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_pad_all(mPowerOverlay,0,0);lv_obj_set_style_border_width(mPowerOverlay,0,0);
    lv_obj_set_style_radius(mPowerOverlay,0,0);lv_obj_set_style_bg_color(mPowerOverlay,lv_color_black(),0);
    lv_obj_set_style_bg_opa(mPowerOverlay,LV_OPA_60,0);
    lv_obj_t *box=lv_obj_create(mPowerOverlay);if(!box)return -1;
    int32_t w=mFramebuffer->width,h=mFramebuffer->height;
    int32_t font=lv_font_get_line_height(mStatusFont),row=font+20;
    if(row<36)row=36;
    lv_obj_set_size(box,min_u32(w-16U,960U),min_u32(h-16U,(uint32_t)(action?row*5+32:row*8+48)));
    lv_obj_center(box);lv_obj_remove_flag(box,LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(box,LV_FLEX_FLOW_COLUMN);lv_obj_set_style_pad_all(box,12,0);lv_obj_set_style_pad_row(box,10,0);
    if(!KSHIM_MENU_IS_NATIVE){
        lv_obj_set_style_bg_color(box,lv_color_hex(mMenuStyle.Panel),0);lv_obj_set_style_bg_opa(box,LV_OPA_COVER,0);
        lv_obj_set_style_text_color(box,lv_color_hex(mMenuStyle.Text),0);lv_obj_set_style_text_font(box,mStatusFont,0);
        lv_obj_set_style_border_color(box,lv_color_hex(mMenuStyle.Border),0);lv_obj_set_style_border_width(box,1,0);
        lv_obj_set_style_radius(box,mMenuDesign.enabled?mMenuDesign.panel_radius:12,0);
    }
    lv_obj_t *title=lv_label_create(box),*body=lv_label_create(box),*choices=lv_obj_create(box);
    if(!title||!body||!choices)return -1;
    const char *names[]={"", "Power off", "Warm restart", "Hard restart", "Enter EDL"};
    lv_label_set_text(title,action?names[action]:"Restart options");lv_obj_set_width(title,LV_PCT(100));
    lv_label_set_long_mode(title,LV_LABEL_LONG_DOT);
    lv_label_set_text(body,action==KSHIM_POWER_EDL?"USB emergency download mode. A computer may be needed to exit.":
        action==KSHIM_POWER_OFF?"Shut down this device?":action?"Restart this device now?":"Choose a mode. Unavailable modes are disabled.");
    lv_obj_set_width(body,LV_PCT(100));lv_label_set_long_mode(body,LV_LABEL_LONG_WRAP);
    if(h<320)lv_obj_add_flag(body,LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_width(choices,LV_PCT(100));lv_obj_set_height(choices,1);lv_obj_set_flex_grow(choices,1);
    lv_obj_set_style_pad_all(choices,3,0);lv_obj_set_style_pad_row(choices,8,0);lv_obj_set_style_border_width(choices,0,0);
    lv_obj_set_style_bg_opa(choices,LV_OPA_TRANSP,0);lv_obj_set_flex_flow(choices,LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scroll_dir(choices,LV_DIR_VER);
    if(!action)for(unsigned i=0;i<3;i++) {
        lv_obj_t *b=KshimPowerButton(choices,names[i+2],POWER_WARM+i,NULL);if(!b)return -1;
        lv_obj_set_width(b,LV_PCT(100));lv_obj_set_height(b,row);
        mContext->PowerModeButtons[i]=b;
        if(mContext->PowerCapabilities&KSHIM_POWER_BIT(i+2))lv_group_add_obj(mPowerGroup,b);
        else lv_obj_add_state(b,LV_STATE_DISABLED);
    }
    lv_obj_t *buttons=lv_obj_create(box);if(!buttons)return -1;
    lv_obj_set_size(buttons,LV_PCT(100),row+6);lv_obj_set_style_pad_all(buttons,3,0);lv_obj_set_style_pad_column(buttons,10,0);
    lv_obj_set_style_border_width(buttons,0,0);lv_obj_set_style_bg_opa(buttons,LV_OPA_TRANSP,0);
    lv_obj_remove_flag(buttons,LV_OBJ_FLAG_SCROLLABLE);lv_obj_set_flex_flow(buttons,LV_FLEX_FLOW_ROW);
    lv_obj_t *cancel=KshimPowerButton(buttons,"Cancel",POWER_CANCEL,mPowerGroup);if(!cancel)return -1;
    lv_obj_set_height(cancel,LV_PCT(100));lv_obj_set_flex_grow(cancel,1);mContext->PowerCancelButton=cancel;
    if(action){lv_obj_t *confirm=KshimPowerButton(buttons,"Confirm",POWER_CONFIRM,mPowerGroup);if(!confirm)return -1;
        lv_obj_set_height(confirm,LV_PCT(100));lv_obj_set_flex_grow(confirm,1);mContext->PowerConfirmButton=confirm;}
    mContext->PowerDialog=mPowerOverlay;mPowerChoice=action;
    lv_indev_set_group(mKeyInput,mPowerGroup);lv_group_focus_obj(cancel);
    lv_obj_update_layout(lv_screen_active());return 0;
}
static void KshimPowerProcess(void)
{
    if(!mContext||!mPowerCommand)return;
    unsigned command=mPowerCommand;mPowerCommand=0;
    if(command==POWER_CANCEL){KshimPowerClose();return;}
    kshim_power_action_t action=command==POWER_OFF?KSHIM_POWER_OFF:
        command>=POWER_WARM&&command<=POWER_EDL?(kshim_power_action_t)(command-1):KSHIM_POWER_NONE;
    if(command==POWER_CONFIRM) {
        if(mPowerOverlay&&mPowerChoice&&(mContext->PowerCapabilities&KSHIM_POWER_BIT(mPowerChoice))) {
            mContext->PendingPower=mPowerChoice;mContext->PendingIndex=-1;mContext->PendingAction=KSHIM_MENU_NONE;
            KshimPowerClose();
        }
        return;
    }
    if(command==POWER_RESTART && !(mContext->PowerCapabilities&(KSHIM_POWER_ALL&~KSHIM_POWER_BIT(KSHIM_POWER_OFF))))return;
    if(action && !(mContext->PowerCapabilities&KSHIM_POWER_BIT(action)))return;
    if(KshimPowerDialog(action)){KshimPowerClose();kshim_lvgl_set_status(mContext,"Unable to open power menu");}
}
int kshim_lvgl_set_power_capabilities(kshim_lvgl_t *c,uint32_t mask)
{
    if(!kshim_lvgl_ready(c)||mFramebuffer->width<240||mFramebuffer->height<160)return -1;
    KshimPowerClose();c->PowerCapabilities=mask&KSHIM_POWER_ALL;
    if(!c->RestartButton)c->RestartButton=KshimPowerButton(lv_screen_active(),"Restart",POWER_RESTART,NULL);
    if(!c->PowerOffButton)c->PowerOffButton=KshimPowerButton(lv_screen_active(),"Power off",POWER_OFF,NULL);
    if(!c->RestartButton||!c->PowerOffButton){
        if(c->RestartButton)lv_obj_delete(c->RestartButton);
        if(c->PowerOffButton)lv_obj_delete(c->PowerOffButton);
        c->RestartButton=NULL;c->PowerOffButton=NULL;c->PowerUiEnabled=0;return -1;
    }
    c->PowerUiEnabled=1;return KshimRebuildEntries(c->SelectedIndex);
}
kshim_power_action_t kshim_lvgl_take_power(kshim_lvgl_t *c)
{
    if(!kshim_lvgl_ready(c))return KSHIM_POWER_NONE;
    kshim_power_action_t action=c->PendingPower;c->PendingPower=KSHIM_POWER_NONE;return action;
}
#endif
