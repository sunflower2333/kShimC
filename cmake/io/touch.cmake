# SPDX-License-Identifier: MIT
# Owner: touch-st. Touch framework (kernel/drivers/input/touch/core.c and the
# driver table registry.c), FTM4 and FTM5 drivers, the touch service in
# kernel/core/input.c and the tracker kernel/drivers/touch.c. See
# docs/io-stack.md.
if(CONFIG_KSHIM_TOUCH_FRAMEWORK)
    list(APPEND KERNEL_SOURCES kernel/drivers/input/touch/registry.c
        kernel/drivers/input/touch/core.c kernel/drivers/touch.c)
endif()
# Both ST drivers share the Android board selection (kernel/dt/touch.c).
if(CONFIG_KSHIM_TOUCH_FTM5)
    list(APPEND KERNEL_SOURCES kernel/drivers/input/touch/ftm5.c kernel/drivers/st_ftm5.c
        kernel/dt/touch.c)
endif()
if(CONFIG_KSHIM_TOUCH_FTM4)
    list(APPEND KERNEL_SOURCES kernel/drivers/input/touch/ftm4.c kernel/dt/touch.c)
endif()
