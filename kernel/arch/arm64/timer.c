/* SPDX-License-Identifier: MIT */
// Arm64 Generic Timer
#include <timer.h>
#include <gicv3.h>
#include <asm/asm_macro.h>
#include <memops.h>
#include <lib/stdport.h>
#include <config.h>
#include <kshim_console.h>

#ifndef CONFIG_KSHIM_IRQ
#define CONFIG_KSHIM_IRQ 0
#endif

/* Below this a WFI round trip (trapped under a hypervisor) costs more than
 * it saves. */
#define TIMER_SLEEP_MIN_US 20U
#define TIMER_MAX_DELTA (UINT64_C(1) << 62)

/*
 * One-shot deadlines on the boot CPU: the virtual timer at EL1 (the EL1
 * physical timer may belong to the hypervisor), the EL2 physical timer
 * (CNTHP) at EL2. Deadlines are CNTPCT values, the base of
 * get_current_time(). The inherited timer state is restored at handoff.
 */
typedef struct ARM64_ARCH_TIMER_CONTEXT
{
    /* IRQ ID used at this exception level */
    uint32_t intid;
    uint32_t el;
    uint32_t ready;
    /* CPU whose timer is armed */
    uint64_t mpidr;
    /* Inherited state */
    uint64_t saved_ctl;
    uint64_t saved_cval;
    /* Statistics */
    uint64_t irqs;
} ARM64_ARCH_TIMER_CONTEXT, *PARM64_ARCH_TIMER_CONTEXT;

#if CONFIG_KSHIM_IRQ
// Driver context
static ARM64_ARCH_TIMER_CONTEXT timer_context;

static uint64_t timer_get_ctl(void)
{
    return timer_context.el == 2 ? arm64_read_cnthpctl() : arm64_read_cntvctl();
}

static void timer_set_ctl(uint64_t ctl)
{
    if (timer_context.el == 2)
        arm64_write_cnthpctl(ctl);
    else
        arm64_write_cntvctl(ctl);
}

static uint64_t timer_get_cval(void)
{
    return timer_context.el == 2 ? arm64_read_cnthpcval() : arm64_read_cntvcval();
}

static void timer_set_cval(uint64_t cval)
{
    if (timer_context.el == 2)
        arm64_write_cnthpcval(cval);
    else
        arm64_write_cntvcval(cval);
}

/* Arm a one-shot interrupt at CNTPCT == deadline. */
static void timer_arm(uint64_t deadline)
{
    uint64_t cval = deadline;

    if (timer_context.el != 2)
    {
        // The virtual counter runs at CNTPCT - CNTVOFF
        uint64_t physical = arm64_read_cntpct(), virtual = arm64_read_cntvct();
        uint64_t delta = deadline > physical ? deadline - physical : 0;
        cval = virtual + (delta > TIMER_MAX_DELTA ? TIMER_MAX_DELTA : delta);
    }
    timer_set_cval(cval);
    timer_set_ctl(CNT_CTL_ENABLE);
}

static void timer_disarm(void)
{
    timer_set_ctl(CNT_CTL_IMASK);
}

static bool timer_can_sleep(void)
{
    return timer_context.ready &&
           (arm64_read_mpidr() & ARM_MPIDR_AFFINITY_MASK) == timer_context.mpidr &&
           kshim_irq_cpu_can_sleep();
}

/* WFI until CNTPCT reaches deadline, or (wake_on_event) a device interrupt
 * has been taken since the last kshim_wait_event(). IRQs are masked around
 * each check so a wake-up cannot slip in between the check and the WFI. */
static void sleep_until(uint64_t deadline, bool wake_on_event)
{
    unsigned long flags = kshim_irq_save();
    GICV3_CPU *cpu = gicv3_this_cpu();

    while (arm64_read_cntpct() < deadline && !(wake_on_event && kshim_irq_take_event()))
    {
        timer_arm(deadline);
        arm64_wfi();
        if (cpu)
            cpu->wfi++;
        kshim_irq_restore(flags);
        flags = kshim_irq_save();
    }
    timer_disarm();
    kshim_irq_restore(flags);
}

#endif

#if CONFIG_KSHIM_CONSOLE_USB_ACM
/*
 * Idle waits (the menus' 5 ms frames, fatal-error loops) keep the polled
 * USB console answering the host, about once per millisecond. Shorter
 * delays are hardware timing and stay exact.
 */
