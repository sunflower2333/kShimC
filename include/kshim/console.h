/* SPDX-License-Identifier: MIT */
#pragma once

/* Public kernel console API (the DT-selected UART; printf also goes here). */

#include <stdint.h>

void kshim_console_write(const char *buffer, uint32_t length);

/* Non-blocking: return non-zero and store one character when available. */
int kshim_console_try_read(char *character);
