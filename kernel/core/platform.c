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
#include <kshim/irq.h>

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

int kshim_platform_quiesce(void)
{
#if CONFIG_KSHIM_TOUCH
    if (kshim_input_touch_stop()) return -1;
#endif
    int status = kshim_user_prepare_handoff();
    if (status) return status;
    /* Workers restore their own CPU interface before PSCI CPU_OFF. */
    if (SmpInitialized) {
        if (kshim_smp_shutdown(KSHIM_SMP_DEFAULT_TIMEOUT_US)) return -2;
        SmpInitialized = 0;
    }
    return 0;
}

int kshim_platform_prepare_handoff(void)
{
    int status = kshim_platform_quiesce();
    if (status) return status;
    /* Interrupt state goes back to the previous bootloader's before the MMU
     * (and with it the GIC mapping and cache coherency) is torn down. */
    if (kshim_irq_teardown()) return -3;
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
