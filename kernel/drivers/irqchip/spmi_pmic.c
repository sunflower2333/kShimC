/* SPDX-License-Identifier: MIT */
/*
 * SPMI PMIC arbiter cascade over CrDK SpmiSharedCrLib/SpmiLib.
 *
 * Each claimed QPNPINT bit is one SpmiSharedCrLib client with a single
 * (sid, peripheral) READ region; Subscribe snapshots SET_TYPE,
 * POLARITY_HIGH/LOW and EN_SET for that bit plus the APID's PIC enable,
 * and Unsubscribe restores and reads them back. SpmiDispatchIrqs (through
 * CrSpmiSharedDispatch) clears the arbiter IRQ_CLEAR and the PMIC
 * LATCHED_CLR before the callbacks. The kShimC SPMI transport only lets
 * SpmiLib write the channel and PIC words of claimed APIDs
 * (kshim_qcom_spmi_grant), so foreign peripherals stay untouched.
 *
 * SharedParentIrqScan is the policy SpmiSharedCrLib requires: a pending
 * APID without a client, or a failed acknowledge, makes the dispatch fail
 * and the parent (periph_irq) is masked here rather than storming; the
 * clients then fall back to polling (kshim_spmi_irq_live() turns false).
 *
 * SpmiSharedCrLib callbacks may not reenter the bus, so the callback only
 * marks the slot; the kShimC handler runs after the dispatch returns, still
 * in IRQ context, where it may read the peripheral.
 *
 * One CR_SPMI_SHARED (7.6 KiB) serves the first arbiter that is claimed;
 * claims on a second arbiter are refused and those clients poll. Every
 * supported board has its keys on one arbiter.
 */
#include "irqchip.h"
#include <qcom_dt.h>
#include <qcom_key_platform.h>
#include <Library/SpmiSharedCrLib.h>

#define SPMI_IRQ_SLOTS CR_SPMI_MAX_CLIENTS

struct spmi_arb {
    bool ready, parent_requested, sealed;
    unsigned controller;
    BOOLEAN saved_interrupt, saved_external, saved_scan;
    SpmiDeviceContext *spmi;
    struct kshim_dt_irq parent_spec;
    uint32_t parent;
    CR_STATUS last_dispatch;
    CR_SPMI_SHARED shared;
};

struct spmi_slot {
    bool used;
    volatile bool pending;
    unsigned controller;
    uint8_t sid, peripheral, irq;
    kshim_spmi_irq_handler_t handler;
    void *context;
    CR_SPMI_REGION region;
    CR_SPMI_CLIENT client;
    CR_SPMI_BUS bus;
    CR_SPMI_IRQ token;
};

static struct spmi_arb Arb;
static struct spmi_slot Slots[SPMI_IRQ_SLOTS];

/* The cascade instance for `controller`, free or already bound to it. */
static struct spmi_arb *arb_for(unsigned controller)
{
    return !Arb.ready || Arb.controller == controller ? &Arb : NULL;
}

static bool arb_has_slots(unsigned controller)
{
    for (unsigned i = 0; i < SPMI_IRQ_SLOTS; ++i)
        if (Slots[i].used && Slots[i].controller == controller) return true;
    return false;
}

static void notify(void *context)
{
    ((struct spmi_slot *)context)->pending = true;
}

static void arb_isr(uint32_t intid, void *context)
{
    struct spmi_arb *a = context;
    unsigned controller = a->controller;
    CR_STATUS status = CrSpmiSharedDispatch(&a->shared);
    a->last_dispatch = status;
    if (status != CR_SUCCESS && !a->sealed) {
        kshim_irq_disable(intid);
        a->sealed = true;
    }
    for (unsigned i = 0; i < SPMI_IRQ_SLOTS; ++i) {
        struct spmi_slot *s = &Slots[i];
        if (!s->used || s->controller != controller || !s->pending) continue;
        s->pending = false;
        s->handler(s->context);
    }
}

