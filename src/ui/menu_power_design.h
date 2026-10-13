#ifndef KSHIM_MENU_POWER_DESIGN_H
#define KSHIM_MENU_POWER_DESIGN_H
/* Reviewed presentation adapters. They never execute power operations or change the
 * common capability/confirmation state machine. Dimensions are design units. */
enum { POWER_VIEW_ENTRY=1, POWER_VIEW_ROW, POWER_VIEW_CANCEL, POWER_VIEW_CONFIRM };
static lv_obj_t *KshimPowerButton(lv_obj_t *,const char *,unsigned,lv_group_t *);
static int32_t KshimPowerPx(uint32_t);
static void KshimPowerGlyphDraw(lv_event_t *e)
{
    lv_obj_t *b=lv_event_get_target_obj(e);
    unsigned data=(unsigned)(uintptr_t)lv_event_get_user_data(e),command=data&255U,role=data>>8;
    int tile=(role&16U)!=0;role&=15U;
    int clover=mMenuDesign.kind==KSHIM_DESIGN_CLOVER;
    lv_area_t a;lv_obj_get_coords(b,&a);
    int32_t w=lv_obj_get_width(b),h=lv_obj_get_height(b);
    int32_t size=KshimPowerPx(tile?32:clover?(role==POWER_VIEW_ROW?48:24):20);
    size=kshim_design_min(size,kshim_design_min(w-8,h-8));if(size<8)return;
    int32_t x=a.x1+KshimPowerPx(12),y=a.y1+(h-size)/2;
    if(role==POWER_VIEW_ENTRY&&KshimPowerProfile().icon_entry==1)x=a.x1+(w-size)/2;
    if(clover||tile){x=a.x1+(w-size)/2;y=role==POWER_VIEW_ENTRY?a.y1+(h-size)/2:a.y1+KshimPowerPx(tile?12:18);}
    uint32_t ink=lv_color_to_u32(lv_obj_get_style_text_color(b,LV_PART_MAIN))&0xffffffU;
    lv_opa_t opa=lv_obj_get_style_opa_recursive(b,LV_PART_MAIN);
    int32_t stroke=KshimDesignStroke(clover?2:1);if(stroke<2&&size>=24)stroke=2;
    if(command==POWER_EDL){
        int32_t cx=x+size/2;
        KshimSymbolLine(e,ink,opa,cx,y+size/8,cx,y+size*7/8,stroke,true);
        KshimSymbolLine(e,ink,opa,cx,y+size/8,cx-size/7,y+size/3,stroke,true);
        KshimSymbolLine(e,ink,opa,cx,y+size/8,cx+size/7,y+size/3,stroke,true);
        KshimSymbolLine(e,ink,opa,cx,y+size*3/4,x+size/5,y+size/2,stroke,true);
        KshimSymbolLine(e,ink,opa,x+size/5,y+size/2,x+size/5,y+size/3,stroke,true);
        KshimSymbolLine(e,ink,opa,cx,y+size*2/3,x+size*4/5,y+size/2,stroke,true);
        KshimSymbolRect(e,ink,opa,x+size*3/4,y+size/3,size/7,size/7,0,0);
        KshimSymbolRect(e,ink,opa,cx-size/10,y+size*4/5,size/5,size/5,size/10,0);
    }else{
        lv_draw_arc_dsc_t d;lv_draw_arc_dsc_init(&d);
        d.color=lv_color_hex(ink);d.opa=opa;d.width=stroke;d.rounded=1;
        d.center=(lv_point_t){x+size/2,y+size/2};d.radius=(uint16_t)(size*3/8);
        d.start_angle=command==POWER_OFF?315:40;d.end_angle=command==POWER_OFF?225:310;
        lv_draw_arc(lv_event_get_layer(e),&d);
        if(command==POWER_OFF)KshimSymbolLine(e,ink,opa,x+size/2,y,x+size/2,y+size/2,stroke,true);
        else{
            KshimSymbolLine(e,ink,opa,x+size*3/4,y+size/12,x+size*3/4,y+size/3,stroke,true);
            KshimSymbolLine(e,ink,opa,x+size*3/4,y+size/3,x+size/2,y+size/3,stroke,true);
            if(command==POWER_HARD)KshimSymbolLine(e,ink,opa,x+size/3,y+size/2,x+size*2/3,y+size/2,stroke,true);
        }
    }
    if(clover&&lv_obj_has_state(b,LV_STATE_FOCUSED))
        KshimSymbolCorners(e,mMenuStyle.FocusBorder,opa,a.x1+2,a.y1+2,w-4,h-4,KshimPowerPx(7),stroke);
}
static void KshimPowerThemeStyle(lv_obj_t *b,unsigned command,unsigned role)
{
    lv_obj_t *label=lv_obj_get_child(b,0);if(!label)return;
    unsigned kind=mMenuDesign.kind;
    int terminal=kind==KSHIM_DESIGN_TERMINAL,clover=kind==KSHIM_DESIGN_CLOVER;
    int material=kind==KSHIM_DESIGN_MATERIAL3,primary=role==POWER_VIEW_CONFIRM;
    int icon_tile=clover&&(role==POWER_VIEW_ENTRY||role==POWER_VIEW_ROW);
    lv_obj_remove_event_cb(b,KshimPowerGlyphDraw);
    int32_t x=lv_obj_get_style_x(b,0),y=lv_obj_get_style_y(b,0);
    int32_t width=lv_obj_get_style_width(b,0),height=lv_obj_get_style_height(b,0);
    lv_obj_remove_style_all(b);lv_obj_set_layout(b,0);
    lv_obj_set_pos(b,x,y);lv_obj_set_size(b,width,height);
    lv_obj_set_style_text_font(b,mStatusFont,0);
    lv_obj_set_style_text_color(b,lv_color_hex(primary?mMenuStyle.BootText:mMenuStyle.Text),0);
    lv_obj_set_style_bg_color(b,lv_color_hex(primary?mMenuStyle.Boot:material?mMenuStyle.Focus:mMenuStyle.Panel),0);
    lv_obj_set_style_bg_opa(b,primary||(material&&role==POWER_VIEW_ENTRY)?LV_OPA_COVER:LV_OPA_TRANSP,0);
    lv_obj_set_style_radius(b,terminal?0:material?LV_RADIUS_CIRCLE:KshimDesignPx(clover?8:4),0);
    lv_obj_set_style_pad_all(b,0,0);
    lv_obj_set_style_border_width(b,role==POWER_VIEW_CANCEL&&!material&&!terminal?1:0,0);
    lv_obj_set_style_border_color(b,lv_color_hex(mMenuStyle.Border),0);
    lv_obj_set_style_bg_color(b,lv_color_hex(mMenuStyle.Focus),LV_STATE_FOCUSED);
    lv_obj_set_style_bg_opa(b,icon_tile?LV_OPA_TRANSP:LV_OPA_COVER,LV_STATE_FOCUSED);
    lv_obj_set_style_text_color(b,lv_color_hex(icon_tile?mMenuStyle.Text:mMenuStyle.FocusText),LV_STATE_FOCUSED);
    lv_obj_set_style_bg_color(b,lv_color_hex(KshimDesignPressColor()),LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(b,LV_OPA_COVER,LV_STATE_PRESSED);
    lv_obj_set_style_text_color(b,lv_color_hex(KshimDesignPressText()),LV_STATE_PRESSED);
    lv_obj_set_style_outline_color(b,lv_color_hex(mMenuStyle.FocusBorder),0);
    lv_obj_set_style_outline_width(b,icon_tile?0:KshimDesignStroke(1),LV_STATE_FOCUSED);
    lv_obj_set_style_outline_pad(b,0,0);
    lv_obj_set_style_opa(b,LV_OPA_40,LV_STATE_DISABLED);
    lv_obj_remove_flag(label,LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_style_text_align(label,LV_TEXT_ALIGN_CENTER,0);
    lv_obj_set_style_text_color(label,lv_color_hex(primary?mMenuStyle.BootText:mMenuStyle.Text),0);
    /* Text color inherits the button's active state. */
    lv_obj_remove_local_style_prop(label,LV_STYLE_TEXT_COLOR,0);
    lv_obj_set_align(label,LV_ALIGN_CENTER);
    lv_obj_set_width(label,LV_PCT(100));lv_obj_set_height(label,lv_font_get_line_height(mStatusFont));
    lv_obj_center(label);
    if(terminal){
        if(role==POWER_VIEW_ENTRY)lv_label_set_text(label,command==POWER_RESTART?"[restart]":"[poweroff]");
        if(role==POWER_VIEW_CANCEL)lv_label_set_text(label,width>=(int32_t)lv_font_get_glyph_width(mStatusFont,'W',0)*10?"[ Cancel ]":"Back");
        if(role==POWER_VIEW_CONFIRM)lv_label_set_text(label,width>=(int32_t)lv_font_get_glyph_width(mStatusFont,'W',0)*11?"[ Execute ]":"Run");
        if(role==POWER_VIEW_ROW){
            static const char *cmds[]={"[1] reboot --warm","[2] reboot --hard","[3] reboot --edl"};
            lv_label_set_text(label,cmds[command-POWER_WARM]);
            lv_obj_set_align(label,LV_ALIGN_LEFT_MID);lv_obj_set_x(label,KshimDesignPx(12));
            lv_obj_set_width(label,width-KshimDesignPx(24));
            lv_obj_set_style_text_align(label,LV_TEXT_ALIGN_LEFT,0);
        }
    }else if(role==POWER_VIEW_ENTRY||role==POWER_VIEW_ROW){
        lv_obj_add_event_cb(b,KshimPowerGlyphDraw,LV_EVENT_DRAW_POST,(void *)(uintptr_t)(command|(role<<8)));
        if(role==POWER_VIEW_ENTRY&&(kind==KSHIM_DESIGN_FLUENT2||clover))lv_obj_add_flag(label,LV_OBJ_FLAG_HIDDEN);
        else if(clover){
            lv_obj_set_align(label,LV_ALIGN_BOTTOM_MID);lv_obj_set_y(label,-KshimDesignPx(4));
            lv_obj_set_width(label,width-8);
        }else{
            lv_obj_set_align(label,LV_ALIGN_LEFT_MID);lv_obj_set_x(label,KshimDesignPx(42));
            lv_obj_set_width(label,kshim_design_max(1,width-KshimDesignPx(52)));
            lv_obj_set_style_text_align(label,LV_TEXT_ALIGN_LEFT,0);
        }
    }
}
static void KshimPowerThemeReattach(void)
{
    if(!mPanel)return;
    lv_obj_t *restart=mContext->RestartButton,*off=mContext->PowerOffButton;
    if(!restart||!off)return;
    lv_obj_set_parent(restart,mPanel);lv_obj_set_parent(off,mPanel);
    int32_t p=KshimDesignPx(mMenuDesign.padding),gap=KshimDesignPx(8);
    int32_t width=mDesignGeometry.panel.w;
    int32_t y=mDesignGeometry.boot.y,bh=mDesignGeometry.boot.h,cw=width-2*p;
    unsigned kind=mMenuDesign.kind;
    if(mDesignGeometry.compact){p=4;gap=4;cw=width-2*p;}
    kshim_design_rect_t a,b;
    if(kind==KSHIM_DESIGN_CLOVER){
        kshim_design_rect_t lane=mDesignGeometry.tools;
        int32_t tw=kshim_design_min(KshimDesignPx(48),(lane.w-gap)/2);
        int32_t x=lane.x+(lane.w-2*tw-gap)/2;
        a=(kshim_design_rect_t){x,lane.y,tw,lane.h};b=a;b.x+=tw+gap;
    }else if(kind==KSHIM_DESIGN_FLUENT2&&!mDesignGeometry.compact&&mDesignGeometry.title.h){
        int32_t sz=KshimDesignPx(36);
        a=(kshim_design_rect_t){width-p-2*sz-gap,p,sz,sz};b=a;b.x+=sz+gap;
        lv_obj_set_width(mTitle,kshim_design_max(1,a.x-p-gap));
    }else{
        /* Fit the real panel's action row, never the physical screen edge. */
        int32_t bw=kshim_design_min(mDesignGeometry.boot.w,(cw-2*gap)/3);
        int32_t tools=(cw-bw-2*gap)/2;
        if(kind==KSHIM_DESIGN_TERMINAL){
            int32_t textwidth=(int32_t)lv_font_get_glyph_width(mStatusFont,'W',0)*10+KshimDesignPx(12);
            int32_t tw=kshim_design_min(textwidth,tools);
            a=(kshim_design_rect_t){width-p-2*tw-gap,y,tw,bh};b=a;b.x+=tw+gap;
            lv_obj_set_width(mBootButton,bw);
        }else{
            int32_t tw=kshim_design_min(KshimDesignPx(136),tools);
            a=(kshim_design_rect_t){p,y,tw,bh};b=a;b.x+=tw+gap;
            lv_obj_set_pos(mBootButton,width-p-bw,y);lv_obj_set_width(mBootButton,bw);
        }
    }
    KshimDesignPlace(restart,a);KshimDesignPlace(off,b);
    KshimPowerThemeStyle(restart,POWER_RESTART,POWER_VIEW_ENTRY);
    KshimPowerThemeStyle(off,POWER_OFF,POWER_VIEW_ENTRY);
    /* Short text-only labels remain readable when icons cannot fit. */
    if(mDesignGeometry.compact){
        lv_obj_t *buttons[]={restart,off};
        for(unsigned i=0;i<2;i++){
            lv_obj_remove_event_cb(buttons[i],KshimPowerGlyphDraw);
            lv_obj_t *label=lv_obj_get_child(buttons[i],0);lv_obj_remove_flag(label,LV_OBJ_FLAG_HIDDEN);
            lv_label_set_text(label,i?"Off":"Restart");lv_obj_set_width(label,LV_PCT(100));
            lv_obj_set_style_text_align(label,LV_TEXT_ALIGN_CENTER,0);lv_obj_center(label);
        }
    }
    lv_obj_update_layout(lv_screen_active());
}
static lv_obj_t *KshimPowerText(lv_obj_t *parent,const char *text,const lv_font_t *font,
    uint32_t color,int32_t x,int32_t y,int32_t w,int32_t h,lv_text_align_t align)
{
    lv_obj_t *label=lv_label_create(parent);if(!label)return NULL;
    lv_label_set_text(label,text);lv_label_set_long_mode(label,LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_font(label,font,0);lv_obj_set_style_text_color(label,lv_color_hex(color),0);
    lv_obj_set_style_text_align(label,align,0);lv_obj_set_pos(label,x,y);lv_obj_set_size(label,w,h);
    return label;
}
static void KshimPowerOutside(lv_event_t *e)
{
    if(lv_event_get_target_obj(e)==mPowerOverlay&&lv_event_get_code(e)==LV_EVENT_CLICKED)
        mPowerCommand=POWER_CANCEL;
}
static int KshimPowerThemeDialog(kshim_power_action_t action)
{
    unsigned kind=mMenuDesign.kind;
    int terminal=kind==KSHIM_DESIGN_TERMINAL,material=kind==KSHIM_DESIGN_MATERIAL3;
    int clover=kind==KSHIM_DESIGN_CLOVER,fluent=kind==KSHIM_DESIGN_FLUENT2;
    int32_t w=mFramebuffer->width,h=mFramebuffer->height;
    int small=w<480||h<360;
    int32_t pad=small?10:KshimDesignPx(material||clover?24:20),gap=small?6:KshimDesignPx(12);
    int32_t line=lv_font_get_line_height(mStatusFont),titleline=lv_font_get_line_height(mRowFont);
    int32_t row=kshim_design_max(line+KshimDesignPx(12),small?30:KshimDesignPx(40));
    int32_t bw=kshim_design_min(w-16,KshimDesignPx(terminal?600:clover&&!action?480:action?360:320));
    int32_t inner=bw-2*pad,buttonh=kshim_design_min(row,small?34:KshimDesignPx(40));
    int32_t textheight=action?(small?line*2:line*3):(!fluent?line:0);
    int tiles=clover&&!action&&!small;
    int32_t modeh=tiles?KshimDesignPx(112):3*row+2*(terminal?0:gap/2);
    int32_t emblem=clover&&action&&!small?KshimDesignPx(76):0;
    int32_t bh=emblem+2*pad+titleline+gap+(textheight?textheight+gap:0)+(action?0:modeh+gap)+buttonh;
    bh=kshim_design_min(bh,h-16);
    mPowerGroup=lv_group_create();if(!mPowerGroup)return -1;
    mPowerOverlay=lv_obj_create(lv_layer_top());if(!mPowerOverlay)return -1;
    lv_obj_remove_style_all(mPowerOverlay);lv_obj_set_size(mPowerOverlay,w,h);
    lv_obj_remove_flag(mPowerOverlay,LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(mPowerOverlay,lv_color_black(),0);
    lv_obj_set_style_bg_opa(mPowerOverlay,fluent&&!action?LV_OPA_TRANSP:terminal?90:clover?200:90,0);
    lv_obj_add_event_cb(mPowerOverlay,KshimPowerOutside,LV_EVENT_CLICKED,NULL);
    lv_obj_t *box=lv_obj_create(mPowerOverlay);if(!box)return -1;
    lv_obj_remove_style_all(box);lv_obj_remove_flag(box,LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(box,bw,bh);lv_obj_center(box);
    lv_obj_set_style_bg_color(box,lv_color_hex(mMenuStyle.Panel),0);lv_obj_set_style_bg_opa(box,LV_OPA_COVER,0);
    lv_obj_set_style_radius(box,terminal?0:KshimDesignPx(material?28:clover?12:8),0);
    lv_obj_set_style_border_color(box,lv_color_hex(mMenuStyle.Border),0);
    lv_obj_set_style_border_width(box,terminal||fluent?1:0,0);
    if(fluent&&!KshimDesignReduced()){
        lv_obj_set_style_shadow_width(box,KshimDesignPx(20),0);lv_obj_set_style_shadow_opa(box,40,0);
        lv_obj_set_style_shadow_offset_y(box,KshimDesignPx(8),0);
    }
    if(fluent&&!action&&!small){
        lv_area_t anchor;lv_obj_get_coords(mContext->RestartButton,&anchor);
        int32_t x=kshim_design_max(8,kshim_design_min(w-bw-8,anchor.x2-bw+1));
        int32_t y=anchor.y2+KshimDesignPx(8);
        if(y+bh>h-8)y=kshim_design_max(8,anchor.y1-bh-KshimDesignPx(8));
        lv_obj_set_align(box,LV_ALIGN_TOP_LEFT);lv_obj_set_pos(box,x,y);
    }
    if(material&&!action&&!small){
        lv_area_t anchor;lv_obj_get_coords(mContext->RestartButton,&anchor);
        int32_t x=kshim_design_max(8,kshim_design_min(w-bw-8,anchor.x1));
        int32_t y=kshim_design_max(8,anchor.y1-bh-KshimDesignPx(8));
        lv_obj_set_align(box,LV_ALIGN_TOP_LEFT);lv_obj_set_pos(box,x,y);
    }
    static const char *names[]={"", "Power off", "Warm restart", "Hard restart", "Enter EDL"};
    const char *title=action?names[action]:terminal?"SYSTEM / RESET":clover?"Restart device":fluent?"Restart":"Restart options";
    const char *body=action==KSHIM_POWER_EDL?(small?"EDL: exit may need a PC.":"USB emergency download mode.\nA computer may be needed to exit."):
        action==KSHIM_POWER_OFF?"The device will shut down.":action?"The device will restart. Your selected image will not be launched.":
        terminal?"$ select reset mode":clover?"Choose a reset method":"Choose how to restart your device";
    lv_text_align_t align=clover?LV_TEXT_ALIGN_CENTER:LV_TEXT_ALIGN_LEFT;
    if(emblem){
        lv_obj_t *icon=lv_obj_create(box);if(!icon)return -1;
        lv_obj_remove_style_all(icon);lv_obj_remove_flag(icon,LV_OBJ_FLAG_CLICKABLE|LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_size(icon,emblem,emblem);lv_obj_set_pos(icon,(bw-emblem)/2,pad);
        lv_obj_set_style_text_color(icon,lv_color_hex(mMenuStyle.FocusBorder),0);
        lv_obj_add_event_cb(icon,KshimPowerGlyphDraw,LV_EVENT_DRAW_POST,
            (void *)(uintptr_t)((unsigned)(action+1)|(POWER_VIEW_ROW<<8)));
    }
    lv_obj_t *heading=KshimPowerText(box,title,mRowFont,mMenuStyle.Text,pad,pad+emblem,inner,titleline,align);
    if(!heading)return -1;
    lv_label_set_long_mode(heading,LV_LABEL_LONG_DOT);
    int32_t y=pad+emblem+titleline+gap;
    if(textheight){if(!KshimPowerText(box,body,mStatusFont,mMenuStyle.Muted,pad,y,inner,textheight,align))return -1;y+=textheight+gap;}
    int32_t footer=bh-pad-buttonh;
    if(!action){
        lv_obj_t *choices=lv_obj_create(box);if(!choices)return -1;
        lv_obj_remove_style_all(choices);lv_obj_set_pos(choices,pad,y);
        lv_obj_set_size(choices,inner,kshim_design_max(1,footer-gap-y));
        lv_obj_set_scroll_dir(choices,LV_DIR_VER);
        for(unsigned i=0;i<3;i++){
            lv_obj_t *b=KshimPowerButton(choices,names[i+2],POWER_WARM+i,NULL);if(!b)return -1;
            int32_t tw=tiles?(inner-2*gap)/3:inner;
            lv_obj_set_pos(b,tiles?(int32_t)i*(tw+gap):0,tiles?0:(int32_t)i*(row+(terminal?0:gap/2)));
            lv_obj_set_size(b,tw,tiles?modeh:row);
            KshimPowerThemeStyle(b,POWER_WARM+i,POWER_VIEW_ROW);
            if(tiles)lv_label_set_text(lv_obj_get_child(b,0),i==0?"Warm":i==1?"Hard":"EDL");
            mContext->PowerModeButtons[i]=b;
            if(mContext->PowerCapabilities&KSHIM_POWER_BIT(i+2))lv_group_add_obj(mPowerGroup,b);
            else lv_obj_add_state(b,LV_STATE_DISABLED);
        }
    }
    int32_t target=KshimDesignPx(terminal?160:112);
    if(terminal)target=kshim_design_max(target,(int32_t)lv_font_get_glyph_width(mStatusFont,'W',0)*11+KshimDesignPx(8));
    int32_t buttonw=kshim_design_min((inner-gap)/2,target);
    lv_obj_t *cancel=KshimPowerButton(box,"Cancel",POWER_CANCEL,mPowerGroup);if(!cancel)return -1;
    lv_obj_set_size(cancel,buttonw,buttonh);
    int32_t cancelx=action?bw-pad-2*buttonw-gap:clover?(bw-buttonw)/2:bw-pad-buttonw;
    if(terminal)cancelx=pad;
    lv_obj_set_pos(cancel,cancelx,footer);KshimPowerThemeStyle(cancel,POWER_CANCEL,POWER_VIEW_CANCEL);
    mContext->PowerCancelButton=cancel;
    if(action){
        lv_obj_t *confirm=KshimPowerButton(box,names[action],POWER_CONFIRM,mPowerGroup);if(!confirm)return -1;
        lv_obj_set_size(confirm,buttonw,buttonh);lv_obj_set_pos(confirm,bw-pad-buttonw,footer);
        KshimPowerThemeStyle(confirm,POWER_CONFIRM,POWER_VIEW_CONFIRM);mContext->PowerConfirmButton=confirm;
    }
    mContext->PowerDialog=mPowerOverlay;mPowerChoice=action;
    lv_indev_set_group(mKeyInput,mPowerGroup);lv_group_focus_obj(cancel);
    lv_obj_update_layout(lv_screen_active());return 0;
}
#endif
