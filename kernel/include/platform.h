#pragma once
#include <stdint.h>
#include <stddef.h>

int kshim_platform_main(uint64_t fdt, uint64_t arg1, uint64_t arg2, uint64_t arg3);
/* Names must be in runtime-owned memory. Returns -1 when UI is unavailable,
 * -2 on unsafe shutdown (caller must not boot), or a selected index via out. */
int kshim_platform_select(uint64_t fdt, const char *const *names, size_t count,
                          size_t initial, uint32_t timeout_ms, size_t *selected);
int kshim_platform_prepare_handoff(void);
