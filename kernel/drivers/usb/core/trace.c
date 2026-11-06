/* SPDX-License-Identifier: MIT */
/*
 * USB bring-up trace (CONFIG_KSHIM_USB_TRACE, kshim/usb.h): stable,
 * line-oriented "usbtrace:" records of every enumerated device, so the
 * identity and report format of an unknown device (the AYANEO Pocket S2
 * internal gamepad behind pcie1 -> uPD720201) can be captured on its first
 * boot and turned into a mapping (docs/usb-hid.md).
 *
 * The core prints the device (dev), its strings (str), every interface
 * (if), the raw configuration descriptor (cfg) and, after class binding,
 * which driver took each interface (bind). An interface no driver took
 * that has an interrupt IN endpoint is read for the trace only (sniff):
 * its first distinct reports are printed and nothing is mapped. The HID
 * class driver adds the report descriptor (rdesc), the mapping decision
 * (hid) and its first distinct input reports (report).
 *
 * Facts used: USB 2.0 9.4.3 / 9.6.7 (GET_DESCRIPTOR(String), string
 * descriptor 0 holds the LANGID array, strings are UTF-16LE). Everything
 * runs in thread context: enumeration and URB callbacks
 * (kshim_usb_poll_events()).
 */
#include "usb_core.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef CONFIG_KSHIM_USB_TRACE_REPORTS
#define CONFIG_KSHIM_USB_TRACE_REPORTS 32
#endif

#define TRACE_LINE 512U
#define TRACE_SNIFFERS 8U
#define TRACE_STRING_CHARS 64U
#define TRACE_LANGID_EN_US 0x0409U

static int field(char *line, size_t size, size_t at, const char *format, ...)
    __attribute__((format(printf, 4, 5)));
static int field(char *line, size_t size, size_t at, const char *format, ...)
{
    if (at >= size) return 0;
    va_list args;
    va_start(args, format);
    int n = vsnprintf(line + at, size - at, format, args);
    va_end(args);
    if (n < 0) return 0;
    return (size_t)n < size - at ? n : (int)(size - at - 1U);
}

static size_t head(char *line, size_t size, const struct kshim_usb_device *device, int interface,
                   const char *event)
{
    char path[32];
    kshim_usb_device_path(device, path, sizeof(path));
    size_t at = (size_t)field(line, size, 0, KSHIM_USB_TRACE_PREFIX " %s path=%s", event, path);
    if (interface >= 0) at += (size_t)field(line, size, at, " if=%d", interface);
    return at;
}

void kshim_usb_trace(const struct kshim_usb_device *device, int interface, const char *event,
                     const char *format, ...)
{
    char line[TRACE_LINE];
    size_t at = head(line, sizeof(line), device, interface, event);
    if (format && *format && at + 1U < sizeof(line)) {
        line[at++] = ' ';
        va_list args;
        va_start(args, format);
        (void)vsnprintf(line + at, sizeof(line) - at, format, args);
        va_end(args);
    } else {
        line[at] = 0;
    }
    printf("%s\n", line);
}

static size_t hex(char *line, size_t size, size_t at, const uint8_t *data, size_t length)
{
    static const char digits[] = "0123456789abcdef";
    for (size_t i = 0; i < length && at + 2U < size; ++i) {
        line[at++] = digits[data[i] >> 4];
        line[at++] = digits[data[i] & 15U];
    }
    line[at] = 0;
    return at;
}

void kshim_usb_trace_hex(const struct kshim_usb_device *device, int interface, const char *event,
                         const uint8_t *data, size_t length)
{
    size_t shown = length > KSHIM_USB_TRACE_DESC_MAX ? KSHIM_USB_TRACE_DESC_MAX : length;
    if (!data) shown = 0;
    size_t off = 0;
    do {
        char line[TRACE_LINE];
        size_t n = shown - off > KSHIM_USB_TRACE_LINE_BYTES ? KSHIM_USB_TRACE_LINE_BYTES : shown - off;
        size_t at = head(line, sizeof(line), device, interface, event);
        at += (size_t)field(line, sizeof(line), at, " len=%u off=%03x hex=", (unsigned)length, (unsigned)off);
        at = hex(line, sizeof(line), at, data ? data + off : NULL, n);
        off += n;
        if (off == shown && shown < length) (void)field(line, sizeof(line), at, " trunc=1");
        printf("%s\n", line);
    } while (off < shown);
}

