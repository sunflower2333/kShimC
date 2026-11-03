/* SPDX-License-Identifier: MIT */
/*
 * Kernel platform lifecycle: SMP/MMU bring-up for the UI, the ordered
 * handoff teardown, and the standalone entry into userspace. Boot-entry
 * selection itself lives in user/bootmenu.
 */
#include <kshim.h>
#include <kshim/boot.h>
#include <kshim/user.h>
#include <platform.h>
#include <smp.h>
#include <smp_selftest.h>
#include <psci.h>
#include <kshim_mmu.h>
#include <config.h>
#include <uefi_boot.h>
#include <kshim_console.h>
#if CONFIG_QCOM_IRQCHIP
#include <qcom_irq.h>
#endif

__attribute__((weak)) int kshim_menu_activate(kshim_menu_action_t action)
{ (void)action; return -1; }
static int SmpInitialized;

void kshim_platform_accelerate(const void *fdt)
{
#if CONFIG_KSHIM_SMP
    int status = kshim_smp_init(fdt);
    SmpInitialized = status == 0;
    if (SmpInitialized) status = kshim_smp_start_workers(KSHIM_SMP_DEFAULT_TIMEOUT_US);
    printf("PSCI workers: %u (status %d)\n", (unsigned)kshim_smp_available_count(), status);
#endif
    if (!kshim_mmu_is_enabled() &&
        (kshim_mmu_root_table() || kshim_mmu_configure_default(fdt) == 0))
        (void)kshim_mmu_enable_current_cpu();
}

/* docs/interrupts.md handoff order. kshim_user_select() runs this before
 * returning and prepare_boot() runs it again; every step is idempotent. */
int kshim_platform_prepare_handoff(void)
{
    /* 1. Input and touch quiesce: no input interrupt fires from here on. */
    if (kshim_input_quiesce()) return -1;
#if CONFIG_KSHIM_CONSOLE_RX_IRQ
    if (kshim_console_rx_irq_stop()) return -1;
#endif
    /* 2. LVGL deinit and worker shutdown. */
    int status = kshim_user_prepare_handoff();
    if (status) return status;
#if CONFIG_QCOM_IRQCHIP
    /* 3. Secondary controllers: TLMM pins, SPMI arbiter, PDC routes. */
    if (kshim_irqchip_restore()) return -3;
#endif
    /* 4. kshim_irq_teardown() (GIC core) belongs here, before the CPUs and
     *    the MMU go down. */
    /* 5. SMP workers and the MMU. */
    if (SmpInitialized) {
        if (kshim_smp_prepare_handoff(KSHIM_SMP_DEFAULT_TIMEOUT_US)) return -2;
        SmpInitialized = 0;
        return 0;
    }
    return kshim_mmu_prepare_handoff();
}

int kshim_platform_main(uint64_t fdt, uint64_t arg1, uint64_t arg2, uint64_t arg3)
{
    kshim_boot_set_args(fdt, arg1, arg2, arg3);
#if CONFIG_KSHIM_SMP_SELFTEST
    int status = kshim_smp_selftest((const void *)(uintptr_t)fdt);
    printf("SMP_SELFTEST %s (%d)\n", status == 0 ? "PASS" : "FAIL", status);
    kshim_psci_system_off();
#endif
    return kshim_user_main(fdt);
}