static uint64_t serviced_delay(uint64_t micro_seconds)
{
    uint64_t frequency = arm64_read_cntfrq();
    uint64_t start = arm64_read_cntpct();
    uint64_t target = start + frequency * micro_seconds / 1000000;
    uint64_t step = frequency / 1000;
    uint64_t next = start;
    for (uint64_t now = start; now < target; now = arm64_read_cntpct()) {
        if (now >= next) {
            kshim_console_service();
            next = now + step;
        }
    }
    return arm64_read_cntpct() - start;
}
#endif

uint64_t delay(uint64_t micro_seconds)
{
#if CONFIG_KSHIM_CONSOLE_USB_ACM
    /* The polled USB console needs service during idle waits. */
    if (micro_seconds >= 1000) return serviced_delay(micro_seconds);
#endif
    uint64_t start = arm64_read_cntpct();
    uint64_t target_time = start + (arm64_read_cntfrq() * (micro_seconds) / 1000000);

#if CONFIG_KSHIM_IRQ
    if (micro_seconds >= TIMER_SLEEP_MIN_US && timer_can_sleep())
    {
        sleep_until(target_time, false);
        return arm64_read_cntpct() - start;
    }
#endif
    while (arm64_read_cntpct() < target_time)
    {
    }
    return arm64_read_cntpct() - start;
}

// @Return: unit milli seconds
uint64_t get_current_time()
{
    uint64_t current_time = arm64_read_cntpct() * 1000 / arm64_read_cntfrq();
    return current_time;
}

void kshim_wait_event(uint64_t deadline_ms)
{
    uint64_t frequency = arm64_read_cntfrq();
    uint64_t target;

    if (!frequency)
        return;
    // First CNTPCT at which get_current_time() >= deadline_ms
    target = deadline_ms > (UINT64_MAX - 999U) / frequency ? UINT64_MAX :
             (deadline_ms * frequency + 999U) / 1000U;
#if CONFIG_KSHIM_IRQ
    if (timer_can_sleep())
    {
        sleep_until(target, true);
        return;
    }
#endif
    // Interrupts are not up: nothing can end the wait early
    while (arm64_read_cntpct() < target)
        __asm__ volatile("yield" ::: "memory");
}

#if CONFIG_KSHIM_IRQ
// Timer interrupt handler: one-shot, so mask the output until re-armed.
static void arm64_arch_timer_interrupt_handler(
    uint32_t intid,
    void *context)
{
    (void)intid;
    (void)context;
    timer_set_ctl(CNT_CTL_IMASK);
    timer_context.irqs++;
}

uint32_t arm64_arch_timer_intid(void)
{
    return timer_context.ready ? timer_context.intid : 0;
}

uint64_t arm64_arch_timer_irqs(void)
{
    return timer_context.irqs;
}

// arm64 arch timer interrupt register
int arm64_arch_timer_init(const struct kshim_timer_desc *desc)
{
    uint32_t el = (uint32_t)arm64_current_el();
    uint32_t intid;
    int status;

    if (!desc)
        return ERROR_INVALID_PARAMETER;
    intid = el == 2 ? desc->intid[KSHIM_TIMER_HYP_PHYS] : desc->intid[KSHIM_TIMER_VIRT];
    if (!intid)
        return ERROR_NOT_FOUND;
    // Initialize timer context
    timer_context = (ARM64_ARCH_TIMER_CONTEXT){
        .intid = intid,
        .el = el,
        .mpidr = arm64_read_mpidr() & ARM_MPIDR_AFFINITY_MASK,
    };

    // 1. Save and disable the inherited timer
    timer_context.saved_ctl = timer_get_ctl();
    timer_context.saved_cval = timer_get_cval();
    timer_disarm();

    // 2. Register timer interrupt handler (level PPI, trigger as wired)
    status = kshim_irq_request_internal(intid, KSHIM_IRQ_TYPE_NONE,
                                        arm64_arch_timer_interrupt_handler, NULL);
    if (status)
    {
        timer_set_cval(timer_context.saved_cval);
        timer_set_ctl(timer_context.saved_ctl);
        return status;
    }
    timer_context.ready = 1;
    return 0;
}

void arm64_arch_timer_teardown(void)
{
    if (!timer_context.ready)
        return;
    timer_context.ready = 0;
    timer_disarm();
    timer_set_cval(timer_context.saved_cval);
    timer_set_ctl(timer_context.saved_ctl);
}
#endif
