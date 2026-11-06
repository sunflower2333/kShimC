/* SPDX-License-Identifier: MIT */
/*
 * USB HID transport (kshim/usb_class.h) and the XInput gamepad fallback.
 *
 * HID (interface class 3), from Device Class Definition for HID 1.11:
 * 7.1.1 GET_DESCRIPTOR(Report) to the interface (bmRequestType 0x81,
 * wValue 0x2200), 7.2.4 SET_IDLE (0x21, bRequest 0x0a, duration 0 = report
 * only on change), 7.2.6 SET_PROTOCOL (0x21, bRequest 0x0b, 0 = boot),
 * 6.2.1 HID descriptor (bNumDescriptors entries of type + length), and
 * appendix B boot layouts. Reports arrive on the interrupt IN endpoint at
 * the endpoint's bInterval; the HCD completes the URB (IRQ-driven through
 * its event ring, or polled) and the callback maps the report and resubmits.
 *
 * XInput (interface class 0xff, subclass 0x5d, protocol 0x01) is the
 * fallback for gamepads that are not HID at all, such as Xbox 360-class
 * wired controllers and the many handheld controllers that copy them. They
 * have no report descriptor; the input report is a fixed 20-byte message
 * (type 0x00, length 0x14): byte 2 D-pad up/down/left/right, start, back,
 * left/right stick press; byte 3 LB, RB, guide, -, A, B, X, Y; bytes 4-5
 * the triggers; bytes 6-13 left X/Y and right X/Y as little-endian int16
 * with Y positive up. That layout is described below as an equivalent HID
 * report descriptor (written for kShimC), so it goes through the same
 * mapping as HID gamepads: A = Button 1 (SELECT), B = Button 2 (BACK),
 * D-pad and left stick Y = UP/DOWN. The Y axes are negated first to the
 * HID convention (positive down). No initialisation or rumble/LED output
 * is sent; controllers that need a start command (Xbox One GIP, class
 * 0xff/0x47/0xd0) are not handled.
 *
 * Provenance of the XInput facts (the interface triples and the 20-byte
 * layout): Linux drivers/input/joystick/xpad.c (GPL-2.0-or-later) at
 * 40288c9206c17eb66a603262e06a58d300d0f279, sha256 92d10718912544a5360eb7e3
 * c12b12bc151b0070621f478bcf0b787dbd8bf200: XPAD_XBOX360_VENDOR_PROTOCOL
 * (class 0xff, subclass 93 = 0x5d, wired protocol 1),
 * XPAD_XBOXONE_VENDOR_PROTOCOL (subclass 71 = 0x47, protocol 208 = 0xd0) and
 * the byte/bit positions xpad360_process_packet reads. Facts only: no xpad
 * code was used; kShimC describes the layout as a HID report descriptor for
 * its own parser.
 */
#include <config.h>
#include <kshim/usb.h>
#include <kshim/usb_class.h>
#include <kshim/hid.h>
#include <kshim/input_inject.h>
#include <kshim/io.h>
#include <kshim/time.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define HID_REQ_SET_IDLE     0x0aU
#define HID_REQ_SET_PROTOCOL 0x0bU
#define HID_PROTOCOL_BOOT    0U
#define HID_SUBCLASS_BOOT    1U
#define HID_PROTOCOL_KEYBOARD 1U
#define HID_PROTOCOL_MOUSE   2U
#define HID_RT_CLASS_OUT_INTERFACE (KSHIM_USB_DIR_OUT | KSHIM_USB_TYPE_CLASS | KSHIM_USB_RECIP_INTERFACE)
#define HID_RT_STD_IN_INTERFACE    (KSHIM_USB_DIR_IN | KSHIM_USB_TYPE_STANDARD | KSHIM_USB_RECIP_INTERFACE)
#define HID_MAX_REPORT_DESCRIPTOR 4096U
#define HID_MAX_REPORT_BYTES 1024U
#define HID_CONTROL_TIMEOUT_MS 1000U
#define HID_MAX_URB_ERRORS 16U

