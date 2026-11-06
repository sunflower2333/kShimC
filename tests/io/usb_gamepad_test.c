/* SPDX-License-Identifier: MIT */
/*
 * Full-stack gamepad test: everything between the PCI host and the boot
 * menu's key poll is kShimC code, run through kshim_io_init() and
 * kshim_io_quiesce() (kernel/core/io.c):
 *   PCI core + generic ECAM host (kernel/drivers/pci/), xhci_pci.c with the
 *   Renesas uPD720201 path (renesas_fw.c, firmware already running),
 *   xhci.c/xhci_ops.c, the USB core with the bring-up trace
 *   (CONFIG_KSHIM_USB_TRACE), the hub and HID/XInput class drivers with a
 *   test quirk table (usb_gamepad_quirks.c), the HID parser and mapper,
 *   input injection and kshim_key_source_poll() (kernel/drivers/keys.c).
 * Modelled: PCI configuration space (pci_model.h: the root port and the
 * uPD720201), the xHCI controller and its USB devices (xhci_model.c), the
 * GIC line of INTA (here), time. BAR0 accesses reach the xHCI model only
 * while the root port forwards and the function decodes its programmed
 * BAR (xhci_pci.c is compiled with xhci_io_mmio renamed to the routing
 * below); the model refuses every DMA access while bus mastering is off on
 * the function or the root port.
 *
 * Topology (Pocket S2 shaped, tests/fixtures/io/usb-gamepad.dts):
 *   00:00.0 root port -> 01:00.0 1912:0014, INTA -> GIC SPI 434
 *   xHCI root port 1: generic HID gamepad 1209:0007 (FS; X/Y, hat, 12 buttons)
 *   xHCI root port 2: high-speed hub, 5 ports, single TT
 *     1: Xbox 360 wired controller 045e:028e (FS, XInput, 4 vendor interfaces)
 *     2: HID joystick 1209:0009 (FS, report ID 3, 16-bit axes)
 *     3: HID boot keyboard 1209:000b (LS)
 *     4: vendor-class pad 1209:000a, mapped by a quirk report descriptor
 *     5: XInput clone 1209:000c on ff/ff/00 (quirk XINPUT) with a HID
 *        interface the quirk table ignores
 * Scenarios (one per process; the PCI and USB cores keep static state):
 *   irq       cold PCI state (COMMAND 0), halted controller, INTx-driven
 *   poll      the GIC refuses the line: the event ring is polled
 *   takeover  UEFI left BAR0, the bridge window and COMMAND (MEM|BME) set
 *             and the controller running with its own rings
 *   nohalt    takeover, but at handoff the controller never reports
 *             HCHalted: the handoff is refused and bus mastering stays off
 * Each checks enumeration and topology (route strings, TT), the trace
 * lines, menu actions from every device (hat, stick hysteresis and dead
 * zone, buttons, auto-repeat, XInput buttons/D-pad/stick, report IDs,
 * quirk layouts), unplug/re-plug below the hub and on a root port, and the
 * handoff state (xHCI halted and reset, interrupt freed, PCI config as
 * found: bus mastering off unless the previous owner left it on).
 */
#include "../../kernel/drivers/pci/pci_internal.h"
#include "../../kernel/drivers/usb/host/xhci.h"
#include "../../kernel/drivers/usb/host/renesas_fw.h"
#include "pci_model.h"
#include "xhci_model.h"

#include <kshim/dma.h>
#include <kshim/input.h>
#include <kshim/input_inject.h>
#include <kshim/io.h>
#include <kshim/irq.h>
#include <kshim/pci.h>
#include <kshim/smp.h>
#include <kshim/time.h>
#include <kshim/usb.h>
#include <qcom_dt.h>
#include <qcom_keys.h>

#include <stdlib.h>

enum scenario { IRQ, POLL, TAKEOVER, NOHALT };
static enum scenario Scenario;

#define ECAM_BASE 0x60000000UL
#define ECAM_SIZE 0x200000UL
#define XHCI_SPI KSHIM_IRQ_SPI(434)
#define BAR_SIZE 0x2000U  /* uPD720201 BAR0: 64-bit, 8 KiB */

/* Linux input codes (input-event-codes.h ABI values). */
enum { KEY_ENTER = 28, KEY_UP = 103, KEY_DOWN = 108, BTN_SOUTH = 0x130, BTN_EAST = 0x131,
       BTN_DPAD_UP = 0x220, BTN_DPAD_DOWN = 0x221 };

/* ---- Time, GIC and interrupt masking ------------------------------------------- */

static uint64_t NowUs = 1000000;
uint64_t kshim_smp_time_us(void) { return NowUs; }
uint64_t get_current_time(void) { return NowUs / 1000U; }
uint64_t delay(uint64_t us) { NowUs += us ? us : 1; return us; }

static struct {
    bool requested, enabled;
    unsigned trigger, requests, frees, delivered;
    kshim_irq_handler_t handler;
    void *context;
} Gic[1020];
static bool GicGrants = true, Woken, Deferred, InHandler;
static int Masked;
static int RootFn = -1, XhciFn = -1;

bool kshim_irq_ready(void) { return true; }

/* INTA as the GIC sees it: the controller's interrupter level, gated by the
 * function's INTx Disable. */
static bool intx_level(void)
{
    return xm_irq_line() && !(model_get(XhciFn, KSHIM_PCI_COMMAND, 2) & KSHIM_PCI_COMMAND_INTX_DISABLE);
}

static void sample_line(void)
{
    for (unsigned guard = 0; guard < 200; ++guard) {
        if (Masked || InHandler || !Gic[XHCI_SPI].requested || !Gic[XHCI_SPI].enabled || !intx_level())
            return;
        InHandler = true;
        Masked = 1;
        Gic[XHCI_SPI].handler(XHCI_SPI, Gic[XHCI_SPI].context);
        Masked = 0;
        InHandler = false;
        ++Gic[XHCI_SPI].delivered;
        Woken = true;
    }
    assert(!"INTx storm");
}

/* The model raises its interrupt: taken now unless the CPU masked IRQs. */
static void xhci_line(void *context)
{
    (void)context;
    if (Masked || InHandler) { Deferred = true; return; }
    sample_line();
}

unsigned long kshim_irq_save(void) { return (unsigned long)Masked++; }
void kshim_irq_restore(unsigned long flags)
{
    assert(Masked > 0 && (unsigned long)(Masked - 1) == flags);
    --Masked;
    if (!Masked && !InHandler && Deferred) {
        Deferred = false;
        sample_line();
    }
}

