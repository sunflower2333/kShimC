/* SPDX-License-Identifier: MIT */
#pragma once
#include <stdint.h>
#include <arm64_sysreg.h>
/* delay() and get_current_time() are public. */
#include <kshim/time.h>

extern uint64_t arm64_read_cntfrq(void);
extern uint64_t arm64_read_cntpct(void);
extern uint64_t arm64_read_cntpctl(void);
extern uint64_t arm64_read_cntpcval(void);
extern uint64_t arm64_read_cntptval(void);
extern void arm64_write_cntpctl(uint64_t cntp_ctlr);
extern void arm64_write_cntpcval(uint64_t cntp_cval);
extern void arm64_write_cntptval(uint64_t cntp_ctlr);

void arm64_arch_timer_init(
    // PPI numbers
    uint8_t arch_timer_virt_irq_no,
    uint8_t arch_timer_hyp_irq_no,
    uint8_t arch_timer_nonsec_irq_no,
    uint8_t arch_timer_sec_irq_no
);
