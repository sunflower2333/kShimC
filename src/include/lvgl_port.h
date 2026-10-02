#pragma once

#include <stddef.h>
#include <stdint.h>

#include <framebuffer.h>
#include <qcom_keys.h>
#include <touch.h>

#define KSHIM_MENU_MAX_ENTRIES 49U
#define KSHIM_UI_ENTRY_DURATION_MS 360U
#define KSHIM_UI_FOCUS_DURATION_MS 260U
#define KSHIM_UI_PRESS_DURATION_MS 120U
#define KSHIM_UI_CONFIRM_DURATION_MS 180U

typedef struct kshim_lvgl kshim_lvgl_t;

typedef enum {
  KSHIM_MENU_NONE = 0,
  KSHIM_MENU_BOOT,
  KSHIM_MENU_RECOVERY,
  KSHIM_MENU_POWER_OFF,
} kshim_menu_action_t;

/* Initialize the LVGL glass menu and Qualcomm keypad. */
int kshim_lvgl_init(kshim_lvgl_t *Context, kshim_framebuffer_t *Framebuffer,
                    QcomKeys *Keys);

/* Advance LVGL's cooperative timer loop. elapsed_ms may be zero. */
void kshim_lvgl_frame(kshim_lvgl_t *Context, uint32_t ElapsedMs);

/* Return non-zero after kshim_lvgl_init has installed a display. */
int kshim_lvgl_ready(const kshim_lvgl_t *Context);

/* Consume one Power-confirmed menu choice; NONE means no new confirmation. */
kshim_menu_action_t kshim_lvgl_take_selection(kshim_lvgl_t *Context);
void kshim_lvgl_set_status(kshim_lvgl_t *Context, const char *Text);
void kshim_lvgl_set_countdown(kshim_lvgl_t *Context, uint32_t timeout_ms);

/* Replace the compatibility menu with 1..49 manifest-owned labels. */
int kshim_lvgl_set_entries(kshim_lvgl_t *Context,
                           const char *const *Names, size_t Count,
                           size_t Initial);

/* Consume one confirmed dynamic-menu index; -1 means no new selection. */
int kshim_lvgl_take_index(kshim_lvgl_t *Context);

/* Touch callback for kshim_touch_config_t.emit; call on the UI/boot CPU,
 * outside rendering. Each event is synchronously delivered to LVGL. */
void kshim_lvgl_touch(void *Context, const kshim_touch_event_t *Event);

/* Release the single early-boot UI instance, including partially built menus. */
void kshim_lvgl_deinit(kshim_lvgl_t *Context);

/* Board hook called by the main loop. Return zero if the action was accepted.
 * The weak default reports unavailable until a board supplies a boot/power
 * handler; menu confirmation never pretends to perform a missing action. */
int kshim_menu_activate(kshim_menu_action_t Action);

struct kshim_lvgl {
  kshim_framebuffer_t *Framebuffer;
  QcomKeys            *Keys;
  void                *Display;
  void                *Input;
  void                *PointerInput;
  void                *Group;
  void                *List;
  void                *BootButton;
  void                *Background;
  const char           *Entries[KSHIM_MENU_MAX_ENTRIES];
  size_t                EntryCount;
  kshim_menu_action_t   PendingAction;
  int                   PendingIndex;
  uint32_t              PointerX;
  uint32_t              PointerY;
  uint32_t              ElapsedMs;
  uint16_t              TextureWidth;
  uint16_t              TextureHeight;
  uint8_t              Ready;
  uint8_t              PointerPressed;
  uint8_t              DefaultEntries;
  uint8_t              ReducedQuality;
  uint8_t              CountdownEnabled;
  uint32_t             CountdownMs;
  uint32_t             CountdownShown;
};
