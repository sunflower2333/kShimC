/* SPDX-License-Identifier: MIT */
/*
 * HID report descriptor parser, report-to-menu mapping, the XInput adapter,
 * the injection queue and the kshim_key_source_poll() hook.
 *
 * Descriptor fixtures:
 *  - QEMU 8.2 usb-kbd, usb-mouse and usb-tablet report descriptors: the
 *    bytes these emulated devices return for GET_DESCRIPTOR(Report) (device
 *    data, as served by hw/usb/dev-hid.c; checked against QEMU 10.1 too).
 *  - An Xbox-style HID gamepad (report ID, 16-bit sticks, 10-bit triggers,
 *    1..8 hat with null state, 15 buttons, consumer AC Back), written for
 *    this test in the layout Bluetooth Xbox controllers use.
 *  - A generic DirectInput-style gamepad (8-bit axes, 0..7 hat, 12 buttons,
 *    vendor-page bits), written for this test.
 *  - Keyboard + consumer control with report IDs, and a sparse consumer
 *    array, written for this test.
 */
#include <kshim/hid.h>
#include <kshim/input_inject.h>
#include <kshim/io.h>
#include <kshim/usb_class.h>
#include <kshim/usb.h>
#include <kshim/input.h>
#include <qcom_keys.h>

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- Environment stubs -------------------------------------------------- */

static uint64_t now_ms;
static bool boot_cpu = true;
uint64_t get_current_time(void) { return now_ms; }
bool kshim_io_on_boot_cpu(void) { return boot_cpu; }
unsigned long kshim_irq_save(void) { return 0; }
void kshim_irq_restore(unsigned long flags) { (void)flags; }
bool kshim_irq_ready(void) { return false; }
/* The USB transport in hid.c is linked for its XInput adapter only; the
 * USB core is exercised by usbhid_stack_test. */
const char *kshim_io_strerror(int status) { (void)status; return "?"; }
int kshim_usb_submit_urb(struct kshim_usb_urb *urb) { (void)urb; abort(); }
int kshim_usb_cancel_urb(struct kshim_usb_urb *urb) { (void)urb; abort(); }
int kshim_usb_clear_halt(struct kshim_usb_device *d, struct kshim_usb_endpoint *e) { (void)d; (void)e; abort(); }
int kshim_usb_control(struct kshim_usb_device *d, uint8_t t, uint8_t r, uint16_t v, uint16_t i, void *data,
                      uint16_t l, uint32_t ms, uint32_t *a)
{ (void)d; (void)t; (void)r; (void)v; (void)i; (void)data; (void)l; (void)ms; (void)a; abort(); }
const uint8_t *kshim_usb_desc_find_in_interface(const uint8_t *c, size_t l, const uint8_t *f, uint8_t t)
{ (void)c; (void)l; (void)f; (void)t; abort(); }
struct kshim_usb_endpoint *kshim_usb_find_endpoint(struct kshim_usb_interface *i, enum kshim_usb_xfer_type t, bool in)
{ (void)i; (void)t; (void)in; abort(); }

/* Linux input codes the mapping must produce. */
enum {
    KEY_ESC = 1, KEY_BACKSPACE = 14, KEY_ENTER = 28, KEY_SPACE = 57, KEY_KPENTER = 96,
    KEY_UP = 103, KEY_DOWN = 108, KEY_VOLUMEDOWN = 114, KEY_VOLUMEUP = 115,
    KEY_SCROLLUP = 177, KEY_SCROLLDOWN = 178,
    BTN_SOUTH = 0x130, BTN_EAST = 0x131, BTN_DPAD_UP = 0x220, BTN_DPAD_DOWN = 0x221,
};

