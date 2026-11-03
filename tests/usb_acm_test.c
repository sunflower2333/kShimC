/* SPDX-License-Identifier: MIT */
/*
 * USB CDC-ACM console: DT discovery, and the console mirror (UART + CrDK
 * UsbAcmCrLib through kernel/drivers/usb_acm.c) against CrDK's DWC3 and USB
 * host model (Tests/UsbAcmDwc3Model.c in the pinned CrDK tree).
 */
#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <kshim_console.h>
#include <pl011_uart.h>
#include <qcom_dt.h>
#include <usb_acm.h>
#include <Library/cr_geni.h>
#include "UsbAcmDwc3Model.h"

#define DWC3 0x0a600000UL

/*------------------------------------------------------------- the UART */
static char uart_out[65536], uart_in[16];
static size_t uart_length, uart_in_length, uart_in_position;
static uint64_t uart_base;
void pl011_init(uint64_t base) { uart_base = base; }
void pl011_write(uint64_t base, char *text, int length)
{
    assert(base == uart_base && uart_length + (size_t)length <= sizeof(uart_out));
    memcpy(uart_out + uart_length, text, (size_t)length);
    uart_length += (size_t)length;
}
int pl011_try_read(uint64_t base, char *character)
{
    assert(base == uart_base);
    if (uart_in_position >= uart_in_length) return 0;
    *character = uart_in[uart_in_position++];
    return 1;
}

/*------------------------------------------- local IRQ masking (per CPU) */
static _Thread_local unsigned long irq_depth;
unsigned long kshim_irq_save(void) { return irq_depth++; }
void kshim_irq_restore(unsigned long flags)
{
    assert(irq_depth > 0 && flags == irq_depth - 1); /* properly nested */
    irq_depth = flags;
}

/*-------------------------------------------- controller MMIO -> model */
static unsigned model_mmio;
static int nest;
static uint32_t read32(void *context, uintptr_t address)
{
    (void)context;
    assert(address >= DWC3 && address < DWC3 + 0xcd00);
    ++model_mmio;
    if (nest) { /* an exception printing from inside the console */
        nest = 0;
        kshim_console_write("nested\n", 7);
    }
    return AcmModelRead32(ACM_MODEL_WINDOW_DWC3, (uint32_t)(address - DWC3));
}
static void write32(void *context, uintptr_t address, uint32_t value)
{
    (void)context;
    assert(address >= DWC3 && address < DWC3 + 0xcd00);
    ++model_mmio;
    AcmModelWrite32(ACM_MODEL_WINDOW_DWC3, (uint32_t)(address - DWC3), value);
}
static uint64_t now_us(void *context)
{
    struct timespec t;
    (void)context;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000000U + (uint64_t)t.tv_nsec / 1000U;
}
static void delay_us(void *context, uint32_t us)
{
    uint64_t start = now_us(context);
    while (now_us(context) - start < us) {}
}
static const struct CrIo model_io = {NULL, read32, write32, now_us, delay_us};
const struct CrIo *kshim_usb_acm_io(void) { return &model_io; }

/*------------------------------------------------------- MMU resources */
static int map_fail;
static unsigned map_calls;
int kshim_resource_map(struct kshim_resources *r, uint64_t base, uint64_t size,
                       kshim_mmu_memory_type_t type, uint32_t permissions)
{
    ++map_calls;
    if (map_fail) return -1;
    assert(r->mapping_count < KSHIM_RESOURCE_MAX);
    r->mappings[r->mapping_count++] = (kshim_mmu_region_t){base, size, type, permissions};
    return 0;
}

/*-------------------------------------------------------------- helpers */
static void *fixture(const char *directory, const char *name)
{
    char path[1024];
    snprintf(path, sizeof(path), "%s/%s", directory, name);
    FILE *fp = fopen(path, "rb");
    assert(fp);
    assert(!fseek(fp, 0, SEEK_END));
    long size = ftell(fp);
    assert(size > 0);
    rewind(fp);
    void *data = calloc(1, (size_t)size + 16384);
    assert(data && fread(data, 1, (size_t)size, fp) == (size_t)size);
    fclose(fp);
    assert(!dt_validate(data, (size_t)size));
    assert(!fdt_open_into(data, data, (int)size + 16384));
    return data;
}

