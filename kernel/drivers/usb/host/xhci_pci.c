/* SPDX-License-Identifier: MIT */
/*
 * xHCI on PCI (kshim_xhci_pci_register, kshim/usb_hcd.h): binds PCI class
 * 0x0c0330 (serial bus / USB / xHCI programming interface, PCI Code and ID
 * Assignment spec) plus the IDs below, prepares Renesas controllers, maps
 * BAR0, takes the function's interrupt (MSI via the host, else INTx, else
 * polling) and starts the controller core.
 *
 * Handoff: the PCI core calls our quiesce (xhci_stop: halt, HCRST, restore
 * the legacy-support semaphores), then frees the interrupt and replays the
 * config journal (COMMAND, BARs). A downloaded Renesas image stays loaded
 * only while the link and the controller's rails were inherited; after a
 * kShimC cold start the host restores PERST# and the endpoint rails, and the
 * payload has to download it again (renesas_fw.h).
 */
#include "xhci.h"

#include <config.h>
#include <kshim/dma.h>
#include <kshim/pci.h>
#include <kshim/time.h>

#include <stdio.h>
#include <string.h>

#if CONFIG_KSHIM_USB_XHCI_RENESAS
#include "renesas_fw.h"
#if CONFIG_FIRMWARE_LOADER
#include <kshim/firmware.h>
#endif
#endif

#ifndef KSHIM_RENESAS_FW_EMBEDDED
#define KSHIM_RENESAS_FW_EMBEDDED 0
#endif

#ifndef XHCI_PCI_MAX
#define XHCI_PCI_MAX 2U
#endif
#define XHCI_ID_RENESAS 0x1U

/* Vendor/device facts: PCI-SIG vendor IDs 0x1912 (Renesas), 0x1033 (NEC);
 * uPD720201 = 0x0014, uPD720202 = 0x0015 (Renesas user's manual), NEC
 * uPD720200 = 0x0194 (QEMU nec-usb-xhci models it). */
static const struct kshim_pci_device_id XhciIds[] = {
    {0x1912, 0x0014, 0, 0, XHCI_ID_RENESAS},
    {0x1912, 0x0015, 0, 0, XHCI_ID_RENESAS},
    {0x1033, 0x0194, 0, 0, 0},
    {KSHIM_PCI_ANY_ID, KSHIM_PCI_ANY_ID, KSHIM_PCI_CLASS_SERIAL_USB_XHCI, 0xffffffU, 0},
    {0, 0, 0, 0, 0},
};

struct xhci_pci {
    struct xhci x;           /* first */
    struct kshim_pci_dev *dev;
    char name[24];
    bool irq;
    uint32_t intid;
};

static struct xhci_pci Controllers[XHCI_PCI_MAX];
static unsigned ControllerCount;

static void xhci_pci_irq(uint32_t intid, void *context)
{
    (void)intid;
    (void)xhci_irq(&((struct xhci_pci *)context)->x);
}

/* ---- Renesas uPD720201/uPD720202 -------------------------------------------- */

#if CONFIG_KSHIM_USB_XHCI_RENESAS
#ifndef CONFIG_KSHIM_USB_XHCI_RENESAS_FW
#define CONFIG_KSHIM_USB_XHCI_RENESAS_FW "renesas_usb_fw.mem"
#endif

#if KSHIM_RENESAS_FW_EMBEDDED
/* Optional image linked in at build time (CONFIG_KSHIM_USB_XHCI_RENESAS_FW_EMBED,
 * renesas_fw_blob.S). Never committed to the tree; a runtime built with it
 * contains Renesas' proprietary firmware. */
extern const uint8_t kshim_renesas_fw_blob[];
extern const uint8_t kshim_renesas_fw_blob_end[];
#endif

static int cfg_read32(void *context, uint16_t reg, uint32_t *value)
{
    return kshim_pci_read_config32(context, reg, value);
}
static int cfg_write32(void *context, uint16_t reg, uint32_t value)
{
    return kshim_pci_write_config_raw32(context, reg, value);
}
static void cfg_delay(void *context, uint32_t us)
{
    (void)context;
    (void)delay(us);
}

#if CONFIG_FIRMWARE_LOADER && !KSHIM_RENESAS_FW_EMBEDDED
/* Where renesas_usb_fw.mem is looked for, one loader request per group (a
 * group the loader cannot read, such as an EROFS vendor partition without
 * the GPL CONFIG_FS_EROFS, does not stop the next one):
 *  1. the loader defaults, the Android vendor firmware directories
 *     (super/vendor{slot}/firmware, .../firmware/image, modem{slot}/image),
 *     where Android ships it for Linux's request_firmware();
 *  2. other dynamic partitions in super (odm, vendor_dlkm), which need the
 *     current slot;
 *  3. GPT partitions a user can provide it on without a slot: persist
 *     (ext4) and the UEFI system partition efisp (FAT), in firmware/ or at
 *     the root. */
