/* SPDX-License-Identifier: MIT */
/*
 * PCI core, inherit-first assignment over the ECAM register model: what the
 * previous firmware assigned is kept when it is valid (bus numbers, BARs,
 * bridge windows, decode/bus-master bits) and only conflicting, outside or
 * missing resources are assigned; a bridge whose inherited window cannot
 * hold its subtree is reassigned as a whole. Handoff restores the inherited
 * configuration exactly.
 */
#define _GNU_SOURCE
#include "../../kernel/drivers/pci/pci_internal.h"
#include "pci_model.h"

#include <kshim/irq.h>
#include <kshim/smp.h>
#include <kshim/time.h>
#include <dt.h>

#define ECAM_BASE UINT64_C(0x4010000000)
#define ECAM_SIZE UINT64_C(0x1000000)

static uint64_t now_us;
uint64_t kshim_smp_time_us(void) { return now_us; }
uint64_t delay(uint64_t us) { now_us += us; return us; }
bool kshim_irq_ready(void) { return false; }
uint64_t get_current_time(void) { return now_us / 1000; }
void kshim_wait_event(uint64_t deadline_ms) { now_us = deadline_ms * 1000 + 1; }
int kshim_irq_request(uint32_t intid, unsigned trigger, kshim_irq_handler_t handler, void *ctx)
{
    (void)intid; (void)trigger; (void)handler; (void)ctx;
    return 0;
}
int kshim_irq_free(uint32_t intid) { (void)intid; return 0; }
unsigned long kshim_irq_save(void) { return 0; }
void kshim_irq_restore(unsigned long flags) { (void)flags; }

static uint32_t mmio_read(uintptr_t a, unsigned width)
{
    uint64_t off = a - ECAM_BASE;
    assert(a >= ECAM_BASE && off < ECAM_SIZE);
    return model_cfg_read((unsigned)(off >> 20), (uint8_t)(off >> 12), off & 0xfff, width);
}
static void mmio_write(uintptr_t a, unsigned width, uint32_t v)
{
    uint64_t off = a - ECAM_BASE;
    assert(a >= ECAM_BASE && off < ECAM_SIZE);
    model_cfg_write((unsigned)(off >> 20), (uint8_t)(off >> 12), off & 0xfff, width, v);
}
static const struct kshim_pci_test_mmio Mmio = {mmio_read, mmio_write};
const struct kshim_pci_test_mmio *kshim_pci_test_mmio = &Mmio;

static void *load(const char *path)
{
    FILE *f = fopen(path, "rb");
    assert(f);
    static char blob[65536] __attribute__((aligned(8)));
    size_t n = fread(blob, 1, sizeof(blob), f);
    fclose(f);
    assert(n > 0 && dt_validate(blob, n) == 0);
    return blob;
}

static struct kshim_pci_dev *dev_at(uint8_t bus, uint8_t slot)
{
    for (struct kshim_pci_dev *d = kshim_pci_find_device(KSHIM_PCI_ANY_ID, KSHIM_PCI_ANY_ID, NULL);
         d; d = kshim_pci_find_device(KSHIM_PCI_ANY_ID, KSHIM_PCI_ANY_ID, d))
        if (d->bus == bus && d->devfn == KSHIM_PCI_DEVFN(slot, 0)) return d;
    return NULL;
}

/* Writes other than the BAR sizing pattern and its immediate restore. */
static unsigned programmed(int f, uint16_t reg, uint32_t inherited)
{
    unsigned n = 0;
    for (unsigned i = 0; i < ModelLogCount; ++i)
        if (ModelLog[i].fn == f && ModelLog[i].reg == reg && ModelLog[i].value != 0xffffffffU &&
            ModelLog[i].value != inherited) ++n;
    return n;
}

