# SPDX-License-Identifier: MIT
# Owner: sm8650. GPIO consumers and pinctrl (kernel/drivers/gpio/), clock
# leaf facts (kernel/drivers/clk/), regulators (kernel/drivers/regulator/)
# and SoC tables (kernel/soc/). See docs/io-stack.md.
if(CONFIG_KSHIM_GPIO OR CONFIG_KSHIM_CLK)
    # CrAndroidPin/CrAndroidClock tables (SM8150/SM8250) and GpioLib.
    set(CRDK_GPIO ON CACHE BOOL "" FORCE)
    list(APPEND KERNEL_SOURCES kernel/dt/gpio.c kernel/soc/soc.c kernel/soc/sm8650_tables.c)
endif()
if(CONFIG_KSHIM_GPIO)
    # PMIC GPIOs use CrDK PmicGpioLib (CRDK_SPMI, on with CONFIG_QCOM_SPMI).
    list(APPEND KERNEL_SOURCES kernel/drivers/gpio/gpio.c kernel/drivers/gpio/pinctrl.c)
endif()
if(CONFIG_KSHIM_CLK)
    list(APPEND KERNEL_SOURCES kernel/drivers/clk/clk.c)
endif()
if(CONFIG_KSHIM_REGULATOR)
    # CmdDBLib + RpmhLib polling API.
    set(CRDK_GENI ON CACHE BOOL "" FORCE)
    list(APPEND KERNEL_SOURCES kernel/drivers/regulator/regulator.c)
endif()
# Table and setup code; size over speed like the irqchip sources.
set_source_files_properties(kernel/soc/soc.c kernel/soc/sm8650_tables.c
    kernel/drivers/gpio/gpio.c kernel/drivers/gpio/pinctrl.c kernel/drivers/clk/clk.c
    kernel/drivers/regulator/regulator.c PROPERTIES COMPILE_OPTIONS -Os)
