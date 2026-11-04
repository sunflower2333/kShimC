/* SPDX-License-Identifier: MIT */
/*
 * QUP/GENI discovery on real and focused DTs (no MMIO): Android andromeda and
 * hdk8150 (SM8150), the focused kona fixture (SM8250 Android form), mainline
 * sm8150-hdk, sm8250-hdk, sm8650-qrd and the AYANEO Pocket S2 with the RFC
 * touch node. Checks wrappers, shared windows, protocols, I2C bus rates, the
 * windows kshim_qup_map_resources() maps, SE interrupts (GIC 3- and 4-cell,
 * Android PDC root) and the I2C/SPI children the touch drivers bind.
 */
#include <kshim/qup.h>
#include <kshim/i2c.h>
#include <kshim/spi.h>
#include <kshim/io.h>
#include <kshim/smp.h>
#include <kshim/time.h>
#include <kshim_irq_dt.h>
#include <qcom_irq.h>
#include <qcom_dt.h>
#include <dt.h>

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* kernel/core/io.c needs time and the GIC state. */
uint64_t kshim_smp_time_us(void) { return 0; }
bool kshim_irq_ready(void) { return false; }
uint64_t get_current_time(void) { return 0; }
void kshim_wait_event(uint64_t deadline_ms) { (void)deadline_ms; }
uint64_t delay(uint64_t us) { return us; }

static void *Loaded[16];
static unsigned LoadedCount;

static void *load(const char *dir, const char *name)
{
    char path[1024];
    snprintf(path, sizeof(path), "%s/%s", dir, name);
    FILE *file = fopen(path, "rb");
    if (!file) fprintf(stderr, "missing fixture %s\n", path);
    assert(file);
    assert(!fseek(file, 0, SEEK_END));
    long size = ftell(file);
    assert(size > 0 && !fseek(file, 0, SEEK_SET));
    void *data = aligned_alloc(8, ((size_t)size + 7) & ~(size_t)7);
    assert(data && fread(data, 1, (size_t)size, file) == (size_t)size);
    fclose(file);
    assert(!dt_validate(data, fdt_totalsize(data)));
    assert(LoadedCount < 16);
    Loaded[LoadedCount++] = data;
    return data;
}

static struct kshim_qup_wrapper W[KSHIM_QUP_MAX_WRAPPERS];
static struct kshim_qup_se S[KSHIM_QUP_MAX_SE];
static unsigned WCount, SCount;

static void discover(const void *f, const char *label)
{
    assert(!kshim_dt_qup_discover(f, W, KSHIM_QUP_MAX_WRAPPERS, &WCount, S, KSHIM_QUP_MAX_SE, &SCount));
    printf("%s: %u wrappers, %u windows\n", label, WCount, SCount);
    for (unsigned i = 0; i < SCount; ++i) {
        const struct kshim_qup_se *se = &S[i];
        unsigned nodes = 0;
        for (unsigned k = 0; k < 3; ++k) nodes += se->nodes[k] >= 0;
        printf("  %#llx %s proto %u wrapper %#llx nodes %u%s\n", (unsigned long long)se->base,
               fdt_get_name(f, se->node, NULL), se->dt_protocol,
               se->wrapper ? (unsigned long long)se->wrapper->base : 0ULL, nodes,
               kshim_dt_qup_window_used(f, se) ? " mapped" : "");
        /* Invariants: index, owner unset, an available first node. */
        assert(se->index == i && se->owner == KSHIM_QUP_OWNER_NONE && se->owner_node == -1);
        assert(dt_available(f, se->node) && se->size >= 0x1000);
        assert(se->dt_protocol == kshim_dt_qup_protocol(f, se->node));
        assert(!se->acquired && !se->intid);
    }
}

static const struct kshim_qup_se *window(uint64_t base)
{
    for (unsigned i = 0; i < SCount; ++i)
        if (S[i].base == base) return &S[i];
    return NULL;
}

