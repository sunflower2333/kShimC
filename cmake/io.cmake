# SPDX-License-Identifier: MIT
#
# I/O stack build glue (docs/io-stack.md). The top-level CMakeLists.txt
# includes this after its own Kconfig-driven source lists and CRDK_* defaults
# and before ports/crdk, so each owner's fragment under cmake/io/ can:
#   - append to KERNEL_SOURCES (and USER_SOURCES) under its CONFIG_* symbols;
#   - switch CRDK_* options ON with set(... CACHE BOOL "" FORCE) (never OFF:
#     another subsystem may need the same CrDK library);
#   - add -Os or other per-file properties for its own sources.
# Fragments are plain include()s (no new scope). Keep edits inside your own
# fragment; this file and the list below are owned by the architect.
list(APPEND KERNEL_SOURCES kernel/core/io.c)
set(KSHIM_IO_FRAGMENTS
    platform      # sm8650: GPIO, pinctrl, clocks, regulators, SoC tables
    qup           # qup: QUP/GENI, I2C, SPI, UART, console
    pci           # pci: PCI core, ECAM and Qualcomm hosts
    usb           # xhci: USB core, xHCI, Renesas, DMA
    usbhid        # usbhid: hub, HID, input injection
    touch         # touch-st: touch framework, FTM4, FTM5
    touch_goodix) # touch-goodix: GT911, GT9916
foreach(fragment ${KSHIM_IO_FRAGMENTS})
    include(${CMAKE_SOURCE_DIR}/cmake/io/${fragment}.cmake)
endforeach()
# Fragments may list a shared source (kernel/dt/gpio.c) the base list has.
list(REMOVE_DUPLICATES KERNEL_SOURCES)
file(GLOB_RECURSE KSHIM_IO_KCONFIG CONFIGURE_DEPENDS
    "${CMAKE_SOURCE_DIR}/kernel/drivers/*/Kconfig*"
    "${CMAKE_SOURCE_DIR}/kernel/drivers/*/*/Kconfig*")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${KSHIM_IO_KCONFIG})
