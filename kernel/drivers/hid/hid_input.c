/* SPDX-License-Identifier: MIT */
/*
 * HID reports to kShimC menu input (kshim/hid.h, docs/io-stack.md 3.8).
 *
 * Every report is reduced to a set of active Linux key codes plus a pointer
 * sample. Key events are the differences against the previous set (RELEASE
 * first, then PRESS), so a report that repeats an unchanged state, a second
 * control with the same code, or a lost report never produces a stray edge.
 *
 * Sources of the facts used here: HID Usage Tables 1.4 (page and usage IDs,
 * hat switch orientation: 0 = north, clockwise, out-of-range = null), the
 * HID 1.11 boot report layouts (appendix B.1 keyboard, B.2 mouse), and the
 * Linux input ABI numbers (include/uapi/linux/input-event-codes.h values,
 * which are an interface, not code).
 */
#include <kshim/hid.h>
#include <kshim/input_inject.h>
#include <kshim/io.h>
#include <kshim/time.h>
#include <string.h>

/* Linux input codes (input-event-codes.h ABI values). */
#define KEY_ESC        1U
#define KEY_BACKSPACE  14U
#define KEY_ENTER      28U
#define KEY_SPACE      57U
#define KEY_KPENTER    96U
#define KEY_UP         103U
#define KEY_DOWN       108U
#define KEY_VOLUMEDOWN 114U
#define KEY_VOLUMEUP   115U
#define KEY_SCROLLUP   177U
#define KEY_SCROLLDOWN 178U
#define BTN_SOUTH      0x130U
#define BTN_EAST       0x131U
#define BTN_DPAD_UP    0x220U
#define BTN_DPAD_DOWN  0x221U

#define PAGE_GD       KSHIM_HID_PAGE_GENERIC_DESKTOP
#define PAGE_KEYBOARD KSHIM_HID_PAGE_KEYBOARD
#define PAGE_BUTTON   KSHIM_HID_PAGE_BUTTON
#define PAGE_CONSUMER KSHIM_HID_PAGE_CONSUMER
#define PAGE_DIGITIZER KSHIM_HID_PAGE_DIGITIZER

#define GD_MULTI_AXIS 0x08U
#define GD_KEYPAD     0x07U
#define KEYBOARD_ERROR_ROLLOVER 0x01U
#define WHEEL_MAX_STEPS 3

int kshim_hid_input_action(uint32_t code)
{
    switch (code) {
    case KEY_UP: case KEY_VOLUMEUP: case KEY_SCROLLUP: case BTN_DPAD_UP:
        return KSHIM_KEY_ACTION_UP;
    case KEY_DOWN: case KEY_VOLUMEDOWN: case KEY_SCROLLDOWN: case BTN_DPAD_DOWN:
        return KSHIM_KEY_ACTION_DOWN;
    case KEY_ENTER: case KEY_KPENTER: case KEY_SPACE: case BTN_SOUTH:
        return KSHIM_KEY_ACTION_SELECT;
    case KEY_ESC: case KEY_BACKSPACE: case BTN_EAST:
        return KSHIM_KEY_ACTION_BACK;
    default:
        return KSHIM_KEY_ACTION_NONE;
    }
}

enum app_kind { APP_OTHER, APP_POINTER, APP_GAMEPAD, APP_DIGITIZER };

static enum app_kind app_kind(uint32_t application)
{
    uint16_t page = KSHIM_HID_USAGE_PAGE(application), id = KSHIM_HID_USAGE_ID(application);
    if (page == PAGE_GD) {
        if (id == KSHIM_HID_GD_MOUSE || id == KSHIM_HID_GD_POINTER) return APP_POINTER;
        if (id == KSHIM_HID_GD_JOYSTICK || id == KSHIM_HID_GD_GAMEPAD || id == GD_MULTI_AXIS)
            return APP_GAMEPAD;
    }
    /* Digitizer, Pen, Touch Screen, Touch Pad, ... (Usage Tables 16). */
    if (page == PAGE_DIGITIZER) return APP_DIGITIZER;
    return APP_OTHER;
}

