# SPDX-License-Identifier: MIT
# Owner: sm8650. Extra CrDK sources for regulators/GPIO/SoC data, for example
# RpmhLib rpmh_rail*.c or Target/Sm8650 tables (docs/io-stack.md). Same
# contract as ports/crdk/io/pci.cmake.
#
# PmicGpioLib (kshim/gpio.h PMIC pins) is part of CRDK_SPMI; CmdDBLib and
# the polled RpmhLib API (kshim/regulator.h) are part of CRDK_GENI, which
# cmake/io/platform.cmake switches on. The SM8650 TLMM target is host-test
# data only (irqchip_test compares kShimC's TLMM layout with it); it
# defines CrSm8650GetSocGpioContext, so no rename is needed.
if(CRDK_GPIO_TARGETS)
    list(APPEND CRDK_SOURCES Target/Sm8650/CrGpioTarget.c)
endif()
if(CRDK_GPIO_TARGETS)
    # The freestanding OSKAL lacks EDK2's MAX_UINT8, which only this
    # generated table uses (0xFF: "no wake bit").
    set_source_files_properties("${CRDK_ROOT}/Target/Sm8650/CrGpioTarget.c" PROPERTIES
        COMPILE_DEFINITIONS "MAX_UINT8=0xFF")
endif()