static const uint8_t QemuKeyboard[] = {
    0x05, 0x01, 0x09, 0x06, 0xa1, 0x01, 0x75, 0x01, 0x95, 0x08, 0x05, 0x07, 0x19, 0xe0, 0x29, 0xe7,
    0x15, 0x00, 0x25, 0x01, 0x81, 0x02, 0x95, 0x01, 0x75, 0x08, 0x81, 0x01, 0x95, 0x05, 0x75, 0x01,
    0x05, 0x08, 0x19, 0x01, 0x29, 0x05, 0x91, 0x02, 0x95, 0x01, 0x75, 0x03, 0x91, 0x01, 0x95, 0x06,
    0x75, 0x08, 0x15, 0x00, 0x25, 0xff, 0x05, 0x07, 0x19, 0x00, 0x29, 0xff, 0x81, 0x00, 0xc0,
};
static const uint8_t QemuMouse[] = {
    0x05, 0x01, 0x09, 0x02, 0xa1, 0x01, 0x09, 0x01, 0xa1, 0x00, 0x05, 0x09, 0x19, 0x01, 0x29, 0x05,
    0x15, 0x00, 0x25, 0x01, 0x95, 0x05, 0x75, 0x01, 0x81, 0x02, 0x95, 0x01, 0x75, 0x03, 0x81, 0x01,
    0x05, 0x01, 0x09, 0x30, 0x09, 0x31, 0x09, 0x38, 0x15, 0x81, 0x25, 0x7f, 0x75, 0x08, 0x95, 0x03,
    0x81, 0x06, 0xc0, 0xc0,
};
static const uint8_t QemuTablet[] = {
    0x05, 0x01, 0x09, 0x02, 0xa1, 0x01, 0x09, 0x01, 0xa1, 0x00, 0x05, 0x09, 0x19, 0x01, 0x29, 0x05,
    0x15, 0x00, 0x25, 0x01, 0x95, 0x05, 0x75, 0x01, 0x81, 0x02, 0x95, 0x01, 0x75, 0x03, 0x81, 0x01,
    0x05, 0x01, 0x09, 0x30, 0x09, 0x31, 0x15, 0x00, 0x26, 0xff, 0x7f, 0x35, 0x00, 0x46, 0xff, 0x7f,
    0x75, 0x10, 0x95, 0x02, 0x81, 0x02, 0x05, 0x01, 0x09, 0x38, 0x15, 0x81, 0x25, 0x7f, 0x35, 0x00,
    0x45, 0x00, 0x75, 0x08, 0x95, 0x01, 0x81, 0x06, 0xc0, 0xc0,
};
static const uint8_t XboxStyle[] = {
    0x05, 0x01, 0x09, 0x05, 0xa1, 0x01, 0x85, 0x01,
    0x09, 0x01, 0xa1, 0x00, 0x09, 0x30, 0x09, 0x31, 0x15, 0x00, 0x27, 0xff, 0xff, 0x00, 0x00,
    0x95, 0x02, 0x75, 0x10, 0x81, 0x02, 0xc0,
    0x09, 0x01, 0xa1, 0x00, 0x09, 0x32, 0x09, 0x35, 0x15, 0x00, 0x27, 0xff, 0xff, 0x00, 0x00,
    0x95, 0x02, 0x75, 0x10, 0x81, 0x02, 0xc0,
    0x05, 0x02, 0x09, 0xc5, 0x15, 0x00, 0x26, 0xff, 0x03, 0x95, 0x01, 0x75, 0x0a, 0x81, 0x02,
    0x15, 0x00, 0x25, 0x00, 0x75, 0x06, 0x95, 0x01, 0x81, 0x03,
    0x05, 0x02, 0x09, 0xc4, 0x15, 0x00, 0x26, 0xff, 0x03, 0x95, 0x01, 0x75, 0x0a, 0x81, 0x02,
    0x15, 0x00, 0x25, 0x00, 0x75, 0x06, 0x95, 0x01, 0x81, 0x03,
    0x05, 0x01, 0x09, 0x39, 0x15, 0x01, 0x25, 0x08, 0x35, 0x00, 0x46, 0x3b, 0x01, 0x66, 0x14, 0x00,
    0x75, 0x04, 0x95, 0x01, 0x81, 0x42,
    0x75, 0x04, 0x95, 0x01, 0x15, 0x00, 0x25, 0x00, 0x35, 0x00, 0x45, 0x00, 0x65, 0x00, 0x81, 0x03,
    0x05, 0x09, 0x19, 0x01, 0x29, 0x0f, 0x15, 0x00, 0x25, 0x01, 0x75, 0x01, 0x95, 0x0f, 0x81, 0x02,
    0x15, 0x00, 0x25, 0x00, 0x75, 0x01, 0x95, 0x01, 0x81, 0x03,
    0x05, 0x0c, 0x0a, 0x24, 0x02, 0x15, 0x00, 0x25, 0x01, 0x95, 0x01, 0x75, 0x01, 0x81, 0x02,
    0x15, 0x00, 0x25, 0x00, 0x75, 0x07, 0x95, 0x01, 0x81, 0x03,
    0xc0,
};
static const uint8_t GenericPad[] = {
    0x05, 0x01, 0x09, 0x04, 0xa1, 0x01, 0xa1, 0x02,
    0x75, 0x08, 0x95, 0x04, 0x15, 0x00, 0x26, 0xff, 0x00, 0x09, 0x30, 0x09, 0x31, 0x09, 0x32, 0x09, 0x35,
    0x81, 0x02,
    0x75, 0x04, 0x95, 0x01, 0x25, 0x07, 0x46, 0x3b, 0x01, 0x65, 0x14, 0x09, 0x39, 0x81, 0x42,
    0x65, 0x00, 0x75, 0x01, 0x95, 0x0c, 0x25, 0x01, 0x45, 0x01, 0x05, 0x09, 0x19, 0x01, 0x29, 0x0c,
    0x81, 0x02,
    0x06, 0x00, 0xff, 0x75, 0x01, 0x95, 0x08, 0x25, 0x01, 0x45, 0x01, 0x09, 0x01, 0x81, 0x02,
    0xc0, 0xc0,
};
static const uint8_t KeyboardConsumer[] = {
    0x05, 0x01, 0x09, 0x06, 0xa1, 0x01, 0x85, 0x01, 0x05, 0x07, 0x19, 0xe0, 0x29, 0xe7, 0x15, 0x00,
    0x25, 0x01, 0x75, 0x01, 0x95, 0x08, 0x81, 0x02, 0x95, 0x06, 0x75, 0x08, 0x15, 0x00, 0x26, 0xff,
    0x00, 0x19, 0x00, 0x2a, 0xff, 0x00, 0x81, 0x00, 0xc0,
    0x05, 0x0c, 0x09, 0x01, 0xa1, 0x01, 0x85, 0x02, 0x19, 0x00, 0x2a, 0xff, 0x03, 0x15, 0x00, 0x26,
    0xff, 0x03, 0x75, 0x10, 0x95, 0x01, 0x81, 0x00, 0xc0,
};
static const uint8_t SparseConsumer[] = {
    0x05, 0x0c, 0x09, 0x01, 0xa1, 0x01, 0x15, 0x01, 0x25, 0x03, 0x75, 0x02, 0x95, 0x01,
    0x09, 0xe9, 0x09, 0xea, 0x09, 0xe2, 0x81, 0x00, 0x75, 0x06, 0x81, 0x03, 0xc0,
};

/* ---- Helpers ------------------------------------------------------------ */

struct key { int type, action; uint32_t code; uint8_t pressed; };
static unsigned drain(struct key *out, unsigned max)
{
    kshim_key_event_t e;
    unsigned n = 0;
    while (kshim_input_inject_poll_key(&e)) {
        assert(n < max);
        out[n++] = (struct key){e.type, e.action, e.code, e.pressed};
    }
    return n;
}
static void expect_none(void)
{
    struct key k[4];
    assert(drain(k, 4) == 0);
}
static void expect_one(int type, int action, uint32_t code)
{
    struct key k[8];
    unsigned n = drain(k, 8);
    if (n != 1 || k[0].type != type || k[0].action != action || k[0].code != code) {
        fprintf(stderr, "expected one %d/%d/%u, got %u:", type, action, (unsigned)code, n);
        for (unsigned i = 0; i < n; ++i) fprintf(stderr, " %d/%d/%u", k[i].type, k[i].action, (unsigned)k[i].code);
        fprintf(stderr, "\n");
        assert(0);
    }
}
#define PRESS KSHIM_KEY_EVENT_PRESS
#define RELEASE KSHIM_KEY_EVENT_RELEASE
#define A_UP KSHIM_KEY_ACTION_UP
#define A_DOWN KSHIM_KEY_ACTION_DOWN
#define A_SELECT KSHIM_KEY_ACTION_SELECT
#define A_BACK KSHIM_KEY_ACTION_BACK

