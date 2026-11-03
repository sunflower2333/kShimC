/* SPDX-License-Identifier: MIT */
#pragma once

/* Public kernel timing API (ARM generic timer). */

#include <stdint.h>

/* Wait for micro_seconds. Sleeps in WFI on the timer interrupt once
 * interrupts are up; busy-waits before that. */
uint64_t delay(uint64_t micro_seconds);

/* Monotonic time in milliseconds. */
uint64_t get_current_time();

/* Sleep until any interrupt arrives or deadline_ms (get_current_time() base)
 * passes, whichever is first. Event loops call this between iterations
 * instead of a fixed delay. Returns immediately if the deadline has passed. */
void kshim_wait_event(uint64_t deadline_ms);
