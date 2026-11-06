/* SPDX-License-Identifier: MIT */
/* kernel/drivers/usb/host/xhci_pci.c for usb_gamepad_test: unchanged, except
 * that BAR0 accesses go through the test's routing to the xHCI model
 * (test_xhci_io_mmio) instead of plain MMIO pointers. */
#define xhci_io_mmio test_xhci_io_mmio
#include "../../kernel/drivers/usb/host/xhci_pci.c"