static unsigned node_count(const struct kshim_qup_se *se)
{
    unsigned n = 0;
    for (unsigned k = 0; k < 3; ++k) n += se->nodes[k] >= 0;
    return n;
}

static int node_of(const void *f, const struct kshim_qup_se *se, enum kshim_qup_protocol protocol)
{
    for (unsigned k = 0; k < 3; ++k)
        if (se->nodes[k] >= 0 && kshim_dt_qup_protocol(f, se->nodes[k]) == protocol) return se->nodes[k];
    return -1;
}

static int child_named(const void *f, int parent, const char *prefix)
{
    int child;
    fdt_for_each_subnode(child, f, parent)
        if (!strncmp(fdt_get_name(f, child, NULL), prefix, strlen(prefix))) return child;
    return -1;
}

/* ---- Fake controllers for the client/device parsers --------------------- */

static int fake_xfer(struct kshim_i2c_adapter *a, const struct kshim_i2c_msg *m, size_t n, uint32_t t)
{ (void)a; (void)m; (void)n; (void)t; return 0; }
static const struct kshim_i2c_adapter_ops FakeI2cOps = {fake_xfer, NULL};
static struct kshim_i2c_adapter Adapters[4];
static unsigned AdapterCount;

static int fake_transfer(struct kshim_spi_controller *c, const struct kshim_spi_device *d,
                         const struct kshim_spi_xfer *x, size_t n, uint32_t t)
{ (void)c; (void)d; (void)x; (void)n; (void)t; return 0; }
static const struct kshim_spi_controller_ops FakeSpiOps = {fake_transfer, NULL};
static struct kshim_spi_controller Controllers[4];
static unsigned ControllerCount;

static struct kshim_i2c_adapter *fake_adapter(const void *f, int node)
{
    struct kshim_i2c_adapter *a = &Adapters[AdapterCount++];
    *a = (struct kshim_i2c_adapter){.name = "fake", .node = node, .ops = &FakeI2cOps,
                                    .bus_hz = kshim_dt_qup_i2c_bus_hz(f, node)};
    assert(!kshim_i2c_adapter_register(a));
    assert(kshim_i2c_adapter_register(a) == KSHIM_IO_ERR_EXISTS);
    return a;
}

static struct kshim_spi_controller *fake_controller(int node)
{
    struct kshim_spi_controller *c = &Controllers[ControllerCount++];
    *c = (struct kshim_spi_controller){.name = "fake", .node = node, .num_chipselect = 4, .ops = &FakeSpiOps};
    assert(!kshim_spi_controller_register(c));
    return c;
}

static int count_child(const struct kshim_i2c_client *c, void *ctx)
{
    (void)c;
    ++*(unsigned *)ctx;
    return 0;
}

/* ---- Fixtures ------------------------------------------------------------ */

