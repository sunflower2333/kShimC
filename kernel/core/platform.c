/* SPDX-License-Identifier: MIT */
/*
 * Kernel platform lifecycle: SMP/MMU bring-up for the UI, the ordered
 * handoff teardown, and the standalone entry into userspace. Boot-entry
 * selection itself lives in user/bootmenu.
 */
#include <arm64_exception.h>
#include <kshim.h>
#include <kshim/boot.h>
#include <kshim/user.h>
#include <platform.h>
#include <smp.h>
#include <smp_selftest.h>
#if CONFIG_KSHIM_IRQ_SELFTEST
#include <irq_selftest.h>
#endif
#include <psci.h>
#include <kshim_mmu.h>
#include <config.h>
#include <uefi_boot.h>
#include <kshim/irq.h>
#include <kshim_console.h>
#include <kshim/io.h>
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

/* docs/interrupts.md handoff steps 1-3. kshim_user_select() and the power
 * menu reach this through kshim_platform_prepare_handoff() and prepare_boot()
 * runs it again; every step is idempotent. */
int kshim_platform_quiesce(void)
{
    /* 1. Input and touch quiesce: no input interrupt fires from here on. */
    if (kshim_input_quiesce()) return -1;
#if CONFIG_KSHIM_CONSOLE_RX_IRQ
    if (kshim_console_rx_irq_stop()) return -1;
#endif
    /* 2. LVGL deinit and worker shutdown. */
    int status = kshim_user_prepare_handoff();
    if (status) return status;
    /* Workers restore their own CPU interface before PSCI CPU_OFF. */
    if (SmpInitialized) {
        if (kshim_smp_shutdown(KSHIM_SMP_DEFAULT_TIMEOUT_US)) return -2;
        SmpInitialized = 0;
    }
    /* 3a. I/O stack (docs/io-stack.md): USB -> xHCI -> PCI -> touch/I2C/SPI
     *     -> QUP -> clocks/regulators/GPIO restore. No-op when not configured. */
    if (kshim_io_quiesce()) return -5;
#if CONFIG_QCOM_IRQCHIP
    /* 3b. Secondary controllers: TLMM pins, SPMI arbiter, PDC routes, before
     *     kshim_irq_teardown() (4) restores the GIC. */
    if (kshim_irqchip_restore()) return -4;
#endif
    return 0;
}

int kshim_platform_prepare_handoff(void)
{
    int status = kshim_platform_quiesce();
    if (status) return status;
    /* Interrupt state goes back to the previous bootloader's before the MMU
     * (and with it the GIC mapping and cache coherency) is torn down. */
    if (kshim_irq_teardown()) return -3;
    arm64_exception_restore();
    return kshim_mmu_prepare_handoff();
}

int kshim_platform_main(uint64_t fdt, uint64_t arg1, uint64_t arg2, uint64_t arg3)
{
    kshim_boot_set_args(fdt, arg1, arg2, arg3);
#if CONFIG_KSHIM_SMP_SELFTEST
    int status = kshim_smp_selftest((const void *)(uintptr_t)fdt);
    printf("SMP_SELFTEST %s (%d)\n", status == 0 ? "PASS" : "FAIL", status);
#endif
#if CONFIG_KSHIM_IRQ_SELFTEST
    int irq_status = kshim_irq_selftest((const void *)(uintptr_t)fdt);
    printf("IRQ_SELFTEST %s (%d)\n", irq_status == 0 ? "PASS" : "FAIL", irq_status);
#endif
#if CONFIG_KSHIM_SMP_SELFTEST || CONFIG_KSHIM_IRQ_SELFTEST
    kshim_psci_system_off();
#endif
    return kshim_user_main(fdt);
}