static bool touch(kshim_touch_event_t *e) { return kshim_input_inject_poll_touch(e); }

static struct kshim_hid_desc Desc;
static struct kshim_hid_input In;

/* ---- Parser ------------------------------------------------------------- */

static void test_parser_qemu(void)
{
    const struct kshim_hid_item *it;
    assert(kshim_hid_parse(QemuKeyboard, sizeof(QemuKeyboard), &Desc) == 0);
    assert(!Desc.report_ids && Desc.application_count == 1);
    assert(Desc.applications[0] == KSHIM_HID_USAGE(1, KSHIM_HID_GD_KEYBOARD));
    /* 8 modifier bits + 1 key array (constant byte and LEDs are no INPUT items). */
    unsigned inputs = 0, outputs = 0;
    for (unsigned i = 0; i < Desc.item_count; ++i) {
        inputs += Desc.items[i].type == KSHIM_HID_REPORT_INPUT;
        outputs += Desc.items[i].type == KSHIM_HID_REPORT_OUTPUT;
    }
    assert(inputs == 9 && outputs == 5);
    it = kshim_hid_find(&Desc, KSHIM_HID_REPORT_INPUT, KSHIM_HID_USAGE(7, KSHIM_HID_KEY_DOWN), -1);
    assert(it && !(it->flags & KSHIM_HID_MAIN_VARIABLE) && it->count == 6 && it->bit_size == 8);
    assert(it->bit_offset == 16 && it->usage == KSHIM_HID_USAGE(7, 0) && it->usage_max == KSHIM_HID_USAGE(7, 0xff));
    /* 0x25 0xff: Logical Maximum -1 read as 255 (minimum 0). */
    assert(it->logical_min == 0 && it->logical_max == 255);
    it = kshim_hid_find(&Desc, KSHIM_HID_REPORT_INPUT, KSHIM_HID_USAGE(7, 0xe1), -1);
    assert(it && (it->flags & KSHIM_HID_MAIN_VARIABLE) && it->bit_offset == 1 && it->bit_size == 1);
    assert(Desc.report_count == 2 && Desc.reports[0].bits == 64 && Desc.reports[1].bits == 8);

    assert(kshim_hid_parse(QemuMouse, sizeof(QemuMouse), &Desc) == 0);
    it = kshim_hid_find(&Desc, KSHIM_HID_REPORT_INPUT, KSHIM_HID_USAGE(1, KSHIM_HID_GD_Y), -1);
    assert(it && it->bit_offset == 16 && it->logical_min == -127 && (it->flags & KSHIM_HID_MAIN_RELATIVE));
    assert(it->application == KSHIM_HID_USAGE(1, KSHIM_HID_GD_MOUSE));
    int32_t v;
    uint8_t mouse_report[] = {0x01, 0xfb, 0x05, 0xff};
    assert(kshim_hid_get_value(it, mouse_report, 4, 0, &v) && v == 5);
    it = kshim_hid_find(&Desc, KSHIM_HID_REPORT_INPUT, KSHIM_HID_USAGE(1, KSHIM_HID_GD_X), -1);
    assert(kshim_hid_get_value(it, mouse_report, 4, 0, &v) && v == -5);
    assert(!kshim_hid_get_value(it, mouse_report, 1, 0, &v)); /* out of the report */

    assert(kshim_hid_parse(QemuTablet, sizeof(QemuTablet), &Desc) == 0);
    it = kshim_hid_find(&Desc, KSHIM_HID_REPORT_INPUT, KSHIM_HID_USAGE(1, KSHIM_HID_GD_X), -1);
    assert(it && it->bit_size == 16 && it->logical_max == 0x7fff && !(it->flags & KSHIM_HID_MAIN_RELATIVE));
    assert(Desc.reports[0].bits == 48);
}

static void test_parser_gamepads(void)
{
    const struct kshim_hid_item *it;
    assert(kshim_hid_parse(XboxStyle, sizeof(XboxStyle), &Desc) == 0);
    assert(Desc.report_ids && Desc.report_count == 1 && Desc.reports[0].id == 1 && Desc.reports[0].bits == 128);
    it = kshim_hid_find(&Desc, KSHIM_HID_REPORT_INPUT, KSHIM_HID_USAGE(1, KSHIM_HID_GD_HAT), 1);
    assert(it && it->bit_offset == 96 && it->logical_min == 1 && it->logical_max == 8 &&
           (it->flags & KSHIM_HID_MAIN_NULL_STATE));
    it = kshim_hid_find(&Desc, KSHIM_HID_REPORT_INPUT, KSHIM_HID_USAGE(1, KSHIM_HID_GD_Y), 1);
    assert(it && it->logical_max == 65535 && it->bit_offset == 16);
    assert(!kshim_hid_find(&Desc, KSHIM_HID_REPORT_INPUT, KSHIM_HID_USAGE(1, KSHIM_HID_GD_Y), 2));
    it = kshim_hid_find(&Desc, KSHIM_HID_REPORT_INPUT, KSHIM_HID_USAGE(9, 15), -1);
    assert(it && it->bit_offset == 104 + 14);
    it = kshim_hid_find(&Desc, KSHIM_HID_REPORT_INPUT, KSHIM_HID_USAGE(0x0c, 0x224), -1);
    assert(it && it->bit_offset == 120);
    assert(kshim_hid_has_application(&Desc, KSHIM_HID_USAGE(1, KSHIM_HID_GD_GAMEPAD)));

    assert(kshim_hid_parse(GenericPad, sizeof(GenericPad), &Desc) == 0);
    /* Vendor-page bits only advance the offset. */
    for (unsigned i = 0; i < Desc.item_count; ++i) assert(KSHIM_HID_USAGE_PAGE(Desc.items[i].usage) < 0xff00);
    assert(Desc.reports[0].bits == 56 && Desc.item_count == 4 + 1 + 12);
    it = kshim_hid_find(&Desc, KSHIM_HID_REPORT_INPUT, KSHIM_HID_USAGE(9, 1), -1);
    assert(it && it->bit_offset == 36 && it->application == KSHIM_HID_USAGE(1, KSHIM_HID_GD_JOYSTICK));

    assert(kshim_hid_parse(KeyboardConsumer, sizeof(KeyboardConsumer), &Desc) == 0);
    assert(Desc.report_ids && Desc.application_count == 2 && Desc.report_count == 2);
    it = kshim_hid_find(&Desc, KSHIM_HID_REPORT_INPUT, KSHIM_HID_USAGE(0x0c, 0xe9), -1);
    assert(it && it->report_id == 2 && it->bit_size == 16 && it->logical_max == 0x3ff);

    assert(kshim_hid_parse(SparseConsumer, sizeof(SparseConsumer), &Desc) == 0);
    it = kshim_hid_find(&Desc, KSHIM_HID_REPORT_INPUT, KSHIM_HID_USAGE(0x0c, 0xe2), -1);
    assert(it && it->usage_count == 3);
    uint32_t usage;
    assert(kshim_hid_array_usage(&Desc, it, 2, &usage) && usage == KSHIM_HID_USAGE(0x0c, 0xea));
    assert(!kshim_hid_array_usage(&Desc, it, 0, &usage));
    assert(!kshim_hid_find(&Desc, KSHIM_HID_REPORT_INPUT, KSHIM_HID_USAGE(0x0c, 0xe5), -1));

    size_t length;
    const uint8_t *x = kshim_usb_xinput_report_descriptor(&length);
    assert(kshim_hid_parse(x, length, &Desc) == 0 && Desc.reports[0].bits == 160);
    it = kshim_hid_find(&Desc, KSHIM_HID_REPORT_INPUT, KSHIM_HID_USAGE(9, 1), -1);
    assert(it && it->bit_offset == 28);
    it = kshim_hid_find(&Desc, KSHIM_HID_REPORT_INPUT, KSHIM_HID_USAGE(1, KSHIM_HID_GD_Y), -1);
    assert(it && it->bit_offset == 64 && it->logical_min == -32768);
}

