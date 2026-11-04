/* SPDX-License-Identifier: MIT */
/*
 * Renesas uPD720201/uPD720202 firmware download against a configuration
 * register model (renesas_fw.c), and the xHCI PCI glue (xhci_pci.c) deciding
 * between "already running", "external ROM", "download" and "refuse",
 * with the firmware loader and the xHCI core faked.
 *
 * The model follows the register descriptions in renesas_fw.h: Set DATA0/1
 * are consumed in the order they were set, after a configurable number of
 * status reads; clearing FW Download Enable latches success only when the
 * consumed stream equals the image (zero-padded to a dword).
 */
#include "../../kernel/drivers/usb/host/renesas_fw.h"

#include <kshim/firmware.h>
#include <kshim/io.h>
#include <kshim/pci.h>
#include <kshim/time.h>

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- Register model ------------------------------------------------------ */

struct rmodel {
    uint32_t fw, rom, version;
    uint32_t data[2];
    uint8_t got[RENESAS_FW_MAX_SIZE + 4];
    size_t got_len;
    const uint8_t *expect;
    size_t expect_len;
    unsigned busy_reads;       /* status reads before the head request is consumed */
    unsigned countdown;
    uint8_t queue[2];          /* pending Set DATA requests (0 or 1), in set order */
    unsigned queued;
    bool stuck, fail_result;
    unsigned order_errors, rom_half_writes, delays, reads, writes;
};
static struct rmodel M;

static void consume_head(void)
{
    uint8_t which = M.queue[0];
    uint32_t v = M.data[which];
    for (unsigned i = 0; i < 4 && M.got_len < sizeof(M.got); ++i) M.got[M.got_len++] = (uint8_t)(v >> (8 * i));
    M.fw &= ~(which ? RENESAS_SET_DATA1 : RENESAS_SET_DATA0);
    M.queue[0] = M.queue[1];
    --M.queued;
    M.countdown = M.busy_reads;
}

static int m_read(void *context, uint16_t reg, uint32_t *value)
{
    (void)context;
    ++M.reads;
    switch (reg) {
    case RENESAS_FW_STATUS:
        if (M.queued && !M.stuck) {
            if (M.countdown) --M.countdown;
            else consume_head();
        }
        *value = (M.fw & 0xffffU) | (M.rom & 0xffffU) << 16;
        return 0;
    case RENESAS_FW_VERSION: *value = M.version; return 0;
    default: *value = 0; return 0;
    }
}

static int m_write(void *context, uint16_t reg, uint32_t value)
{
    (void)context;
    ++M.writes;
    if (reg == RENESAS_DATA0 || reg == RENESAS_DATA1) {
        unsigned which = reg == RENESAS_DATA1;
        for (unsigned i = 0; i < M.queued; ++i)
            if (M.queue[i] == which) ++M.order_errors; /* overwritten before consumed */
        if (!(M.fw & RENESAS_FW_ENABLE)) ++M.order_errors;
        M.data[which] = value;
        return 0;
    }
    if (reg != RENESAS_FW_STATUS) return 0;
    if (value >> 16) ++M.rom_half_writes;
    bool was = M.fw & RENESAS_FW_ENABLE, now = value & RENESAS_FW_ENABLE;
    M.fw = (M.fw & ~(RENESAS_FW_ENABLE | RENESAS_FW_LOCK)) | (value & (RENESAS_FW_ENABLE | RENESAS_FW_LOCK));
    for (unsigned which = 0; which < 2; ++which) {
        uint32_t bit = which ? RENESAS_SET_DATA1 : RENESAS_SET_DATA0;
        if (!(value & bit)) continue;
        if (!now || (M.fw & bit)) { ++M.order_errors; continue; }
        M.fw |= bit;
        M.queue[M.queued++] = (uint8_t)which;
        if (M.queued == 1) M.countdown = M.busy_reads;
    }
    if (was && !now) {
        size_t padded = (M.expect_len + 3U) & ~(size_t)3U;
        bool ok = !M.fail_result && M.got_len == padded && !memcmp(M.got, M.expect, M.expect_len);
        for (size_t i = M.expect_len; ok && i < padded; ++i) ok = M.got[i] == 0;
        M.fw = (M.fw & ~(7U << 4)) | (ok ? RENESAS_RESULT_SUCCESS : RENESAS_RESULT_ERROR) << 4;
    }
    return 0;
}

static void m_delay(void *context, uint32_t us)
{
    (void)context;
    (void)us;
    ++M.delays;
}

static void reset_model(uint32_t fw, uint32_t rom)
{
    memset(&M, 0, sizeof(M));
    M.fw = fw;
    M.rom = rom;
    M.version = 0x00202600;
}