#ifndef CONFIG_KSHIM_USB_TRACE
#define CONFIG_KSHIM_USB_TRACE 0
#endif

#define XINPUT_SUBCLASS 0x5dU
#define XINPUT_PROTOCOL_GAMEPAD 0x01U
#define XINPUT_REPORT_BYTES 20U

struct hid_if {
    struct kshim_usb_interface *intf;
    struct kshim_usb_device *dev;
    struct kshim_usb_endpoint *ep;
    struct kshim_usb_urb urb;
    void *raw;
    uint8_t *buffer;
    bool xinput;
    bool urb_active, urb_error, stopped;
    unsigned urb_errors;
    const struct kshim_hid_quirk *quirk;
    struct kshim_hid_desc desc;
    struct kshim_hid_input input;
#if CONFIG_KSHIM_USB_TRACE
    struct kshim_usb_trace_state trace;
#endif
};

/* ---- Quirks (kshim/usb_class.h; rows in hid_quirks.c) --------------------- */

const struct kshim_hid_quirk *kshim_hid_quirk_find(uint16_t vendor, uint16_t product, uint8_t interface)
{
    for (const struct kshim_hid_quirk *q = kshim_hid_quirks; q->vendor; ++q)
        if (q->vendor == vendor && (!q->product || q->product == product) &&
            (q->interface < 0 || q->interface == interface))
            return q;
    return NULL;
}

static const struct kshim_hid_quirk *quirk_of(const struct kshim_usb_interface *intf)
{
    const struct kshim_usb_device *dev = intf->device;
    return kshim_hid_quirk_find(dev->descriptor.idVendor, dev->descriptor.idProduct, intf->number);
}

static const char *quirk_name(const struct kshim_hid_quirk *quirk)
{
    return quirk ? (quirk->name ? quirk->name : "unnamed") : "none";
}

/* ---- Trace (CONFIG_KSHIM_USB_TRACE) ----------------------------------------- */

#if CONFIG_KSHIM_USB_TRACE
static void trace_unbound(const struct kshim_usb_interface *intf, const struct kshim_hid_quirk *quirk,
                          const char *reason)
{
    kshim_usb_trace(intf->device, intf->number, "hid", "map=unbound quirk=%s reason=\"%s\"", quirk_name(quirk),
                    reason);
}

/* Held key codes after a report: "<code>:<action>,...". */
static void held_text(const struct kshim_hid_input *input, char *out, size_t size)
{
    static const char *const actions[] = {"none", "up", "down", "select", "back"};
    size_t at = 0;
    out[0] = 0;
    for (unsigned i = 0; i < input->held_count && at + 1U < size; ++i) {
        int action = kshim_hid_input_action(input->held[i]);
        int n = snprintf(out + at, size - at, "%s%x:%s", i ? "," : "", (unsigned)input->held[i],
                         actions[action >= 0 && action <= 4 ? action : 0]);
        if (n < 0) break;
        at += (size_t)n;
    }
}
#else
#define trace_unbound(intf, quirk, reason) ((void)(intf), (void)(quirk), (void)(reason))
#endif

static struct kshim_input_source HidSource = {.name = "usb-hid"};
static bool HidSourceRegistered;

static void source_add(unsigned hid_caps)
{
    if (hid_caps & KSHIM_HID_INPUT_KEYS) HidSource.caps |= KSHIM_INPUT_CAP_KEYS;
    if (hid_caps & KSHIM_HID_INPUT_POINTER) HidSource.caps |= KSHIM_INPUT_CAP_POINTER;
    if (HidSourceRegistered) return;
    int status = kshim_input_source_register(&HidSource);
    HidSourceRegistered = !status || status == KSHIM_IO_ERR_EXISTS;
}

