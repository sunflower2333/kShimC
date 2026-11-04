/* SPDX-License-Identifier: MIT */
#pragma once
/*
 * HID report descriptors and their mapping to kShimC input
 * (CONFIG_KSHIM_USB_HID).
 * Owner: usbhid (kernel/drivers/hid/; the USB transport is
 * kernel/drivers/usb/class/hid.c). See docs/io-stack.md. Written from the
 * HID 1.11 specification and HID Usage Tables 1.4 (facts only).
 *
 * The parser flattens a report descriptor into items: one item per
 * variable element (so a field with Usage X, Usage Y, Report Count 2
 * becomes two items) and one item per array field. Usages are 32-bit
 * extended usages: page << 16 | id.
 */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "error_no.h"

#define KSHIM_HID_USAGE(page, id) (((uint32_t)(page) << 16) | (uint16_t)(id))
#define KSHIM_HID_USAGE_PAGE(usage) ((uint16_t)((usage) >> 16))
#define KSHIM_HID_USAGE_ID(usage)   ((uint16_t)(usage))

/* Usage pages and the usages kShimC maps. */
#define KSHIM_HID_PAGE_GENERIC_DESKTOP 0x01U
#define KSHIM_HID_PAGE_KEYBOARD        0x07U
#define KSHIM_HID_PAGE_BUTTON          0x09U
#define KSHIM_HID_PAGE_CONSUMER        0x0cU
#define KSHIM_HID_PAGE_DIGITIZER       0x0dU

#define KSHIM_HID_GD_POINTER  0x01U
#define KSHIM_HID_GD_MOUSE    0x02U
#define KSHIM_HID_GD_JOYSTICK 0x04U
#define KSHIM_HID_GD_GAMEPAD  0x05U
#define KSHIM_HID_GD_KEYBOARD 0x06U
#define KSHIM_HID_GD_X        0x30U
#define KSHIM_HID_GD_Y        0x31U
#define KSHIM_HID_GD_Z        0x32U
#define KSHIM_HID_GD_RX       0x33U
#define KSHIM_HID_GD_RY       0x34U
#define KSHIM_HID_GD_RZ       0x35U
#define KSHIM_HID_GD_WHEEL    0x38U
#define KSHIM_HID_GD_HAT      0x39U
#define KSHIM_HID_GD_DPAD_UP    0x90U
#define KSHIM_HID_GD_DPAD_DOWN  0x91U
#define KSHIM_HID_GD_DPAD_RIGHT 0x92U
#define KSHIM_HID_GD_DPAD_LEFT  0x93U

#define KSHIM_HID_KEY_ENTER     0x28U
#define KSHIM_HID_KEY_ESCAPE    0x29U
#define KSHIM_HID_KEY_BACKSPACE 0x2aU
#define KSHIM_HID_KEY_SPACE     0x2cU
#define KSHIM_HID_KEY_RIGHT     0x4fU
#define KSHIM_HID_KEY_LEFT      0x50U
#define KSHIM_HID_KEY_DOWN      0x51U
#define KSHIM_HID_KEY_UP        0x52U
#define KSHIM_HID_KEY_KP_ENTER  0x58U

#define KSHIM_HID_CONSUMER_VOLUME_UP   0xe9U
#define KSHIM_HID_CONSUMER_VOLUME_DOWN 0xeaU

#define KSHIM_HID_DIGITIZER_TIP_SWITCH 0x42U

enum kshim_hid_report_type {
    KSHIM_HID_REPORT_INPUT = 1,
    KSHIM_HID_REPORT_OUTPUT = 2,
    KSHIM_HID_REPORT_FEATURE = 3,
};

/* Main item data bits (HID 1.11 6.2.2.5). */
#define KSHIM_HID_MAIN_CONSTANT   0x001U
#define KSHIM_HID_MAIN_VARIABLE   0x002U
#define KSHIM_HID_MAIN_RELATIVE   0x004U
#define KSHIM_HID_MAIN_NULL_STATE 0x040U

#define KSHIM_HID_MAX_ITEMS   192U
#define KSHIM_HID_MAX_REPORTS 32U
#define KSHIM_HID_MAX_APPLICATIONS 8U
/* Explicit usage lists of array items ("Usage A, Usage C" instead of a
 * Usage Minimum/Maximum range) are kept in desc->usages. */
#define KSHIM_HID_MAX_ARRAY_USAGES 64U