static uint8_t *make_image(size_t size)
{
    uint8_t *image = malloc(size);
    assert(image);
    for (size_t i = 0; i < size; ++i) image[i] = (uint8_t)(i * 7U + 3U);
    image[0] = 0xaa;
    image[1] = 0x55;
    return image;
}

static const struct renesas_cfg Cfg = {NULL, m_read, m_write, m_delay};

static enum renesas_fw_state state_of(uint32_t fw, uint32_t rom)
{
    reset_model(fw, rom);
    enum renesas_fw_state state;
    uint32_t raw = 0;
    assert(renesas_fw_state(&Cfg, &state, &raw) == 0);
    assert(raw == ((fw & 0xffffU) | rom << 16));
    return state;
}

static void test_core(void)
{
    /* State decisions (inherit first). */
    assert(state_of(0, 0) == RENESAS_FW_NEEDED);
    assert(state_of(RENESAS_RESULT_SUCCESS << 4, 0) == RENESAS_FW_RUNNING);
    assert(state_of(RENESAS_FW_LOCK | RENESAS_RESULT_SUCCESS << 4, 0) == RENESAS_FW_RUNNING);
    assert(state_of(RENESAS_FW_LOCK, 0) == RENESAS_FW_FAILED);
    assert(state_of(RENESAS_FW_ENABLE, 0) == RENESAS_FW_FAILED);
    assert(state_of(RENESAS_RESULT_ERROR << 4, 0) == RENESAS_FW_FAILED);
    assert(state_of(0, RENESAS_ROM_EXISTS | RENESAS_RESULT_SUCCESS << 4) == RENESAS_FW_FROM_ROM);
    assert(state_of(0, RENESAS_ROM_EXISTS) == RENESAS_FW_NEEDED);
    /* Running firmware wins over the ROM state. */
    assert(state_of(RENESAS_RESULT_SUCCESS << 4, RENESAS_ROM_EXISTS) == RENESAS_FW_RUNNING);

    /* Image checks: size window and the AA 55 signature. */
    uint8_t *image = make_image(RENESAS_FW_MAX_SIZE);
    assert(renesas_fw_validate(image, RENESAS_FW_MIN_SIZE) == 0);
    assert(renesas_fw_validate(image, RENESAS_FW_MAX_SIZE) == 0);
    assert(renesas_fw_validate(image, RENESAS_FW_MIN_SIZE - 4) == KSHIM_IO_ERR_PROTOCOL);
    assert(renesas_fw_validate(image, RENESAS_FW_MAX_SIZE + 4) == KSHIM_IO_ERR_PROTOCOL);
    image[1] = 0x56;
    assert(renesas_fw_validate(image, 0x2000) == KSHIM_IO_ERR_PROTOCOL);
    image[1] = 0x55;

    /* Full download with handshake delays and an odd, unaligned length:
     * the stream alternates DATA0/DATA1 and the tail is zero-padded. */
    size_t size = 0x1000 + 6;
    reset_model(0, 0);
    M.busy_reads = 3;
    M.expect = image;
    M.expect_len = size;
    assert(renesas_fw_download(&Cfg, image, size) == 0);
    assert(M.got_len == ((size + 3) & ~(size_t)3) && !memcmp(M.got, image, size));
    assert(!M.order_errors && !M.rom_half_writes && M.delays > 0);
    assert(!(M.fw & RENESAS_FW_ENABLE) && RENESAS_RESULT(M.fw) == RENESAS_RESULT_SUCCESS);
    enum renesas_fw_state state;
    assert(renesas_fw_state(&Cfg, &state, NULL) == 0 && state == RENESAS_FW_RUNNING);

    /* Largest image, no handshake delay. */
    reset_model(0, 0);
    M.expect = image;
    M.expect_len = RENESAS_FW_MAX_SIZE;
    assert(renesas_fw_download(&Cfg, image, RENESAS_FW_MAX_SIZE) == 0 && !M.order_errors);

    /* The controller never consumes: bounded wait, download aborted. */
    reset_model(0, 0);
    M.stuck = true;
    M.expect = image;
    M.expect_len = 0x1000;
    assert(renesas_fw_download(&Cfg, image, 0x1000) == ERROR_TIMEOUT);
    assert(!(M.fw & RENESAS_FW_ENABLE));

    /* The controller rejects the image. */
    reset_model(0, 0);
    M.fail_result = true;
    M.expect = image;
    M.expect_len = 0x1000;
    assert(renesas_fw_download(&Cfg, image, 0x1000) == ERROR_FW);

    /* Locked, a download in progress, or ROM access active: refused. */
    reset_model(RENESAS_FW_LOCK, 0);
    assert(renesas_fw_download(&Cfg, image, 0x1000) == ERROR_BUSY && M.writes == 0);
    reset_model(RENESAS_FW_ENABLE, 0);
    assert(renesas_fw_download(&Cfg, image, 0x1000) == ERROR_BUSY && M.writes == 0);
    reset_model(0, RENESAS_ROM_ACCESS);
    assert(renesas_fw_download(&Cfg, image, 0x1000) == ERROR_BUSY && M.writes == 0);
    free(image);
}