static void *alloc_aligned(size_t size, void **raw)
{
    *raw = calloc(1, size + 63U);
    if (!*raw) return NULL;
    return (void *)(((uintptr_t)*raw + 63U) & ~(uintptr_t)63U);
}

static uint16_t le16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }

/* Equivalent HID report descriptor of the XInput 20-byte input report (see
 * the file comment), using HID 1.11 short items. */
static const uint8_t XInputReportDescriptor[] = {
    0x05, 0x01,             /* Usage Page (Generic Desktop) */
    0x09, 0x05,             /* Usage (Game Pad) */
    0xa1, 0x01,             /* Collection (Application) */
    0x75, 0x08, 0x95, 0x02, /*   Report Size 8, Count 2: message type, length */
    0x81, 0x01,             /*   Input (Constant) */
    0x15, 0x00, 0x25, 0x01, /*   Logical 0..1 */
    0x75, 0x01, 0x95, 0x04, /*   Report Size 1, Count 4 */
    0x09, 0x90, 0x09, 0x91, /*   Usage D-pad Up, D-pad Down */
    0x09, 0x93, 0x09, 0x92, /*   Usage D-pad Left, D-pad Right */
    0x81, 0x02,             /*   Input (Data, Variable, Absolute) */
    0x05, 0x09,             /*   Usage Page (Button) */
    0x09, 0x08, 0x09, 0x07, /*   Start (8), Back (7) */
    0x09, 0x09, 0x09, 0x0a, /*   left stick (9), right stick (10) */
    0x81, 0x02,             /*   Input (Data, Variable, Absolute) */
    0x95, 0x03,             /*   Report Count 3 */
    0x09, 0x05, 0x09, 0x06, /*   LB (5), RB (6) */
    0x09, 0x0d,             /*   Guide (13) */
    0x81, 0x02,             /*   Input (Data, Variable, Absolute) */
    0x95, 0x01, 0x81, 0x01, /*   1 bit padding */
    0x95, 0x04,             /*   Report Count 4 */
    0x09, 0x01, 0x09, 0x02, /*   A (1), B (2) */
    0x09, 0x03, 0x09, 0x04, /*   X (3), Y (4) */
    0x81, 0x02,             /*   Input (Data, Variable, Absolute) */
    0x05, 0x01,             /*   Usage Page (Generic Desktop) */
    0x26, 0xff, 0x00,       /*   Logical Maximum 255 */
    0x75, 0x08, 0x95, 0x02, /*   Report Size 8, Count 2 */
    0x09, 0x32, 0x09, 0x35, /*   Usage Z, Rz: left and right trigger */
    0x81, 0x02,             /*   Input (Data, Variable, Absolute) */
    0x16, 0x00, 0x80,       /*   Logical Minimum -32768 */
    0x26, 0xff, 0x7f,       /*   Logical Maximum 32767 */
    0x75, 0x10, 0x95, 0x04, /*   Report Size 16, Count 4 */
    0x09, 0x30, 0x09, 0x31, /*   Usage X, Y: left stick */
    0x09, 0x33, 0x09, 0x34, /*   Usage Rx, Ry: right stick */
    0x81, 0x02,             /*   Input (Data, Variable, Absolute) */
    0x75, 0x08, 0x95, 0x06, /*   Report Size 8, Count 6: reserved */
    0x81, 0x01,             /*   Input (Constant) */
    0xc0,                   /* End Collection */
};

const uint8_t *kshim_usb_xinput_report_descriptor(size_t *length)
{
    if (length) *length = sizeof(XInputReportDescriptor);
    return XInputReportDescriptor;
}

static int16_t negate16(int16_t value) { return value == INT16_MIN ? INT16_MAX : (int16_t)-value; }

/* XInput input message to the HID convention (Y positive down), then the
 * common mapping. Other message types (LED state, rumble acknowledgement)
 * are ignored. */
