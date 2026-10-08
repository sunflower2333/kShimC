#ifndef KSHIM_MENU_STYLE_RENDER_H
#define KSHIM_MENU_STYLE_RENDER_H

/* 绘制两个端点的渐变；相同端点使用纯色，按压态可显式关闭渐变。 */
static void KshimStyleGradient(
    lv_obj_t *Object,
    uint32_t First,
    uint32_t Last,
    lv_style_selector_t State
)
{
  lv_obj_set_style_bg_color(Object, lv_color_hex(First), State);
  lv_obj_set_style_bg_grad_color(Object, lv_color_hex(Last), State);
  lv_obj_set_style_bg_grad_dir(Object,
      First == Last ? LV_GRAD_DIR_NONE : LV_GRAD_DIR_VER, State);
}

/* 在原有面板上应用现代材质，阴影范围限制在预设的小尺寸预算内。 */
static void KshimModernPanel(lv_obj_t *Screen, lv_obj_t *Panel)
{
  if (mMenuStyle.Modern == 0U)
    return;
  KshimStyleGradient(Screen, mMenuStyle.Screen, mMenuStyle.ScreenEnd, 0);
  KshimStyleGradient(Panel, mMenuStyle.Panel, mMenuStyle.PanelEnd, 0);
  lv_obj_set_style_shadow_width(Panel, mMenuStyle.PanelShadow, 0);
  lv_obj_set_style_shadow_color(Panel, lv_color_hex(mMenuStyle.Shadow), 0);
  lv_obj_set_style_shadow_opa(Panel, 32, 0);
  lv_obj_set_style_shadow_offset_y(Panel, mMenuStyle.PanelShadow / 4U, 0);
}

/* 应用行的分隔线、层次、焦点轮廓和独立按压颜色，不改变确认语义。 */
static void KshimModernEntry(lv_obj_t *Button)
{
  if (mMenuStyle.Modern == 0U)
    return;
  if (mMenuStyle.Divider != 0U)
    lv_obj_set_style_border_side(Button, LV_BORDER_SIDE_BOTTOM, 0);
  KshimStyleGradient(Button, mMenuStyle.Focus, mMenuStyle.FocusEnd, LV_STATE_FOCUSED);
  KshimStyleGradient(Button, mMenuStyle.Press, mMenuStyle.Press, LV_STATE_PRESSED);
  lv_obj_set_style_text_color(Button, lv_color_hex(mMenuStyle.PressText), LV_STATE_PRESSED);
  lv_obj_set_style_shadow_width(Button, mMenuStyle.RowShadow, 0);
  lv_obj_set_style_shadow_color(Button, lv_color_hex(mMenuStyle.Shadow), 0);
  lv_obj_set_style_shadow_opa(Button, 42, 0);
  lv_obj_set_style_shadow_offset_y(Button, mMenuStyle.RowShadow / 3U, 0);
  if (mMenuStyle.RowShadow != 0U) {
    lv_obj_set_style_shadow_width(Button, 2, LV_STATE_PRESSED);
    lv_obj_set_style_shadow_offset_y(Button, 1, LV_STATE_PRESSED);
  }
  if (mMenuStyle.FocusGlow != 0U) {
    lv_obj_set_style_shadow_width(Button, mMenuStyle.FocusGlow, LV_STATE_FOCUSED);
    lv_obj_set_style_shadow_color(Button, lv_color_hex(mMenuStyle.FocusBorder), LV_STATE_FOCUSED);
    lv_obj_set_style_shadow_opa(Button, 64, LV_STATE_FOCUSED);
    lv_obj_set_style_shadow_offset_y(Button, 0, LV_STATE_FOCUSED);
  }
  lv_obj_set_style_outline_color(Button, lv_color_hex(mMenuStyle.FocusBorder), 0);
  lv_obj_set_style_outline_opa(Button, LV_OPA_COVER, 0);
  lv_obj_set_style_outline_pad(Button, 0, 0);
  lv_obj_set_style_outline_width(Button, mMenuStyle.FocusOutline, LV_STATE_FOCUSED);
  lv_obj_set_style_outline_width(Button, mMenuStyle.FocusOutline,
      LV_STATE_FOCUSED | LV_STATE_FOCUS_KEY);
}

/* 计算主按钮圆角，Material 3 / Aurora 使用半高胶囊而不复用行圆角。 */
static uint32_t KshimBootRadius(uint32_t Height)
{
  if (mMenuStyle.Modern == 0U)
    return kshim_menu_style_radius(Height);
  uint32_t Radius = mMenuStyle.BootRadius == 255U ? Height / 2U : mMenuStyle.BootRadius;
  return Radius < Height / 2U ? Radius : Height / 2U;
}

#endif
