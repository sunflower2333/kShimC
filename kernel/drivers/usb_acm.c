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

static CR_USB_ACM_CONTEXT context __attribute__((aligned(CR_USB_ACM_CONTEXT_ALIGN)));
static CR_USB_ACM acm;
static struct kshim_usb_acm_resource resource;
static struct CrIo io;
static bool active;

__attribute__((weak)) const struct CrIo *kshim_usb_acm_io(void)
{
    return CrIoDefault();
}

static UINT32 read32(VOID *unused, CR_USB_ACM_WINDOW window, UINT32 offset)
{
    (void)unused;
    if (window != CrUsbAcmWindowDwc3 || offset > resource.size - 4 || (offset & 3))
        return 0;
    return io.read32(io.context, (uintptr_t)resource.base + offset);
}

static VOID write32(VOID *unused, CR_USB_ACM_WINDOW window, UINT32 offset, UINT32 value)
{
    (void)unused;
    if (window != CrUsbAcmWindowDwc3 || offset > resource.size - 4 || (offset & 3))
        return;
    io.write32(io.context, (uintptr_t)resource.base + offset, value);
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

int kshim_usb_acm_start(const struct kshim_usb_acm_resource *r, const struct CrIo *with)
{
    static const CR_USB_ACM_IO hooks = {NULL, read32, write32, dma_address, cache, NULL, NULL};
    /* Linux g_serial's IDs: hosts bind cdc_acm/usbser by class anyway. */
    static const CR_USB_ACM_CONFIG config = {
        .VendorId = 0x0525, .ProductId = 0xA4A7,
        .Manufacturer = "Project Aloha", .Product = "kShimC USB Serial", .SerialNumber = "0001",
        .QcomVbusOverride = FALSE, .InterruptEnable = FALSE, .DeviceEvents = 0,
    };
    if (active || !r || !with || !CrIoValid(with) || r->size < CR_USB_ACM_DWC3_WINDOW_BYTES)
        return -1;
    resource = *r;
    io = *with;
    if (CrUsbAcmInit(&acm, &hooks, &context) != CR_SUCCESS) return -2;
    if (CrUsbAcmStart(&acm, &config) != CR_SUCCESS) return -3;
    active = true;
    return 0;
}

bool kshim_usb_acm_active(void) { return active; }
const struct kshim_usb_acm_resource *kshim_usb_acm_resource(void) { return active ? &resource : NULL; }

void kshim_usb_acm_write(const char *buffer, uint32_t length)
{
    if (active) (void)CrUsbAcmWrite(&acm, (const UINT8 *)buffer, length);
}

int kshim_usb_acm_read(char *character)
{
    return active && CrUsbAcmRead(&acm, (UINT8 *)character, 1) == 1;
}

void kshim_usb_acm_poll(void)
{
    if (active) CrUsbAcmPoll(&acm);
}

bool kshim_usb_acm_open(void)
{
    return active && CrUsbAcmIsOpen(&acm);
}

void kshim_usb_acm_stop(void)
{
    if (!active) return;
    CrUsbAcmShutdown(&acm);
    active = false;
}
