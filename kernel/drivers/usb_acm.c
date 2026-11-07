/* SPDX-License-Identifier: MIT */
/*
 * USB CDC-ACM console backend: binds CrDK's UsbAcmCrLib engine to kShimC.
 *
 * MMIO goes through the injectable CrIo, bounded to the DT core window. The
 * context (event ring, TRBs, buffers, rings) is static runtime memory, which
 * is identity mapped and write-back cacheable once the MMU is on, so DMA
 * addresses are the pointers themselves and every buffer is maintained by
 * VA to the point of coherency. Before the MMU is on the same operations
 * act on uncached memory and stay correct.
 *
 * No engine lock hooks: kernel/drivers/console.c serializes every caller,
 * across the SMP workers too, and the engine's own busy flag turns away a
 * nested entry (an exception printing from inside the console).
 */
#include <usb_acm.h>
#include <Library/UsbAcmCrLib.h>
#include <Library/cr_geni.h>

/* QSCRATCH registers the engine's VBUS override sets (UsbAcmDwc3Regs.h). */
#define QSCRATCH_HS_PHY_CTRL 0x10U
#define QSCRATCH_SS_PHY_CTRL 0x30U

static CR_USB_ACM_CONTEXT context __attribute__((aligned(CR_USB_ACM_CONTEXT_ALIGN)));
static CR_USB_ACM acm;
static struct kshim_usb_acm_resource resource;
static struct CrIo io;
static bool active;
/* The previous bootloader's QSCRATCH, put back at stop. */
static bool qscratch_saved;
static uint32_t inherited_hs, inherited_ss, started_hs;
/* Link milestones, updated under the console lock. */
static bool was_configured, was_open;
static unsigned pending_milestones; /* bit n: Milestones[n] */

__attribute__((weak)) const struct CrIo *kshim_usb_acm_io(void)
{
    return CrIoDefault();
}

/* The bus address of window+offset, or 0 outside the DT windows. */
static uintptr_t window_address(CR_USB_ACM_WINDOW window, UINT32 offset)
{
    if (offset & 3) return 0;
    if (window == CrUsbAcmWindowDwc3 && offset <= resource.size - 4)
        return (uintptr_t)resource.base + offset;
    if (window == CrUsbAcmWindowQscratch && resource.qscratch &&
        offset <= CR_USB_ACM_QSCRATCH_BYTES - 4)
        return (uintptr_t)resource.qscratch + offset;
    return 0;
}

static UINT32 read32(VOID *unused, CR_USB_ACM_WINDOW window, UINT32 offset)
{
    (void)unused;
    uintptr_t address = window_address(window, offset);
    return address ? io.read32(io.context, address) : 0;
}

static VOID write32(VOID *unused, CR_USB_ACM_WINDOW window, UINT32 offset, UINT32 value)
{
    (void)unused;
    uintptr_t address = window_address(window, offset);
    if (address) io.write32(io.context, address, value);
}

static UINT64 dma_address(VOID *unused, CONST VOID *buffer)
{
    (void)unused;
    return (UINT64)(uintptr_t)buffer;
}

static VOID cache(VOID *unused, CR_USB_ACM_CACHE_OP operation, VOID *buffer, UINTN length)
{
    (void)unused;
#if defined(__aarch64__)
    uint64_t ctr;
    __asm__ volatile("mrs %0, ctr_el0" : "=r"(ctr));
    uintptr_t line = (uintptr_t)4 << ((ctr >> 16) & 0xF);
    uintptr_t end = (uintptr_t)buffer + length;
    __asm__ volatile("dsb sy" ::: "memory");
    for (uintptr_t a = (uintptr_t)buffer & ~(line - 1); a < end; a += line) {
        if (operation == CrUsbAcmCacheClean)
            __asm__ volatile("dc cvac, %0" : : "r"(a) : "memory");
        else if (operation == CrUsbAcmCacheInvalidate)
            __asm__ volatile("dc ivac, %0" : : "r"(a) : "memory");
        else
            __asm__ volatile("dc civac, %0" : : "r"(a) : "memory");
    }
    __asm__ volatile("dsb sy" ::: "memory");
#else
    (void)operation;
    (void)buffer;
    (void)length;
#endif
}

static void restore_qscratch(void)
{
    if (!qscratch_saved) return;
    write32(NULL, CrUsbAcmWindowQscratch, QSCRATCH_SS_PHY_CTRL, inherited_ss);
    write32(NULL, CrUsbAcmWindowQscratch, QSCRATCH_HS_PHY_CTRL, inherited_hs);
    qscratch_saved = false;
}

