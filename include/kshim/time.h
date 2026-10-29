/* SPDX-License-Identifier: MIT */
#pragma once

/* Public kernel timing API (ARM generic timer). */

#include <stdint.h>

/* Busy-wait for micro_seconds. */
uint64_t delay(uint64_t micro_seconds);

/* Monotonic time in milliseconds. */
uint64_t get_current_time();
