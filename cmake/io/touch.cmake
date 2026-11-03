# SPDX-License-Identifier: MIT
# Owner: touch-st. Touch framework (kernel/drivers/input/touch/core.c and the
# driver table registry.c), FTM4 and FTM5 drivers, the touch service in
# kernel/core/input.c and the tracker kernel/drivers/touch.c. See
# docs/io-stack.md.
if(CONFIG_KSHIM_TOUCH_FRAMEWORK)
    list(APPEND KERNEL_SOURCES kernel/drivers/input/touch/registry.c)
endif()