static void test_parser_errors(void)
{
    static const uint8_t truncated[] = {0x05, 0x01, 0x26, 0xff};
    static const uint8_t stray_end[] = {0xc0};
    static const uint8_t bad_pop[] = {0xb4};
    static const uint8_t zero_id[] = {0x85, 0x00};
    static const uint8_t reserved[] = {0x0c};
    assert(kshim_hid_parse(truncated, sizeof(truncated), &Desc) == KSHIM_IO_ERR_PROTOCOL);
    assert(kshim_hid_parse(stray_end, sizeof(stray_end), &Desc) == KSHIM_IO_ERR_PROTOCOL);
    assert(kshim_hid_parse(bad_pop, sizeof(bad_pop), &Desc) == KSHIM_IO_ERR_PROTOCOL);
    assert(kshim_hid_parse(zero_id, sizeof(zero_id), &Desc) == KSHIM_IO_ERR_PROTOCOL);
    assert(kshim_hid_parse(reserved, sizeof(reserved), &Desc) == KSHIM_IO_ERR_PROTOCOL);
    assert(kshim_hid_parse(QemuMouse, 0, &Desc) == KSHIM_IO_ERR_PROTOCOL);
    uint8_t deep[2 * 9 + 9];
    for (unsigned i = 0; i < 9; ++i) { deep[2 * i] = 0xa1; deep[2 * i + 1] = 0x00; }
    assert(kshim_hid_parse(deep, 18, &Desc) == KSHIM_IO_ERR_OVERFLOW);
    uint8_t push[5] = {0xa4, 0xa4, 0xa4, 0xa4, 0xa4};
    assert(kshim_hid_parse(push, 5, &Desc) == KSHIM_IO_ERR_OVERFLOW);
    /* 200 buttons as variables exceed the item table. */
    static const uint8_t many[] = {0x05, 0x09, 0x19, 0x01, 0x29, 0xc8, 0x15, 0x00, 0x25, 0x01,
                                   0x75, 0x01, 0x95, 0xc8, 0x81, 0x02};
    assert(kshim_hid_parse(many, sizeof(many), &Desc) == KSHIM_IO_ERR_OVERFLOW);
    /* Long items are skipped; Push/Pop restores the usage page. */
    static const uint8_t pushpop[] = {0xfe, 0x02, 0x10, 0xaa, 0xbb, 0x05, 0x01, 0xa4, 0x05, 0x09,
                                      0xb4, 0x09, 0x30, 0x15, 0x00, 0x25, 0x7f, 0x75, 0x08, 0x95,
                                      0x01, 0x81, 0x02};
    assert(kshim_hid_parse(pushpop, sizeof(pushpop), &Desc) == 0);
    assert(Desc.item_count == 1 && Desc.items[0].usage == KSHIM_HID_USAGE(1, 0x30));
    /* An extended (4-byte) usage ignores the usage page. */
    static const uint8_t extended[] = {0x05, 0x09, 0x0b, 0xe9, 0x00, 0x0c, 0x00, 0x15, 0x00, 0x25,
                                       0x01, 0x75, 0x01, 0x95, 0x01, 0x81, 0x02};
    assert(kshim_hid_parse(extended, sizeof(extended), &Desc) == 0);
    assert(Desc.items[0].usage == KSHIM_HID_USAGE(0x0c, 0xe9));
    /* Nothing mappable. */
    static const uint8_t vendor[] = {0x06, 0x00, 0xff, 0x09, 0x01, 0xa1, 0x01, 0x75, 0x08, 0x95,
                                     0x08, 0x09, 0x01, 0x81, 0x02, 0xc0};
    assert(kshim_hid_parse(vendor, sizeof(vendor), &Desc) == 0);
    assert(kshim_hid_input_init(&In, &Desc) == ERROR_NOT_SUPPORTED);
}

/* ---- Mapping ------------------------------------------------------------ */