static void arb_close(struct spmi_arb *a)
{
    if (!a->ready || CrSpmiSharedClose(&a->shared) != CR_SUCCESS ||
        CrSpmiSharedUninit(&a->shared) != CR_SUCCESS) return;
    a->spmi->HasInterrupt = a->saved_interrupt;
    a->spmi->InterruptExternallyManaged = a->saved_external;
    a->spmi->SharedParentIrqScan = a->saved_scan;
    a->ready = a->sealed = false;
}

static int arb_open(struct spmi_arb *a, const void *fdt, unsigned controller)
{
    if (a->ready) return 0;
    SpmiDeviceContext *spmi = kshim_qcom_spmi_context(controller);
    int node = kshim_dt_spmi_controller(fdt, controller);
    if (!spmi || !spmi->Initialized || node < 0) return ERROR_NOT_FOUND;
    if (kshim_dt_irq_named(fdt, node, "periph_irq", &a->parent_spec) &&
        kshim_dt_irq(fdt, node, 0, &a->parent_spec)) return ERROR_NOT_FOUND;
    if (spmi->InterruptRegistered || spmi->ParentIrq) return ERROR_DEVICE_ERROR;
    a->saved_interrupt = spmi->HasInterrupt;
    a->saved_external = spmi->InterruptExternallyManaged;
    a->saved_scan = spmi->SharedParentIrqScan;
    /* kShimC owns the periph_irq line (kshim_irq_request) and masks it on a
     * failed dispatch, which is what SharedParentIrqScan requires. */
    spmi->HasInterrupt = spmi->InterruptExternallyManaged = spmi->SharedParentIrqScan = TRUE;
    if (CrSpmiSharedInit(&a->shared, spmi) != CR_SUCCESS) {
        spmi->HasInterrupt = a->saved_interrupt;
        spmi->InterruptExternallyManaged = a->saved_external;
        spmi->SharedParentIrqScan = a->saved_scan;
        return ERROR_DEVICE_ERROR;
    }
    a->spmi = spmi;
    a->controller = controller;
    a->ready = true;
    a->sealed = a->parent_requested = false;
    a->last_dispatch = CR_SUCCESS;
    return 0;
}

static struct spmi_slot *find(unsigned controller, uint8_t sid, uint8_t peripheral, uint8_t irq)
{
    for (unsigned i = 0; i < SPMI_IRQ_SLOTS; ++i) {
        struct spmi_slot *s = &Slots[i];
        if (s->used && s->controller == controller && s->sid == sid &&
            s->peripheral == peripheral && s->irq == irq) return s;
    }
    return NULL;
}

/* Undo a (partial) claim; IRQs masked. Returns non-zero when the client must
 * be retained because CrDK could not restore and read back its state. */
static int slot_release(struct spmi_arb *a, struct spmi_slot *s)
{
    if (s->token.Generation) {
        (void)s->bus.Enable(&s->bus, &s->token, FALSE);
        if (s->bus.Unsubscribe(&s->bus, &s->token) != CR_SUCCESS) return ERROR_DEVICE_ERROR;
    }
    if (s->client.Generation && CrSpmiSharedRelease(&a->shared, &s->client) != CR_SUCCESS)
        return ERROR_DEVICE_ERROR;
    kshim_qcom_spmi_revoke(s->controller, s->sid, s->peripheral);
    *s = (struct spmi_slot){0};
    return 0;
}

