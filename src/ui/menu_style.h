#ifndef KSHIM_UI_MENU_STYLE_H
#define KSHIM_UI_MENU_STYLE_H

#include <stdint.h>

/* 编译期样式数据：无 LVGL 依赖；只编入选中的预设，旧配置默认 FLAT。
 * 生产代码应在 lvgl.h（会加载生成的 config.h）之后包含本文件。 */
typedef struct {
    const char *Name;
    uint32_t Screen;
    uint32_t Panel;
    uint32_t Text;
    uint32_t Muted;
    uint32_t Item;
    uint32_t Focus;
    uint32_t FocusText;
    uint32_t Scrollbar;
    uint32_t Boot;
    uint32_t BootPressed;
    uint32_t BootText;
    uint32_t Border;
    uint32_t FocusBorder;
    uint8_t PanelOpacity;
    uint8_t ItemOpacity;
    uint8_t FocusOpacity;
    uint8_t RowBorderWidth;
    uint8_t PanelOutlineWidth;
    uint8_t BorderAtRest;
    uint8_t LeftBorderOnly;
    uint8_t RadiusMin;
    uint8_t RadiusMax;
    uint8_t RowGapDivisor;
    uint8_t AnimatedBackdrop;
} kshim_menu_style_t;

/* 拒绝手工构造的多选配置，避免静默采用第一个分支。 */
#if (defined(CONFIG_KSHIM_MENU_STYLE_FLAT) && CONFIG_KSHIM_MENU_STYLE_FLAT) + \
    (defined(CONFIG_KSHIM_MENU_STYLE_CLASSIC) && CONFIG_KSHIM_MENU_STYLE_CLASSIC) + \
    (defined(CONFIG_KSHIM_MENU_STYLE_CARDS) && CONFIG_KSHIM_MENU_STYLE_CARDS) + \
    (defined(CONFIG_KSHIM_MENU_STYLE_TERMINAL) && CONFIG_KSHIM_MENU_STYLE_TERMINAL) + \
    (defined(CONFIG_KSHIM_MENU_STYLE_MINIMAL) && CONFIG_KSHIM_MENU_STYLE_MINIMAL) + \
    (defined(CONFIG_KSHIM_MENU_STYLE_HIGH_CONTRAST) && CONFIG_KSHIM_MENU_STYLE_HIGH_CONTRAST) > 1
#error "Select exactly one KSHIM_MENU_STYLE preset"
#endif

