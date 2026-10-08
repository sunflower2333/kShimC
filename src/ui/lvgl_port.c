#include <lvgl_port.h>

#include <limits.h>
#include <stddef.h>
#include <stdio.h>

#include <backdrop.h>
#include <lvgl.h>
#include <ui_font.h>

#ifndef CONFIG_KSHIM_MENU_ENTRY1
#define CONFIG_KSHIM_MENU_ENTRY1 "Boot"
#endif
#ifndef CONFIG_KSHIM_MENU_ENTRY2
#define CONFIG_KSHIM_MENU_ENTRY2 "Recovery"
#endif
#ifndef CONFIG_KSHIM_LVGL_BUFFER_BYTES
#define CONFIG_KSHIM_LVGL_BUFFER_BYTES (16U * 1024U)
#endif
#ifndef CONFIG_KSHIM_BACKDROP_TEXTURE_PIXELS
#define CONFIG_KSHIM_BACKDROP_TEXTURE_PIXELS (256U * 256U)
#endif

#define KSHIM_BACKDROP_REFRESH_MS 120U

/* Flat dark scheme: a borderless indigo acrylic panel over the animated
 * background, white text, one accent blue for the selection. */
#define KSHIM_UI_PANEL_TINT 0x161c3eU
#define KSHIM_UI_PANEL_OPA 128          /* 50% over the background */
#define KSHIM_UI_TEXT 0xffffffU
#define KSHIM_UI_TEXT_DIM 0xc9d1eeU     /* subtitle, status, rows at rest */
#define KSHIM_UI_ITEM_OPA 20            /* white plate behind rows at rest */
#define KSHIM_UI_ACCENT 0x2f6bf0U       /* white text on it: ~5:1 */
#define KSHIM_UI_ACCENT_LIGHT 0x7aa2ff
#define KSHIM_UI_FOCUS_OPA 214          /* 84% */
#define KSHIM_UI_SCREEN 0x13235aU       /* under the background image */

static uint8_t mLvglRenderBuffers[2][CONFIG_KSHIM_LVGL_BUFFER_BYTES]
    __attribute__((aligned(8)));
/* The background textures and the inflated font live in scratch RAM
 * outside the runtime slot (kshim_lvgl_set_scratch). */
static void *mScratch;
static size_t mScratchSize;
static uint32_t *mBackdropTextures[2];
static const void *mFontData;
static lv_font_t *mFonts[3];
static const lv_font_t *mTitleFont;
static const lv_font_t *mStatusFont;
static lv_image_dsc_t mBackdropImages[2];
static kshim_backdrop_layout_t mBackdropLayout;

static QcomKeys *mKeys;
static kshim_framebuffer_t *mFramebuffer;
static lv_indev_t *mKeyInput;
static lv_indev_t *mPointerInput;
static lv_display_t *mDisplay;
static lv_obj_t *mBackground;
static lv_obj_t *mPanel;
static lv_obj_t *mList;
static lv_obj_t *mBootButton;
static lv_obj_t *mStatus;
static lv_obj_t *mTitle;
static kshim_lvgl_t *mContext;
static uint32_t mKey;
static uint32_t mLastTextureUpdate;
static uint8_t mKeyReleasePending;
static uint8_t mActiveTexture;
static uint16_t mRowHeight;
static const lv_font_t *mRowFont;

static uint32_t min_u32(uint32_t first, uint32_t second)
{
  return first < second ? first : second;
}

static uint32_t max_u32(uint32_t first, uint32_t second)
{
  return first > second ? first : second;
}

static uint32_t clamp_u32(uint32_t value, uint32_t minimum, uint32_t maximum)
{
  return min_u32(max_u32(value, minimum), maximum);
}

static lv_color_t KshimTextColor(void)
{
  return lv_color_hex(KSHIM_UI_TEXT);
}

static lv_color_t KshimMutedColor(void)
{
  return lv_color_hex(KSHIM_UI_TEXT_DIM);
}

static lv_color_t KshimAccentColor(void)
{
  return lv_color_hex(KSHIM_UI_ACCENT);
}

static void KshimLvglFlush(lv_display_t *Display, const lv_area_t *Area,
                           uint8_t *Pixels)
{
  int32_t Width;
  int32_t Height;
  uint32_t SourceStride;

  if (Display == NULL || Area == NULL || Pixels == NULL) {
    if (Display != NULL)
      lv_display_flush_ready(Display);
    return;
  }
  Width = Area->x2 - Area->x1 + 1;
  Height = Area->y2 - Area->y1 + 1;
  if (Width <= 0 || Height <= 0 || mDisplay == NULL ||
      mFramebuffer == NULL) {
    lv_display_flush_ready(Display);
    return;
  }
  SourceStride = lv_draw_buf_width_to_stride((uint32_t)Width,
                                              LV_COLOR_FORMAT_ARGB8888);
  (void)kshim_fb_blit_argb8888(
      mFramebuffer, Area->x1, Area->y1, (uint32_t)Width, (uint32_t)Height,
      (const uint32_t *)(const void *)Pixels, SourceStride);
  kshim_fb_sync();
  lv_display_flush_ready(Display);
}

