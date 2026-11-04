/* SPDX-License-Identifier: MIT */
/*
 * PCI self-test (CONFIG_KSHIM_PCI_SELFTEST), for tools/test_qemu_pci.py.
 * A probe-only driver for xHCI functions (class 0x0c0330): it enables
 * memory decoding, reads the xHCI capability registers through BAR0 (xHCI
 * 1.2 section 5.3: CAPLENGTH at +0, HCIVERSION at +2, HCSPARAMS1 at +4),
 * requests and frees the function's interrupt, and then declines the
 * function so a real driver registered later can still bind it. All config
 * writes go through the journal and are undone by kshim_pci_quiesce().
 */
#include "pci_internal.h"

#include <stdio.h>

static unsigned Interrupts;

static void selftest_irq(uint32_t intid, void *context)
{
    (void)intid;
    (void)context;
    ++Interrupts;
}

static int selftest_probe(struct kshim_pci_dev *dev, const struct kshim_pci_device_id *id)
{
    (void)id;
    int status = kshim_pci_enable_device(dev, false);
    volatile void *bar = kshim_pci_bar_map(dev, 0);
    if (status || !bar) {
        printf("PCI_SELFTEST %02x:%02x.%u: BAR0 unusable (%d)\n", dev->bus,
               KSHIM_PCI_SLOT(dev->devfn), KSHIM_PCI_FUNC(dev->devfn), status);
        return ERROR_NOT_SUPPORTED;
    }
    uint32_t cap = pci_read32((uintptr_t)bar);
    uint32_t hcs1 = pci_read32((uintptr_t)bar + 4);
    uint32_t intid = 0;
    int irq = kshim_pci_request_irq(dev, KSHIM_PCI_IRQ_INTX, selftest_irq, NULL, &intid);
    printf("PCI_SELFTEST %02x:%02x.%u [%04x:%04x] bar0=0x%llx caplength=0x%02x "
           "hciversion=0x%04x ports=%u slots=%u irq=%s intid=%u\n",
           dev->bus, KSHIM_PCI_SLOT(dev->devfn), KSHIM_PCI_FUNC(dev->devfn), dev->vendor,
           dev->device, (unsigned long long)dev->bars[0].bus_addr, cap & 0xffU, cap >> 16,
           hcs1 >> 24, hcs1 & 0xffU, irq ? "none" : "intx", (unsigned)intid);
    if (!irq) (void)kshim_pci_free_irq(dev);
    return ERROR_NOT_SUPPORTED; /* probe only: leave the function unbound */
}

static const struct kshim_pci_device_id SelftestIds[] = {
    {KSHIM_PCI_ANY_ID, KSHIM_PCI_ANY_ID, KSHIM_PCI_CLASS_SERIAL_USB_XHCI, 0xffffffU, 0},
    {0, 0, 0, 0, 0},
};

static struct kshim_pci_driver SelftestDriver = {
    .name = "pci-selftest",
    .ids = SelftestIds,
    .probe = selftest_probe,
};

int kshim_pci_selftest_register(void)
{
    return kshim_pci_driver_register(&SelftestDriver);
}