static void test_andromeda(void)
{
    const void *f = load(FIXTURES, "andromeda-fdt.dtb");
    discover(f, "andromeda");
    assert(WCount == 3);
    /* Touch: qupv3_se17_i2c, wrapper 2 (0xcc0000); the disabled SPI node on
     * the same window is recorded too. */
    const struct kshim_qup_se *touch = window(0xc80000);
    assert(touch && touch->dt_protocol == KSHIM_QUP_PROTOCOL_I2C && node_count(touch) == 2);
    assert(touch->wrapper && touch->wrapper->base == 0xcc0000 && touch->wrapper->size == 0x6000);
    assert(!fdt_node_check_compatible(f, touch->wrapper->node, "qcom,qupv3-geni-se"));
    int i2c = node_of(f, touch, KSHIM_QUP_PROTOCOL_I2C), spi = node_of(f, touch, KSHIM_QUP_PROTOCOL_SPI);
    assert(i2c == touch->node && spi >= 0 && !dt_available(f, spi));
    assert(kshim_dt_qup_android(f, i2c) && touch->clock_frequency == 0);
    assert(kshim_dt_qup_i2c_bus_hz(f, i2c) == 400000); /* the former FTM5 binding's default */
    assert(kshim_dt_qup_window_used(f, touch));
    /* Console: msm-geni-console at 0xa90000 under wrapper 1. */
    const struct kshim_qup_se *console = window(0xa90000);
    assert(console && console->dt_protocol == KSHIM_QUP_PROTOCOL_UART);
    assert(console->wrapper && console->wrapper->base == 0xac0000);
    assert(!fdt_node_check_compatible(f, console->node, "qcom,msm-geni-console"));
    assert(kshim_dt_qup_window_used(f, console));
    /* The HS UART uses interrupts-extended (GIC + wake GPIO). */
    const struct kshim_qup_se *hs = window(0xc8c000);
    assert(hs && hs->dt_protocol == KSHIM_QUP_PROTOCOL_UART && hs->wrapper->base == 0xcc0000);
    /* SE interrupt: <0 0x175 0> under the Android PDC root. */
    struct kshim_dt_irq irq;
    assert(!kshim_dt_irq(f, i2c, 0, &irq) && irq.count == 3 && irq.cells[1] == 0x175);
    assert(kshim_dt_irq_kind(f, irq.parent) == KSHIM_DT_IRQ_PDC);
    /* FTM5 client. */
    struct kshim_i2c_adapter *a = fake_adapter(f, i2c);
    struct kshim_i2c_client client;
    int fts = child_named(f, i2c, "fts@49");
    assert(fts >= 0 && !kshim_i2c_client_from_node(f, fts, &client));
    assert(client.adapter == a && client.addr == 0x49 && !client.flags && client.node == fts);
    unsigned children = 0;
    assert(!kshim_i2c_for_each_child(f, a, count_child, &children) && children == 1);
    /* The former FTM5 binding (kernel/dt/touch.c, CrQupResources) and the
     * new layer name the same engine, address and bus rate. */
    struct kshim_touch_resource legacy;
    assert(!kshim_dt_touch(f, &legacy) && legacy.node == fts);
    assert(legacy.address == client.addr && legacy.qup.geni == touch->base);
    assert(legacy.qup.geni_size == touch->size && legacy.qup.speed_hz == a->bus_hz);
    assert(legacy.qup.speed_hz == kshim_dt_qup_i2c_bus_hz(f, i2c));
}

static void test_hdk8150(void)
{
    const void *f = load(FIXTURES, "hdk8150-fdt.dtb");
    discover(f, "hdk8150");
    assert(WCount >= 2 && SCount >= 1);
    for (unsigned i = 0; i < SCount; ++i)
        assert(S[i].wrapper && !fdt_node_check_compatible(f, S[i].wrapper->node, "qcom,qupv3-geni-se"));
    const struct kshim_qup_se *console = window(0xa90000);
    assert(console && console->dt_protocol == KSHIM_QUP_PROTOCOL_UART);
}

static void test_kona(void)
{
    const void *f = load(QUP_GENERATED, "kona-qup.dtb");
    discover(f, "kona-qup");
    /* Only windows with an available node; the shared SE12 window (console
     * UART and I2C, both disabled) is not registered. */
    assert(SCount == 2 && WCount == 2);
    const struct kshim_qup_se *console = window(0x988000), *touch = window(0xa94000);
    assert(console && console->dt_protocol == KSHIM_QUP_PROTOCOL_UART && console->wrapper->base == 0x9c0000);
    assert(touch && touch->dt_protocol == KSHIM_QUP_PROTOCOL_I2C && touch->wrapper->base == 0xac0000);
    assert(!window(0xa90000) && !window(0xa80000) && !window(0x88c000));
    /* The wrapper node of the focused fixture has a compatible here; the
     * back reference alone is what discovery uses. */
    assert(kshim_dt_qup_i2c_bus_hz(f, touch->node) == 400000);
    assert(kshim_dt_qup_window_used(f, touch) && kshim_dt_qup_window_used(f, console));
    /* SE13 SPI 358, level high, GIC parent. */
    uint32_t intid;
    unsigned trigger;
    assert(!kshim_irq_from_fdt(f, touch->node, 0, &intid, &trigger));
    assert(intid == KSHIM_IRQ_SPI(358) && trigger == KSHIM_IRQ_TYPE_LEVEL_HIGH);
    struct kshim_i2c_adapter *a = fake_adapter(f, touch->node);
    struct kshim_i2c_client client;
    int fts = child_named(f, touch->node, "st_fts@49");
    assert(!kshim_i2c_client_from_node(f, fts, &client) && client.adapter == a && client.addr == 0x49);
}

