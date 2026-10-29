/* SPDX-License-Identifier: MIT */
#include <smp_selftest.h>

#include <config.h>
#ifndef CONFIG_KSHIM_SMP_SELFTEST
#define CONFIG_KSHIM_SMP_SELFTEST 0
#endif
#include <smp.h>
#include <kshim/user.h>

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#ifndef CONFIG_KSHIM_SMP_WORKERS
#define CONFIG_KSHIM_SMP_WORKERS 3
#endif

#define SELFTEST_PROBE_ADDRESS UINT64_C(0x46000000)
#define SELFTEST_ITERATIONS UINT64_C(1024)
#define SELFTEST_PERMANENT_VISITS UINT64_C(4)
#define SELFTEST_MPIDR_MASK UINT64_C(0x000000ff00ffffff)

typedef struct {
    uint64_t expected_mpidr;
    uint64_t *probe;
    uint64_t iterations;
    uint64_t observed_mpidr;
} smp_task_context_t;

typedef struct {
    uint64_t visits;
} smp_permanent_context_t;

static uint64_t selftest_mpidr(void)
{
#if defined(__aarch64__)
    uint64_t value;

    __asm__ volatile("mrs %0, mpidr_el1" : "=r"(value));
    return value & SELFTEST_MPIDR_MASK;
#else
    return 0U;
#endif
}

static int shared_probe_task(void *opaque)
{
    smp_task_context_t *context = opaque;
    uint64_t mpidr = selftest_mpidr();

    context->observed_mpidr = mpidr;
    for (uint64_t index = 0U; index < context->iterations; index++)
        (void)__atomic_fetch_add(context->probe, 1U, __ATOMIC_RELAXED);
    return mpidr == context->expected_mpidr ? 0 : -1;
}

static int permanent_probe(void *opaque)
{
    smp_permanent_context_t *context = opaque;

    (void)__atomic_fetch_add(&context->visits, 1U, __ATOMIC_RELAXED);
    return 0;
}

static int wait_for_permanent_callbacks(
    smp_permanent_context_t *contexts, size_t count, uint64_t timeout_us)
{
    uint64_t start = kshim_smp_time_us();

    for (;;) {
        size_t ready = 0U;

        for (size_t index = 0U; index < count; index++) {
            if (__atomic_load_n(&contexts[index].visits, __ATOMIC_ACQUIRE) >=
                SELFTEST_PERMANENT_VISITS)
                ready++;
        }
        if (ready == count)
            return 0;
        if (kshim_smp_time_us() - start >= timeout_us)
            return -1;
#if defined(__aarch64__)
        __asm__ volatile("yield" ::: "memory");
#endif
    }
}

