/* SPDX-License-Identifier: MIT */
/*
 * QEMU interrupt self-test. The GIC snapshot here deliberately does not use
 * gicv3.c: it reads every distributor/redistributor register and the boot
 * CPU's interface, timer and vector state independently, before kShimC
 * touches anything and again after the handoff teardown.
 */
#include <arm64_exception.h>
#include <irq_selftest.h>
#include <config.h>

#if CONFIG_KSHIM_IRQ_SELFTEST
#include <gicv3.h>
#include <timer.h>
#include <smp.h>
#include <platform.h>
#include <kshim_console.h>
#include <kshim/irq.h>
#include <kshim/time.h>
#include <memops.h>
#include <libfdt.h>

#include <stdio.h>
#include <string.h>

#define SNAPSHOT_MAX 2048U
#define SEED_SPI_FIRST 100U
#define SEED_SPI_COUNT 4U
#define SEED_PPI 21U
#define SEED_SGI 3U
#define SGI_ROUNDS 4U
#define RX_EXPECTED "kshim\r"
#define RX_TIMEOUT_MS 30000U

#define PL011_DR 0x00U
#define PL011_FR 0x18U
#define PL011_IMSC 0x38U
#define PL011_ICR 0x44U
#define PL011_RX_IRQS (BIT(4) | BIT(6))

/* Keys: MMIO address, or a tag for system registers. */
#define KEY_SYSREG(n) (UINT64_C(0xFFFF000000000000) | (n))

typedef struct {
    unsigned count;
    uint64_t key[SNAPSHOT_MAX];
    uint32_t value[SNAPSHOT_MAX];
} snapshot_t;

static snapshot_t mPre, mPost;
static struct kshim_gic_desc mGicDesc;
static int mSeeded;
static int mPreSnapshot;
static volatile uint64_t mRxIrqs;

static uint64_t sysreg_hcr(void) { uint64_t v; __asm__ volatile("mrs %0, hcr_el2" : "=r"(v)); return v; }
static uint64_t sysreg_vbar(void)
{
    uint64_t v;
    /* At EL1 kshim_main installs its vectors before this self-test runs, so
     * the "before kShimC touched anything" snapshot takes the VBAR it found
     * at entry. The post-handoff snapshot reads the live register. */
    if (arm64_current_el() == 2) __asm__ volatile("mrs %0, vbar_el2" : "=r"(v));
    else if (mPreSnapshot) v = arm64_exception_inherited_vbar();
    else __asm__ volatile("mrs %0, vbar_el1" : "=r"(v));
    return v;
}

static void add(snapshot_t *s, uint64_t key, uint32_t value)
{
    if (s->count < SNAPSHOT_MAX) {
        s->key[s->count] = key;
        s->value[s->count++] = value;
    }
}

static void add32(snapshot_t *s, uintptr_t address)
{
    add(s, address, REG32_OUT(address));
}

static void add64(snapshot_t *s, uint64_t tag, uint64_t value)
{
    add(s, KEY_SYSREG(tag * 2), (uint32_t)value);
    add(s, KEY_SYSREG(tag * 2 + 1), (uint32_t)(value >> 32));
}

static uint32_t gic_lines(uintptr_t dist)
{
    uint32_t lines = ((REG32_OUT(dist + ARM_GICD_TYPER) & 0x1FU) + 1U) * 32U;
    return lines > 1020U ? 1020U : lines;
}

/* Visit every implemented redistributor frame. */
static unsigned frames(uintptr_t *out, unsigned capacity)
{
    unsigned count = 0;
    for (unsigned r = 0; r < mGicDesc.redist_count; ++r) {
        uint32_t arch = (REG32_OUT((uintptr_t)mGicDesc.redist[r].base + ARM_GIC_PIDR2) >> 4) & 0xFU;
        if (arch != 3U && arch != 4U) continue;
        for (uint64_t off = 0; off + 0x20000 <= mGicDesc.redist[r].size && count < capacity;) {
            uintptr_t rd = (uintptr_t)(mGicDesc.redist[r].base + off);
            uint64_t typer = REG64_OUT(rd + ARM_GICR_TYPER);
            out[count++] = rd;
            if (typer & BIT_ARM_GICR_TYPER_LAST) break;
            off += mGicDesc.redist_stride ? mGicDesc.redist_stride : (typer & 2U ? 0x40000U : 0x20000U);
        }
    }
    return count;
}