static void service(void *context) { (void)context; kshim_console_service(); }

static void fresh_model(void)
{
    AcmModelConfig config = {0x5533330AU, 1, NULL, 0, 0, 3, 0};
    AcmModelInit(&config);
    AcmModelSetService(service, NULL);
    model_mmio = 0;
    uart_length = uart_in_length = uart_in_position = 0;
    map_fail = 0;
    map_calls = 0;
}

static size_t host_read(char *buffer, size_t capacity)
{
    return AcmModelReadAll((uint8_t *)buffer, capacity);
}

static void expect_host(const char *text)
{
    char got[16384];
    size_t n = host_read(got, sizeof(got));
    if (n != strlen(text) || memcmp(got, text, n)) fprintf(stderr, "host got %zu: %.*s\n", n, (int)n, got);
    assert(n == strlen(text) && !memcmp(got, text, n));
}

/*--------------------------------------------------------- DT discovery */
static void discovery(void)
{
    struct kshim_usb_acm_resource r;
    void *f = fixture(GENERATED_FIXTURES, "usb-acm.dtb");
    assert(!kshim_dt_usb_acm(f, &r));
    assert(r.base == 0xa600000 && r.size == 0xcd00 && r.node == fdt_path_offset(f, "/soc/usb@a6f8800/usb@a600000"));
    assert(r.has_irq && r.gic && !r.pdc && r.irq_cells == 3 && r.intid == 32 + 133 && r.trigger == 4);
    assert(r.irq_parent == fdt_path_offset(f, "/soc/interrupt-controller@17a00000"));

    /* A host-only core is never chosen. */
    int first = fdt_path_offset(f, "/soc/usb@a6f8800/usb@a600000");
    assert(!fdt_setprop_string(f, first, "status", "disabled"));
    assert(kshim_dt_usb_acm(f, &r));
    first = fdt_path_offset(f, "/soc/usb@a6f8800/usb@a600000");
    assert(!fdt_setprop_string(f, first, "status", "okay"));

    /* Both can be devices: DT order, unless the bootloader names one. */
    int second = fdt_path_offset(f, "/soc/usb@a8f8800/usb@a800000"), chosen = fdt_path_offset(f, "/chosen");
    assert(!fdt_setprop_string(f, second, "dr_mode", "otg"));
    assert(!kshim_dt_usb_acm(f, &r) && r.base == 0xa600000);
    assert(!fdt_setprop_string(f, chosen, "bootargs", "console=ttyAMA0 androidboot.usbcontroller=a800000.dwc3 quiet"));
    assert(!kshim_dt_usb_acm(f, &r) && r.base == 0xa800000 && r.intid == 32 + 138);
    assert(!fdt_setprop_string(f, chosen, "bootargs", "androidboot.usbcontroller=a700000.dwc3"));
    assert(!kshim_dt_usb_acm(f, &r) && r.base == 0xa600000);
    /* Disabled cores and short windows are skipped; then nothing is left. */
    first = fdt_path_offset(f, "/soc/usb@a6f8800/usb@a600000");
    assert(!fdt_setprop_string(f, first, "status", "disabled"));
    assert(!kshim_dt_usb_acm(f, &r) && r.base == 0xa800000);
    fdt32_t small[4] = {cpu_to_fdt32(0), cpu_to_fdt32(0x0a800000), cpu_to_fdt32(0), cpu_to_fdt32(0x1000)};
    second = fdt_path_offset(f, "/soc/usb@a8f8800/usb@a800000");
    assert(!fdt_setprop(f, second, "reg", small, sizeof(small)));
    assert(kshim_dt_usb_acm(f, &r) && r.node == -1);
    free(f);

    /* Android vendor DTs: "qcom,dwc-usb3-msm" wrappers, inherited GIC parent. */
    const char *real[] = {"hdk8150-fdt.dtb", "andromeda-fdt.dtb"};
    for (unsigned i = 0; i < 2; ++i) {
        f = fixture(FIXTURES, real[i]);
        assert(!kshim_dt_usb_acm(f, &r));
        assert(r.base == 0xa600000 && r.size == 0xcd00);
        assert(!strcmp(fdt_get_name(f, r.node, NULL), "dwc3@a600000"));
        assert(r.has_irq && r.gic && r.pdc && r.intid == 32 + 0x85 && r.irq_cells == 3);
        free(f);
    }

    /* A DT with no DWC3 at all (the QEMU virt machine). */
    f = fixture(GENERATED_FIXTURES, "usb-acm.dtb");
    assert(!fdt_del_node(f, fdt_path_offset(f, "/soc/usb@a8f8800")));
    assert(!fdt_del_node(f, fdt_path_offset(f, "/soc/usb@a6f8800")));
    assert(kshim_dt_usb_acm(f, &r));
    free(f);
    printf("discovery: mainline and Android wrappers, bootargs selection, interrupts parsed\n");
}

