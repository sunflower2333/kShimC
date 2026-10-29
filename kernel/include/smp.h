/* SPDX-License-Identifier: MIT */
#pragma once

#include <stddef.h>
#include <stdint.h>

/* Worker offload types and calls are public; this adds kernel lifecycle. */
#include <kshim/smp.h>

int kshim_smp_init(const void *fdt);
int kshim_smp_start_workers(uint64_t timeout_us);
size_t kshim_smp_online_count(void);
uint64_t kshim_smp_cpu_mpidr(size_t cpu_index);

/* Invoked while a worker is idle. It must return promptly. NULL removes it. */
int kshim_smp_set_permanent_callback(
    size_t worker_index, kshim_smp_callback_t callback, void *context);

int kshim_smp_shutdown(uint64_t timeout_us);
int kshim_smp_prepare_handoff(uint64_t timeout_us);

uint64_t kshim_smp_time_ticks(void);
uint64_t kshim_smp_time_frequency(void);

void kshim_smp_secondary_main(uintptr_t boot_context, uint64_t entry_el)
    __attribute__((noreturn));
void kshim_smp_secondary_entry(void);
