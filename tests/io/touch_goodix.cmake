# SPDX-License-Identifier: MIT
# Owner: touch-goodix. Host register-model tests (docs/io-stack.md "Test plan",
# docs/touch-goodix.md). The touch framework helpers (touch-st) and the
# regulator/GPIO calls (sm8650) are faked in io/touch_goodix_fake.c until
# those branches merge.
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
