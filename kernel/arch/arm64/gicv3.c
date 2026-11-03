/* SPDX-License-Identifier: MIT */
#include <asm/arm64_exception.h>
#include <arm64_exception.h>
#include <arm64_sysreg.h>
#include <lib/stdport.h>
#include <gicv3.h>
#include <timer.h>
#include <bitops.h>
#include <memops.h>
#include <error_no.h>
#include <string.h>

/*
 * Acknowledgements:
 *   mu_silicon_arm_tiano/ArmPkg/Drivers/ArmGic/GicV3/ArmGicV3Dxe.c
 *   HI0069H_b_gic_architecture_specification.pdf
 *   linux/drivers/irqchip/gic-v3.c
 *
 * kShimC borrows the GIC from the previous bootloader: everything it changes
 * is saved first and put back by gicv3_teardown()/gicv3_cpu_teardown(), so a
 * payload starts from the inherited state. Interrupts are Group 1 NS, which a
 * hypervisor's vGIC at EL1 and a native GIC at EL2 both deliver as IRQs.
 */

#define HCR_EL2_IMO BIT(4)
/* RWP waits are bounded at 1 s like Linux; WAKER at 10 ms, since a vGIC may
 * not model ChildrenAsleep for a sleeping redistributor. */
#define RWP_TIMEOUT_DIVISOR 1U
#define WAKER_TIMEOUT_DIVISOR 100U

extern void arm64_install_exception_vector_el2(void);

static ARM64_GIC_V3_CONTEXT mGic;
PARM64_GIC_V3_CONTEXT gicv3_driver_context = NULL;

#define GICD(offset) (mGic.gicd_base + (offset))
#define SGI_BASE(rd) ((rd) + ARM_GICR_CTLR_FRAME_SIZE)
#define WORD(intid) ((intid) / 32U)
#define MASK(intid) ((uint32_t)1U << ((intid) % 32U))

/**
 * @brief   Get the maximum number of interrupts supported by the GICv3
 *
 * @param gicd_base
 * @return uint16_t max interrupt number
 */
uint16_t gicv3_get_max_interrupts(
    uint64_t gicd_base)
{
    // Get the number of interrupts
    uint16_t interrupt_num = ((REG32_OUT(gicd_base + ARM_GICD_TYPER) & BIT_ARM_GICD_TYPER_ITLINE_NUM) + 1) * 32;
    return interrupt_num > GICV3_MAX_INTID ? GICV3_MAX_INTID : interrupt_num;
}

/**
 * @brief   Get the type of interrupt
 *
 * @param gicd_base
 * @param interrupt_num
 * @return enum INT_TYPE
 */
enum INT_TYPE gicv3_get_interrupt_type(
    uint16_t interrupt_num)
{
    if (interrupt_num < 16)
    {
        return GIC_IT_SGI;
    }
    else if (interrupt_num < 32)
    {
        return GIC_IT_PPI;
    }
    else if (interrupt_num < 1020)
    {
        return GIC_IT_SPI;
    }
    else if (interrupt_num < 1024)
    {
        return GIC_IT_RESERVED;
    }
    else if (interrupt_num > 8192)
    {
        return GIC_IT_LPI;
    }
    else
    {
        return GIC_IT_UNK;
    }
}

uint64_t arm64_current_el(void)
{
    uint64_t value;
    __asm__ volatile("mrs %0, CurrentEL" : "=r"(value));
    return (value >> 2) & 3U;
}

static uint64_t read_vbar(void)
{
    uint64_t value;
    if (mGic.el == 2) __asm__ volatile("mrs %0, vbar_el2" : "=r"(value));
    else __asm__ volatile("mrs %0, vbar_el1" : "=r"(value));
    return value;
}

static void write_vbar(uint64_t value)
{
    if (mGic.el == 2) __asm__ volatile("msr vbar_el2, %0\nisb" : : "r"(value) : "memory");
    else __asm__ volatile("msr vbar_el1, %0\nisb" : : "r"(value) : "memory");
}

static uint64_t read_hcr(void)
{
    uint64_t value;
    __asm__ volatile("mrs %0, hcr_el2" : "=r"(value));
    return value;
}

static void write_hcr(uint64_t value)
{
    __asm__ volatile("msr hcr_el2, %0\nisb" : : "r"(value) : "memory");
}

