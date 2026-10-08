cmake_minimum_required(VERSION 3.16)
project(kshim_host_tests C ASM)
enable_testing()

get_filename_component(KSHIM_ROOT "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)
set(KSHIM_TEST_MENU_STYLE "FLAT" CACHE STRING "Boot menu preset used by host UI tests")
set(KSHIM_TEST_MENU_STYLES FLAT CLASSIC CARDS TERMINAL MINIMAL HIGH_CONTRAST)
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

set(LV_CONF_PATH "${KSHIM_ROOT}/src/include/lv_conf.h" CACHE FILEPATH "" FORCE)
set(LV_CONF_BUILD_DISABLE_THORVG_INTERNAL ON CACHE BOOL "" FORCE)
set(LV_CONF_BUILD_DISABLE_EXAMPLES ON CACHE BOOL "" FORCE)
set(LV_CONF_BUILD_DISABLE_DEMOS ON CACHE BOOL "" FORCE)
add_subdirectory("${KSHIM_ROOT}/third_party/lvgl" lvgl)
add_subdirectory("${KSHIM_ROOT}/ports/crdk" crdk)
set(KSHIM_UI_FONT_BLOB "${KSHIM_ROOT}/src/ui/fonts/noto_sans_cjk_sc.ksf")
configure_file("${KSHIM_ROOT}/src/ui/fonts/ui_font_blob.S.in"
               "${CMAKE_CURRENT_BINARY_DIR}/ui_font_blob.S" @ONLY)
set_source_files_properties("${CMAKE_CURRENT_BINARY_DIR}/ui_font_blob.S"
    PROPERTIES OBJECT_DEPENDS "${KSHIM_UI_FONT_BLOB}")
add_library(kshim_backdrop STATIC "${KSHIM_ROOT}/src/ui/backdrop.c"
    "${KSHIM_ROOT}/src/ui/font.c" "${CMAKE_CURRENT_BINARY_DIR}/ui_font_blob.S")
target_include_directories(kshim_backdrop PUBLIC "${KSHIM_ROOT}/src/include")
target_link_libraries(kshim_backdrop PUBLIC lvgl)

function(kshim_test Target)
    add_executable(${Target} ${ARGN})
    target_compile_options(${Target} PRIVATE -Wall -Wextra -Werror -UNDEBUG)
    target_compile_definitions(${Target} PRIVATE _POSIX_C_SOURCE=200809L)
    target_include_directories(${Target} PRIVATE "${KSHIM_ROOT}/src/include")
    add_test(NAME ${Target} COMMAND ${Target})
endfunction()

kshim_test(qualcomm_test qualcomm_test.c
    "${KSHIM_ROOT}/src/drivers/keys.c"
    "${KSHIM_ROOT}/src/dt/keys.c"
)
target_include_directories(qualcomm_test PRIVATE "${KSHIM_ROOT}/lib/libfdt/include")

kshim_test(spmi_test spmi_test.c "${KSHIM_ROOT}/src/drivers/qcom_keys.c")
target_link_libraries(spmi_test PRIVATE crdk_spmi)

kshim_test(framebuffer_test framebuffer_test.c "${KSHIM_ROOT}/src/ui/framebuffer.c")
kshim_test(lvgl_test lvgl_test.c "${KSHIM_ROOT}/src/ui/lvgl_port.c"
    "${KSHIM_ROOT}/src/ui/framebuffer.c" "${KSHIM_ROOT}/src/drivers/keys.c")
target_link_libraries(lvgl_test PRIVATE lvgl kshim_backdrop m)
target_sources(lvgl_test PRIVATE "${KSHIM_ROOT}/src/drivers/touch.c")

kshim_test(lvgl_hdk_test lvgl_test.c "${KSHIM_ROOT}/src/ui/lvgl_port.c"
    "${KSHIM_ROOT}/src/ui/framebuffer.c" "${KSHIM_ROOT}/src/drivers/keys.c")
target_compile_definitions(lvgl_hdk_test PRIVATE CONFIG_KSHIM_UEFI_TEST_MENU=1
    "CONFIG_KSHIM_MENU_ENTRY1=\"UEFI A\"" "CONFIG_KSHIM_MENU_ENTRY2=\"UEFI B\"")
target_link_libraries(lvgl_hdk_test PRIVATE lvgl kshim_backdrop m)
target_sources(lvgl_hdk_test PRIVATE "${KSHIM_ROOT}/src/drivers/touch.c")

add_executable(ui_preview_test ui_preview_test.c
    "${KSHIM_ROOT}/src/ui/lvgl_port.c" "${KSHIM_ROOT}/src/ui/framebuffer.c"
    "${KSHIM_ROOT}/src/drivers/keys.c")
target_include_directories(ui_preview_test PRIVATE "${KSHIM_ROOT}/src/include")
target_compile_definitions(ui_preview_test PRIVATE CONFIG_KSHIM_UEFI_TEST_MENU=1
    "CONFIG_KSHIM_MENU_ENTRY1=\"UEFI A\"" "CONFIG_KSHIM_MENU_ENTRY2=\"UEFI B\"")
target_link_libraries(ui_preview_test PRIVATE lvgl kshim_backdrop m)

# 同一预设运行原有交互回归和样式专项，避免测试与预览风格不一致。
kshim_test(menu_style_render_test menu_style_render_test.c
    "${KSHIM_ROOT}/src/ui/lvgl_port.c" "${KSHIM_ROOT}/src/ui/framebuffer.c"
    "${KSHIM_ROOT}/src/drivers/keys.c")
target_link_libraries(menu_style_render_test PRIVATE lvgl kshim_backdrop m)
foreach(target lvgl_test lvgl_hdk_test ui_preview_test menu_style_render_test)
    target_compile_definitions(${target} PRIVATE
        CONFIG_KSHIM_MENU_STYLE_${KSHIM_TEST_MENU_STYLE}=1)
endforeach()

kshim_test(qcom_bus_test qcom_bus_test.c)
kshim_test(qcom_touch_platform_test qcom_touch_platform_test.c
   )
kshim_test(hdk8250_touch_platform_test hdk8250_touch_platform_test.c
   )
kshim_test(touch_test touch_test.c "${KSHIM_ROOT}/src/drivers/touch.c")
kshim_test(st_ftm5_test st_ftm5_test.c "${KSHIM_ROOT}/src/drivers/st_ftm5.c"
    "${KSHIM_ROOT}/src/drivers/touch.c")
kshim_test(st_ftm4_test st_ftm4_test.c "${KSHIM_ROOT}/src/drivers/st_ftm4.c"
    "${KSHIM_ROOT}/src/drivers/touch.c")

add_library(kshim_dt STATIC
    "${KSHIM_ROOT}/src/dt/dt.c" "${KSHIM_ROOT}/src/dt/spmi.c"
    "${KSHIM_ROOT}/src/dt/gpio.c" "${KSHIM_ROOT}/src/dt/touch.c" "${KSHIM_ROOT}/src/dt/console.c"
    "${KSHIM_ROOT}/src/dt/splash.c"
    "${KSHIM_ROOT}/lib/libfdt/fdt.c" "${KSHIM_ROOT}/lib/libfdt/fdt_check.c"
    "${KSHIM_ROOT}/lib/libfdt/fdt_ro.c" "${KSHIM_ROOT}/lib/libfdt/fdt_rw.c"
    "${KSHIM_ROOT}/lib/libfdt/fdt_sw.c" "${KSHIM_ROOT}/lib/libfdt/fdt_wip.c"
    "${KSHIM_ROOT}/lib/libfdt/fdt_empty_tree.c" "${KSHIM_ROOT}/lib/libfdt/fdt_addresses.c")
target_include_directories(kshim_dt PUBLIC "${KSHIM_ROOT}/src/include" "${KSHIM_ROOT}/lib/libfdt/include")
target_link_libraries(kshim_dt PUBLIC crdk)
foreach(target qualcomm_test qcom_bus_test qcom_touch_platform_test hdk8250_touch_platform_test st_ftm4_test st_ftm5_test)
    target_link_libraries(${target} PRIVATE kshim_dt)
endforeach()
add_link_options(-Wl,--gc-sections)

kshim_test(dt_test dt_test.c "${KSHIM_ROOT}/src/dt/keys.c" "${KSHIM_ROOT}/src/drivers/keys.c")
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
kshim_test(mmu_test mmu_test.c "${KSHIM_ROOT}/src/arch/arm64/mmu.c")
target_include_directories(mmu_test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(mmu_test PRIVATE kshim_dt)