static const char *const RenesasSlotLocations[] = {
    "super|odm{slot}|firmware",
    "super|vendor_dlkm{slot}|firmware",
};
static const char *const RenesasPlainLocations[] = {
    "persist||firmware", "persist||", "efisp||firmware", "efisp||",
};
static const struct {
    const char *const *locations;
    unsigned count;
    const char *what;
} RenesasSources[] = {
    {NULL, 0, "vendor firmware (super/vendor{slot}, modem{slot})"},
    {RenesasSlotLocations, 2, "super/odm{slot}, super/vendor_dlkm{slot}"},
    {RenesasPlainLocations, 4, "persist, efisp"},
};

static const char *renesas_source_error(int status)
{
    switch (status) {
    case ERROR_NOT_FOUND: return "absent";
    case ERROR_NOT_SUPPORTED: return "filesystem not readable (EROFS needs the GPL CONFIG_FS_EROFS)";
    case KSHIM_ERR_NOT_READY: return "storage or boot slot not ready";
    default: return kshim_io_strerror(status);
    }
}
#endif

/* Sources after "already running" and "loaded from the external ROM": the
 * image linked in at build time when CONFIG_KSHIM_USB_XHCI_RENESAS_FW_EMBED
 * names one (opt-in; then no partition is read), else the firmware loader
 * groups above in order. Nothing found: the controller stays off with one
 * log line naming the places to provide the file (docs/usb.md). */
static int renesas_download_from(const struct renesas_cfg *cfg, const char *name)
{
#if KSHIM_RENESAS_FW_EMBEDDED
    (void)name;
    size_t size = (size_t)(kshim_renesas_fw_blob_end - kshim_renesas_fw_blob);
    printf("xhci: Renesas firmware (built in, %u bytes)\n", (unsigned)size);
    return renesas_fw_download(cfg, kshim_renesas_fw_blob, size);
#elif CONFIG_FIRMWARE_LOADER
    /* The loader reads into scratch memory borrowed from the DMA pool and
     * returned right after the download, before the rings are allocated. */
    uint64_t bus;
    void *buffer = kshim_dma_alloc(RENESAS_FW_MAX_SIZE, 64, 0, UINT64_MAX, &bus);
    if (!buffer) return KSHIM_IO_ERR_NO_MEMORY;
    int status = ERROR_NOT_FOUND;
    bool found = false;
    for (unsigned i = 0; i < sizeof(RenesasSources) / sizeof(RenesasSources[0]) && !found; ++i) {
        struct kshim_fw_request req = {
            .locations = RenesasSources[i].locations,
            .location_count = RenesasSources[i].count,
            .kind = KSHIM_FW_FILE,
            .max_size = RENESAS_FW_MAX_SIZE,
            .buffer = buffer,
            .buffer_size = RENESAS_FW_MAX_SIZE,
        };
        struct kshim_firmware fw;
        memset(&fw, 0, sizeof(fw));
        int request = kshim_firmware_request(name, &req, &fw);
        if (request) {
            printf("xhci: Renesas firmware %s: %s: %s\n", name, RenesasSources[i].what,
                   renesas_source_error(request));
            continue;
        }
        found = true;
        printf("xhci: Renesas firmware %s (%u bytes, partition)\n", fw.name[0] ? fw.name : name,
               (unsigned)fw.size);
        status = renesas_fw_download(cfg, fw.data, fw.size);
        kshim_firmware_release(&fw);
    }
    kshim_dma_free(buffer, RENESAS_FW_MAX_SIZE);
    if (!found)
        printf("xhci: Renesas firmware %s not found; the controller stays off. Put it in the vendor "
               "firmware directory, persist or efisp (firmware/ or the root), or build it in with "
               "CONFIG_KSHIM_USB_XHCI_RENESAS_FW_EMBED\n", name);
    return status;
#else
    (void)cfg;
    printf("xhci: Renesas firmware %s needed but neither the firmware loader nor an embedded "
           "image is configured; the controller stays off\n", name);
    return ERROR_NOT_SUPPORTED;
#endif
}

/* Inherit first: firmware already running (UEFI XhciDxe downloaded it) or
 * loaded from an external ROM is used as is. */
