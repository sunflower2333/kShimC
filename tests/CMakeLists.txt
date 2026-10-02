cmake_minimum_required(VERSION 3.16)
project(kshim_host_tests C ASM)
enable_testing()

get_filename_component(KSHIM_ROOT "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)
option(KSHIM_TEST_SANITIZERS "Use address and undefined behavior sanitizers" ON)
option(KSHIM_TEST_DARK "Build the dark menu theme" ON)
add_compile_definitions(KSHIM_HOST_TEST=1 CONFIG_QCOM_SPMI=1 KSHIM_STANDALONE=1
    CONFIG_KSHIM_UI_DARK=$<BOOL:${KSHIM_TEST_DARK}>
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
add_library(kshim_glass STATIC
    "${KSHIM_ROOT}/src/ui/glass_renderer.c"
    "${KSHIM_ROOT}/third_party/liquid-glass/ui_glass_optics.c"
    "${KSHIM_ROOT}/third_party/liquid-glass/ui_glass_motion.c"
    "${KSHIM_ROOT}/third_party/liquid-glass/liquid_glass_motion.c"
    "${KSHIM_ROOT}/third_party/liquid-glass/liquid_glass_compositor_core.c")
target_include_directories(kshim_glass PUBLIC
    "${KSHIM_ROOT}/third_party/liquid-glass" "${KSHIM_ROOT}/src/include")

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
target_link_libraries(lvgl_test PRIVATE lvgl kshim_glass m)
target_sources(lvgl_test PRIVATE "${KSHIM_ROOT}/src/drivers/touch.c")

kshim_test(lvgl_hdk_test lvgl_test.c "${KSHIM_ROOT}/src/ui/lvgl_port.c"
    "${KSHIM_ROOT}/src/ui/framebuffer.c" "${KSHIM_ROOT}/src/drivers/keys.c")
target_compile_definitions(lvgl_hdk_test PRIVATE CONFIG_KSHIM_UEFI_TEST_MENU=1
    "CONFIG_KSHIM_MENU_ENTRY1=\"UEFI A\"" "CONFIG_KSHIM_MENU_ENTRY2=\"UEFI B\"")
target_link_libraries(lvgl_hdk_test PRIVATE lvgl kshim_glass m)
target_sources(lvgl_hdk_test PRIVATE "${KSHIM_ROOT}/src/drivers/touch.c")

add_executable(glass_preview_test glass_preview_test.c
    "${KSHIM_ROOT}/src/ui/lvgl_port.c" "${KSHIM_ROOT}/src/ui/framebuffer.c"
    "${KSHIM_ROOT}/src/drivers/keys.c")
target_include_directories(glass_preview_test PRIVATE "${KSHIM_ROOT}/src/include")
target_compile_definitions(glass_preview_test PRIVATE CONFIG_KSHIM_UEFI_TEST_MENU=1
    "CONFIG_KSHIM_MENU_ENTRY1=\"UEFI A\"" "CONFIG_KSHIM_MENU_ENTRY2=\"UEFI B\"")
target_link_libraries(glass_preview_test PRIVATE lvgl kshim_glass m)

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
# TB323FU key resource regression fixture.
set(TB323FU_KEYS_DTS "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/tb323fu-keys.dts")
set(TB323FU_KEYS_DTB "${CMAKE_CURRENT_BINARY_DIR}/fixtures/tb323fu-keys.dtb")

add_custom_command(
    OUTPUT "${TB323FU_KEYS_DTB}"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_CURRENT_BINARY_DIR}/fixtures"
    COMMAND "${DTC_EXECUTABLE}" -I dts -O dtb -o "${TB323FU_KEYS_DTB}" "${TB323FU_KEYS_DTS}"
    DEPENDS "${TB323FU_KEYS_DTS}"
    VERBATIM)

add_custom_target(tb323fu_fixture DEPENDS "${TB323FU_KEYS_DTB}")

kshim_test(tb323fu_keys_test
    tb323fu_keys_test.c
    ${KSHIM_ROOT}/src/dt/keys.c
    ${KSHIM_ROOT}/src/drivers/keys.c)

target_link_libraries(tb323fu_keys_test PRIVATE kshim_dt)
add_dependencies(tb323fu_keys_test tb323fu_fixture)
target_compile_definitions(tb323fu_keys_test PRIVATE FIXTURE_PATH="${TB323FU_KEYS_DTB}")
