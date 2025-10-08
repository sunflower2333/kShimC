#include <asm/asm_macro.h>
#include <arm64_sysreg.h>
#include <ld/arm64_ld.h>

.section .text.boot
.align 3

/* entry point */
.global _start

_start:
    b       _platform_init        // Code 1, Goto ShellCode

_LinuxStart:
    nop                 // Code 2
    .quad   0           // Image load offset
    .quad   0x00400000  // Effective Image size
    .long   0           // Flags L
    .long   0           // Flags H

_StackBase:
    .long   0           // reserved2, Stack Base LWORD
    .long   0           // reserved2, Stack Base HWORD

_StackSize:
    .quad LD_STACK_SIZE // reserved3

_KernelSize:
    .long   0           // reserved4, Kernel Size L
    .long   0           // reserved4, Kernel Size H

    .ascii  "ARM\x64"   // magic_64
    .long   0           // reserve5

_platform_init:
    /* Store linux kernel params */
    mov x10, x0
    mov x11, x1
    mov x12, x2
    mov x13, x3

    /* Clear registers */
    mov x29, xzr // Clean frame

    /* Set svc mode */
    mrs x0, sctlr_el1
    orr x0, x0, #SCTLR_SPAN_BIT
    msr sctlr_el1, x0

_setup_simd:
    /* Not trap simd */
    mrs x0, cpacr_el1
    orr x0, x0, CPACR_FPEN_BIT
    msr cpacr_el1, x0
    isb sy

// Calculate and fix got address
_setup_gots:
    adrp x0, _start         // load address of program
    add x0, x0, :lo12:_start
    mov x14, x0             // save start address to x14
    adrp x1, __gots_start__ // offset of gots start
    add x1, x1, :lo12:__gots_start__
    adrp x2, __gots_end__   // offset of gots end
    add x2, x2, :lo12:__gots_end__
    sub x2, x2, x1          // size of gots (x2 -x1)

    // check gots size
    cbz x2, _setup_stack    // go over if gots size is 0

    // Pre sub size
    subs x2, x2, #0x8       // 8 bytes per got entry

_setup_gots_loop:
    ldr x3, [x1]        // load got entry
    add x3, x3, x0      // add load address
    str x3, [x1], #0x8  // store back to got entry
    subs x2, x2, #0x8   // 8 bytes per got entry
    b.cs _setup_gots_loop    // x2 >= 0, continue loop

_setup_bss:
    /* Clear bss */
    adrp x0, __bss_start__
    add x0, x0, :lo12:__bss_start__
    adrp x1, __bss_end__
    add x1, x1, :lo12:__bss_end__
    sub x1, x1, x0              // size of bss (x1 -x0)
    cbz x1, _setup_stack       // go over if bss size is 0
    bl _aligned_mem64_set_zero_simd

_setup_stack:
    /* Clear Stack */
    adrp x0, __stack_base__     // stack base in memory
    add x0, x0, :lo12:__stack_base__ 
    adr x1, _StackSize          // use adr here because _StackSize can never be far from here.
    ldr x1, [x1]                // stack size
    add x2, x0, x1              // stack top

    // check stack base and size
    cbz x1, _dead // Invalid stack size

    // clear stack
    bl _aligned_mem64_set_zero_simd

    /* store stack top to sp */
    add sp, x2, xzr

_setup_relocation:
    /* Relocation */
    // Rela
    mov  x0, #0                     // old_image_start
    mov  x1, x14                    // new_image_start
    stp  x0, x1, [sp, #0x10]!       // push old_image_start and new_image_start
    adrp x2, __reladyn_start__      // rela start address
    add  x2, x2, :lo12:__reladyn_start__
    adrp x3, __reladyn_end__        // rela end address
    add  x3, x3, :lo12:__reladyn_end__
    bl do_relocation_rela           // call relocation function

    // Rel
    ldp x0, x1, [sp], #0x10         // pop old_image_start and new_image_start
    adrp x2, __reldyn_start__       // rela start address
    add  x2, x2, :lo12:__reldyn_start__
    adrp x3, __reldyn_end__         // rela end address
    add  x3, x3, :lo12:__reldyn_end__
    bl do_relocation_rel            // call relocation function

_start_kshim:
    mov  x0, x10
    mov  x1, x11
    mov  x2, x12
    mov  x3, x13
    adrp x4, kshim_main
    add  x4, x4, :lo12:kshim_main
    br   x4

/* Should never get here */
_dead:
    b .

// x0: start
// x1: size
_aligned_mem64_set_zero_simd:
    // Clear vfp registers
    movi v3.2d, #0
    mov v4.16b, v3.16b
    mov v5.16b, v3.16b
    mov v6.16b, v3.16b
    subs x1, x1, #0x40
_aligned_mem64_set_zero_simd_loop:
    /* Use SIMD to clear bss*/
    st4 {v3.2d, v4.2d, v5.2d, v6.2d}, [X0], #0x40
    subs x1, x1, #0x40
    // stp xzr, xzr, [x0], #0x20 // store 64 bytes
    // subs x1, x1, #0x20
    b.hs _aligned_mem64_set_zero_simd_loop // x1 >= 0, continue loop
_ret:
    ret