/* Key code of a usage that is active (pressed / non-zero), by application. */
static uint32_t key_code(uint32_t usage, enum app_kind app)
{
    uint16_t page = KSHIM_HID_USAGE_PAGE(usage), id = KSHIM_HID_USAGE_ID(usage);
    switch (page) {
    case PAGE_KEYBOARD:
        switch (id) {
        case KSHIM_HID_KEY_UP: return KEY_UP;
        case KSHIM_HID_KEY_DOWN: return KEY_DOWN;
        case KSHIM_HID_KEY_ENTER: return KEY_ENTER;
        case KSHIM_HID_KEY_KP_ENTER: return KEY_KPENTER;
        case KSHIM_HID_KEY_SPACE: return KEY_SPACE;
        case KSHIM_HID_KEY_ESCAPE: return KEY_ESC;
        case KSHIM_HID_KEY_BACKSPACE: return KEY_BACKSPACE;
        default: return 0;
        }
    case PAGE_CONSUMER:
        if (id == KSHIM_HID_CONSUMER_VOLUME_UP) return KEY_VOLUMEUP;
        if (id == KSHIM_HID_CONSUMER_VOLUME_DOWN) return KEY_VOLUMEDOWN;
        return 0;
    case PAGE_BUTTON:
        if (app != APP_GAMEPAD) return 0;
        if (id == 1) return BTN_SOUTH;
        if (id == 2) return BTN_EAST;
        return 0;
    case PAGE_GD:
        if (app != APP_GAMEPAD) return 0;
        if (id == KSHIM_HID_GD_DPAD_UP) return BTN_DPAD_UP;
        if (id == KSHIM_HID_GD_DPAD_DOWN) return BTN_DPAD_DOWN;
        return 0;
    default:
        return 0;
    }
}

/* What one item contributes, decided from its usage and application. */
enum item_role {
    ROLE_NONE,
    ROLE_KEYS,       /* key usages (variable bit or array) */
    ROLE_HAT,
    ROLE_STICK_X,
    ROLE_STICK_Y,
    ROLE_POINTER_X,
    ROLE_POINTER_Y,
    ROLE_POINTER_BUTTON,
    ROLE_WHEEL,
};

static enum item_role item_role(const struct kshim_hid_desc *desc, const struct kshim_hid_item *item)
{
    enum app_kind app = app_kind(item->application);
    if (!(item->flags & KSHIM_HID_MAIN_VARIABLE)) {
        /* Arrays: any key usage in the list or range maps. */
        if (item->usage_count) {
            for (unsigned i = 0; i < item->usage_count; ++i)
                if (key_code(desc->usages[item->usage_index + i], app)) return ROLE_KEYS;
            return ROLE_NONE;
        }
        uint16_t page = KSHIM_HID_USAGE_PAGE(item->usage);
        if (page == PAGE_KEYBOARD || page == PAGE_CONSUMER) {
            /* Ranges such as 0x00..0xff or 0x000..0x3ff cover the keys. */
            static const uint16_t keyboard[] = {KSHIM_HID_KEY_UP, KSHIM_HID_KEY_DOWN, KSHIM_HID_KEY_ENTER,
                                                KSHIM_HID_KEY_ESCAPE};
            static const uint16_t consumer[] = {KSHIM_HID_CONSUMER_VOLUME_UP, KSHIM_HID_CONSUMER_VOLUME_DOWN};
            const uint16_t *ids = page == PAGE_KEYBOARD ? keyboard : consumer;
            unsigned n = page == PAGE_KEYBOARD ? 4U : 2U;
            for (unsigned i = 0; i < n; ++i) {
                uint32_t usage = KSHIM_HID_USAGE(page, ids[i]);
                if (usage >= item->usage && usage <= item->usage_max) return ROLE_KEYS;
            }
            return ROLE_NONE;
        }
        if (page == PAGE_BUTTON && app == APP_GAMEPAD &&
            item->usage <= KSHIM_HID_USAGE(PAGE_BUTTON, 2) &&
            item->usage_max >= KSHIM_HID_USAGE(PAGE_BUTTON, 1))
            return ROLE_KEYS;
        return ROLE_NONE;
    }
    uint32_t usage = item->usage;
    uint16_t page = KSHIM_HID_USAGE_PAGE(usage), id = KSHIM_HID_USAGE_ID(usage);
    bool relative = (item->flags & KSHIM_HID_MAIN_RELATIVE) != 0;
    if (page == PAGE_GD) {
        if (id == KSHIM_HID_GD_X || id == KSHIM_HID_GD_Y) {
            bool x = id == KSHIM_HID_GD_X;
            if (app == APP_POINTER || app == APP_DIGITIZER) return x ? ROLE_POINTER_X : ROLE_POINTER_Y;
            if (app == APP_GAMEPAD && !relative) return x ? ROLE_STICK_X : ROLE_STICK_Y;
            return ROLE_NONE;
        }
        if (id == KSHIM_HID_GD_WHEEL && relative) return ROLE_WHEEL;
        if (id == KSHIM_HID_GD_HAT && app == APP_GAMEPAD) return ROLE_HAT;
    }
    if (page == PAGE_BUTTON && id == 1 && (app == APP_POINTER || app == APP_DIGITIZER))
        return ROLE_POINTER_BUTTON;
    if (page == PAGE_DIGITIZER && id == KSHIM_HID_DIGITIZER_TIP_SWITCH) return ROLE_POINTER_BUTTON;
    if (key_code(usage, app)) return ROLE_KEYS;
    return ROLE_NONE;
}

