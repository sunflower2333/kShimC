/* SPDX-License-Identifier: MIT */
#pragma once

#include <kshim.h>
#include <stdbool.h>
#include <arm64_exception.h>
#include <kshim_irq_dt.h>
#include <kshim/irq.h>
/** Macros **/
/* GICD Registers */
#define ARM_GICD_CTLR 0x0
#define ARM_GICD_TYPER 0x4
#define ARM_GICD_IIDR 0x8

#define ARM_GICD_IGROUPR 0x80
#define ARM_GICD_ISENABLER 0x100
#define ARM_GICD_ICENABLER 0x180
#define ARM_GICD_ISPENDR 0x200
#define ARM_GICD_ICPENDR 0x280
#define ARM_GICD_ISACTIVER 0x300
#define ARM_GICD_ICACTIVER 0x380

#define ARM_GICD_IPRIORITYR 0x400

#define ARM_GICD_ITARGETSR 0x800
#define ARM_GICD_ICFGR 0xC00
#define ARM_GICD_IGRPMODR 0xD00
/* IROUTER<n> is at 0x6000 + 8n; n starts at 32, hence 0x6100 for SPI 0. */
#define ARM_GICD_IROUTER 0x6100
#define ARM_GIC_PIDR2 0xFFE8

/* GICR RD_base frame */
#define ARM_GICR_CTLR 0x0
#define ARM_GICR_TYPER 0x8
#define ARM_GICR_WAKER 0x14
/* GICR SGI_base frame, used with + ARM_GICR_CTLR_FRAME_SIZE */
#define ARM_GICR_IGROUPR 0x80
#define ARM_GICR_ISENABLER 0x100
#define ARM_GICR_ICENABLER 0x180
#define ARM_GICR_ISPENDR 0x200
#define ARM_GICR_ICPENDR 0x280
#define ARM_GICR_ISACTIVER 0x300
#define ARM_GICR_ICACTIVER 0x380
#define ARM_GICR_IPRIORITYR 0x400
#define ARM_GICR_ICFGR 0xC00
#define ARM_GICR_IGRPMODR 0xD00

// GICD CTLR. In the Non-secure view with DS == 0, bit 0 is EnableGrp1 and
// bit 1 EnableGrp1A; writing both (as Linux does) works in either view.
#define BIT_ARM_GICD_CTLR_ENABLE_GRP0   BIT(0)
#define BIT_ARM_GICD_CTLR_ENABLE_GRP1NS BIT(1)
#define BIT_ARM_GICD_CTLR_ENABLE_GRP1S       BIT(2)
#define BIT_ARM_GICD_CTLR_ARES          BIT(4)
#define BIT_ARM_GICD_CTLR_ARE_NS          BIT(4)
#define BIT_ARM_GICD_CTLR_DS            BIT(6)
#define BIT_ARM_GICD_CTLR_RWP           BIT(31)

// GICD TYPER
#define BIT_ARM_GICD_TYPER_ITLINE_NUM GENBITS(4, 0)

// GICR CTLR
#define BIT_ARM_GICR_CTLR_RWP BIT(3)

// GICR TYPER
#define BIT_ARM_GICR_TYPER_PLPIS BIT(0)
#define BIT_ARM_GICR_TYPER_VLPIS BIT(1)
#define BIT_ARM_GICR_TYPER_DIRECTLPI BIT(3)
#define BIT_ARM_GICR_TYPER_LAST BIT(4)
#define BIT_ARM_GICR_TYPER_AFFINITY GENBITS(63, 32)
#define BIT_ARM_GICR_TYPER_AFFINITY_SHT 32
#define ARM_GIC_INTERRUPT_DEFAULT_PRIORITY 0xA0
#define ARM_GIC_DEFAULT_PMR 0xF0

// GICR WAKER
#define BIT_ARM_GICR_WAKER_PROCESSOR_SLEEP BIT(1)
#define BIT_ARM_GICR_WAKER_CHILDREN_ASLEEP BIT(2)

#define ARM_GICR_CTLR_FRAME_SIZE SIZE_K(64)    // 64KB
#define ARM_GICR_SGI_PPI_FRAME_SIZE SIZE_K(64) // 64KB

// ICC_CTLR_EL1
#define BIT_ICC_CTLR_EOIMODE BIT(1)
#define BIT_ICC_CTLR_RSS BIT(18)
// ICC_SRE_ELx
#define BIT_ICC_SRE_SRE BIT(0)
#define BIT_ICC_SRE_ENABLE BIT(3)

