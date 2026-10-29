# SPDX-License-Identifier: MIT
#
# Block layer, read-only filesystems and firmware loader.
#
#   kshim_storage_sources(<out-var> <kshim-root> <firmwareloaderlib-root>
#                         [BLOCK] [FAT] [EXT4] [F2FS] [EROFS] [FIRMWARE])
# returns the source list for the selected features.  FirmwareLoaderLib is
# compiled unmodified from <firmwareloaderlib-root>/Library/FirmwareLoaderLib.
# NhErofs.c (GPL-2.0-or-later) is listed only with EROFS; otherwise
# kernel/fs/nh_stubs.c provides MIT stand-ins for every disabled reader.
#
#   kshim_storage_include_dirs(<out-var> <kshim-root> <firmwareloaderlib-root>)
#
# The firmware build (top-level CMakeLists.txt) includes this file after
# ports/crdk so CRDK_ROOT (the materialized, pinned CrDK tree) is known, and
# builds a kshim_storage static library from the Kconfig selection.

function(kshim_storage_sources out root nhroot)
    cmake_parse_arguments(S "BLOCK;FAT;EXT4;F2FS;EROFS;FIRMWARE" "" "" ${ARGN})
    set(sources)
    if(S_BLOCK)
        list(APPEND sources ${root}/kernel/block/blkdev.c ${root}/kernel/block/memdisk.c
            ${root}/kernel/block/gpt.c ${root}/kernel/block/storage.c)
    endif()
    if(S_FAT OR S_EXT4 OR S_F2FS OR S_EROFS)
        set(lib ${nhroot}/Library/FirmwareLoaderLib)
        list(APPEND sources ${root}/kernel/fs/fs.c ${root}/kernel/fs/lp.c
            ${root}/kernel/fs/nh_stubs.c ${lib}/NhBase.c ${lib}/NhFs.c
            ${lib}/NhGpt.c ${lib}/NhBundle.c ${lib}/NhPolicy.c)
        if(S_FAT)
            list(APPEND sources ${lib}/NhFat.c)
        endif()
        if(S_EXT4)
            list(APPEND sources ${lib}/NhExt4.c)
        endif()
        if(S_F2FS)
            list(APPEND sources ${lib}/NhF2fs.c)
        endif()
        if(S_EROFS)
            # GPL-2.0-or-later: the linked binary becomes GPL (CONFIG_FS_EROFS).
            list(APPEND sources ${lib}/NhErofs.c)
        endif()
        if(S_FIRMWARE)
            list(APPEND sources ${root}/kernel/firmware/firmware.c
                ${root}/kernel/firmware/bootslot.c)
        endif()
    endif()
    set(${out} ${sources} PARENT_SCOPE)
endfunction()

function(kshim_storage_include_dirs out root nhroot)
    set(${out} ${root}/kernel/include ${root}/include ${nhroot}/Include
        ${nhroot}/Library/FirmwareLoaderLib ${root}/lib/libfdt/include PARENT_SCOPE)
endfunction()

# Pick the FirmwareLoaderLib tree: the materialized CrDK copy when the port
# has prepared one, else the submodule (FirmwareLoaderLib is not patched).
function(kshim_storage_nh_root out root)
    if(DEFINED CRDK_ROOT AND EXISTS "${CRDK_ROOT}/Library/FirmwareLoaderLib/NhFs.c")
        set(${out} "${CRDK_ROOT}" PARENT_SCOPE)
    elseif(EXISTS "${root}/lib/CrDK/Library/FirmwareLoaderLib/NhFs.c")
        set(${out} "${root}/lib/CrDK" PARENT_SCOPE)
    else()
        message(FATAL_ERROR "CrDK FirmwareLoaderLib not found; CrDK feature/2610 "
                            "(f23922d) or later is required for CONFIG_FS_*")
    endif()
endfunction()

# Firmware build: only when included from the top-level project.
if(DEFINED OUTPUT_NAME AND CONFIG_BLOCK)
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
        "${CMAKE_SOURCE_DIR}/kernel/block/Kconfig")
    set(_storage_features BLOCK)
    foreach(_feature FAT EXT4 F2FS EROFS)
        if(CONFIG_FS_${_feature})
            list(APPEND _storage_features ${_feature})
        endif()
    endforeach()
    if(CONFIG_FIRMWARE_LOADER)
        list(APPEND _storage_features FIRMWARE)
    endif()
    if(CONFIG_FS_EROFS)
        message(WARNING "CONFIG_FS_EROFS=y links GPL-2.0-or-later NhErofs.c: "
                        "the resulting kShimC binary must be distributed under GPL-2.0-or-later")
    endif()
    if(CONFIG_FS_CORE)
        kshim_storage_nh_root(KSHIM_STORAGE_NH_ROOT "${CMAKE_SOURCE_DIR}")
    else()
        set(KSHIM_STORAGE_NH_ROOT "${CMAKE_SOURCE_DIR}/lib/CrDK")
    endif()
    kshim_storage_sources(KSHIM_STORAGE_SOURCES "${CMAKE_SOURCE_DIR}"
        "${KSHIM_STORAGE_NH_ROOT}" ${_storage_features})
    kshim_storage_include_dirs(_storage_includes "${CMAKE_SOURCE_DIR}" "${KSHIM_STORAGE_NH_ROOT}")
    add_library(kshim_storage STATIC ${KSHIM_STORAGE_SOURCES})
    target_include_directories(kshim_storage PRIVATE ${_storage_includes})
    target_link_libraries(kshim_storage PRIVATE c)
    set(KSHIM_STORAGE_LIBRARIES kshim_storage)
endif()
