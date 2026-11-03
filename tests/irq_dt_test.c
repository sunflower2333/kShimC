/* SPDX-License-Identifier: MIT */
/* Interrupt devicetree parsing (kernel/dt/irq.c): GICv3 node, timer PPIs,
 * interrupts/interrupts-extended, non-GIC parents, against the real SM8150
 * runtime DTBs and a synthetic fixture. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <kshim_irq_dt.h>
#include <kshim/irq.h>

static void *load(const char *path)
{
    FILE *file = fopen(path, "rb");
    assert(file);
    assert(fseek(file, 0, SEEK_END) == 0);
    long size = ftell(file);
    assert(size > 0 && fseek(file, 0, SEEK_SET) == 0);
    /* Room for in-place edits; 8-byte aligned as dt_validate requires. */
    void *blob = aligned_alloc(8, (size_t)size + 4096 + 8 - ((size_t)size + 4096) % 8);
    assert(blob && fread(blob, 1, (size_t)size, file) == (size_t)size);
    fclose(file);
    assert(dt_validate(blob, (size_t)size) == 0);
    assert(fdt_open_into(blob, blob, (int)size + 4096) == 0);
    return blob;
}

static int path(const void *fdt, const char *p)
{
    int node = fdt_path_offset(fdt, p);
    assert(node >= 0);
    return node;
}

static void check_sm8150(const char *file)
{
    void *fdt = load(file);
    struct kshim_gic_desc gic;
    struct kshim_timer_desc timer;
    uint32_t intid = 0;
    unsigned trigger = 0;

    /* docs/interrupts.md: GICD 0x17a00000+64K, GICR 0x17a60000+1M, stride 0x20000 */
    assert(kshim_dt_gic(fdt, &gic) == 0);
    assert(gic.node == path(fdt, "/soc/interrupt-controller@17a00000"));
    assert(gic.dist.base == 0x17a00000u && gic.dist.size == 0x10000u);
    assert(gic.redist_count == 1);
    assert(gic.redist[0].base == 0x17a60000u && gic.redist[0].size == 0x100000u);
    assert(gic.redist_stride == 0x20000u); /* two-cell u64 form */

    /* PPIs <1 1>,<1 2>,<1 3>,<1 0> (secure, non-secure, virtual, hyp), not
     * QEMU's 29/30/27/26; the timer inherits the PDC as interrupt-parent. */
    assert(kshim_dt_arch_timer(fdt, &timer) == 0);
    assert(timer.intid[KSHIM_TIMER_SEC_PHYS] == 17 && timer.intid[KSHIM_TIMER_PHYS] == 18);
    assert(timer.intid[KSHIM_TIMER_VIRT] == 19 && timer.intid[KSHIM_TIMER_HYP_PHYS] == 16);
    assert(timer.intid[KSHIM_TIMER_HYP_VIRT] == 0);

    /* The GIC's own maintenance PPI: interrupt-parent is the GIC itself. */
    assert(kshim_irq_from_fdt(fdt, gic.node, 0, &intid, &trigger) == 0);
    assert(intid == KSHIM_IRQ_PPI(9) && trigger == KSHIM_IRQ_TYPE_LEVEL_HIGH);
    assert(kshim_irq_from_fdt(fdt, gic.node, 1, &intid, NULL) == ERROR_NOT_FOUND);

    /* SM8150 devices inherit the PDC from / (interrupt-parent = <&pdc>), a
     * secondary controller: the cascade driver resolves those. */
    int gict = path(fdt, "/soc/gict@17a20000");
    assert(kshim_irq_from_fdt(fdt, gict, 0, &intid, &trigger) == ERROR_NOT_SUPPORTED);
    assert(kshim_irq_from_fdt(fdt, gict, 2, &intid, &trigger) == ERROR_NOT_FOUND);
    /* The GENI console and the SPMI arbiter likewise. */
    int console = fdt_node_offset_by_compatible(fdt, -1, "qcom,msm-geni-console");
    assert(console >= 0 && kshim_irq_from_fdt(fdt, console, 0, &intid, NULL) == ERROR_NOT_SUPPORTED);
    int spmi = fdt_node_offset_by_compatible(fdt, -1, "qcom,spmi-pmic-arb");
    assert(spmi >= 0 && kshim_irq_from_fdt(fdt, spmi, 0, &intid, NULL) == ERROR_NOT_SUPPORTED);

    /* interrupts-extended with PDC and SMP2P parents (modem PIL). */
    int mss = path(fdt, "/soc/qcom,mss@4080000");
    assert(kshim_irq_from_fdt(fdt, mss, 0, &intid, NULL) == ERROR_NOT_SUPPORTED);
    assert(kshim_irq_from_fdt(fdt, mss, 1, &intid, NULL) == ERROR_NOT_SUPPORTED);
    assert(kshim_irq_from_fdt(fdt, mss, 64, &intid, NULL) == ERROR_NOT_FOUND);

    /* Bad arguments. */
    assert(kshim_irq_from_fdt(NULL, gic.node, 0, &intid, NULL) == ERROR_INVALID_PARAMETER);
    assert(kshim_irq_from_fdt(fdt, gic.node, -1, &intid, NULL) == ERROR_INVALID_PARAMETER);
    assert(kshim_irq_from_fdt(fdt, gic.node, 0, NULL, NULL) == ERROR_INVALID_PARAMETER);
    free(fdt);
}

