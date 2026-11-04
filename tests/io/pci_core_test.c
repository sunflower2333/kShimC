/* SPDX-License-Identifier: MIT */
/*
 * PCI core over an ECAM register model (tests/io/pci_model.h) behind the
 * generic ECAM host and tests/fixtures/io/pci-ecam.dts:
 *  - map_resources: ECAM limited to bus-range, windows capped, disabled
 *    hosts skipped;
 *  - enumeration: multifunction, conventional bridge, PCIe root port (only
 *    device 0 below), depth-first bus numbers, DT node matching;
 *  - assignment from the fresh state: 32/64-bit, prefetchable and I/O BARs
 *    aligned inside the windows, bridge windows covering their children;
 *  - INTx swizzle through bridges and the interrupt-map, a shared GIC line;
 *  - driver binding order, probe fallback, late registration;
 *  - handoff: drivers quiesced newest first, every config byte restored
 *    (W1C status bits left alone), idempotent, API refused afterwards.
 */
#define _GNU_SOURCE
#include "../../kernel/drivers/pci/pci_internal.h"
#include "pci_model.h"

#include <kshim/irq.h>
#include <kshim/smp.h>
#include <kshim/time.h>
#include <qcom_dt.h>

#include <stdlib.h>

#define ECAM_BASE UINT64_C(0x4010000000)
#define ECAM_SIZE UINT64_C(0x1000000)

/* ---- environment stubs ------------------------------------------------- */
static uint64_t now_us;
uint64_t kshim_smp_time_us(void) { return now_us; }
uint64_t delay(uint64_t us) { now_us += us; return us; }
bool kshim_irq_ready(void) { return false; }
uint64_t get_current_time(void) { return now_us / 1000; }
void kshim_wait_event(uint64_t deadline_ms) { now_us = deadline_ms * 1000 + 1; }

struct irq_rec { uint32_t intid; unsigned trigger; kshim_irq_handler_t handler; void *ctx; bool live; };
static struct irq_rec Irqs[8];
static unsigned IrqRequests, IrqFrees;
int kshim_irq_request(uint32_t intid, unsigned trigger, kshim_irq_handler_t handler, void *ctx)
{
    for (unsigned i = 0; i < 8; ++i)
        if (!Irqs[i].live) {
            Irqs[i] = (struct irq_rec){intid, trigger, handler, ctx, true};
            ++IrqRequests;
            return 0;
        }
    return ERROR_BUSY;
}
int kshim_irq_free(uint32_t intid)
{
    for (unsigned i = 0; i < 8; ++i)
        if (Irqs[i].live && Irqs[i].intid == intid) {
            Irqs[i].live = false;
            ++IrqFrees;
            return 0;
        }
    return ERROR_NOT_FOUND;
}
unsigned long kshim_irq_save(void) { return 0; }
void kshim_irq_restore(unsigned long flags) { (void)flags; }
static void fire(uint32_t intid)
{
    for (unsigned i = 0; i < 8; ++i)
        if (Irqs[i].live && Irqs[i].intid == intid) Irqs[i].handler(intid, Irqs[i].ctx);
}

static struct dt_range Maps[32];
static unsigned MapCount;
int kshim_resource_map(struct kshim_resources *r, uint64_t base, uint64_t size,
                       kshim_mmu_memory_type_t type, uint32_t permissions)
{
    (void)r;
    assert(type == KSHIM_MMU_DEVICE && permissions == (KSHIM_MMU_READ | KSHIM_MMU_WRITE));
    assert(MapCount < 32);
    Maps[MapCount++] = (struct dt_range){base, size};
    return 0;
}

/* ECAM decode onto the model. */
static uint32_t mmio_read(uintptr_t a, unsigned width)
{
    assert(a >= ECAM_BASE && a < ECAM_BASE + ECAM_SIZE);
    uint64_t off = a - ECAM_BASE;
    return model_cfg_read((unsigned)(off >> 20), (uint8_t)(off >> 12), off & 0xfff, width);
}
static void mmio_write(uintptr_t a, unsigned width, uint32_t v)
{
    assert(a >= ECAM_BASE && a < ECAM_BASE + ECAM_SIZE);
    uint64_t off = a - ECAM_BASE;
    model_cfg_write((unsigned)(off >> 20), (uint8_t)(off >> 12), off & 0xfff, width, v);
}
static const struct kshim_pci_test_mmio Mmio = {mmio_read, mmio_write};
const struct kshim_pci_test_mmio *kshim_pci_test_mmio = &Mmio;