int kshim_usb_xinput_report(struct kshim_hid_input *input, const uint8_t *data, size_t length)
{
    if (!input || !data) return ERROR_INVALID_PARAMETER;
    if (length < XINPUT_REPORT_BYTES || data[0] != 0x00 || data[1] < XINPUT_REPORT_BYTES) return 0;
    uint8_t report[XINPUT_REPORT_BYTES];
    memcpy(report, data, sizeof(report));
    static const unsigned y_axes[] = {8, 12};
    for (unsigned i = 0; i < 2; ++i) {
        unsigned at = y_axes[i];
        uint16_t y = (uint16_t)negate16((int16_t)le16(&report[at]));
        report[at] = (uint8_t)y;
        report[at + 1U] = (uint8_t)(y >> 8);
    }
    return kshim_hid_input_report(input, report, sizeof(report));
}

static int submit_report(struct hid_if *h)
{
    int status = kshim_usb_submit_urb(&h->urb);
    h->urb_active = status == 0;
    return status;
}

/* URB callback (kshim_usb_poll_events, thread context): map and resubmit. */
static void report_complete(struct kshim_usb_urb *urb)
{
    struct hid_if *h = urb->context;
    h->urb_active = false;
    if (h->stopped) return;
    if (!urb->status) {
        h->urb_errors = 0;
        if (urb->actual) {
            if (h->xinput) (void)kshim_usb_xinput_report(&h->input, h->buffer, urb->actual);
            else (void)kshim_hid_input_report(&h->input, h->buffer, urb->actual);
#if CONFIG_KSHIM_USB_TRACE
            char held[128];
            held_text(&h->input, held, sizeof(held));
            (void)kshim_usb_trace_report(&h->trace, h->dev, h->intf->number, h->buffer, urb->actual, held);
#endif
        }
        if (submit_report(h)) h->urb_error = true;
        return;
    }
    if (urb->status == KSHIM_IO_ERR_CANCELLED || urb->status == KSHIM_IO_ERR_QUIESCED ||
        urb->status == KSHIM_IO_ERR_NO_DEVICE || urb->status == ERROR_NOT_READY)
        return;
    h->urb_error = true; /* the service hook recovers the endpoint */
}

static void hid_service(struct kshim_usb_interface *intf)
{
    struct hid_if *h = intf->driver_data;
    if (!h || h->stopped) return;
    if (h->urb_error && !h->urb_active) {
        h->urb_error = false;
        (void)kshim_usb_clear_halt(h->dev, h->ep);
        if (++h->urb_errors > HID_MAX_URB_ERRORS) {
            printf("HID %04x:%04x interface %u: input endpoint failing, stopped\n",
                   h->dev->descriptor.idVendor, h->dev->descriptor.idProduct, intf->number);
            h->stopped = true;
            kshim_hid_input_reset(&h->input);
            return;
        }
        if (submit_report(h)) h->urb_error = true;
    }
    kshim_hid_input_service(&h->input, get_current_time());
}

static uint32_t max_input_bytes(const struct kshim_hid_desc *desc)
{
    uint32_t bytes = 0;
    for (unsigned i = 0; i < desc->report_count; ++i) {
        if (desc->reports[i].type != KSHIM_HID_REPORT_INPUT) continue;
        uint32_t n = (desc->reports[i].bits + 7U) / 8U + (desc->report_ids ? 1U : 0U);
        if (n > bytes) bytes = n;
    }
    return bytes;
}

