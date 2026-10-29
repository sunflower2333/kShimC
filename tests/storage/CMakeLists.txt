# SPDX-License-Identifier: MIT
# Host tests for the block layer, filesystem adapter and firmware loader.
# Standalone:   cmake -S tests/storage -B build/storage -G Ninja
#               cmake --build build/storage && ctest --test-dir build/storage
# Also included by tests/CMakeLists.txt.
cmake_minimum_required(VERSION 3.16)
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    project(kshim_storage_tests C)
    enable_testing()
    option(KSHIM_TEST_SANITIZERS "Use address and undefined behavior sanitizers" ON)
    if(KSHIM_TEST_SANITIZERS)
        add_compile_options(-fsanitize=address,undefined -fno-sanitize-recover=all
                            -fno-omit-frame-pointer)
        add_link_options(-fsanitize=address,undefined)
    endif()
    add_compile_options(-g -O1)
endif()
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
find_package(Python3 REQUIRED COMPONENTS Interpreter)

get_filename_component(KSHIM_STORAGE_ROOT "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)
include("${KSHIM_STORAGE_ROOT}/cmake/storage.cmake")
# Opt-in only: links GPL-2.0-or-later NhErofs.c into that one test binary.
option(KSHIM_STORAGE_TEST_EROFS "Also build the GPL EROFS test variant" OFF)

kshim_storage_nh_root(_nh "${KSHIM_STORAGE_ROOT}")
kshim_storage_include_dirs(_inc "${KSHIM_STORAGE_ROOT}" "${_nh}")
set(_fdt "${KSHIM_STORAGE_ROOT}/lib/libfdt")
add_library(kshim_storage_fdt STATIC ${_fdt}/fdt.c ${_fdt}/fdt_ro.c ${_fdt}/fdt_rw.c
    ${_fdt}/fdt_sw.c ${_fdt}/fdt_wip.c ${_fdt}/fdt_strerror.c ${_fdt}/fdt_empty_tree.c)
target_include_directories(kshim_storage_fdt PUBLIC ${_fdt}/include)

# kshim_storage_variant(<name> <features...>)
function(kshim_storage_variant name)
    kshim_storage_sources(_src "${KSHIM_STORAGE_ROOT}" "${_nh}" ${ARGN})
    if(NOT "EROFS" IN_LIST ARGN AND _src MATCHES "NhErofs\\.c")
        message(FATAL_ERROR "non-EROFS variant ${name} lists GPL NhErofs.c")
    endif()
    add_library(${name} STATIC ${_src})
    target_include_directories(${name} PUBLIC ${_inc}
        "${KSHIM_STORAGE_ROOT}/tests/include")
    set(_defs CONFIG_BLOCK=1)
    foreach(f FAT EXT4 F2FS EROFS)
        if(f IN_LIST ARGN)
            list(APPEND _defs CONFIG_FS_${f}=1 CONFIG_FS_CORE=1)
        endif()
    endforeach()
    if("FIRMWARE" IN_LIST ARGN)
        list(APPEND _defs CONFIG_FIRMWARE_LOADER=1)
        target_link_libraries(${name} PUBLIC kshim_storage_fdt)
    endif()
    list(REMOVE_DUPLICATES _defs)
    target_compile_definitions(${name} PUBLIC ${_defs})
    target_compile_options(${name} PRIVATE -Wall -Wextra -Werror)
    # FirmwareLoaderLib is compiled unmodified from CrDK; its style trips
    # GCC-only warnings (e.g. -Wmisleading-indentation), so keep them visible
    # but non-fatal for those files.
    set(_crdk_src ${_src})
    list(FILTER _crdk_src INCLUDE REGEX "/Library/FirmwareLoaderLib/")
    set_source_files_properties(${_crdk_src} PROPERTIES COMPILE_OPTIONS -Wno-error)
endfunction()

kshim_storage_variant(kshim_storage_block BLOCK)
kshim_storage_variant(kshim_storage_fat BLOCK FAT)
kshim_storage_variant(kshim_storage_default BLOCK FAT EXT4 F2FS FIRMWARE)

set(_fixtures "${CMAKE_CURRENT_BINARY_DIR}/fixtures")
add_test(NAME storage_fixtures
    COMMAND ${Python3_EXECUTABLE} "${CMAKE_CURRENT_SOURCE_DIR}/mkimages.py" --out "${_fixtures}")
set_tests_properties(storage_fixtures PROPERTIES FIXTURES_SETUP storage_images
    SKIP_RETURN_CODE 77 TIMEOUT 600)

function(kshim_storage_test name lib)
    add_executable(${name} ${ARGN})
    target_link_libraries(${name} PRIVATE ${lib})
    target_compile_options(${name} PRIVATE -Wall -Wextra -Werror -UNDEBUG)
    target_compile_definitions(${name} PRIVATE _POSIX_C_SOURCE=200809L _DEFAULT_SOURCE)
    add_test(NAME ${name} COMMAND ${name} "${_fixtures}")
    set_tests_properties(${name} PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 300)
endfunction()

kshim_storage_test(storage_blkdev_test kshim_storage_block blkdev_test.c)
kshim_storage_test(storage_gpt_test kshim_storage_default gpt_test.c)
kshim_storage_test(storage_fs_test kshim_storage_default fs_test.c)
kshim_storage_test(storage_fs_fatonly_test kshim_storage_fat fs_test.c)
kshim_storage_test(storage_firmware_test kshim_storage_default firmware_test.c)
foreach(t storage_gpt_test storage_fs_test storage_fs_fatonly_test storage_firmware_test)
    set_tests_properties(${t} PROPERTIES FIXTURES_REQUIRED storage_images)
endforeach()

# No GPL object in any default (non-opt-in) storage variant.
add_test(NAME storage_no_gpl
    COMMAND ${Python3_EXECUTABLE} "${CMAKE_CURRENT_SOURCE_DIR}/check_no_gpl.py"
        --compile-commands "${CMAKE_BINARY_DIR}/compile_commands.json"
        --archive $<TARGET_FILE:kshim_storage_default>
        --archive $<TARGET_FILE:kshim_storage_fat>
        --archive $<TARGET_FILE:kshim_storage_block>
        --elf $<TARGET_FILE:storage_fs_test>
        --elf $<TARGET_FILE:storage_firmware_test>
        --allow-target kshim_storage_erofs)

if(KSHIM_STORAGE_TEST_EROFS)
    kshim_storage_variant(kshim_storage_erofs BLOCK FAT EXT4 F2FS EROFS FIRMWARE)
    kshim_storage_test(storage_fs_erofs_test kshim_storage_erofs fs_test.c)
    set_tests_properties(storage_fs_erofs_test PROPERTIES FIXTURES_REQUIRED storage_images)
endif()
