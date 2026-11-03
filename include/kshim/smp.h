/* SPDX-License-Identifier: MIT */
#pragma once

/*
 * Public kernel SMP API: offload work to the PSCI worker CPUs started by
 * kshim_platform_accelerate(). Bring-up, shutdown and handoff stay kernel
 * internal (kernel/include/smp.h).
 */

#include <stddef.h>
#include <stdint.h>

#define KSHIM_SMP_MAX_CPUS 8U
#define KSHIM_SMP_DEFAULT_TIMEOUT_US UINT64_C(1000000)

typedef uint64_t kshim_smp_ticket_t;
typedef int (*kshim_smp_callback_t)(void *context);

enum {
    KSHIM_SMP_OK = 0,
    KSHIM_SMP_INVALID = -1,
    KSHIM_SMP_NOT_READY = -2,
    KSHIM_SMP_BUSY = -3,
    KSHIM_SMP_TIMEOUT = -4,
    KSHIM_SMP_PSCI_ERROR = -5,
    KSHIM_SMP_CPU_ERROR = -6,
};

size_t kshim_smp_cpu_count(void);
size_t kshim_smp_available_count(void);

/* Run callback(context) on worker worker_index (0-based, boot CPU excluded). */
int kshim_smp_submit(
    size_t worker_index, kshim_smp_callback_t callback, void *context,
    kshim_smp_ticket_t *ticket);
int kshim_smp_join(
    size_t worker_index, kshim_smp_ticket_t ticket, uint64_t timeout_us,
    int *result);

uint64_t kshim_smp_time_us(void);

/* Cross-CPU events with WFE semantics: kshim_smp_wait_event() returns after
 * a kshim_smp_send_event() from another CPU since this CPU's previous wait,
 * or spuriously, so callers re-check their condition in a loop. CPUs taking
 * interrupts sleep in WFI and are woken by an SGI; otherwise WFE/SEV. */
void kshim_smp_wait_event(void);
void kshim_smp_send_event(void);