static uintptr_t frame_of(uint64_t mpidr)
{
    uintptr_t list[GICV3_MAX_CPUS * 4];
    unsigned count = frames(list, GICV3_MAX_CPUS * 4);
    for (unsigned i = 0; i < count; ++i)
        if ((uint32_t)(REG64_OUT(list[i] + ARM_GICR_TYPER) >> 32) == MPIDR_TO_AFF(mpidr)) return list[i];
    return 0;
}

static void snapshot(snapshot_t *s)
{
    uintptr_t dist = (uintptr_t)mGicDesc.dist.base, list[GICV3_MAX_CPUS * 4];
    uint32_t lines = gic_lines(dist), words = (lines + 31U) / 32U;
    uint32_t el = (uint32_t)arm64_current_el();

    s->count = 0;
    add(s, dist + ARM_GICD_CTLR, REG32_OUT(dist + ARM_GICD_CTLR) & ~(uint32_t)BIT_ARM_GICD_CTLR_RWP);
    for (uint32_t w = 1; w < words; ++w) {
        static const uint32_t regs[] = {ARM_GICD_IGROUPR, ARM_GICD_ISENABLER, ARM_GICD_ISPENDR,
                                        ARM_GICD_ISACTIVER, ARM_GICD_IGRPMODR};
        for (unsigned r = 0; r < sizeof(regs) / sizeof(regs[0]); ++r)
            add32(s, dist + regs[r] + w * 4U);
    }
    for (uint32_t i = 8; i < lines / 4U; ++i) add32(s, dist + ARM_GICD_IPRIORITYR + i * 4U);
    for (uint32_t i = 2; i < lines / 16U; ++i) add32(s, dist + ARM_GICD_ICFGR + i * 4U);
    for (uint32_t n = 32; n < lines; ++n) {
        add32(s, dist + 0x6000U + n * 8U);
        add32(s, dist + 0x6000U + n * 8U + 4U);
    }
    unsigned count = frames(list, GICV3_MAX_CPUS * 4);
    for (unsigned i = 0; i < count; ++i) {
        uintptr_t rd = list[i], sgi = rd + 0x10000U;
        add(s, rd + ARM_GICR_CTLR, REG32_OUT(rd + ARM_GICR_CTLR) & ~(uint32_t)BIT_ARM_GICR_CTLR_RWP);
        add32(s, rd + ARM_GICR_WAKER);
        static const uint32_t regs[] = {ARM_GICR_IGROUPR, ARM_GICR_ISENABLER, ARM_GICR_ISPENDR,
                                        ARM_GICR_ISACTIVER, ARM_GICR_IGRPMODR, ARM_GICR_ICFGR,
                                        ARM_GICR_ICFGR + 4U};
        for (unsigned r = 0; r < sizeof(regs) / sizeof(regs[0]); ++r) add32(s, sgi + regs[r]);
        for (unsigned p = 0; p < 8; ++p) add32(s, sgi + ARM_GICR_IPRIORITYR + p * 4U);
    }
    /* Boot CPU interface, vectors, IRQ routing and timer */
    uint64_t sre = el == 2 ? arm64_gicv3_get_sre_el2() : arm64_gicv3_get_sre_el1();
    add64(s, 1, sre);
    if (sre & 1U) {
        add64(s, 2, arm64_gicv3_get_ctlr());
        add64(s, 3, arm64_gicv3_get_pmr());
        add64(s, 4, arm64_gicv3_get_bpr());
        add64(s, 5, arm64_gicv3_get_igrpen1());
    }
    add64(s, 6, sysreg_vbar());
    if (el == 2) {
        add64(s, 7, sysreg_hcr() & BIT(4));
        add64(s, 8, arm64_read_cnthpctl());
        add64(s, 9, arm64_read_cnthpcval());
    } else {
        add64(s, 8, arm64_read_cntvctl());
        add64(s, 9, arm64_read_cntvcval());
    }
}

