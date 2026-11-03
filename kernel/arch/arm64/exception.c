/* SPDX-License-Identifier: MIT */
#include <arm64_exception.h>
#include <asm/arm64_exception.h>
#include <arm64_sysreg.h>
#include <gicv3.h>

// Synchronous exception handler
void arm64_synchronous_exception_handler(
    ARM64_EXCEPTION_CONTEXT *context,
    uint64_t exception_type)
{
    static const char *const names[] = {"Synchronous exception", "IRQ", "FIQ", "SError"};
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