int kshim_irq_request(uint32_t intid, unsigned trigger, kshim_irq_handler_t handler, void *context)
{
    assert(intid < 1020 && handler);
    if (!GicGrants) return ERROR_NOT_SUPPORTED;
    assert(trigger == KSHIM_IRQ_TYPE_LEVEL_HIGH);
    if (Gic[intid].requested) return ERROR_BUSY;
    Gic[intid].requested = Gic[intid].enabled = true;
    Gic[intid].trigger = trigger;
    Gic[intid].handler = handler;
    Gic[intid].context = context;
    ++Gic[intid].requests;
    sample_line(); /* a level already asserted is taken at once */
    return 0;
}
int kshim_irq_free(uint32_t intid)
{
    if (!Gic[intid].requested) return ERROR_NOT_FOUND;
    Gic[intid].requested = Gic[intid].enabled = false;
    ++Gic[intid].frees;
    return 0;
}
void kshim_irq_enable(uint32_t intid) { Gic[intid].enabled = Gic[intid].requested; sample_line(); }
void kshim_irq_disable(uint32_t intid) { Gic[intid].enabled = false; }

void kshim_wait_event(uint64_t deadline_ms)
{
    Woken = false;
    sample_line();
    if (Woken) return;
    uint64_t deadline = deadline_ms * 1000U;
    NowUs = deadline > NowUs ? deadline : NowUs + 1U;
}

int kshim_resource_map(struct kshim_resources *r, uint64_t base, uint64_t size,
                       kshim_mmu_memory_type_t type, uint32_t permissions)
{
    (void)type; (void)permissions;
    if (r->mapping_count == KSHIM_RESOURCE_MAX) return -1;
    r->mappings[r->mapping_count++] = (kshim_mmu_region_t){base, size, type, permissions};
    return 0;
}

/* ---- PCI: ECAM and BAR0 routing ---------------------------------------------- */

static uint32_t ecam_read(uintptr_t a, unsigned width)
{
    assert(a >= ECAM_BASE && a < ECAM_BASE + ECAM_SIZE);
    uint64_t off = a - ECAM_BASE;
    return model_cfg_read((unsigned)(off >> 20), (uint8_t)(off >> 12), off & 0xfff, width);
}
static void ecam_write(uintptr_t a, unsigned width, uint32_t v)
{
    assert(a >= ECAM_BASE && a < ECAM_BASE + ECAM_SIZE);
    uint64_t off = a - ECAM_BASE;
    model_cfg_write((unsigned)(off >> 20), (uint8_t)(off >> 12), off & 0xfff, width, v);
}
static const struct kshim_pci_test_mmio Ecam = {ecam_read, ecam_write};
const struct kshim_pci_test_mmio *kshim_pci_test_mmio = &Ecam;

static unsigned DecodeMisses, MmioAfterHandoff;
static bool HandedOff;

static uint64_t bar0(void)
{
    return ((uint64_t)model_get(XhciFn, 0x14, 4) << 32 | model_get(XhciFn, 0x10, 4)) & ~(uint64_t)0xf;
}

/* A memory access reaches the function only through the root port's
 * memory window with both COMMAND.MEM bits set, and only at its BAR. */
static bool decoded(uintptr_t a)
{
    uint32_t window = model_get(RootFn, 0x20, 4);
    uint64_t base = (uint64_t)(window & 0xfff0U) << 16, limit = ((uint64_t)(window >> 16 & 0xfff0U) << 16) | 0xfffffU;
    uint64_t b = bar0();
    return (model_get(RootFn, KSHIM_PCI_COMMAND, 2) & KSHIM_PCI_COMMAND_MEMORY) &&
           (model_get(XhciFn, KSHIM_PCI_COMMAND, 2) & KSHIM_PCI_COMMAND_MEMORY) && a >= base && a <= limit &&
           b && a >= b && a < b + BAR_SIZE;
}
static uint32_t bar_read(void *context, uintptr_t a)
{
    if (HandedOff) ++MmioAfterHandoff;
    if (!decoded(a)) { ++DecodeMisses; return UINT32_MAX; }
    return xm_read32(context, XM_BASE + (a - (uintptr_t)bar0()));
}
static void bar_write(void *context, uintptr_t a, uint32_t v)
{
    if (HandedOff) ++MmioAfterHandoff;
    if (!decoded(a)) { ++DecodeMisses; return; }
    xm_write32(context, XM_BASE + (a - (uintptr_t)bar0()), v);
}
/* xhci_pci.c calls this instead of xhci_io_mmio() (tests/io/usb.cmake). */
void test_xhci_io_mmio(struct xhci_io *io);
void test_xhci_io_mmio(struct xhci_io *io)
{
    io->context = NULL;
    io->read32 = bar_read;
    io->write32 = bar_write;
}

static bool bus_master(void)
{
    return (model_get(XhciFn, KSHIM_PCI_COMMAND, 2) & KSHIM_PCI_COMMAND_MASTER) &&
           (model_get(RootFn, KSHIM_PCI_COMMAND, 2) & KSHIM_PCI_COMMAND_MASTER);
}

/* ---- Devices ---------------------------------------------------------------------- */

static void put16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }

static void device_desc(uint8_t *d, uint16_t usb, uint8_t cls, uint8_t mps0, uint16_t vid, uint16_t pid,
                        uint8_t imanufacturer, uint8_t iproduct)
{
    const uint8_t t[18] = {18, 1, 0, 0, cls, cls == 0xff ? 0xff : 0, cls == 0xff ? 0xff : 0, mps0, 0, 0, 0, 0,
                           0x00, 0x01, imanufacturer, iproduct, 0, 1};
    memcpy(d, t, 18);
    put16(&d[2], usb);
    put16(&d[8], vid);
    put16(&d[10], pid);
}

/* String descriptor (USB 2.0 9.6.7): UTF-16LE of ASCII text, or LANGID 0x0409. */
static void string_desc(uint8_t *d, const char *text)
{
    size_t n = text ? strlen(text) : 1;
    d[0] = (uint8_t)(2 + 2 * n);
    d[1] = 3;
    if (!text) { put16(&d[2], 0x0409); return; }
    for (size_t i = 0; i < n; ++i) put16(&d[2 + 2 * i], (uint8_t)text[i]);
}

/* Generic HID gamepad: X, Y (0..255), a hat (0..7, null state), 12 buttons;
 * 4-byte report = X, Y, hat | buttons 1-4 << 4, buttons 5-12. */