static void KshimLvglReadKeys(lv_indev_t *Input, lv_indev_data_t *Data)
{
  QcomKeyEvent Event;
  int Status;

  (void)Input;
  if (Data == NULL)
    return;
  Data->state = LV_INDEV_STATE_RELEASED;
  Data->key = mKey;
  Data->continue_reading = false;
  if (mKeyReleasePending != 0U) {
    mKeyReleasePending = 0U;
    return;
  }
  if (mKeys == NULL)
    return;

  Status = QcomKeysPoll(mKeys, &Event);
  if (Status != QCOM_KEYS_EVENT || Event.Type != QCOM_KEY_EVENT_PRESS)
    return;
  switch (Event.Action) {
  case QCOM_KEY_ACTION_UP:
    mKey = LV_KEY_PREV;
    break;
  case QCOM_KEY_ACTION_DOWN:
    mKey = LV_KEY_NEXT;
    break;
  case QCOM_KEY_ACTION_SELECT:
    mKey = LV_KEY_ENTER;
    break;
  default:
    return;
  }
  Data->key = mKey;
  Data->state = LV_INDEV_STATE_PRESSED;
  mKeyReleasePending = 1U;
}

static void KshimLvglReadPointer(lv_indev_t *Input, lv_indev_data_t *Data)
{
  (void)Input;
  if (Data == NULL || mContext == NULL)
    return;
  Data->point.x = (int32_t)mContext->PointerX;
  Data->point.y = (int32_t)mContext->PointerY;
  Data->state = mContext->PointerPressed != 0U
                    ? LV_INDEV_STATE_PRESSED
                    : LV_INDEV_STATE_RELEASED;
  Data->continue_reading = false;
}

static void KshimSetTranslateX(void *Object, int32_t Value)
{
  lv_obj_set_style_translate_x(Object, Value, 0);
}

static void KshimSetTranslateY(void *Object, int32_t Value)
{
  lv_obj_set_style_translate_y(Object, Value, 0);
}

static void KshimSetOpacity(void *Object, int32_t Value)
{
  if (Value < LV_OPA_TRANSP)
    Value = LV_OPA_TRANSP;
  if (Value > LV_OPA_COVER)
    Value = LV_OPA_COVER;
  lv_obj_set_style_opa(Object, (lv_opa_t)Value, 0);
}

static void KshimSetScale(void *Object, int32_t Value)
{
  lv_obj_set_style_transform_scale(Object, Value, 0);
}

static void KshimSetConfirmPulse(void *Object, int32_t Value)
{
  int32_t Triangle = Value <= 512 ? Value : 1024 - Value;
  lv_obj_set_style_transform_scale(Object, 256 + Triangle * 14 / 512, 0);
}

static void KshimAnimate(lv_obj_t *Object, lv_anim_exec_xcb_t Callback,
                         int32_t Start, int32_t End, uint32_t Duration,
                         lv_anim_path_cb_t Path)
{
  lv_anim_t Animation;

  if (Object == NULL || Callback == NULL)
    return;
  lv_anim_delete(Object, Callback);
  lv_anim_init(&Animation);
  lv_anim_set_var(&Animation, Object);
  lv_anim_set_exec_cb(&Animation, Callback);
  lv_anim_set_values(&Animation, Start, End);
  lv_anim_set_duration(&Animation, Duration);
  lv_anim_set_path_cb(&Animation, Path);
  lv_anim_start(&Animation);
}

static int KshimButtonIndex(const lv_obj_t *Button)
{
  if (mContext == NULL || mContext->Group == NULL || Button == NULL)
    return -1;
  lv_group_t *Group = (lv_group_t *)mContext->Group;
  uint32_t Count = lv_group_get_obj_count(Group);
  for (uint32_t Index = 0; Index < Count; Index++) {
    if (lv_group_get_obj_by_index(Group, Index) == Button)
      return (int)Index;
  }
  return -1;
}

static void KshimConfirmIndex(size_t Index)
{
  if (mContext == NULL || Index >= mContext->EntryCount)
    return;
  mContext->PendingIndex = (int)Index;
  if (mContext->DefaultEntries != 0U && Index < KSHIM_MENU_POWER_OFF)
    mContext->PendingAction = (kshim_menu_action_t)(Index + 1U);
  if (mStatus != NULL)
    lv_label_set_text(mStatus, mContext->Entries[Index]);
  if (mBootButton != NULL)
    KshimAnimate(mBootButton, KshimSetConfirmPulse, 0, 1024,
                 KSHIM_UI_CONFIRM_DURATION_MS, lv_anim_path_ease_in_out);
}