bool kshim_usb_trace_report(struct kshim_usb_trace_state *state, const struct kshim_usb_device *device,
                            uint8_t interface, const uint8_t *data, size_t length, const char *held)
{
    if (!state || !data) return false;
    size_t kept = length > KSHIM_USB_TRACE_REPORT_MAX ? KSHIM_USB_TRACE_REPORT_MAX : length;
    if (state->seq && state->last_length == length && !memcmp(state->last, data, kept)) return false;
    if (state->seq >= (unsigned)CONFIG_KSHIM_USB_TRACE_REPORTS) return false;
    ++state->seq;
    state->last_length = (uint16_t)length;
    memcpy(state->last, data, kept);
    char line[TRACE_LINE];
    size_t at = head(line, sizeof(line), device, interface, "report");
    at += (size_t)field(line, sizeof(line), at, " seq=%u len=%u hex=", state->seq, (unsigned)length);
    at = hex(line, sizeof(line), at, data, kept);
    if (kept < length) at += (size_t)field(line, sizeof(line), at, " trunc=1");
    (void)field(line, sizeof(line), at, " held=%s", held && *held ? held : "-");
    printf("%s\n", line);
    if (state->seq == (unsigned)CONFIG_KSHIM_USB_TRACE_REPORTS)
        kshim_usb_trace(device, interface, "report", "seq=%u limit=1", state->seq);
    return true;
}

/* ---- Device records -------------------------------------------------------- */

static const char *ep_type(uint8_t type)
{
    static const char *const names[] = {"ctrl", "isoc", "bulk", "int"};
    return names[type & 3U];
}

/* String descriptor `index` as printable ASCII (others become '?'). */
static bool read_string(struct kshim_usb_device *dev, uint8_t index, uint16_t langid, char *out,
                        size_t size)
{
    uint8_t raw[2U + 2U * TRACE_STRING_CHARS];
    uint32_t actual = 0;
    if (!index || kshim_usb_get_descriptor(dev, KSHIM_USB_DT_STRING, index, langid, raw, sizeof(raw), &actual) ||
        actual < 2U || raw[1] != KSHIM_USB_DT_STRING)
        return false;
    size_t n = 0, length = raw[0] < actual ? raw[0] : actual;
    for (size_t i = 2; i + 1U < length && n + 1U < size; i += 2) {
        uint16_t c = (uint16_t)(raw[i] | raw[i + 1U] << 8);
        out[n++] = c >= 0x20 && c < 0x7f && c != '"' ? (char)c : '?';
    }
    out[n] = 0;
    return true;
}

static void trace_strings(struct kshim_usb_device *dev)
{
    const struct kshim_usb_device_descriptor *d = &dev->descriptor;
    if (!d->iManufacturer && !d->iProduct) return;
    uint8_t langs[4];
    uint32_t actual = 0;
    uint16_t langid = TRACE_LANGID_EN_US;
    /* A STALL on string 0 means the device has no strings at all. */
    if (kshim_usb_get_descriptor(dev, KSHIM_USB_DT_STRING, 0, 0, langs, sizeof(langs), &actual) ||
        actual < 4U || langs[1] != KSHIM_USB_DT_STRING)
        return;
    langid = (uint16_t)(langs[2] | langs[3] << 8);
    static const struct { const char *kind; size_t offset; } which[] = {
        {"manufacturer", offsetof(struct kshim_usb_device_descriptor, iManufacturer)},
        {"product", offsetof(struct kshim_usb_device_descriptor, iProduct)},
    };
    for (unsigned i = 0; i < 2; ++i) {
        uint8_t index = ((const uint8_t *)d)[which[i].offset];
        char text[TRACE_STRING_CHARS + 1U];
        if (read_string(dev, index, langid, text, sizeof(text)))
            kshim_usb_trace(dev, -1, "str", "kind=%s index=%u lang=%04x text=\"%s\"", which[i].kind, index,
                            langid, text);
    }
}

void usb_trace_device(struct kshim_usb_device *dev)
{
    const struct kshim_usb_device_descriptor *d = &dev->descriptor;
    kshim_usb_trace(dev, -1, "dev",
                    "addr=%u speed=%s vid=%04x pid=%04x rel=%04x usb=%04x class=%02x/%02x/%02x mps0=%u "
                    "config=%u nconfig=%u ifaces=%u tier=%u route=%05x tt=%u/%u",
                    dev->address, kshim_usb_speed_name(dev->speed), d->idVendor, d->idProduct, d->bcdDevice,
                    d->bcdUSB, d->bDeviceClass, d->bDeviceSubClass, d->bDeviceProtocol, dev->ep0.max_packet,
                    dev->config_value, d->bNumConfigurations, dev->interface_count, dev->tier,
                    (unsigned)(dev->route & 0xfffffU), dev->tt_hub_slot, dev->tt_port);
    trace_strings(dev);
    for (unsigned i = 0; i < dev->interface_count; ++i) {
        const struct kshim_usb_interface *intf = &dev->interfaces[i];
        char eps[8U * 20U] = "-";
        size_t at = 0;
        for (unsigned j = 0; j < intf->endpoint_count; ++j) {
            const struct kshim_usb_endpoint *ep = &intf->endpoints[j];
            at += (size_t)field(eps, sizeof(eps), at, "%s%02x:%s:%u:%u", j ? "," : "", ep->address,
                                ep_type(ep->type), ep->max_packet, ep->interval);
        }
        kshim_usb_trace(dev, intf->number, "if", "alt=%u class=%02x/%02x/%02x eps=%s", intf->alternate,
                        intf->class_code, intf->subclass, intf->protocol, eps);
    }
    kshim_usb_trace_hex(dev, -1, "cfg", dev->config, dev->config_length);
}