/* ---- xhci_pci.c with fakes ------------------------------------------------- */

static struct kshim_pci_driver *Driver;
static unsigned JournaledWrites, RawWrites, StartCalls, FwRequests, IrqRequests;
static int FwStatus, IrqStatus;
static bool StartIrq;
static uint8_t *FwImage;
static size_t FwSize;
static char FwName[64];
static size_t FwBufferSize, FwMaxSize;
static uint32_t FakeBar[64];

int kshim_pci_driver_register(struct kshim_pci_driver *driver) { Driver = driver; return 0; }
int kshim_pci_read_config32(struct kshim_pci_dev *dev, uint16_t reg, uint32_t *value)
{
    (void)dev;
    return m_read(NULL, reg, value);
}
int kshim_pci_write_config32(struct kshim_pci_dev *dev, uint16_t reg, uint32_t value)
{
    (void)dev; (void)reg; (void)value;
    ++JournaledWrites;
    return 0;
}
int kshim_pci_write_config_raw32(struct kshim_pci_dev *dev, uint16_t reg, uint32_t value)
{
    (void)dev;
    ++RawWrites;
    return m_write(NULL, reg, value);
}
volatile void *kshim_pci_bar_map(struct kshim_pci_dev *dev, unsigned bar)
{
    (void)dev;
    return bar == 0 ? (volatile void *)FakeBar : NULL;
}
uint64_t kshim_pci_dma_addr(const struct kshim_pci_dev *dev, uint64_t bus) { (void)dev; return bus; }
int kshim_pci_enable_device(struct kshim_pci_dev *dev, bool bus_master)
{
    (void)bus_master;
    dev->enabled = true;
    return 0;
}
int kshim_pci_request_irq(struct kshim_pci_dev *dev, unsigned allowed, kshim_irq_handler_t handler,
                          void *context, uint32_t *intid)
{
    (void)dev; (void)handler; (void)context;
    ++IrqRequests;
    assert(allowed & KSHIM_PCI_IRQ_INTX);
    if (!IrqStatus && intid) *intid = 434;
    return IrqStatus;
}
int kshim_pci_free_irq(struct kshim_pci_dev *dev) { (void)dev; return 0; }

int kshim_firmware_request(const char *name, const struct kshim_fw_request *req, struct kshim_firmware *fw)
{
    ++FwRequests;
    snprintf(FwName, sizeof(FwName), "%s", name);
    FwBufferSize = req->buffer_size;
    FwMaxSize = req->max_size;
    assert(req->kind == KSHIM_FW_FILE && req->buffer && !req->locations);
    if (FwStatus) return FwStatus;
    assert(FwSize <= req->buffer_size);
    memcpy(req->buffer, FwImage, FwSize);
    fw->data = req->buffer;
    fw->size = FwSize;
    fw->origin = KSHIM_FW_ORIGIN_ANDROID;
    snprintf(fw->name, sizeof(fw->name), "vendor/firmware/%s", name);
    return 0;
}
void kshim_firmware_release(struct kshim_firmware *fw) { memset(fw, 0, sizeof(*fw)); }
int kshim_firmware_builtin_register(struct kshim_fw_builtin *entry) { (void)entry; return 0; }

uint64_t delay(uint64_t us) { return us; }
/* kernel/core/io.c (status names, completions) needs a clock. */
uint64_t kshim_smp_time_us(void) { return 0; }
bool kshim_irq_ready(void) { return false; }
uint64_t get_current_time(void) { return 0; }
void kshim_wait_event(uint64_t deadline_ms) { (void)deadline_ms; }

/* xHCI core fakes: the glue only decides whether and how to start it. */
#include "../../kernel/drivers/usb/host/xhci.h"
void xhci_io_mmio(struct xhci_io *io) { memset(io, 0, sizeof(*io)); }
int xhci_start(struct xhci *x, const char *name, uintptr_t base, const struct xhci_io *io,
               uint64_t dma_limit, bool irq)
{
    (void)x; (void)name; (void)io; (void)dma_limit;
    assert(base == (uintptr_t)FakeBar);
    ++StartCalls;
    StartIrq = irq;
    return 0;
}
int xhci_stop(struct xhci *x) { (void)x; return 0; }
bool xhci_irq(struct xhci *x) { (void)x; return false; }

static const struct kshim_pci_device_id *match(const struct kshim_pci_dev *dev)
{
    for (const struct kshim_pci_device_id *id = Driver->ids; id->vendor || id->device || id->class_mask; ++id) {
        if (id->vendor != KSHIM_PCI_ANY_ID && id->vendor != dev->vendor) continue;
        if (id->device != KSHIM_PCI_ANY_ID && id->device != dev->device) continue;
        if (id->class_mask && (dev->class_code & id->class_mask) != (id->class_code & id->class_mask)) continue;
        return id;
    }
    return NULL;
}