struct kshim_hid_item {
    uint32_t usage;          /* variable: its usage; array: usage minimum */
    uint32_t usage_max;      /* variable: == usage; array: usage maximum */
    uint32_t application;    /* enclosing Application collection usage */
    uint32_t bit_offset;     /* from the start of the report data (after the ID byte) */
    uint8_t bit_size;        /* 1..32 */
    uint8_t count;           /* 1 for variables; array slots */
    uint8_t report_id;       /* 0 when the descriptor has no Report ID */
    uint8_t type;            /* enum kshim_hid_report_type */
    uint16_t flags;          /* KSHIM_HID_MAIN_* */
    int32_t logical_min, logical_max;  /* sign-extended per the item size */
    /* Arrays declared with an explicit usage list: desc->usages[usage_index ..
     * usage_index + usage_count - 1] in declaration order (usage/usage_max
     * then hold the first and last). 0 for ranges and variables. */
    uint8_t usage_index, usage_count;
};

struct kshim_hid_report {
    uint8_t id;
    uint8_t type;
    uint16_t bits;           /* data bits, excluding the ID byte */
};

struct kshim_hid_desc {
    struct kshim_hid_item items[KSHIM_HID_MAX_ITEMS];
    unsigned item_count;
    struct kshim_hid_report reports[KSHIM_HID_MAX_REPORTS];
    unsigned report_count;
    uint32_t applications[KSHIM_HID_MAX_APPLICATIONS];
    unsigned application_count;
    bool report_ids;         /* every report starts with its ID byte */
    uint32_t usages[KSHIM_HID_MAX_ARRAY_USAGES]; /* explicit array usage lists */
    unsigned usage_count;
};

/* Parse a report descriptor (short items; long items are skipped). Push/Pop
 * depth 4, collection depth 8; Usage Minimum/Maximum, extended (4-byte)
 * usages, Unit/Exponent ignored. KSHIM_IO_ERR_PROTOCOL for malformed input,
 * KSHIM_IO_ERR_OVERFLOW when a table limit is exceeded. Pure function.
 * Constant (padding) fields and data fields on vendor-defined usage pages
 * (0xff00-0xffff) only advance the bit offsets; they produce no items. A
 * Logical Maximum that reads negative while Logical Minimum is >= 0 (for
 * example "Logical Maximum (255)" encoded as the single byte 0xff) is taken
 * as unsigned. */
int kshim_hid_parse(const uint8_t *descriptor, size_t length, struct kshim_hid_desc *desc);

/* First item of `type` whose usage range contains `usage` in report
 * `report_id` (or any report when report_id < 0); NULL when absent. */
const struct kshim_hid_item *kshim_hid_find(const struct kshim_hid_desc *desc, uint8_t type,
                                            uint32_t usage, int report_id);
/* Extract element `index` of `item` from report data (ID byte removed).
 * Values are sign-extended when logical_min < 0. false when out of range. */
bool kshim_hid_get_value(const struct kshim_hid_item *item, const uint8_t *data, size_t length,
                         unsigned index, int32_t *value);
bool kshim_hid_has_application(const struct kshim_hid_desc *desc, uint32_t usage);
/* Usage selected by an array element's value (HID 1.11 6.2.2.5: Usage
 * Minimum + value - Logical Minimum, or the value's entry of an explicit
 * usage list). false for a null value (outside the logical range or the
 * usage range), which means "no control active" in that slot. */
bool kshim_hid_array_usage(const struct kshim_hid_desc *desc, const struct kshim_hid_item *item,
                           int32_t value, uint32_t *usage);

/* ---- Mapping to kShimC input (kshim/input_inject.h) --------------------- */

/* What a HID interface feeds: keys (UP/DOWN/SELECT/BACK) and/or a pointer. */
#define KSHIM_HID_INPUT_KEYS    0x1U
#define KSHIM_HID_INPUT_POINTER 0x2U

#define KSHIM_HID_INPUT_MAX_KEYS 16U

/* Per-interface mapping state: previous report contents for edge detection,
 * axis hysteresis and the pointer. Zero-initialise, then kshim_hid_input_init. */