static const uint8_t PadReport[] = {
    0x05, 0x01, 0x09, 0x05, 0xa1, 0x01,
    0x15, 0x00, 0x26, 0xff, 0x00, 0x75, 0x08, 0x95, 0x02, 0x09, 0x30, 0x09, 0x31, 0x81, 0x02,
    0x25, 0x07, 0x46, 0x3b, 0x01, 0x65, 0x14, 0x75, 0x04, 0x95, 0x01, 0x09, 0x39, 0x81, 0x42,
    0x65, 0x00, 0x45, 0x00,
    0x05, 0x09, 0x19, 0x01, 0x29, 0x0c, 0x25, 0x01, 0x75, 0x01, 0x95, 0x0c, 0x81, 0x02,
    0xc0,
};
/* Joystick with report ID 3: X, Y signed 16-bit, buttons 1-4. */
static const uint8_t StickReport[] = {
    0x05, 0x01, 0x09, 0x04, 0xa1, 0x01, 0x85, 0x03,
    0x16, 0x00, 0x80, 0x26, 0xff, 0x7f, 0x75, 0x10, 0x95, 0x02, 0x09, 0x30, 0x09, 0x31, 0x81, 0x02,
    0x05, 0x09, 0x19, 0x01, 0x29, 0x04, 0x15, 0x00, 0x25, 0x01, 0x75, 0x01, 0x95, 0x04, 0x81, 0x02,
    0x75, 0x04, 0x95, 0x01, 0x81, 0x01,
    0xc0,
};
/* Boot keyboard (the QEMU usb-kbd report descriptor, as usbhid_stack_test). */
static const uint8_t KbdReport[] = {
    0x05, 0x01, 0x09, 0x06, 0xa1, 0x01, 0x75, 0x01, 0x95, 0x08, 0x05, 0x07, 0x19, 0xe0, 0x29, 0xe7,
    0x15, 0x00, 0x25, 0x01, 0x81, 0x02, 0x95, 0x01, 0x75, 0x08, 0x81, 0x01, 0x95, 0x05, 0x75, 0x01,
    0x05, 0x08, 0x19, 0x01, 0x29, 0x05, 0x91, 0x02, 0x95, 0x01, 0x75, 0x03, 0x91, 0x01, 0x95, 0x06,
    0x75, 0x08, 0x15, 0x00, 0x25, 0xff, 0x05, 0x07, 0x19, 0x00, 0x29, 0xff, 0x81, 0x00, 0xc0,
};
/* Consumer control (the clone's ignored interface). */
static const uint8_t ConsumerReport[] = {
    0x05, 0x0c, 0x09, 0x01, 0xa1, 0x01, 0x15, 0x00, 0x25, 0x01, 0x75, 0x01, 0x95, 0x02,
    0x09, 0xe9, 0x09, 0xea, 0x81, 0x02, 0x95, 0x06, 0x81, 0x01, 0xc0,
};

/* Xbox 360 wired controller 045e:028e: the configuration layout of the
 * commonly published `lsusb -v` dump (153 bytes, 4 vendor interfaces:
 * ff/5d/01 gamepad with interrupt IN 0x81 and OUT 0x01, ff/5d/03 headset,
 * ff/5d/02, ff/fd/13 security). The undocumented class descriptors
 * (types 0x21, 0x22... with 0x41) are reproduced for structure; kShimC
 * does not interpret them. Facts only. */
static const uint8_t XboxCfg[153] = {
    9, 2, 153, 0, 4, 1, 0, 0xa0, 0xfa,
    9, 4, 0, 0, 2, 0xff, 0x5d, 0x01, 0,
    0x11, 0x21, 0x00, 0x01, 0x01, 0x25, 0x81, 0x14, 0x00, 0x00, 0x00, 0x00, 0x13, 0x01, 0x08, 0x00, 0x00,
    7, 5, 0x81, 3, 0x20, 0, 4,
    7, 5, 0x01, 3, 0x20, 0, 8,
    9, 4, 1, 0, 4, 0xff, 0x5d, 0x03, 0,
    0x1b, 0x21, 0x00, 0x01, 0x01, 0x01, 0x82, 0x40, 0x01, 0x02, 0x20, 0x16, 0x83, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x16, 0x03, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    7, 5, 0x82, 3, 0x20, 0, 2,
    7, 5, 0x02, 3, 0x20, 0, 4,
    7, 5, 0x83, 3, 0x20, 0, 64,
    7, 5, 0x03, 3, 0x20, 0, 16,
    9, 4, 2, 0, 1, 0xff, 0x5d, 0x02, 0,
    0x09, 0x21, 0x00, 0x01, 0x01, 0x22, 0x84, 0x07, 0x00,
    7, 5, 0x84, 3, 0x20, 0, 16,
    9, 4, 3, 0, 0, 0xff, 0xfd, 0x13, 4,
    0x06, 0x41, 0x00, 0x01, 0x01, 0x03,
};

static uint8_t PadDev[18], StickDev[18], KbdDev[18], VendorDev[18], CloneDev[18], XboxDev[18];
static uint8_t PadCfg[34], StickCfg[34], KbdCfg[34], VendorCfg[25], CloneCfg[64];
static uint8_t Lang[4], XboxMaker[64], XboxProduct[32], PadProduct[32];

static void hid_config(uint8_t *c, uint8_t sub, uint8_t proto, uint16_t report_length, uint8_t mps, uint8_t interval)
{
    const uint8_t t[34] = {9, 2, 34, 0, 1, 1, 0, 0xa0, 50,
                           9, 4, 0, 0, 1, 3, sub, proto, 0,
                           9, 0x21, 0x11, 0x01, 0, 1, 0x22, 0, 0,
                           7, 5, 0x81, 3, mps, 0, interval};
    memcpy(c, t, 34);
    put16(&c[25], report_length);
}

static struct xm_dev Pad, Hub, Xbox, Stick, Kbd, Vendor, Clone;
static unsigned XboxIdx, StickIdx, KbdIdx, VendorIdx, CloneIdx;