static void test_sm8650_qrd(void)
{
    const void *f = load(QUP_FIXTURES, "sm8650-qrd.dtb");
    discover(f, "sm8650-qrd");
    /* &spi4 is enabled for the GT9916, &i2c4 shares its window. */
    const struct kshim_qup_se *se4 = window(0xa90000);
    assert(se4 && se4->dt_protocol == KSHIM_QUP_PROTOCOL_SPI && node_count(se4) == 2);
    int spi = node_of(f, se4, KSHIM_QUP_PROTOCOL_SPI), i2c = node_of(f, se4, KSHIM_QUP_PROTOCOL_I2C);
    assert(spi == se4->node && i2c >= 0 && !dt_available(f, i2c));
    assert(!strcmp(fdt_get_name(f, spi, NULL), "spi@a90000"));
    assert(se4->wrapper && se4->wrapper->base == 0xac0000 && se4->wrapper->size == 0x2000);
    assert(!fdt_node_check_compatible(f, se4->wrapper->node, "qcom,geni-se-qup"));
    assert(dt_string_index(f, se4->wrapper->node, "clock-names", "m-ahb") == 0);
    assert(dt_string_index(f, spi, "clock-names", "se") == 0);
    assert(!kshim_dt_qup_android(f, spi) && kshim_dt_qup_window_used(f, se4));
    /* <GIC_SPI 357 IRQ_TYPE_LEVEL_HIGH 0> on the 4-cell GIC. */
    uint32_t intid;
    unsigned trigger;
    assert(!kshim_irq_from_fdt(f, spi, 0, &intid, &trigger));
    assert(intid == KSHIM_IRQ_SPI(357) && trigger == KSHIM_IRQ_TYPE_LEVEL_HIGH);
    /* Console: uart15, qcom,geni-debug-uart at 0x89c000 under the other wrapper. */
    const struct kshim_qup_se *uart15 = window(0x89c000);
    assert(uart15 && uart15->dt_protocol == KSHIM_QUP_PROTOCOL_UART && uart15->wrapper->base == 0x8c0000);
    /* GT9916 SPI device: CS 0, mode 0, 1 MHz. */
    struct kshim_spi_controller *c = fake_controller(spi);
    struct kshim_spi_device device;
    int ts = child_named(f, spi, "touchscreen@0");
    assert(ts >= 0 && !kshim_spi_device_from_node(f, ts, &device));
    assert(device.controller == c && device.chip_select == 0 && device.mode == KSHIM_SPI_MODE_0);
    assert(device.max_speed_hz == 1000000 && device.bits_per_word == 8 && device.node == ts);
    /* The disabled i2c4 node does not resolve as an adapter. */
    struct kshim_i2c_adapter *a;
    assert(kshim_i2c_adapter_get(f, i2c, &a) == ERROR_NOT_FOUND);
}

