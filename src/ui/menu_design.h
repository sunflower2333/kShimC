#ifndef KSHIM_MENU_DESIGN_H
#define KSHIM_MENU_DESIGN_H

#include <stdint.h>

/*
 * Theme presentation contract.
 *
 * This layer describes visual intent only. Boot state, input handling and
 * confirmation remain owned by lvgl_port.c.
 */
typedef enum {
    KSHIM_MENU_LAYOUT_LIST = 0,
    KSHIM_MENU_LAYOUT_SPLIT = 1,
    KSHIM_MENU_LAYOUT_GRID = 2,
    KSHIM_MENU_LAYOUT_GROUPED = 3,
    KSHIM_MENU_LAYOUT_RIBBON = 4,
    KSHIM_MENU_LAYOUT_IOS_LIST = 5,
} kshim_menu_layout_t;

typedef struct {
    kshim_menu_layout_t Layout;
    uint8_t Compact;
    uint8_t UseSelectionSymbol;
    uint8_t UseIconTiles;
    uint8_t UseMonoText;
    uint8_t Reserved;
    uint16_t PanelRadius;
    uint16_t RowRadius;
    uint16_t RowGap;
    uint16_t IconSize;
} kshim_menu_design_t;

/* FLAT intentionally keeps the legacy renderer path. */
static const kshim_menu_design_t kshim_menu_design_flat = {
    .Layout = KSHIM_MENU_LAYOUT_LIST,
};

#endif