int kshim_smp_selftest(const void *fdt)
{
    smp_task_context_t tasks[KSHIM_SMP_MAX_CPUS - 1U] = {0};
    smp_permanent_context_t permanent[KSHIM_SMP_MAX_CPUS - 1U] = {0};
    kshim_smp_ticket_t tickets[KSHIM_SMP_MAX_CPUS - 1U] = {0};
    uint64_t permanent_snapshot[KSHIM_SMP_MAX_CPUS - 1U] = {0};
    uint64_t *probe = (uint64_t *)(uintptr_t)SELFTEST_PROBE_ADDRESS;
    size_t cpu_count;
    size_t worker_count;
    size_t permanent_set = 0U;
    int failure = 0;
    int status;

    status = kshim_smp_init(fdt);
    if (status != KSHIM_SMP_OK) {
        printf("SMP: init failed (%d)\n", status);
        return -100;
    }
    cpu_count = kshim_smp_cpu_count();
    if (cpu_count == 0U || cpu_count > KSHIM_SMP_MAX_CPUS)
        return -101;
    worker_count = cpu_count - 1U;
    if (worker_count > (size_t)CONFIG_KSHIM_SMP_WORKERS)
        worker_count = (size_t)CONFIG_KSHIM_SMP_WORKERS;

    status = kshim_smp_start_workers(KSHIM_SMP_DEFAULT_TIMEOUT_US);
    if (status != KSHIM_SMP_OK) {
        printf("SMP: CPU_ON failed (%d)\n", status);
        (void)kshim_smp_shutdown(KSHIM_SMP_DEFAULT_TIMEOUT_US);
        return -102;
    }
    if (kshim_smp_online_count() != worker_count + 1U ||
        kshim_smp_available_count() != worker_count) {
        failure = -103;
        goto Shutdown;
    }

    printf("SMP: discovered %u CPU(s), started %u worker(s)\n",
           (unsigned)cpu_count, (unsigned)worker_count);
    if (worker_count == 0U) {
#if CONFIG_KSHIM_SMP_SELFTEST
        goto Lvgl;
#else
        goto Shutdown;
#endif
    }

    __atomic_store_n(probe, 0U, __ATOMIC_RELEASE);
    for (size_t index = 0U; index < worker_count; index++) {
        tasks[index].expected_mpidr = kshim_smp_cpu_mpidr(index + 1U);
        tasks[index].probe = probe;
        tasks[index].iterations = SELFTEST_ITERATIONS;
        status = kshim_smp_submit(
            index, shared_probe_task, &tasks[index], &tickets[index]);
        if (status != KSHIM_SMP_OK || tickets[index] == 0U) {
            failure = -104;
            break;
        }
    }
    for (uint64_t index = 0U; index < SELFTEST_ITERATIONS; index++)
        (void)__atomic_fetch_add(probe, 1U, __ATOMIC_RELAXED);

    for (size_t index = 0U; index < worker_count; index++) {
        int result = -1;

        if (tickets[index] == 0U)
            continue;
        status = kshim_smp_join(
            index, tickets[index], KSHIM_SMP_DEFAULT_TIMEOUT_US, &result);
        if ((status != KSHIM_SMP_OK || result != 0 ||
             tasks[index].observed_mpidr != tasks[index].expected_mpidr) &&
            failure == 0)
            failure = -105;
    }
    if (failure == 0 &&
        __atomic_load_n(probe, __ATOMIC_ACQUIRE) !=
            (worker_count + 1U) * SELFTEST_ITERATIONS)
        failure = -106;
    if (failure != 0)
        goto Shutdown;

    for (size_t index = 0U; index < worker_count; index++) {
        status = kshim_smp_set_permanent_callback(
            index, permanent_probe, &permanent[index]);
        if (status != KSHIM_SMP_OK) {
            failure = -107;
            break;
        }
        permanent_set++;
    }
    if (failure == 0 && wait_for_permanent_callbacks(
            permanent, worker_count, KSHIM_SMP_DEFAULT_TIMEOUT_US) != 0)
        failure = -108;

    for (size_t index = 0U; index < permanent_set; index++) {
        status = kshim_smp_set_permanent_callback(index, NULL, NULL);
        if (status != KSHIM_SMP_OK && failure == 0)
            failure = -109;
        permanent_snapshot[index] =
            __atomic_load_n(&permanent[index].visits, __ATOMIC_ACQUIRE);
    }
    for (unsigned spin = 0U; spin < 256U; spin++) {
#if defined(__aarch64__)
        __asm__ volatile("yield" ::: "memory");
#endif
    }
    for (size_t index = 0U; index < permanent_set; index++) {
        if (__atomic_load_n(&permanent[index].visits, __ATOMIC_ACQUIRE) !=
                permanent_snapshot[index] && failure == 0)
            failure = -110;
    }

#if CONFIG_KSHIM_SMP_SELFTEST
Lvgl:
    status = kshim_user_smp_selftest(worker_count);
    if (status != 0 && failure == 0)
        failure = status;
#endif

Shutdown:
    status = kshim_smp_shutdown(KSHIM_SMP_DEFAULT_TIMEOUT_US);
    if (status != KSHIM_SMP_OK && failure == 0)
        failure = -111;
    if (status == KSHIM_SMP_OK &&
        (kshim_smp_online_count() != 1U ||
         kshim_smp_available_count() != 0U) && failure == 0)
        failure = -112;
    if (failure != 0)
        printf("SMP: self-test stage failed (%d)\n", failure);
    return failure;
}
