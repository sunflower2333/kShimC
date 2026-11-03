/* SPDX-License-Identifier: MIT */
/*
 * Contract checks for the I/O stack headers (docs/io-stack.md): every header
 * compiles on its own and together, wire formats have their specified sizes,
 * code ranges do not collide, and the public input enums only grew.
 */
#include <kshim/io.h>
#include <kshim/dma.h>
#include <kshim/gpio.h>
#include <kshim/pinctrl.h>
#include <kshim/clk.h>
#include <kshim/regulator.h>
#include <kshim/qup.h>
#include <kshim/i2c.h>
#include <kshim/spi.h>
#include <kshim/uart.h>
#include <kshim/pci.h>
#include <kshim/usb.h>
#include <kshim/usb_hcd.h>
#include <kshim/usb_class.h>
#include <kshim/hid.h>
#include <kshim/input_inject.h>
#include <kshim/touch_driver.h>
#include <kshim/storage.h>

#include <assert.h>
#include <stddef.h>
#include <stdio.h>

/* USB 2.0 chapter 9, USB 3.2 9.6.7, HID 1.11 6.2.1 wire sizes. */
_Static_assert(sizeof(struct kshim_usb_setup) == 8, "setup packet");
_Static_assert(sizeof(struct kshim_usb_device_descriptor) == 18, "device descriptor");
_Static_assert(sizeof(struct kshim_usb_config_descriptor) == 9, "configuration descriptor");
_Static_assert(sizeof(struct kshim_usb_interface_descriptor) == 9, "interface descriptor");
_Static_assert(sizeof(struct kshim_usb_endpoint_descriptor) == 7, "endpoint descriptor");
_Static_assert(sizeof(struct kshim_usb_ss_ep_comp_descriptor) == 6, "SS endpoint companion");
_Static_assert(sizeof(struct kshim_usb_hub_descriptor) == 7, "hub descriptor fixed part");
_Static_assert(sizeof(struct kshim_usb_hid_descriptor) == 9, "HID descriptor");
_Static_assert(offsetof(struct kshim_usb_device_descriptor, idVendor) == 8, "idVendor");
_Static_assert(offsetof(struct kshim_usb_config_descriptor, wTotalLength) == 2, "wTotalLength");
_Static_assert(offsetof(struct kshim_usb_hid_descriptor, wReportDescriptorLength) == 7,
               "wDescriptorLength");

/* Status codes: ERROR_* < storage (64..) < I/O (96..). */
_Static_assert(ERROR_NOT_READY < KSHIM_ERR_STORAGE_BASE, "generic codes");
_Static_assert(KSHIM_ERR_BUSY < KSHIM_IO_ERR_BASE, "storage range below I/O range");
_Static_assert(KSHIM_IO_ERR_PENDING < 128, "I/O range");

/* Public input enums only grew (include/kshim/input.h consumers). */
_Static_assert(KSHIM_KEY_ACTION_NONE == 0 && KSHIM_KEY_ACTION_UP == 1 &&
               KSHIM_KEY_ACTION_DOWN == 2 && KSHIM_KEY_ACTION_SELECT == 3 &&
               KSHIM_KEY_ACTION_BACK == 4, "key actions");
_Static_assert(KSHIM_TOUCH_EVENT_PRESS == 0 && KSHIM_TOUCH_EVENT_MOVE == 1 &&
               KSHIM_TOUCH_EVENT_RELEASE == 2 && KSHIM_TOUCH_EVENT_CANCEL == 3 &&
               KSHIM_TOUCH_EVENT_HOVER == 4, "touch events");
_Static_assert(KSHIM_INPUT_POINTER_CONTACT >= KSHIM_TOUCH_MAX_CONTACTS, "pointer contact id");
_Static_assert(KSHIM_INPUT_POINTER_CONTACT <= UINT8_MAX, "contact_id is uint8_t");

/* GENI protocol numbers match CrDK enum CrGeniProtocol. */
_Static_assert(KSHIM_QUP_PROTOCOL_SPI == 1 && KSHIM_QUP_PROTOCOL_I2C == 3, "GENI protocols");

int main(void)
{
    /* CrDK CrIoResult values (Library/cr_geni.h) map onto kShimC codes. */
    assert(kshim_io_err_from_bus(0) == 0);
    assert(kshim_io_err_from_bus(-121) == KSHIM_IO_ERR_NACK);
    assert(kshim_io_err_from_bus(-110) == ERROR_TIMEOUT);
    assert(kshim_io_err_from_bus(-16) == ERROR_BUSY);
    assert(kshim_io_err_from_bus(-22) == ERROR_INVALID_PARAMETER);
    assert(kshim_io_err_from_bus(-95) == ERROR_NOT_SUPPORTED);
    assert(kshim_io_err_from_bus(-5) == ERROR_DEVICE_ERROR);
    assert(kshim_io_err_from_bus(-11) == ERROR_DEVICE_ERROR);

    assert(KSHIM_HID_USAGE(KSHIM_HID_PAGE_GENERIC_DESKTOP, KSHIM_HID_GD_GAMEPAD) == 0x00010005U);
    assert(KSHIM_HID_USAGE_PAGE(0x000c00e9U) == KSHIM_HID_PAGE_CONSUMER);
    assert(KSHIM_HID_USAGE_ID(0x000c00e9U) == KSHIM_HID_CONSUMER_VOLUME_UP);

    assert(KSHIM_PCI_DEVFN(31, 7) == 0xff && KSHIM_PCI_SLOT(0x10) == 2 && KSHIM_PCI_FUNC(0x13) == 3);

    struct kshim_completion c;
    kshim_completion_init(&c);
    assert(!kshim_completion_done(&c));
    puts("io_headers_test: ok");
    return 0;
}
