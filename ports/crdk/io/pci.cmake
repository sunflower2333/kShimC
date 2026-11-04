# SPDX-License-Identifier: MIT
# Owner: pci. CrDK PcieLib for the Qualcomm root complex (docs/io-stack.md).
# Included by ports/crdk/CMakeLists.txt before CRDK_SOURCES is made absolute:
# append CrDK-relative paths; set_source_files_properties() takes
# "${CRDK_ROOT}/..." paths.
option(CRDK_PCIE "Build PcieLib (Qualcomm RC + QMP PHY) for kShimC" OFF)
if(CRDK_PCIE)
    # PcieLib itself plus the unmodified SM8450 and SM8250 target data. Each
    # target defines CrTargetGetPcieContext; it is renamed per SoC here, as
    # the TLMM targets are. PcieLib's EDK2-style "%a" log strings go through
    # KshimPcieCrLog (kernel/drivers/pci/qcom_pcielib.c), which prints them
    # as "%s". The SM8650 target data lives in kernel/drivers/pci.
    list(APPEND CRDK_SOURCES Library/PcieLib/pcie.c
        Target/Sm8450/CrPcieTarget.c Target/Sm8250/CrPcieTarget.c)
    set_source_files_properties("${CRDK_ROOT}/Library/PcieLib/pcie.c" PROPERTIES
        COMPILE_DEFINITIONS "CrOsLog=KshimPcieCrLog")
    foreach(soc Sm8450 Sm8250)
        set_source_files_properties("${CRDK_ROOT}/Target/${soc}/CrPcieTarget.c" PROPERTIES
            COMPILE_DEFINITIONS "CrTargetGetPcieContext=KshimCrdk${soc}PcieContext")
    endforeach()
endif()