static void KshimMenuEvent(lv_event_t *Event)
{
  if (Event == NULL || mContext == NULL)
    return;
  lv_obj_t *Button = lv_event_get_target_obj(Event);
  lv_obj_t *Label = lv_obj_get_child(Button, 0);
  lv_event_code_t Code = lv_event_get_code(Event);
  int Index = (int)(uintptr_t)lv_event_get_user_data(Event) - 1;

  if (Code == LV_EVENT_FOCUSED || Code == LV_EVENT_DEFOCUSED) {
    if (Label != NULL) {
      int32_t Offset = lv_obj_get_style_translate_x(Label, LV_PART_MAIN);
      KshimAnimate(Label, KshimSetTranslateX, Offset,
                   Code == LV_EVENT_FOCUSED ? 12 : 0,
                   KSHIM_UI_FOCUS_DURATION_MS, lv_anim_path_overshoot);
    }
    if (Code == LV_EVENT_FOCUSED) {
      lv_obj_scroll_to_view(Button, LV_ANIM_ON);
      if (Index >= 0 && (size_t)Index < mContext->EntryCount &&
          mStatus != NULL)
        lv_label_set_text(mStatus, mContext->Entries[Index]);
    }
    return;
  }

  if (Code == LV_EVENT_PRESSED) {
    lv_indev_t *Input = lv_event_get_indev(Event);
    if (Input != NULL && lv_indev_get_type(Input) == LV_INDEV_TYPE_POINTER)
      lv_group_focus_obj(Button);
    KshimAnimate(Button, KshimSetScale,
                 lv_obj_get_style_transform_scale_x(Button, LV_PART_MAIN),
                 248, KSHIM_UI_PRESS_DURATION_MS, lv_anim_path_ease_out);
    return;
  }
  if (Code == LV_EVENT_RELEASED || Code == LV_EVENT_PRESS_LOST) {
    KshimAnimate(Button, KshimSetScale,
                 lv_obj_get_style_transform_scale_x(Button, LV_PART_MAIN),
                 256, KSHIM_UI_PRESS_DURATION_MS, lv_anim_path_ease_out);
    return;
  }
  if (Code != LV_EVENT_CLICKED || Index < 0 ||
      (size_t)Index >= mContext->EntryCount)
    return;

  lv_indev_t *Input = lv_event_get_indev(Event);
  /* A row tap changes focus. Touch confirmation is deliberately isolated on
   * the fixed Boot button so an imprecise scroll cannot launch an image. */
  if (Input != NULL && lv_indev_get_type(Input) == LV_INDEV_TYPE_POINTER)
    return;
  KshimConfirmIndex((size_t)Index);
}

static void KshimBootEvent(lv_event_t *Event)
{
  if (Event == NULL || lv_event_get_code(Event) != LV_EVENT_CLICKED ||
      mContext == NULL || mContext->Group == NULL)
    return;
  lv_obj_t *Focused = lv_group_get_focused((lv_group_t *)mContext->Group);
  int Index = KshimButtonIndex(Focused);
  if (Index >= 0)
    KshimConfirmIndex((size_t)Index);
}

/* Flat rows: a faint white plate at rest, the accent when focused; no
 * border, gradient or shadow. */
static void KshimStyleEntry(lv_obj_t *Button)
{
  uint32_t Radius = clamp_u32(mRowHeight / 5U, 8U, 16U);

  lv_obj_set_width(Button, LV_PCT(100));
  lv_obj_set_height(Button, mRowHeight);
  lv_obj_set_style_radius(Button, (int32_t)Radius, 0);
  lv_obj_set_style_bg_color(Button, lv_color_white(), 0);
  lv_obj_set_style_bg_opa(Button, KSHIM_UI_ITEM_OPA, 0);
  lv_obj_set_style_bg_color(Button, KshimAccentColor(), LV_STATE_FOCUSED);
  lv_obj_set_style_bg_opa(Button, KSHIM_UI_FOCUS_OPA, LV_STATE_FOCUSED);
  lv_obj_set_style_bg_color(Button, KshimAccentColor(), LV_STATE_PRESSED);
  lv_obj_set_style_bg_opa(Button, LV_OPA_COVER, LV_STATE_PRESSED);
  lv_obj_set_style_border_width(Button, 0, 0);
  lv_obj_set_style_border_width(Button, 0, LV_STATE_FOCUSED);
  lv_obj_set_style_shadow_width(Button, 0, 0);
  lv_obj_set_style_outline_width(Button, 0, 0);
  lv_obj_set_style_outline_width(Button, 0, LV_STATE_FOCUS_KEY);
  lv_obj_set_style_text_color(Button, KshimMutedColor(), 0);
  lv_obj_set_style_text_color(Button, KshimTextColor(), LV_STATE_FOCUSED);
  lv_obj_set_style_text_font(Button, mRowFont, 0);
  lv_obj_set_style_pad_hor(Button, mRowHeight / 4U, 0);
  lv_obj_set_style_pad_ver(Button, max_u32(4U, mRowHeight / 6U), 0);
  lv_obj_set_flex_align(Button, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                        LV_FLEX_ALIGN_CENTER);
}

static uint32_t KshimPadding(void)
{
  uint32_t ShortEdge = min_u32(mFramebuffer->width, mFramebuffer->height);
  return clamp_u32(ShortEdge / 28U, 2U, 36U);
}