static const char *describe(const struct hid_if *h)
{
    if (h->xinput) return "XInput gamepad";
    if (h->input.boot_keyboard) return "boot keyboard";
    if (h->input.boot_mouse) return "boot mouse";
    const struct kshim_hid_desc *d = &h->desc;
    if (kshim_hid_has_application(d, KSHIM_HID_USAGE(KSHIM_HID_PAGE_GENERIC_DESKTOP, KSHIM_HID_GD_GAMEPAD)) ||
        kshim_hid_has_application(d, KSHIM_HID_USAGE(KSHIM_HID_PAGE_GENERIC_DESKTOP, KSHIM_HID_GD_JOYSTICK)))
        return "gamepad";
    if (kshim_hid_has_application(d, KSHIM_HID_USAGE(KSHIM_HID_PAGE_GENERIC_DESKTOP, KSHIM_HID_GD_KEYBOARD)))
        return "keyboard";
    if (h->input.caps & KSHIM_HID_INPUT_POINTER) return "pointer";
    return "keys";
}

#if CONFIG_KSHIM_USB_TRACE
/* Stable mapping class for the trace (docs/usb-hid.md). */
static const char *map_kind(const struct hid_if *h)
{
    if (h->xinput) return "xinput";
    if (h->input.boot_keyboard) return "boot-keyboard";
    if (h->input.boot_mouse) return "boot-mouse";
    const struct kshim_hid_desc *d = &h->desc;
    if (kshim_hid_has_application(d, KSHIM_HID_USAGE(KSHIM_HID_PAGE_GENERIC_DESKTOP, KSHIM_HID_GD_GAMEPAD)))
        return "gamepad";
    if (kshim_hid_has_application(d, KSHIM_HID_USAGE(KSHIM_HID_PAGE_GENERIC_DESKTOP, KSHIM_HID_GD_JOYSTICK)))
        return "joystick";
    if (kshim_hid_has_application(d, KSHIM_HID_USAGE(KSHIM_HID_PAGE_GENERIC_DESKTOP, KSHIM_HID_GD_KEYBOARD)))
        return "keyboard";
    if (h->input.caps & KSHIM_HID_INPUT_POINTER) {
        const struct kshim_hid_item *x = kshim_hid_find(d, KSHIM_HID_REPORT_INPUT,
                                                        KSHIM_HID_USAGE(KSHIM_HID_PAGE_GENERIC_DESKTOP,
                                                                        KSHIM_HID_GD_X), -1);
        return x && (x->flags & KSHIM_HID_MAIN_RELATIVE) ? "mouse" : "tablet";
    }
    if (kshim_hid_has_application(d, KSHIM_HID_USAGE(KSHIM_HID_PAGE_CONSUMER, 0x01))) return "consumer";
    return "keys";
}
#endif

static int start(struct hid_if *h, uint32_t report_bytes)
{
    struct kshim_usb_interface *intf = h->intf;
    uint32_t length = h->ep->max_packet;
    if (report_bytes > length) length = report_bytes;
    if (length > HID_MAX_REPORT_BYTES) length = HID_MAX_REPORT_BYTES;
    if (!length) return KSHIM_IO_ERR_PROTOCOL;
    h->buffer = alloc_aligned((length + 63U) & ~63U, &h->raw);
    if (!h->buffer) return KSHIM_IO_ERR_NO_MEMORY;
    /* The transfer length equals the packet size unless a report is larger:
     * an interrupt transfer ends on its length or a short packet. */
    h->urb = (struct kshim_usb_urb){.device = h->dev, .endpoint = h->ep, .buffer = h->buffer,
                                    .length = length, .complete = report_complete, .context = h};
    intf->driver_data = h;
    int status = submit_report(h);
    if (status) {
        intf->driver_data = NULL;
        free(h->raw);
        return status;
    }
    source_add(h->input.caps);
    const char *caps = (h->input.caps & KSHIM_HID_INPUT_KEYS)
                           ? ((h->input.caps & KSHIM_HID_INPUT_POINTER) ? "keys+pointer" : "keys")
                           : "pointer";
    printf("HID %04x:%04x interface %u: %s (%s), interval %u%s%s\n", h->dev->descriptor.idVendor,
           h->dev->descriptor.idProduct, intf->number, describe(h), caps, h->ep->interval,
           h->quirk ? ", quirk " : "", h->quirk ? quirk_name(h->quirk) : "");
#if CONFIG_KSHIM_USB_TRACE
    kshim_usb_trace(h->dev, intf->number, "hid", "map=%s caps=%s ep=%02x len=%u report_bytes=%u ids=%u quirk=%s",
                    map_kind(h), caps, h->ep->address, (unsigned)length, (unsigned)report_bytes,
                    h->desc.report_ids ? 1U : 0U, quirk_name(h->quirk));
#endif
    return 0;
}

