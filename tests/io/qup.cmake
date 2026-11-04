# SPDX-License-Identifier: MIT
# Owner: qup. Host register-model tests (docs/io-stack.md "Test plan",
# docs/qup.md): DT discovery on the Android and mainline fixtures, the QUP
# core (ownership, inherited vs cold bring-up, firmware protocol, release),
# the GENI I2C and SPI controllers over a GENI register model with its SE
# interrupt on the fake GIC, and the UART layer with its RX interrupt queue.
set(QUP_FIXTURE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/io/qup")
set(QUP_GENERATED_DIR "${CMAKE_CURRENT_BINARY_DIR}/fixtures/io/qup")
add_custom_command(OUTPUT ${QUP_GENERATED_DIR}/kona-qup.dtb
    COMMAND ${CMAKE_COMMAND} -E make_directory ${QUP_GENERATED_DIR}
    COMMAND ${DTC_EXECUTABLE} -q -I dts -O dtb -o ${QUP_GENERATED_DIR}/kona-qup.dtb
        ${QUP_FIXTURE_DIR}/kona-qup.dts
    DEPENDS ${QUP_FIXTURE_DIR}/kona-qup.dts)
add_custom_target(qup_fixtures DEPENDS ${QUP_GENERATED_DIR}/kona-qup.dtb)
set(QUP_TEST_DEFINITIONS
    "FIXTURES=\"${CMAKE_CURRENT_SOURCE_DIR}/fixtures\""
    "QUP_FIXTURES=\"${QUP_FIXTURE_DIR}\""
    "QUP_GENERATED=\"${QUP_GENERATED_DIR}\"")
set(QUP_CORE_SOURCES io/qup_fakes.c ${KSHIM_IRQCHIP_SOURCES}
    "${KSHIM_ROOT}/kernel/dt/qup.c" "${KSHIM_ROOT}/kernel/drivers/qup/qup.c"
    "${KSHIM_ROOT}/kernel/drivers/qup/geni_uart.c" "${KSHIM_ROOT}/kernel/drivers/geni_rx_irq.c"
    "${KSHIM_ROOT}/kernel/core/io.c")

# Pure DT: discovery, bus rates, windows, SE interrupts, I2C/SPI children.
kshim_test(qup_dt_test io/qup_dt_test.c "${KSHIM_ROOT}/kernel/dt/qup.c"
    "${KSHIM_ROOT}/kernel/dt/irq.c" "${KSHIM_ROOT}/kernel/dt/qcom_irq.c"
    "${KSHIM_ROOT}/kernel/drivers/i2c/core.c" "${KSHIM_ROOT}/kernel/drivers/spi/core.c"
    "${KSHIM_ROOT}/kernel/core/io.c")
target_compile_definitions(qup_dt_test PRIVATE ${QUP_TEST_DEFINITIONS}
    CONFIG_KSHIM_I2C=1 CONFIG_KSHIM_SPI=1)

kshim_test(qup_core_test io/qup_core_test.c ${QUP_CORE_SOURCES})
target_compile_definitions(qup_core_test PRIVATE ${QUP_TEST_DEFINITIONS}
    CONFIG_QCOM_IRQCHIP=1 CONFIG_KSHIM_QUP=1)

kshim_test(qup_i2c_test io/qup_i2c_test.c ${QUP_CORE_SOURCES}
    "${KSHIM_ROOT}/kernel/drivers/qup/geni_engine.c" "${KSHIM_ROOT}/kernel/drivers/qup/geni_i2c.c"
    "${KSHIM_ROOT}/kernel/drivers/i2c/core.c")
target_compile_definitions(qup_i2c_test PRIVATE ${QUP_TEST_DEFINITIONS}
    CONFIG_QCOM_IRQCHIP=1 CONFIG_KSHIM_QUP=1 CONFIG_KSHIM_I2C=1 CONFIG_KSHIM_I2C_GENI=1)

kshim_test(qup_spi_test io/qup_spi_test.c ${QUP_CORE_SOURCES}
    "${KSHIM_ROOT}/kernel/drivers/qup/geni_engine.c" "${KSHIM_ROOT}/kernel/drivers/qup/geni_spi.c"
    "${KSHIM_ROOT}/kernel/drivers/spi/core.c")
target_compile_definitions(qup_spi_test PRIVATE ${QUP_TEST_DEFINITIONS}
    CONFIG_QCOM_IRQCHIP=1 CONFIG_KSHIM_QUP=1 CONFIG_KSHIM_SPI=1 CONFIG_KSHIM_SPI_GENI=1)

kshim_test(qup_uart_test io/qup_uart_test.c ${QUP_CORE_SOURCES})
target_compile_definitions(qup_uart_test PRIVATE ${QUP_TEST_DEFINITIONS}
    CONFIG_QCOM_IRQCHIP=1 CONFIG_KSHIM_QUP=1)

foreach(target qup_dt_test qup_core_test qup_i2c_test qup_spi_test qup_uart_test)
    target_link_libraries(${target} PRIVATE kshim_dt)
    target_include_directories(${target} PRIVATE "${KSHIM_ROOT}/kernel/drivers/qup"
        "${KSHIM_ROOT}/tests/include")
    add_dependencies(${target} qup_fixtures)
endforeach()

# kernel/drivers/console.c sits on the UART layer (kshim/uart.h) now: the
# existing console test links it too.
target_sources(usb_acm_test PRIVATE "${KSHIM_ROOT}/kernel/drivers/qup/geni_uart.c")