static uint8_t Saved[MODEL_FUNCS][4096];

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    const void *fdt = load(FIXTURES "/pci-ecam.dtb");

    /* Root bus: A kept (decoding, bus master), F overlaps A, G outside. */
    int a = model_add(-1, 1, 0, 0x1b36, 0x000d, 0x0c0330, 0x00, 1);
    model_bar(a, 0, 0x4000, 4);
    model_set(a, 0x10, 4, 0x10200004);
    model_set(a, 0x04, 2, 0x0006);
    int f = model_add(-1, 5, 0, 0x1af4, 0x1000, 0x020000, 0x00, 1);
    model_bar(f, 0, 0x4000, 0);
    model_set(f, 0x10, 4, 0x10200000);
    int g = model_add(-1, 6, 0, 0x1af4, 0x1000, 0x020000, 0x00, 1);
    model_bar(g, 0, 0x1000, 0);
    model_set(g, 0x10, 4, 0x50000000);
    /* Bridge B kept: buses 1-1, window 0x10000000-0x100fffff; C kept, C3 fits. */
    int b = model_add(-1, 2, 0, 0x1b36, 0x0001, 0x060400, 0x01, 1);
    model_set(b, 0x18, 4, 0x00010100);
    model_set(b, 0x20, 4, 0x10001000);
    model_set(b, 0x04, 2, 0x0006);
    int c = model_add(b, 0, 0, 0x1b36, 0x000d, 0x0c0330, 0x00, 1);
    model_bar(c, 0, 0x4000, 4);
    model_set(c, 0x10, 4, 0x10000004);
    int c3 = model_add(b, 1, 0, 0x1af4, 0x1000, 0x020000, 0x00, 1);
    model_bar(c3, 0, 0x1000, 0);
    /* Bridge H: its 1 MiB window cannot also hold H1, so it is reassigned. */
    int h = model_add(-1, 7, 0, 0x1b36, 0x0001, 0x060400, 0x01, 1);
    model_set(h, 0x18, 4, 0x00020200);
    model_set(h, 0x20, 4, 0x10401040);
    int h0 = model_add(h, 0, 0, 0x1af4, 0x1000, 0x020000, 0x00, 1);
    model_bar(h0, 0, 0x100000, 0);
    model_set(h0, 0x10, 4, 0x10400000);
    int h1 = model_add(h, 1, 0, 0x1af4, 0x1000, 0x020000, 0x00, 1);
    model_bar(h1, 0, 0x100000, 0);
    /* Bridge J claims bus 2 as well: renumbered after H. */
    int j = model_add(-1, 8, 0, 0x1b36, 0x0001, 0x060400, 0x01, 1);
    model_set(j, 0x18, 4, 0x00020200);
    int j0 = model_add(j, 0, 0, 0x1af4, 0x1000, 0x020000, 0x00, 1);
    model_bar(j0, 0, 0x1000, 0);
    model_snapshot(Saved);

    assert(!kshim_pci_init(fdt));
    struct kshim_pci_dev *A = dev_at(0, 1), *F = dev_at(0, 5), *G = dev_at(0, 6),
                         *B = dev_at(0, 2), *C = dev_at(1, 0), *C3 = dev_at(1, 1),
                         *H = dev_at(0, 7), *H0 = dev_at(2, 0), *H1 = dev_at(2, 1),
                         *J = dev_at(0, 8), *J0 = dev_at(3, 0);
    assert(A && F && G && B && C && C3 && H && H0 && H1 && J && J0);

    /* Kept as found, never rewritten. */
    assert(A->bars[0].inherited && A->bars[0].bus_addr == 0x10200000);
    assert(model_get(a, 0x10, 4) == 0x10200004 && !programmed(a, 0x10, 0x10200004));
    assert(B->secondary_bus == 1 && B->subordinate_bus == 1 && !programmed(b, 0x18, 0x00010100));
    assert(!programmed(b, 0x20, 0x10001000));
    assert(C->bars[0].inherited && C->bars[0].bus_addr == 0x10000000);
    /* Assigned around the kept ones. */
    assert(!F->bars[0].inherited && F->bars[0].bus_addr != 0x10200000 && F->bars[0].cpu_addr);
    assert(F->bars[0].bus_addr + 0x4000 <= 0x10200000 || F->bars[0].bus_addr >= 0x10204000);
    assert(!G->bars[0].inherited && G->bars[0].bus_addr < 0x3eff0000 && G->bars[0].cpu_addr);
    assert(!C3->bars[0].inherited && C3->bars[0].bus_addr >= 0x10004000 &&
           C3->bars[0].bus_addr + 0x1000 <= 0x10100000);
    /* H reassigned with its whole subtree, outside every kept range. */
    uint32_t hw = model_get(h, 0x20, 4);
    uint64_t hb = (uint64_t)(hw & 0xfff0) << 16, hl = ((uint64_t)(hw >> 16 & 0xfff0) << 16) | 0xfffff;
    assert(hl - hb + 1 == 0x200000 && hb % 0x100000 == 0);
    assert(!H0->bars[0].inherited && !H1->bars[0].inherited);
    assert(H0->bars[0].bus_addr >= hb && H1->bars[0].bus_addr + 0x100000 - 1 <= hl);
    assert(hb >= 0x10300000 || hl < 0x10000000);
    /* J renumbered: bus 3. */
    assert(J->secondary_bus == 3 && J->subordinate_bus == 3 && model_get(j, 0x19, 1) == 3);

    /* Enabling C writes nothing on B (already decoding and mastering). */
    unsigned before = model_writes(b, 0x04, 0x06);
    assert(!kshim_pci_enable_device(C, true));
    assert(model_writes(b, 0x04, 0x06) == before);
    assert(model_get(c, 0x04, 2) == 0x0006);

    /* Handoff: everything as found, bus mastering on A and B as found. */
    assert(!kshim_pci_quiesce());
    model_expect(Saved);
    assert(model_get(a, 0x04, 2) == 0x0006 && model_get(b, 0x04, 2) == 0x0006);
    (void)c3; (void)h0; (void)h1; (void)j0; (void)g; (void)f;
    printf("pci_inherit_test: ok\n");
    return 0;
}