int kshim_usb_acm_start(const struct kshim_usb_acm_resource *r, const struct CrIo *with)
{
    static const CR_USB_ACM_IO hooks = {NULL, read32, write32, dma_address, cache, NULL, NULL};
    /* Linux g_serial's IDs: hosts bind cdc_acm/usbser by class anyway.
     * Qualcomm cores have no VBUS detection path to the controller: they
     * connect only once software claims VBUS and session valid in QSCRATCH,
     * as Linux's dwc3-qcom does for the device role. Without it the core
     * runs but the host never sees a device. */
    CR_USB_ACM_CONFIG config = {
        .VendorId = 0x0525, .ProductId = 0xA4A7,
        .Manufacturer = "Project Aloha", .Product = "kShimC USB Serial", .SerialNumber = "0001",
        .QcomVbusOverride = r && r->qscratch ? TRUE : FALSE,
        .InterruptEnable = FALSE, .DeviceEvents = 0,
    };
    if (active || !r || !with || !CrIoValid(with) || r->size < CR_USB_ACM_DWC3_WINDOW_BYTES)
        return -1;
    resource = *r;
    io = *with;
    was_configured = was_open = false;
    __atomic_store_n(&pending_milestones, 0, __ATOMIC_RELAXED);
    if (CrUsbAcmInit(&acm, &hooks, &context) != CR_SUCCESS) return -2;
    if (resource.qscratch) {
        inherited_hs = read32(NULL, CrUsbAcmWindowQscratch, QSCRATCH_HS_PHY_CTRL);
        inherited_ss = read32(NULL, CrUsbAcmWindowQscratch, QSCRATCH_SS_PHY_CTRL);
        qscratch_saved = true;
    }
    if (CrUsbAcmStart(&acm, &config) != CR_SUCCESS) {
        restore_qscratch();
        return -3;
    }
    if (resource.qscratch) started_hs = read32(NULL, CrUsbAcmWindowQscratch, QSCRATCH_HS_PHY_CTRL);
    active = true;
    return 0;
}

bool kshim_usb_acm_qscratch_hs(uint32_t *inherited, uint32_t *now)
{
    if (!active || !resource.qscratch) return false;
    if (inherited) *inherited = inherited_hs;
    if (now) *now = started_hs;
    return true;
}

static const char *const Milestones[] = {
    NULL, "host configured the device", "host opened the port",
    "host closed the port", "host reset or detached",
};

/* After every engine call: note each change of the link since the last. */
static void track(void)
{
    bool configured = CrUsbAcmIsConfigured(&acm), open = CrUsbAcmIsOpen(&acm);
    unsigned bits = 0;
    if (configured && !was_configured) bits |= 1U << 1;
    if (open && !was_open) bits |= 1U << 2;
    if (!open && was_open && configured) bits |= 1U << 3;
    if (!configured && was_configured) bits |= 1U << 4;
    was_configured = configured;
    was_open = open;
    if (bits) __atomic_fetch_or(&pending_milestones, bits, __ATOMIC_RELEASE);
}

const char *kshim_usb_acm_milestone(void)
{
    unsigned pending = __atomic_load_n(&pending_milestones, __ATOMIC_ACQUIRE);
    if (!pending) return NULL;
    unsigned bit = pending & -pending; /* oldest kind first */
    __atomic_fetch_and(&pending_milestones, ~bit, __ATOMIC_ACQ_REL);
    return Milestones[__builtin_ctz(bit)];
}

bool kshim_usb_acm_active(void) { return active; }
const struct kshim_usb_acm_resource *kshim_usb_acm_resource(void) { return active ? &resource : NULL; }

void kshim_usb_acm_write(const char *buffer, uint32_t length)
{
    if (!active) return;
    (void)CrUsbAcmWrite(&acm, (const UINT8 *)buffer, length);
    track();
}

int kshim_usb_acm_read(char *character)
{
    if (!active) return 0;
    int got = CrUsbAcmRead(&acm, (UINT8 *)character, 1) == 1;
    track();
    return got;
}

void kshim_usb_acm_poll(void)
{
    if (!active) return;
    CrUsbAcmPoll(&acm);
    track();
}

bool kshim_usb_acm_open(void)
{
    return active && CrUsbAcmIsOpen(&acm);
}

void kshim_usb_acm_stop(void)
{
    if (!active) return;
    CrUsbAcmShutdown(&acm);
    /* The pull-up is gone: hand QSCRATCH back as the bootloader left it. */
    restore_qscratch();
    active = false;
}