/* Bounded poll of an MMIO status field. */
static int wait_reg(uintptr_t reg, uint32_t mask, uint32_t want, uint32_t divisor)
{
    uint64_t start = arm64_read_cntpct();
    uint64_t limit = arm64_read_cntfrq() / divisor + 1U;
    while ((REG32_OUT(reg) & mask) != want)
    {
        if (arm64_read_cntpct() - start > limit)
            return ERROR_TIMEOUT;
    }
    return 0;
}

static int gicd_wait_rwp(void)
{
    return wait_reg(GICD(ARM_GICD_CTLR), (uint32_t)BIT_ARM_GICD_CTLR_RWP, 0, RWP_TIMEOUT_DIVISOR);
}

static int gicr_wait_rwp(uintptr_t rd)
{
    return wait_reg(rd + ARM_GICR_CTLR, (uint32_t)BIT_ARM_GICR_CTLR_RWP, 0, RWP_TIMEOUT_DIVISOR);
}

static bool gic_arch_valid(uintptr_t frame)
{
    uint32_t arch = (REG32_OUT(frame + ARM_GIC_PIDR2) >> 4) & 0xfU;
    return arch == 3U || arch == 4U;
}

/* Walk every redistributor region frame by frame (DT stride, else 2 or 4
 * 64K frames from GICR_TYPER.VLPIS) until GICR_TYPER.Affinity matches. */
uint64_t gicv3_find_gicr_addr(
    uint64_t mpidr)
{
    uint32_t cpu_aff = MPIDR_TO_AFF(mpidr);

    for (unsigned region = 0; region < mGic.redist_count; region++)
    {
        uint64_t base = mGic.redist[region].base;
        uint64_t size = mGic.redist[region].size;

        // As Linux: a region without a GICv3/v4 redistributor is skipped
        if (size < 2 * ARM_GICR_CTLR_FRAME_SIZE || !gic_arch_valid((uintptr_t)base))
            continue;
        for (uint64_t offset = 0; offset + 2 * ARM_GICR_CTLR_FRAME_SIZE <= size;)
        {
            uintptr_t rd = (uintptr_t)(base + offset);
            uint64_t typer = REG64_OUT(rd + ARM_GICR_TYPER);
            if ((uint32_t)(typer >> BIT_ARM_GICR_TYPER_AFFINITY_SHT) == cpu_aff)
                return rd;
            if (typer & BIT_ARM_GICR_TYPER_LAST)
                break;
            offset += mGic.gicr_stride ? mGic.gicr_stride :
                (typer & BIT_ARM_GICR_TYPER_VLPIS ? 4 : 2) * ARM_GICR_CTLR_FRAME_SIZE;
        }
    }
    // Not found, return 0
    return 0;
}

GICV3_CPU *gicv3_cpu(size_t index)
{
    return index < GICV3_MAX_CPUS ? &mGic.cpus[index] : NULL;
}

GICV3_CPU *gicv3_this_cpu(void)
{
    uint64_t mpidr = arm64_read_mpidr() & ARM_MPIDR_AFFINITY_MASK;
    for (size_t index = 0; index < GICV3_MAX_CPUS; index++)
    {
        if (__atomic_load_n(&mGic.cpus[index].mpidr, __ATOMIC_ACQUIRE) == mpidr)
            return &mGic.cpus[index];
    }
    return NULL;
}

uint32_t gicv3_max_intid(void)
{
    return gicv3_driver_context ? (uint32_t)mGic.max_interrupt_num : 0;
}