int kshim_spmi_irq_request(const void *fdt, unsigned controller, uint8_t sid,
                           uint8_t peripheral, uint8_t irq, unsigned trigger,
                           kshim_spmi_irq_handler_t handler, void *context)
{
    CR_INTERRUPT_TRIGGER_TYPE type;
    if (!fdt || controller >= KSHIM_SPMI_MAX || sid > 15 || irq > 7 || !handler ||
        !irqchip_cr_trigger(trigger, &type)) return ERROR_INVALID_PARAMETER;
    struct spmi_arb *a = arb_for(controller);
    if (!a) return ERROR_NOT_SUPPORTED;
    unsigned long flags = kshim_irq_save();
    int status = arb_open(a, fdt, controller);
    struct spmi_slot *s = NULL;
    if (!status && (a->sealed || find(controller, sid, peripheral, irq))) status = ERROR_DEVICE_ERROR;
    for (unsigned i = 0; !status && !s && i < SPMI_IRQ_SLOTS; ++i)
        if (!Slots[i].used) s = &Slots[i];
    if (!status && !s) status = ERROR_NOT_SUPPORTED;
    if (status) goto out;
    *s = (struct spmi_slot){.used = true, .controller = controller, .sid = sid,
        .peripheral = peripheral, .irq = irq, .handler = handler, .context = context,
        .region = {sid, peripheral, CR_SPMI_READ, (UINT8)(1U << irq)}};
    if (kshim_qcom_spmi_grant(controller, sid, peripheral)) { *s = (struct spmi_slot){0}; status = ERROR_DEVICE_ERROR; goto out; }
    if (CrSpmiSharedAcquire(&a->shared, s, &s->region, 1, &s->client, &s->bus) != CR_SUCCESS ||
        s->bus.Subscribe(&s->bus, sid, peripheral, irq, type, notify, s, &s->token) != CR_SUCCESS ||
        s->bus.Enable(&s->bus, &s->token, TRUE) != CR_SUCCESS) {
        /* A failed release keeps the slot owned for the handoff retry. */
        (void)slot_release(a, s);
        status = ERROR_DEVICE_ERROR;
    }
out:
    if (status && a->ready && !arb_has_slots(controller) && !a->parent_requested) arb_close(a);
    kshim_irq_restore(flags);
    if (status || a->parent_requested) return status;
    /* SM8150: <0 481 0> under the PDC root, i.e. PDC pin 1. The arbiter
     * output is level high (CrDK target data, upstream binding). */
    status = kshim_qcom_irq_request_spec(fdt, &a->parent_spec, KSHIM_IRQ_TYPE_LEVEL_HIGH,
                                         arb_isr, a, &a->parent);
    if (status) {
        flags = kshim_irq_save();
        (void)slot_release(a, s);
        if (!arb_has_slots(controller)) arb_close(a);
        kshim_irq_restore(flags);
        return status;
    }
    a->parent_requested = true;
    return 0;
}

static int release_parent(struct spmi_arb *a)
{
    if (!a->parent_requested) return 0;
    int status = kshim_qcom_irq_free(a->parent);
    if (!status) a->parent_requested = false;
    return status;
}

int kshim_spmi_irq_free(unsigned controller, uint8_t sid, uint8_t peripheral, uint8_t irq)
{
    struct spmi_arb *a = arb_for(controller);
    struct spmi_slot *s = find(controller, sid, peripheral, irq);
    if (!a || !s) return ERROR_NOT_FOUND;
    bool last = true;
    for (unsigned i = 0; i < SPMI_IRQ_SLOTS; ++i)
        if (&Slots[i] != s && Slots[i].used && Slots[i].controller == controller) last = false;
    /* Last client: stop periph_irq delivery before the PMIC is restored. */
    int status = last ? release_parent(a) : 0;
    if (status) return status;
    unsigned long flags = kshim_irq_save();
    status = slot_release(a, s);
    if (!status && last) arb_close(a);
    kshim_irq_restore(flags);
    return status;
}

bool kshim_spmi_irq_live(unsigned controller)
{
    return Arb.ready && Arb.controller == controller && Arb.parent_requested && !Arb.sealed;
}

int kshim_spmi_irq_restore(void)
{
    struct spmi_arb *a = &Arb;
    if (!a->ready) return 0;
    int status = release_parent(a);
    unsigned long flags = kshim_irq_save();
    for (unsigned i = 0; !status && i < SPMI_IRQ_SLOTS; ++i)
        if (Slots[i].used) status = slot_release(a, &Slots[i]);
    if (!status) arb_close(a);
    if (!status && a->ready) status = ERROR_DEVICE_ERROR;
    kshim_irq_restore(flags);
    return status;
}