static void make_devices(void)
{
    device_desc(PadDev, 0x0200, 0, 8, 0x1209, 0x0007, 0, 2);
    hid_config(PadCfg, 0, 0, sizeof(PadReport), 8, 4);
    string_desc(Lang, NULL);
    string_desc(PadProduct, "Test Pad");
    memset(&Pad, 0, sizeof(Pad));
    Pad.speed = 1;
    Pad.dev_desc = PadDev; Pad.dev_len = 18;
    Pad.cfg_desc = PadCfg; Pad.cfg_len = sizeof(PadCfg);
    Pad.report_desc = PadReport; Pad.report_len = sizeof(PadReport);
    Pad.strings[0] = Lang; Pad.strings[2] = PadProduct;
    Pad.stall_set_idle = true;          /* many pads: SET_IDLE unsupported */

    xm_make_hub(&Hub, 3, 5);

    device_desc(XboxDev, 0x0200, 0xff, 8, 0x045e, 0x028e, 1, 2);
    put16(&XboxDev[12], 0x0114);
    string_desc(XboxMaker, "\xa9Microsoft Corporation");
    string_desc(XboxProduct, "Controller");
    memset(&Xbox, 0, sizeof(Xbox));
    Xbox.speed = 1;
    Xbox.dev_desc = XboxDev; Xbox.dev_len = 18;
    Xbox.cfg_desc = XboxCfg; Xbox.cfg_len = sizeof(XboxCfg);
    Xbox.strings[0] = Lang; Xbox.strings[1] = XboxMaker; Xbox.strings[2] = XboxProduct;

    device_desc(StickDev, 0x0110, 0, 8, 0x1209, 0x0009, 0, 0);
    hid_config(StickCfg, 0, 0, sizeof(StickReport), 8, 10);
    memset(&Stick, 0, sizeof(Stick));
    Stick.speed = 1;
    Stick.dev_desc = StickDev; Stick.dev_len = 18;
    Stick.cfg_desc = StickCfg; Stick.cfg_len = sizeof(StickCfg);
    Stick.report_desc = StickReport; Stick.report_len = sizeof(StickReport);

    device_desc(KbdDev, 0x0110, 0, 8, 0x1209, 0x000b, 0, 0);
    hid_config(KbdCfg, 1, 1, sizeof(KbdReport), 8, 10);
    memset(&Kbd, 0, sizeof(Kbd));
    Kbd.speed = 2;                      /* low speed behind the HS hub: TT */
    Kbd.dev_desc = KbdDev; Kbd.dev_len = 18;
    Kbd.cfg_desc = KbdCfg; Kbd.cfg_len = sizeof(KbdCfg);
    Kbd.report_desc = KbdReport; Kbd.report_len = sizeof(KbdReport);

    device_desc(VendorDev, 0x0200, 0, 8, 0x1209, 0x000a, 0, 0);
    const uint8_t vc[25] = {9, 2, 25, 0, 1, 1, 0, 0xa0, 50, 9, 4, 0, 0, 1, 0xff, 0x00, 0x00, 0,
                            7, 5, 0x81, 3, 8, 0, 4};
    memcpy(VendorCfg, vc, sizeof(vc));
    memset(&Vendor, 0, sizeof(Vendor));
    Vendor.speed = 1;
    Vendor.dev_desc = VendorDev; Vendor.dev_len = 18;
    Vendor.cfg_desc = VendorCfg; Vendor.cfg_len = sizeof(VendorCfg);

    device_desc(CloneDev, 0x0200, 0xff, 8, 0x1209, 0x000c, 0, 0);
    const uint8_t cc[57] = {9, 2, 57, 0, 2, 1, 0, 0xa0, 50,
                            9, 4, 0, 0, 2, 0xff, 0xff, 0x00, 0,
                            7, 5, 0x81, 3, 0x20, 0, 4,
                            7, 5, 0x01, 3, 0x20, 0, 8,
                            9, 4, 1, 0, 1, 3, 0, 0, 0,
                            9, 0x21, 0x11, 0x01, 0, 1, 0x22, sizeof(ConsumerReport), 0,
                            7, 5, 0x82, 3, 8, 0, 10};
    memcpy(CloneCfg, cc, sizeof(cc));
    memset(&Clone, 0, sizeof(Clone));
    Clone.speed = 1;
    Clone.dev_desc = CloneDev; Clone.dev_len = 18;
    Clone.cfg_desc = CloneCfg; Clone.cfg_len = sizeof(cc);
    Clone.if_report[1] = ConsumerReport; Clone.if_report_len[1] = sizeof(ConsumerReport);
}

/* ---- Log capture -------------------------------------------------------------- */

static char LogPath[128];

static char *log_text(void)
{
    fflush(stdout);
    FILE *f = fopen(LogPath, "rb");
    assert(f);
    static char *text;
    free(text);
    assert(!fseek(f, 0, SEEK_END));
    long n = ftell(f);
    rewind(f);
    text = calloc(1, (size_t)n + 1);
    assert(text && fread(text, 1, (size_t)n, f) == (size_t)n);
    fclose(f);
    return text;
}
static unsigned logged(const char *needle)
{
    unsigned n = 0;
    for (const char *at = strstr(log_text(), needle); at; at = strstr(at + 1, needle)) ++n;
    return n;
}
static void expect_log(const char *needle)
{
    if (!logged(needle)) {
        fprintf(stderr, "missing log line: %s\n", needle);
        assert(0);
    }
}

/* ---- Menu side ---------------------------------------------------------------- */

static QcomKeys PlatformKeys;

static bool next_key(kshim_key_event_t *e)
{
    return kshim_key_source_poll(QcomKeysSource(&PlatformKeys), e) == KSHIM_KEYS_EVENT;
}
static void expect_key(int type, int action, uint32_t code, uint8_t pressed)
{
    kshim_key_event_t e = {0};
    bool got = false;
    for (unsigned i = 0; i < 8 && !(got = next_key(&e)); ++i) (void)0;
    if (!got || e.type != (kshim_key_event_type_t)type || e.action != (kshim_key_action_t)action ||
        e.code != code || e.pressed != pressed) {
        fprintf(stderr, "key %s %d/%d/%#x/%u, expected %d/%d/%#x/%u\n", got ? "" : "(none)", e.type, e.action,
                (unsigned)e.code, e.pressed, type, action, (unsigned)code, pressed);
        assert(0);
    }
}
static void press(int action, uint32_t code) { expect_key(KSHIM_KEY_EVENT_PRESS, action, code, 1); }
static void release(int action, uint32_t code) { expect_key(KSHIM_KEY_EVENT_RELEASE, action, code, 0); }
static void expect_no_key(void)
{
    kshim_key_event_t e;
    for (unsigned i = 0; i < 4; ++i)
        if (next_key(&e)) {
            fprintf(stderr, "unexpected key %d/%d/%#x\n", e.type, e.action, (unsigned)e.code);
            assert(0);
        }
}
/* Menu loop stand-in: the key poll pumps kshim_input_service() for `ms`. */
static void run_ms(unsigned ms)
{
    uint64_t end = NowUs + (uint64_t)ms * 1000U;
    kshim_key_event_t e;
    while (NowUs < end) {
        kshim_input_service();
        NowUs += 1000U;
    }
    (void)e;
}

static void pad(uint8_t x, uint8_t y, uint8_t hat, uint16_t buttons)
{
    uint8_t r[4] = {x, y, (uint8_t)((hat & 0xfU) | (buttons & 0xfU) << 4), (uint8_t)(buttons >> 4)};
    xm_queue_report(1, 0x81, r, sizeof(r));
}
static void xinput(unsigned index, uint8_t dpad, uint8_t buttons, int16_t ly)
{
    uint8_t r[20] = {0x00, 0x14, dpad, buttons};
    put16(&r[8], (uint16_t)ly);
    xm_queue_report(index, 0x81, r, sizeof(r));
}
static void stick(int16_t x, int16_t y, uint8_t buttons)
{
    uint8_t r[6] = {3, 0, 0, 0, 0, buttons};
    put16(&r[1], (uint16_t)x);
    put16(&r[3], (uint16_t)y);
    xm_queue_report(StickIdx, 0x81, r, sizeof(r));
}

/* ---- Checks ------------------------------------------------------------------------ */

static struct xhci *Xhci;

