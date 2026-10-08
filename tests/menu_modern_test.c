/* 现代主题的布局和交互专项；使用真实 LVGL，不模拟绘制 API。 */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <lvgl.h>
#include <lvgl_port.h>
#include "../src/ui/menu_style.h"

/* 等待入场、焦点及滚动动画稳定。 */
static void Settle(kshim_lvgl_t *Ui)
{
    for (unsigned Frame = 0; Frame < 48U; Frame++)
        kshim_lvgl_frame(Ui, 16U);
}

/* 从真实控件坐标发送触摸，验证卡片或分栏不会绕过确认规则。 */
static void Tap(kshim_lvgl_t *Ui, lv_obj_t *Object, int Cancel)
{
    lv_area_t Area;
    lv_obj_get_coords(Object, &Area);
    assert(Area.x1 >= 0 && Area.y1 >= 0);
    kshim_touch_event_t Event = {
        .type = KSHIM_TOUCH_EVENT_PRESS,
        .x = (uint32_t)((Area.x1 + Area.x2) / 2),
        .y = (uint32_t)((Area.y1 + Area.y2) / 2),
    };
    kshim_lvgl_touch(Ui, &Event);
    Event.type = Cancel ? KSHIM_TOUCH_EVENT_CANCEL : KSHIM_TOUCH_EVENT_RELEASE;
    kshim_lvgl_touch(Ui, &Event);
    Settle(Ui);
}

/* 检查几何边界以及卡片行优先顺序，不能让 Boot 按钮覆盖菜单。 */
static void CheckLayout(kshim_lvgl_t *Ui, unsigned Width, unsigned Height)
{
    lv_area_t Panel, List, Boot, First, Second;
    lv_obj_get_coords(lv_obj_get_parent(Ui->List), &Panel);
    lv_obj_get_coords(Ui->List, &List);
    lv_obj_get_coords(Ui->BootButton, &Boot);
    assert(Panel.x1 >= 0 && Panel.y1 >= 0);
    assert(Panel.x2 < (int32_t)Width && Panel.y2 < (int32_t)Height);
    assert(Boot.x1 >= Panel.x1 && Boot.x2 <= Panel.x2);
    assert(Boot.y1 >= Panel.y1 && Boot.y2 <= Panel.y2);
    if (mMenuStyle.Layout == 1U && Width >= 720U && Width > Height) {
        assert(Boot.x1 > List.x2);
    } else {
        assert(Boot.y1 > List.y2);
    }
    lv_obj_t *A = lv_group_get_obj_by_index(Ui->Group, 0);
    lv_obj_t *B = lv_group_get_obj_by_index(Ui->Group, 1);
    assert(A != NULL);
    if (B == NULL) return;
    lv_obj_get_coords(A, &First);
    lv_obj_get_coords(B, &Second);
    if (mMenuStyle.Layout == 2U && lv_obj_get_width(lv_obj_get_parent(Ui->List)) >= 560 && Height >= 320U) {
        assert(First.y1 == Second.y1 && First.x2 < Second.x1);
        assert(lv_obj_get_child_count(A) == 2U);
    } else {
        assert(First.y2 < Second.y1);
    }
}

/* 验证菜单数目、长标签、触摸取消和重建后的风格，逐像素检查边界哨兵。 */
static void TestSize(unsigned Width, unsigned Height)
{
    size_t Bytes = (size_t)Width * Height * 4U;
    uint8_t *Memory = malloc(Bytes + 128U);
    assert(Memory != NULL);
    memset(Memory, 0xa5, Bytes + 128U);
    kshim_framebuffer_config_t Config = {
        .render_address = (uintptr_t)(Memory + 64U),
        .width = Width, .height = Height, .stride = Width * 4U,
        .bpp = 32U, .format = KSHIM_FB_FORMAT_ARGB8888,
        .buffer_size = Bytes, .foreground = UINT32_MAX,
    };
    kshim_framebuffer_t Fb;
    kshim_lvgl_t Ui;
    assert(kshim_fb_init(&Fb, &Config) == 0);
    assert(kshim_lvgl_init(&Ui, &Fb, NULL) == 0);
    char Text[KSHIM_MENU_MAX_ENTRIES][96];
    const char *Names[KSHIM_MENU_MAX_ENTRIES];
    for (unsigned I = 0; I < KSHIM_MENU_MAX_ENTRIES; I++) {
        (void)snprintf(Text[I], sizeof(Text[I]), "Image %02u - a deliberately long boot entry for clipping checks", I);
        Names[I] = Text[I];
    }
    const unsigned Counts[] = {1U, 2U, 3U, 49U, 2U};
    for (unsigned I = 0; I < sizeof(Counts) / sizeof(Counts[0]); I++) {
        unsigned Count = Counts[I];
        assert(kshim_lvgl_set_entries(&Ui, Names, Count, Count - 1U) == 0);
        Settle(&Ui);
        assert(lv_group_get_focused(Ui.Group) == lv_group_get_obj_by_index(Ui.Group, Count - 1U));
        CheckLayout(&Ui, Width, Height);
        lv_obj_t *Focused = lv_group_get_focused(Ui.Group);
        Tap(&Ui, Focused, 0);
        assert(kshim_lvgl_take_index(&Ui) == -1);
        Tap(&Ui, Ui.BootButton, 1);
        assert(kshim_lvgl_take_index(&Ui) == -1);
        Tap(&Ui, Ui.BootButton, 0);
        assert(kshim_lvgl_take_index(&Ui) == (int)Count - 1);
        assert(kshim_lvgl_take_index(&Ui) == -1);
        assert(kshim_lvgl_take_selection(&Ui) == KSHIM_MENU_NONE);
    }
    kshim_lvgl_deinit(&Ui);
    for (unsigned I = 0; I < 64U; I++) {
        assert(Memory[I] == 0xa5);
        assert(Memory[64U + Bytes + I] == 0xa5);
    }
    free(Memory);
}