static uint16_t report_descriptor_length(const struct kshim_usb_interface *intf)
{
    const struct kshim_usb_device *dev = intf->device;
    const uint8_t *hid = kshim_usb_desc_find_in_interface(dev->config, dev->config_length,
                                                         intf->descriptor, KSHIM_USB_DT_HID);
    if (!hid || hid[0] < 9U) return 0;
    /* bNumDescriptors entries of (bDescriptorType, wDescriptorLength). */
    unsigned count = hid[5];
    for (unsigned i = 0; i < count && 6U + 3U * i + 2U < hid[0]; ++i)
        if (hid[6U + 3U * i] == KSHIM_USB_DT_REPORT) return le16(&hid[7U + 3U * i]);
    return 0;
}

/* The XInput layout, or a quirk's report descriptor, on an interface that
 * has no HID report descriptor of its own. */
static int layout_bind(struct kshim_usb_interface *intf, const struct kshim_hid_quirk *quirk, bool xinput)
{
    struct kshim_usb_endpoint *ep = kshim_usb_find_endpoint(intf, KSHIM_USB_XFER_INTERRUPT, true);
    if (!ep) {
        trace_unbound(intf, quirk, "no interrupt IN endpoint");
        return ERROR_NOT_SUPPORTED;
    }
    struct hid_if *h = calloc(1, sizeof(*h));
    if (!h) return KSHIM_IO_ERR_NO_MEMORY;
    h->intf = intf;
    h->dev = intf->device;
    h->ep = ep;
    h->xinput = xinput;
    h->quirk = quirk;
    const uint8_t *descriptor = xinput ? XInputReportDescriptor : quirk->report_descriptor;
    size_t length = xinput ? sizeof(XInputReportDescriptor) : quirk->report_descriptor_length;
    int status = kshim_hid_parse(descriptor, length, &h->desc);
    if (!status) status = kshim_hid_input_init(&h->input, &h->desc);
    if (!status) status = start(h, xinput ? XINPUT_REPORT_BYTES : max_input_bytes(&h->desc));
    if (status) {
        trace_unbound(intf, quirk, kshim_io_strerror(status));
        free(h);
    }
    return status;
}