static const kshim_menu_style_t mMenuStyle = {
#if defined(CONFIG_KSHIM_MENU_STYLE_CLASSIC) && CONFIG_KSHIM_MENU_STYLE_CLASSIC
    .Name = "CLASSIC",
    .Screen = 0x101828U,
    .Panel = 0x202c44U,
    .Text = 0xffffffU,
    .Muted = 0xc7d1e0U,
    .Item = 0x182338U,
    .Focus = 0x1d4ed8U,
    .FocusText = 0xffffffU,
    .Scrollbar = 0xaabbd2U,
    .Boot = 0xc7d1e0U,
    .BootPressed = 0xaabbd2U,
    .BootText = 0x101828U,
    .Border = 0x8293aaU,
    .FocusBorder = 0xc7d1e0U,
    .PanelOpacity = 255U,
    .ItemOpacity = 255U,
    .FocusOpacity = 255U,
    .RowBorderWidth = 1U,
    .PanelOutlineWidth = 2U,
    .BorderAtRest = 1U,
    .LeftBorderOnly = 0U,
    .RadiusMin = 0U,
    .RadiusMax = 0U,
    .RowGapDivisor = 4U,
    .AnimatedBackdrop = 0U,
#elif defined(CONFIG_KSHIM_MENU_STYLE_CARDS) && CONFIG_KSHIM_MENU_STYLE_CARDS
    .Name = "CARDS",
    .Screen = 0xe8eef6U,
    .Panel = 0xf6f8fcU,
    .Text = 0x17243bU,
    .Muted = 0x46546bU,
    .Item = 0xffffffU,
    .Focus = 0xdae7ffU,
    .FocusText = 0x173d83U,
    .Scrollbar = 0x1d4ed8U,
    .Boot = 0x1d4ed8U,
    .BootPressed = 0x1e40afU,
    .BootText = 0xffffffU,
    .Border = 0xc4d0e3U,
    .FocusBorder = 0x1d4ed8U,
    .PanelOpacity = 255U,
    .ItemOpacity = 255U,
    .FocusOpacity = 255U,
    .RowBorderWidth = 1U,
    .PanelOutlineWidth = 0U,
    .BorderAtRest = 1U,
    .LeftBorderOnly = 0U,
    .RadiusMin = 12U,
    .RadiusMax = 24U,
    .RowGapDivisor = 2U,
    .AnimatedBackdrop = 0U,
#elif defined(CONFIG_KSHIM_MENU_STYLE_TERMINAL) && CONFIG_KSHIM_MENU_STYLE_TERMINAL
    .Name = "TERMINAL",
    .Screen = 0x000805U,
    .Panel = 0x020e08U,
    .Text = 0xa7ffc0U,
    .Muted = 0x87ce98U,
    .Item = 0x02150bU,
    .Focus = 0x8ff0a4U,
    .FocusText = 0x07120aU,
    .Scrollbar = 0x8ff0a4U,
    .Boot = 0x8ff0a4U,
    .BootPressed = 0x70c788U,
    .BootText = 0x07120aU,
    .Border = 0x389e58U,
    .FocusBorder = 0x8ff0a4U,
    .PanelOpacity = 255U,
    .ItemOpacity = 255U,
    .FocusOpacity = 255U,
    .RowBorderWidth = 1U,
    .PanelOutlineWidth = 1U,
    .BorderAtRest = 1U,
    .LeftBorderOnly = 0U,
    .RadiusMin = 0U,
    .RadiusMax = 0U,
    .RowGapDivisor = 8U,
    .AnimatedBackdrop = 0U,
#elif defined(CONFIG_KSHIM_MENU_STYLE_MINIMAL) && CONFIG_KSHIM_MENU_STYLE_MINIMAL
    .Name = "MINIMAL",
    .Screen = 0x080808U,
    .Panel = 0x080808U,
    .Text = 0xffffffU,
    .Muted = 0xbfbfbfU,
    .Item = 0x080808U,
    .Focus = 0x242424U,
    .FocusText = 0xffffffU,
    .Scrollbar = 0xbfbfbfU,
    .Boot = 0xffffffU,
    .BootPressed = 0xd6d6d6U,
    .BootText = 0x080808U,
    .Border = 0xbfbfbfU,
    .FocusBorder = 0xffffffU,
    .PanelOpacity = 255U,
    .ItemOpacity = 255U,
    .FocusOpacity = 255U,
    .RowBorderWidth = 3U,
    .PanelOutlineWidth = 0U,
    .BorderAtRest = 0U,
    .LeftBorderOnly = 1U,
    .RadiusMin = 0U,
    .RadiusMax = 0U,
    .RowGapDivisor = 6U,
    .AnimatedBackdrop = 0U,
#elif defined(CONFIG_KSHIM_MENU_STYLE_HIGH_CONTRAST) && CONFIG_KSHIM_MENU_STYLE_HIGH_CONTRAST
    .Name = "HIGH_CONTRAST",
    .Screen = 0x000000U,
    .Panel = 0x000000U,
    .Text = 0xffffffU,
    .Muted = 0xffffffU,
    .Item = 0x000000U,
    .Focus = 0xffffffU,
    .FocusText = 0x000000U,
    .Scrollbar = 0xffffffU,
    .Boot = 0xffffffU,
    .BootPressed = 0xccccccU,
    .BootText = 0x000000U,
    .Border = 0xffffffU,
    .FocusBorder = 0xffffffU,
    .PanelOpacity = 255U,
    .ItemOpacity = 255U,
    .FocusOpacity = 255U,
    .RowBorderWidth = 2U,
    .PanelOutlineWidth = 2U,
    .BorderAtRest = 1U,
    .LeftBorderOnly = 0U,
    .RadiusMin = 0U,
    .RadiusMax = 0U,
    .RowGapDivisor = 4U,
    .AnimatedBackdrop = 0U,
#else
    /* 原有 FLAT 色值、透明度、圆角和动态背景保持不变。 */
    .Name = "FLAT",
    .Screen = 0x13235aU,
    .Panel = 0x161c3eU,
    .Text = 0xffffffU,
    .Muted = 0xc9d1eeU,
    .Item = 0xffffffU,
    .Focus = 0x2f6bf0U,
    .FocusText = 0xffffffU,
    .Scrollbar = 0x7aa2ffU,
    .Boot = 0xf4f7ffU,
    .BootPressed = 0xdfe5fbU,
    .BootText = 0x161c3eU,
    .Border = 0xc9d1eeU,
    .FocusBorder = 0x7aa2ffU,
    .PanelOpacity = 128U,
    .ItemOpacity = 20U,
    .FocusOpacity = 214U,
    .RowBorderWidth = 0U,
    .PanelOutlineWidth = 0U,
    .BorderAtRest = 0U,
    .LeftBorderOnly = 0U,
    .RadiusMin = 8U,
    .RadiusMax = 16U,
    .RowGapDivisor = 4U,
    .AnimatedBackdrop = 1U,
#endif
};

/* 计算行和 Boot 按钮的圆角，小尺寸控件不超过半高。 */
static inline uint32_t kshim_menu_style_radius(uint32_t Height)
{
    uint32_t Radius = Height / 5U;

    if (Radius < mMenuStyle.RadiusMin)
        Radius = mMenuStyle.RadiusMin;
    if (Radius > mMenuStyle.RadiusMax)
        Radius = mMenuStyle.RadiusMax;
    return Radius < Height / 2U ? Radius : Height / 2U;
}

#endif /* KSHIM_UI_MENU_STYLE_H */
