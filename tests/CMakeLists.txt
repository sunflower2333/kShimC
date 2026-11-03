cmake_minimum_required(VERSION 3.16)
project(kshim_host_tests C ASM)
enable_testing()

get_filename_component(KSHIM_ROOT "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)
# Host tests unit-test internals on both sides of the kernel/user boundary.
set(KSHIM_TEST_INCLUDE_DIRS "${KSHIM_ROOT}/include" "${KSHIM_ROOT}/kernel/include"
    "${KSHIM_ROOT}/user/include")
set(KSHIM_TEST_MENU_STYLE "FLAT" CACHE STRING "Boot menu preset used by host UI tests")
set(KSHIM_TEST_MENU_STYLES FLAT CLASSIC CARDS TERMINAL MINIMAL HIGH_CONTRAST
    FLUENT MATERIAL3 SURFACE CUPERTINO GLASS AURORA SOFT_UI BENTO NEON NORD
    MATERIAL2 MATERIAL2_DARK FLUENT2 FLUENT2_DARK FLUENT_DARK SURFACE_DARK HARMONYOS CLOVER IOS_HIG
    METRO ADWAITA HOLO LVGL_DEFAULT)
set_property(CACHE KSHIM_TEST_MENU_STYLE PROPERTY STRINGS ${KSHIM_TEST_MENU_STYLES})
if(NOT KSHIM_TEST_MENU_STYLE IN_LIST KSHIM_TEST_MENU_STYLES)
    message(FATAL_ERROR "Unknown KSHIM_TEST_MENU_STYLE: ${KSHIM_TEST_MENU_STYLE}")
endif()
option(KSHIM_TEST_SANITIZERS "Use address and undefined behavior sanitizers" ON)
add_compile_definitions(KSHIM_HOST_TEST=1 CONFIG_QCOM_SPMI=1 KSHIM_STANDALONE=1
    CONFIG_KSHIM_LVGL_BUFFER_BYTES=262144)
if(KSHIM_TEST_SANITIZERS)
    add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer)
    add_link_options(-fsanitize=address,undefined)
endif()
add_compile_options(-g -O1 -ffunction-sections -fdata-sections)
add_link_options(-Wl,--gc-sections)

if(KSHIM_TEST_MENU_STYLE STREQUAL "LVGL_DEFAULT")
    add_compile_definitions(CONFIG_KSHIM_MENU_STYLE_LVGL_DEFAULT=1)
endif()

set(LV_CONF_PATH "${KSHIM_ROOT}/user/include/lv_conf.h" CACHE FILEPATH "" FORCE)
set(LV_CONF_BUILD_DISABLE_THORVG_INTERNAL ON CACHE BOOL "" FORCE)
set(LV_CONF_BUILD_DISABLE_EXAMPLES ON CACHE BOOL "" FORCE)
set(LV_CONF_BUILD_DISABLE_DEMOS ON CACHE BOOL "" FORCE)
add_subdirectory("${KSHIM_ROOT}/third_party/lvgl" lvgl)
set(CRDK_USB_ACM ON CACHE BOOL "" FORCE)
add_subdirectory("${KSHIM_ROOT}/ports/crdk" crdk)
set(KSHIM_UI_FONT_BLOB "${KSHIM_ROOT}/user/ui/fonts/noto_sans_cjk_sc.ksf")
configure_file("${KSHIM_ROOT}/user/ui/fonts/ui_font_blob.S.in"
    "${CMAKE_CURRENT_BINARY_DIR}/ui_font_blob.S" @ONLY)
set_source_files_properties("${CMAKE_CURRENT_BINARY_DIR}/ui_font_blob.S"
    PROPERTIES OBJECT_DEPENDS "${KSHIM_UI_FONT_BLOB}")
add_library(kshim_backdrop STATIC "${KSHIM_ROOT}/user/ui/backdrop.c"
    "${KSHIM_ROOT}/user/ui/font.c" "${CMAKE_CURRENT_BINARY_DIR}/ui_font_blob.S")
target_include_directories(kshim_backdrop PUBLIC ${KSHIM_TEST_INCLUDE_DIRS})
target_link_libraries(kshim_backdrop PUBLIC lvgl)

# 创建带断言和严格告警的主机测试目标。
function(kshim_test Target)
    add_executable(${Target} ${ARGN})
    target_compile_options(${Target} PRIVATE -Wall -Wextra -Werror -UNDEBUG)
    target_compile_definitions(${Target} PRIVATE _POSIX_C_SOURCE=200809L)
    target_include_directories(${Target} PRIVATE ${KSHIM_TEST_INCLUDE_DIRS})
    add_test(NAME ${Target} COMMAND ${Target})
endfunction()