static uint32_t hash(const snapshot_t *s)
{
    uint32_t h = 2166136261U;
    for (unsigned i = 0; i < s->count; ++i) {
        uint64_t k = s->key[i];
        for (unsigned b = 0; b < 8; ++b) h = (h ^ (uint8_t)(k >> (b * 8))) * 16777619U;
        for (unsigned b = 0; b < 4; ++b) h = (h ^ (uint8_t)(s->value[i] >> (b * 8))) * 16777619U;
    }
    return h;
}

static void wait_rwp(uintptr_t reg, uint32_t bit)
{
    for (unsigned i = 0; i < 1000000U && (REG32_OUT(reg) & bit); ++i) {
    }
}

static void set_byte(uintptr_t reg_base, uint32_t index, uint8_t value)
{
    uintptr_t reg = reg_base + (index / 4U) * 4U;
    uint32_t shift = (index % 4U) * 8U;
    REG32_IN(reg, (REG32_OUT(reg) & ~(0xFFU << shift)) | ((uint32_t)value << shift));
}

void kshim_irq_selftest_inherit(const void *fdt)
{
    if (!fdt || kshim_dt_gic(fdt, &mGicDesc)) {
        printf("IRQ_SELFTEST inherit: no GICv3 in the DT\n");
        return;
    }
    uintptr_t dist = (uintptr_t)mGicDesc.dist.base;
    uint64_t mpidr = arm64_read_mpidr() & ARM_MPIDR_AFFINITY_MASK;
    uintptr_t rd = frame_of(mpidr);
    uint32_t el = (uint32_t)arm64_current_el();
    if (!rd || gic_lines(dist) < 32U + SEED_SPI_FIRST + SEED_SPI_COUNT) {
        printf("IRQ_SELFTEST inherit: unexpected GIC layout\n");
        return;
    }

    /* A distributor left running by a UEFI-like bootloader: affinity routing,
     * both groups on, a few unrelated SPIs configured, routed and enabled. */
    REG32_IN(dist + ARM_GICD_CTLR, REG32_OUT(dist + ARM_GICD_CTLR) | 0x10U);
    wait_rwp(dist + ARM_GICD_CTLR, (uint32_t)BIT_ARM_GICD_CTLR_RWP);
    REG32_IN(dist + ARM_GICD_CTLR, REG32_OUT(dist + ARM_GICD_CTLR) | 0x13U);
    wait_rwp(dist + ARM_GICD_CTLR, (uint32_t)BIT_ARM_GICD_CTLR_RWP);
    for (uint32_t i = 0; i < SEED_SPI_COUNT; ++i) {
        uint32_t intid = 32U + SEED_SPI_FIRST + i;
        uintptr_t word = (intid / 32U) * 4U;
        REG32_IN(dist + ARM_GICD_IGROUPR + word, REG32_OUT(dist + ARM_GICD_IGROUPR + word) | (1U << (intid % 32U)));
        set_byte(dist + ARM_GICD_IPRIORITYR, intid, (uint8_t)(0x40U + i * 0x10U));
        if (i & 1U) {
            uintptr_t cfg = dist + ARM_GICD_ICFGR + (intid / 16U) * 4U;
            REG32_IN(cfg, REG32_OUT(cfg) | (2U << ((intid % 16U) * 2U)));
        }
        REG64_IN(dist + 0x6000U + intid * 8U, i == 3U ? (UINT64_C(1) << 31) : mpidr);
        REG32_IN(dist + ARM_GICD_ISENABLER + word, 1U << (intid % 32U));
    }
    /* The boot CPU's redistributor awake, an unused PPI and SGI enabled. */
    REG32_IN(rd + ARM_GICR_WAKER, REG32_OUT(rd + ARM_GICR_WAKER) & ~(uint32_t)BIT_ARM_GICR_WAKER_PROCESSOR_SLEEP);
    wait_rwp(rd + ARM_GICR_WAKER, (uint32_t)BIT_ARM_GICR_WAKER_CHILDREN_ASLEEP);
    uintptr_t sgi = rd + 0x10000U;
    REG32_IN(sgi + ARM_GICR_IGROUPR, REG32_OUT(sgi + ARM_GICR_IGROUPR) | (1U << SEED_PPI) | (1U << SEED_SGI));
    set_byte(sgi + ARM_GICR_IPRIORITYR, SEED_PPI, 0x50);
    set_byte(sgi + ARM_GICR_IPRIORITYR, SEED_SGI, 0x60);
    REG32_IN(sgi + ARM_GICR_ISENABLER, (1U << SEED_PPI) | (1U << SEED_SGI));
    /* CPU interface as Linux at EL2 would leave it: split EOI, Group 1 on. */
    if (el == 2) arm64_gicv3_set_sre_el2(arm64_gicv3_get_sre_el2() | 9U);
    else arm64_gicv3_set_sre_el1(arm64_gicv3_get_sre_el1() | 1U);
    arm64_gicv3_set_pmr(0xE0);
    arm64_gicv3_set_bpr(3);
    arm64_gicv3_set_ctlr(arm64_gicv3_get_ctlr() | BIT_ICC_CTLR_EOIMODE);
    arm64_gicv3_set_igrpen1(1);
    /* A timer enabled but masked, one hour away. */
    uint64_t hour = arm64_read_cntfrq() * 3600U;
    if (el == 2) {
        arm64_write_cnthpcval(arm64_read_cntpct() + hour);
        arm64_write_cnthpctl(CNT_CTL_ENABLE | CNT_CTL_IMASK);
    } else {
        arm64_write_cntvcval(arm64_read_cntvct() + hour);
        arm64_write_cntvctl(CNT_CTL_ENABLE | CNT_CTL_IMASK);
    }
    mPreSnapshot = 1;
    snapshot(&mPre);
    mPreSnapshot = 0;
    mSeeded = 1;
    printf("IRQ_SELFTEST inherit: el=%u words=%u hash=%08x gicd_ctlr=%#x waker=%#x\n",
           (unsigned)el, mPre.count, hash(&mPre), (unsigned)REG32_OUT(dist + ARM_GICD_CTLR),
           (unsigned)REG32_OUT(rd + ARM_GICR_WAKER));
}