static void test_keyboard(void)
{
    assert(kshim_hid_parse(QemuKeyboard, sizeof(QemuKeyboard), &Desc) == 0);
    assert(kshim_hid_input_init(&In, &Desc) == 0 && In.caps == KSHIM_HID_INPUT_KEYS);
    uint8_t r[8] = {0};
    r[2] = 0x51;
    assert(kshim_hid_input_report(&In, r, 8) == 0);
    expect_one(PRESS, A_DOWN, KEY_DOWN);
    assert(kshim_hid_input_report(&In, r, 8) == 0); /* unchanged: no edge */
    expect_none();
    r[0] = 0x02; r[3] = 0x28;                       /* shift + Enter */
    kshim_hid_input_report(&In, r, 8);
    expect_one(PRESS, A_SELECT, KEY_ENTER);
    uint8_t phantom[8] = {0, 0, 1, 1, 1, 1, 1, 1};  /* ErrorRollOver: keep state */
    kshim_hid_input_report(&In, phantom, 8);
    expect_none();
    memset(r, 0, sizeof(r));
    kshim_hid_input_report(&In, r, 8);
    struct key k[4];
    assert(drain(k, 4) == 2 && k[0].type == RELEASE && k[1].type == RELEASE);
    assert((k[0].code == KEY_DOWN && k[1].code == KEY_ENTER) || (k[0].code == KEY_ENTER && k[1].code == KEY_DOWN));
    static const struct { uint8_t usage; int action; uint32_t code; } keys[] = {
        {0x52, A_UP, KEY_UP}, {0x29, A_BACK, KEY_ESC}, {0x2a, A_BACK, KEY_BACKSPACE},
        {0x2c, A_SELECT, KEY_SPACE}, {0x58, A_SELECT, KEY_KPENTER},
    };
    for (unsigned i = 0; i < sizeof(keys) / sizeof(keys[0]); ++i) {
        r[4] = keys[i].usage;
        kshim_hid_input_report(&In, r, 8);
        expect_one(PRESS, keys[i].action, keys[i].code);
        r[4] = 0;
        kshim_hid_input_report(&In, r, 8);
        expect_one(RELEASE, keys[i].action, keys[i].code);
    }
    r[4] = 0x04; /* 'a' does not map */
    kshim_hid_input_report(&In, r, 8);
    expect_none();
    /* Keyboards never repeat. */
    r[4] = 0x51;
    kshim_hid_input_report(&In, r, 8);
    expect_one(PRESS, A_DOWN, KEY_DOWN);
    now_ms += 5000;
    kshim_hid_input_service(&In, now_ms);
    expect_none();
    kshim_hid_input_reset(&In);
    expect_one(RELEASE, A_DOWN, KEY_DOWN);
    /* Boot layout (HID 1.11 B.1). */
    kshim_hid_input_init_boot(&In, true);
    uint8_t boot[8] = {0, 0, 0x52, 0, 0, 0, 0, 0};
    kshim_hid_input_report(&In, boot, 8);
    expect_one(PRESS, A_UP, KEY_UP);
    boot[2] = 0;
    kshim_hid_input_report(&In, boot, 8);
    expect_one(RELEASE, A_UP, KEY_UP);
}

static void test_mouse_and_tablet(void)
{
    kshim_touch_event_t e;
    kshim_input_inject_set_surface(320, 240);
    assert(kshim_hid_parse(QemuMouse, sizeof(QemuMouse), &Desc) == 0);
    assert(kshim_hid_input_init(&In, &Desc) == 0);
    assert(In.caps == (KSHIM_HID_INPUT_POINTER | KSHIM_HID_INPUT_KEYS)); /* wheel keys */
    uint8_t r[4] = {0x00, 10, (uint8_t)-20, 0};
    kshim_hid_input_report(&In, r, 4);
    assert(touch(&e) && e.type == KSHIM_TOUCH_EVENT_HOVER && e.x == 170 && e.y == 100 &&
           e.contact_id == KSHIM_INPUT_POINTER_CONTACT);
    r[0] = 1; r[1] = 0; r[2] = 0;
    kshim_hid_input_report(&In, r, 4);
    assert(touch(&e) && e.type == KSHIM_TOUCH_EVENT_PRESS && e.x == 170 && e.y == 100);
    r[1] = (uint8_t)-127; r[2] = (uint8_t)-127;
    kshim_hid_input_report(&In, r, 4);
    kshim_hid_input_report(&In, r, 4);
    kshim_hid_input_report(&In, r, 4); /* merges into the queued move before it */
    assert(touch(&e) && e.type == KSHIM_TOUCH_EVENT_MOVE && e.x == 43 && e.y == 0);
    assert(touch(&e) && e.type == KSHIM_TOUCH_EVENT_MOVE && e.x == 0 && e.y == 0); /* clamped */
    assert(!touch(&e));
    r[0] = 0; r[1] = r[2] = 0;
    kshim_hid_input_report(&In, r, 4);
    assert(touch(&e) && e.type == KSHIM_TOUCH_EVENT_RELEASE && e.x == 0 && e.y == 0);
    kshim_hid_input_report(&In, r, 4); /* nothing changed */
    assert(!touch(&e));
    r[3] = 1;
    kshim_hid_input_report(&In, r, 4);
    struct key k[8];
    assert(drain(k, 8) == 2 && k[0].type == PRESS && k[0].code == KEY_SCROLLUP && k[0].action == A_UP &&
           k[1].type == RELEASE && k[1].code == KEY_SCROLLUP);
    r[3] = (uint8_t)-9; /* at most 3 steps */
    kshim_hid_input_report(&In, r, 4);
    assert(drain(k, 8) == 6 && k[5].code == KEY_SCROLLDOWN && k[4].action == A_DOWN);

    /* Boot mouse layout (HID 1.11 B.2). */
    kshim_hid_input_init_boot(&In, false);
    assert(In.caps == KSHIM_HID_INPUT_POINTER);
    uint8_t boot[3] = {1, 5, 5};
    kshim_hid_input_report(&In, boot, 3);
    assert(touch(&e) && e.type == KSHIM_TOUCH_EVENT_PRESS && e.x == 5 && e.y == 5);
    kshim_hid_input_reset(&In);
    assert(touch(&e) && e.type == KSHIM_TOUCH_EVENT_RELEASE);

    /* usb-tablet: absolute 0..0x7fff scaled to the surface. */
    assert(kshim_hid_parse(QemuTablet, sizeof(QemuTablet), &Desc) == 0);
    assert(kshim_hid_input_init(&In, &Desc) == 0 && (In.caps & KSHIM_HID_INPUT_POINTER));
    uint8_t t[6] = {0, 0xff, 0x7f, 0x00, 0x40, 0};
    kshim_hid_input_report(&In, t, 6);
    assert(touch(&e) && e.type == KSHIM_TOUCH_EVENT_HOVER && e.x == 319 && e.y == 120);
    kshim_hid_input_report(&In, t, 6);
    assert(!touch(&e));
    t[0] = 1;
    kshim_hid_input_report(&In, t, 6);
    assert(touch(&e) && e.type == KSHIM_TOUCH_EVENT_PRESS && e.x == 319 && e.y == 120);
    t[0] = 0;
    kshim_hid_input_report(&In, t, 6);
    assert(touch(&e) && e.type == KSHIM_TOUCH_EVENT_RELEASE);
    t[0] = 1;
    kshim_hid_input_report(&In, t, 6);
    kshim_hid_input_reset(&In); /* device gone while pressed */
    assert(touch(&e) && e.type == KSHIM_TOUCH_EVENT_PRESS);
    assert(touch(&e) && e.type == KSHIM_TOUCH_EVENT_RELEASE && e.x == 319);
}