static int hid_probe(struct kshim_usb_interface *intf)
{
    struct kshim_usb_device *dev = intf->device;
    const struct kshim_hid_quirk *quirk = quirk_of(intf);
    if (quirk && (quirk->flags & KSHIM_HID_QUIRK_IGNORE)) {
        trace_unbound(intf, quirk, "ignored by quirk");
        return ERROR_NOT_SUPPORTED;
    }
    if (quirk && (quirk->flags & KSHIM_HID_QUIRK_XINPUT)) return layout_bind(intf, quirk, true);
    struct kshim_usb_endpoint *ep = kshim_usb_find_endpoint(intf, KSHIM_USB_XFER_INTERRUPT, true);
    if (!ep) {
        trace_unbound(intf, quirk, "no interrupt IN endpoint");
        return ERROR_NOT_SUPPORTED;
    }
    struct hid_if *h = calloc(1, sizeof(*h));
    if (!h) return KSHIM_IO_ERR_NO_MEMORY;
    h->intf = intf;
    h->dev = dev;
    h->ep = ep;
    h->quirk = quirk;
    bool replaced = quirk && quirk->report_descriptor;
    int status = ERROR_NOT_SUPPORTED;
    uint16_t length = report_descriptor_length(intf);
    /* The device's own descriptor is read unless a quirk replaces it; the
     * trace always reads (and prints) it. */
    if (length && length <= HID_MAX_REPORT_DESCRIPTOR && (!replaced || CONFIG_KSHIM_USB_TRACE)) {
        uint8_t *report = malloc(length);
        uint32_t actual = 0;
        status = report ? kshim_usb_control(dev, HID_RT_STD_IN_INTERFACE, KSHIM_USB_REQ_GET_DESCRIPTOR,
                                            (uint16_t)(KSHIM_USB_DT_REPORT << 8), intf->number, report,
                                            length, HID_CONTROL_TIMEOUT_MS, &actual)
                        : KSHIM_IO_ERR_NO_MEMORY;
#if CONFIG_KSHIM_USB_TRACE
        if (!status) kshim_usb_trace_hex(dev, intf->number, "rdesc", report, actual);
        else kshim_usb_trace(dev, intf->number, "rdesc", "len=%u error=\"%s\"", length, kshim_io_strerror(status));
#endif
        if (!status && !replaced) status = kshim_hid_parse(report, actual, &h->desc);
        free(report);
    }
#if CONFIG_KSHIM_USB_TRACE
    else if (!length) {
        kshim_usb_trace(dev, intf->number, "rdesc", "len=0 error=\"no report descriptor in the HID descriptor\"");
    }
#endif
    if (replaced) status = kshim_hid_parse(quirk->report_descriptor, quirk->report_descriptor_length, &h->desc);
    if (!status) status = kshim_hid_input_init(&h->input, &h->desc);
    uint32_t report_bytes = status ? 0 : max_input_bytes(&h->desc);
    bool boot = intf->subclass == HID_SUBCLASS_BOOT &&
                (intf->protocol == HID_PROTOCOL_KEYBOARD || intf->protocol == HID_PROTOCOL_MOUSE);
    if (status && boot) {
        /* The boot layouts need the device in boot protocol. */
        printf("HID %04x:%04x interface %u: report descriptor unusable (%s), boot protocol\n",
               dev->descriptor.idVendor, dev->descriptor.idProduct, intf->number, kshim_io_strerror(status));
        status = kshim_usb_control(dev, HID_RT_CLASS_OUT_INTERFACE, HID_REQ_SET_PROTOCOL, HID_PROTOCOL_BOOT,
                                   intf->number, NULL, 0, HID_CONTROL_TIMEOUT_MS, NULL);
        if (!status) {
            kshim_hid_input_init_boot(&h->input, intf->protocol == HID_PROTOCOL_KEYBOARD);
            report_bytes = intf->protocol == HID_PROTOCOL_KEYBOARD ? 8U : 3U;
        }
    }
    if (status) {
        trace_unbound(intf, quirk, kshim_io_strerror(status));
        free(h);
        return status;
    }
    /* Report only on change; a STALL means the device does not support it. */
    (void)kshim_usb_control(dev, HID_RT_CLASS_OUT_INTERFACE, HID_REQ_SET_IDLE, 0, intf->number, NULL, 0,
                            HID_CONTROL_TIMEOUT_MS, NULL);
    status = start(h, report_bytes);
    if (status) {
        trace_unbound(intf, quirk, kshim_io_strerror(status));
        free(h);
    }
    return status;
}

static int xinput_probe(struct kshim_usb_interface *intf)
{
    const struct kshim_hid_quirk *quirk = quirk_of(intf);
    if (quirk && (quirk->flags & KSHIM_HID_QUIRK_IGNORE)) {
        trace_unbound(intf, quirk, "ignored by quirk");
        return ERROR_NOT_SUPPORTED;
    }
    return layout_bind(intf, quirk, true);
}

/* A vendor (non-HID) interface named by a quirk row with the XInput flag or
 * a report descriptor. HID interfaces are left to hid_probe. */
