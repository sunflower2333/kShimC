#ifndef KSHIM_MENU_POWER_H
#define KSHIM_MENU_POWER_H
/* Presentation only: callbacks enqueue intentions; firmware calls are owned by
 * the platform loop after quiescing input and rendering. */
static lv_obj_t *mPowerOverlay, *mPowerReturnFocus;
static lv_group_t *mPowerGroup;
static unsigned mPowerCommand;
static kshim_power_action_t mPowerChoice;
enum { POWER_RESTART=1, POWER_OFF, POWER_WARM, POWER_HARD, POWER_EDL, POWER_CANCEL, POWER_CONFIRM };
#include "menu_power_profile.h"
#include "menu_power_design.h"
#include "menu_power_extended.h"
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
    for(unsigned i=0;i<2;i++) {
        lv_obj_t *b=buttons[i];if(!b)continue;
        lv_group_remove_obj(b);
        if(mContext->PowerCapabilities&masks[i]){lv_obj_remove_state(b,LV_STATE_DISABLED);lv_group_add_obj(mContext->Group,b);}
        else lv_obj_add_state(b,LV_STATE_DISABLED);
    }
    if(KshimPowerReviewed())KshimPowerThemeReattach();
    else KshimPowerExtendedReattach();
}
static int KshimPowerDialog(kshim_power_action_t action)
{
    if(!mPowerOverlay)mPowerReturnFocus=lv_group_get_focused(mContext->Group);
    KshimPowerDestroy(0);
    return KshimPowerReviewed()?KshimPowerThemeDialog(action):KshimPowerExtendedDialog(action);
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