static void check_topology(void)
{
    struct kshim_pci_dev *dev = kshim_pci_find_device(0x1912, 0x0014, NULL);
    assert(dev && dev->driver_data && dev->bus == 1);
    Xhci = dev->driver_data; /* struct xhci_pci starts with its struct xhci */
    assert(Xhci->running && !Xhci->dead);
    uint16_t cmd = (uint16_t)model_get(XhciFn, KSHIM_PCI_COMMAND, 2);
    assert((cmd & (KSHIM_PCI_COMMAND_MEMORY | KSHIM_PCI_COMMAND_MASTER)) ==
           (KSHIM_PCI_COMMAND_MEMORY | KSHIM_PCI_COMMAND_MASTER));
    assert(bar0() >= 0x40300000U && bar0() < 0x40300000U + 0x1fd00000U && DecodeMisses == 0);
    if (Scenario == POLL) {
        assert(!Xhci->irq_enabled && !Gic[XHCI_SPI].requested && Xm.irqs_raised == 0);
    } else {
        assert(Xhci->irq_enabled && Gic[XHCI_SPI].requested && Gic[XHCI_SPI].delivered > 0);
        assert(!(cmd & KSHIM_PCI_COMMAND_INTX_DISABLE));
    }
    if (Scenario >= TAKEOVER) assert(Xhci->inherited_running && Xm.stop_count >= 1 && Xm.hi_at_reset == 0);

    /* xHCI slots: route strings and the TT of the high-speed hub. */
    struct xm_slot *pad_slot = xm_slot_of(1), *hub_slot = xm_slot_of(2);
    assert(pad_slot && hub_slot && pad_slot->route == 0 && hub_slot->route == 0);
    assert(hub_slot->hub && hub_slot->hub_ports == 5 && Xm.devs[2].port_resets == 5);
    struct xm_slot *xbox = xm_slot_of(XboxIdx), *kbd = xm_slot_of(KbdIdx), *stick_slot = xm_slot_of(StickIdx);
    struct xm_slot *vendor = xm_slot_of(VendorIdx), *clone = xm_slot_of(CloneIdx);
    assert(xbox && kbd && stick_slot && vendor && clone && Xm.tt_errors == 0);
    unsigned hub_id = (unsigned)(hub_slot - Xm.slots);
    assert(xbox->route == 0x1 && xbox->tt_slot == hub_id && xbox->tt_port == 1);
    assert(stick_slot->route == 0x2 && kbd->route == 0x3 && kbd->speed == 2 && kbd->tt_port == 3);
    assert(vendor->route == 0x4 && clone->route == 0x5);
    /* Periodic intervals (xHCI 6.2.3.6): FS/LS bInterval frames ->
     * 2^Interval microframes rounded down, also behind the hub; HS
     * 2^(bInterval-1). */
    assert(xbox->ep[3].interval == 5 && kbd->ep[3].interval == 6 && stick_slot->ep[3].interval == 6);
    assert(pad_slot->ep[3].interval == 5 && hub_slot->ep[3].interval == 11);
    /* Every interface's endpoints configured, incl. the Xbox headset's. */
    assert(xbox->ep[3].enabled && xbox->ep[3].type == 7 && xbox->ep[2].enabled && xbox->ep[2].type == 3);
    assert(xbox->ep[5].enabled && xbox->ep[9].enabled);
    assert(Xm.devs[XboxIdx].config == 1 && Xm.devs[1].config == 1 && Xm.dma_blocked == 0);
    /* HID setup: SET_IDLE (the pad stalls it), boot keyboard parsed, no SET_PROTOCOL. */
    assert(Xm.devs[StickIdx].set_idles == 1 && Xm.devs[1].set_idles == 0 && Xm.devs[KbdIdx].set_protocols == 0);
    assert(Xm.devs[XboxIdx].string_reads >= 3);
}

static void check_trace(void)
{
    /* Device, strings, interfaces, configuration. */
    expect_log("usbtrace: dev path=0-1 addr=");
    expect_log("speed=full vid=1209 pid=0007 rel=0100 usb=0200 class=00/00/00 mps0=8 config=1 nconfig=1 ifaces=1 "
               "tier=1 route=00000 tt=0/0");
    expect_log("usbtrace: str path=0-1 kind=product index=2 lang=0409 text=\"Test Pad\"");
    expect_log("usbtrace: if path=0-1 if=0 alt=0 class=03/00/00 eps=81:int:8:4");
    expect_log("usbtrace: cfg path=0-1 len=34 off=000 hex=09022200010100a0320904000001030000000921110100012237000705810308");
    expect_log("usbtrace: cfg path=0-1 len=34 off=020 hex=0004");
    expect_log("usbtrace: rdesc path=0-1 if=0 len=55 off=000 hex=05010905a1011500");
    expect_log("usbtrace: rdesc path=0-1 if=0 len=55 off=020 hex=093981426500450005091901290c25017501950c8102c0");
    expect_log("usbtrace: hid path=0-1 if=0 map=gamepad caps=keys ep=81 len=8 report_bytes=4 ids=0 quirk=none");
    expect_log("usbtrace: bind path=0-1 if=0 class=03/00/00 driver=usb-hid");
    expect_log("usbtrace: dev path=0-2 addr=");
    expect_log("speed=high vid=0409 pid=555a");
    expect_log("usbtrace: bind path=0-2 if=0 class=09/00/01 driver=usb-hub");
    /* The Xbox 360 pad behind the hub: TT, strings, all four interfaces. */
    char tt[160];
    snprintf(tt, sizeof(tt), "vid=045e pid=028e rel=0114 usb=0200 class=ff/ff/ff mps0=8 config=1 nconfig=1 ifaces=4 "
             "tier=2 route=00001 tt=%u/1", (unsigned)(xm_slot_of(2) - Xm.slots));
    expect_log(tt);
    expect_log("usbtrace: str path=0-2.1 kind=manufacturer index=1 lang=0409 text=\"?Microsoft Corporation\"");
    expect_log("usbtrace: str path=0-2.1 kind=product index=2 lang=0409 text=\"Controller\"");
    expect_log("usbtrace: if path=0-2.1 if=0 alt=0 class=ff/5d/01 eps=81:int:32:4,01:int:32:8");
    expect_log("usbtrace: if path=0-2.1 if=1 alt=0 class=ff/5d/03 eps=82:int:32:2,02:int:32:4,83:int:32:64,03:int:32:16");
    expect_log("usbtrace: if path=0-2.1 if=3 alt=0 class=ff/fd/13 eps=-");
    expect_log("usbtrace: cfg path=0-2.1 len=153 off=080 hex=");
    expect_log("usbtrace: hid path=0-2.1 if=0 map=xinput caps=keys ep=81 len=32 report_bytes=20 ids=0 quirk=none");
    expect_log("usbtrace: bind path=0-2.1 if=0 class=ff/5d/01 driver=xinput");
    expect_log("usbtrace: bind path=0-2.1 if=1 class=ff/5d/03 driver=none");
    expect_log("usbtrace: sniff path=0-2.1 if=1 ep=82 len=32 started=1");
    expect_log("usbtrace: sniff path=0-2.1 if=2 ep=84 len=32 started=1");
    expect_log("usbtrace: bind path=0-2.1 if=3 class=ff/fd/13 driver=none");
    /* Joystick (report ID), keyboard (low speed, TT port 3), quirk devices. */
    expect_log("usbtrace: hid path=0-2.2 if=0 map=joystick caps=keys ep=81 len=8 report_bytes=6 ids=1 quirk=none");
    expect_log("speed=low vid=1209 pid=000b");
    expect_log("tier=2 route=00003 tt=");
    expect_log("usbtrace: hid path=0-2.3 if=0 map=keyboard caps=keys");
    expect_log("usbtrace: bind path=0-2.4 if=0 class=ff/00/00 driver=usb-hid-quirk");
    expect_log("usbtrace: hid path=0-2.4 if=0 map=gamepad caps=keys ep=81 len=8 report_bytes=2 ids=0 "
               "quirk=test-vendor-pad");
    expect_log("usbtrace: hid path=0-2.5 if=0 map=xinput caps=keys ep=81 len=32 report_bytes=20 ids=0 "
               "quirk=test-xinput-clone");
    expect_log("usbtrace: hid path=0-2.5 if=1 map=unbound quirk=test-ignore reason=\"ignored by quirk\"");
    expect_log("usbtrace: bind path=0-2.5 if=1 class=03/00/00 driver=none");
    expect_log("usbtrace: sniff path=0-2.5 if=1 ep=82 len=8 started=1");
    /* No report descriptor was dumped for the ignored interface. */
    assert(!logged("usbtrace: rdesc path=0-2.5 if=1"));
}