static int KshimRelayoutChrome(void)
{
  uint32_t ShortEdge;
  uint32_t Padding;
  int32_t TitleHeight;
  int32_t StatusHeight;
  uint32_t BootHeight;
  int32_t ListY;
  int32_t ListHeight;
  kshim_backdrop_layout_t NextLayout;

  if (mContext == NULL || mFramebuffer == NULL || mContext->EntryCount == 0U)
    return -1;
  if (kshim_backdrop_layout(
          mFramebuffer->width, mFramebuffer->height,
          CONFIG_KSHIM_BACKDROP_TEXTURE_PIXELS, mContext->EntryCount,
          &NextLayout) != 0)
    return -2;
  mBackdropLayout = NextLayout;
  if (mPanel == NULL)
    return 0;

  ShortEdge = min_u32(mFramebuffer->width, mFramebuffer->height);
  Padding = KshimPadding();
  TitleHeight = (int32_t)lv_obj_get_height(lv_obj_get_child(mPanel, 0));
  if (TitleHeight <= 0)
    TitleHeight = ShortEdge >= 720U ? 70 :
                  ShortEdge >= 320U ? 41 : 29;
  StatusHeight = ShortEdge >= 160U ? lv_font_get_line_height(mStatusFont) : 0;
  BootHeight = clamp_u32(ShortEdge / 11U, 16U, 88U);
  ListY = (int32_t)Padding + TitleHeight +
          (StatusHeight > 0 ? StatusHeight + (int32_t)Padding / 3 : 0) +
          (int32_t)Padding;
  ListHeight = (int32_t)mBackdropLayout.panel_height - ListY -
               (int32_t)BootHeight - (int32_t)Padding * 2;
  if (ListHeight < 8)
    ListHeight = 8;

  lv_obj_set_pos(mPanel, mBackdropLayout.panel_x, mBackdropLayout.panel_y);
  lv_obj_set_size(mPanel, mBackdropLayout.panel_width,
                  mBackdropLayout.panel_height);
  lv_obj_set_style_radius(mPanel, (int32_t)mBackdropLayout.panel_radius, 0);
  if (mStatus != NULL) {
    lv_obj_set_width(mStatus, (int32_t)mBackdropLayout.panel_width -
                              (int32_t)Padding * 2);
    lv_obj_set_pos(mStatus, (int32_t)Padding,
                   (int32_t)Padding + TitleHeight);
  }
  if (mList != NULL) {
    lv_obj_set_pos(mList, (int32_t)Padding, ListY);
    lv_obj_set_size(mList, (int32_t)mBackdropLayout.panel_width -
                            (int32_t)Padding * 2, ListHeight);
  }
  if (mBootButton != NULL) {
    lv_obj_set_pos(mBootButton, (int32_t)Padding,
                   (int32_t)mBackdropLayout.panel_height -
                   (int32_t)BootHeight - (int32_t)Padding);
    lv_obj_set_size(mBootButton, (int32_t)mBackdropLayout.panel_width -
                                 (int32_t)Padding * 2, BootHeight);
  }
  lv_obj_update_layout(lv_screen_active());
  return 0;
}

static int KshimRebuildEntries(size_t Initial)
{
  lv_group_t *Group;

  if (mContext == NULL || mList == NULL || mContext->Group == NULL ||
      mContext->EntryCount == 0U || Initial >= mContext->EntryCount)
    return -1;
  if (KshimRelayoutChrome() != 0)
    return -2;
  Group = (lv_group_t *)mContext->Group;
  lv_group_remove_all_objs(Group);
  lv_obj_clean(mList);
  for (size_t Index = 0; Index < mContext->EntryCount; Index++) {
    lv_obj_t *Button = lv_list_add_button(mList, NULL,
                                          mContext->Entries[Index]);
    if (Button == NULL)
      return -3;
    KshimStyleEntry(Button);
    lv_obj_add_event_cb(Button, KshimMenuEvent, LV_EVENT_ALL,
                        (void *)(uintptr_t)(Index + 1U));
    lv_group_add_obj(Group, Button);
  }
  lv_obj_t *InitialButton = lv_group_get_obj_by_index(Group,
                                                       (uint32_t)Initial);
  if (InitialButton == NULL)
    return -4;
  lv_group_focus_obj(InitialButton);
  lv_obj_scroll_to_view(InitialButton, LV_ANIM_OFF);
  if (mStatus != NULL)
    lv_label_set_text(mStatus, mContext->Entries[Initial]);
  return 0;
}

static void KshimPrepareImage(lv_image_dsc_t *Image, uint32_t *Pixels)
{
  *Image = (lv_image_dsc_t){
      .header = {
          .magic = LV_IMAGE_HEADER_MAGIC,
          .cf = LV_COLOR_FORMAT_ARGB8888,
          .flags = 0,
          .w = mBackdropLayout.texture_width,
          .h = mBackdropLayout.texture_height,
          .stride = (uint16_t)(mBackdropLayout.texture_width * sizeof(uint32_t)),
      },
      .data_size = (uint32_t)((size_t)mBackdropLayout.texture_width *
                              mBackdropLayout.texture_height * sizeof(uint32_t)),
      .data = (const uint8_t *)(const void *)Pixels,
  };
}

/* Carve the two textures and the inflated font out of the scratch memory.
 * Either may be missing: the menu then has a plain background or the
 * Latin-only fallback font. */
static void KshimPlaceScratch(void)
{
  size_t Texture = (size_t)CONFIG_KSHIM_BACKDROP_TEXTURE_PIXELS * sizeof(uint32_t);
  uint8_t *Cursor = mScratch;
  size_t Left = mScratchSize;

  mBackdropTextures[0] = mBackdropTextures[1] = NULL;
  mFontData = NULL;
  if (Cursor == NULL || ((uintptr_t)Cursor & 63U) != 0U)
    return;
  if (Left >= 2U * Texture) {
    mBackdropTextures[0] = (uint32_t *)(void *)Cursor;
    mBackdropTextures[1] = (uint32_t *)(void *)(Cursor + Texture);
    Cursor += 2U * Texture;
    Left -= 2U * Texture;
  }
  (void)kshim_ui_font_inflate(Cursor, Left, &mFontData);
}

static int KshimRenderInitialBackground(void)
{
  mActiveTexture = 0U;
  mLastTextureUpdate = 0U;
  if (mBackdropTextures[0] == NULL)
    return 0;
  if (kshim_backdrop_render(mBackdropTextures[0],
                            CONFIG_KSHIM_BACKDROP_TEXTURE_PIXELS,
                            &mBackdropLayout, 0U, true) != 0)
    return -1;
  KshimPrepareImage(&mBackdropImages[0], mBackdropTextures[0]);
  KshimPrepareImage(&mBackdropImages[1], mBackdropTextures[1]);
  return 0;
}