static GICV3_CPU *claim_cpu(uint64_t mpidr)
{
    GICV3_CPU *cpu = gicv3_this_cpu();
    if (cpu)
        return cpu;
    for (size_t index = 0; index < GICV3_MAX_CPUS; index++)
    {
        uint64_t expected = UINT64_MAX;
        if (__atomic_compare_exchange_n(&mGic.cpus[index].mpidr, &expected, mpidr, 0,
                                        __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
            return &mGic.cpus[index];
    }
    return NULL;
}

/* Redistributor SGI/PPI state */
static void frame_save(GICV3_CPU *cpu)
{
    uintptr_t sgi = SGI_BASE(cpu->rd);
    GICV3_SAVED_FRAME *frame = &cpu->frame;

    frame->waker = REG32_OUT(cpu->rd + ARM_GICR_WAKER);
    frame->igroupr = REG32_OUT(sgi + ARM_GICR_IGROUPR);
    frame->igrpmodr = REG32_OUT(sgi + ARM_GICR_IGRPMODR);
    frame->isenabler = REG32_OUT(sgi + ARM_GICR_ISENABLER);
    for (int i = 0; i < 2; i++)
        frame->icfgr[i] = REG32_OUT(sgi + ARM_GICR_ICFGR + i * 4);
    for (int i = 0; i < 8; i++)
        frame->ipriorityr[i] = REG32_OUT(sgi + ARM_GICR_IPRIORITYR + i * 4);
    cpu->enabled = cpu->touched = 0;
}

static int frame_set_sleep(uintptr_t rd, bool sleep)
{
    uint32_t waker = REG32_OUT(rd + ARM_GICR_WAKER);
    uint32_t bit = (uint32_t)BIT_ARM_GICR_WAKER_PROCESSOR_SLEEP;

    if (((waker & bit) != 0) != sleep)
        REG32_IN(rd + ARM_GICR_WAKER, sleep ? waker | bit : waker & ~bit);
    return wait_reg(rd + ARM_GICR_WAKER, (uint32_t)BIT_ARM_GICR_WAKER_CHILDREN_ASLEEP,
                    sleep ? (uint32_t)BIT_ARM_GICR_WAKER_CHILDREN_ASLEEP : 0, WAKER_TIMEOUT_DIVISOR);
}

static int frame_restore(GICV3_CPU *cpu)
{
    uintptr_t sgi = SGI_BASE(cpu->rd);
    GICV3_SAVED_FRAME *frame = &cpu->frame;
    int status;

    // Disable everything, drop the state kShimC introduced, then put back
    // the inherited configuration and enables.
    REG32_IN(sgi + ARM_GICR_ICENABLER, 0xFFFFFFFFU);
    status = gicr_wait_rwp(cpu->rd);
    REG32_IN(sgi + ARM_GICR_ICPENDR, cpu->touched);
    REG32_IN(sgi + ARM_GICR_ICACTIVER, cpu->touched);
    REG32_IN(sgi + ARM_GICR_IGROUPR, frame->igroupr);
    REG32_IN(sgi + ARM_GICR_IGRPMODR, frame->igrpmodr);
    for (int i = 0; i < 2; i++)
        REG32_IN(sgi + ARM_GICR_ICFGR + i * 4, frame->icfgr[i]);
    for (int i = 0; i < 8; i++)
        REG32_IN(sgi + ARM_GICR_IPRIORITYR + i * 4, frame->ipriorityr[i]);
    REG32_IN(sgi + ARM_GICR_ISENABLER, frame->isenabler);
    if (frame_set_sleep(cpu->rd, (frame->waker & BIT_ARM_GICR_WAKER_PROCESSOR_SLEEP) != 0) && !status)
        status = ERROR_TIMEOUT;
    cpu->enabled = cpu->touched = 0;
    cpu->state = GICV3_CPU_RESTORED;
    return status;
}

/* CPU interface, vectors and IRQ routing of the calling CPU. ICC registers
 * are UNDEFINED while the system register interface is off. */
static bool sre_enabled(void)
{
    uint64_t sre = mGic.el == 2 ? arm64_gicv3_get_sre_el2() : arm64_gicv3_get_sre_el1();
    return (sre & BIT_ICC_SRE_SRE) != 0;
}

static void cpuif_quiesce(void)
{
    if (sre_enabled())
        arm64_gicv3_disable_gicc();
}

static void cpuif_save(GICV3_CPU *cpu)
{
    GICV3_SAVED_CPUIF *cpuif = &cpu->cpuif;

    cpuif->sre = mGic.el == 2 ? arm64_gicv3_get_sre_el2() : arm64_gicv3_get_sre_el1();
    cpuif->vbar = read_vbar();
    cpuif->hcr_imo = mGic.el == 2 ? read_hcr() & HCR_EL2_IMO : 0;
    if (cpuif->sre & BIT_ICC_SRE_SRE)
    {
        cpuif->ctlr = arm64_gicv3_get_ctlr();
        cpuif->pmr = arm64_gicv3_get_pmr();
        cpuif->bpr1 = arm64_gicv3_get_bpr();
        cpuif->igrpen1 = arm64_gicv3_get_igrpen1();
    }
}

static void cpuif_restore(GICV3_CPU *cpu)
{
    GICV3_SAVED_CPUIF *cpuif = &cpu->cpuif;

    cpuif_quiesce();
    if ((cpuif->sre & BIT_ICC_SRE_SRE) && sre_enabled())
    {
        arm64_gicv3_set_ctlr(cpuif->ctlr);
        arm64_gicv3_set_bpr(cpuif->bpr1);
        arm64_gicv3_set_pmr(cpuif->pmr);
        arm64_gicv3_set_igrpen1(cpuif->igrpen1);
    }
    write_vbar(cpuif->vbar);
    // ICC_SRE_EL1 writes can trap to a hypervisor: only when it changed
    if (mGic.el == 2)
    {
        write_hcr((read_hcr() & ~HCR_EL2_IMO) | cpuif->hcr_imo);
        if (arm64_gicv3_get_sre_el2() != cpuif->sre)
            arm64_gicv3_set_sre_el2(cpuif->sre);
    }
    else if (arm64_gicv3_get_sre_el1() != cpuif->sre)
        arm64_gicv3_set_sre_el1(cpuif->sre);
}

static int cpuif_enable(GICV3_CPU *cpu)
{
    uint64_t sre = cpu->cpuif.sre;

    // System register access to the CPU interface. Under a hypervisor it is
    // already on (and the write may trap), so write only when it is off.
    if (!(sre & BIT_ICC_SRE_SRE))
    {
        if (mGic.el == 2)
        {
            arm64_gicv3_set_sre_el2(sre | BIT_ICC_SRE_SRE);
            sre = arm64_gicv3_get_sre_el2();
        }
        else
        {
            arm64_gicv3_set_sre_el1(sre | BIT_ICC_SRE_SRE);
            sre = arm64_gicv3_get_sre_el1();
        }
    }
    if (!(sre & BIT_ICC_SRE_SRE))
        return ERROR_NOT_SUPPORTED;
    if (!(cpu->cpuif.sre & BIT_ICC_SRE_SRE))
    {
        // The inherited interface was memory mapped; nothing to save there.
        cpu->cpuif.ctlr = arm64_gicv3_get_ctlr();
        cpu->cpuif.pmr = arm64_gicv3_get_pmr();
        cpu->cpuif.bpr1 = arm64_gicv3_get_bpr();
        cpu->cpuif.igrpen1 = arm64_gicv3_get_igrpen1();
    }

    // Install Vector table, and route physical IRQs to EL2 when running there
    if (mGic.el == 2)
    {
        arm64_install_exception_vector_el2();
        write_hcr(read_hcr() | HCR_EL2_IMO);
    }
    else
        arm64_install_exception_vector();

    //// One EOI write drops priority and deactivates
    arm64_gicv3_set_ctlr(arm64_gicv3_get_ctlr() & ~BIT_ICC_CTLR_EOIMODE);
    //// Disable interrupt preempt (binary point register)
    arm64_gicv3_set_bpr(0x7);
    //// Allow all used priorities (priority mask register)
    arm64_gicv3_set_pmr(ARM_GIC_DEFAULT_PMR);
    //// Enable Group 1 at the CPU interface
    arm64_gicv3_enable_gicc();
    return 0;
}

/* Lazily save an SPI's inherited configuration before changing it. */
static int save_spi(uint32_t intid)
{
    if (mGic.spi_saved[WORD(intid)] & MASK(intid))
        return 0;
    if (mGic.saved_spi_count == GICV3_MAX_SAVED_SPIS)
        return ERROR_NOT_SUPPORTED;
    GICV3_SAVED_SPI *saved = &mGic.saved_spi[mGic.saved_spi_count++];
    saved->intid = (uint16_t)intid;
    saved->group = (REG32_OUT(GICD(ARM_GICD_IGROUPR) + WORD(intid) * 4) >> (intid % 32)) & 1U;
    saved->grpmod = (REG32_OUT(GICD(ARM_GICD_IGRPMODR) + WORD(intid) * 4) >> (intid % 32)) & 1U;
    saved->priority = (REG32_OUT(GICD(ARM_GICD_IPRIORITYR) + (intid / 4) * 4) >> ((intid % 4) * 8)) & 0xFFU;
    saved->config = (REG32_OUT(GICD(ARM_GICD_ICFGR) + (intid / 16) * 4) >> ((intid % 16) * 2)) & 3U;
    saved->router = REG64_OUT(GICD(ARM_GICD_IROUTER) + (intid - 32) * 8);
    mGic.spi_saved[WORD(intid)] |= MASK(intid);
    return 0;
}

static void set_field(uintptr_t reg, uint32_t mask, uint32_t value)
{
    uint32_t old = REG32_OUT(reg);
    if ((old & mask) != value)
        REG32_IN(reg, (old & ~mask) | value);
}

static void restore_spi(const GICV3_SAVED_SPI *saved)
{
    uint32_t intid = saved->intid;
    uint32_t prio_shift = (intid % 4) * 8, cfg_shift = (intid % 16) * 2;

    set_field(GICD(ARM_GICD_IGROUPR) + WORD(intid) * 4, MASK(intid), saved->group ? MASK(intid) : 0);
    set_field(GICD(ARM_GICD_IGRPMODR) + WORD(intid) * 4, MASK(intid), saved->grpmod ? MASK(intid) : 0);
    set_field(GICD(ARM_GICD_IPRIORITYR) + (intid / 4) * 4, 0xFFU << prio_shift,
              (uint32_t)saved->priority << prio_shift);
    set_field(GICD(ARM_GICD_ICFGR) + (intid / 16) * 4, 3U << cfg_shift,
              (uint32_t)saved->config << cfg_shift);
    REG64_IN(GICD(ARM_GICD_IROUTER) + (intid - 32) * 8, saved->router);
}

static uintptr_t interrupt_base(uint32_t intid, GICV3_CPU **cpu)
{
    if (intid >= 32)
        return (uintptr_t)mGic.gicd_base;
    *cpu = gicv3_this_cpu();
    if (!*cpu || (*cpu)->state != GICV3_CPU_UP)
        return 0;
    return SGI_BASE((*cpu)->rd);
}

int gicv3_configure_interrupt(uint32_t intid, unsigned trigger)
{
    GICV3_CPU *cpu = NULL;
    uintptr_t base;
    uint32_t edge;

    if (!gicv3_driver_context || intid >= mGic.max_interrupt_num)
        return ERROR_INVALID_PARAMETER;
    if (trigger == KSHIM_IRQ_TYPE_LEVEL_HIGH)
        edge = 0;
    else if (trigger == KSHIM_IRQ_TYPE_EDGE_RISING)
        edge = 2;
    else if (trigger == KSHIM_IRQ_TYPE_NONE)
        edge = UINT32_MAX;
    else
        return ERROR_NOT_SUPPORTED;
    base = interrupt_base(intid, &cpu);
    if (!base)
        return ERROR_NOT_READY;
    if (cpu)
        cpu->touched |= MASK(intid);
    else if (save_spi(intid))
        return ERROR_NOT_SUPPORTED;

    // Group 1 Non-secure, default priority
    set_field(base + ARM_GICD_IGROUPR + WORD(intid) * 4, MASK(intid), MASK(intid));
    set_field(base + ARM_GICD_IGRPMODR + WORD(intid) * 4, MASK(intid), 0);
    set_field(base + ARM_GICD_IPRIORITYR + (intid / 4) * 4, 0xFFU << ((intid % 4) * 8),
              (uint32_t)ARM_GIC_INTERRUPT_DEFAULT_PRIORITY << ((intid % 4) * 8));
    // Trigger: SGIs are always edge; some PPIs are fixed by the implementation
    if (edge != UINT32_MAX && intid >= 16)
    {
        uintptr_t reg = base + ARM_GICD_ICFGR + (intid / 16) * 4;
        uint32_t shift = (intid % 16) * 2;
        set_field(reg, 2U << shift, edge << shift);
        if (((REG32_OUT(reg) >> shift) & 2U) != edge && intid >= 32)
            return ERROR_NOT_SUPPORTED;
    }
    //// Route SPIs to the boot CPU
    if (intid >= 32)
        REG64_IN(GICD(ARM_GICD_IROUTER) + (intid - 32) * 8,
                 mGic.cpus[0].mpidr & ARM_MPIDR_AFFINITY_MASK);
    return 0;
}

// Enable interrupt
void gicv3_enable_interrupt(
    uint16_t interrupt_no)
{
    GICV3_CPU *cpu = NULL;
    uintptr_t base = interrupt_base(interrupt_no, &cpu);

    if (!base || interrupt_no >= mGic.max_interrupt_num)
        return;
    // SPI on the distributor, SGI or PPI on this CPU's redistributor
    REG32_IN(base + ARM_GICD_ISENABLER + WORD(interrupt_no) * 4, MASK(interrupt_no));
    if (cpu)
    {
        cpu->enabled |= MASK(interrupt_no);
        cpu->touched |= MASK(interrupt_no);
    }
    else
    {
        mGic.spi_enabled[WORD(interrupt_no)] |= MASK(interrupt_no);
        mGic.spi_touched[WORD(interrupt_no)] |= MASK(interrupt_no);
    }
}

// Disable interrupt
void gicv3_disable_interrupt(
    uint16_t interrupt_no)
{
    GICV3_CPU *cpu = NULL;
    uintptr_t base = interrupt_base(interrupt_no, &cpu);

    if (!base || interrupt_no >= mGic.max_interrupt_num)
        return;
    REG32_IN(base + ARM_GICD_ICENABLER + WORD(interrupt_no) * 4, MASK(interrupt_no));
    if (cpu)
    {
        (void)gicr_wait_rwp(cpu->rd);
        cpu->enabled &= ~MASK(interrupt_no);
    }
    else
    {
        (void)gicd_wait_rwp();
        mGic.spi_enabled[WORD(interrupt_no)] &= ~MASK(interrupt_no);
    }
}

// Drop a pending or active state left by a disabled interrupt
void gicv3_clear_interrupt(
    uint16_t interrupt_no)
{
    GICV3_CPU *cpu = NULL;
    uintptr_t base = interrupt_base(interrupt_no, &cpu);

    if (!base || interrupt_no >= mGic.max_interrupt_num)
        return;
    REG32_IN(base + ARM_GICD_ICPENDR + WORD(interrupt_no) * 4, MASK(interrupt_no));
}

int gicv3_send_sgi(uint32_t sgi, uint64_t mpidr)
{
    uint64_t aff0 = mpidr & 0xFFU;

    if (!gicv3_driver_context || sgi > 15)
        return ERROR_INVALID_PARAMETER;
    // Aff0 >= 16 needs range selector support
    if (aff0 >= 16 && !(arm64_gicv3_get_ctlr() & BIT_ICC_CTLR_RSS))
        return ERROR_NOT_SUPPORTED;
    arm64_gicv3_write_sgi1r(((mpidr >> 32) & 0xFFU) << 48 |   // Aff3
                            ((mpidr >> 16) & 0xFFU) << 32 |   // Aff2
                            (aff0 >> 4) << 44 |               // RS
                            (uint64_t)sgi << 24 |
                            ((mpidr >> 8) & 0xFFU) << 16 |    // Aff1
                            (UINT64_C(1) << (aff0 & 15)));    // TargetList
    return 0;
}

/* Restore the distributor and every redistributor still up. Safe to call
 * with a partially initialised context (init rollback). */
static int restore_all(GICV3_CPU *self)
{
    int status = 0;
    uint32_t words = (uint32_t)(mGic.max_interrupt_num + 31) / 32;

    cpuif_quiesce();
    for (uint32_t w = 1; w < words; w++)
        REG32_IN(GICD(ARM_GICD_ICENABLER) + w * 4, 0xFFFFFFFFU);
    status = gicd_wait_rwp();
    for (uint32_t w = 1; w < words; w++)
    {
        if (!mGic.spi_touched[w])
            continue;
        REG32_IN(GICD(ARM_GICD_ICPENDR) + w * 4, mGic.spi_touched[w]);
        REG32_IN(GICD(ARM_GICD_ICACTIVER) + w * 4, mGic.spi_touched[w]);
    }
    for (unsigned i = 0; i < mGic.saved_spi_count; i++)
        restore_spi(&mGic.saved_spi[i]);
    for (uint32_t w = 1; w < words; w++)
        REG32_IN(GICD(ARM_GICD_ISENABLER) + w * 4, mGic.saved_enable[w]);
    REG32_IN(GICD(ARM_GICD_CTLR), mGic.saved_ctlr & ~(uint32_t)BIT_ARM_GICD_CTLR_RWP);
    if (gicd_wait_rwp() && !status)
        status = ERROR_TIMEOUT;

    // Frames of CPUs that did not restore their own (and this CPU's)
    for (size_t i = 0; i < GICV3_MAX_CPUS; i++)
    {
        GICV3_CPU *cpu = &mGic.cpus[i];
        if (cpu->state == GICV3_CPU_UP && cpu->rd && frame_restore(cpu) && !status)
            status = ERROR_TIMEOUT;
    }
    if (self)
        cpuif_restore(self);
    memset(mGic.spi_saved, 0, sizeof(mGic.spi_saved));
    memset(mGic.spi_enabled, 0, sizeof(mGic.spi_enabled));
    memset(mGic.spi_touched, 0, sizeof(mGic.spi_touched));
    mGic.saved_spi_count = 0;
    return status;
}

int gicv3_init(const struct kshim_gic_desc *desc)
{
    uint64_t mpidr = arm64_read_mpidr() & ARM_MPIDR_AFFINITY_MASK;
    GICV3_CPU *cpu;
    uint32_t words;
    int status;

    if (!desc || !desc->dist.base || !desc->redist_count || desc->redist_count > KSHIM_GIC_MAX_REDIST_REGIONS)
        return ERROR_INVALID_PARAMETER;
    // Initialize driver context
    memset(&mGic, 0, sizeof(mGic));
    for (size_t i = 0; i < GICV3_MAX_CPUS; i++)
        mGic.cpus[i].mpidr = UINT64_MAX;
    mGic.gicd_base = desc->dist.base;
    mGic.gicd_size = desc->dist.size;
    memcpy(mGic.redist, desc->redist, sizeof(mGic.redist));
    mGic.redist_count = desc->redist_count;
    mGic.gicr_stride = desc->redist_stride;
    mGic.el = (uint32_t)arm64_current_el();
    if ((mGic.el != 1 && mGic.el != 2) || !gic_arch_valid((uintptr_t)mGic.gicd_base))
        return ERROR_NOT_SUPPORTED;
    mGic.max_interrupt_num = gicv3_get_max_interrupts(mGic.gicd_base);
    words = (uint32_t)(mGic.max_interrupt_num + 31) / 32;

    cpu = &mGic.cpus[0];
    cpu->mpidr = mpidr;
    cpu->rd = gicv3_find_gicr_addr(mpidr);
    if (!cpu->rd)
    {
        printf("GICR base address not found for this cpu\n");
        cpu->mpidr = UINT64_MAX;
        return ERROR_NOT_FOUND;
    }

    /// 1. Save what the previous bootloader left
    mGic.saved_ctlr = REG32_OUT(GICD(ARM_GICD_CTLR));
    for (uint32_t w = 1; w < words; w++)
        mGic.saved_enable[w] = REG32_OUT(GICD(ARM_GICD_ISENABLER) + w * 4);
    frame_save(cpu);
    cpuif_save(cpu);
    cpu->state = GICV3_CPU_UP;

    /// 2. Quiesce inherited sources: nothing fires on kShimC without a handler
    for (uint32_t w = 1; w < words; w++)
        REG32_IN(GICD(ARM_GICD_ICENABLER) + w * 4, 0xFFFFFFFFU);
    status = gicd_wait_rwp();
    REG32_IN(SGI_BASE(cpu->rd) + ARM_GICR_ICENABLER, 0xFFFFFFFFU);
    if (!status)
        status = gicr_wait_rwp(cpu->rd);

    /// 3. Distributor: affinity routing, Group 1 NS (and Group 0, as Linux)
    uint32_t ctlr = mGic.saved_ctlr & ~(uint32_t)BIT_ARM_GICD_CTLR_RWP;
    if (!status && !(ctlr & BIT_ARM_GICD_CTLR_ARE_NS))
    {
        // ARE may only change while the groups are disabled
        ctlr &= ~(uint32_t)(BIT_ARM_GICD_CTLR_ENABLE_GRP0 | BIT_ARM_GICD_CTLR_ENABLE_GRP1NS |
                            BIT_ARM_GICD_CTLR_ENABLE_GRP1S);
        REG32_IN(GICD(ARM_GICD_CTLR), ctlr);
        status = gicd_wait_rwp();
        REG32_IN(GICD(ARM_GICD_CTLR), ctlr | (uint32_t)BIT_ARM_GICD_CTLR_ARE_NS);
        if (!status)
            status = gicd_wait_rwp();
        if (!status && !(REG32_OUT(GICD(ARM_GICD_CTLR)) & BIT_ARM_GICD_CTLR_ARE_NS))
            status = ERROR_NOT_SUPPORTED;
    }
    if (!status)
    {
        REG32_IN(GICD(ARM_GICD_CTLR), ctlr | (uint32_t)(BIT_ARM_GICD_CTLR_ARE_NS |
                 BIT_ARM_GICD_CTLR_ENABLE_GRP1NS | BIT_ARM_GICD_CTLR_ENABLE_GRP0));
        status = gicd_wait_rwp();
    }

    /// 4. This CPU's redistributor and CPU interface
    if (!status)
        status = frame_set_sleep(cpu->rd, false);
    if (!status)
        status = cpuif_enable(cpu);
    if (status)
    {
        (void)restore_all(cpu);
        memset(&mGic.cpus[0], 0, sizeof(mGic.cpus[0]));
        mGic.cpus[0].mpidr = UINT64_MAX;
        return status;
    }
    gicv3_driver_context = &mGic;
    return 0;
}

int gicv3_cpu_init(void)
{
    uint64_t mpidr = arm64_read_mpidr() & ARM_MPIDR_AFFINITY_MASK;
    GICV3_CPU *cpu;
    int status;

    if (!gicv3_driver_context)
        return ERROR_NOT_READY;
    cpu = claim_cpu(mpidr);
    if (!cpu)
        return ERROR_NOT_SUPPORTED;
    if (cpu->state == GICV3_CPU_UP)
        return 0;
    cpu->rd = gicv3_find_gicr_addr(mpidr);
    if (!cpu->rd)
        return ERROR_NOT_FOUND;
    frame_save(cpu);
    cpuif_save(cpu);
    cpu->state = GICV3_CPU_UP;
    REG32_IN(SGI_BASE(cpu->rd) + ARM_GICR_ICENABLER, 0xFFFFFFFFU);
    status = gicr_wait_rwp(cpu->rd);
    if (!status)
        status = frame_set_sleep(cpu->rd, false);
    if (!status)
        status = cpuif_enable(cpu);
    if (status)
    {
        cpuif_quiesce();
        (void)frame_restore(cpu);
        cpuif_restore(cpu);
    }
    return status;
}

int gicv3_cpu_teardown(void)
{
    GICV3_CPU *cpu = gicv3_this_cpu();
    int status;

    if (!cpu || cpu->state != GICV3_CPU_UP)
        return 0;
    cpuif_quiesce();
    status = frame_restore(cpu);
    cpuif_restore(cpu);
    return status;
}

int gicv3_teardown(void)
{
    int status;

    if (!gicv3_driver_context)
        return 0;
    status = restore_all(gicv3_this_cpu());
    gicv3_driver_context = NULL;
    return status;
}

// Mark interrupt is finished
void gicv3_end_of_interrupt(
    uint64_t interrupt_no)
{
    // End of interrupt (EOImode 0: priority drop and deactivation)
    arm64_gicv3_write_eoir1(interrupt_no);
}

// Get IRQ number and call its handler
void gicv3_irq_handler(
    ARM64_EXCEPTION_CONTEXT *context,
    uint64_t exception_type)
{
    (void)context;
    (void)exception_type;
    // Acknowledge the interrupt
    uint64_t iar = arm64_gicv3_acknowledge_interrupt();
    uint32_t intid = (uint32_t)(iar & 0xFFFFFFU);

    // 1020-1023: spurious or special, nothing to complete
    if (intid >= GICV3_MAX_INTID)
        return;
    GICV3_CPU *cpu = gicv3_this_cpu();
    if (cpu)
        cpu->irqs++;
    kshim_irq_dispatch(intid);
    // The core completes the interrupt after the handler returns
    gicv3_end_of_interrupt(iar);
}
