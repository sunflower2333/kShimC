/* SPDX-License-Identifier: MIT */
#include <asm/asm_macro.h>
// Enable GIC cpu interface
ASM_FUNC(arm64_gicv3_enable_gicc)
    mov x0, #0x1
    msr ICC_IGRPEN1_EL1, x0
    isb
    ret

// Disable GIC cpu interface
ASM_FUNC(arm64_gicv3_disable_gicc)
    mov x0, #0
    msr ICC_IGRPEN1_EL1, x0
    isb
    ret

// uint64_t arm64_gicv3_get_igrpen1(void)
ASM_FUNC(arm64_gicv3_get_igrpen1)
    mrs x0, ICC_IGRPEN1_EL1
    ret

// void arm64_gicv3_set_igrpen1(uint64_t value)
ASM_FUNC(arm64_gicv3_set_igrpen1)
    msr ICC_IGRPEN1_EL1, x0
    isb
    ret

// Set binary point register
// void arm64_set_bpr(uint64_t bpr)
ASM_FUNC(arm64_gicv3_set_bpr)
    msr ICC_BPR1_EL1, x0
    isb
    ret

// uint64_t arm64_gicv3_get_bpr(void)
ASM_FUNC(arm64_gicv3_get_bpr)
    mrs x0, ICC_BPR1_EL1
    ret

// Set priority mask register
// void arm64_set_pmr(uint64_t pmr)
ASM_FUNC(arm64_gicv3_set_pmr)
    msr ICC_PMR_EL1, x0
    isb
    ret

// uint64_t arm64_gicv3_get_pmr(void)
ASM_FUNC(arm64_gicv3_get_pmr)
    mrs x0, ICC_PMR_EL1
    ret

// uint64_t arm64_gicv3_get_ctlr(void)
ASM_FUNC(arm64_gicv3_get_ctlr)
    mrs x0, ICC_CTLR_EL1
    ret

// void arm64_gicv3_set_ctlr(uint64_t value)
ASM_FUNC(arm64_gicv3_set_ctlr)
    msr ICC_CTLR_EL1, x0
    isb
    ret

// System register interface enable at EL1 and EL2
ASM_FUNC(arm64_gicv3_get_sre_el1)
    mrs x0, ICC_SRE_EL1
    ret

ASM_FUNC(arm64_gicv3_set_sre_el1)
    msr ICC_SRE_EL1, x0
    isb
    ret

ASM_FUNC(arm64_gicv3_get_sre_el2)
    mrs x0, ICC_SRE_EL2
    ret

ASM_FUNC(arm64_gicv3_set_sre_el2)
    msr ICC_SRE_EL2, x0
    isb
    ret

// Raise a Group 1 SGI; stores must be visible to the target first.
// void arm64_gicv3_write_sgi1r(uint64_t value)
ASM_FUNC(arm64_gicv3_write_sgi1r)
    dsb ishst
    msr ICC_SGI1R_EL1, x0
    isb
    ret

// Write end of interrupt register
// void arm64_write_eoi(uint64_t interrupt_no)
ASM_FUNC(arm64_gicv3_write_eoir1)
    msr ICC_EOIR1_EL1, x0
    isb
    ret

// Read interrupt acknowledge register
// uint64_t arm64_gicv3_acknowledge_interrupt(void)
ASM_FUNC(arm64_gicv3_acknowledge_interrupt)
    mrs x0, ICC_IAR1_EL1
    dsb sy
    ret