/* Noto Sans CJK SC at the three sizes the chrome uses (title, rows,
 * status); Montserrat 20 when the font could not be inflated. */
static void KshimCreateFonts(uint32_t ShortEdge)
{
  static const int32_t Sizes[3][3] = {
      {48, 28, 22}, {28, 20, 16}, {20, 14, 12},
  };
  const int32_t *Size = Sizes[ShortEdge >= 720U ? 0 : ShortEdge >= 320U ? 1 : 2];

  for (unsigned Index = 0; Index < 3U; Index++) {
    mFonts[Index] = mFontData == NULL ? NULL :
        lv_tiny_ttf_create_data_ex(mFontData, kshim_ui_font_size(), Size[Index],
                                   LV_FONT_KERNING_NONE,
                                   Index == 0U ? 32U : LV_TINY_TTF_CACHE_GLYPH_CNT);
  }
  mTitleFont = mFonts[0] != NULL ? mFonts[0] : &lv_font_montserrat_20;
  mRowFont = mFonts[1] != NULL ? mFonts[1] : &lv_font_montserrat_20;
  mStatusFont = mFonts[2] != NULL ? mFonts[2] : &lv_font_montserrat_20;
}

/* The fonts live on LVGL's heap, which lv_deinit() releases as a whole;
 * destroying them earlier would leave the labels it deletes dangling. */
static void KshimForgetFonts(void)
{
  for (unsigned Index = 0; Index < 3U; Index++)
    mFonts[Index] = NULL;
  mTitleFont = mRowFont = mStatusFont = NULL;
}