/*------------------------------------------------------- console mirror */
static void mirror(void)
{
    fresh_model();
    void *f = fixture(GENERATED_FIXTURES, "usb-acm.dtb");
    assert(!kshim_console_init(f));
    const AcmModelState *s = AcmModelObserve();
    assert(kshim_usb_acm_active() && s->SoftResets == 1 && s->Running && !s->Halted);
    assert(kshim_console_base() == 0x9000000);
    /* Before any host: kept for later. */
    kshim_console_write("kShimC banner\n", 14);
    assert(uart_length == 14 && !memcmp(uart_out, "kShimC banner\n", 14));

    struct kshim_resources *r = calloc(1, sizeof(*r));
    assert(r && !kshim_console_map_resources(r));
    assert(r->mapping_count == 1 && r->mappings[0].base == DWC3 && r->mappings[0].size == 0xcd00 &&
           r->mappings[0].type == KSHIM_MMU_DEVICE &&
           r->mappings[0].permissions == (KSHIM_MMU_READ | KSHIM_MMU_WRITE));

    assert(AcmModelConnect() == ACM_MODEL_OK && AcmModelEnumerate(6) == ACM_MODEL_OK);
    assert(AcmModelOpenPort() == ACM_MODEL_OK);
    expect_host("kShimC banner\n");
    assert(s->StringLength[2] == 2 + 2 * 17 && s->Strings[2][2] == 'k');
    /* printf writes one character at a time. */
    const char *line = "Select kernel and press Enter: ";
    for (const char *p = line; *p; ++p) kshim_console_write(p, 1);
    expect_host(line);
    assert(uart_length == 14 + strlen(line));

    /* Input from either side; the UART is read first. */
    char c;
    assert(AcmModelBulkOut((const uint8_t *)"2\r", 2) == 2);
    assert(kshim_console_try_read(&c) && c == '2');
    assert(kshim_console_try_read(&c) && c == '\r');
    assert(!kshim_console_try_read(&c));
    memcpy(uart_in, "u", 1);
    uart_in_length = 1;
    assert(AcmModelBulkOut((const uint8_t *)"a", 1) == 1);
    assert(kshim_console_try_read(&c) && c == 'u');
    assert(kshim_console_try_read(&c) && c == 'a');

    /* Idle servicing alone answers the host. */
    uint8_t b[8];
    uint32_t n;
    assert(AcmModelControl(0x80, 0x00, 0, 0, 2, NULL, b, &n) == ACM_MODEL_OK && n == 2);

    /* Re-entry from inside the console reaches the UART only. */
    size_t before = uart_length;
    nest = 1;
    kshim_console_write("outer\n", 6);
    assert(!nest && uart_length == before + 13 && !memcmp(uart_out + before, "outer\nnested\n", 13));
    expect_host("outer\n");

    /* Handoff: flush, stop, nothing more reaches the controller. */
    kshim_console_write("Booting...\n", 11);
    assert(!kshim_console_quiesce());
    assert(!kshim_usb_acm_active() && !s->Running && s->Halted && s->DevTen == 0 && s->EventAddress == 0);
    unsigned mmio = model_mmio;
    kshim_console_write("late", 4);
    kshim_console_service();
    assert(!kshim_console_try_read(&c) && model_mmio == mmio);
    assert(s->Errors == 0);
    free(r);
    free(f);
    printf("mirror: UART and USB carry the same output, input from both, stopped at quiesce\n");
}