static void check_menu(void)
{
    kshim_input_inject_set_surface(320, 240);
    expect_no_key();
    /* Generic pad: the idle report changes nothing; hat N/S; null releases. */
    pad(0x80, 0x80, 8, 0);
    expect_no_key();
    pad(0x80, 0x80, 0, 0);
    press(KSHIM_KEY_ACTION_UP, BTN_DPAD_UP);
    pad(0x80, 0x80, 8, 0);
    release(KSHIM_KEY_ACTION_UP, BTN_DPAD_UP);
    pad(0x80, 0x80, 3, 0); /* SE */
    press(KSHIM_KEY_ACTION_DOWN, BTN_DPAD_DOWN);
    pad(0x80, 0x80, 8, 0);
    release(KSHIM_KEY_ACTION_DOWN, BTN_DPAD_DOWN);
    /* Stick Y: dead zone and hysteresis (active past 50 %, held to 25 %). */
    pad(0x80, 0xb0, 8, 0);          /* +37 %: inside the activation band */
    expect_no_key();
    pad(0x80, 0xff, 8, 0);          /* +100 % */
    press(KSHIM_KEY_ACTION_DOWN, BTN_DPAD_DOWN);
    pad(0x80, 0xb0, 8, 0);          /* back to +37 %: still held */
    expect_no_key();
    pad(0x80, 0x98, 8, 0);          /* +19 %: inside the dead zone */
    release(KSHIM_KEY_ACTION_DOWN, BTN_DPAD_DOWN);
    pad(0x00, 0x80, 8, 0);          /* X is not mapped */
    pad(0xff, 0x80, 8, 0);
    expect_no_key();
    pad(0x80, 0x00, 8, 0);          /* -100 %: UP */
    press(KSHIM_KEY_ACTION_UP, BTN_DPAD_UP);
    /* Held direction: repeat after 500 ms, then every 150 ms. */
    run_ms(505);
    expect_key(KSHIM_KEY_EVENT_PRESS, KSHIM_KEY_ACTION_UP, BTN_DPAD_UP, 2);
    run_ms(150);
    expect_key(KSHIM_KEY_EVENT_PRESS, KSHIM_KEY_ACTION_UP, BTN_DPAD_UP, 2);
    pad(0x80, 0x80, 8, 0);
    release(KSHIM_KEY_ACTION_UP, BTN_DPAD_UP);
    /* Buttons 1 and 2; button 3 does nothing. */
    pad(0x80, 0x80, 8, 0x1);
    press(KSHIM_KEY_ACTION_SELECT, BTN_SOUTH);
    pad(0x80, 0x80, 8, 0x2);
    release(KSHIM_KEY_ACTION_SELECT, BTN_SOUTH);
    press(KSHIM_KEY_ACTION_BACK, BTN_EAST);
    pad(0x80, 0x80, 8, 0x4);
    release(KSHIM_KEY_ACTION_BACK, BTN_EAST);
    pad(0x80, 0x80, 8, 0);
    expect_no_key();

    /* Xbox 360 pad behind the hub: its non-input messages (LED state,
     * type 0x01; 0x02/0x03 status, as these pads send after connecting)
     * map nothing; then A, B, D-pad down, left stick up. */
    static const uint8_t led[3] = {0x01, 0x03, 0x0e}, st2[3] = {0x02, 0x03, 0x00}, st3[3] = {0x03, 0x03, 0x03};
    xm_queue_report(XboxIdx, 0x81, led, 3);
    xm_queue_report(XboxIdx, 0x81, st2, 3);
    xm_queue_report(XboxIdx, 0x81, st3, 3);
    run_ms(2);
    expect_no_key();
    expect_log("usbtrace: report path=0-2.1 if=0 seq=1 len=3 hex=01030e held=-");
    xinput(XboxIdx, 0, 0x10, 0);
    press(KSHIM_KEY_ACTION_SELECT, BTN_SOUTH);
    xinput(XboxIdx, 0, 0x20, 0);
    release(KSHIM_KEY_ACTION_SELECT, BTN_SOUTH);
    press(KSHIM_KEY_ACTION_BACK, BTN_EAST);
    xinput(XboxIdx, 0x02, 0, 0);
    release(KSHIM_KEY_ACTION_BACK, BTN_EAST);
    press(KSHIM_KEY_ACTION_DOWN, BTN_DPAD_DOWN);
    xinput(XboxIdx, 0, 0, 32767);   /* left stick fully up (XInput Y is positive up) */
    release(KSHIM_KEY_ACTION_DOWN, BTN_DPAD_DOWN);
    press(KSHIM_KEY_ACTION_UP, BTN_DPAD_UP);
    xinput(XboxIdx, 0, 0, 0);
    release(KSHIM_KEY_ACTION_UP, BTN_DPAD_UP);
    /* Its headset interface (unbound) is only read for the trace. */
    uint8_t headset[3] = {0x01, 0x03, 0x00};
    xm_queue_report(XboxIdx, 0x82, headset, sizeof(headset));
    run_ms(2);
    expect_no_key();
    expect_log("usbtrace: report path=0-2.1 if=1 seq=1 len=3 hex=010300 held=unbound");

    /* Joystick with report ID 3, signed 16-bit Y: +61 % / +30 % / +15 %. */
    stick(0, 20000, 0);
    press(KSHIM_KEY_ACTION_DOWN, BTN_DPAD_DOWN);
    stick(0, 10000, 0);
    expect_no_key();
    stick(0, 5000, 0);
    release(KSHIM_KEY_ACTION_DOWN, BTN_DPAD_DOWN);
    stick(0, 0, 0x2);
    press(KSHIM_KEY_ACTION_BACK, BTN_EAST);
    stick(0, 0, 0);
    release(KSHIM_KEY_ACTION_BACK, BTN_EAST);

    /* Low-speed keyboard through the TT: Down, Enter. */
    uint8_t k[8] = {0, 0, 0x51};
    xm_queue_report(KbdIdx, 0x81, k, 8);
    press(KSHIM_KEY_ACTION_DOWN, KEY_DOWN);
    k[2] = 0x28;
    xm_queue_report(KbdIdx, 0x81, k, 8);
    release(KSHIM_KEY_ACTION_DOWN, KEY_DOWN);
    press(KSHIM_KEY_ACTION_SELECT, KEY_ENTER);
    k[2] = 0;
    xm_queue_report(KbdIdx, 0x81, k, 8);
    release(KSHIM_KEY_ACTION_SELECT, KEY_ENTER);

    /* Quirk layouts: the vendor pad's report descriptor, the XInput clone. */
    uint8_t v[2] = {0x00, 0x01};
    xm_queue_report(VendorIdx, 0x81, v, 2);
    press(KSHIM_KEY_ACTION_UP, BTN_DPAD_UP);
    v[0] = 0x02; v[1] = 0;
    xm_queue_report(VendorIdx, 0x81, v, 2);
    release(KSHIM_KEY_ACTION_UP, BTN_DPAD_UP);
    press(KSHIM_KEY_ACTION_BACK, BTN_EAST);
    v[0] = 0;
    xm_queue_report(VendorIdx, 0x81, v, 2);
    release(KSHIM_KEY_ACTION_BACK, BTN_EAST);
    xinput(CloneIdx, 0x01, 0, 0);
    press(KSHIM_KEY_ACTION_UP, BTN_DPAD_UP);
    xinput(CloneIdx, 0, 0, 0);
    release(KSHIM_KEY_ACTION_UP, BTN_DPAD_UP);
    /* The ignored consumer interface maps nothing (volume up). */
    uint8_t vol = 0x01;
    xm_queue_report(CloneIdx, 0x82, &vol, 1);
    run_ms(2);
    expect_no_key();
    expect_log("usbtrace: report path=0-2.5 if=1 seq=1 len=1 hex=01 held=unbound");

    /* Report trace: distinct reports with the mapped keys, then the cap. */
    expect_log("usbtrace: report path=0-1 if=0 seq=1 len=4 hex=80800800 held=-");
    expect_log("usbtrace: report path=0-1 if=0 seq=2 len=4 hex=80800000 held=220:up");
    expect_log("usbtrace: report path=0-1 if=0 seq=12 ");
    expect_log("usbtrace: report path=0-1 if=0 seq=12 limit=1");
    assert(!logged("usbtrace: report path=0-1 if=0 seq=13"));
    expect_log("usbtrace: report path=0-2.1 if=0 seq=4 len=20 hex=0014001000000000000000000000000000000000 "
               "held=130:select");
    expect_log("usbtrace: report path=0-2.2 if=0 seq=1 len=6 hex=030000204e00 held=221:down");
    expect_log("usbtrace: report path=0-2.4 if=0 seq=1 len=2 hex=0001 held=220:up");
    assert(Xm.dma_blocked == 0 && DecodeMisses == 0);
}

