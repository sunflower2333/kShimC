/* SPDX-License-Identifier: MIT */
/*
 * Interrupt core (kshim/irq.h) over the GICv3 driver: the INTID handler
 * table, request/free, dispatch, the boot-CPU bring-up from the runtime DT
 * and the handoff teardown. With CONFIG_KSHIM_IRQ off every entry point
 * reports ERROR_NOT_SUPPORTED and the runtime keeps polling.
 */
#include <kshim/irq.h>
#include <gicv3.h>
#include <qcom_dt.h>
#include <config.h>

#include <stdio.h>
#include <string.h>

#ifndef CONFIG_KSHIM_IRQ
#define CONFIG_KSHIM_IRQ 0
#endif

/* Local CPU masking, available whatever the configuration (console and
 * allocator paths use it before interrupts are up). Only PSTATE.I. */
unsigned long kshim_irq_save(void)
{
    unsigned long flags;
    __asm__ volatile("mrs %0, daif\n msr daifset, #2" : "=r"(flags) : : "memory");
    return flags;
}

void kshim_irq_restore(unsigned long flags)
{
    __asm__ volatile("msr daif, %0" : : "r"(flags) : "memory");
}

bool arm64_irqs_masked(void)
{
    unsigned long flags;
    __asm__ volatile("mrs %0, daif" : "=r"(flags));
    return (flags & (1UL << 7)) != 0;
}

#if CONFIG_KSHIM_IRQ

#define MAX_HANDLERS 64U

/* The handler table is indexed by INTID through a byte map into a small pool,
 * which keeps the runtime slot cost at ~2 KiB instead of 16 KiB. */
typedef struct {
    kshim_irq_handler_t handler;
    void *context;
    uint16_t intid;
    uint8_t used;
    uint8_t internal;
} irq_slot_t;

static irq_slot_t mSlots[MAX_HANDLERS];
static uint8_t mSlotOf[GICV3_MAX_INTID];
static volatile uint32_t mEvent;
static volatile uint32_t mReady;

static irq_slot_t *slot_find(uint32_t intid, bool create)
{
    if (intid >= GICV3_MAX_INTID)
        return NULL;
    if (mSlotOf[intid])
        return &mSlots[mSlotOf[intid] - 1U];
    if (!create)
        return NULL;
    for (unsigned i = 0; i < MAX_HANDLERS; i++)
    {
        if (!mSlots[i].used)
        {
            mSlots[i] = (irq_slot_t){.intid = (uint16_t)intid, .used = 1};
            mSlotOf[intid] = (uint8_t)(i + 1U);
            return &mSlots[i];
        }
    }
    return NULL;
}

void kshim_irq_dispatch(uint32_t intid)
{
    irq_slot_t *slot = slot_find(intid, false);
    kshim_irq_handler_t handler = slot ? slot->handler : NULL;

    if (!handler)
    {
        // Enabled without an owner: keep it from firing again
        gicv3_disable_interrupt((uint16_t)intid);
        return;
    }
    handler(intid, slot->context);
    if (!slot->internal)
        mEvent = 1;
}

bool kshim_irq_take_event(void)
{
    uint32_t event = mEvent;
    mEvent = 0;
    return event != 0;
}

static void wake_sgi(uint32_t intid, void *context)
{
    (void)intid;
    (void)context;
    GICV3_CPU *cpu = gicv3_this_cpu();
    if (cpu)
        cpu->wake_sgis++;
}

static int request(uint32_t intid, unsigned trigger, kshim_irq_handler_t handler,
                   void *context, bool internal)
{
    irq_slot_t *slot;
    unsigned long flags;
    int status;

    if (!mReady)
        return ERROR_NOT_READY;
    if (!handler || intid >= gicv3_max_intid())
        return ERROR_INVALID_PARAMETER;
    flags = kshim_irq_save();
    slot = slot_find(intid, true);
    if (!slot)
        status = ERROR_NOT_SUPPORTED;
    else if (slot->handler)
        status = ERROR_BUSY;
    else
    {
        // Configuration changes need the interrupt disabled
        gicv3_disable_interrupt((uint16_t)intid);
        status = gicv3_configure_interrupt(intid, trigger);
        if (!status)
        {
            slot->context = context;
            slot->internal = internal;
            slot->handler = handler;
            gicv3_enable_interrupt((uint16_t)intid);
        }
    }
    kshim_irq_restore(flags);
    return status;
}

int kshim_irq_request_internal(uint32_t intid, unsigned trigger, kshim_irq_handler_t handler,
                               void *context)
{
    return request(intid, trigger, handler, context, true);
}

int kshim_irq_request(uint32_t intid, unsigned trigger, kshim_irq_handler_t handler,
                      void *context)
{
    return request(intid, trigger, handler, context, false);
}

int kshim_irq_free(uint32_t intid)
{
    unsigned long flags;
    irq_slot_t *slot;
    int status = ERROR_NOT_FOUND;

    if (!mReady)
        return ERROR_NOT_READY;
    flags = kshim_irq_save();
    slot = slot_find(intid, false);
    if (slot && slot->handler)
    {
        gicv3_disable_interrupt((uint16_t)intid);
        gicv3_clear_interrupt((uint16_t)intid);
        // Keep the slot: the INTID's saved configuration stays bound to it
        slot->handler = NULL;
        slot->context = NULL;
        status = 0;
    }
    kshim_irq_restore(flags);
    return status;
}

void kshim_irq_enable(uint32_t intid)
{
    if (mReady && intid < gicv3_max_intid())
        gicv3_enable_interrupt((uint16_t)intid);
}

void kshim_irq_disable(uint32_t intid)
{
    if (mReady && intid < gicv3_max_intid())
        gicv3_disable_interrupt((uint16_t)intid);
}

