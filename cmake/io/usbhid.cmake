# SPDX-License-Identifier: MIT
# Owner: usbhid. Hub and HID class drivers (kernel/drivers/usb/class/), HID
# parser and input mapping (kernel/drivers/hid/), input injection
# (kernel/core/input_inject.c). See docs/io-stack.md.
#
# USB core: kernel/drivers/usb/core/ belongs to the xhci fragment
# (cmake/io/usb.cmake, included before this one). The usbhid branch carries
# a core of its own (usb_core.c) so the class drivers build and run without
# it; it is only compiled while no other kernel/drivers/usb/core/ source is
# listed, so a merged xhci core replaces it without duplicate symbols.
if(CONFIG_KSHIM_USB)
    set(KSHIM_USBHID_CORE_LISTED OFF)
    foreach(source IN LISTS KERNEL_SOURCES)
        if(source MATCHES "^kernel/drivers/usb/core/")
            set(KSHIM_USBHID_CORE_LISTED ON)
        endif()
    endforeach()
    if(NOT KSHIM_USBHID_CORE_LISTED)
        list(APPEND KERNEL_SOURCES kernel/drivers/usb/core/usb_core.c)
    endif()
endif()
if(CONFIG_KSHIM_USB_HUB)
    list(APPEND KERNEL_SOURCES kernel/drivers/usb/class/hub.c)
endif()
if(CONFIG_KSHIM_USB_HID)
    list(APPEND KERNEL_SOURCES kernel/drivers/usb/class/hid.c
        kernel/drivers/hid/hid_parse.c kernel/drivers/hid/hid_input.c)
endif()
if(CONFIG_KSHIM_INPUT_INJECT)
    list(APPEND KERNEL_SOURCES kernel/core/input_inject.c)
endif()