kshim_test(qualcomm_test qualcomm_test.c "${KSHIM_ROOT}/kernel/drivers/keys.c" "${KSHIM_ROOT}/kernel/dt/keys.c")
target_include_directories(qualcomm_test PRIVATE "${KSHIM_ROOT}/lib/libfdt/include")
kshim_test(spmi_test spmi_test.c "${KSHIM_ROOT}/kernel/drivers/qcom_keys.c")
target_link_libraries(spmi_test PRIVATE crdk_spmi)
kshim_test(framebuffer_test framebuffer_test.c "${KSHIM_ROOT}/user/ui/framebuffer.c")

set(KSHIM_TEST_UI_SOURCES "${KSHIM_ROOT}/user/ui/lvgl_port.c"
    "${KSHIM_ROOT}/user/ui/framebuffer.c" "${KSHIM_ROOT}/kernel/drivers/keys.c")
kshim_test(lvgl_test lvgl_test.c ${KSHIM_TEST_UI_SOURCES} "${KSHIM_ROOT}/kernel/drivers/touch.c")
kshim_test(lvgl_hdk_test lvgl_test.c ${KSHIM_TEST_UI_SOURCES} "${KSHIM_ROOT}/kernel/drivers/touch.c")
kshim_test(menu_style_render_test menu_style_render_test.c ${KSHIM_TEST_UI_SOURCES})
kshim_test(menu_modern_test menu_modern_test.c ${KSHIM_TEST_UI_SOURCES})
add_executable(ui_preview_test ui_preview_test.c ${KSHIM_TEST_UI_SOURCES})
target_include_directories(ui_preview_test PRIVATE ${KSHIM_TEST_INCLUDE_DIRS})
foreach(target lvgl_test lvgl_hdk_test menu_style_render_test menu_modern_test ui_preview_test)
    target_link_libraries(${target} PRIVATE lvgl kshim_backdrop m)
    target_compile_definitions(${target} PRIVATE CONFIG_KSHIM_MENU_STYLE_${KSHIM_TEST_MENU_STYLE}=1)
endforeach()
foreach(target lvgl_hdk_test ui_preview_test)
    target_compile_definitions(${target} PRIVATE CONFIG_KSHIM_UEFI_TEST_MENU=1
        "CONFIG_KSHIM_MENU_ENTRY1=\"UEFI A\"" "CONFIG_KSHIM_MENU_ENTRY2=\"UEFI B\"")
endforeach()

kshim_test(qcom_bus_test qcom_bus_test.c)
kshim_test(qcom_touch_platform_test qcom_touch_platform_test.c)
kshim_test(hdk8250_touch_platform_test hdk8250_touch_platform_test.c)
kshim_test(touch_test touch_test.c "${KSHIM_ROOT}/kernel/drivers/touch.c")
kshim_test(st_ftm5_test st_ftm5_test.c "${KSHIM_ROOT}/kernel/drivers/st_ftm5.c" "${KSHIM_ROOT}/kernel/drivers/touch.c")

add_library(kshim_dt STATIC
    "${KSHIM_ROOT}/kernel/dt/dt.c" "${KSHIM_ROOT}/kernel/dt/spmi.c"
    "${KSHIM_ROOT}/kernel/dt/gpio.c" "${KSHIM_ROOT}/kernel/dt/touch.c" "${KSHIM_ROOT}/kernel/dt/console.c"
    "${KSHIM_ROOT}/kernel/dt/splash.c"
    "${KSHIM_ROOT}/lib/libfdt/fdt.c" "${KSHIM_ROOT}/lib/libfdt/fdt_check.c"
    "${KSHIM_ROOT}/lib/libfdt/fdt_ro.c" "${KSHIM_ROOT}/lib/libfdt/fdt_rw.c"
    "${KSHIM_ROOT}/lib/libfdt/fdt_sw.c" "${KSHIM_ROOT}/lib/libfdt/fdt_wip.c"
    "${KSHIM_ROOT}/lib/libfdt/fdt_empty_tree.c" "${KSHIM_ROOT}/lib/libfdt/fdt_addresses.c")
target_include_directories(kshim_dt PUBLIC ${KSHIM_TEST_INCLUDE_DIRS} "${KSHIM_ROOT}/lib/libfdt/include")
target_link_libraries(kshim_dt PUBLIC crdk)
foreach(target qualcomm_test qcom_bus_test qcom_touch_platform_test hdk8250_touch_platform_test st_ftm5_test)
    target_link_libraries(${target} PRIVATE kshim_dt)