static void check_hotplug(void)
{
    /* Xbox pad unplugged below the hub with the D-pad held: RELEASE. */
    xinput(XboxIdx, 0x02, 0, 0);
    press(KSHIM_KEY_ACTION_DOWN, BTN_DPAD_DOWN);
    unsigned disables = Xm.commands[10];
    xm_hub_detach(2, 1);
    run_ms(5);
    release(KSHIM_KEY_ACTION_DOWN, BTN_DPAD_DOWN);
    assert(!xm_slot_of(XboxIdx) && Xm.commands[10] == disables + 1);
    expect_log("usbtrace: gone path=0-2.1 vid=045e pid=028e");
    /* Re-plug: debounce, reset through the hub, enumerated again. */
    unsigned devs = logged("pid=028e rel=0114");
    XboxIdx = xm_hub_attach(2, 1, &Xbox);
    run_ms(50);
    assert(!xm_slot_of(XboxIdx));
    run_ms(200);
    struct xm_slot *xbox = xm_slot_of(XboxIdx);
    assert(xbox && xbox->tt_port == 1 && Xm.tt_errors == 0 && logged("pid=028e rel=0114") == devs + 1);
    xinput(XboxIdx, 0, 0x10, 0);
    press(KSHIM_KEY_ACTION_SELECT, BTN_SOUTH);
    xinput(XboxIdx, 0, 0, 0);
    release(KSHIM_KEY_ACTION_SELECT, BTN_SOUTH);

    /* Generic pad unplugged from the root port with button 1 held. */
    pad(0x80, 0x80, 8, 0x1);
    press(KSHIM_KEY_ACTION_SELECT, BTN_SOUTH);
    xm_detach(1);
    run_ms(5);
    release(KSHIM_KEY_ACTION_SELECT, BTN_SOUTH);
    assert(!xm_slot_of(1));
    expect_log("usbtrace: gone path=0-1 vid=1209 pid=0007");
    xm_attach(1, &Pad);
    run_ms(250);
    assert(xm_slot_of(1));
    pad(0x80, 0x80, 0, 0);
    press(KSHIM_KEY_ACTION_UP, BTN_DPAD_UP);
    pad(0x80, 0x80, 8, 0);
    release(KSHIM_KEY_ACTION_UP, BTN_DPAD_UP);
    /* The whole hub goes away with a key held on a pad behind it: the
     * subtree is removed children first, the key released, every slot
     * below it disabled. */
    uint8_t v[2] = {0x00, 0x02};
    xm_queue_report(VendorIdx, 0x81, v, 2);
    press(KSHIM_KEY_ACTION_DOWN, BTN_DPAD_DOWN);
    disables = Xm.commands[10];
    xm_detach(2);
    run_ms(5);
    release(KSHIM_KEY_ACTION_DOWN, BTN_DPAD_DOWN);
    assert(Xm.commands[10] == disables + 6 && !xm_slot_of(2) && !xm_slot_of(VendorIdx) && !xm_slot_of(XboxIdx));
    const char *text = log_text();
    const char *child = strstr(text, "usbtrace: gone path=0-2.5 "), *hub = strstr(text, "usbtrace: gone path=0-2 ");
    assert(child && hub && child < hub);
    /* The controller kept running, decoding and mastering throughout. */
    assert(Xhci->running && (Xm.usbcmd & 1U) && !(Xm.usbsts & 1U) && bus_master());
    assert(Xm.dma_blocked == 0 && DecodeMisses == 0 && Xm.unacked == 0);
}