/* ---- drivers ----------------------------------------------------------- */
static unsigned IrqCount[KSHIM_PCI_MAX_DEVICES];
static struct kshim_pci_dev *QuiesceOrder[8];
static unsigned QuiesceCount, ProbesX, ProbesY;

static void count_irq(uint32_t intid, void *ctx)
{
    (void)intid;
    ++*(unsigned *)ctx;
}

static int probe_x(struct kshim_pci_dev *dev, const struct kshim_pci_device_id *id)
{
    ++ProbesX;
    assert(id->data == 0x58);
    if (dev->vendor != 0x1b36) return ERROR_NOT_SUPPORTED; /* NEC: leave to driver Y */
    assert(!kshim_pci_enable_device(dev, true));
    uint32_t intid = 0;
    static unsigned slot;
    assert(!kshim_pci_request_irq(dev, KSHIM_PCI_IRQ_INTX | KSHIM_PCI_IRQ_MSI, count_irq,
                                  &IrqCount[slot++], &intid));
    assert(intid == dev->intx_intid);
    return 0;
}
static int probe_y(struct kshim_pci_dev *dev, const struct kshim_pci_device_id *id)
{
    (void)id;
    ++ProbesY;
    assert(!kshim_pci_enable_device(dev, true));
    return 0;
}
static int probe_z(struct kshim_pci_dev *dev, const struct kshim_pci_device_id *id)
{
    (void)id;
    return kshim_pci_enable_device(dev, false);
}
static int quiesce_any(struct kshim_pci_dev *dev)
{
    assert(QuiesceCount < 8);
    QuiesceOrder[QuiesceCount++] = dev;
    return 0;
}
static const struct kshim_pci_device_id IdsX[] = {
    {KSHIM_PCI_ANY_ID, KSHIM_PCI_ANY_ID, 0x0c0330, 0xffffff, 0x58}, {0}};
static const struct kshim_pci_device_id IdsY[] = {{0x1033, 0x0194, 0, 0, 0}, {0}};
static const struct kshim_pci_device_id IdsZ[] = {{0x1af4, KSHIM_PCI_ANY_ID, 0, 0, 0}, {0}};
static struct kshim_pci_driver DriverX = {"x", IdsX, probe_x, quiesce_any, NULL};
static struct kshim_pci_driver DriverY = {"y", IdsY, probe_y, quiesce_any, NULL};
static struct kshim_pci_driver DriverZ = {"z", IdsZ, probe_z, quiesce_any, NULL};

/* ---- helpers ----------------------------------------------------------- */
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

static struct kshim_pci_dev *dev_at(uint8_t bus, uint8_t slot, uint8_t fn)
{
    for (struct kshim_pci_dev *d = kshim_pci_find_device(KSHIM_PCI_ANY_ID, KSHIM_PCI_ANY_ID, NULL);
         d; d = kshim_pci_find_device(KSHIM_PCI_ANY_ID, KSHIM_PCI_ANY_ID, d))
        if (d->bus == bus && d->devfn == KSHIM_PCI_DEVFN(slot, fn)) return d;
    return NULL;
}

static void check_bar(const struct kshim_pci_dev *d, unsigned i, uint64_t lo, uint64_t hi)
{
    const struct kshim_pci_bar *b = &d->bars[i];
    assert(b->size && b->cpu_addr && !b->inherited);
    assert(b->bus_addr % b->size == 0);
    assert(b->bus_addr >= lo && b->bus_addr + b->size <= hi);
    assert(b->cpu_addr == b->bus_addr); /* identity windows in the fixture */
}

static void bridge_mem(int f, uint64_t *base, uint64_t *limit)
{
    uint32_t v = model_get(f, 0x20, 4);
    *base = (uint64_t)(v & 0xfff0) << 16;
    *limit = ((uint64_t)(v >> 16 & 0xfff0) << 16) | 0xfffff;
}