endforeach()
add_link_options(-Wl,--gc-sections)
kshim_test(dt_test dt_test.c "${KSHIM_ROOT}/kernel/dt/keys.c" "${KSHIM_ROOT}/kernel/drivers/keys.c")
target_compile_definitions(dt_test PRIVATE "FIXTURES=\"${CMAKE_CURRENT_SOURCE_DIR}/fixtures\"")
target_link_libraries(dt_test PRIVATE kshim_dt)
kshim_test(crdk_poll_test crdk_poll_test.c)
target_link_libraries(crdk_poll_test PRIVATE crdk)
find_program(DTC_EXECUTABLE dtc REQUIRED)
add_custom_command(OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/fixtures/kona-resources.dtb
    COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_CURRENT_BINARY_DIR}/fixtures
    COMMAND ${DTC_EXECUTABLE} -q -I dts -O dtb -o ${CMAKE_CURRENT_BINARY_DIR}/fixtures/kona-resources.dtb
        ${CMAKE_CURRENT_SOURCE_DIR}/fixtures/kona-resources.dts
    DEPENDS fixtures/kona-resources.dts)
add_custom_target(dt_fixtures DEPENDS ${CMAKE_CURRENT_BINARY_DIR}/fixtures/kona-resources.dtb)
add_dependencies(dt_test dt_fixtures)
target_compile_definitions(dt_test PRIVATE "GENERATED_FIXTURES=\"${CMAKE_CURRENT_BINARY_DIR}/fixtures\"")
# USB CDC-ACM console: kShimC backend + CrDK engine against CrDK's DWC3/host model.
add_custom_command(OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/fixtures/usb-acm.dtb
    COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_CURRENT_BINARY_DIR}/fixtures
    COMMAND ${DTC_EXECUTABLE} -q -I dts -O dtb -o ${CMAKE_CURRENT_BINARY_DIR}/fixtures/usb-acm.dtb
        ${CMAKE_CURRENT_SOURCE_DIR}/fixtures/usb-acm.dts
    DEPENDS fixtures/usb-acm.dts)
add_custom_target(usb_acm_fixtures DEPENDS ${CMAKE_CURRENT_BINARY_DIR}/fixtures/usb-acm.dtb)
kshim_test(usb_acm_test usb_acm_test.c "${KSHIM_ROOT}/kernel/drivers/console.c"
    "${KSHIM_ROOT}/kernel/drivers/usb_acm.c" "${KSHIM_ROOT}/kernel/dt/dwc3.c"
    "${CRDK_ROOT}/Tests/UsbAcmDwc3Model.c")