/*---------------------------------------------------------- SMP writers */
static void *writer(void *argument)
{
    char line[64];
    int id = (int)(intptr_t)argument;
    for (int i = 0; i < 120; ++i) {
        int length = snprintf(line, sizeof(line), "cpu%d line %03d ................\n", id, i);
        kshim_console_write(line, (uint32_t)length);
    }
    return NULL;
}

static void check_lines(const char *text, size_t length)
{
    int next[3] = {0, 0, 0};
    const char *p = text, *end = text + length;
    while (p < end) {
        int id, i;
        const char *eol = memchr(p, '\n', (size_t)(end - p));
        assert(eol && sscanf(p, "cpu%d line %d ", &id, &i) == 2 && id >= 1 && id <= 2);
        assert(i == next[id]++ && eol - p == 30);
        p = eol + 1;
    }
    assert(next[1] == 120 && next[2] == 120);
}

static void writers(void)
{
    fresh_model();
    void *f = fixture(GENERATED_FIXTURES, "usb-acm.dtb");
    assert(!kshim_console_init(f) && kshim_usb_acm_active());
    pthread_t a, b;
    assert(!pthread_create(&a, NULL, writer, (void *)(intptr_t)1));
    assert(!pthread_create(&b, NULL, writer, (void *)(intptr_t)2));
    assert(!pthread_join(a, NULL) && !pthread_join(b, NULL));
    check_lines(uart_out, uart_length);
    assert(AcmModelConnect() == ACM_MODEL_OK && AcmModelEnumerate(3) == ACM_MODEL_OK);
    assert(AcmModelOpenPort() == ACM_MODEL_OK);
    static char got[16384];
    size_t n = host_read(got, sizeof(got));
    assert(n == uart_length && !memcmp(got, uart_out, n));
    check_lines(got, n);
    assert(!kshim_console_quiesce() && AcmModelObserve()->Errors == 0);
    free(f);
    printf("writers: two CPUs' lines arrive whole and in order on both backends\n");
}

/*------------------------------------------- unmappable or absent core */
static void map_failure(void)
{
    fresh_model();
    void *f = fixture(GENERATED_FIXTURES, "usb-acm.dtb");
    assert(!kshim_console_init(f) && kshim_usb_acm_active());
    struct kshim_resources *r = calloc(1, sizeof(*r));
    map_fail = 1;
    assert(kshim_console_map_resources(r) && r->mapping_count == 0);
    const AcmModelState *s = AcmModelObserve();
    assert(!kshim_usb_acm_active() && !s->Running && s->EventAddress == 0);
    unsigned mmio = model_mmio;
    kshim_console_write("uart only\n", 10);
    kshim_console_service();
    assert(model_mmio == mmio && uart_length == 10);
    assert(!kshim_console_quiesce());
    free(r);
    free(f);

    fresh_model();
    f = fixture(GENERATED_FIXTURES, "usb-acm.dtb");
    assert(!fdt_del_node(f, fdt_path_offset(f, "/soc/usb@a8f8800")));
    assert(!fdt_del_node(f, fdt_path_offset(f, "/soc/usb@a6f8800")));
    assert(!kshim_console_init(f) && !kshim_usb_acm_active());
    r = calloc(1, sizeof(*r));
    assert(!kshim_console_map_resources(r) && map_calls == 0);
    kshim_console_write("serial\n", 7);
    kshim_console_service();
    char c;
    assert(!kshim_console_try_read(&c));
    assert(!kshim_console_quiesce() && model_mmio == 0 && uart_length == 7);
    free(r);
    free(f);
    printf("fallback: an unmappable core is stopped before the MMU; no DWC3 means UART only\n");
}

int main(void)
{
    discovery();
    mirror();
    writers();
    map_failure();
    AcmModelFree();
    printf("usb_acm_test: PASS\n");
    return 0;
}
