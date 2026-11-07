/* SPDX-License-Identifier: MIT */
#include <arm64_exception.h>
#include <asm/arm64_exception.h>
#include <arm64_sysreg.h>
#include <gicv3.h>
#include <kshim_build_id_compat.h>
#include <stdbool.h>

/* VBAR_EL1 as the previous bootloader left it, and whether ours replaced it. */
static uint64_t InheritedVbar;
static bool OwnVbar;

static uint64_t current_el(void)
{
    uint64_t el;
    __asm__ volatile("mrs %0, CurrentEL" : "=r"(el));
    return (el >> 2) & 3;
}

/* First thing in kshim_main: until now VBAR still points at the previous
 * bootloader's handlers. After ExitBootServices those are dead UEFI code, so
 * a kShimC fault was reported as a "Thread UEFI DXE" panic with UEFI's state.
 * At EL2 the interrupt core installs the EL2 table. */
void arm64_exception_early_init(void)
{
    if (current_el() != 1 || OwnVbar)
        return;
    __asm__ volatile("mrs %0, vbar_el1" : "=r"(InheritedVbar));
    arm64_install_exception_vector();
    OwnVbar = true;
}

/* VBAR_EL1 as the previous bootloader left it: the value a payload receives.
 * Equals the live register once arm64_exception_restore() has run. */
uint64_t arm64_exception_inherited_vbar(void)
{
    uint64_t live;
    if (OwnVbar)
        return InheritedVbar;
    __asm__ volatile("mrs %0, vbar_el1" : "=r"(live));
    return live;
}

/* Payload handoff, after kshim_irq_teardown() (which restores the VBAR it
 * saw, by then ours): give the payload the bootloader's VBAR back. */
void arm64_exception_restore(void)
{
    if (!OwnVbar)
        return;
    __asm__ volatile("msr vbar_el1, %0\n isb" : : "r"(InheritedVbar) : "memory");
    OwnVbar = false;
}

// Synchronous exception handler
void arm64_synchronous_exception_handler(
    ARM64_EXCEPTION_CONTEXT *context,
    uint64_t exception_type)
{
    static const char *const names[] = {"Synchronous exception", "IRQ", "FIQ", "SError"};
    printf("kShimC %s: fatal exception\n", KSHIM_BUILD_ID);
    // Print registers
    printf_e("%s occurred at EL%lu\n", exception_type < 4 ? names[exception_type] : "Exception",
             (unsigned long)arm64_current_el());
    // GP registers
    for (int i = 0; i < 32; i += 2)
    {
        printf("X%d: 0x%016lx X%d: 0x%016lx\n", i, context->x[i], i + 1, context->x[i + 1]);
    }

    // FP registers
    for (int i = 0; i < 32; i++)
    {
        printf("Q%d[0]: 0x%016lx Q%d[1]: 0x%016lx\n", i, context->q[i][0], i, context->q[i][1]);
    }

    // Sys registers
    printf("ELR: 0x%016lx\n", context->elr);
    printf("SPSR: 0x%016lx\n", context->spsr);
    printf("FPSR: 0x%016lx\n", context->fpsr);
    printf("ESR: 0x%016lx\n", context->esr);
    printf("FAR: 0x%016lx\n", context->far);

    // Print Stack trace
    printf("Stack trace:\n");
    uint64_t *stack_ptr = (uint64_t *)context->x[31]; // Stack pointer
    for (int i = 0; i < 16; i++)
    {
        printf("0x%016lx\n", stack_ptr[i]);
    }
    
    // Disable interrupts
    arm64_disable_interrupts();
    // Hang up when exception occurs
    while (1)
        ;
}

void arm64_expetion_handler(
    ARM64_EXCEPTION_CONTEXT *context,
    uint64_t exception_type)
{
    switch (exception_type)
    {
    case EXCEPTION_TYPE_SYNCHRONOUS:
        // Synchronous exception handler
        arm64_synchronous_exception_handler(context, exception_type);
        break;
    case EXCEPTION_TYPE_IRQ:
        // Irq handler: acknowledge, dispatch by INTID, EOI
        gicv3_irq_handler(context, exception_type);
        break;
    case EXCEPTION_TYPE_FIQ:
        // kShimC only uses Group 1; a FIQ is unexpected
        arm64_synchronous_exception_handler(context, exception_type);
        break;
    case EXCEPTION_TYPE_SERROR:
        arm64_synchronous_exception_handler(context, exception_type);
        break;
    default:
        break;
    }
}