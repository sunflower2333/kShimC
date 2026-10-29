/* SPDX-License-Identifier: MIT */
/* Minimal EL1 entry for the QEMU storage smoke test (MMU and caches off). */
    .section .text.start, "ax"
    .global _start
_start:
    mrs x0, CurrentEL
    ldr x1, =__stack_top
    mov sp, x1
    ldr x1, =__bss_start
    ldr x2, =__bss_end
1:  cmp x1, x2
    b.hs 2f
    str xzr, [x1], #8
    b 1b
2:  /* Allow FP/SIMD (compiler may use q registers for copies). */
    mov x1, #(3 << 20)
    msr cpacr_el1, x1
    isb
    bl smoke_main
    /* PSCI SYSTEM_OFF */
    mov x0, #8
    movk x0, #0x8400, lsl #16
    hvc #0
3:  wfe
    b 3b