static void xbox_report(uint8_t *r, uint16_t x, uint16_t y, uint8_t hat, uint16_t buttons)
{
    memset(r, 0, 17);
    r[0] = 1;
    r[1] = (uint8_t)x; r[2] = (uint8_t)(x >> 8);
    r[3] = (uint8_t)y; r[4] = (uint8_t)(y >> 8);
    r[13] = hat & 0x0fU;
    r[14] = (uint8_t)buttons; r[15] = (uint8_t)(buttons >> 8);
}

static void test_gamepads(void)
{
    uint8_t r[17];
    assert(kshim_hid_parse(XboxStyle, sizeof(XboxStyle), &Desc) == 0);
    assert(kshim_hid_input_init(&In, &Desc) == 0 && In.caps == KSHIM_HID_INPUT_KEYS);
    xbox_report(r, 32768, 32768, 0, 0);
    kshim_hid_input_report(&In, r, 17);
    expect_none();
    xbox_report(r, 32768, 32768, 1, 0); /* hat N */
    kshim_hid_input_report(&In, r, 17);
    expect_one(PRESS, A_UP, BTN_DPAD_UP);
    xbox_report(r, 32768, 32768, 2, 0); /* NE still up */
    kshim_hid_input_report(&In, r, 17);
    expect_none();
    xbox_report(r, 32768, 32768, 5, 0); /* S */
    kshim_hid_input_report(&In, r, 17);
    struct key k[8];
    assert(drain(k, 8) == 2 && k[0].type == RELEASE && k[0].code == BTN_DPAD_UP &&
           k[1].type == PRESS && k[1].code == BTN_DPAD_DOWN && k[1].action == A_DOWN);
    xbox_report(r, 32768, 32768, 0, 0); /* null: centred */
    kshim_hid_input_report(&In, r, 17);
    expect_one(RELEASE, A_DOWN, BTN_DPAD_DOWN);
    xbox_report(r, 32768, 32768, 0, 0x0001); /* A */
    kshim_hid_input_report(&In, r, 17);
    expect_one(PRESS, A_SELECT, BTN_SOUTH);
    xbox_report(r, 32768, 32768, 0, 0x0002); /* B */
    kshim_hid_input_report(&In, r, 17);
    assert(drain(k, 8) == 2 && k[0].code == BTN_SOUTH && k[0].type == RELEASE &&
           k[1].code == BTN_EAST && k[1].action == A_BACK && k[1].type == PRESS);
    xbox_report(r, 32768, 32768, 0, 0x0004); /* X: unmapped */
    kshim_hid_input_report(&In, r, 17);
    expect_one(RELEASE, A_BACK, BTN_EAST);

    /* Stick Y: press past 50 %, hold above 25 %, release inside 25 %. */
    xbox_report(r, 32768, 16000, 0, 0); /* about -51 % */
    kshim_hid_input_report(&In, r, 17);
    expect_one(PRESS, A_UP, BTN_DPAD_UP);
    xbox_report(r, 32768, 22000, 0, 0); /* about -33 %: held */
    kshim_hid_input_report(&In, r, 17);
    expect_none();
    xbox_report(r, 32768, 24000, 1, 0); /* hat up too: same code, no second press */
    kshim_hid_input_report(&In, r, 17);
    expect_none();
    xbox_report(r, 32768, 26000, 0, 0); /* about -21 %: released */
    kshim_hid_input_report(&In, r, 17);
    expect_one(RELEASE, A_UP, BTN_DPAD_UP);
    xbox_report(r, 0, 32768, 0, 0); /* X is not mapped */
    kshim_hid_input_report(&In, r, 17);
    expect_none();
    xbox_report(r, 32768, 20000, 0, 0); /* -39 %: inside the press threshold */
    kshim_hid_input_report(&In, r, 17);
    expect_none();

    /* Auto-repeat of a held direction: 500 ms delay, then every 150 ms. */
    now_ms = 10000;
    xbox_report(r, 32768, 65535, 0, 0);
    kshim_hid_input_report(&In, r, 17);
    expect_one(PRESS, A_DOWN, BTN_DPAD_DOWN);
    kshim_hid_input_service(&In, 10499);
    expect_none();
    kshim_hid_input_service(&In, 10500);
    assert(drain(k, 8) == 1 && k[0].type == PRESS && k[0].pressed == 2 && k[0].code == BTN_DPAD_DOWN);
    kshim_hid_input_service(&In, 10600);
    expect_none();
    kshim_hid_input_service(&In, 10650);
    assert(drain(k, 8) == 1 && k[0].pressed == 2);
    xbox_report(r, 32768, 32768, 0, 0);
    kshim_hid_input_report(&In, r, 17);
    expect_one(RELEASE, A_DOWN, BTN_DPAD_DOWN);
    kshim_hid_input_service(&In, 20000);
    expect_none();
    /* Buttons do not repeat. */
    xbox_report(r, 32768, 32768, 0, 1);
    kshim_hid_input_report(&In, r, 17);
    expect_one(PRESS, A_SELECT, BTN_SOUTH);
    kshim_hid_input_service(&In, 30000);
    expect_none();
    kshim_hid_input_reset(&In);
    expect_one(RELEASE, A_SELECT, BTN_SOUTH);
    /* A report for another ID is ignored. */
    xbox_report(r, 32768, 0, 1, 1);
    r[0] = 2;
    kshim_hid_input_report(&In, r, 17);
    expect_none();

    /* Generic joystick: 0..7 hat with 8 = null, 8-bit axes, vendor bits. */
    assert(kshim_hid_parse(GenericPad, sizeof(GenericPad), &Desc) == 0);
    assert(kshim_hid_input_init(&In, &Desc) == 0);
    uint8_t g[7] = {0x80, 0x80, 0x80, 0x80, 0x08, 0x00, 0xff};
    kshim_hid_input_report(&In, g, 7);
    expect_none();
    g[4] = 0x04; /* hat S */
    kshim_hid_input_report(&In, g, 7);
    expect_one(PRESS, A_DOWN, BTN_DPAD_DOWN);
    g[4] = 0x08 | 0x10; /* null hat + button 1 */
    kshim_hid_input_report(&In, g, 7);
    assert(drain(k, 8) == 2 && k[0].code == BTN_DPAD_DOWN && k[1].code == BTN_SOUTH);
    g[4] = 0x08; g[1] = 0x00; /* stick up */
    kshim_hid_input_report(&In, g, 7);
    assert(drain(k, 8) == 2 && k[0].type == RELEASE && k[1].code == BTN_DPAD_UP);
    kshim_hid_input_reset(&In);
    expect_one(RELEASE, A_UP, BTN_DPAD_UP);
}

