/* SPDX-License-Identifier: MIT */
/*
 * kShimC SPMI transport with the interrupt cascade (CONFIG_QCOM_IRQCHIP):
 * write grants cover exactly the RW channel, observer command and PIC words
 * of a claimed peripheral, are reference counted, and every thread-context
 * arbiter transaction runs with IRQs masked. Array-backed PMIC Arbiter v5.
 */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <kshim/irq.h>
#include <qcom_key_platform.h>

static uint32_t Core[0x1100 / 4], Chnls[0x40000 / 4], Obs[0x1000 / 4], Intr[0x1000 / 4], Cnfg[0x1000 / 4];
static unsigned Saves, Depth;

unsigned long kshim_irq_save(void) { ++Saves; return Depth++; }
void kshim_irq_restore(unsigned long flags) { assert(Depth && --Depth == flags); }

static uint32_t *apid_word(unsigned apid, unsigned offset) { return &Chnls[(0x10000U * apid + offset) / 4]; }

int main(void)
{
    static SpmiDeviceContext ctx;
#define REGION(Type, Array) ctx.Regions[SPMI_MEMORY_REGION_TYPE_##Type] = (SpmiMemoryRegion){(uintptr_t)(Array), sizeof(Array)}
    REGION(CORE, Core); REGION(CH_SLAVES, Chnls); REGION(OBSERVER, Obs);
    REGION(INTERRUPT, Intr); REGION(CONFIG, Cnfg);
#undef REGION
    Core[0] = 0x50000000U;                      /* v5 */
    Core[1] = 4;                                /* APIDs */
    Core[(0x900 + 4 * 2) / 4] = (0x0008U << 8) | (1U << 24); /* PON */
    Core[(0x900 + 4 * 3) / 4] = (0x00c5U << 8) | (1U << 24); /* PMIC GPIO6 */
    SpmiDeviceContext *contexts[] = {&ctx};
    kshim_qcom_spmi_bind(contexts, 1);
    SpmiDeviceContext *spmi = kshim_qcom_spmi_context(0);
    assert(spmi == &ctx && ctx.Initialized && ctx.Version == SPMI_ARB_VERSION_5);
    assert(!kshim_qcom_spmi_context(1));
    *apid_word(2, 8) = 1; *apid_word(3, 8) = 1; /* channel STATUS: done */
    Obs[(0x80 * 2 + 8) / 4] = 1;
    Obs[(0x80 * 2 + 0x18) / 4] = 0xa5;
    const uint8_t on = 1;

    /* Without a grant SpmiLib cannot reach IRQ configuration. */
    (void)SpmiWrite(spmi, 0, 0x815, &on, 1);
    assert(*apid_word(2, 0) == 0 && *apid_word(2, 0x10) == 0);
    assert(kshim_qcom_spmi_grant(0, 0, 0x99) != 0); /* no APID */
    assert(kshim_qcom_spmi_grant(0, 16, 0x08) != 0 && kshim_qcom_spmi_grant(1, 0, 0x08) != 0);

    assert(!kshim_qcom_spmi_grant(0, 0, 0x08));
    assert(SpmiWrite(spmi, 0, 0x815, &on, 1) == CR_SUCCESS);
    assert(*apid_word(2, 0x10) == 1 && *apid_word(2, 0) == ((0x15U << 4)));
    spmi->Io.Write32(spmi->Io.Context, (UINTN)apid_word(2, 0x100), 1); /* ACC_ENABLE */
    spmi->Io.Write32(spmi->Io.Context, (UINTN)apid_word(2, 0x108), 4); /* IRQ_CLEAR */
    spmi->Io.Write32(spmi->Io.Context, (UINTN)apid_word(2, 0x104), 9); /* IRQ_STATUS: no */
    spmi->Io.Write32(spmi->Io.Context, (UINTN)apid_word(3, 0x100), 1); /* other APID: no */
    assert(*apid_word(2, 0x100) == 1 && *apid_word(2, 0x108) == 4);
    assert(*apid_word(2, 0x104) == 0 && *apid_word(3, 0x100) == 0);
    uint8_t value = 0;
    assert(SpmiRead(spmi, 0, 0x810, &value, 1) == CR_SUCCESS && value == 0xa5);
    assert(Obs[(0x80 * 2) / 4] == ((1U << 27) | (0x10U << 4)));

    /* Reference counted: two bits of one peripheral share the grant. */
    assert(!kshim_qcom_spmi_grant(0, 0, 0x08));
    kshim_qcom_spmi_revoke(0, 0, 0x08);
    *apid_word(2, 0x100) = 0;
    spmi->Io.Write32(spmi->Io.Context, (UINTN)apid_word(2, 0x100), 1);
    assert(*apid_word(2, 0x100) == 1);
    kshim_qcom_spmi_revoke(0, 0, 0x08);
    *apid_word(2, 0x100) = 0;
    spmi->Io.Write32(spmi->Io.Context, (UINTN)apid_word(2, 0x100), 1);
    assert(*apid_word(2, 0x100) == 0);

    /* Thread-context transactions run with IRQs masked. */
    unsigned before = Saves;
    uint8_t byte;
    assert(!kshim_qcom_power_read8(0, 0, 0, 0x810, &byte) && byte == 0xa5);
    assert(Saves > before && Depth == 0);
    printf("spmi grants: ok\n");
    return 0;
}
