#include <asm/asm_macro.h>
#include <asm/arm64_exception.h>
#include <arm64_sysreg.h>

// void arm64_idle_processor_startup(void)
// void arm64_idle_processor_startup(
//    uint64 stack_base,
//    uint64_t stack_size,
//    uint64_t entrypoint)
ASM_FUNC(arm64_idle_processor_startup)
    /* Store func params */
    mov x10, x0
    mov x11, x1
    mov x12, x2

    /* Set svc mode */
    mrs x0, sctlr_el1
    orr x0, x0, #SCTLR_SPAN_BIT
    msr sctlr_el1, x0
    
    /* Not trap simd */
    mrs x0, cpacr_el1
    orr x0, x0, CPACR_FPEN_BIT
    msr cpacr_el1, x0
    isb sy

    /* assume stack is zeroed by primary core */
    
    /* start task */
_start_process:
    mov x0, x10
    mov sp, x0
    mov x1, x11
    mov x2, x12
    blr x2

/* Should never get here */
_dead:
    b .
