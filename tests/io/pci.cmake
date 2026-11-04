# SPDX-License-Identifier: MIT
# Owner: pci. Host register-model tests (docs/io-stack.md "Test plan").
set(KSHIM_PCI_FIXTURES "${CMAKE_CURRENT_BINARY_DIR}/fixtures/io")
set(KSHIM_PCI_DIR "${KSHIM_ROOT}/kernel/drivers/pci")
foreach(fixture pci-ecam pci-sm8650)
    add_custom_command(OUTPUT "${KSHIM_PCI_FIXTURES}/${fixture}.dtb"
        COMMAND ${CMAKE_COMMAND} -E make_directory "${KSHIM_PCI_FIXTURES}"
        COMMAND ${DTC_EXECUTABLE} -q -I dts -O dtb -o "${KSHIM_PCI_FIXTURES}/${fixture}.dtb"
            "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/io/${fixture}.dts"
        DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/io/${fixture}.dts")
    list(APPEND KSHIM_PCI_FIXTURE_DTBS "${KSHIM_PCI_FIXTURES}/${fixture}.dtb")
endforeach()
add_custom_target(pci_fixtures DEPENDS ${KSHIM_PCI_FIXTURE_DTBS})
set(KSHIM_PCI_CORE_SOURCES "${KSHIM_PCI_DIR}/core.c" "${KSHIM_PCI_DIR}/dt.c"
    "${KSHIM_PCI_DIR}/alloc.c" "${KSHIM_PCI_DIR}/map.c" "${KSHIM_ROOT}/kernel/core/io.c")

# Core over an ECAM register model: enumeration, assignment, INTx, journal.
kshim_test(pci_core_test io/pci_core_test.c ${KSHIM_PCI_CORE_SOURCES} "${KSHIM_PCI_DIR}/ecam.c")
target_compile_definitions(pci_core_test PRIVATE CONFIG_KSHIM_PCI=1 CONFIG_KSHIM_PCI_ECAM=1
    "FIXTURES=\"${KSHIM_PCI_FIXTURES}\"")

# Inherit first: kept BARs, bus numbers and windows; partial reassignment.
kshim_test(pci_inherit_test io/pci_inherit_test.c ${KSHIM_PCI_CORE_SOURCES} "${KSHIM_PCI_DIR}/ecam.c")
target_compile_definitions(pci_inherit_test PRIVATE CONFIG_KSHIM_PCI=1 CONFIG_KSHIM_PCI_ECAM=1
    "FIXTURES=\"${KSHIM_PCI_FIXTURES}\"")

# Qualcomm host over a DWC/PARF/iATU/QMP model with fake clk/regulator/gpio:
# inherited link (1), inherited without a memory iATU region (2), cold start
# through the unmodified CrDK PcieLib and the kShimC SM8650 target (3).
set(KSHIM_PCI_CRDK_SOURCES "${CRDK_ROOT}/Library/PcieLib/pcie.c"
    "${CRDK_ROOT}/Target/Sm8450/CrPcieTarget.c" "${CRDK_ROOT}/Target/Sm8250/CrPcieTarget.c")
set_source_files_properties(${KSHIM_PCI_CRDK_SOURCES} PROPERTIES COMPILE_OPTIONS "-w")
set_source_files_properties("${CRDK_ROOT}/Library/PcieLib/pcie.c" PROPERTIES
    COMPILE_DEFINITIONS "CrOsLog=KshimPcieCrLog")
foreach(soc Sm8450 Sm8250)
    set_source_files_properties("${CRDK_ROOT}/Target/${soc}/CrPcieTarget.c" PROPERTIES
        COMPILE_DEFINITIONS "CrTargetGetPcieContext=KshimCrdk${soc}PcieContext")
endforeach()
set(KSHIM_PCI_QCOM_TESTS)
foreach(scenario 1 2 3)
    kshim_test(pci_qcom_test_${scenario} io/pci_qcom_test.c ${KSHIM_PCI_CORE_SOURCES}
        "${KSHIM_PCI_DIR}/qcom.c" "${KSHIM_PCI_DIR}/qcom_pcielib.c"
        "${KSHIM_PCI_DIR}/qcom_sm8650.c" ${KSHIM_PCI_CRDK_SOURCES})
    target_compile_definitions(pci_qcom_test_${scenario} PRIVATE SCENARIO=${scenario}
        CONFIG_KSHIM_PCI=1 CONFIG_KSHIM_PCI_QCOM=1 CONFIG_KSHIM_REGULATOR=1
        CONFIG_KSHIM_GPIO=1 CONFIG_KSHIM_CLK=1 "FIXTURES=\"${KSHIM_PCI_FIXTURES}\"")
    list(APPEND KSHIM_PCI_QCOM_TESTS pci_qcom_test_${scenario})
endforeach()

foreach(target pci_core_test pci_inherit_test ${KSHIM_PCI_QCOM_TESTS})
    target_include_directories(${target} PRIVATE "${KSHIM_ROOT}/tests/include")
    target_link_libraries(${target} PRIVATE kshim_dt)
    add_dependencies(${target} pci_fixtures)
endforeach()