static void test_pocket_s2(void)
{
    const void *f = load(QUP_FIXTURES, "sm8650-ayaneo-pocket-s2-touch.dtb");
    discover(f, "pocket-s2");
    const struct kshim_qup_se *se4 = window(0xa90000);
    assert(se4 && se4->dt_protocol == KSHIM_QUP_PROTOCOL_I2C && node_count(se4) == 2);
    int i2c = node_of(f, se4, KSHIM_QUP_PROTOCOL_I2C), spi = node_of(f, se4, KSHIM_QUP_PROTOCOL_SPI);
    assert(i2c == se4->node && spi >= 0 && !dt_available(f, spi));
    assert(se4->clock_frequency == 400000 && kshim_dt_qup_i2c_bus_hz(f, i2c) == 400000);
    assert(se4->wrapper && se4->wrapper->base == 0xac0000 && kshim_dt_qup_window_used(f, se4));
    uint32_t intid;
    unsigned trigger;
    assert(!kshim_irq_from_fdt(f, i2c, 0, &intid, &trigger) && intid == KSHIM_IRQ_SPI(357));
    /* GT911 at 0x5d. */
    struct kshim_i2c_adapter *a = fake_adapter(f, i2c);
    struct kshim_i2c_client client;
    int ts = child_named(f, i2c, "touchscreen@5d");
    assert(ts >= 0 && !kshim_i2c_client_from_node(f, ts, &client));
    assert(client.adapter == a && client.addr == 0x5d && !client.flags);
    /* Lookups by adapter node are cached; the disabled SPI node is not a
     * controller. */
    struct kshim_i2c_adapter *again;
    assert(!kshim_i2c_adapter_get(f, i2c, &again) && again == a);
    struct kshim_spi_controller *c;
    assert(kshim_spi_controller_get(f, spi, &c) == ERROR_NOT_FOUND);
}

static void test_mainline_8150_8250(void)
{
    static const struct { const char *file; uint64_t console; } boards[] = {
        {"sm8250-hdk.dtb", 0xa90000}, {"sm8150-hdk.dtb", 0xa90000},
    };
    for (unsigned b = 0; b < 2; ++b) {
        const void *f = load(QUP_FIXTURES, boards[b].file);
        discover(f, boards[b].file);
        assert(SCount >= 1 && WCount >= 1);
        for (unsigned i = 0; i < SCount; ++i) {
            assert(S[i].wrapper && !fdt_node_check_compatible(f, S[i].wrapper->node, "qcom,geni-se-qup"));
            assert(!kshim_dt_qup_android(f, S[i].node));
            if (S[i].dt_protocol == KSHIM_QUP_PROTOCOL_I2C)
                assert(kshim_dt_qup_i2c_bus_hz(f, S[i].node) ==
                       (S[i].clock_frequency ? S[i].clock_frequency : 100000U));
        }
        const struct kshim_qup_se *console = window(boards[b].console);
        assert(console && console->dt_protocol == KSHIM_QUP_PROTOCOL_UART);
        assert(!fdt_node_check_compatible(f, console->node, "qcom,geni-debug-uart"));
    }
}

static void test_capacity(void)
{
    const void *f = load(FIXTURES, "andromeda-fdt.dtb");
    struct kshim_qup_wrapper w[1];
    struct kshim_qup_se s[2];
    unsigned wc, sc;
    assert(kshim_dt_qup_discover(f, w, 1, &wc, s, 2, &sc) == KSHIM_IO_ERR_OVERFLOW);
    assert(sc == 2 && wc == 1);
    assert(kshim_dt_qup_discover(NULL, w, 1, &wc, s, 2, &sc) == ERROR_INVALID_PARAMETER);
    assert(kshim_dt_qup_protocol(f, 0) == KSHIM_QUP_PROTOCOL_NONE);
}

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    test_andromeda();
    test_hdk8150();
    test_kona();
    test_sm8650_qrd();
    test_pocket_s2();
    test_mainline_8150_8250();
    test_capacity();
    while (LoadedCount) free(Loaded[--LoadedCount]);
    puts("qup_dt_test: ok");
    return 0;
}