/* ---- Unbound interfaces: read for the trace only ------------------------- */

struct sniffer {
    bool used, active;
    struct kshim_usb_device *dev;
    uint8_t interface;
    struct kshim_usb_urb urb;
    void *raw;
    uint8_t *buffer;
    struct kshim_usb_trace_state state;
};

static struct sniffer Sniffers[TRACE_SNIFFERS];

static void sniff_complete(struct kshim_usb_urb *urb)
{
    struct sniffer *s = urb->context;
    s->active = false;
    if (urb->status) {
        if (urb->status != KSHIM_IO_ERR_CANCELLED && urb->status != KSHIM_IO_ERR_NO_DEVICE &&
            urb->status != KSHIM_IO_ERR_QUIESCED)
            kshim_usb_trace(s->dev, s->interface, "sniff", "stopped=1 reason=\"%s\"",
                            kshim_io_strerror(urb->status));
        return;
    }
    if (urb->actual) (void)kshim_usb_trace_report(&s->state, s->dev, s->interface, s->buffer, urb->actual,
                                                  "unbound");
    /* Stop reading once the report budget is spent. */
    if (s->state.seq >= (unsigned)CONFIG_KSHIM_USB_TRACE_REPORTS) return;
    s->active = kshim_usb_submit_urb(&s->urb) == 0;
}

static struct sniffer *sniffer_of(const struct kshim_usb_device *dev, uint8_t interface)
{
    for (unsigned i = 0; i < TRACE_SNIFFERS; ++i)
        if (Sniffers[i].used && Sniffers[i].dev == dev && Sniffers[i].interface == interface) return &Sniffers[i];
    return NULL;
}

static void sniffer_stop(struct sniffer *s)
{
    if (s->active) (void)kshim_usb_cancel_urb(&s->urb);
    s->active = false;
    free(s->raw);
    memset(s, 0, sizeof(*s));
}

static void sniff(struct kshim_usb_device *dev, struct kshim_usb_interface *intf)
{
    if (sniffer_of(dev, intf->number)) return; /* already read (a later class registration) */
    struct kshim_usb_endpoint *ep = kshim_usb_find_endpoint(intf, KSHIM_USB_XFER_INTERRUPT, true);
    if (!ep || !ep->max_packet) return;
    struct sniffer *s = NULL;
    for (unsigned i = 0; i < TRACE_SNIFFERS && !s; ++i)
        if (!Sniffers[i].used) s = &Sniffers[i];
    if (!s) {
        kshim_usb_trace(dev, intf->number, "sniff", "ep=%02x skipped=1", ep->address);
        return;
    }
    uint32_t length = ep->max_packet;
    memset(s, 0, sizeof(*s));
    s->raw = calloc(1, length + 63U);
    if (!s->raw) return;
    s->buffer = (uint8_t *)(((uintptr_t)s->raw + 63U) & ~(uintptr_t)63U);
    s->used = true;
    s->dev = dev;
    s->interface = intf->number;
    s->urb = (struct kshim_usb_urb){.device = dev, .endpoint = ep, .buffer = s->buffer, .length = length,
                                    .complete = sniff_complete, .context = s};
    s->active = kshim_usb_submit_urb(&s->urb) == 0;
    kshim_usb_trace(dev, intf->number, "sniff", "ep=%02x len=%u started=%u", ep->address, (unsigned)length,
                    s->active ? 1U : 0U);
}

/* Also runs again when a class driver registers after the device was
 * configured (usb_core_bind_device()); then bind lines repeat and an
 * interface a driver took over stops being read for the trace. */
void usb_trace_bound(struct kshim_usb_device *dev)
{
    for (unsigned i = 0; i < dev->interface_count; ++i) {
        struct kshim_usb_interface *intf = &dev->interfaces[i];
        kshim_usb_trace(dev, intf->number, "bind", "class=%02x/%02x/%02x driver=%s", intf->class_code,
                        intf->subclass, intf->protocol,
                        intf->driver ? (intf->driver->name ? intf->driver->name : "?") : "none");
        struct sniffer *s = sniffer_of(dev, intf->number);
        if (intf->driver && s) sniffer_stop(s);
        else if (!intf->driver) sniff(dev, intf);
    }
}

void usb_trace_disconnect(struct kshim_usb_device *dev)
{
    for (unsigned i = 0; i < TRACE_SNIFFERS; ++i)
        if (Sniffers[i].used && Sniffers[i].dev == dev) sniffer_stop(&Sniffers[i]);
    kshim_usb_trace(dev, -1, "gone", "vid=%04x pid=%04x", dev->descriptor.idVendor, dev->descriptor.idProduct);
}