int kshim_irq_send_sgi(uint32_t sgi, uint64_t mpidr)
{
    return mReady ? gicv3_send_sgi(sgi, mpidr) : ERROR_NOT_READY;
}

bool kshim_irq_ready(void)
{
    return mReady != 0;
}

bool kshim_irq_cpu_can_sleep(void)
{
    GICV3_CPU *cpu;
    if (!mReady || arm64_irqs_masked())
        return false;
    cpu = gicv3_this_cpu();
    return cpu && cpu->state == GICV3_CPU_UP;
}

static int enable_wake_sgi(void)
{
    int status = gicv3_configure_interrupt(KSHIM_IRQ_SGI(KSHIM_SGI_WAKE), KSHIM_IRQ_TYPE_NONE);
    if (!status)
        gicv3_enable_interrupt(KSHIM_IRQ_SGI(KSHIM_SGI_WAKE));
    return status;
}

int kshim_irq_init(const void *fdt)
{
    struct kshim_resources *resources = kshim_resources_get();
    struct kshim_timer_desc timer;
    int status;

    if (mReady)
        return 0;
    if (!fdt)
        return ERROR_INVALID_PARAMETER;
    // The GIC must be in the resource map the MMU is built from
    if (resources->fdt != fdt || resources->gic_status)
        return ERROR_NOT_FOUND;
    memset(mSlots, 0, sizeof(mSlots));
    memset(mSlotOf, 0, sizeof(mSlotOf));
    mEvent = 0;
    status = gicv3_init(&resources->gic);
    if (status)
    {
        printf("IRQ: GICv3 bring-up failed (%d), polling\n", status);
        return status;
    }
    mReady = 1;
    status = request(KSHIM_IRQ_SGI(KSHIM_SGI_WAKE), KSHIM_IRQ_TYPE_NONE, wake_sgi, NULL, true);
    if (status)
    {
        (void)kshim_irq_teardown();
        printf("IRQ: wake SGI unavailable (%d), polling\n", status);
        return status;
    }
    // Without a usable timer interrupt, waits keep busy-polling.
    status = kshim_dt_arch_timer(fdt, &timer);
    if (!status)
        status = arm64_arch_timer_init(&timer);
    printf("IRQ: GICv3 %#llx, %u lines, EL%u, timer INTID %u%s\n",
           (unsigned long long)resources->gic.dist.base, (unsigned)gicv3_max_intid(),
           (unsigned)arm64_current_el(), (unsigned)arm64_arch_timer_intid(),
           status ? " unavailable" : "");
    // Unmask IRQ only; SError, Debug and FIQ keep their inherited masks.
    arm64_enable_interrupts();
    return 0;
}

int kshim_irq_cpu_init(void)
{
    GICV3_CPU *cpu;
    int status;

    if (!mReady)
        return ERROR_NOT_READY;
    cpu = gicv3_this_cpu();
    if (cpu && cpu->state == GICV3_CPU_UP)
        return 0;
    status = gicv3_cpu_init();
    if (!status)
        status = enable_wake_sgi();
    if (status)
    {
        (void)gicv3_cpu_teardown();
        return status;
    }
    arm64_enable_interrupts();
    return 0;
}

int kshim_irq_cpu_teardown(void)
{
    __asm__ volatile("msr daifset, #2" ::: "memory");
    return mReady ? gicv3_cpu_teardown() : 0;
}

int kshim_irq_teardown(void)
{
    int status;

    if (!mReady)
        return 0;
    // Contract: DAIF.I stays masked for the payload
    __asm__ volatile("msr daifset, #2" ::: "memory");
    arm64_arch_timer_teardown();
    status = gicv3_teardown();
    mReady = 0;
    memset(mSlots, 0, sizeof(mSlots));
    memset(mSlotOf, 0, sizeof(mSlotOf));
    mEvent = 0;
    /* Every register was written back; only a completion (RWP/WAKER) was not
     * observed in time. Like Linux, warn and go on: refusing the handoff
     * would strand the device over a slow vGIC. */
    if (status)
        printf("IRQ: GIC restore unconfirmed (%d), continuing\n", status);
    return 0;
}

#else /* !CONFIG_KSHIM_IRQ */

int kshim_irq_init(const void *fdt) { (void)fdt; return ERROR_NOT_SUPPORTED; }
int kshim_irq_cpu_init(void) { return ERROR_NOT_SUPPORTED; }
bool kshim_irq_ready(void) { return false; }
int kshim_irq_request(uint32_t intid, unsigned trigger, kshim_irq_handler_t handler, void *context)
{
    (void)intid; (void)trigger; (void)handler; (void)context;
    return ERROR_NOT_SUPPORTED;
}
int kshim_irq_request_internal(uint32_t intid, unsigned trigger, kshim_irq_handler_t handler,
                               void *context)
{
    return kshim_irq_request(intid, trigger, handler, context);
}
int kshim_irq_free(uint32_t intid) { (void)intid; return ERROR_NOT_SUPPORTED; }
void kshim_irq_enable(uint32_t intid) { (void)intid; }
void kshim_irq_disable(uint32_t intid) { (void)intid; }
int kshim_irq_send_sgi(uint32_t sgi, uint64_t mpidr) { (void)sgi; (void)mpidr; return ERROR_NOT_SUPPORTED; }
int kshim_irq_teardown(void) { return 0; }
int kshim_irq_cpu_teardown(void) { return 0; }
bool kshim_irq_cpu_can_sleep(void) { return false; }
bool kshim_irq_take_event(void) { return false; }
void kshim_irq_dispatch(uint32_t intid) { (void)intid; }

#endif
