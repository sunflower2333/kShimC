# SPDX-License-Identifier: MIT
# Owner: sm8650. GPIO consumers and pinctrl (kernel/drivers/gpio/), clock
# leaf facts (kernel/drivers/clk/), regulators (kernel/drivers/regulator/)
# and SoC tables (kernel/soc/). See docs/io-stack.md.
if(CONFIG_KSHIM_GPIO OR CONFIG_KSHIM_CLK)
    # CrAndroidPin/CrAndroidClock tables (SM8150/SM8250) and GpioLib.
    set(CRDK_GPIO ON CACHE BOOL "" FORCE)
    list(APPEND KERNEL_SOURCES kernel/dt/gpio.c)
endif()
if(CONFIG_KSHIM_REGULATOR)
    # CmdDBLib + RpmhLib polling API.
    set(CRDK_GENI ON CACHE BOOL "" FORCE)
endif()