static int noop_task(void *context)
{
    (void)context;
    return 0;
}

static void rx_interrupt(uint32_t intid, void *context)
{
    (void)intid;
    /* Mask until the wait loop has drained the FIFO; the line drops. */
    REG32_IN((uintptr_t)context + PL011_IMSC, 0);
    mRxIrqs++;
}

static int test_timer(void)
{
    GICV3_CPU *boot = gicv3_this_cpu();
    uint64_t frequency = arm64_read_cntfrq();
    uint64_t irqs = arm64_arch_timer_irqs(), wfi = boot->wfi, start = arm64_read_cntpct();
    (void)delay(200000);
    uint64_t delay_ms = (arm64_read_cntpct() - start) * 1000U / frequency;
    uint64_t delay_irqs = arm64_arch_timer_irqs() - irqs, delay_wfi = boot->wfi - wfi;
    irqs = arm64_arch_timer_irqs(); wfi = boot->wfi;
    uint64_t begin = get_current_time();
    kshim_wait_event(begin + 150U);
    uint64_t wait_ms = get_current_time() - begin;
    uint64_t wait_irqs = arm64_arch_timer_irqs() - irqs, wait_wfi = boot->wfi - wfi;
    printf("IRQ_SELFTEST timer: intid=%u el=%u delay_ms=%llu timer_irqs=%llu wfi=%llu "
           "wait_ms=%llu wait_timer_irqs=%llu wait_wfi=%llu\n",
           (unsigned)arm64_arch_timer_intid(), (unsigned)arm64_current_el(),
           (unsigned long long)delay_ms, (unsigned long long)delay_irqs, (unsigned long long)delay_wfi,
           (unsigned long long)wait_ms, (unsigned long long)wait_irqs, (unsigned long long)wait_wfi);
    if (delay_ms < 200U || delay_ms > 1000U || !delay_irqs || !delay_wfi || delay_wfi > 10U ||
        wait_ms < 150U || wait_ms > 1000U || !wait_irqs || !wait_wfi || wait_wfi > 10U)
        return -10;
    return 0;
}