int kshim_hid_input_init(struct kshim_hid_input *input, const struct kshim_hid_desc *desc)
{
    if (!input || !desc) return ERROR_INVALID_PARAMETER;
    memset(input, 0, sizeof(*input));
    input->desc = desc;
    bool pointer_x = false, pointer_y = false;
    for (unsigned i = 0; i < desc->item_count; ++i) {
        const struct kshim_hid_item *item = &desc->items[i];
        if (item->type != KSHIM_HID_REPORT_INPUT) continue;
        switch (item_role(desc, item)) {
        case ROLE_KEYS: case ROLE_HAT: case ROLE_STICK_Y: case ROLE_WHEEL:
            input->caps |= KSHIM_HID_INPUT_KEYS;
            break;
        case ROLE_POINTER_X: pointer_x = true; break;
        case ROLE_POINTER_Y: pointer_y = true; break;
        default: break;
        }
    }
    if (pointer_x && pointer_y) input->caps |= KSHIM_HID_INPUT_POINTER;
    if (!input->caps) {
        input->desc = NULL;
        return ERROR_NOT_SUPPORTED;
    }
    return 0;
}

void kshim_hid_input_init_boot(struct kshim_hid_input *input, bool keyboard)
{
    if (!input) return;
    memset(input, 0, sizeof(*input));
    input->boot_keyboard = keyboard;
    input->boot_mouse = !keyboard;
    input->caps = keyboard ? KSHIM_HID_INPUT_KEYS : KSHIM_HID_INPUT_POINTER;
}

/* ---- Report evaluation -------------------------------------------------- */

struct sample {
    uint32_t codes[KSHIM_HID_INPUT_MAX_KEYS];
    unsigned code_count;
    bool rollover;
    bool keys_seen;          /* the report carries key controls at all */
    bool have_x, have_y, relative, pressed, have_button;
    int32_t x, y;
    uint32_t range_x, range_y;
    int32_t wheel;
};

static void sample_add(struct sample *s, uint32_t code)
{
    if (!code) return;
    for (unsigned i = 0; i < s->code_count; ++i)
        if (s->codes[i] == code) return;
    if (s->code_count < KSHIM_HID_INPUT_MAX_KEYS) s->codes[s->code_count++] = code;
}

/* Hat switch (Usage Tables 4.3): positions clockwise from north; a value
 * outside the logical range is the null (centred) state. 8-position hats
 * have N, NE, E, SE, S, SW, W, NW; 4-position hats N, E, S, W. */
