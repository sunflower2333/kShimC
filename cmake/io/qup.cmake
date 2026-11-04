# SPDX-License-Identifier: MIT
# Owner: qup. QUP/GENI serial engines (kernel/drivers/qup/, kernel/dt/qup.c),
# I2C core (kernel/drivers/i2c/), SPI core (kernel/drivers/spi/), GENI UART
# instances under the console. See docs/io-stack.md and docs/qup.md.

# The console's UART layer (GENI and PL011) is used in every profile.
list(APPEND KERNEL_SOURCES kernel/drivers/qup/geni_uart.c)
# GENI UART RX interrupt engine: the console's (CONFIG_KSHIM_CONSOLE_RX_IRQ,
# also listed by the top level) and other GENI UART instances'.
if(CONFIG_QCOM_IRQCHIP)
    list(APPEND KERNEL_SOURCES kernel/drivers/geni_rx_irq.c)
endif()
if(CONFIG_KSHIM_QUP)
    list(APPEND KERNEL_SOURCES kernel/dt/qup.c kernel/drivers/qup/qup.c)
    # GeniSeLib FIFO core, ClockLib clock_poll, CmdDB/RPMh.
    set(CRDK_GENI ON CACHE BOOL "" FORCE)
    set(CRDK_GPIO ON CACHE BOOL "" FORCE)
endif()
if(CONFIG_KSHIM_I2C)
    list(APPEND KERNEL_SOURCES kernel/drivers/i2c/core.c)
endif()
if(CONFIG_KSHIM_SPI)
    list(APPEND KERNEL_SOURCES kernel/drivers/spi/core.c)
endif()
if(CONFIG_KSHIM_I2C_GENI OR CONFIG_KSHIM_SPI_GENI)
    list(APPEND KERNEL_SOURCES kernel/drivers/qup/geni_engine.c)
endif()
if(CONFIG_KSHIM_I2C_GENI)
    list(APPEND KERNEL_SOURCES kernel/drivers/qup/geni_i2c.c)
endif()
if(CONFIG_KSHIM_SPI_GENI)
    list(APPEND KERNEL_SOURCES kernel/drivers/qup/geni_spi.c)
endif()
