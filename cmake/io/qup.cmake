# SPDX-License-Identifier: MIT
# Owner: qup. QUP/GENI serial engines (kernel/drivers/qup/, kernel/dt/qup.c),
# I2C core (kernel/drivers/i2c/), SPI core (kernel/drivers/spi/), GENI UART
# instances under the console. See docs/io-stack.md.
if(CONFIG_KSHIM_QUP)
    # GeniSeLib FIFO core, ClockLib clock_poll, CmdDB/RPMh.
    set(CRDK_GENI ON CACHE BOOL "" FORCE)
    set(CRDK_GPIO ON CACHE BOOL "" FORCE)
endif()