struct kshim_hid_input {
    const struct kshim_hid_desc *desc;
    unsigned caps;               /* KSHIM_HID_INPUT_* found in the descriptor */
    bool boot_keyboard, boot_mouse; /* fixed boot-protocol layouts instead of desc */
    /* Linux key codes currently reported as pressed (edge detection; one
     * code per physical control, so two controls with the same code, such
     * as a hat and a stick both pointing up, give one PRESS). */
    uint32_t held[KSHIM_HID_INPUT_MAX_KEYS];
    unsigned held_count;
    /* Addition (usbhid): report ID that pressed held[i]; a report only
     * releases the codes of its own ID. */
    uint8_t held_report[KSHIM_HID_INPUT_MAX_KEYS];
    int8_t axis_state[2];        /* -1/0/+1 per stick axis after hysteresis */
    bool pointer_pressed;
    /* Additions (usbhid): last pointer report, for change detection and the
     * release on reset; gamepad direction auto-repeat. */
    bool pointer_valid, pointer_absolute;
    int32_t pointer_x, pointer_y;
    uint32_t pointer_range_x, pointer_range_y;
    uint32_t repeat_code;        /* held gamepad direction code, 0 = none */
    uint64_t repeat_at_ms;       /* next repeat (get_current_time() base) */
};

/* Gamepad direction auto-repeat (hat, D-pad usages, stick): a direction held
 * for KSHIM_HID_REPEAT_DELAY_MS repeats every KSHIM_HID_REPEAT_INTERVAL_MS
 * as a PRESS event with pressed = 2 (the Linux autorepeat value), until it
 * is released. Keyboards, buttons and wheels never repeat. */
#define KSHIM_HID_REPEAT_DELAY_MS    500U
#define KSHIM_HID_REPEAT_INTERVAL_MS 150U

/* Classify the descriptor (keyboard 0x00010006, mouse 0x00010002,
 * gamepad 0x00010005, joystick 0x00010004, consumer control 0x000c0001,
 * digitizer/tablet) and select the items the mapping uses. Returns
 * ERROR_NOT_SUPPORTED when nothing maps. */
int kshim_hid_input_init(struct kshim_hid_input *input, const struct kshim_hid_desc *desc);
/* Boot protocol (HID 1.11 appendix B): keyboard 8-byte, mouse 3+ byte. */
void kshim_hid_input_init_boot(struct kshim_hid_input *input, bool keyboard);
/* Feed one input report as received (with the ID byte when desc->report_ids).
 * Emits key/pointer events through kshim_input_inject_*(); thread context. */
int kshim_hid_input_report(struct kshim_hid_input *input, const uint8_t *report, size_t length);
/* Release everything held (device gone / handoff): RELEASE for each held
 * key, pointer release. */
void kshim_hid_input_reset(struct kshim_hid_input *input);
/* Periodic work from the transport's service hook (thread context): the
 * gamepad direction auto-repeat. Devices report only on change after
 * SET_IDLE(0), so the repeat cannot come from reports. */
void kshim_hid_input_service(struct kshim_hid_input *input, uint64_t now_ms);
/* Key action of a Linux code produced by the mapping (KEY_UP -> UP, ...). */
int kshim_hid_input_action(uint32_t code);

/* Mapping (docs/io-stack.md "HID mapping"):
 *  keyboard  Up -> UP, Down -> DOWN, Enter/Keypad Enter/Space -> SELECT,
 *            Escape/Backspace -> BACK; code = Linux KEY_*.
 *  gamepad / joystick
 *            Hat switch N/NE/NW -> UP, S/SE/SW -> DOWN; D-pad usages
 *            0x90/0x91 -> UP/DOWN; Y axis past 50 % of its half range ->
 *            UP/DOWN, released below 25 % (hysteresis: the 25 % band is
 *            the dead zone); Button 1 -> SELECT, Button 2 -> BACK. Codes:
 *            BTN_DPAD_UP/BTN_DPAD_DOWN for every direction source,
 *            BTN_SOUTH/BTN_EAST for the buttons. Directions auto-repeat
 *            (kshim_hid_input_service). The X axis is tracked but not mapped.
 *  consumer  Volume Increment -> UP, Volume Decrement -> DOWN.
 *  mouse     relative X/Y -> relative pointer, Button 1 -> pointer press,
 *            Wheel +/- -> UP/DOWN key press+release (KEY_SCROLLUP/DOWN, at
 *            most 3 per report).
 *  tablet    absolute X/Y (logical range) -> absolute pointer, Button 1 or
 *            Tip Switch -> pointer press.
 * Key events are edges: one PRESS when the mapped control becomes active,
 * one RELEASE when it stops; only gamepad directions auto-repeat. A
 * keyboard report whose array slots hold ErrorRollOver changes nothing. */