target_include_directories(usb_acm_test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/include" "${CRDK_ROOT}/Tests")
target_compile_definitions(usb_acm_test PRIVATE CONFIG_KSHIM_CONSOLE_USB_ACM=1
    "FIXTURES=\"${CMAKE_CURRENT_SOURCE_DIR}/fixtures\""
    "GENERATED_FIXTURES=\"${CMAKE_CURRENT_BINARY_DIR}/fixtures\"")
find_package(Threads REQUIRED)
target_link_libraries(usb_acm_test PRIVATE kshim_dt Threads::Threads)
add_dependencies(usb_acm_test usb_acm_fixtures)
set_tests_properties(usb_acm_test PROPERTIES TIMEOUT 120)
kshim_test(mmu_test mmu_test.c "${KSHIM_ROOT}/kernel/arch/arm64/mmu.c")
target_include_directories(mmu_test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(mmu_test PRIVATE kshim_dt)

set(KSHIM_TEST_MENU_SCALE_Q8 "0" CACHE STRING "Theme design scale; 0=automatic")
option(KSHIM_TEST_MENU_REDUCED_EFFECTS "Reduce theme drawing effects" OFF)
foreach(target lvgl_test lvgl_hdk_test menu_style_render_test menu_modern_test ui_preview_test)
    target_compile_definitions(${target} PRIVATE
        CONFIG_KSHIM_MENU_SCALE_Q8=${KSHIM_TEST_MENU_SCALE_Q8}
        CONFIG_KSHIM_MENU_REDUCED_EFFECTS=$<BOOL:${KSHIM_TEST_MENU_REDUCED_EFFECTS}>)
endforeach()
kshim_test(menu_design_contract_test menu_design_contract_test.c)
target_compile_definitions(menu_design_contract_test PRIVATE
    CONFIG_KSHIM_MENU_STYLE_${KSHIM_TEST_MENU_STYLE}=1
    "EXPECTED_STYLE=\"${KSHIM_TEST_MENU_STYLE}\"")

if(KSHIM_TEST_MENU_STYLE MATCHES "^(METRO|ADWAITA|HOLO)$")
    kshim_test(menu_new_themes_lvgl_test menu_new_themes_lvgl_test.c ${KSHIM_TEST_UI_SOURCES})
    target_link_libraries(menu_new_themes_lvgl_test PRIVATE lvgl kshim_backdrop m)
    target_compile_definitions(menu_new_themes_lvgl_test PRIVATE
        CONFIG_KSHIM_MENU_STYLE_${KSHIM_TEST_MENU_STYLE}=1
        CONFIG_KSHIM_MENU_SCALE_Q8=${KSHIM_TEST_MENU_SCALE_Q8}
        CONFIG_KSHIM_MENU_REDUCED_EFFECTS=$<BOOL:${KSHIM_TEST_MENU_REDUCED_EFFECTS}>)
endif()

if(KSHIM_TEST_MENU_STYLE STREQUAL "LVGL_DEFAULT")
    kshim_test(menu_native_theme_test menu_native_theme_test.c ${KSHIM_TEST_UI_SOURCES})
    target_link_libraries(menu_native_theme_test PRIVATE lvgl kshim_backdrop m)
    target_compile_definitions(menu_native_theme_test PRIVATE
        CONFIG_KSHIM_MENU_SCALE_Q8=${KSHIM_TEST_MENU_SCALE_Q8}
        CONFIG_KSHIM_MENU_REDUCED_EFFECTS=$<BOOL:${KSHIM_TEST_MENU_REDUCED_EFFECTS}>)
endif()

# Power driver is tested with fake register/firmware operations, never real MMIO.
kshim_test(power_test power_test.c "${KSHIM_ROOT}/kernel/drivers/power.c")
kshim_test(power_menu_test power_menu_test.c ${KSHIM_TEST_UI_SOURCES})
target_link_libraries(power_menu_test PRIVATE lvgl kshim_backdrop m)
target_compile_definitions(power_menu_test PRIVATE CONFIG_KSHIM_MENU_STYLE_${KSHIM_TEST_MENU_STYLE}=1
    CONFIG_KSHIM_MENU_SCALE_Q8=${KSHIM_TEST_MENU_SCALE_Q8}
    CONFIG_KSHIM_MENU_REDUCED_EFFECTS=$<BOOL:${KSHIM_TEST_MENU_REDUCED_EFFECTS}>)
kshim_test(power_dt_test power_dt_test.c "${KSHIM_ROOT}/kernel/dt/power.c")
target_link_libraries(power_dt_test PRIVATE kshim_dt)

# Serial fallback boot menu over stubbed console, key source and clock.
kshim_test(bootmenu_test bootmenu_test.c "${KSHIM_ROOT}/user/bootmenu/bootmenu.c")
target_include_directories(bootmenu_test PRIVATE "${KSHIM_ROOT}/tests/include")
target_compile_definitions(bootmenu_test PRIVATE
    "CONFIG_KSHIM_MENU_ENTRY1=\"Linux\"" "CONFIG_KSHIM_MENU_ENTRY2=\"Recovery\"")
add_executable(power_preview_test power_preview_test.c ${KSHIM_TEST_UI_SOURCES})
target_include_directories(power_preview_test PRIVATE ${KSHIM_TEST_INCLUDE_DIRS})
target_link_libraries(power_preview_test PRIVATE lvgl kshim_backdrop m)
target_compile_options(power_preview_test PRIVATE -Wall -Wextra -Werror -UNDEBUG)
target_compile_definitions(power_preview_test PRIVATE
    CONFIG_KSHIM_MENU_STYLE_${KSHIM_TEST_MENU_STYLE}=1
    CONFIG_KSHIM_MENU_SCALE_Q8=${KSHIM_TEST_MENU_SCALE_Q8}
    CONFIG_KSHIM_MENU_REDUCED_EFFECTS=$<BOOL:${KSHIM_TEST_MENU_REDUCED_EFFECTS}>)

target_compile_definitions(power_dt_test PRIVATE "FIXTURES=\"${CMAKE_CURRENT_SOURCE_DIR}/fixtures\"")

set_tests_properties(power_test power_dt_test power_menu_test PROPERTIES TIMEOUT 60)

if(KSHIM_TEST_MENU_STYLE STREQUAL "CLOVER")
    kshim_test(menu_clover_picker_test menu_clover_picker_test.c ${KSHIM_TEST_UI_SOURCES})
    target_link_libraries(menu_clover_picker_test PRIVATE lvgl kshim_backdrop m)
    target_compile_definitions(menu_clover_picker_test PRIVATE CONFIG_KSHIM_MENU_STYLE_CLOVER=1
        CONFIG_KSHIM_MENU_SCALE_Q8=${KSHIM_TEST_MENU_SCALE_Q8}
        CONFIG_KSHIM_MENU_REDUCED_EFFECTS=$<BOOL:${KSHIM_TEST_MENU_REDUCED_EFFECTS}>)
endif()

# Block layer, filesystems and firmware loader (MIT variants; EROFS opt-in).
add_subdirectory(storage)
include("${CMAKE_CURRENT_LIST_DIR}/storage_drivers.cmake")
