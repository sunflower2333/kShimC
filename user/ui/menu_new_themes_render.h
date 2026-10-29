#ifndef KSHIM_MENU_NEW_THEMES_RENDER_H
#define KSHIM_MENU_NEW_THEMES_RENDER_H
/* Presentation-only hooks. No input, selection ownership or fabricated actions. */
static inline void KshimNewThemePanelDraw(lv_event_t *event) {
    if(mMenuDesign.kind!=KSHIM_DESIGN_ADWAITA && mMenuDesign.kind!=KSHIM_DESIGN_HOLO) return;
    if(mDesignGeometry.compact || !mDesignGeometry.title.h) return;
    lv_obj_t *panel=lv_event_get_target_obj(event);
    lv_area_t a; lv_obj_get_coords(panel,&a);
    lv_opa_t opacity=lv_obj_get_style_opa_recursive(panel,LV_PART_MAIN);
    int32_t bottom=mDesignGeometry.title.y+mDesignGeometry.title.h+KshimDesignPx(3);
    KshimSymbolRect(event,mMenuStyle.Navigation,opacity,a.x1,a.y1,lv_obj_get_width(panel),bottom,0,0);
    uint32_t rule=mMenuDesign.kind==KSHIM_DESIGN_HOLO?mMenuStyle.FocusBorder:mMenuStyle.Border;
    KshimSymbolLine(event,rule,opacity,a.x1,a.y1+bottom,a.x2,a.y1+bottom,
        KshimDesignStroke(mMenuDesign.kind==KSHIM_DESIGN_HOLO?2:1),false);
}
static inline void KshimNewThemeRowDraw(lv_event_t *event,size_t index) {
    unsigned kind=mMenuDesign.kind;
    if(kind!=KSHIM_DESIGN_ADWAITA && kind!=KSHIM_DESIGN_HOLO) return;
    if(mDesignGeometry.compact || index+1U>=mDesignGeometry.count) return;
    lv_obj_t *row=lv_event_get_target_obj(event);
    lv_area_t a; lv_obj_get_coords(row,&a);
    int32_t inset=kind==KSHIM_DESIGN_HOLO?0:KshimDesignPx(12);
    lv_opa_t opacity=lv_obj_get_style_opa_recursive(row,LV_PART_MAIN);
    KshimSymbolLine(event,mMenuStyle.Border,opacity,a.x1+inset,a.y2,a.x2-inset,a.y2,KshimDesignStroke(1),false);
}
static inline void KshimNewThemeRelayout(void) {
    unsigned kind=mMenuDesign.kind;
    if(kind!=KSHIM_DESIGN_METRO && kind!=KSHIM_DESIGN_ADWAITA && kind!=KSHIM_DESIGN_HOLO) return;
    bool compact=mDesignGeometry.compact!=0;
    bool centered=kind==KSHIM_DESIGN_ADWAITA;
    lv_obj_set_style_text_align(mTitle,centered?LV_TEXT_ALIGN_CENTER:LV_TEXT_ALIGN_LEFT,0);
    lv_obj_set_style_text_align(mStatus,LV_TEXT_ALIGN_LEFT,0);
    if(kind==KSHIM_DESIGN_ADWAITA && !compact) {
        KshimDesignSolid(mList,mMenuStyle.Item,LV_OPA_COVER,0);
        lv_obj_set_style_radius(mList,KshimDesignPx(12),0);
        lv_obj_set_style_clip_corner(mList,true,0);
    }
    if(kind==KSHIM_DESIGN_ADWAITA) {
        lv_obj_set_style_outline_color(mList,lv_color_hex(mMenuStyle.Border),0);
        lv_obj_set_style_outline_opa(mList,LV_OPA_COVER,0);
        lv_obj_set_style_outline_width(mList,compact?0:KshimDesignStroke(1),0);
    }
    if(kind==KSHIM_DESIGN_METRO || kind==KSHIM_DESIGN_HOLO) {
        lv_obj_set_style_border_color(mBootButton,lv_color_hex(kind==KSHIM_DESIGN_METRO?mMenuStyle.Text:mMenuStyle.Border),0);
        lv_obj_set_style_border_opa(mBootButton,LV_OPA_COVER,0);
        lv_obj_set_style_border_width(mBootButton,compact?0:KshimDesignStroke(kind==KSHIM_DESIGN_METRO?2:1),0);
    }
}
#endif
