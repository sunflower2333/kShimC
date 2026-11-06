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