static uint8_t Saved[MODEL_FUNCS][4096];

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    const void *fdt = load(FIXTURES "/pci-ecam.dtb");

    /* Topology. */
    int host = model_add(-1, 0, 0, 0x1b36, 0x0008, 0x060000, 0x00, 0);
    int a = model_add(-1, 1, 0, 0x1b36, 0x000d, 0x0c0330, 0x00, 1);
    model_bar(a, 0, 0x4000, 4);
    model_set(a, 0x06, 2, 0x2000);                 /* inherited W1C status bit */
    int b = model_add(-1, 2, 0, 0x1b36, 0x0001, 0x060400, 0x01, 1);
    model_bar(b, 0, 0x100, 0);
    int c = model_add(b, 0, 0, 0x1033, 0x0194, 0x0c0330, 0x00, 1);
    model_bar(c, 0, 0x100000, 0xc);                /* 64-bit prefetchable 1 MiB */
    model_bar(c, 2, 0x20, 1);                      /* 32 bytes of I/O */
    int c2 = model_add(b, 3, 0, 0x1b36, 0x000d, 0x0c0330, 0x00, 1);
    model_bar(c2, 0, 0x4000, 4);
    int d0 = model_add(-1, 3, 0, 0x1af4, 0x1000, 0x020000, 0x80, 1);
    model_bar(d0, 0, 0x1000, 0);
    int d1 = model_add(-1, 3, 1, 0x1af4, 0x1001, 0x020000, 0x00, 2);
    model_bar(d1, 0, 0x1000, 0);
    int e = model_add(-1, 4, 0, 0x1b36, 0x000c, 0x060400, 0x01, 1);
    model_pcie_port(e, 4);
    int e0 = model_add(e, 0, 0, 0x1b36, 0x0010, 0x010802, 0x00, 1);
    model_bar(e0, 0, 0x1000, 0);
    int e1 = model_add(e, 1, 0, 0x1b36, 0x0010, 0x010802, 0x00, 1); /* alias: never scanned */
    (void)host;
    model_snapshot(Saved);

    /* Map: ECAM limited to the window (16 buses), windows capped at 1 GiB. */
    struct kshim_resources res;
    memset(&res, 0, sizeof(res));
    res.fdt = fdt;
    assert(!kshim_pci_map_resources(&res));
    assert(MapCount == 4);
    assert(Maps[0].base == ECAM_BASE && Maps[0].size == ECAM_SIZE);
    assert(Maps[1].base == 0x3eff0000 && Maps[1].size == 0x10000);
    assert(Maps[2].base == 0x10000000 && Maps[2].size == 0x2eff0000);
    assert(Maps[3].base == UINT64_C(0x8000000000) && Maps[3].size == KSHIM_PCI_WINDOW_CAP);

    assert(!kshim_pci_driver_register(&DriverX));
    assert(!kshim_pci_driver_register(&DriverY));
    assert(kshim_pci_driver_register(&DriverY) == KSHIM_IO_ERR_EXISTS);
    assert(!kshim_pci_init(fdt));
    assert(!kshim_pci_init(fdt)); /* idempotent */
    assert(!ModelLogCount || model_writes(e1, 0, 0x1000) == 0);

    /* Enumeration and bus numbers (depth first: bridge 00:02 -> 1, port 00:04 -> 2). */
    struct kshim_pci_dev *A = dev_at(0, 1, 0), *B = dev_at(0, 2, 0), *C = dev_at(1, 0, 0),
                         *C2 = dev_at(1, 3, 0), *D0 = dev_at(0, 3, 0), *D1 = dev_at(0, 3, 1),
                         *E = dev_at(0, 4, 0), *E0 = dev_at(2, 0, 0);
    assert(A && B && C && C2 && D0 && D1 && E && E0 && dev_at(0, 0, 0));
    assert(!dev_at(2, 1, 0));
    assert(B->secondary_bus == 1 && B->subordinate_bus == 1);
    assert(E->secondary_bus == 2 && E->subordinate_bus == 2);
    assert(model_get(b, 0x18, 4) == 0x010100 && model_get(e, 0x18, 4) == 0x020200);
    assert(C->bridge == B && E0->bridge == E && A->bridge == NULL);
    assert(C->class_code == 0x0c0330 && C->vendor == 0x1033 && C->device == 0x0194);
    assert(E->cap_exp == 0x40 && !A->cap_exp);
    assert(B->node >= 0 && C->node >= 0 && A->node < 0);
    assert(!strcmp(fdt_get_name(fdt, C->node, NULL), "usb@0"));

    /* BARs: inside the 32-bit window, aligned, non-overlapping, programmed. */
    const uint64_t lo = 0x10000000, hi = 0x10000000 + 0x2eff0000;
    check_bar(A, 0, lo, hi);
    assert(A->bars[0].flags == KSHIM_PCI_BAR_MEM64 && A->bars[0].size == 0x4000);
    assert(model_get(a, 0x10, 4) == ((uint32_t)A->bars[0].bus_addr | 4) && !model_get(a, 0x14, 4));
    check_bar(B, 0, lo, hi);
    check_bar(C, 0, lo, hi);
    assert(C->bars[0].flags == (KSHIM_PCI_BAR_MEM64 | KSHIM_PCI_BAR_PREFETCH));
    assert(C->bars[2].flags == KSHIM_PCI_BAR_IO && C->bars[2].size == 0x20);
    assert(C->bars[2].bus_addr >= 0x1000 && C->bars[2].bus_addr < 0x10000);
    assert(C->bars[2].cpu_addr == 0x3eff0000 + C->bars[2].bus_addr);
    check_bar(C2, 0, lo, hi);
    check_bar(D0, 0, lo, hi);
    check_bar(D1, 0, lo, hi);
    check_bar(E0, 0, lo, hi);
    struct kshim_pci_dev *all[] = {A, B, C, C2, D0, D1, E0};
    for (unsigned i = 0; i < 7; ++i)
        for (unsigned j = i + 1; j < 7; ++j)
            assert(all[i]->bars[0].bus_addr + all[i]->bars[0].size <= all[j]->bars[0].bus_addr ||
                   all[j]->bars[0].bus_addr + all[j]->bars[0].size <= all[i]->bars[0].bus_addr);
    /* Bridge windows cover the subtree; I/O window opened for C's I/O BAR. */
    uint64_t wb, wl;
    bridge_mem(b, &wb, &wl);
    assert(wb % 0x100000 == 0 && C->bars[0].bus_addr >= wb && C2->bars[0].bus_addr >= wb);
    assert(C->bars[0].bus_addr + C->bars[0].size - 1 <= wl);
    assert(C2->bars[0].bus_addr + C2->bars[0].size - 1 <= wl);
    assert(B->bars[0].bus_addr > wl || B->bars[0].bus_addr + 0x100 <= wb);
    uint32_t io = model_get(b, 0x1c, 2);
    assert(C->bars[2].bus_addr >= ((io & 0xf0U) << 8) &&
           C->bars[2].bus_addr + 0x20 - 1 <= (((io >> 8) & 0xf0U) << 8 | 0xfff));
    assert(model_get(b, 0x24, 4) == 0x0001fff1); /* prefetchable window closed */
    bridge_mem(e, &wb, &wl);
    assert(E0->bars[0].bus_addr >= wb && E0->bars[0].bus_addr + 0x1000 - 1 <= wl);
    assert(model_get(b, 0x04, 2) & KSHIM_PCI_COMMAND_MEMORY);
    assert(model_get(b, 0x04, 2) & KSHIM_PCI_COMMAND_IO);

    /* INTx: interrupt-map slot/pin, swizzled through the bridges. */
    assert(A->intx_intid == 32 + 4 && A->intx_trigger == KSHIM_IRQ_TYPE_LEVEL_HIGH);
    assert(C->intx_intid == 32 + 5);   /* 01:00 INTA -> 00:02 INTA -> SPI 5 */
    assert(C2->intx_intid == 32 + 4);  /* 01:03 INTA -> 00:02 INTD -> SPI 4 */
    assert(D1->intx_intid == 32 + 3);  /* 00:03 INTB -> SPI 3 */
    assert(E0->intx_intid == 32 + 3);  /* 02:00 INTA -> 00:04 INTA (slot 0 row) */

    /* Binding: X takes the QEMU xHCIs, declines NEC, Y then binds NEC. */
    assert(A->driver == &DriverX && C2->driver == &DriverX && C->driver == &DriverY);
    assert(ProbesX == 3 && ProbesY == 1 && !D0->driver);
    assert(!kshim_pci_driver_register(&DriverZ)); /* late: bound at once */
    assert(D0->driver == &DriverZ && D1->driver == &DriverZ);
    /* Enable: device and every upstream bridge decode and master. */
    assert((model_get(c2, 0x04, 2) & 6) == 6 && (model_get(b, 0x04, 2) & 6) == 6);
    unsigned before = ModelLogCount;
    assert(!kshim_pci_enable_device(C2, true));
    assert(ModelLogCount == before); /* equal value: nothing written */

    /* Shared INTx line 36 (A and C2): one GIC request, both handlers run. */
    assert(IrqRequests == 1 && Irqs[0].intid == 36 && Irqs[0].trigger == 4);
    fire(36);
    assert(IrqCount[0] == 1 && IrqCount[1] == 1);
    assert(!(model_get(a, 0x04, 2) & KSHIM_PCI_COMMAND_INTX_DISABLE));
    assert(kshim_pci_request_irq(A, KSHIM_PCI_IRQ_INTX, count_irq, NULL, NULL) == ERROR_BUSY);
    assert(kshim_pci_request_irq(D0, KSHIM_PCI_IRQ_MSI, count_irq, NULL, NULL) ==
           ERROR_NOT_SUPPORTED);
    assert(!kshim_pci_free_irq(A));
    assert(IrqFrees == 0);              /* C2 still uses the line */
    fire(36);
    assert(IrqCount[0] == 1 && IrqCount[1] == 2);
    assert(model_get(a, 0x04, 2) & KSHIM_PCI_COMMAND_INTX_DISABLE);

    /* Config API, capabilities, lookups. */
    uint16_t v16;
    uint8_t v8;
    assert(!kshim_pci_read_config16(C, 0, &v16) && v16 == 0x1033);
    assert(kshim_pci_read_config16(C, 1, &v16) == ERROR_INVALID_PARAMETER);
    assert(kshim_pci_find_capability(E, KSHIM_PCI_CAP_ID_EXP) == 0x40);
    assert(!kshim_pci_find_capability(A, KSHIM_PCI_CAP_ID_EXP));
    assert(kshim_pci_find_class(0x0c0330, 0xffffff, NULL) == A);
    assert(kshim_pci_find_class(0x0c0330, 0xffffff, A) == C);
    assert(kshim_pci_find_device(0x1033, 0x0194, NULL) == C);
    assert(kshim_pci_bar_map(A, 0) == (volatile void *)(uintptr_t)A->bars[0].cpu_addr);
    assert(!kshim_pci_bar_map(C, 2) && !kshim_pci_bar_map(A, 1));
    assert(kshim_pci_dma_addr(A, 0x80000000) == 0x80000000);
    assert(!kshim_pci_write_config8(D0, 0x3c, 0x5a));
    assert(!kshim_pci_read_config8(D0, 0x3c, &v8) && v8 == 0); /* RO in the model */
    unsigned raw_before = ModelLogCount;
    assert(!kshim_pci_write_config_raw32(D1, 0x40, 0x12345678));
    assert(ModelLogCount == raw_before + 1);

    /* Handoff: newest binding first, all config back, W1C bit kept. */
    assert(!kshim_pci_quiesce());
    assert(QuiesceCount == 5);
    assert(QuiesceOrder[0] == D1 && QuiesceOrder[4] == A);
    assert(IrqFrees == 1 && !Irqs[0].live);
    model_expect(Saved);
    assert(model_get(a, 0x06, 2) == 0x2000);
    before = ModelLogCount;
    assert(!kshim_pci_quiesce());
    assert(ModelLogCount == before);
    assert(kshim_pci_read_config16(A, 0, &v16) == KSHIM_IO_ERR_QUIESCED);
    assert(kshim_pci_enable_device(A, true) == KSHIM_IO_ERR_QUIESCED);
    assert(kshim_pci_request_irq(A, KSHIM_PCI_IRQ_INTX, count_irq, NULL, NULL) ==
           KSHIM_IO_ERR_QUIESCED);
    assert(!kshim_pci_bar_map(A, 0));
    printf("pci_core_test: ok\n");
    return 0;
}
