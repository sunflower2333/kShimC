# SPDX-License-Identifier: MIT
# Owner: touch-goodix. GT911 (I2C) and GT9916 Berlin (SPI) drivers under
# kernel/drivers/input/touch/. See docs/io-stack.md and docs/touch-goodix.md.
# Each driver registers through registry.c (touch-st), which the touch
# fragment builds whenever any KSHIM_TOUCH_<chip> is set.
if(CONFIG_KSHIM_TOUCH_GT911)
    list(APPEND KERNEL_SOURCES kernel/drivers/input/touch/goodix_gt911.c)
endif()
if(CONFIG_KSHIM_TOUCH_GT9916)
    list(APPEND KERNEL_SOURCES kernel/drivers/input/touch/goodix_gt9916.c)
endif()