static int KshimBuildChrome(void)
{
  lv_obj_t *Screen = lv_screen_active();
  if (Screen == NULL || mContext == NULL)
    return -1;

  uint32_t ShortEdge = min_u32(mFramebuffer->width, mFramebuffer->height);
  uint32_t Padding = clamp_u32(ShortEdge / 28U, 2U, 36U);
  KshimCreateFonts(ShortEdge);
  const lv_font_t *TitleFont = mTitleFont;
  const lv_font_t *StatusFont = mStatusFont;
  mRowHeight = ShortEdge >= 720U ? 72U : ShortEdge >= 320U ? 52U : 24U;

  lv_obj_set_style_bg_color(Screen, lv_color_hex(KSHIM_UI_SCREEN), 0);
  lv_obj_set_style_bg_opa(Screen, LV_OPA_COVER, 0);
  lv_obj_set_style_text_color(Screen, KshimTextColor(), 0);
  lv_obj_set_scrollbar_mode(Screen, LV_SCROLLBAR_MODE_OFF);

  mBackground = lv_image_create(Screen);
  if (mBackground == NULL)
    return -1;
  lv_obj_set_size(mBackground, LV_PCT(100), LV_PCT(100));
  lv_obj_align(mBackground, LV_ALIGN_CENTER, 0, 0);
  if (mBackdropTextures[0] != NULL)
    lv_image_set_src(mBackground, &mBackdropImages[0]);
  lv_image_set_inner_align(mBackground, LV_IMAGE_ALIGN_STRETCH);
  lv_image_set_antialias(mBackground, true);
  lv_obj_remove_flag(mBackground, LV_OBJ_FLAG_CLICKABLE);

  mPanel = lv_obj_create(Screen);
  if (mPanel == NULL)
    return -1;
  lv_obj_set_pos(mPanel, mBackdropLayout.panel_x, mBackdropLayout.panel_y);
  lv_obj_set_size(mPanel, mBackdropLayout.panel_width,
                  mBackdropLayout.panel_height);
  lv_obj_set_style_radius(mPanel, (int32_t)mBackdropLayout.panel_radius, 0);
  /* Flat acrylic: the (already soft) background under a translucent
   * indigo tint, with no border, rim or shadow. */
  lv_obj_set_style_bg_color(mPanel, lv_color_hex(KSHIM_UI_PANEL_TINT), 0);
  lv_obj_set_style_bg_opa(mPanel, KSHIM_UI_PANEL_OPA, 0);
  lv_obj_set_style_border_width(mPanel, 0, 0);
  lv_obj_set_style_shadow_width(mPanel, 0, 0);
  lv_obj_set_style_pad_all(mPanel, 0, 0);
  lv_obj_set_scrollbar_mode(mPanel, LV_SCROLLBAR_MODE_OFF);
  lv_obj_remove_flag(mPanel, LV_OBJ_FLAG_SCROLLABLE);

  int32_t TitleHeight = lv_font_get_line_height(TitleFont);
  int32_t StatusHeight = ShortEdge >= 160U
      ? lv_font_get_line_height(StatusFont) : 0;
  uint32_t BootHeight = clamp_u32(ShortEdge / 11U, 16U, 88U);
  int32_t ListY = (int32_t)Padding + TitleHeight +
                  (StatusHeight > 0 ? StatusHeight + (int32_t)Padding / 3 : 0) +
                  (int32_t)Padding;
  int32_t ListHeight = (int32_t)mBackdropLayout.panel_height - ListY -
                       (int32_t)BootHeight - (int32_t)Padding * 2;
  if (ListHeight < 8)
    ListHeight = 8;

  lv_obj_t *Title = lv_label_create(mPanel);
  if (Title == NULL)
    return -1;
  lv_label_set_text_static(Title, "Select OS to Boot");
  lv_obj_set_style_text_font(Title, TitleFont, 0);
  lv_obj_set_style_text_color(Title, KshimTextColor(), 0);
  lv_obj_align(Title, LV_ALIGN_TOP_LEFT, (int32_t)Padding,
               (int32_t)Padding);

  mStatus = lv_label_create(mPanel);
  if (mStatus == NULL)
    return -1;
  /* Long-mode ellipsis rewrites its owned buffer, so this label must copy. */
  lv_label_set_text(mStatus, "选择要启动的系统");
  lv_obj_set_style_text_font(mStatus, StatusFont, 0);
  lv_obj_set_style_text_color(mStatus, KshimMutedColor(), 0);
  lv_obj_set_width(mStatus, (int32_t)mBackdropLayout.panel_width -
                            (int32_t)Padding * 2);
  lv_label_set_long_mode(mStatus, LV_LABEL_LONG_DOT);
  lv_obj_align(mStatus, LV_ALIGN_TOP_LEFT, (int32_t)Padding,
               (int32_t)Padding + TitleHeight);
  if (StatusHeight == 0)
    lv_obj_add_flag(mStatus, LV_OBJ_FLAG_HIDDEN);
  mTitle = Title;

  mList = lv_list_create(mPanel);
  if (mList == NULL)
    return -1;
  lv_obj_set_pos(mList, (int32_t)Padding, ListY);
  lv_obj_set_size(mList, (int32_t)mBackdropLayout.panel_width -
                         (int32_t)Padding * 2, ListHeight);
  lv_obj_set_style_bg_opa(mList, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(mList, 0, 0);
  lv_obj_set_style_radius(mList, 0, 0);
  lv_obj_set_style_pad_all(mList, 0, 0);
  lv_obj_set_style_pad_row(mList, max_u32(2U, Padding / 4U), 0);
  lv_obj_set_style_bg_color(mList, lv_color_hex(KSHIM_UI_ACCENT_LIGHT),
                            LV_PART_SCROLLBAR);
  lv_obj_set_style_bg_opa(mList, LV_OPA_50, LV_PART_SCROLLBAR);
  lv_obj_set_style_width(mList, 3, LV_PART_SCROLLBAR);
  lv_obj_set_scrollbar_mode(mList, LV_SCROLLBAR_MODE_AUTO);

  mBootButton = lv_button_create(mPanel);
  if (mBootButton == NULL)
    return -1;
  lv_obj_set_pos(mBootButton, (int32_t)Padding,
                 (int32_t)mBackdropLayout.panel_height -
                 (int32_t)BootHeight - (int32_t)Padding);
  lv_obj_set_size(mBootButton, (int32_t)mBackdropLayout.panel_width -
                               (int32_t)Padding * 2, BootHeight);
  /* The primary action: a solid light plate with dark text, distinct from
   * the accent-blue selection above it. */
  lv_obj_set_style_radius(mBootButton, clamp_u32(BootHeight / 5U, 8U, 16U), 0);
  lv_obj_set_style_bg_color(mBootButton, lv_color_hex(0xf4f7ffU), 0);
  lv_obj_set_style_bg_opa(mBootButton, LV_OPA_COVER, 0);
  lv_obj_set_style_bg_color(mBootButton, lv_color_hex(0xdfe5fbU),
                            LV_STATE_PRESSED);
  lv_obj_set_style_border_width(mBootButton, 0, 0);
  lv_obj_set_style_shadow_width(mBootButton, 0, 0);
  lv_obj_set_style_outline_width(mBootButton, 0, LV_STATE_FOCUS_KEY);
  lv_obj_add_event_cb(mBootButton, KshimBootEvent, LV_EVENT_CLICKED, NULL);

  lv_obj_t *BootLabel = lv_label_create(mBootButton);
  if (BootLabel == NULL)
    return -1;
  lv_label_set_text_static(BootLabel, "Boot");
  lv_obj_set_style_text_font(BootLabel, mRowFont, 0);
  lv_obj_set_style_text_color(BootLabel, lv_color_hex(KSHIM_UI_PANEL_TINT),
                              0);
  lv_obj_center(BootLabel);

  lv_obj_update_layout(Screen);
  if (KshimRelayoutChrome() != 0)
    return -1;
  lv_obj_set_style_translate_y(mPanel, 32, 0);
  lv_obj_set_style_opa(mPanel, LV_OPA_TRANSP, 0);
  KshimAnimate(mPanel, KshimSetTranslateY, 32, 0,
               KSHIM_UI_ENTRY_DURATION_MS, lv_anim_path_ease_out);
  KshimAnimate(mPanel, KshimSetOpacity, LV_OPA_TRANSP, LV_OPA_COVER,
               KSHIM_UI_ENTRY_DURATION_MS, lv_anim_path_ease_out);
  return 0;
}

int kshim_lvgl_init(kshim_lvgl_t *Context, kshim_framebuffer_t *Framebuffer,
                    QcomKeys *Keys)
{
  static const char *const DefaultEntries[] = {
      CONFIG_KSHIM_MENU_ENTRY1, CONFIG_KSHIM_MENU_ENTRY2,
#if !defined(CONFIG_KSHIM_UEFI_TEST_MENU) || !CONFIG_KSHIM_UEFI_TEST_MENU
      "Power off",
#endif
  };
  if (mContext != NULL)
    return -9;
  if (Context != NULL)
    *Context = (kshim_lvgl_t){0};
  if (Context == NULL || Framebuffer == NULL ||
      Framebuffer->width < 64U || Framebuffer->height < 48U ||
      Framebuffer->render_address == 0U ||
      Framebuffer->width > INT32_MAX || Framebuffer->height > INT32_MAX)
    return -1;
  if (kshim_fb_format_bpp(Framebuffer->format) == 0U)
    return -2;
  if (Framebuffer->width > UINT32_MAX / sizeof(uint32_t))
    return -3;
  uint32_t RenderStride = Framebuffer->width * sizeof(uint32_t);
  if (sizeof(mLvglRenderBuffers[0]) < RenderStride)
    return -3;
  if (kshim_backdrop_layout(
          Framebuffer->width, Framebuffer->height,
          CONFIG_KSHIM_BACKDROP_TEXTURE_PIXELS,
          sizeof(DefaultEntries) / sizeof(DefaultEntries[0]),
          &mBackdropLayout) != 0)
    return -4;

  *Context = (kshim_lvgl_t){
      .Framebuffer = Framebuffer,
      .Keys = Keys,
      .PendingIndex = -1,
      .PointerX = Framebuffer->width / 2U,
      .PointerY = Framebuffer->height / 2U,
      .DefaultEntries = 1U,
  };
  Context->EntryCount = sizeof(DefaultEntries) / sizeof(DefaultEntries[0]);
  for (size_t Index = 0; Index < Context->EntryCount; Index++)
    Context->Entries[Index] = DefaultEntries[Index];

  mContext = Context;
  mKeys = Keys;
  mFramebuffer = Framebuffer;
  mDisplay = NULL;
  mKeyInput = NULL;
  mPointerInput = NULL;
  mBackground = NULL;
  mPanel = NULL;
  mList = NULL;
  mBootButton = NULL;
  mStatus = NULL;
  mTitle = NULL;
  mKey = LV_KEY_ENTER;
  mKeyReleasePending = 0U;
  KshimPlaceScratch();
  if (KshimRenderInitialBackground() != 0)
    goto Failed;

  lv_init();
  mDisplay = lv_display_create((int32_t)Framebuffer->width,
                               (int32_t)Framebuffer->height);
  if (mDisplay == NULL)
    goto Failed;
  lv_display_set_color_format(mDisplay, LV_COLOR_FORMAT_ARGB8888);
  lv_display_set_antialiasing(mDisplay, true);
  lv_display_set_flush_cb(mDisplay, KshimLvglFlush);
  lv_display_set_buffers(mDisplay, mLvglRenderBuffers[0],
                         mLvglRenderBuffers[1],
                         sizeof(mLvglRenderBuffers[0]),
                         LV_DISPLAY_RENDER_MODE_PARTIAL);

  mKeyInput = lv_indev_create();
  if (mKeyInput == NULL)
    goto Failed;
  lv_indev_set_type(mKeyInput, LV_INDEV_TYPE_KEYPAD);
  lv_indev_set_read_cb(mKeyInput, KshimLvglReadKeys);
  lv_indev_set_display(mKeyInput, mDisplay);

  mPointerInput = lv_indev_create();
  if (mPointerInput == NULL)
    goto Failed;
  lv_indev_set_type(mPointerInput, LV_INDEV_TYPE_POINTER);
  lv_indev_set_read_cb(mPointerInput, KshimLvglReadPointer);
  lv_indev_set_display(mPointerInput, mDisplay);
  lv_indev_set_mode(mPointerInput, LV_INDEV_MODE_EVENT);

  lv_group_t *Group = lv_group_create();
  if (Group == NULL)
    goto Failed;
  lv_group_set_default(Group);
  lv_indev_set_group(mKeyInput, Group);
  Context->Group = Group;
  if (KshimBuildChrome() != 0 || KshimRebuildEntries(0U) != 0)
    goto Failed;

  Context->Display = mDisplay;
  Context->Input = mKeyInput;
  Context->PointerInput = mPointerInput;
  Context->List = mList;
  Context->BootButton = mBootButton;
  Context->Background = mBackground;
  Context->TextureWidth = mBackdropLayout.texture_width;
  Context->TextureHeight = mBackdropLayout.texture_height;
  Context->Ready = 1U;
  return 0;

Failed:
  kshim_lvgl_deinit(Context);
  return -5;
}

void kshim_lvgl_frame(kshim_lvgl_t *Context, uint32_t ElapsedMs)
{
  if (Context == NULL || Context != mContext || Context->Ready == 0U)
    return;
  if (ElapsedMs != 0U) {
    lv_tick_inc(ElapsedMs);
    Context->ElapsedMs += ElapsedMs;
    if (ElapsedMs > 40U)
      Context->ReducedQuality = 1U;
  }
  lv_timer_handler();

  if (Context->CountdownEnabled != 0U && mStatus != NULL) {
    uint32_t remaining = Context->ElapsedMs < Context->CountdownMs ?
                         Context->CountdownMs - Context->ElapsedMs : 0U;
    uint32_t seconds = (remaining + 999U) / 1000U;
    if (seconds != Context->CountdownShown) {
      char countdown[96];
      (void)snprintf(countdown, sizeof(countdown),
                     "Auto boot in %u s / 自动启动倒计时 %u 秒",
                     (unsigned)seconds, (unsigned)seconds);
      lv_label_set_text(mStatus, countdown);
      Context->CountdownShown = seconds;
    }
  }

  if (Context->ElapsedMs - mLastTextureUpdate >= KSHIM_BACKDROP_REFRESH_MS &&
      mBackground != NULL && mBackdropTextures[0] != NULL) {
    uint8_t Next = (uint8_t)(mActiveTexture ^ 1U);
    lv_image_cache_drop(&mBackdropImages[Next]);
    if (kshim_backdrop_render(mBackdropTextures[Next],
                              CONFIG_KSHIM_BACKDROP_TEXTURE_PIXELS,
                              &mBackdropLayout, Context->ElapsedMs,
                              Context->ReducedQuality != 0U) == 0) {
      lv_image_set_src(mBackground, &mBackdropImages[Next]);
      mActiveTexture = Next;
      mLastTextureUpdate = Context->ElapsedMs;
    }
  }
}

int kshim_lvgl_ready(const kshim_lvgl_t *Context)
{
  return Context != NULL && Context == mContext && Context->Ready != 0U;
}

kshim_menu_action_t kshim_lvgl_take_selection(kshim_lvgl_t *Context)
{
  if (!kshim_lvgl_ready(Context))
    return KSHIM_MENU_NONE;
  kshim_menu_action_t Action = Context->PendingAction;
  Context->PendingAction = KSHIM_MENU_NONE;
  return Action;
}

int kshim_lvgl_take_index(kshim_lvgl_t *Context)
{
  if (!kshim_lvgl_ready(Context))
    return -1;
  int Index = Context->PendingIndex;
  Context->PendingIndex = -1;
  return Index;
}

int kshim_lvgl_set_entries(kshim_lvgl_t *Context,
                           const char *const *Names, size_t Count,
                           size_t Initial)
{
  if (!kshim_lvgl_ready(Context) || Names == NULL || Count == 0U ||
      Count > KSHIM_MENU_MAX_ENTRIES || Initial >= Count)
    return -1;
  for (size_t Index = 0; Index < Count; Index++) {
    if (Names[Index] == NULL || Names[Index][0] == '\0')
      return -2;
  }
  Context->EntryCount = Count;
  Context->DefaultEntries = 0U;
  Context->PendingAction = KSHIM_MENU_NONE;
  Context->PendingIndex = -1;
  for (size_t Index = 0; Index < Count; Index++)
    Context->Entries[Index] = Names[Index];
  for (size_t Index = Count; Index < KSHIM_MENU_MAX_ENTRIES; Index++)
    Context->Entries[Index] = NULL;
  return KshimRebuildEntries(Initial);
}

void kshim_lvgl_set_status(kshim_lvgl_t *Context, const char *Text)
{
  if (kshim_lvgl_ready(Context) && Text != NULL && mStatus != NULL)
    lv_label_set_text(mStatus, Text);
}

void kshim_lvgl_set_countdown(kshim_lvgl_t *Context, uint32_t timeout_ms)
{
  if (!kshim_lvgl_ready(Context))
    return;
  Context->CountdownMs = timeout_ms;
  Context->CountdownEnabled = timeout_ms != 0U;
  Context->CountdownShown = UINT32_MAX;
}

void kshim_lvgl_touch(void *Context, const kshim_touch_event_t *Event)
{
  kshim_lvgl_t *Ui = Context;
  if (!kshim_lvgl_ready(Ui) || Event == NULL)
    return;
  Ui->PointerX = min_u32(Event->x, Ui->Framebuffer->width - 1U);
  Ui->PointerY = min_u32(Event->y, Ui->Framebuffer->height - 1U);
  switch (Event->type) {
  case KSHIM_TOUCH_EVENT_PRESS:
  case KSHIM_TOUCH_EVENT_MOVE:
    Ui->PointerPressed = 1U;
    break;
  case KSHIM_TOUCH_EVENT_RELEASE:
    Ui->PointerPressed = 0U;
    break;
  case KSHIM_TOUCH_EVENT_CANCEL:
    Ui->PointerPressed = 0U;
    /* A bus failure or controller reset must not confirm the Boot button. */
    lv_indev_wait_release(mPointerInput);
    break;
  default:
    return;
  }
  /* Both ST controllers can report a whole tap in one FIFO batch. Deliver
   * every transition on the UI/boot CPU before the next rendering frame. */
  lv_indev_read(mPointerInput);
}

void kshim_lvgl_set_scratch(void *Base, size_t Size)
{
  if (mContext != NULL)
    return;
  mScratch = Base;
  mScratchSize = Base != NULL ? Size : 0U;
}

int kshim_lvgl_has_cjk_font(const kshim_lvgl_t *Context)
{
  return kshim_lvgl_ready(Context) && mFonts[1] != NULL;
}

void kshim_lvgl_deinit(kshim_lvgl_t *Context)
{
  if (Context == NULL || Context != mContext)
    return;
  lv_deinit();
  KshimForgetFonts();
  mBackdropTextures[0] = mBackdropTextures[1] = NULL;
  mFontData = NULL;
  *Context = (kshim_lvgl_t){0};
  mContext = NULL;
  mKeys = NULL;
  mFramebuffer = NULL;
  mDisplay = NULL;
  mKeyInput = NULL;
  mPointerInput = NULL;
  mBackground = NULL;
  mPanel = NULL;
  mList = NULL;
  mBootButton = NULL;
  mStatus = NULL;
  mTitle = NULL;
}