static void hat_sample(struct sample *s, const struct kshim_hid_item *item, int32_t value)
{
    if (value < item->logical_min || value > item->logical_max) return;
    int32_t positions = item->logical_max - item->logical_min + 1;
    int32_t at = value - item->logical_min;
    if (positions == 8) {
        if (at == 0 || at == 1 || at == 7) sample_add(s, BTN_DPAD_UP);
        else if (at >= 3 && at <= 5) sample_add(s, BTN_DPAD_DOWN);
    } else if (positions == 4) {
        if (at == 0) sample_add(s, BTN_DPAD_UP);
        else if (at == 2) sample_add(s, BTN_DPAD_DOWN);
    }
}

/* Stick hysteresis: active past 50 % of the half range, released inside
 * 25 % (the dead zone). state: -1 up/left, +1 down/right. */
static int8_t axis_update(int8_t state, const struct kshim_hid_item *item, int32_t value)
{
    int64_t min = item->logical_min, max = item->logical_max;
    if (max <= min) return 0;
    /* Twice the offset from the centre, against the full range, avoids
     * rounding a half-unit centre. */
    int64_t twice = 2 * (int64_t)value - (min + max);
    int64_t full = max - min;
    /* Held while at least 25 % out; a release can flip straight to the
     * other side in the same report. */
    if (state < 0 && 4 * twice <= -full) return -1;
    if (state > 0 && 4 * twice >= full) return 1;
    if (2 * twice <= -full) return -1;   /* offset <= -50 % of half range */
    if (2 * twice >= full) return 1;
    return 0;
}

static void evaluate_desc(struct kshim_hid_input *input, uint8_t report_id, const uint8_t *data,
                          size_t length, struct sample *s)
{
    const struct kshim_hid_desc *desc = input->desc;
    for (unsigned i = 0; i < desc->item_count; ++i) {
        const struct kshim_hid_item *item = &desc->items[i];
        if (item->type != KSHIM_HID_REPORT_INPUT || item->report_id != report_id) continue;
        enum item_role role = item_role(desc, item);
        if (role == ROLE_NONE) continue;
        enum app_kind app = app_kind(item->application);
        int32_t value;
        if (!(item->flags & KSHIM_HID_MAIN_VARIABLE)) {
            s->keys_seen = true;
            for (unsigned slot = 0; slot < item->count; ++slot) {
                uint32_t usage;
                if (!kshim_hid_get_value(item, data, length, slot, &value)) break;
                if (!kshim_hid_array_usage(desc, item, value, &usage)) continue;
                if (usage == KSHIM_HID_USAGE(PAGE_KEYBOARD, KEYBOARD_ERROR_ROLLOVER)) s->rollover = true;
                sample_add(s, key_code(usage, app));
            }
            continue;
        }
        if (!kshim_hid_get_value(item, data, length, 0, &value)) continue;
        switch (role) {
        case ROLE_KEYS:
            s->keys_seen = true;
            /* A null value (outside the logical range) is "not pressed". */
            if (value != 0 && value >= item->logical_min && value <= item->logical_max)
                sample_add(s, key_code(item->usage, app));
            break;
        case ROLE_HAT:
            s->keys_seen = true;
            hat_sample(s, item, value);
            break;
        case ROLE_STICK_X:
            input->axis_state[0] = axis_update(input->axis_state[0], item, value);
            break;
        case ROLE_STICK_Y:
            s->keys_seen = true;
            input->axis_state[1] = axis_update(input->axis_state[1], item, value);
            if (input->axis_state[1] < 0) sample_add(s, BTN_DPAD_UP);
            if (input->axis_state[1] > 0) sample_add(s, BTN_DPAD_DOWN);
            break;
        case ROLE_POINTER_X:
        case ROLE_POINTER_Y: {
            bool x = role == ROLE_POINTER_X;
            bool relative = (item->flags & KSHIM_HID_MAIN_RELATIVE) != 0;
            int32_t v = value;
            uint32_t range = 0;
            if (!relative) {
                if (item->logical_max <= item->logical_min) break;
                v = value - item->logical_min;
                range = (uint32_t)(item->logical_max - item->logical_min);
                if (v < 0) v = 0;
                if ((uint32_t)v > range) v = (int32_t)range;
            }
            s->relative = relative;
            if (x) { s->have_x = true; s->x = v; s->range_x = range; }
            else { s->have_y = true; s->y = v; s->range_y = range; }
            break;
        }
        case ROLE_POINTER_BUTTON:
            s->have_button = true;
            if (value != 0) s->pressed = true;
            break;
        case ROLE_WHEEL:
            s->keys_seen = true;
            s->wheel += value;
            break;
        default:
            break;
        }
    }
}