static void expect(const void *fdt, const char *p, int index, int status, uint32_t intid,
                   unsigned trigger)
{
    uint32_t got = 0;
    unsigned got_trigger = 0xff;
    int result = kshim_irq_from_fdt(fdt, path(fdt, p), index, &got, &got_trigger);
    if (result != status || (!status && (got != intid || got_trigger != trigger))) {
        fprintf(stderr, "%s[%d]: status %d intid %u trigger %u\n", p, index, result, got, got_trigger);
        abort();
    }
}

static void check_fixture(const char *file)
{
    void *fdt = load(file);
    struct kshim_gic_desc gic;
    struct kshim_timer_desc timer;

    assert(kshim_dt_gic(fdt, &gic) == 0);
    assert(gic.dist.base == 0x8000000u && gic.redist_count == 2);
    assert(gic.redist[0].base == 0x80a0000u && gic.redist[0].size == 0x80000u);
    assert(gic.redist[1].base == 0x9100000u && gic.redist[1].size == 0x40000u);
    assert(gic.redist_stride == 0x40000u); /* one-cell form */

    /* interrupt-names map out of order; PPIs pass through the 3-cell PDC. */
    assert(kshim_dt_arch_timer(fdt, &timer) == 0);
    assert(timer.intid[KSHIM_TIMER_HYP_PHYS] == 26 && timer.intid[KSHIM_TIMER_VIRT] == 27);
    assert(timer.intid[KSHIM_TIMER_PHYS] == 30 && timer.intid[KSHIM_TIMER_SEC_PHYS] == 0);

    /* "interrupts" with the GIC inherited from /, through a simple-bus. */
    expect(fdt, "/soc/uart@1000", 0, 0, 33, KSHIM_IRQ_TYPE_LEVEL_HIGH);
    expect(fdt, "/soc/uart@1000", 1, 0, 23, KSHIM_IRQ_TYPE_EDGE_RISING);
    expect(fdt, "/soc/uart@1000", 2, ERROR_NOT_FOUND, 0, 0);
    /* interrupts-extended: GIC, TLMM (secondary), GIC. */
    expect(fdt, "/soc/dev@2000", 0, 0, 37, KSHIM_IRQ_TYPE_EDGE_RISING);
    expect(fdt, "/soc/dev@2000", 1, ERROR_NOT_SUPPORTED, 0, 0);
    expect(fdt, "/soc/dev@2000", 2, 0, 18, KSHIM_IRQ_TYPE_LEVEL_HIGH);
    expect(fdt, "/soc/dev@2000", 3, ERROR_NOT_FOUND, 0, 0);
    /* Non-GIC parents: the PDC forwarder and the TLMM. */
    expect(fdt, "/soc/through-pdc@3000", 0, ERROR_NOT_SUPPORTED, 0, 0);
    expect(fdt, "/soc/keys", 0, ERROR_NOT_SUPPORTED, 0, 0);
    expect(fdt, "/soc/pinctrl@f000000", 0, 0, 240, KSHIM_IRQ_TYPE_LEVEL_HIGH);
    /* Malformed or unsupported specifiers. */
    expect(fdt, "/soc/bad-cells", 0, ERROR_INVALID_PARAMETER, 0, 0);
    expect(fdt, "/soc/espi", 0, ERROR_NOT_SUPPORTED, 0, 0);
    expect(fdt, "/soc/bad-type", 0, ERROR_INVALID_PARAMETER, 0, 0);
    expect(fdt, "/soc/bad-spi", 0, ERROR_INVALID_PARAMETER, 0, 0);
    expect(fdt, "/soc/bad-ppi", 0, ERROR_INVALID_PARAMETER, 0, 0);
    expect(fdt, "/soc/bad-ext", 0, ERROR_INVALID_PARAMETER, 0, 0);
    expect(fdt, "/soc/bad-phandle", 0, ERROR_INVALID_PARAMETER, 0, 0);
    expect(fdt, "/soc/no-irq", 0, ERROR_NOT_FOUND, 0, 0);

    /* GIC node validation. */
    int node = gic.node;
    assert(fdt_setprop_u32(fdt, node, "redistributor-stride", 0x1000) == 0);
    assert(kshim_dt_gic(fdt, &gic) == ERROR_INVALID_PARAMETER);
    assert(fdt_setprop_u32(fdt, node, "redistributor-stride", 0x20000) == 0);
    assert(kshim_dt_gic(fdt, &gic) == 0 && gic.redist_stride == 0x20000u);
    assert(fdt_setprop_u32(fdt, node, "#redistributor-regions", 3) == 0);
    assert(kshim_dt_gic(fdt, &gic) == ERROR_INVALID_PARAMETER);
    assert(fdt_delprop(fdt, node, "#redistributor-regions") == 0);
    assert(kshim_dt_gic(fdt, &gic) == 0 && gic.redist_count == 1);
    assert(fdt_setprop_string(fdt, node, "status", "disabled") == 0);
    assert(kshim_dt_gic(fdt, &gic) == ERROR_NOT_FOUND && gic.node == -1);
    free(fdt);
}

int main(void)
{
    check_sm8150(FIXTURES "/andromeda-fdt.dtb");
    check_sm8150(FIXTURES "/hdk8150-fdt.dtb");
    check_fixture(GENERATED_FIXTURES "/irq-gic.dtb");
    puts("irq_dt_test: ok");
    return 0;
}
