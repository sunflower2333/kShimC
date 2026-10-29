#ifndef KSHIM_MENU_POWER_EXTENDED_H
#define KSHIM_MENU_POWER_EXTENDED_H
/* Remaining presentations. Entry placement is relative to the boot chrome;
 * modal layout follows the selected family. No reset calls live in this file. */
static int32_t KshimPowerPx(uint32_t n)
{
    if(mMenuDesign.enabled)return KshimDesignPx(n);
    if(KSHIM_MENU_IS_NATIVE)return lv_dpx(n);
    return kshim_design_px(n,kshim_design_scale(mFramebuffer->width,mFramebuffer->height,0));
}
static void KshimPowerEdgeDraw(lv_event_t *e)
{
    lv_obj_t *o=lv_event_get_target_obj(e);lv_area_t a;lv_obj_get_coords(o,&a);
    unsigned look=KshimPowerProfile().look;
    lv_opa_t opacity=lv_obj_get_style_opa_recursive(o,LV_PART_MAIN);
    int32_t stroke=kshim_design_max(1,KshimPowerPx(1)),inset=KshimPowerPx(7);
    if(look==POWER_LOOK_SOFT){
        lv_color_t color=lv_obj_get_style_bg_color(o,LV_PART_MAIN);
        uint32_t light=lv_color_to_u32(lv_color_mix(lv_color_white(),color,170))&0xffffffU;
        uint32_t dark=lv_color_to_u32(lv_color_mix(lv_color_black(),color,45))&0xffffffU;
        if(lv_obj_has_state(o,LV_STATE_PRESSED)){uint32_t swap=light;light=dark;dark=swap;}
        KshimSymbolLine(e,light,opacity,a.x1+inset,a.y1+1,a.x2-inset,a.y1+1,stroke,true);
        KshimSymbolLine(e,light,opacity,a.x1+1,a.y1+inset,a.x1+1,a.y2-inset,stroke,true);
        KshimSymbolLine(e,dark,opacity,a.x1+inset,a.y2-1,a.x2-inset,a.y2-1,stroke,true);
        KshimSymbolLine(e,dark,opacity,a.x2-1,a.y1+inset,a.x2-1,a.y2-inset,stroke,true);
    }else if(look==POWER_LOOK_GLASS){
        KshimSymbolLine(e,mMenuStyle.Text,opacity/3,a.x1+inset,a.y1+1,a.x2-inset,a.y1+1,stroke,true);
    }else if(look==POWER_LOOK_NEON){
        KshimSymbolCorners(e,mMenuStyle.FocusBorder,opacity,a.x1+1,a.y1+1,
            lv_area_get_width(&a)-2,lv_area_get_height(&a)-2,KshimPowerPx(8),stroke);
    }
}
static void KshimPowerExtendedStyle(lv_obj_t *b,unsigned command,unsigned role,int tile)
{
    lv_obj_t *label=lv_obj_get_child(b,0);if(!label)return;
    kshim_power_profile_t p=KshimPowerProfile();
    int entry=role==POWER_VIEW_ENTRY,primary=role==POWER_VIEW_CONFIRM;
    int32_t width=lv_obj_get_style_width(b,0),height=lv_obj_get_style_height(b,0);
    if(KSHIM_MENU_IS_NATIVE){
        /* Native theme owns all state colors, fonts, radius, borders and shadows. */
        if(entry)lv_label_set_text(label,command==POWER_OFF?LV_SYMBOL_POWER:LV_SYMBOL_REFRESH);
        lv_obj_set_width(label,LV_PCT(100));lv_obj_center(label);return;
    }
    int32_t x=lv_obj_get_style_x(b,0),y=lv_obj_get_style_y(b,0);
    lv_obj_remove_style_all(b);lv_obj_set_pos(b,x,y);lv_obj_set_size(b,width,height);
    lv_obj_remove_event_cb(b,KshimPowerGlyphDraw);lv_obj_remove_event_cb(b,KshimPowerEdgeDraw);
    lv_obj_set_style_pad_all(b,0,0);lv_obj_set_style_text_font(b,mStatusFont,0);
    uint32_t ink=primary?mMenuStyle.BootText:mMenuStyle.Text,fill=primary?mMenuStyle.Boot:mMenuStyle.Item;
    int filled=primary||tile||(entry&&p.icon_entry==2)||p.look==POWER_LOOK_SOFT||p.look==POWER_LOOK_CLASSIC;
    int32_t radius=KshimPowerPx(p.radius);
    if(p.look==POWER_LOOK_AURORA||p.look==POWER_LOOK_HARMONY)radius=LV_RADIUS_CIRCLE;
    if(p.look==POWER_LOOK_MATERIAL2)radius=primary?KshimPowerPx(4):0;
    if(p.look==POWER_LOOK_IOS){
        ink=primary?0xc62828U:mMenuStyle.Boot;fill=mMenuStyle.Item;filled=!entry;radius=entry?KshimPowerPx(8):0;
    }
    if(entry&&p.look!=POWER_LOOK_IOS&&(p.location==POWER_AT_HEADER||p.location==POWER_AT_DETAIL)) {
        /* Action bar colors must match its foreground, including dark bars in
         * light Material2, Classic and Holo palettes. */
        ink=lv_color_to_u32(lv_obj_get_style_text_color(mTitle,LV_PART_MAIN))&0xffffffU;
    }
    if(p.look==POWER_LOOK_MATERIAL2||p.menu==POWER_MENU_TYPE) {
        filled=0;ink=p.menu==POWER_MENU_TYPE?mMenuStyle.Text:primary?mMenuStyle.Boot:mMenuStyle.Text;
        if(p.look==POWER_LOOK_MATERIAL2&&entry)ink=lv_color_to_u32(lv_obj_get_style_text_color(mTitle,LV_PART_MAIN))&0xffffffU;
    }
    lv_obj_set_style_radius(b,radius,0);
    lv_obj_set_style_bg_color(b,lv_color_hex(fill),0);
    lv_obj_set_style_bg_opa(b,filled?LV_OPA_COVER:LV_OPA_TRANSP,0);
    lv_obj_set_style_text_color(b,lv_color_hex(ink),0);
    int32_t border=p.look==POWER_LOOK_CONTRAST?KshimPowerPx(2):
        p.look==POWER_LOOK_CLASSIC||p.look==POWER_LOOK_NORD||p.look==POWER_LOOK_NEON?1:0;
    if(entry&&p.location==POWER_AT_HEADER&&p.look!=POWER_LOOK_NEON)border=0;
    lv_obj_set_style_border_width(b,border,0);lv_obj_set_style_border_color(b,lv_color_hex(mMenuStyle.Border),0);
    if(p.look==POWER_LOOK_ADWAITA&&role==POWER_VIEW_ROW){
        lv_obj_set_style_bg_opa(b,LV_OPA_COVER,0);lv_obj_set_style_border_width(b,1,0);
        lv_obj_set_style_border_side(b,LV_BORDER_SIDE_BOTTOM,0);lv_obj_set_style_radius(b,0,0);
    }
    if((p.look==POWER_LOOK_HOLO||p.look==POWER_LOOK_IOS)&&role==POWER_VIEW_ROW){
        lv_obj_set_style_border_width(b,1,0);lv_obj_set_style_border_side(b,LV_BORDER_SIDE_BOTTOM,0);
    }
    const lv_style_selector_t states[]={LV_STATE_FOCUSED,LV_STATE_PRESSED};
    for(unsigned i=0;i<2;i++){
        lv_obj_set_style_bg_color(b,lv_color_hex(i?KshimDesignPressColor():mMenuStyle.Focus),states[i]);
        lv_obj_set_style_bg_opa(b,LV_OPA_COVER,states[i]);
        lv_obj_set_style_text_color(b,lv_color_hex(i?KshimDesignPressText():mMenuStyle.FocusText),states[i]);
    }
    lv_obj_set_style_outline_width(b,kshim_design_max(1,KshimPowerPx(p.look==POWER_LOOK_CONTRAST?3:1)),LV_STATE_FOCUSED);
    lv_obj_set_style_outline_color(b,lv_color_hex(mMenuStyle.FocusBorder),0);lv_obj_set_style_outline_pad(b,0,0);
    lv_obj_set_style_opa(b,LV_OPA_40,LV_STATE_DISABLED);
    if(p.look==POWER_LOOK_NEON&&!KshimDesignReduced()){
        lv_obj_set_style_shadow_color(b,lv_color_hex(mMenuStyle.FocusBorder),LV_STATE_FOCUSED);
        lv_obj_set_style_shadow_width(b,KshimPowerPx(8),LV_STATE_FOCUSED);lv_obj_set_style_shadow_opa(b,60,LV_STATE_FOCUSED);
    }
    if(p.look==POWER_LOOK_SOFT||p.look==POWER_LOOK_GLASS||p.look==POWER_LOOK_NEON)
        lv_obj_add_event_cb(b,KshimPowerEdgeDraw,LV_EVENT_DRAW_POST,NULL);
    if(p.look==POWER_LOOK_SOFT&&!KshimDesignReduced()){
        lv_obj_set_style_shadow_width(b,KshimPowerPx(8),0);lv_obj_set_style_shadow_opa(b,30,0);
        lv_obj_set_style_shadow_offset_y(b,KshimPowerPx(3),0);
    }
    lv_obj_remove_flag(label,LV_OBJ_FLAG_HIDDEN);lv_obj_remove_local_style_prop(label,LV_STYLE_TEXT_COLOR,0);
    lv_obj_set_style_text_align(label,LV_TEXT_ALIGN_CENTER,0);
    lv_obj_set_size(label,kshim_design_max(1,width-12),lv_font_get_line_height(mStatusFont));lv_obj_center(label);
    int icon=(entry&&p.icon_entry)||(role==POWER_VIEW_ROW&&p.menu!=POWER_MENU_TYPE&&p.look!=POWER_LOOK_IOS);
    if(p.look==POWER_LOOK_METRO){
        if(role==POWER_VIEW_ROW&&mFramebuffer->width>=480&&mFramebuffer->height>=360){lv_obj_set_style_text_font(b,mRowFont,0);lv_obj_set_height(label,lv_font_get_line_height(mRowFont));}
        if(entry)lv_label_set_text(label,command==POWER_OFF?"power off":"restart");
        if(role==POWER_VIEW_ROW)lv_label_set_text(label,command==POWER_WARM?"warm restart":command==POWER_HARD?"hard restart":"emergency download");
    }
    if(icon){
        lv_obj_add_event_cb(b,KshimPowerGlyphDraw,LV_EVENT_DRAW_POST,(void *)(uintptr_t)(command|(role<<8)));
        if(entry&&p.icon_entry==1)lv_obj_add_flag(label,LV_OBJ_FLAG_HIDDEN);
        else if(tile){
            /* Reuse glyph primitives as an upper emblem, leaving a separate
             * baseline for text. The layout flag is carried in role bit 8. */
            lv_obj_remove_event_cb(b,KshimPowerGlyphDraw);
            lv_obj_add_event_cb(b,KshimPowerGlyphDraw,LV_EVENT_DRAW_POST,(void *)(uintptr_t)(command|((role|16)<<8)));
            lv_label_set_text(label,command==POWER_WARM?"Warm":command==POWER_HARD?"Hard":"EDL");
            lv_obj_align(label,LV_ALIGN_BOTTOM_MID,0,-KshimPowerPx(10));
        }else{
            lv_obj_set_style_text_align(label,LV_TEXT_ALIGN_LEFT,0);lv_obj_set_width(label,kshim_design_max(1,width-KshimPowerPx(54)));
            lv_obj_align(label,LV_ALIGN_LEFT_MID,KshimPowerPx(42),0);
        }
    }else if(role==POWER_VIEW_ROW&&p.look!=POWER_LOOK_IOS){
        lv_obj_set_style_text_align(label,LV_TEXT_ALIGN_LEFT,0);lv_obj_align(label,LV_ALIGN_LEFT_MID,KshimPowerPx(12),0);
        lv_obj_set_width(label,kshim_design_max(1,width-KshimPowerPx(24)));
    }
}
static void KshimPowerExtendedReattach(void)
{
    if(!mPanel||!mContext->RestartButton||!mContext->PowerOffButton)return;
    kshim_power_profile_t p=KshimPowerProfile();
    lv_obj_t *a=mContext->RestartButton,*b=mContext->PowerOffButton;
    lv_obj_set_parent(a,mPanel);lv_obj_set_parent(b,mPanel);
    lv_obj_add_flag(a,LV_OBJ_FLAG_IGNORE_LAYOUT);lv_obj_add_flag(b,LV_OBJ_FLAG_IGNORE_LAYOUT);
    /* Tiny native panels need a geometry-only inset reduction to fit three
     * default-padded controls. The actual button theme remains untouched. */
    if(KSHIM_MENU_IS_NATIVE&&mFramebuffer->width<320)lv_obj_set_style_pad_hor(mPanel,2,0);
    lv_obj_update_layout(lv_screen_active());
    int32_t width=lv_obj_get_content_width(mPanel),gap=KshimPowerPx(8);
    int32_t pad=mMenuDesign.enabled?KshimDesignPx(mMenuDesign.padding):KSHIM_MENU_IS_NATIVE?0:(int32_t)KshimPadding();
    int small=mFramebuffer->width<480||mFramebuffer->height<360||mDesignGeometry.compact;
    if(mMenuDesign.enabled&&mDesignGeometry.compact)pad=4;
    int32_t cw=width-2*pad,sz=kshim_design_min(KshimPowerPx(36),kshim_design_max(16,(cw-3*gap)/5));
    int32_t y=lv_obj_get_y(mBootButton),bh=lv_obj_get_height(mBootButton);
    kshim_design_rect_t ra,rb;
    int header=p.location==POWER_AT_HEADER||(p.location==POWER_AT_DETAIL&&!mDesignGeometry.split);
    if(header&&!lv_obj_has_flag(mTitle,LV_OBJ_FLAG_HIDDEN)&&!small){
        int32_t ty=lv_obj_get_y(mTitle),th=lv_obj_get_height(mTitle);
        sz=kshim_design_min(sz,kshim_design_max(24,th));
        ra=(kshim_design_rect_t){width-pad-2*sz-gap,ty+(th-sz)/2,sz,sz};rb=ra;rb.x+=sz+gap;
        lv_label_set_long_mode(mTitle,LV_LABEL_LONG_DOT);
        lv_obj_set_width(mTitle,kshim_design_max(1,ra.x-lv_obj_get_x(mTitle)-gap));
    }else if(p.location==POWER_AT_DETAIL&&mDesignGeometry.split){
        int32_t start=mDesignGeometry.status.x,available=width-pad-start;
        sz=kshim_design_min(sz,(available-2*gap)/4);
        ra=(kshim_design_rect_t){width-pad-2*sz-gap,y,sz,bh};rb=ra;rb.x+=sz+gap;
        lv_obj_set_width(mBootButton,kshim_design_min(lv_obj_get_width(mBootButton),ra.x-start-gap));
    }else if(p.location==POWER_AT_DISKS||p.location==POWER_AT_NATIVE){
        int32_t tw=KSHIM_MENU_IS_NATIVE?sz:kshim_design_min(KshimPowerPx(108),(cw-2*gap)/3);
        if(KSHIM_MENU_IS_NATIVE){
            const lv_font_t *font=lv_obj_get_style_text_font(a,LV_PART_MAIN);
            int32_t glyph=lv_text_get_width(LV_SYMBOL_REFRESH,(uint32_t)strlen(LV_SYMBOL_REFRESH),font,0);
            tw=kshim_design_max(tw,glyph+lv_obj_get_style_pad_left(a,0)+lv_obj_get_style_pad_right(a,0)+2*lv_obj_get_style_border_width(a,0));
        }
        ra=(kshim_design_rect_t){pad,y,tw,bh};rb=ra;rb.x=width-pad-tw;
        if(!KSHIM_MENU_IS_NATIVE){
            int32_t bw=kshim_design_min(lv_obj_get_width(mBootButton),cw-2*tw-2*gap);
            lv_obj_set_width(mBootButton,bw);lv_obj_set_x(mBootButton,(width-bw)/2);
        }
    }else{
        int32_t tw=p.icon_entry==1?sz:kshim_design_min(KshimPowerPx(p.look==POWER_LOOK_CONTRAST?132:p.icon_entry==2?140:104),(cw-2*gap)/3);
        if(small&&p.icon_entry==1)tw=kshim_design_min(tw,bh);
        ra=(kshim_design_rect_t){pad,y,tw,bh};rb=ra;rb.x+=tw+gap;
        int32_t bw=kshim_design_min(lv_obj_get_width(mBootButton),cw-2*tw-2*gap);
        if(p.look==POWER_LOOK_METRO||mMenuDesign.kind==KSHIM_DESIGN_MINIMAL){
            ra.x=width-pad-2*tw-gap;rb.x=ra.x+tw+gap;lv_obj_set_x(mBootButton,pad);
        }else lv_obj_set_x(mBootButton,width-pad-bw);
        lv_obj_set_width(mBootButton,kshim_design_max(1,bw));
    }
    KshimDesignPlace(a,ra);KshimDesignPlace(b,rb);
    KshimPowerExtendedStyle(a,POWER_RESTART,POWER_VIEW_ENTRY,0);
    KshimPowerExtendedStyle(b,POWER_OFF,POWER_VIEW_ENTRY,0);
    /* On compact text controls, short labels still express the actual actions. */
    if(small&&p.icon_entry!=1){
        lv_obj_t *buttons[]={a,b};
        for(unsigned i=0;i<2;i++){
            lv_obj_remove_event_cb(buttons[i],KshimPowerGlyphDraw);
            lv_obj_t *label=lv_obj_get_child(buttons[i],0);
            lv_label_set_text(label,i?"Off":"Reset");lv_obj_set_width(label,LV_PCT(100));
            lv_obj_set_style_text_align(label,LV_TEXT_ALIGN_CENTER,0);lv_obj_center(label);
        }
    }
    lv_obj_update_layout(lv_screen_active());
}
static int KshimPowerExtendedDialog(kshim_power_action_t action)
{
    kshim_power_profile_t p=KshimPowerProfile();
    int32_t w=mFramebuffer->width,h=mFramebuffer->height;
    int small=w<480||h<360,native=KSHIM_MENU_IS_NATIVE,ios=p.menu==POWER_MENU_SHEET;
    int tile=!action&&!small&&(p.menu==POWER_MENU_TILES||p.menu==POWER_MENU_BENTO);
    int32_t pad=small?10:KshimPowerPx(20),gap=small?6:KshimPowerPx(12);
    const lv_font_t *headingfont=p.look==POWER_LOOK_METRO&&!small?mTitleFont:mRowFont;
    int32_t line=lv_font_get_line_height(mStatusFont),headline=lv_font_get_line_height(headingfont);
    int32_t bw=kshim_design_min(w-16,KshimPowerPx(action?360:p.width));
    int32_t row=kshim_design_max(line+KshimPowerPx(16),small?28:KshimPowerPx(44));
    if(ios)row=kshim_design_max(row,KshimPowerPx(52));
    if(p.look==POWER_LOOK_METRO&&!small)row=kshim_design_max(row,lv_font_get_line_height(mRowFont)+KshimPowerPx(16));
    int32_t inner=bw-2*pad,buttonh=kshim_design_max(line+8,small?28:KshimPowerPx(40));
    int32_t description=action?(small?line*2:line*3):p.menu==POWER_MENU_TYPE?line:0;
    int bento=tile&&p.menu==POWER_MENU_BENTO;
    int32_t modes=tile?(bento?2*KshimPowerPx(84)+gap:KshimPowerPx(116)):3*row;
    if(p.look==POWER_LOOK_SOFT)modes+=2*gap;
    int32_t bh=kshim_design_min(h-16,2*pad+headline+gap+(description?description+gap:0)+(!action?modes+gap:0)+buttonh);
    if(h<220&&!action){description=0;headline=0;pad=8;gap=4;bh=h-16;}
    mPowerGroup=lv_group_create();if(!mPowerGroup)return -1;
    mPowerOverlay=lv_obj_create(lv_layer_top());if(!mPowerOverlay)return -1;
    lv_obj_remove_style_all(mPowerOverlay);lv_obj_set_size(mPowerOverlay,w,h);
    lv_obj_remove_flag(mPowerOverlay,LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(mPowerOverlay,lv_color_black(),0);
    lv_obj_set_style_bg_opa(mPowerOverlay,p.look==POWER_LOOK_METRO?LV_OPA_COVER:p.menu==POWER_MENU_FLYOUT&&!action?0:ios?90:100,0);
    lv_obj_add_event_cb(mPowerOverlay,KshimPowerOutside,LV_EVENT_CLICKED,NULL);
    lv_obj_t *box=lv_obj_create(mPowerOverlay);if(!box)return -1;
    if(!native)lv_obj_remove_style_all(box);
    /* Geometry only for the native container. */
    lv_obj_set_style_pad_all(box,0,0);lv_obj_remove_flag(box,LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(box,bw,bh);lv_obj_center(box);
    if(!native){
        lv_obj_set_style_bg_color(box,lv_color_hex(ios?mMenuStyle.Item:mMenuStyle.Panel),0);
        lv_obj_set_style_bg_opa(box,p.look==POWER_LOOK_GLASS&&!KshimDesignReduced()?235:LV_OPA_COVER,0);
        lv_obj_set_style_radius(box,KshimPowerPx(p.radius),0);
        lv_obj_set_style_border_width(box,p.look==POWER_LOOK_CONTRAST?KshimPowerPx(3):
            p.look==POWER_LOOK_CLASSIC||p.look==POWER_LOOK_GLASS||p.look==POWER_LOOK_NEON||p.look==POWER_LOOK_NORD?1:0,0);
        lv_obj_set_style_border_color(box,lv_color_hex(mMenuStyle.Border),0);
        if(p.look==POWER_LOOK_GLASS||p.look==POWER_LOOK_SOFT||p.look==POWER_LOOK_NEON)
            lv_obj_add_event_cb(box,KshimPowerEdgeDraw,LV_EVENT_DRAW_POST,NULL);
        if((p.look==POWER_LOOK_MATERIAL2||p.look==POWER_LOOK_ADWAITA||p.menu==POWER_MENU_FLYOUT)&&!KshimDesignReduced()){
            lv_obj_set_style_shadow_width(box,KshimPowerPx(16),0);lv_obj_set_style_shadow_offset_y(box,KshimPowerPx(5),0);
            lv_obj_set_style_shadow_opa(box,36,0);
        }
    }
    if(ios&&!action){
        /* The action group and Cancel are separate sheets, not one giant
         * rounded dialog with another full-width button bolted underneath. */
        lv_obj_set_style_bg_opa(box,LV_OPA_TRANSP,0);
        lv_obj_t *group=lv_obj_create(box);if(!group)return -1;
        lv_obj_remove_style_all(group);lv_obj_remove_flag(group,LV_OBJ_FLAG_CLICKABLE|LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_pos(group,0,0);lv_obj_set_size(group,bw,bh-pad-buttonh-gap);
        lv_obj_set_style_bg_color(group,lv_color_hex(mMenuStyle.Item),0);lv_obj_set_style_bg_opa(group,LV_OPA_COVER,0);
        lv_obj_set_style_radius(group,KshimPowerPx(14),0);
    }
    lv_area_t anchor;lv_obj_get_coords(mContext->RestartButton,&anchor);
    if(p.menu==POWER_MENU_FLYOUT&&!action&&!small){
        int32_t x=kshim_design_max(8,kshim_design_min(w-bw-8,anchor.x2-bw+1));
        int32_t y=anchor.y2+gap;if(y+bh>h-8)y=kshim_design_max(8,anchor.y1-bh-gap);
        lv_obj_set_align(box,LV_ALIGN_TOP_LEFT);lv_obj_set_pos(box,x,y);
    }else if(ios&&!action){lv_obj_align(box,LV_ALIGN_BOTTOM_MID,0,-kshim_design_max(8,KshimPowerPx(20)));}
    else if(p.look==POWER_LOOK_METRO&&!small){
        lv_area_t panel;lv_obj_get_coords(mPanel,&panel);
        lv_obj_set_align(box,LV_ALIGN_TOP_LEFT);
        lv_obj_set_pos(box,kshim_design_max(8,kshim_design_min(w-bw-8,panel.x1)),kshim_design_max(8,kshim_design_min(h-bh-8,panel.y1)));
    }
    static const char *names[]={"", "Power off", "Warm restart", "Hard restart", "Enter EDL"};
    const char *title=action?names[action]:p.look==POWER_LOOK_METRO?"restart":p.look==POWER_LOOK_CLASSIC?"System power":
        p.look==POWER_LOOK_NEON?"POWER / RESET":p.look==POWER_LOOK_ADWAITA?"Restart Device":"Restart options";
    const char *body=action==KSHIM_POWER_EDL?(small?"EDL: exit may need a PC.":"USB emergency download mode.\nA computer may be needed to exit."):
        action==KSHIM_POWER_OFF?"Shut down this device?":action?"Restart without launching the selected image?":"Choose a reset method";
    uint32_t ink=native?lv_color_to_u32(lv_obj_get_style_text_color(box,LV_PART_MAIN))&0xffffffU:mMenuStyle.Text;
    uint32_t muted=native?ink:mMenuStyle.Muted;
    lv_text_align_t align=ios||mMenuDesign.kind==KSHIM_DESIGN_CUPERTINO||p.look==POWER_LOOK_HARMONY?LV_TEXT_ALIGN_CENTER:LV_TEXT_ALIGN_LEFT;
    int32_t y=pad;
    if(headline){
        lv_obj_t *heading=KshimPowerText(box,title,headingfont,ink,pad,y,inner,headline,align);if(!heading)return -1;
        lv_label_set_long_mode(heading,LV_LABEL_LONG_DOT);
        if(p.look==POWER_LOOK_HOLO||p.look==POWER_LOOK_CLASSIC||p.look==POWER_LOOK_NEON){
            lv_obj_set_style_border_side(heading,LV_BORDER_SIDE_BOTTOM,0);lv_obj_set_style_border_width(heading,kshim_design_max(1,KshimPowerPx(2)),0);
            lv_obj_set_style_border_color(heading,lv_color_hex(mMenuStyle.FocusBorder),0);
        }
        y+=headline+gap;
    }
    if(description){if(!KshimPowerText(box,body,mStatusFont,muted,pad,y,inner,description,align))return -1;y+=description+gap;}
    int32_t footer=bh-pad-buttonh;
    if(!action){
        lv_obj_t *choices=native?lv_list_create(box):lv_obj_create(box);if(!choices)return -1;
        if(!native)lv_obj_remove_style_all(choices);
        lv_obj_set_style_pad_all(choices,0,0);lv_obj_set_style_border_width(choices,0,0);
        lv_obj_set_layout(choices,0);lv_obj_set_pos(choices,pad,y);lv_obj_set_size(choices,inner,kshim_design_max(1,footer-gap-y));
        lv_obj_set_scroll_dir(choices,LV_DIR_VER);
        for(unsigned i=0;i<3;i++){
            lv_obj_t *b=KshimPowerButton(choices,names[i+2],POWER_WARM+i,NULL);if(!b)return -1;
            int32_t tw=tile?(inner-(bento?gap:2*gap))/(bento?2:3):inner;
            kshim_design_rect_t r={0,(int32_t)i*(row+(p.look==POWER_LOOK_SOFT?gap:0)),tw,row};
            if(tile){r=(kshim_design_rect_t){(int32_t)i*(tw+gap),0,tw,modes};
                if(bento)r=(kshim_design_rect_t){i?((int32_t)i-1)*(tw+gap):0,i?KshimPowerPx(84)+gap:0,i?tw:inner,KshimPowerPx(84)};}
            KshimDesignPlace(b,r);KshimPowerExtendedStyle(b,POWER_WARM+i,POWER_VIEW_ROW,tile);
            mContext->PowerModeButtons[i]=b;
            if(mContext->PowerCapabilities&KSHIM_POWER_BIT(i+2))lv_group_add_obj(mPowerGroup,b);else lv_obj_add_state(b,LV_STATE_DISABLED);
        }
    }
    int32_t buttonw=kshim_design_min((inner-gap)/2,KshimPowerPx(native?164:124));
    lv_obj_t *cancel=KshimPowerButton(box,"Cancel",POWER_CANCEL,mPowerGroup);if(!cancel)return -1;
    if(ios&&!action)buttonw=inner;
    KshimDesignPlace(cancel,(kshim_design_rect_t){ios&&!action?pad:action?bw-pad-2*buttonw-gap:bw-pad-buttonw,footer,buttonw,buttonh});
    KshimPowerExtendedStyle(cancel,POWER_CANCEL,POWER_VIEW_CANCEL,0);
    if(ios&&!action){lv_obj_set_x(cancel,0);lv_obj_set_width(cancel,bw);lv_obj_set_style_radius(cancel,KshimPowerPx(14),0);}
    mContext->PowerCancelButton=cancel;
    if(action){
        const char *shortnames[]={"","Off","Warm","Hard","EDL"};
        lv_obj_t *confirm=KshimPowerButton(box,small?shortnames[action]:names[action],POWER_CONFIRM,mPowerGroup);if(!confirm)return -1;
        KshimDesignPlace(confirm,(kshim_design_rect_t){bw-pad-buttonw,footer,buttonw,buttonh});
        KshimPowerExtendedStyle(confirm,POWER_CONFIRM,POWER_VIEW_CONFIRM,0);mContext->PowerConfirmButton=confirm;
    }
    mContext->PowerDialog=mPowerOverlay;mPowerChoice=action;
    lv_indev_set_group(mKeyInput,mPowerGroup);lv_group_focus_obj(cancel);
    lv_obj_update_layout(lv_screen_active());return 0;
}
#endif