// MPIDR_EL1
#define BIT_ARM_MPIDR_EL1_AFF0 GENBITS(7, 0)
#define BIT_ARM_MPIDR_EL1_AFF0_SHT 0
#define BIT_ARM_MPIDR_EL1_AFF1 GENBITS(15, 8)
#define BIT_ARM_MPIDR_EL1_AFF1_SHT 8
#define BIT_ARM_MPIDR_EL1_AFF2 GENBITS(23, 16)
#define BIT_ARM_MPIDR_EL1_AFF2_SHT 16
#define BIT_ARM_MPIDR_EL1_AFF3 GENBITS(39, 32)
#define BIT_ARM_MPIDR_EL1_AFF3_SHT 32
#define ARM_MPIDR_AFFINITY_MASK (BIT_ARM_MPIDR_EL1_AFF0 | BIT_ARM_MPIDR_EL1_AFF1 | \
                                 BIT_ARM_MPIDR_EL1_AFF2 | BIT_ARM_MPIDR_EL1_AFF3)
/// squash mpidr to the 32-bit Aff3.Aff2.Aff1.Aff0 of GICR_TYPER
#define MPIDR_TO_AFF(mpidr)                                                         \
    ((uint32_t)(((mpidr) & (BIT_ARM_MPIDR_EL1_AFF0 | BIT_ARM_MPIDR_EL1_AFF1 |      \
                            BIT_ARM_MPIDR_EL1_AFF2)) |                             \
                (((mpidr) & BIT_ARM_MPIDR_EL1_AFF3) >> 8)))

extern uint64_t arm64_read_mpidr(void);                     // Read MPIDR_EL1 register
extern uint64_t arm64_gicv3_acknowledge_interrupt(void);    // GICv3 acknowledge interrupt function
extern void arm64_gicv3_set_pmr(uint64_t pmr);              // Set GIC PMR register
extern uint64_t arm64_gicv3_get_pmr(void);
extern void arm64_gicv3_set_bpr(uint64_t bpr);              // Set GIC BPR1 register
extern uint64_t arm64_gicv3_get_bpr(void);
extern void arm64_gicv3_write_eoir1(uint64_t interrupt_no); // GICv3 end of interrupt function
extern void arm64_gicv3_enable_gicc(void);                  // Enable GIC CPU interface
extern void arm64_gicv3_disable_gicc(void);
extern uint64_t arm64_gicv3_get_igrpen1(void);
extern void arm64_gicv3_set_igrpen1(uint64_t value);
extern uint64_t arm64_gicv3_get_ctlr(void);
extern void arm64_gicv3_set_ctlr(uint64_t value);
extern uint64_t arm64_gicv3_get_sre_el1(void);
extern void arm64_gicv3_set_sre_el1(uint64_t value);
extern uint64_t arm64_gicv3_get_sre_el2(void);
extern void arm64_gicv3_set_sre_el2(uint64_t value);
extern void arm64_gicv3_write_sgi1r(uint64_t value);
/** Enums **/
enum INT_TYPE
{
    GIC_IT_SGI,      // Software Generated Interrupt 0-15
    GIC_IT_PPI,      // Private Peripheral Interrupt 16-31
    GIC_IT_SPI,      // Shared Peripheral Interrupt 32-1020
    GIC_IT_LPI,      // Locality Specific Peripheral Interrupt > 8192
    GIC_IT_RESERVED, // Reserved 1020-1023
    GIC_IT_UNK       // Unknown Interrupt Type
};

/** Types **/
#define GICV3_MAX_INTID 1020U
#define GICV3_MAX_CPUS 8U
/* SPIs whose configuration kShimC changed; their original group, priority,
 * trigger and route are saved on first use and restored at handoff. */
#define GICV3_MAX_SAVED_SPIS 64U

typedef struct {
    uint16_t intid;
    uint8_t group, grpmod, priority, config;
    uint64_t router;
} GICV3_SAVED_SPI;

/* Inherited SGI/PPI frame of one redistributor. */
typedef struct {
    uint32_t waker, igroupr, igrpmodr, isenabler, icfgr[2], ipriorityr[8];
} GICV3_SAVED_FRAME;

/* Inherited CPU interface and exception state, restored on that CPU. */
typedef struct {
    uint64_t sre, ctlr, pmr, bpr1, igrpen1, vbar, hcr_imo;
} GICV3_SAVED_CPUIF;

enum { GICV3_CPU_FREE, GICV3_CPU_UP, GICV3_CPU_RESTORED };

