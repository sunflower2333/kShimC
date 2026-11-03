/* SPDX-License-Identifier: MIT */
/*
 * Touch controller drivers known to the framework, in match order
 * (kshim/touch_driver.h). FTM5 precedes FTM4: both bind "st,fts" and their
 * match() hooks tell the property families apart. Each driver owns its own
 * line; nothing else belongs in this file.
 */
#include <config.h>
#include <stddef.h>
#include <kshim/touch_driver.h>

const struct kshim_touch_driver *const kshim_touch_drivers[] = {
#if CONFIG_KSHIM_TOUCH_FTM5
    &kshim_touch_driver_ftm5,
#endif
#if CONFIG_KSHIM_TOUCH_FTM4
    &kshim_touch_driver_ftm4,
#endif
#if CONFIG_KSHIM_TOUCH_GT911
    &kshim_touch_driver_gt911,
#endif
#if CONFIG_KSHIM_TOUCH_GT9916
    &kshim_touch_driver_gt9916,
#endif
    NULL,
};
