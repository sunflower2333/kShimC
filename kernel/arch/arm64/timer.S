/* SPDX-License-Identifier: MIT */
#include <asm/asm_macro.h>

ASM_FUNC(arm64_read_cntfrq)
    mrs x0, cntfrq_el0 
    ret

/* Physical timer */
ASM_FUNC(arm64_read_cntpct)
    mrs x0, cntpct_el0
    ret

ASM_FUNC(arm64_read_cntpctl)
    mrs x0,  cntp_ctl_el0
    ret
    
ASM_FUNC(arm64_write_cntpctl)
    msr cntp_ctl_el0, x0
    ret

ASM_FUNC(arm64_read_cntpcval)
    mrs x0, cntp_cval_el0
    ret

ASM_FUNC(arm64_write_cntpcval)
    msr cntp_cval_el0, x0
    ret

ASM_FUNC(arm64_read_cntptval)
    mrs x0, cntp_tval_el0
    ret

ASM_FUNC(arm64_write_cntptval)
    msr cntp_tval_el0, x0
    ret

/* Virtual timer: kShimC's timer at EL1 (the EL1 physical timer may belong
 * to the hypervisor). */
ASM_FUNC(arm64_read_cntvct)
    isb
    mrs x0, cntvct_el0
    ret

ASM_FUNC(arm64_read_cntvctl)
    mrs x0, cntv_ctl_el0
    ret

ASM_FUNC(arm64_write_cntvctl)
    msr cntv_ctl_el0, x0
    isb
    ret

ASM_FUNC(arm64_read_cntvcval)
    mrs x0, cntv_cval_el0
    ret

ASM_FUNC(arm64_write_cntvcval)
    msr cntv_cval_el0, x0
    ret

/* EL2 physical timer: kShimC's timer at EL2. */
ASM_FUNC(arm64_read_cnthpctl)
    mrs x0, cnthp_ctl_el2
    ret

ASM_FUNC(arm64_write_cnthpctl)
    msr cnthp_ctl_el2, x0
    isb
    ret

ASM_FUNC(arm64_read_cnthpcval)
    mrs x0, cnthp_cval_el2
    ret

ASM_FUNC(arm64_write_cnthpcval)
    msr cnthp_cval_el2, x0
    ret