static int renesas_prepare(struct kshim_pci_dev *dev)
{
    struct renesas_cfg cfg = {dev, cfg_read32, cfg_write32, cfg_delay};
    enum renesas_fw_state state;
    uint32_t raw = 0, version = 0;
    int status = renesas_fw_state(&cfg, &state, &raw);
    (void)kshim_pci_read_config32(dev, RENESAS_FW_VERSION, &version);
    if (status) return status;
    switch (state) {
    case RENESAS_FW_RUNNING:
        printf("xhci: Renesas %04x firmware already running (status 0x%08x, version 0x%08x)\n",
               dev->device, (unsigned)raw, (unsigned)version);
        return 0;
    case RENESAS_FW_FROM_ROM:
        printf("xhci: Renesas %04x firmware loaded from external ROM (status 0x%08x)\n",
               dev->device, (unsigned)raw);
        return 0;
    case RENESAS_FW_FAILED:
        printf("xhci: Renesas %04x firmware state 0x%08x needs a reset; not downloading\n",
               dev->device, (unsigned)raw);
        return ERROR_DEVICE_ERROR;
    default:
        break;
    }
    status = renesas_download_from(&cfg, CONFIG_KSHIM_USB_XHCI_RENESAS_FW);
    if (status) {
        printf("xhci: Renesas firmware download failed: %s (%d)\n", kshim_io_strerror(status), status);
        return status;
    }
    (void)kshim_pci_read_config32(dev, RENESAS_FW_VERSION, &version);
    printf("xhci: Renesas firmware downloaded (version 0x%08x)\n", (unsigned)version);
    return 0;
}
#endif

/* ---- PCI driver ------------------------------------------------------------- */

static int xhci_pci_probe(struct kshim_pci_dev *dev, const struct kshim_pci_device_id *id)
{
    if (ControllerCount >= XHCI_PCI_MAX) return KSHIM_IO_ERR_NO_MEMORY;
    struct xhci_pci *xp = &Controllers[ControllerCount];
    memset(xp, 0, sizeof(*xp));
    xp->dev = dev;
    snprintf(xp->name, sizeof(xp->name), "xhci-%02x:%02x.%u", dev->bus,
             KSHIM_PCI_SLOT(dev->devfn), (unsigned)KSHIM_PCI_FUNC(dev->devfn));
    printf("xhci: %s: %04x:%04x class %06x\n", xp->name, dev->vendor, dev->device,
           (unsigned)dev->class_code);
    int status;
    if (id->data & XHCI_ID_RENESAS) {
#if CONFIG_KSHIM_USB_XHCI_RENESAS
        if ((status = renesas_prepare(dev))) return status;
#else
        printf("xhci: %s: Renesas firmware download not configured; needs ROM or running firmware\n",
               xp->name);
#endif
    }
    volatile void *base = kshim_pci_bar_map(dev, 0);
    if (!base) {
        printf("xhci: %s: BAR0 not assigned\n", xp->name);
        return ERROR_NOT_SUPPORTED;
    }
    /* kShimC is identity mapped and the DMA pool is bus-addressable as is;
     * a host with a dma-ranges offset would need translation here. */
    if (kshim_pci_dma_addr(dev, 0x40000000ULL) != 0x40000000ULL) {
        printf("xhci: %s: host bridge translates DMA addresses; not supported\n", xp->name);
        return ERROR_NOT_SUPPORTED;
    }
    if ((status = kshim_pci_enable_device(dev, true))) return status;
    struct xhci_io io;
    xhci_io_mmio(&io);
    xp->x.quirk_zero_64b = dev->vendor == 0x1912;
    /* Halt and reset first: a controller inherited from UEFI may still
     * hold its interrupt asserted, and only after HCRST (INTE and IMAN.IE
     * clear) is it safe to unmask the line. */
    status = xhci_prepare(&xp->x, xp->name, (uintptr_t)base, &io, UINT64_MAX);
    if (status) {
        printf("xhci: %s: start failed: %s (%d)\n", xp->name, kshim_io_strerror(status), status);
        return status;
    }
    xp->irq = false;
#if !CONFIG_KSHIM_USB_XHCI_FORCE_POLL
    status = kshim_pci_request_irq(dev, KSHIM_PCI_IRQ_MSI | KSHIM_PCI_IRQ_INTX, xhci_pci_irq, xp,
                                   &xp->intid);
    xp->irq = status == 0;
    if (status && status != ERROR_NOT_SUPPORTED)
        printf("xhci: %s: interrupt unavailable (%d), polling\n", xp->name, status);
#endif
    status = xhci_run(&xp->x, xp->irq);
    if (status) {
        printf("xhci: %s: start failed: %s (%d)\n", xp->name, kshim_io_strerror(status), status);
        if (xp->irq) (void)kshim_pci_free_irq(dev);
        return status;
    }
    if (xp->irq) printf("xhci: %s: interrupt INTID %u\n", xp->name, (unsigned)xp->intid);
    else printf("xhci: %s: no interrupt, event ring polled\n", xp->name);
    dev->driver_data = xp;
    ++ControllerCount;
    return 0;
}

static int xhci_pci_quiesce(struct kshim_pci_dev *dev)
{
    struct xhci_pci *xp = dev ? dev->driver_data : NULL;
    if (!xp) return 0;
    return xhci_stop(&xp->x);
}

static struct kshim_pci_driver XhciPciDriver = {
    .name = "xhci",
    .ids = XhciIds,
    .probe = xhci_pci_probe,
    .quiesce = xhci_pci_quiesce,
};

int kshim_xhci_pci_register(void)
{
    static bool registered;
    if (registered) return 0;
    registered = true;
    return kshim_pci_driver_register(&XhciPciDriver);
}
