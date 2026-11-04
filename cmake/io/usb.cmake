# SPDX-License-Identifier: MIT
# Owner: xhci. USB core (kernel/drivers/usb/core/), xHCI and the Renesas
# firmware download (kernel/drivers/usb/host/), DMA pool (kernel/core/dma.c).
# No CrDK code: RenesasXhciCrLib and the other CrDK USB host libraries are
# GPL-2.0-only. See docs/io-stack.md and docs/usb.md.
if(CONFIG_KSHIM_USB)
    list(APPEND KERNEL_SOURCES kernel/core/dma.c
        kernel/drivers/usb/core/core.c kernel/drivers/usb/core/desc.c
        kernel/drivers/usb/core/enumerate.c)
    if(CONFIG_KSHIM_USB_SELFTEST)
        list(APPEND KERNEL_SOURCES kernel/drivers/usb/core/selftest.c)
    endif()
endif()
if(CONFIG_KSHIM_USB_XHCI)
    list(APPEND KERNEL_SOURCES kernel/drivers/usb/host/xhci.c
        kernel/drivers/usb/host/xhci_ops.c kernel/drivers/usb/host/xhci_pci.c)
endif()
if(CONFIG_KSHIM_USB_XHCI_RENESAS)
    list(APPEND KERNEL_SOURCES kernel/drivers/usb/host/renesas_fw.c)
    # Optional build-time image (never committed): the Kconfig string is a
    # path; empty embeds nothing.
    set(KSHIM_RENESAS_FW_EMBED "${CONFIG_KSHIM_USB_XHCI_RENESAS_FW_EMBED}")
    if(KSHIM_RENESAS_FW_EMBED)
        get_filename_component(KSHIM_RENESAS_FW_EMBED "${KSHIM_RENESAS_FW_EMBED}" ABSOLUTE)
        if(NOT EXISTS "${KSHIM_RENESAS_FW_EMBED}")
            message(FATAL_ERROR "KSHIM_USB_XHCI_RENESAS_FW_EMBED: ${KSHIM_RENESAS_FW_EMBED} not found")
        endif()
        configure_file(kernel/drivers/usb/host/renesas_fw_blob.S.in
            ${CMAKE_BINARY_DIR}/renesas_fw_blob.S @ONLY)
        set_source_files_properties(${CMAKE_BINARY_DIR}/renesas_fw_blob.S PROPERTIES
            OBJECT_DEPENDS "${KSHIM_RENESAS_FW_EMBED}")
        list(APPEND KERNEL_SOURCES ${CMAKE_BINARY_DIR}/renesas_fw_blob.S)
        set_source_files_properties(kernel/drivers/usb/host/xhci_pci.c PROPERTIES
            COMPILE_DEFINITIONS KSHIM_RENESAS_FW_EMBEDDED=1)
    endif()
endif()
