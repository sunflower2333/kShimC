# SPDX-License-Identifier: MIT
# Owner: touch-st. Host register-model tests (docs/io-stack.md "Test plan").
#
# The framework and the ST drivers run over tests/io/touch_fakes.c, which
# stands in for kshim/gpio.h, pinctrl.h, regulator.h, i2c.h and spi.h until
# the sm8650 and qup branches are merged, and for the TLMM interrupt API
# (TOUCH_FAKE_TLMM=1) or, in touch_st_irq_test, the real TLMM cascade over
# the irq_fake.c register model.
set(KSHIM_TOUCH_TEST_DEFS CONFIG_KSHIM_TOUCH=1 CONFIG_KSHIM_TOUCH_FRAMEWORK=1
    CONFIG_KSHIM_I2C=1 CONFIG_KSHIM_SPI=1 CONFIG_KSHIM_REGULATOR=1 CONFIG_KSHIM_GPIO=1
    CONFIG_QCOM_IRQCHIP=1 CONFIG_KSHIM_INPUT_IRQ=1)
set(KSHIM_TOUCH_CORE_SOURCES "${KSHIM_ROOT}/kernel/drivers/input/touch/core.c"
    "${KSHIM_ROOT}/kernel/drivers/touch.c" "${KSHIM_ROOT}/kernel/dt/qcom_irq.c"
    "${CMAKE_CURRENT_LIST_DIR}/touch_fakes.c")

add_custom_command(OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/fixtures/touch-framework.dtb
    COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_CURRENT_BINARY_DIR}/fixtures
    COMMAND ${DTC_EXECUTABLE} -q -I dts -O dtb -o ${CMAKE_CURRENT_BINARY_DIR}/fixtures/touch-framework.dtb
        ${CMAKE_CURRENT_SOURCE_DIR}/fixtures/io/touch-framework.dts
    DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/fixtures/io/touch-framework.dts)
add_custom_target(touch_fixtures DEPENDS ${CMAKE_CURRENT_BINARY_DIR}/fixtures/touch-framework.dtb)

# Framework: selection, power order, deferred IRQ, fallbacks, transform, quiesce.
kshim_test(touch_framework_test io/touch_framework_test.c ${KSHIM_TOUCH_CORE_SOURCES})
target_compile_definitions(touch_framework_test PRIVATE ${KSHIM_TOUCH_TEST_DEFS} TOUCH_FAKE_TLMM=1
    "GENERATED_FIXTURES=\"${CMAKE_CURRENT_BINARY_DIR}/fixtures\"")
target_include_directories(touch_framework_test PRIVATE "${KSHIM_ROOT}/tests/include")
target_link_libraries(touch_framework_test PRIVATE kshim_dt)
add_dependencies(touch_framework_test touch_fixtures)

# ST drivers on the real andromeda DTB, the kona binding fixture and, when
# available, the real Android SM8250 HDK DTB (not committed: GPL-2.0 vendor
# DT; pass -DKSHIM_HDK8250_DTB=<path>, the case is skipped without it).
set(KSHIM_HDK8250_DTB
    "/home/sunf/mu_aloha_platforms/Platforms/KonaCrPkg/Device/qcom-hdk8250/DeviceTreeBlob/Android/android-hdk8250.dtb"
    CACHE FILEPATH "Android SM8250 HDK DTB for touch_st_test (optional)")
set(KSHIM_TOUCH_ST_SOURCES io/touch_st_test.c ${KSHIM_TOUCH_CORE_SOURCES}
    "${KSHIM_ROOT}/kernel/drivers/input/touch/registry.c"
    "${KSHIM_ROOT}/kernel/drivers/input/touch/ftm5.c"
    "${KSHIM_ROOT}/kernel/drivers/input/touch/ftm4.c"
    "${KSHIM_ROOT}/kernel/drivers/st_ftm5.c")
kshim_test(touch_st_test ${KSHIM_TOUCH_ST_SOURCES})
target_compile_definitions(touch_st_test PRIVATE ${KSHIM_TOUCH_TEST_DEFS} TOUCH_FAKE_TLMM=1
    CONFIG_KSHIM_TOUCH_FTM5=1 CONFIG_KSHIM_TOUCH_FTM4=1
    "FIXTURES=\"${CMAKE_CURRENT_SOURCE_DIR}/fixtures\""
    "GENERATED_FIXTURES=\"${CMAKE_CURRENT_BINARY_DIR}/fixtures\""
    "KSHIM_HDK8250_DTB=\"${KSHIM_HDK8250_DTB}\"")
target_include_directories(touch_st_test PRIVATE "${KSHIM_ROOT}/tests/include")
target_link_libraries(touch_st_test PRIVATE kshim_dt)
add_dependencies(touch_st_test dt_fixtures)

# The andromeda FTM5 case again, through the real TLMM cascade.
kshim_test(touch_st_irq_test io/touch_st_test.c ${KSHIM_TOUCH_CORE_SOURCES} ${KSHIM_IRQCHIP_SOURCES}
    "${KSHIM_ROOT}/kernel/drivers/input/touch/registry.c"
    "${KSHIM_ROOT}/kernel/drivers/input/touch/ftm5.c"
    "${KSHIM_ROOT}/kernel/drivers/st_ftm5.c")
target_compile_definitions(touch_st_irq_test PRIVATE ${KSHIM_TOUCH_TEST_DEFS} TOUCH_FAKE_TLMM=0
    CONFIG_KSHIM_TOUCH_FTM5=1 "FIXTURES=\"${CMAKE_CURRENT_SOURCE_DIR}/fixtures\""
    "GENERATED_FIXTURES=\"${CMAKE_CURRENT_BINARY_DIR}/fixtures\""
    "KSHIM_HDK8250_DTB=\"${KSHIM_HDK8250_DTB}\"")
target_include_directories(touch_st_irq_test PRIVATE "${KSHIM_ROOT}/tests/include")
target_link_libraries(touch_st_irq_test PRIVATE kshim_dt)
