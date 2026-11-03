/* SPDX-License-Identifier: MIT */
/*
 * kernel/core/io.c over a fake clock: completion waits (already done,
 * completed by the poll pump, timeout with and without interrupts), status
 * names, and the orchestration hooks with every subsystem configured out.
 */
#include <kshim/io.h>
#include <kshim/smp.h>
#include <kshim/time.h>

#include <assert.h>
#include <stdio.h>
#include <string.h>

static uint64_t now_us;
static bool irq_ready;
static unsigned waits, delays;

uint64_t kshim_smp_time_us(void) { return now_us; }
bool kshim_irq_ready(void) { return irq_ready; }
uint64_t get_current_time(void) { return now_us / 1000U; }
void kshim_wait_event(uint64_t deadline_ms)
{
    ++waits;
    /* WFI until the next millisecond tick (or an interrupt). */
    uint64_t deadline = deadline_ms * 1000U;
    now_us = deadline > now_us ? deadline : now_us + 1U;
}
uint64_t delay(uint64_t micro_seconds)
{
    ++delays;
    now_us += micro_seconds;
    return micro_seconds;
}

struct pump { struct kshim_completion *c; unsigned calls, complete_at; int status; };
static void pump(void *context)
{
    struct pump *p = context;
    if (++p->calls == p->complete_at) kshim_completion_complete(p->c, p->status);
}

int main(void)
{
    struct kshim_completion c;

    /* Already complete: no sleep, its status. */
    kshim_completion_init(&c);
    kshim_completion_complete(&c, KSHIM_IO_ERR_STALL);
    assert(kshim_completion_done(&c));
    assert(kshim_completion_wait(&c, 1000, NULL, NULL) == KSHIM_IO_ERR_STALL);
    assert(!waits && !delays);

    /* Completed by the pump on its third call, interrupts live: WFI waits. */
    kshim_completion_init(&c);
    irq_ready = true;
    struct pump p = {&c, 0, 3, 0};
    assert(kshim_completion_wait(&c, 20000, pump, &p) == 0);
    assert(p.calls == 3 && waits == 2 && delays == 0);

    /* Timeout with interrupts: bounded by the deadline, request left pending. */
    kshim_completion_init(&c);
    waits = 0; now_us = 5000;
    assert(kshim_completion_wait(&c, 3500, NULL, NULL) == ERROR_TIMEOUT);
    assert(!kshim_completion_done(&c) && now_us >= 8500 && now_us < 10000 && waits >= 3);

    /* Polling fallback without interrupts: short busy steps, no WFI. */
    kshim_completion_init(&c);
    irq_ready = false; waits = 0; delays = 0;
    struct pump q = {&c, 0, 50, ERROR_DEVICE_ERROR};
    assert(kshim_completion_wait(&c, 20000, pump, &q) == ERROR_DEVICE_ERROR);
    assert(q.calls == 50 && delays == 49 && waits == 0);
    kshim_completion_init(&c);
    uint64_t start = now_us;
    assert(kshim_completion_wait(&c, 100, NULL, NULL) == ERROR_TIMEOUT);
    assert(now_us - start >= 100 && now_us - start < 200);

    assert(kshim_completion_wait(NULL, 1, NULL, NULL) == ERROR_INVALID_PARAMETER);

    /* Status names: never NULL, distinct for every I/O code. */
    for (int s = 0; s < 140; ++s) assert(kshim_io_strerror(s) != NULL);
    for (int a = KSHIM_IO_ERR_BASE; a <= KSHIM_IO_ERR_PENDING; ++a)
        for (int b = a + 1; b <= KSHIM_IO_ERR_PENDING; ++b)
            assert(strcmp(kshim_io_strerror(a), kshim_io_strerror(b)));
    assert(!strcmp(kshim_io_strerror(-1), "unknown error"));

    /* Nothing configured: every hook is a successful no-op. */
    assert(kshim_io_on_boot_cpu());
    assert(kshim_io_map_resources(NULL) == 0);
    assert(kshim_io_init(NULL) == 0);
    assert(kshim_io_on_boot_cpu());
    assert(kshim_io_quiesce() == 0 && kshim_io_quiesce() == 0);
    puts("io_core_test: ok");
    return 0;
}
