/* SPDX-License-Identifier: MIT */
/*
 * TEST ONLY (CONFIG_KSHIM_USB_SELFTEST): a class driver for the xHCI QEMU
 * test (tools/test_qemu_xhci.py). It binds HID interfaces (class 3) and
 * exercises the core end to end without the HID driver (owner usbhid):
 *  - a synchronous IN control transfer: GET_DESCRIPTOR(Report) (HID 1.11
 *    7.1.1, wIndex = interface), logged with its length and checksum;
 *  - SET_IDLE(0) (HID 1.11 7.2.4), whose STALL is tolerated;
 *  - an interrupt-IN URB resubmitted from its completion callback; reports
 *    are logged as they arrive.
 * It also binds mass-storage Bulk-Only interfaces (class 8, subclass 6,
 * protocol 0x50) and runs one SCSI INQUIRY over bulk OUT/IN (USB Mass
 * Storage Class Bulk-Only Transport 1.0: 31-byte CBW "USBC", data, 13-byte
 * CSW "USBS"; SPC INQUIRY opcode 0x12, 36 bytes).
 * kshim_usb_settle() then keeps pumping for up to
 * CONFIG_KSHIM_USB_SELFTEST_MS so the test can type keys (QMP send-key)
 * after "USB_SELFTEST armed".
 */
#include "usb_core.h"

#include <kshim/time.h>

#include <stdio.h>
#include <string.h>

#ifndef CONFIG_KSHIM_USB_SELFTEST_MS
#define CONFIG_KSHIM_USB_SELFTEST_MS 5000
#endif
#define SELFTEST_MAX 4U
#define SELFTEST_REPORTS_WANTED 2U

struct selftest_if {
    struct kshim_usb_interface *intf;
    struct kshim_usb_urb urb;
    uint8_t report[64];
    unsigned reports;
    bool active;
};

static struct selftest_if Interfaces[SELFTEST_MAX];
static unsigned TotalReports, Bound;

static void report_done(struct kshim_usb_urb *urb)
{
    struct selftest_if *s = urb->context;
    if (urb->status) {
        printf("USB_SELFTEST if%u report status %d\n", s->intf ? s->intf->number : 0U, urb->status);
        s->active = false;
        return;
    }
    ++s->reports;
    ++TotalReports;
    printf("USB_SELFTEST report if%u len=%u data=", s->intf->number, (unsigned)urb->actual);
    for (unsigned i = 0; i < urb->actual && i < sizeof(s->report); ++i) printf("%02x", s->report[i]);
    printf("\n");
    if (kshim_usb_submit_urb(urb)) s->active = false;
}

static int selftest_probe(struct kshim_usb_interface *intf)
{
    struct selftest_if *s = NULL;
    for (unsigned i = 0; i < SELFTEST_MAX; ++i)
        if (!Interfaces[i].intf) { s = &Interfaces[i]; break; }
    struct kshim_usb_endpoint *ep = kshim_usb_find_endpoint(intf, KSHIM_USB_XFER_INTERRUPT, true);
    if (!s || !ep) return ERROR_NOT_SUPPORTED;
    struct kshim_usb_device *dev = intf->device;
    const uint8_t *hid = kshim_usb_desc_find_in_interface(dev->config, dev->config_length,
                                                          intf->descriptor, KSHIM_USB_DT_HID);
    uint16_t report_len = hid && hid[0] >= 9 ? (uint16_t)(hid[7] | hid[8] << 8) : 0;
    uint8_t report_desc[256];
    uint32_t actual = 0;
    int status = ERROR_NOT_FOUND;
    if (report_len) {
        if (report_len > sizeof(report_desc)) report_len = sizeof(report_desc);
        status = kshim_usb_control(dev, KSHIM_USB_DIR_IN | KSHIM_USB_TYPE_STANDARD | KSHIM_USB_RECIP_INTERFACE,
                                   KSHIM_USB_REQ_GET_DESCRIPTOR, KSHIM_USB_DT_REPORT << 8, intf->number,
                                   report_desc, report_len, 1000, &actual);
    }
    uint32_t sum = 0;
    for (uint32_t i = 0; i < actual; ++i) sum = sum * 31U + report_desc[i];
    printf("USB_SELFTEST if%u report-descriptor status=%d len=%u sum=%08x\n", intf->number, status,
           (unsigned)actual, (unsigned)sum);
    int idle = kshim_usb_control(dev, KSHIM_USB_DIR_OUT | KSHIM_USB_TYPE_CLASS | KSHIM_USB_RECIP_INTERFACE,
                                 0x0a /* SET_IDLE */, 0, intf->number, NULL, 0, 1000, NULL);
    printf("USB_SELFTEST if%u set-idle status=%d\n", intf->number, idle);
    memset(s, 0, sizeof(*s));
    s->intf = intf;
    s->urb.device = dev;
    s->urb.endpoint = ep;
    s->urb.buffer = s->report;
    s->urb.length = ep->max_packet < sizeof(s->report) ? ep->max_packet : sizeof(s->report);
    s->urb.complete = report_done;
    s->urb.context = s;
    status = kshim_usb_submit_urb(&s->urb);
    if (status) {
        s->intf = NULL;
        return status;
    }
    s->active = true;
    intf->driver_data = s;
    ++Bound;
    return 0;
}

