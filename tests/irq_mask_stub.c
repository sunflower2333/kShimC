/* SPDX-License-Identifier: MIT */
/* Host stand-in for the core's local IRQ masking (kernel/irq/irq.c masks
 * PSTATE.I); tests without interrupt handlers only need it to nest. */
#include <assert.h>
#include <kshim/irq.h>

static _Thread_local unsigned long Depth;

unsigned long kshim_irq_save(void) { return Depth++; }
void kshim_irq_restore(unsigned long flags)
{
    assert(Depth && Depth - 1 == flags);
    Depth = flags;
}
