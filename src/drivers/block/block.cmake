# SPDX-License-Identifier: MIT
# UFS / SDHCI block drivers. Include after ports/crdk (needs CRDK_ROOT and the
# crdk target). Firmware: include with CONFIG_BLK_* set; host tests call the
# functions directly.
get_filename_component(KSHIM_BLOCK_DIR "${CMAKE_CURRENT_LIST_DIR}" ABSOLUTE)
get_filename_component(KSHIM_BLOCK_ROOT "${KSHIM_BLOCK_DIR}/../../.." ABSOLUTE)

# Unmodified CrDK sources: MIT-tagged target data and libraries, plus
# SdhciMsm.c ("MIT" in its header; see docs/storage.md for provenance).
function(kshim_add_crdk_storage Target Clocks)
    set(src "${CRDK_ROOT}")
    set(files
        Library/UfsPowerFdtCrLib/UfsPowerFdt.c
        Library/UfsPowerPolicyCrLib/UfsPowerPolicy.c
        Library/SdhciMsmLib/SdhciMsm.c
        Target/Sm8250/CrUfsTarget.c Target/Sm8250/CrSdTarget.c
        Target/Sm8450/CrUfsTarget.c Target/Sm8450/CrSdTarget.c)
    if(Clocks)
        list(APPEND files Target/Sm8250/CrClockTarget.c)
    endif()
    foreach(f IN LISTS files)
        if(NOT EXISTS "${src}/${f}")
            message(FATAL_ERROR "CrDK storage source ${f} missing: CONFIG_BLK_UFS/SDHCI need CrDK >= f23922d")
        endif()
    endforeach()
    # ClockLib pieces the port's crdk library does not already build.
    if(Clocks)
        get_target_property(crdk_sources crdk SOURCES)
        foreach(f clock.c gdsc.c clock_config.c clock_votes.c fixed_pll.c)
            if(NOT "${src}/Library/ClockLib/${f}" IN_LIST crdk_sources)
                list(APPEND files Library/ClockLib/${f})
            endif()
        endforeach()
    endif()
    list(TRANSFORM files PREPEND "${src}/")
    add_library(${Target} STATIC ${files})
    target_include_directories(${Target} BEFORE PRIVATE "${KSHIM_BLOCK_DIR}/crdk_shim")
    target_include_directories(${Target} PRIVATE "${KSHIM_BLOCK_ROOT}/lib/libfdt/include")
    target_link_libraries(${Target} PUBLIC crdk)
    # -Os after the build type flags: the 4 MiB runtime slot is tight.
    target_compile_options(${Target} PRIVATE -ffunction-sections -fdata-sections -w -Os)
    # Several SoC targets in one image: rename each file's CrTarget* entry points.
    set_property(SOURCE "${src}/Target/Sm8250/CrUfsTarget.c" APPEND PROPERTY COMPILE_DEFINITIONS
        CrTargetGetUfsContext=kshim_crdk_sm8250_ufs)
    set_property(SOURCE "${src}/Target/Sm8450/CrUfsTarget.c" APPEND PROPERTY COMPILE_DEFINITIONS
        CrTargetGetUfsContext=kshim_crdk_sm8450_ufs)
    set_property(SOURCE "${src}/Target/Sm8250/CrSdTarget.c" APPEND PROPERTY COMPILE_DEFINITIONS
        CrTargetGetSdContext=kshim_crdk_sm8250_sd)
    set_property(SOURCE "${src}/Target/Sm8450/CrSdTarget.c" APPEND PROPERTY COMPILE_DEFINITIONS
        CrTargetGetSdContext=kshim_crdk_sm8450_sd)
    set_property(SOURCE "${src}/Target/Sm8250/CrClockTarget.c" APPEND PROPERTY COMPILE_DEFINITIONS
        CrTargetGetClockContext=kshim_crdk_sm8250_clock
        CrTargetClockInit=kshim_crdk_sm8250_clock_init
        CrTargetClockReset=kshim_crdk_sm8250_clock_reset
        CrTargetGetDebugccContext=kshim_crdk_sm8250_debugcc
        CrTargetClockResolveResource=kshim_crdk_sm8250_clock_resolve)
endfunction()

# kShimC block sources. Ufs/Sdhci select the controller cores.
function(kshim_add_block Target Ufs Sdhci AndroidPins Clocks)
    set(files "${KSHIM_BLOCK_DIR}/storage.c" "${KSHIM_BLOCK_DIR}/qcom_res.c"
        "${KSHIM_BLOCK_ROOT}/src/dt/storage.c")
    if(Ufs)
        list(APPEND files "${KSHIM_BLOCK_DIR}/ufshci.c" "${KSHIM_BLOCK_DIR}/ufs_qcom.c")
    endif()
    if(Sdhci)
        list(APPEND files "${KSHIM_BLOCK_DIR}/sdmmc.c" "${KSHIM_BLOCK_DIR}/sdhci_msm.c")
    endif()
    add_library(${Target} STATIC ${files})
    target_include_directories(${Target} BEFORE PRIVATE "${KSHIM_BLOCK_DIR}/crdk_shim")
    target_include_directories(${Target} PRIVATE "${KSHIM_BLOCK_ROOT}/src/include"
        "${KSHIM_BLOCK_ROOT}/lib/libfdt/include")
    target_compile_definitions(${Target} PRIVATE KSHIM_STORAGE_ANDROID_PINS=$<BOOL:${AndroidPins}>
        KSHIM_STORAGE_CLOCKS=$<BOOL:${Clocks}>)
    target_compile_options(${Target} PRIVATE -ffunction-sections -fdata-sections -Os)
    target_link_libraries(${Target} PUBLIC crdk)
endfunction()

if(DEFINED CONFIG_BLK_UFS OR DEFINED CONFIG_BLK_SDHCI)
    if(CONFIG_BLK_UFS OR CONFIG_BLK_SDHCI)
        if(CONFIG_BLK_QCOM_CLOCKS)
            set(_kshim_clocks ON)
        else()
            set(_kshim_clocks OFF)
        endif()
        kshim_add_crdk_storage(kshim_crdk_storage ${_kshim_clocks})
        target_link_libraries(kshim_crdk_storage PRIVATE c)
        if(CRDK_GPIO OR CRDK_GENI)
            set(_kshim_pins ON)
        else()
            set(_kshim_pins OFF)
        endif()
        kshim_add_block(kshim_block "${CONFIG_BLK_UFS}" "${CONFIG_BLK_SDHCI}" ${_kshim_pins} ${_kshim_clocks})
        target_include_directories(kshim_block PRIVATE ${INCLUDE_DIRS})
        target_link_libraries(kshim_block PRIVATE c)
        set(KSHIM_BLOCK_LIBRARIES kshim_block kshim_crdk_storage)
        # src/dt/gpio.c (kshim_dt_gpio) resolves the SD card-detect pin.
        if(_kshim_pins)
            list(APPEND SRC_FILES src/dt/gpio.c)
            list(REMOVE_DUPLICATES SRC_FILES)
        endif()
    endif()
endif()