static void selftest_disconnect(struct kshim_usb_interface *intf)
{
    struct selftest_if *s = intf->driver_data;
    if (!s) return;
    s->active = false;
    (void)kshim_usb_cancel_urb(&s->urb);
    s->intf = NULL;
}

static void put32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}

static int bot_probe(struct kshim_usb_interface *intf)
{
    struct kshim_usb_device *dev = intf->device;
    struct kshim_usb_endpoint *out = kshim_usb_find_endpoint(intf, KSHIM_USB_XFER_BULK, false);
    struct kshim_usb_endpoint *in = kshim_usb_find_endpoint(intf, KSHIM_USB_XFER_BULK, true);
    if (!out || !in) return ERROR_NOT_SUPPORTED;
    uint8_t cbw[31] = {0}, data[36] = {0}, csw[13] = {0};
    put32(cbw, 0x43425355U);        /* "USBC" */
    put32(cbw + 4, 0x6b73686dU);    /* tag */
    put32(cbw + 8, sizeof(data));   /* dCBWDataTransferLength */
    cbw[12] = 0x80;                 /* data in */
    cbw[13] = 0;                    /* LUN */
    cbw[14] = 6;                    /* CDB length */
    cbw[15] = 0x12;                 /* INQUIRY */
    cbw[19] = sizeof(data);
    uint32_t sent = 0, got = 0, csw_len = 0;
    int status = kshim_usb_bulk(dev, out, cbw, sizeof(cbw), 2000, &sent);
    if (!status) status = kshim_usb_bulk(dev, in, data, sizeof(data), 2000, &got);
    if (!status) status = kshim_usb_bulk(dev, in, csw, sizeof(csw), 2000, &csw_len);
    bool csw_ok = !status && csw_len == sizeof(csw) && csw[0] == 'U' && csw[1] == 'S' &&
                  csw[2] == 'B' && csw[3] == 'S' && csw[12] == 0;
    char vendor[9], product[17];
    for (unsigned i = 0; i < 8; ++i) vendor[i] = data[8 + i] >= 0x20 && data[8 + i] < 0x7f ? (char)data[8 + i] : '.';
    for (unsigned i = 0; i < 16; ++i) product[i] = data[16 + i] >= 0x20 && data[16 + i] < 0x7f ? (char)data[16 + i] : '.';
    vendor[8] = product[16] = 0;
    printf("USB_SELFTEST bulk if%u status=%d out=%u in=%u csw=%s vendor=\"%s\" product=\"%s\"\n",
           intf->number, status, (unsigned)sent, (unsigned)got, csw_ok ? "ok" : "bad", vendor, product);
    return status ? status : (csw_ok ? 0 : KSHIM_IO_ERR_PROTOCOL);
}

static struct kshim_usb_class_driver BotDriver = {
    .name = "usb-selftest-bot",
    .class_code = 8,
    .subclass = 6,
    .protocol = 0x50,
    .probe = bot_probe,
};

static struct kshim_usb_class_driver SelftestDriver = {
    .name = "usb-selftest",
    .class_code = KSHIM_USB_CLASS_HID,
    .subclass = KSHIM_USB_MATCH_ANY,
    .protocol = KSHIM_USB_MATCH_ANY,
    .probe = selftest_probe,
    .disconnect = selftest_disconnect,
};

int kshim_usb_selftest_register(void)
{
    int status = kshim_usb_class_register(&SelftestDriver);
    int bot = kshim_usb_class_register(&BotDriver);
    return status ? status : bot;
}

void kshim_usb_selftest_settle(void)
{
    if (!Bound) {
        printf("USB_SELFTEST no HID interface bound\n");
        return;
    }
    printf("USB_SELFTEST armed interfaces=%u\n", Bound);
    uint64_t start = get_current_time();
    while (TotalReports < SELFTEST_REPORTS_WANTED &&
           get_current_time() - start < CONFIG_KSHIM_USB_SELFTEST_MS) {
        kshim_usb_poll();
        kshim_wait_event(get_current_time() + 5);
    }
    printf("USB_SELFTEST done reports=%u\n", TotalReports);
}
