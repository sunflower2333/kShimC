# SPDX-License-Identifier: MIT
# Owner: usbhid. Host register-model tests (docs/io-stack.md "Test plan").
#
# usbhid_hid_test: HID parser and mapping on real descriptors (QEMU usb-kbd,
#   usb-mouse, usb-tablet) and written ones (Xbox-style and generic gamepads,
#   consumer control, report IDs), the XInput adapter, the injection queue
#   and the kshim_key_source_poll() hook in kernel/drivers/keys.c.
# usbhid_stack_test: USB core + hub + HID + XInput over a fake HCD with fake
#   devices (enumeration, nested hubs, hotplug, STALL recovery, boot
#   protocol fallback, the menu events, quiesce order).
set(KSHIM_USBHID_HID_SOURCES
    "${KSHIM_ROOT}/kernel/drivers/hid/hid_parse.c"
    "${KSHIM_ROOT}/kernel/drivers/hid/hid_input.c"
    "${KSHIM_ROOT}/kernel/core/input_inject.c")
kshim_test(usbhid_hid_test io/usbhid_hid_test.c ${KSHIM_USBHID_HID_SOURCES}
    "${KSHIM_ROOT}/kernel/drivers/usb/class/hid.c"
    "${KSHIM_ROOT}/kernel/drivers/keys.c")
target_compile_definitions(usbhid_hid_test PRIVATE CONFIG_KSHIM_INPUT_INJECT=1)
target_include_directories(usbhid_hid_test PRIVATE "${KSHIM_ROOT}/tests/include")
kshim_test(usbhid_stack_test io/usbhid_stack_test.c ${KSHIM_USBHID_HID_SOURCES}
    "${KSHIM_ROOT}/kernel/drivers/usb/core/usb_core.c"
    "${KSHIM_ROOT}/kernel/drivers/usb/class/hub.c"
    "${KSHIM_ROOT}/kernel/drivers/usb/class/hid.c"
    "${KSHIM_ROOT}/kernel/drivers/keys.c"
    "${KSHIM_ROOT}/kernel/core/io.c")
target_compile_definitions(usbhid_stack_test PRIVATE CONFIG_KSHIM_INPUT_INJECT=1)
target_include_directories(usbhid_stack_test PRIVATE "${KSHIM_ROOT}/tests/include")