/* HID 1.11 B.1: modifiers, reserved, six key codes. */
static void evaluate_boot_keyboard(const uint8_t *data, size_t length, struct sample *s)
{
    s->keys_seen = true;
    for (size_t i = 2; i < 8 && i < length; ++i) {
        if (data[i] == KEYBOARD_ERROR_ROLLOVER) s->rollover = true;
        sample_add(s, key_code(KSHIM_HID_USAGE(PAGE_KEYBOARD, data[i]), APP_OTHER));
    }
}

/* HID 1.11 B.2: buttons (bit 0 = button 1), X and Y displacement. */
static void evaluate_boot_mouse(const uint8_t *data, size_t length, struct sample *s)
{
    if (length < 3) return;
    s->have_button = true;
    s->pressed = (data[0] & 1U) != 0;
    s->have_x = s->have_y = true;
    s->relative = true;
    s->x = (int8_t)data[1];
    s->y = (int8_t)data[2];
}

static void emit_key(uint32_t code, kshim_key_event_type_t type, uint8_t pressed)
{
    kshim_key_event_t event = {
        .type = type,
        .action = (kshim_key_action_t)kshim_hid_input_action(code),
        .code = code,
        .pressed = pressed,
    };
    (void)kshim_input_inject_key(&event);
}

static bool is_direction(uint32_t code)
{
    return code == BTN_DPAD_UP || code == BTN_DPAD_DOWN;
}

static void apply_keys(struct kshim_hid_input *input, uint8_t report_id, const struct sample *s,
                       uint64_t now_ms)
{
    /* Releases first, so a hand-over between two codes of the same action
     * reads as release + press. A report speaks only for its own ID. */
    for (unsigned i = 0; i < input->held_count;) {
        uint32_t code = input->held[i];
        bool still = input->held_report[i] != report_id;
        for (unsigned j = 0; j < s->code_count; ++j) still |= s->codes[j] == code;
        if (still) { ++i; continue; }
        emit_key(code, KSHIM_KEY_EVENT_RELEASE, 0);
        if (input->repeat_code == code) input->repeat_code = 0;
        --input->held_count;
        input->held[i] = input->held[input->held_count];
        input->held_report[i] = input->held_report[input->held_count];
    }
    for (unsigned j = 0; j < s->code_count; ++j) {
        uint32_t code = s->codes[j];
        bool known = false;
        for (unsigned i = 0; i < input->held_count; ++i) known |= input->held[i] == code;
        if (known || input->held_count == KSHIM_HID_INPUT_MAX_KEYS) continue;
        input->held_report[input->held_count] = report_id;
        input->held[input->held_count++] = code;
        emit_key(code, KSHIM_KEY_EVENT_PRESS, 1);
        if (is_direction(code)) {
            input->repeat_code = code;
            input->repeat_at_ms = now_ms + KSHIM_HID_REPEAT_DELAY_MS;
        }
    }
}

static void apply_wheel(int32_t wheel)
{
    if (!wheel) return;
    uint32_t code = wheel > 0 ? KEY_SCROLLUP : KEY_SCROLLDOWN;
    int32_t steps = wheel > 0 ? wheel : -wheel;
    if (steps > WHEEL_MAX_STEPS) steps = WHEEL_MAX_STEPS;
    while (steps--) {
        emit_key(code, KSHIM_KEY_EVENT_PRESS, 1);
        emit_key(code, KSHIM_KEY_EVENT_RELEASE, 0);
    }
}