static int test_sgi(const void *fdt)
{
    GICV3_CPU *boot = gicv3_this_cpu();
    uint64_t own = boot->wake_sgis;
    (void)kshim_irq_send_sgi(KSHIM_SGI_WAKE, arm64_read_mpidr() & ARM_MPIDR_AFFINITY_MASK);
    (void)delay(1000);
    int failure = boot->wake_sgis == own ? -20 : 0;

    kshim_platform_accelerate(fdt);
    size_t workers = kshim_smp_available_count();
    uint64_t wakes[KSHIM_SMP_MAX_CPUS] = {0}, wfi[KSHIM_SMP_MAX_CPUS] = {0};
    GICV3_CPU *cpus[KSHIM_SMP_MAX_CPUS] = {0};
    for (size_t w = 0; w < workers; ++w) {
        uint64_t mpidr = kshim_smp_cpu_mpidr(w + 1U);
        for (size_t i = 0; i < GICV3_MAX_CPUS; ++i)
            if (gicv3_cpu(i)->mpidr == mpidr && gicv3_cpu(i)->state == GICV3_CPU_UP) cpus[w] = gicv3_cpu(i);
        if (!cpus[w]) { failure = -21; continue; }
        wakes[w] = cpus[w]->wake_sgis;
        wfi[w] = cpus[w]->wfi;
    }
    for (unsigned round = 0; round < SGI_ROUNDS && !failure; ++round) {
        kshim_smp_ticket_t tickets[KSHIM_SMP_MAX_CPUS];
        (void)delay(5000); /* let every worker go back to sleep */
        for (size_t w = 0; w < workers; ++w)
            if (kshim_smp_submit(w, noop_task, NULL, &tickets[w])) failure = -22;
        for (size_t w = 0; w < workers && !failure; ++w)
            if (kshim_smp_join(w, tickets[w], KSHIM_SMP_DEFAULT_TIMEOUT_US, NULL)) failure = -23;
    }
    printf("IRQ_SELFTEST sgi: self=%llu workers=%u", (unsigned long long)(boot->wake_sgis - own),
           (unsigned)workers);
    for (size_t w = 0; w < workers; ++w) {
        uint64_t got = cpus[w] ? cpus[w]->wake_sgis - wakes[w] : 0, slept = cpus[w] ? cpus[w]->wfi - wfi[w] : 0;
        printf(" cpu%u=%llu/%llu", (unsigned)(w + 1U), (unsigned long long)got, (unsigned long long)slept);
        if (!got || !slept) failure = failure ? failure : -24;
    }
    printf(" (wake SGIs/WFI)\n");
    return failure;
}