typedef struct {
    uint64_t mpidr;          // UINT64_MAX when the slot is free
    uintptr_t rd;            // RD_base of this CPU's redistributor
    uint32_t state;
    uint32_t enabled;        // SGI/PPI bits kShimC enabled
    uint32_t touched;        // SGI/PPI bits kShimC ever configured
    GICV3_SAVED_FRAME frame;
    GICV3_SAVED_CPUIF cpuif;
    /* Statistics for the QEMU self-test. */
    uint64_t irqs, wake_sgis, wfi;
} GICV3_CPU;

typedef struct arm64_gicv3_context
{
    uint64_t gicd_base;                                // GIC Distributor base address
    uint64_t gicd_size;                                // GIC Distributor size
    struct dt_range redist[KSHIM_GIC_MAX_REDIST_REGIONS]; // GIC Redistributor regions
    unsigned redist_count;
    uint64_t gicr_stride;                              // GIC Redistributor stride size (0: from GICR_TYPER)
    uint64_t max_interrupt_num;                        // Maximum number of interrupts supported
    uint32_t el;                                       // 1 or 2
    uint32_t saved_ctlr;                               // inherited GICD_CTLR
    uint32_t saved_enable[GICV3_MAX_INTID / 32 + 1];   // inherited GICD_ISENABLER (SPI words)
    uint32_t spi_saved[GICV3_MAX_INTID / 32 + 1];      // SPIs in saved_spi[]
    uint32_t spi_enabled[GICV3_MAX_INTID / 32 + 1];    // SPIs kShimC enabled
    uint32_t spi_touched[GICV3_MAX_INTID / 32 + 1];    // SPIs kShimC ever enabled
    GICV3_SAVED_SPI saved_spi[GICV3_MAX_SAVED_SPIS];
    unsigned saved_spi_count;
    GICV3_CPU cpus[GICV3_MAX_CPUS];
} ARM64_GIC_V3_CONTEXT, *PARM64_GIC_V3_CONTEXT;

/** Functions **/
/* Boot CPU: save the inherited distributor, redistributor and CPU interface
 * state, then bring up Group 1 NS delivery to this CPU. */
int gicv3_init(const struct kshim_gic_desc *desc);
/* Secondary CPU: save and bring up its redistributor and CPU interface. */
int gicv3_cpu_init(void);
/* Restore everything gicv3_init/gicv3_cpu_init changed: per CPU on that CPU
 * (cpu_teardown), and the distributor plus every remaining frame on the boot
 * CPU (teardown). */
int gicv3_cpu_teardown(void);
int gicv3_teardown(void);

GICV3_CPU *gicv3_this_cpu(void);
GICV3_CPU *gicv3_cpu(size_t index);
uint32_t gicv3_max_intid(void);
/* Program group 1 NS, priority, trigger (NONE keeps it), route to the boot
 * CPU for SPIs, and enable; SGI/PPI apply to the calling CPU. */
int gicv3_configure_interrupt(uint32_t intid, unsigned trigger);
void gicv3_enable_interrupt(uint16_t interrupt_no);
void gicv3_disable_interrupt(uint16_t interrupt_no);
void gicv3_clear_interrupt(uint16_t interrupt_no);
int gicv3_send_sgi(uint32_t sgi, uint64_t mpidr);

void gicv3_irq_handler(
    ARM64_EXCEPTION_CONTEXT *context,
    uint64_t exception_type);

void gicv3_end_of_interrupt(
    uint64_t interrupt_no);

/* Interrupt core internals shared by the GIC, timer and SMP code. */
#define KSHIM_SGI_WAKE 1U
void kshim_irq_dispatch(uint32_t intid);
/* Internal handlers (timer, SGI wake) do not end kshim_wait_event(). */
int kshim_irq_request_internal(uint32_t intid, unsigned trigger, kshim_irq_handler_t handler,
                               void *context);
/* Consume the "device interrupt seen" flag that ends kshim_wait_event(). */
bool kshim_irq_take_event(void);
/* This CPU takes interrupts and PSTATE.I is clear. */
bool kshim_irq_cpu_can_sleep(void);
/* Secondary CPU before PSCI CPU_OFF. */
int kshim_irq_cpu_teardown(void);
uint64_t arm64_current_el(void);
bool arm64_irqs_masked(void);
static inline void arm64_wfi(void) { __asm__ volatile("dsb sy\n wfi" ::: "memory"); }

/* Generic timer (timer.c). */
int arm64_arch_timer_init(const struct kshim_timer_desc *desc);
void arm64_arch_timer_teardown(void);
uint32_t arm64_arch_timer_intid(void);
uint64_t arm64_arch_timer_irqs(void);