static void apply_pointer(struct kshim_hid_input *input, const struct sample *s)
{
    if (!(input->caps & KSHIM_HID_INPUT_POINTER) || !s->have_x || !s->have_y) return;
    bool pressed = s->have_button ? s->pressed : input->pointer_pressed;
    struct kshim_input_pointer_event event = {.pressed = pressed};
    if (s->relative) {
        if (!s->x && !s->y && pressed == input->pointer_pressed) return;
        event.mode = KSHIM_INPUT_POINTER_RELATIVE;
        event.x = s->x;
        event.y = s->y;
    } else {
        if (!s->range_x || !s->range_y) return;
        if (input->pointer_valid && input->pointer_absolute && s->x == input->pointer_x &&
            s->y == input->pointer_y && pressed == input->pointer_pressed)
            return;
        event.mode = KSHIM_INPUT_POINTER_ABSOLUTE;
        event.x = s->x;
        event.y = s->y;
        event.range_x = s->range_x;
        event.range_y = s->range_y;
    }
    (void)kshim_input_inject_pointer(&event);
    input->pointer_valid = true;
    input->pointer_absolute = !s->relative;
    input->pointer_x = event.x;
    input->pointer_y = event.y;
    input->pointer_range_x = event.range_x;
    input->pointer_range_y = event.range_y;
    input->pointer_pressed = pressed;
}

int kshim_hid_input_report(struct kshim_hid_input *input, const uint8_t *report, size_t length)
{
    if (!input || !report) return ERROR_INVALID_PARAMETER;
    if (!input->desc && !input->boot_keyboard && !input->boot_mouse) return ERROR_NOT_READY;
    struct sample s;
    uint8_t id = 0;
    memset(&s, 0, sizeof(s));
    if (input->boot_keyboard) {
        evaluate_boot_keyboard(report, length, &s);
    } else if (input->boot_mouse) {
        evaluate_boot_mouse(report, length, &s);
    } else {
        if (input->desc->report_ids) {
            if (!length) return KSHIM_IO_ERR_PROTOCOL;
            id = report[0];
            ++report;
            --length;
        }
        evaluate_desc(input, id, report, length, &s);
    }
    /* A keyboard in phantom state (every slot ErrorRollOver) says nothing
     * about which keys are down: keep the previous state. */
    if (s.keys_seen && !s.rollover) apply_keys(input, id, &s, get_current_time());
    apply_wheel(s.wheel);
    apply_pointer(input, &s);
    return 0;
}

void kshim_hid_input_service(struct kshim_hid_input *input, uint64_t now_ms)
{
    if (!input || !input->repeat_code) return;
    bool held = false;
    for (unsigned i = 0; i < input->held_count; ++i) held |= input->held[i] == input->repeat_code;
    if (!held) { input->repeat_code = 0; return; }
    if (now_ms < input->repeat_at_ms) return;
    emit_key(input->repeat_code, KSHIM_KEY_EVENT_PRESS, 2);
    input->repeat_at_ms = now_ms + KSHIM_HID_REPEAT_INTERVAL_MS;
}

void kshim_hid_input_reset(struct kshim_hid_input *input)
{
    if (!input) return;
    while (input->held_count) emit_key(input->held[--input->held_count], KSHIM_KEY_EVENT_RELEASE, 0);
    input->repeat_code = 0;
    input->axis_state[0] = input->axis_state[1] = 0;
    if (input->pointer_pressed) {
        struct kshim_input_pointer_event event = {.pressed = false};
        if (input->pointer_absolute && input->pointer_range_x && input->pointer_range_y) {
            event.mode = KSHIM_INPUT_POINTER_ABSOLUTE;
            event.x = input->pointer_x;
            event.y = input->pointer_y;
            event.range_x = input->pointer_range_x;
            event.range_y = input->pointer_range_y;
        } else {
            event.mode = KSHIM_INPUT_POINTER_RELATIVE;
        }
        (void)kshim_input_inject_pointer(&event);
        input->pointer_pressed = false;
    }
    input->pointer_valid = false;
}