static int test_rx(const void *fdt)
{
    uintptr_t base = (uintptr_t)kshim_console_base();
    int node = -1;
    for (int n = fdt_node_offset_by_compatible(fdt, -1, "arm,pl011"); n >= 0;
         n = fdt_node_offset_by_compatible(fdt, n, "arm,pl011")) {
        struct dt_range reg;
        if (!dt_reg(fdt, n, 0, &reg) && reg.base == base) node = n;
    }
    uint32_t intid = 0;
    unsigned trigger = 0;
    if (node < 0 || kshim_irq_from_fdt(fdt, node, 0, &intid, &trigger)) {
        printf("IRQ_SELFTEST rx: console PL011 interrupt not found\n");
        return -30;
    }
    char c, data[16] = {0};
    unsigned count = 0, wakes = 0;
    while (kshim_console_try_read(&c)) {
    }
    int status = kshim_irq_request(intid, trigger, rx_interrupt, (void *)base);
    if (status) {
        printf("IRQ_SELFTEST rx: request %u failed (%d)\n", (unsigned)intid, status);
        return -31;
    }
    REG32_IN(base + PL011_IMSC, PL011_RX_IRQS);
    printf("IRQ_SELFTEST rx: ready intid=%u trigger=%u\n", (unsigned)intid, trigger);
    uint64_t deadline = get_current_time() + RX_TIMEOUT_MS;
    while (count < sizeof(RX_EXPECTED) - 1U && get_current_time() < deadline) {
        /* No polling cap: only the receive interrupt or the deadline wakes. */
        kshim_wait_event(deadline);
        ++wakes;
        while (count < sizeof(data) - 1U && kshim_console_try_read(&c)) data[count++] = c;
        REG32_IN(base + PL011_IMSC, PL011_RX_IRQS);
    }
    REG32_IN(base + PL011_IMSC, 0);
    REG32_IN(base + PL011_ICR, 0x7FFU);
    (void)kshim_irq_free(intid);
    for (unsigned i = 0; i < count; ++i)
        if (data[i] == '\r') data[i] = '|';
    printf("IRQ_SELFTEST rx: intid=%u irqs=%llu wakes=%u chars=%u data=%s\n", (unsigned)intid,
           (unsigned long long)mRxIrqs, wakes, count, data);
    if (intid != 33U || !mRxIrqs || count != sizeof(RX_EXPECTED) - 1U || memcmp(data, "kshim|", count))
        return -32;
    return 0;
}

int kshim_irq_selftest(const void *fdt)
{
    int failure = 0, status;

    if (!mSeeded) return -1;
    if (!kshim_irq_ready()) {
        printf("IRQ_SELFTEST: interrupts are not up\n");
        return -2;
    }
    status = test_timer();
    failure = status;
    status = test_sgi(fdt);
    if (!failure) failure = status;
    status = test_rx(fdt);
    if (!failure) failure = status;

    /* The payload handoff path: workers restore their CPU interface, the
     * boot CPU restores the GIC, timer and vectors, the MMU goes off. */
    status = kshim_platform_prepare_handoff();
    uint64_t total = 0;
    for (size_t i = 0; i < GICV3_MAX_CPUS; ++i) total += gicv3_cpu(i)->irqs;
    snapshot(&mPost);
    unsigned mismatches = mPre.count == mPost.count ? 0 : 1;
    for (unsigned i = 0; i < mPre.count && i < mPost.count; ++i) {
        if (mPre.key[i] == mPost.key[i] && mPre.value[i] == mPost.value[i]) continue;
        if (++mismatches <= 16)
            printf("IRQ_SELFTEST gic-state: %#llx pre=%#x post=%#x\n", (unsigned long long)mPre.key[i],
                   (unsigned)mPre.value[i], (unsigned)mPost.value[i]);
    }
    printf("IRQ_SELFTEST gic-state: handoff=%d words=%u pre=%08x post=%08x irqs_total=%llu "
           "daif_i=%u ready=%u %s\n", status, mPost.count, hash(&mPre), hash(&mPost),
           (unsigned long long)total, (unsigned)arm64_irqs_masked(), (unsigned)kshim_irq_ready(),
           mismatches ? "MISMATCH" : "MATCH");
    if (!failure && (status || mismatches || !arm64_irqs_masked() || kshim_irq_ready()))
        failure = -40;
    return failure;
}

#else
void kshim_irq_selftest_inherit(const void *fdt) { (void)fdt; }
int kshim_irq_selftest(const void *fdt) { (void)fdt; return -1; }
#endif
