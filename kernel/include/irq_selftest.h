/* SPDX-License-Identifier: MIT */
#pragma once
/* QEMU interrupt self-test (CONFIG_KSHIM_IRQ_SELFTEST), tools/test_qemu_irq.py. */

/* Before kshim_irq_init(): play the previous bootloader (leave a configured,
 * enabled GIC, CPU interface and timer behind) and snapshot that state. */
void kshim_irq_selftest_inherit(const void *fdt);
/* Timer/WFI, SGI wake-up of workers, PL011 RX interrupt, then the platform
 * handoff and a comparison of the restored state with the snapshot. */
int kshim_irq_selftest(const void *fdt);