static int quirk_probe(struct kshim_usb_interface *intf)
{
    const struct kshim_hid_quirk *quirk = quirk_of(intf);
    if (!quirk || (quirk->flags & KSHIM_HID_QUIRK_IGNORE) || intf->class_code == KSHIM_USB_CLASS_HID)
        return ERROR_NOT_SUPPORTED;
    if (quirk->flags & KSHIM_HID_QUIRK_XINPUT) return layout_bind(intf, quirk, true);
    if (quirk->report_descriptor) return layout_bind(intf, quirk, false);
    return ERROR_NOT_SUPPORTED;
}

static void hid_disconnect(struct kshim_usb_interface *intf)
{
    struct hid_if *h = intf->driver_data;
    if (!h) return;
    h->stopped = true;
    if (h->urb_active) (void)kshim_usb_cancel_urb(&h->urb);
    /* Keys held on a device that went away must not stay pressed. */
    kshim_hid_input_reset(&h->input);
    intf->driver_data = NULL;
    free(h->raw);
    free(h);
}

static struct kshim_usb_class_driver HidDriver = {
    .name = "usb-hid",
    .class_code = KSHIM_USB_CLASS_HID,
    .subclass = KSHIM_USB_MATCH_ANY,
    .protocol = KSHIM_USB_MATCH_ANY,
    .probe = hid_probe,
    .disconnect = hid_disconnect,
    .service = hid_service,
};

static struct kshim_usb_class_driver XInputDriver = {
    .name = "xinput",
    .class_code = KSHIM_USB_CLASS_VENDOR,
    .subclass = XINPUT_SUBCLASS,
    .protocol = XINPUT_PROTOCOL_GAMEPAD,
    .probe = xinput_probe,
    .disconnect = hid_disconnect,
    .service = hid_service,
};

/* One class driver per quirk device with a vendor-interface layout, tried
 * before the generic drivers; its VID:PID match also makes the core count
 * the configuration as supported. */
#define HID_QUIRK_DRIVERS 8U
static struct kshim_usb_class_driver QuirkDrivers[HID_QUIRK_DRIVERS];

static int register_quirk_drivers(void)
{
    unsigned count = 0;
    for (const struct kshim_hid_quirk *q = kshim_hid_quirks; q->vendor; ++q) {
        if (!(q->flags & KSHIM_HID_QUIRK_XINPUT) && !q->report_descriptor) continue;
        bool known = false;
        for (unsigned i = 0; i < count; ++i)
            known |= QuirkDrivers[i].vendor == q->vendor && QuirkDrivers[i].product == q->product;
        if (known) continue;
        if (count == HID_QUIRK_DRIVERS) {
            printf("HID: quirk %s for %04x:%04x skipped (more than %u devices)\n", quirk_name(q), q->vendor,
                   q->product, HID_QUIRK_DRIVERS);
            continue;
        }
        QuirkDrivers[count] = (struct kshim_usb_class_driver){
            .name = "usb-hid-quirk", .class_code = KSHIM_USB_MATCH_ANY, .subclass = KSHIM_USB_MATCH_ANY,
            .protocol = KSHIM_USB_MATCH_ANY, .vendor = q->vendor, .product = q->product,
            .probe = quirk_probe, .disconnect = hid_disconnect, .service = hid_service,
        };
        int status = kshim_usb_class_register(&QuirkDrivers[count++]);
        if (status && status != KSHIM_IO_ERR_EXISTS) return status;
    }
    return 0;
}

int kshim_usb_hid_register(void)
{
    static bool quirks_registered;
    if (!quirks_registered) {
        quirks_registered = true;
        int quirks = register_quirk_drivers();
        if (quirks) return quirks;
    }
    int status = kshim_usb_class_register(&HidDriver);
    if (status && status != KSHIM_IO_ERR_EXISTS) return status;
    status = kshim_usb_class_register(&XInputDriver);
    return status == KSHIM_IO_ERR_EXISTS ? 0 : status;
}