static void xinput_report(uint8_t *r, uint8_t b2, uint8_t b3, int16_t lx, int16_t ly)
{
    memset(r, 0, 20);
    r[0] = 0x00; r[1] = 0x14; r[2] = b2; r[3] = b3;
    r[6] = (uint8_t)lx; r[7] = (uint8_t)((uint16_t)lx >> 8);
    r[8] = (uint8_t)ly; r[9] = (uint8_t)((uint16_t)ly >> 8);
}

static void test_xinput(void)
{
    uint8_t r[20];
    size_t length;
    const uint8_t *x = kshim_usb_xinput_report_descriptor(&length);
    assert(kshim_hid_parse(x, length, &Desc) == 0 && kshim_hid_input_init(&In, &Desc) == 0);
    xinput_report(r, 0, 0x10, 0, 0); /* A */
    assert(kshim_usb_xinput_report(&In, r, 20) == 0);
    expect_one(PRESS, A_SELECT, BTN_SOUTH);
    xinput_report(r, 0, 0x20, 0, 0); /* B */
    kshim_usb_xinput_report(&In, r, 20);
    struct key k[8];
    assert(drain(k, 8) == 2 && k[1].code == BTN_EAST && k[1].action == A_BACK);
    xinput_report(r, 0x02, 0, 0, 0); /* D-pad down */
    kshim_usb_xinput_report(&In, r, 20);
    assert(drain(k, 8) == 2 && k[1].code == BTN_DPAD_DOWN && k[1].type == PRESS);
    xinput_report(r, 0x01, 0, 0, 0); /* D-pad up */
    kshim_usb_xinput_report(&In, r, 20);
    assert(drain(k, 8) == 2 && k[1].code == BTN_DPAD_UP && k[1].action == A_UP);
    xinput_report(r, 0x00, 0, 0, 32767); /* left stick up (XInput Y is positive up) */
    kshim_usb_xinput_report(&In, r, 20);
    expect_none(); /* still UP: same code */
    xinput_report(r, 0x00, 0, 0, -32768); /* stick down */
    kshim_usb_xinput_report(&In, r, 20);
    assert(drain(k, 8) == 2 && k[0].code == BTN_DPAD_UP && k[0].type == RELEASE &&
           k[1].code == BTN_DPAD_DOWN && k[1].type == PRESS);
    xinput_report(r, 0x00, 0x40, 0, 2000); /* X button, stick inside the dead zone */
    kshim_usb_xinput_report(&In, r, 20);
    expect_one(RELEASE, A_DOWN, BTN_DPAD_DOWN);
    /* Non-input messages (LED status 0x01, length 3) are ignored. */
    uint8_t led[3] = {0x01, 0x03, 0x0e};
    assert(kshim_usb_xinput_report(&In, led, 3) == 0);
    expect_none();
    kshim_hid_input_reset(&In);
    expect_none();
}

static void test_report_ids_and_consumer(void)
{
    assert(kshim_hid_parse(KeyboardConsumer, sizeof(KeyboardConsumer), &Desc) == 0);
    assert(kshim_hid_input_init(&In, &Desc) == 0);
    uint8_t kb[8] = {1, 0, 0x51, 0, 0, 0, 0, 0};
    kshim_hid_input_report(&In, kb, 8);
    expect_one(PRESS, A_DOWN, KEY_DOWN);
    uint8_t cc[3] = {2, 0xe9, 0x00};
    kshim_hid_input_report(&In, cc, 3);
    expect_one(PRESS, A_UP, KEY_VOLUMEUP); /* the keyboard's Down stays held */
    cc[1] = 0;
    kshim_hid_input_report(&In, cc, 3);
    expect_one(RELEASE, A_UP, KEY_VOLUMEUP);
    kb[2] = 0;
    kshim_hid_input_report(&In, kb, 8);
    expect_one(RELEASE, A_DOWN, KEY_DOWN);
    assert(kshim_hid_input_report(&In, kb, 0) == KSHIM_IO_ERR_PROTOCOL);

    assert(kshim_hid_parse(SparseConsumer, sizeof(SparseConsumer), &Desc) == 0);
    assert(kshim_hid_input_init(&In, &Desc) == 0);
    uint8_t s = 2;
    kshim_hid_input_report(&In, &s, 1);
    expect_one(PRESS, A_DOWN, KEY_VOLUMEDOWN);
    s = 3; /* mute: not mapped */
    kshim_hid_input_report(&In, &s, 1);
    expect_one(RELEASE, A_DOWN, KEY_VOLUMEDOWN);
    s = 1;
    kshim_hid_input_report(&In, &s, 1);
    expect_one(PRESS, A_UP, KEY_VOLUMEUP);
    s = 0; /* null */
    kshim_hid_input_report(&In, &s, 1);
    expect_one(RELEASE, A_UP, KEY_VOLUMEUP);
}

