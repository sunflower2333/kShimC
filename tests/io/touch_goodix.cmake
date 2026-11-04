# SPDX-License-Identifier: MIT
# Owner: touch-goodix. Host register-model tests (docs/io-stack.md "Test plan",
# docs/touch-goodix.md). The unit tests fake the touch framework helpers
# (touch-st) and the regulator/GPIO calls (sm8650) in io/touch_goodix_fake.c;
# touch_goodix_framework_test runs the drivers on the real framework.
set(GOODIX_DRIVERS
    "${KSHIM_ROOT}/kernel/drivers/input/touch/goodix_gt911.c"
    "${KSHIM_ROOT}/kernel/drivers/input/touch/goodix_gt9916.c")
set(GOODIX_FAKE "${CMAKE_CURRENT_LIST_DIR}/touch_goodix_fake.c")

# GT911 I2C model: address select timing, product/config reads, point
# frames and the status handshake, inheritance, polling and quiesce.
kshim_test(touch_goodix_gt911_test io/touch_goodix_gt911_test.c ${GOODIX_FAKE}
    "${KSHIM_ROOT}/kernel/drivers/input/touch/goodix_gt911.c")
# GT9916 Berlin SPI model: framing, handshake, version/IC-info checksums,
# touch events, reset request, inheritance, polling and quiesce.
kshim_test(touch_goodix_gt9916_test io/touch_goodix_gt9916_test.c ${GOODIX_FAKE}
    "${KSHIM_ROOT}/kernel/drivers/input/touch/goodix_gt9916.c")
foreach(target touch_goodix_gt911_test touch_goodix_gt9916_test)
    target_link_libraries(${target} PRIVATE kshim_dt)
endforeach()

# DT resolution on the mainline SM8650 QRD and Pocket S2 DTBs; the Pocket S2
# RFC touch node is an overlay compiled here and applied with libfdt.
set(GOODIX_FIXTURES "${KSHIM_ROOT}/tests/fixtures/io/goodix")
set(GOODIX_GENERATED "${CMAKE_CURRENT_BINARY_DIR}/fixtures/io/goodix")
add_custom_command(OUTPUT "${GOODIX_GENERATED}/pocket-s2-touch.dtbo"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${GOODIX_GENERATED}"
    COMMAND ${DTC_EXECUTABLE} -q -@ -I dts -O dtb -o "${GOODIX_GENERATED}/pocket-s2-touch.dtbo"
        "${GOODIX_FIXTURES}/pocket-s2-touch.dtso"
    DEPENDS "${GOODIX_FIXTURES}/pocket-s2-touch.dtso")
add_custom_target(touch_goodix_fixtures DEPENDS "${GOODIX_GENERATED}/pocket-s2-touch.dtbo")
kshim_test(touch_goodix_dt_test io/touch_goodix_dt_test.c ${GOODIX_FAKE} ${GOODIX_DRIVERS}
    "${KSHIM_ROOT}/kernel/dt/qcom_irq.c" "${KSHIM_ROOT}/lib/libfdt/fdt_overlay.c")
target_link_libraries(touch_goodix_dt_test PRIVATE kshim_dt)
# kShimC's libfdt_env.h leaves out <stdlib.h>, which only the overlay code
# (strtoul) needs; the kernel does not build fdt_overlay.c.
set_source_files_properties("${KSHIM_ROOT}/lib/libfdt/fdt_overlay.c" PROPERTIES
    COMPILE_OPTIONS "-include;stdlib.h")
target_compile_definitions(touch_goodix_dt_test PRIVATE
    "GOODIX_FIXTURES=\"${GOODIX_FIXTURES}\"" "GOODIX_GENERATED=\"${GOODIX_GENERATED}\"")
add_dependencies(touch_goodix_dt_test touch_goodix_fixtures)

# Integration (merged tree): the real touch framework (core.c, registry.c)
# with both Goodix drivers on the mainline SM8650 fixtures, over touch-st's
# platform fakes (GPIO, pinctrl, regulators, I2C/SPI clients, TLMM IRQ) and
# chip models written from the same protocol facts.
kshim_test(touch_goodix_framework_test io/touch_goodix_framework_test.c ${GOODIX_DRIVERS}
    "${KSHIM_ROOT}/kernel/drivers/input/touch/core.c"
    "${KSHIM_ROOT}/kernel/drivers/input/touch/registry.c"
    "${KSHIM_ROOT}/kernel/drivers/touch.c" "${KSHIM_ROOT}/kernel/dt/qcom_irq.c"
    "${CMAKE_CURRENT_LIST_DIR}/touch_fakes.c")
target_compile_definitions(touch_goodix_framework_test PRIVATE CONFIG_KSHIM_TOUCH=1
    CONFIG_KSHIM_TOUCH_FRAMEWORK=1 CONFIG_KSHIM_I2C=1 CONFIG_KSHIM_SPI=1
    CONFIG_KSHIM_REGULATOR=1 CONFIG_KSHIM_GPIO=1 CONFIG_QCOM_IRQCHIP=1 CONFIG_KSHIM_INPUT_IRQ=1
    CONFIG_KSHIM_TOUCH_GT911=1 CONFIG_KSHIM_TOUCH_GT9916=1 TOUCH_FAKE_TLMM=1
    "FIXTURES=\"${KSHIM_ROOT}/tests/fixtures\"")
target_include_directories(touch_goodix_framework_test PRIVATE "${KSHIM_ROOT}/tests/include")
target_link_libraries(touch_goodix_framework_test PRIVATE kshim_dt)
