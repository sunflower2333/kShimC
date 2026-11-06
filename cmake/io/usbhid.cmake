# SPDX-License-Identifier: MIT
# Owner: usbhid. Hub and HID class drivers (kernel/drivers/usb/class/), HID
# parser and input mapping (kernel/drivers/hid/), input injection
# (kernel/core/input_inject.c). See docs/io-stack.md.
#
# The USB core (kernel/drivers/usb/core/) belongs to the xhci fragment
# (cmake/io/usb.cmake); the class drivers use only its public API.
if(CONFIG_KSHIM_USB_HUB)
    list(APPEND KERNEL_SOURCES kernel/drivers/usb/class/hub.c)
endif()
if(CONFIG_KSHIM_USB_HID)
    list(APPEND KERNEL_SOURCES kernel/drivers/usb/class/hid.c
        kernel/drivers/usb/class/hid_quirks.c
        kernel/drivers/hid/hid_parse.c kernel/drivers/hid/hid_input.c)
endif()
if(CONFIG_KSHIM_INPUT_INJECT)
    list(APPEND KERNEL_SOURCES kernel/core/input_inject.c)
endif()
