# SPDX-License-Identifier: MIT
# Host tests for the UFS and SDHCI block drivers (register models, no MMIO).
include("${KSHIM_ROOT}/src/drivers/block/block.cmake")

add_library(kshim_blkdev_stub STATIC "${CMAKE_CURRENT_LIST_DIR}/blkdev_stub.c")
target_include_directories(kshim_blkdev_stub PUBLIC "${KSHIM_ROOT}/src/include" "${CMAKE_CURRENT_LIST_DIR}")

# Unmodified CrDK storage sources (UFS power libs, targets, SdhciMsmLib, ClockLib).
kshim_add_crdk_storage(kshim_test_crdk_storage ON)

kshim_test(ufshci_test ufshci_test.c "${KSHIM_ROOT}/src/drivers/block/ufshci.c")
target_link_libraries(ufshci_test PRIVATE kshim_blkdev_stub crdk)

kshim_test(sdmmc_test sdmmc_test.c "${KSHIM_ROOT}/src/drivers/block/sdmmc.c")
target_link_libraries(sdmmc_test PRIVATE kshim_blkdev_stub kshim_test_crdk_storage)

add_custom_command(OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/fixtures/kona-storage.dtb
    COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_CURRENT_BINARY_DIR}/fixtures
    COMMAND ${DTC_EXECUTABLE} -q -I dts -O dtb -o ${CMAKE_CURRENT_BINARY_DIR}/fixtures/kona-storage.dtb
        ${CMAKE_CURRENT_LIST_DIR}/fixtures/kona-storage.dts
    DEPENDS ${CMAKE_CURRENT_LIST_DIR}/fixtures/kona-storage.dts)
add_custom_target(storage_fixtures DEPENDS ${CMAKE_CURRENT_BINARY_DIR}/fixtures/kona-storage.dtb)
kshim_test(qcom_storage_test qcom_storage_test.c "${KSHIM_ROOT}/src/dt/storage.c"
    "${KSHIM_ROOT}/src/drivers/block/qcom_res.c")
add_dependencies(qcom_storage_test storage_fixtures)
target_include_directories(qcom_storage_test BEFORE PRIVATE "${KSHIM_ROOT}/src/drivers/block/crdk_shim")
target_compile_definitions(qcom_storage_test PRIVATE KSHIM_STORAGE_ANDROID_PINS=1
    "FIXTURES=\"${CMAKE_CURRENT_LIST_DIR}/fixtures\""
    "GENERATED_FIXTURES=\"${CMAKE_CURRENT_BINARY_DIR}/fixtures\"")
target_link_libraries(qcom_storage_test PRIVATE kshim_dt kshim_test_crdk_storage)

# QEMU smoke test of the same cores against PCI ufs/ufs-lu and sdhci-pci/sd-card.
find_program(KSHIM_QEMU_AARCH64 qemu-system-aarch64)
find_program(KSHIM_CROSS_CC aarch64-linux-gnu-gcc)
find_package(Python3 COMPONENTS Interpreter)
if(KSHIM_QEMU_AARCH64 AND KSHIM_CROSS_CC AND Python3_FOUND)
    add_test(NAME storage_qemu COMMAND ${Python3_EXECUTABLE} "${CMAKE_CURRENT_LIST_DIR}/storage_qemu.py"
        --crdk "${CRDK_ROOT}" --out "${CMAKE_CURRENT_BINARY_DIR}/storage-qemu"
        --cc "${KSHIM_CROSS_CC}" --qemu "${KSHIM_QEMU_AARCH64}")
    set_tests_properties(storage_qemu PROPERTIES TIMEOUT 300)
else()
    message(STATUS "storage_qemu test skipped: needs qemu-system-aarch64 and aarch64-linux-gnu-gcc")
endif()
