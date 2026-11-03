/* SPDX-License-Identifier: MIT */
#pragma once
#include <stdint.h>
#include <arm64_sysreg.h>
/* delay(), get_current_time() and kshim_wait_event() are public. */
#include <kshim/time.h>

extern uint64_t arm64_read_cntfrq(void);
extern uint64_t arm64_read_cntpct(void);
extern uint64_t arm64_read_cntpctl(void);
extern uint64_t arm64_read_cntpcval(void);
extern uint64_t arm64_read_cntptval(void);
extern void arm64_write_cntpctl(uint64_t cntp_ctlr);
extern void arm64_write_cntpcval(uint64_t cntp_cval);
extern void arm64_write_cntptval(uint64_t cntp_ctlr);
extern uint64_t arm64_read_cntvct(void);
extern uint64_t arm64_read_cntvctl(void);
extern void arm64_write_cntvctl(uint64_t ctl);
extern uint64_t arm64_read_cntvcval(void);
extern void arm64_write_cntvcval(uint64_t cval);
extern uint64_t arm64_read_cnthpctl(void);
extern void arm64_write_cnthpctl(uint64_t ctl);
extern uint64_t arm64_read_cnthpcval(void);
extern void arm64_write_cnthpcval(uint64_t cval);

/* arm64_arch_timer_init()/teardown() take the PPIs from the runtime DT;
 * see gicv3.h. */
