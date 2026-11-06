# SPDX-License-Identifier: MIT
# Owner: xhci. Host register-model tests (docs/io-stack.md "Test plan").
set(KSHIM_USB_CORE_SOURCES
    "${KSHIM_ROOT}/kernel/drivers/usb/core/core.c"
    "${KSHIM_ROOT}/kernel/drivers/usb/core/desc.c"
    "${KSHIM_ROOT}/kernel/drivers/usb/core/enumerate.c"
    "${KSHIM_ROOT}/kernel/core/dma.c"
    "${KSHIM_ROOT}/kernel/core/io.c")

# DMA pool: alignment, boundary, limit, ownership, exhaustion.
kshim_test(usb_dma_test io/usb_dma_test.c "${KSHIM_ROOT}/kernel/core/dma.c")
target_include_directories(usb_dma_test PRIVATE "${KSHIM_ROOT}/tests/include")

# Renesas uPD720201/202 firmware download against a config-register model,
# and the xHCI PCI glue's inherit/ROM/download/refuse decisions.
kshim_test(usb_renesas_test io/usb_renesas_test.c
    "${KSHIM_ROOT}/kernel/drivers/usb/host/renesas_fw.c"
    "${KSHIM_ROOT}/kernel/drivers/usb/host/xhci_pci.c"
    "${KSHIM_ROOT}/kernel/core/dma.c" "${KSHIM_ROOT}/kernel/core/io.c")
target_include_directories(usb_renesas_test PRIVATE "${KSHIM_ROOT}/tests/include")
target_compile_definitions(usb_renesas_test PRIVATE CONFIG_KSHIM_USB_XHCI_RENESAS=1
    CONFIG_FIRMWARE_LOADER=1 XHCI_PCI_MAX=8U
    "CONFIG_KSHIM_USB_XHCI_RENESAS_FW=\"renesas_usb_fw.mem\"")

# xHCI driver + USB core against a behavioural controller model, one
# scenario per test (see the file header).
kshim_test(usb_xhci_test io/usb_xhci_test.c io/xhci_model.c
    "${KSHIM_ROOT}/kernel/drivers/usb/host/xhci.c"
    "${KSHIM_ROOT}/kernel/drivers/usb/host/xhci_ops.c"
    ${KSHIM_USB_CORE_SOURCES})
target_include_directories(usb_xhci_test PRIVATE "${KSHIM_ROOT}/tests/include")
set_tests_properties(usb_xhci_test PROPERTIES ARGS irq)
foreach(scenario poll lostirq takeover dead stuck runwindow ehb gone)
    add_test(NAME usb_xhci_${scenario}_test COMMAND usb_xhci_test ${scenario})
endforeach()

# Full stack (io-evidence): PCI core + ECAM host + xhci_pci.c (Renesas,
# firmware running) + xHCI + USB core with the bring-up trace + hub +
# HID/XInput + quirks + HID mapping + input injection + the menu key poll,
# through kshim_io_init()/kshim_io_quiesce(), on the Pocket S2-shaped
# fixture. xhci_pci.c is built through io/usb_gamepad_xhci_pci.c, which
# routes BAR0 to the model; no kernel test hook. One scenario per process.
set(KSHIM_USB_GAMEPAD_FIXTURE "${CMAKE_CURRENT_BINARY_DIR}/fixtures/io/usb-gamepad.dtb")
add_custom_command(OUTPUT "${KSHIM_USB_GAMEPAD_FIXTURE}"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_CURRENT_BINARY_DIR}/fixtures/io"
    COMMAND ${DTC_EXECUTABLE} -q -I dts -O dtb -o "${KSHIM_USB_GAMEPAD_FIXTURE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/io/usb-gamepad.dts"
    DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/io/usb-gamepad.dts")
add_custom_target(usb_gamepad_fixture DEPENDS "${KSHIM_USB_GAMEPAD_FIXTURE}")
set(KSHIM_PCI_DIR "${KSHIM_ROOT}/kernel/drivers/pci")
kshim_test(usb_gamepad_test io/usb_gamepad_test.c io/xhci_model.c io/usb_gamepad_quirks.c
    "${KSHIM_PCI_DIR}/core.c" "${KSHIM_PCI_DIR}/dt.c" "${KSHIM_PCI_DIR}/alloc.c"
    "${KSHIM_PCI_DIR}/map.c" "${KSHIM_PCI_DIR}/ecam.c"
    "${KSHIM_ROOT}/kernel/drivers/usb/host/xhci.c"
    "${KSHIM_ROOT}/kernel/drivers/usb/host/xhci_ops.c"
    io/usb_gamepad_xhci_pci.c
    "${KSHIM_ROOT}/kernel/drivers/usb/host/renesas_fw.c"
    ${KSHIM_USB_CORE_SOURCES}
    "${KSHIM_ROOT}/kernel/drivers/usb/core/trace.c"
    "${KSHIM_ROOT}/kernel/drivers/usb/class/hub.c"
    "${KSHIM_ROOT}/kernel/drivers/usb/class/hid.c"
    "${KSHIM_ROOT}/kernel/drivers/hid/hid_parse.c"
    "${KSHIM_ROOT}/kernel/drivers/hid/hid_input.c"
    "${KSHIM_ROOT}/kernel/core/input_inject.c"
    "${KSHIM_ROOT}/kernel/drivers/keys.c")
target_compile_definitions(usb_gamepad_test PRIVATE CONFIG_KSHIM_PCI=1 CONFIG_KSHIM_PCI_ECAM=1
    CONFIG_KSHIM_USB=1 CONFIG_KSHIM_USB_XHCI=1 CONFIG_KSHIM_USB_XHCI_RENESAS=1
    CONFIG_KSHIM_USB_HUB=1 CONFIG_KSHIM_USB_HID=1 CONFIG_KSHIM_INPUT_INJECT=1
    CONFIG_KSHIM_USB_TRACE=1 CONFIG_KSHIM_USB_TRACE_REPORTS=12
    "FIXTURES=\"${CMAKE_CURRENT_BINARY_DIR}/fixtures/io\"")
target_include_directories(usb_gamepad_test PRIVATE "${KSHIM_ROOT}/tests/include")
target_link_libraries(usb_gamepad_test PRIVATE kshim_dt)
add_dependencies(usb_gamepad_test usb_gamepad_fixture)
set_tests_properties(usb_gamepad_test PROPERTIES ARGS irq)
foreach(scenario poll takeover nohalt)
    add_test(NAME usb_gamepad_${scenario}_test COMMAND usb_gamepad_test ${scenario})
endforeach()