static int probe(uint16_t vendor, uint16_t device, uint32_t class_code)
{
    static struct kshim_pci_dev devs[8];
    static unsigned used;
    struct kshim_pci_dev *dev = &devs[used++ % 8];
    memset(dev, 0, sizeof(*dev));
    dev->vendor = vendor;
    dev->device = device;
    dev->class_code = class_code;
    dev->bus = 1;
    const struct kshim_pci_device_id *id = match(dev);
    assert(id);
    StartCalls = FwRequests = RawWrites = JournaledWrites = IrqRequests = 0;
    return Driver->probe(dev, id);
}

#include <kshim/dma.h>
#include <kshim/usb_hcd.h>

static void test_glue(void)
{
    assert(kshim_xhci_pci_register() == 0 && Driver && Driver->probe && Driver->quiesce);
    /* The ID table: Renesas 0014/0015, NEC 0194, and any xHCI by class. */
    struct kshim_pci_dev probe_dev = {.vendor = 0x1912, .device = 0x0015, .class_code = 0x0c0330};
    assert(match(&probe_dev) && match(&probe_dev)->data);
    probe_dev = (struct kshim_pci_dev){.vendor = 0x1033, .device = 0x0194, .class_code = 0x0c0330};
    assert(match(&probe_dev) && !match(&probe_dev)->data);
    probe_dev = (struct kshim_pci_dev){.vendor = 0x8086, .device = 0x1234, .class_code = 0x0c0330};
    assert(match(&probe_dev));
    probe_dev = (struct kshim_pci_dev){.vendor = 0x8086, .device = 0x1234, .class_code = 0x0c0320};
    assert(!match(&probe_dev)); /* EHCI is not ours */

    FwImage = make_image(0x3000);
    FwSize = 0x3000;

    /* Firmware needed: loaded through the firmware loader into pool
     * scratch, downloaded with raw (never journaled) config writes, then
     * the controller starts; the scratch goes back to the pool. */
    reset_model(0, 0);
    M.expect = FwImage;
    M.expect_len = FwSize;
    M.busy_reads = 1;
    IrqStatus = 0;
    size_t before = kshim_dma_in_use();
    assert(probe(0x1912, 0x0014, 0x0c0330) == 0);
    assert(FwRequests == 1 && !strcmp(FwName, "renesas_usb_fw.mem"));
    assert(FwBufferSize == RENESAS_FW_MAX_SIZE && FwMaxSize == RENESAS_FW_MAX_SIZE);
    assert(RawWrites > FwSize / 4 && JournaledWrites == 0 && !M.order_errors);
    assert(RENESAS_RESULT(M.fw) == RENESAS_RESULT_SUCCESS);
    assert(StartCalls == 1 && StartIrq && IrqRequests == 1);
    assert(kshim_dma_in_use() == before);

    /* Already running (UEFI XhciDxe downloaded it): inherited, no load. */
    reset_model(RENESAS_RESULT_SUCCESS << 4, 0);
    IrqStatus = ERROR_NOT_SUPPORTED; /* no interrupt: polled start */
    assert(probe(0x1912, 0x0015, 0x0c0330) == 0);
    assert(FwRequests == 0 && RawWrites == 0 && StartCalls == 1 && !StartIrq);

    /* External ROM loaded it. */
    reset_model(0, RENESAS_ROM_EXISTS | RENESAS_RESULT_SUCCESS << 4);
    assert(probe(0x1912, 0x0014, 0x0c0330) == 0 && FwRequests == 0 && RawWrites == 0);

    /* Stale download state: refused without touching it. */
    reset_model(RENESAS_FW_ENABLE, 0);
    assert(probe(0x1912, 0x0014, 0x0c0330) == ERROR_DEVICE_ERROR && StartCalls == 0 && RawWrites == 0);

    /* Firmware absent from the partitions: the controller stays off. */
    reset_model(0, 0);
    FwStatus = ERROR_NOT_FOUND;
    assert(probe(0x1912, 0x0014, 0x0c0330) == ERROR_NOT_FOUND && StartCalls == 0 && RawWrites == 0);
    assert(kshim_dma_in_use() == before);
    FwStatus = 0;

    /* Any other xHCI: no Renesas register access at all. */
    reset_model(0, 0);
    unsigned reads = M.reads;
    assert(probe(0x1b36, 0x000d, 0x0c0330) == 0 && M.reads == reads && StartCalls == 1);
    free(FwImage);
}

int main(void)
{
    test_core();
    test_glue();
    puts("usb_renesas_test: ok");
    return 0;
}