/* ---- Injection queue and consumer hooks --------------------------------- */

static unsigned services;
static void count_service(void *context)
{
    (void)context;
    ++services;
    kshim_input_service(); /* re-entry is a no-op */
}

static void test_injection(void)
{
    kshim_key_event_t e = {PRESS, A_DOWN, KEY_DOWN, 1}, out;
    struct kshim_input_source src = {.name = "test", .caps = KSHIM_INPUT_CAP_KEYS, .service = count_service};
    struct kshim_input_source ptr = {.name = "ptr", .caps = KSHIM_INPUT_CAP_POINTER};
    assert(!kshim_input_inject_has(KSHIM_INPUT_CAP_KEYS));
    assert(kshim_input_source_register(&src) == 0);
    assert(kshim_input_source_register(&src) == KSHIM_IO_ERR_EXISTS);
    assert(kshim_input_inject_has(KSHIM_INPUT_CAP_KEYS));
    assert(!kshim_input_inject_has(KSHIM_INPUT_CAP_KEYS | KSHIM_INPUT_CAP_POINTER));
    assert(kshim_input_source_register(&ptr) == 0);
    assert(kshim_input_inject_has(KSHIM_INPUT_CAP_KEYS | KSHIM_INPUT_CAP_POINTER));
    assert(kshim_input_source_unregister(&ptr) == 0 && kshim_input_source_unregister(&ptr) == ERROR_NOT_FOUND);

    kshim_input_service();
    assert(services == 1);
    boot_cpu = false; /* other CPUs never pump */
    kshim_input_service();
    assert(services == 1);
    boot_cpu = true;

    /* Overflow drops and counts; FIFO order. */
    for (unsigned i = 0; i < 32; ++i) {
        e.code = i;
        assert(kshim_input_inject_key(&e) == 0);
    }
    assert(kshim_input_inject_key(&e) == ERROR_BUSY);
    for (unsigned i = 0; i < 32; ++i) assert(kshim_input_inject_poll_key(&out) && out.code == i);
    assert(!kshim_input_inject_poll_key(&out));

    /* The kshim_key_source_poll() hook: service first, then injected keys
     * before the (unconfigured) platform keys. */
    unsigned before = services;
    e.code = KEY_UP; e.action = A_UP;
    kshim_input_inject_key(&e);
    static QcomKeys platform_keys; /* none configured (QEMU) */
    kshim_key_source_t *keys = QcomKeysSource(&platform_keys);
    assert(kshim_key_source_poll(keys, &out) == KSHIM_KEYS_EVENT && out.code == KEY_UP && out.action == A_UP);
    assert(services == before + 1);
    assert(kshim_key_source_poll(keys, &out) < 0); /* no platform keys configured */

    /* Pointer queue: relative moves coalesce, edges stay. */
    kshim_touch_event_t t;
    kshim_input_inject_set_surface(100, 50);
    struct kshim_input_pointer_event p = {.mode = KSHIM_INPUT_POINTER_RELATIVE, .x = 1, .y = 1};
    for (unsigned i = 0; i < 200; ++i) assert(kshim_input_inject_pointer(&p) == 0);
    assert(touch(&t) && t.type == KSHIM_TOUCH_EVENT_HOVER && t.x == 51 && t.y == 26);
    assert(touch(&t) && t.type == KSHIM_TOUCH_EVENT_HOVER && t.x == 99 && t.y == 49);
    assert(!touch(&t));
    struct kshim_input_pointer_event a = {.mode = KSHIM_INPUT_POINTER_ABSOLUTE, .x = 0, .y = 0,
                                          .range_x = 1000, .range_y = 1000, .pressed = true};
    assert(kshim_input_inject_pointer(&a) == 0);
    a.pressed = false;
    assert(kshim_input_inject_pointer(&a) == 0);
    a.x = 500;
    assert(kshim_input_inject_pointer(&a) == 0);
    assert(touch(&t) && t.type == KSHIM_TOUCH_EVENT_PRESS && t.x == 0);
    assert(touch(&t) && t.type == KSHIM_TOUCH_EVENT_RELEASE && t.x == 0);
    assert(touch(&t) && t.type == KSHIM_TOUCH_EVENT_HOVER && t.x == 50);
    a.range_x = 0;
    assert(kshim_input_inject_pointer(&a) == ERROR_INVALID_PARAMETER);
    /* No surface yet: events wait in the queue. */
    kshim_input_inject_set_surface(0, 0);
    a.range_x = 1000;
    kshim_input_inject_pointer(&a);
    assert(!touch(&t));
    kshim_input_inject_set_surface(100, 50);
    assert(touch(&t) && t.type == KSHIM_TOUCH_EVENT_HOVER);

    /* Handoff: queued events dropped, new ones refused, no more pumping. */
    kshim_input_inject_key(&e);
    kshim_input_inject_pointer(&p);
    kshim_input_inject_quiesce();
    kshim_input_inject_quiesce(); /* idempotent */
    assert(!kshim_input_inject_poll_key(&out) && !touch(&t));
    assert(kshim_input_inject_key(&e) == ERROR_NOT_READY);
    assert(kshim_input_inject_pointer(&p) == ERROR_NOT_READY);
    before = services;
    kshim_input_service();
    assert(services == before);
}

int main(void)
{
    test_parser_qemu();
    test_parser_gamepads();
    test_parser_errors();
    test_keyboard();
    test_mouse_and_tablet();
    test_gamepads();
    test_xinput();
    test_report_ids_and_consumer();
    test_injection(); /* last: ends with the handoff quiesce */
    puts("usbhid_hid_test: OK");
    return 0;
}