typedef struct {
    kshim_lvgl_t *Ui;
    unsigned Calls;
} closing_probe_t;

/* 销毁回调中公共交互入口必须失效，不能访问尚在删除的对象或重新确认。 */
static void CheckClosing(lv_event_t *Event)
{
    closing_probe_t *Probe = lv_event_get_user_data(Event);
    assert(Probe != NULL && !kshim_lvgl_ready(Probe->Ui));
    assert(Probe->Ui->Ready == 0U);
    kshim_lvgl_set_status(Probe->Ui, "must not touch a closing label");
    kshim_lvgl_set_countdown(Probe->Ui, 1000U);
    kshim_lvgl_frame(Probe->Ui, 16U);
    kshim_touch_event_t Touch = {.type = KSHIM_TOUCH_EVENT_PRESS, .x = 1U, .y = 1U};
    kshim_lvgl_touch(Probe->Ui, &Touch);
    assert(kshim_lvgl_take_index(Probe->Ui) == -1);
    assert(kshim_lvgl_take_selection(Probe->Ui) == KSHIM_MENU_NONE);
    kshim_lvgl_deinit(Probe->Ui); /* 重入销毁应当直接返回。 */
    Probe->Calls++;
}

/* 每款新旧主题都检查活动焦点/动画下的销毁、重入和重新初始化。 */
static void TestClosing(void)
{
    const size_t Bytes = 320U * 240U * 4U;
    void *Pixels = calloc(1U, Bytes);
    assert(Pixels != NULL);
    kshim_framebuffer_config_t Config = {
        .render_address = (uintptr_t)Pixels, .width = 320U, .height = 240U,
        .stride = 1280U, .bpp = 32U, .format = KSHIM_FB_FORMAT_ARGB8888,
        .buffer_size = Bytes,
    };
    kshim_framebuffer_t Fb;
    assert(kshim_fb_init(&Fb, &Config) == 0);
    for (unsigned Pass = 0U; Pass < 8U; Pass++) {
        kshim_lvgl_t Ui;
        closing_probe_t Probe = {.Ui = &Ui};
        assert(kshim_lvgl_init(&Ui, &Fb, NULL) == 0);
        lv_obj_t *Panel = lv_obj_get_parent(Ui.List);
        lv_obj_t *Status = lv_obj_get_child(Panel, 1U);
        assert(Status != NULL);
        lv_obj_add_event_cb(Panel, CheckClosing, LV_EVENT_DELETE, &Probe);
        lv_obj_add_event_cb(Status, CheckClosing, LV_EVENT_DELETE, &Probe);
        lv_obj_t *Row = lv_group_get_obj_by_index(Ui.Group, Pass % Ui.EntryCount);
        lv_group_focus_obj(Row);
        lv_obj_send_event(Row, LV_EVENT_PRESSED, NULL);
        kshim_lvgl_deinit(&Ui);
        assert(Probe.Calls == 2U && !kshim_lvgl_ready(&Ui));
        kshim_lvgl_deinit(&Ui);
    }
    free(Pixels);
}

/* 覆盖会触发两种自适应布局的阈值两侧和常见横竖屏。 */
int main(void)
{
    TestClosing();
    if (mMenuStyle.Modern != 0U) {
        TestSize(320U, 240U);
        TestSize(559U, 360U);
        TestSize(640U, 360U);
        TestSize(720U, 480U);
        TestSize(1080U, 1920U);
        TestSize(1920U, 1080U);
    }
    printf("PASS: %s lifecycle, geometry, long labels, 1/2/3/49 entries and touch cancellation\n", mMenuStyle.Name);
    return 0;
}
