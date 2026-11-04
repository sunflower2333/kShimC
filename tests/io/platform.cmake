# SPDX-License-Identifier: MIT
# Owner: sm8650. Host register-model tests (docs/io-stack.md "Test plan").
# Fixtures: tests/fixtures/io (README.md there says how they were made).
set(KSHIM_PLATFORM_SOURCES
    "${KSHIM_ROOT}/kernel/soc/soc.c" "${KSHIM_ROOT}/kernel/soc/sm8650_tables.c"
    "${KSHIM_ROOT}/kernel/drivers/gpio/gpio.c" "${KSHIM_ROOT}/kernel/drivers/gpio/pinctrl.c"
    "${KSHIM_ROOT}/kernel/drivers/clk/clk.c" "${KSHIM_ROOT}/kernel/drivers/regulator/regulator.c")

# Every resource the other implementers need from the SM8650 boards.
kshim_test(platform_dt_test io/platform_dt_test.c ${KSHIM_PLATFORM_SOURCES}
    "${KSHIM_ROOT}/kernel/dt/irq.c" "${KSHIM_ROOT}/kernel/dt/qcom_irq.c"
    "${KSHIM_ROOT}/kernel/dt/keys.c" "${KSHIM_ROOT}/kernel/drivers/keys.c")
add_test(NAME platform_dt_qrd_test COMMAND platform_dt_test qrd)
# TLMM/PMIC GPIO registry and pinctrl over a register model.
kshim_test(platform_gpio_test io/platform_gpio_test.c ${KSHIM_PLATFORM_SOURCES})
# Clock leaves: SM8650 and Android tables, inherit vs cold, RCG, restore.
kshim_test(platform_clk_test io/platform_clk_test.c ${KSHIM_PLATFORM_SOURCES})
add_test(NAME platform_clk_kona_test COMMAND platform_clk_test kona)
# Regulators: fixed chains and RPMh over a fake RSC/TCS and CMD DB.
kshim_test(platform_regulator_test io/platform_regulator_test.c ${KSHIM_PLATFORM_SOURCES})
add_test(NAME platform_regulator_kona_test COMMAND platform_regulator_test kona)
foreach(target platform_dt_test platform_gpio_test platform_clk_test platform_regulator_test)
    target_include_directories(${target} PRIVATE "${KSHIM_ROOT}/tests/include")
    target_compile_definitions(${target} PRIVATE "FIXTURES=\"${KSHIM_ROOT}/tests/fixtures\""
        "GENERATED_FIXTURES=\"${CMAKE_CURRENT_BINARY_DIR}/fixtures\"")
    target_link_libraries(${target} PRIVATE kshim_dt)
endforeach()
add_dependencies(platform_gpio_test dt_fixtures)
add_dependencies(platform_clk_test dt_fixtures)
add_dependencies(platform_regulator_test dt_fixtures)
# The SM8650 TLMM summary layout (INTR_CFG target bits 10:8) over irq_fake.
kshim_test(platform_tlmm_irq_test io/platform_tlmm_irq_test.c ${KSHIM_IRQCHIP_SOURCES})
target_compile_definitions(platform_tlmm_irq_test PRIVATE CONFIG_QCOM_IRQCHIP=1
    "FIXTURES=\"${KSHIM_ROOT}/tests/fixtures\"")
target_link_libraries(platform_tlmm_irq_test PRIVATE kshim_dt)