static void check_handoff(uint8_t (*saved)[4096])
{
    unsigned hcrst = Xm.hcrst_count, frees = Gic[XHCI_SPI].frees;
    if (Scenario == NOHALT) Xm.never_halts = true;
    kshim_input_inject_quiesce();
    int status = kshim_io_quiesce();
    for (const char *const *path = (const char *const[]){"0-1 ", "0-2 ", "0-2.1 ", "0-2.2 ", "0-2.3 ", "0-2.4 ",
                                                          "0-2.5 ", NULL};
         *path; ++path) {
        char line[48];
        snprintf(line, sizeof(line), "usbtrace: gone path=%s", *path);
        expect_log(line);
    }
    HandedOff = true;
    if (Scenario == NOHALT) {
        /* The controller did not halt: handoff refused, R/S cleared, no
         * reset, and bus mastering off although UEFI had it on. */
        assert(status != 0 && !(Xm.usbcmd & 1U) && !(Xm.usbsts & 1U) && Xm.hcrst_count == hcrst);
        assert(!(model_get(XhciFn, KSHIM_PCI_COMMAND, 2) & KSHIM_PCI_COMMAND_MASTER));
        assert(kshim_io_quiesce() != 0); /* stays refused */
        return;
    }
    assert(status == 0);
    /* xHCI: halted, then HCRST: no rings, no interrupter, no slots. */
    assert((Xm.usbsts & 1U) && Xm.usbcmd == 0 && Xm.hcrst_count == hcrst + 1 && Xm.rs_clear_seq < Xm.hcrst_seq);
    assert(!Xm.dcbaap_lo && !Xm.dcbaap_hi && !Xm.crcr_lo && !Xm.iman && !Xm.erstba_lo && !Xm.config);
    assert(!(Xm.legsup & (1U << 24)) && (Xm.legsup & (1U << 16))); /* BIOS semaphore back */
    for (unsigned s = 1; s <= XM_SLOTS; ++s) assert(!Xm.slots[s].enabled);
    assert(kshim_dma_in_use() == 0);
    if (Scenario != POLL) assert(!Gic[XHCI_SPI].requested && Gic[XHCI_SPI].frees == frees + 1);
    /* PCI: the whole configuration space as found (journal replay), i.e.
     * bus mastering, memory decode and BAR0 off after the cold scenarios,
     * and exactly UEFI's settings after the takeover. */
    model_expect(saved);
    uint16_t cmd = (uint16_t)model_get(XhciFn, KSHIM_PCI_COMMAND, 2);
    if (Scenario == TAKEOVER) assert((cmd & 6U) == 6U);
    else assert(!(cmd & (KSHIM_PCI_COMMAND_MASTER | KSHIM_PCI_COMMAND_MEMORY)) && !bus_master());
    /* Nothing moves afterwards: a new report or connection is not DMAed. */
    unsigned events = Xm.ev_posted;
    uint8_t r[4] = {0x80, 0x80, 0, 0};
    xm_queue_report(1, 0x81, r, 4);
    xm_detach(1);
    xm_attach(2, &Hub);
    assert(Xm.ev_posted == events && MmioAfterHandoff == 0 && Xm.dma_blocked == 0);
    assert(kshim_io_quiesce() == 0 && MmioAfterHandoff == 0); /* idempotent */
    kshim_key_event_t e;
    assert(!next_key(&e));
}

/* ---- Scenarios ---------------------------------------------------------------------- */

static void *load(const char *path)
{
    FILE *f = fopen(path, "rb");
    assert(f);
    static uint8_t blob[16384];
    size_t n = fread(blob, 1, sizeof(blob), f);
    fclose(f);
    assert(n > 0 && !fdt_check_header(blob));
    return blob;
}

int main(int argc, char **argv)
{
    const char *which = argc > 1 ? argv[1] : "irq";
    if (!strcmp(which, "irq")) Scenario = IRQ;
    else if (!strcmp(which, "poll")) Scenario = POLL;
    else if (!strcmp(which, "takeover")) Scenario = TAKEOVER;
    else if (!strcmp(which, "nohalt")) Scenario = NOHALT;
    else return 2;
    GicGrants = Scenario != POLL;
    snprintf(LogPath, sizeof(LogPath), "usb_gamepad_%s.log", which);
    fflush(stdout);
    assert(freopen(LogPath, "w", stdout));
    setvbuf(stdout, NULL, _IONBF, 0);
    const void *fdt = load(FIXTURES "/usb-gamepad.dtb");

    /* PCI: the root port and the uPD720201 (Renesas firmware running, as
     * UEFI XhciDxe leaves it), 64-bit 8 KiB BAR0, INTA. */
    RootFn = model_add(-1, 0, 0, 0x17cb, 0x0111, 0x060400, 0x01, 0);
    model_pcie_port(RootFn, 4);
    XhciFn = model_add(RootFn, 0, 0, 0x1912, 0x0014, 0x0c0330, 0x00, 1);
    model_pcie_port(XhciFn, 0);
    model_bar(XhciFn, 0, BAR_SIZE, 4);
    model_set(XhciFn, RENESAS_FW_STATUS, 2, RENESAS_RESULT_SUCCESS << 4);
    model_set(XhciFn, RENESAS_FW_VERSION, 4, 0x00202600);
    bool uefi = Scenario == TAKEOVER || Scenario == NOHALT;
    if (uefi) {
        model_set(RootFn, 0x18, 4, 0x00010100);         /* bus 0 -> 1..1 */
        model_set(RootFn, 0x20, 4, 0x40304030);         /* window 0x40300000-0x403fffff */
        model_set(RootFn, KSHIM_PCI_COMMAND, 2, 0x0006);
        model_set(XhciFn, 0x10, 4, 0x40300004);
        model_set(XhciFn, KSHIM_PCI_COMMAND, 2, 0x0006);
    }
    static uint8_t saved[MODEL_FUNCS][4096];
    model_snapshot(saved);

    xm_init(32, 2, uefi);
    /* UEFI's interrupter left asserted: the line must not be taken before
     * the driver halted and reset the controller. */
    if (uefi) Xm.iman = 3U;
    Xm.irq = xhci_line;
    Xm.dma_allowed = bus_master;
    make_devices();
    xm_attach(1, &Pad);
    xm_attach(2, &Hub);
    XboxIdx = xm_hub_attach(2, 1, &Xbox);
    StickIdx = xm_hub_attach(2, 2, &Stick);
    KbdIdx = xm_hub_attach(2, 3, &Kbd);
    VendorIdx = xm_hub_attach(2, 4, &Vendor);
    CloneIdx = xm_hub_attach(2, 5, &Clone);

    struct kshim_resources res;
    memset(&res, 0, sizeof(res));
    res.fdt = fdt;
    assert(!kshim_pci_map_resources(&res));
    uint64_t start = NowUs;
    assert(!kshim_io_init(fdt));
    fprintf(stderr, "%s: enumerated in %u ms\n", which, (unsigned)((NowUs - start) / 1000U));
    expect_log("xhci: Renesas 0014 firmware already running");
    check_topology();
    check_trace();
    check_menu();
    check_hotplug();
    check_handoff(saved);
    /* The trace lines, for the log (ctest --verbose). */
    for (char *line = strtok(log_text(), "\n"); line; line = strtok(NULL, "\n"))
        if (!strncmp(line, "usbtrace:", 9)) fprintf(stderr, "%s\n", line);
    fprintf(stderr, "usb_gamepad_test %s: ok (%u interrupts, %u events)\n", which, Gic[XHCI_SPI].delivered,
            Xm.ev_posted);
    return 0;
}
