/* SPDX-License-Identifier: MIT */
/*
 * I/O stack core (docs/io-stack.md): status names, completions, and the
 * boot/handoff orchestration of the configured subsystems. Every call site
 * below is compiled only with its subsystem's Kconfig symbol, so with the
 * whole stack off these hooks do nothing.
 */
#include <kshim/io.h>
#include <kshim/irq.h>
#include <kshim/smp.h>
#include <kshim/time.h>
#include <config.h>
#include <stdio.h>
#include <string.h>

#ifndef KSHIM_HOST_TEST
#include <qcom_dt.h>
#endif
#if CONFIG_KSHIM_GPIO
#include <kshim/gpio.h>
#endif
#if CONFIG_KSHIM_CLK
#include <kshim/clk.h>
#endif
#if CONFIG_KSHIM_REGULATOR
#include <kshim/regulator.h>
#endif
#if CONFIG_KSHIM_QUP
#include <kshim/qup.h>
#endif
#if CONFIG_KSHIM_I2C
#include <kshim/i2c.h>
#endif
#if CONFIG_KSHIM_SPI
#include <kshim/spi.h>
#endif
#if CONFIG_KSHIM_PCI
#include <kshim/pci.h>
#endif
#if CONFIG_KSHIM_USB
#include <kshim/usb_hcd.h>
#endif
#if CONFIG_KSHIM_USB_HUB || CONFIG_KSHIM_USB_HID
#include <kshim/usb_class.h>
#endif
#if CONFIG_KSHIM_TOUCH_FRAMEWORK
#include <kshim/touch_driver.h>
#endif

#if defined(__aarch64__) && !defined(KSHIM_HOST_TEST)
/* MPIDR_EL1 affinity: Aff3 [39:32], Aff2 [23:16], Aff1 [15:8], Aff0 [7:0]. */
static uint64_t cpu_id(void)
{
    uint64_t mpidr;
    __asm__ volatile("mrs %0, mpidr_el1" : "=r"(mpidr));
    return mpidr & UINT64_C(0xff00ffffff);
}
#else
static uint64_t cpu_id(void) { return 0; }
#endif

/* Busy-wait step while interrupts are not live. */
#define IO_POLL_STEP_US 10U
/* Initial USB enumeration bound (root ports, hubs, their children). */
#define IO_USB_SETTLE_MS 1500U

static uint64_t BootCpu;
static bool BootCpuKnown;

const char *kshim_io_strerror(int status)
{
    switch (status) {
    case 0: return "ok";
    case ERROR_FW: return "firmware error";
    case ERROR_DEVICE_ERROR: return "device error";
    case ERROR_INVALID_PARAMETER: return "invalid parameter";
    case ERROR_NOT_SUPPORTED: return "not supported";
    case ERROR_NOT_FOUND: return "not found";
    case ERROR_TIMEOUT: return "timeout";
    case ERROR_BUSY: return "busy";
    case ERROR_NOT_READY: return "not ready";
    case KSHIM_IO_ERR_NACK: return "NACK";
    case KSHIM_IO_ERR_NO_MEMORY: return "out of memory";
    case KSHIM_IO_ERR_NO_DEVICE: return "no device";
    case KSHIM_IO_ERR_STALL: return "stall";
    case KSHIM_IO_ERR_SHORT: return "short transfer";
    case KSHIM_IO_ERR_CANCELLED: return "cancelled";
    case KSHIM_IO_ERR_OVERFLOW: return "overflow";
    case KSHIM_IO_ERR_PROTOCOL: return "protocol error";
    case KSHIM_IO_ERR_EXISTS: return "exists";
    case KSHIM_IO_ERR_QUIESCED: return "quiesced";
    case KSHIM_IO_ERR_PENDING: return "pending";
    default: return "unknown error";
    }
}

void kshim_completion_complete(struct kshim_completion *c, int status)
{
    if (!c) return;
    c->status = status;
    __atomic_store_n(&c->done, 1U, __ATOMIC_RELEASE);
}

int kshim_completion_wait(struct kshim_completion *c, uint32_t timeout_us,
                          kshim_io_poll_fn poll, void *context)
{
    if (!c) return ERROR_INVALID_PARAMETER;
    uint64_t start = kshim_smp_time_us();
    for (;;) {
        if (poll) poll(context);
        if (__atomic_load_n(&c->done, __ATOMIC_ACQUIRE)) return c->status;
        if (kshim_smp_time_us() - start >= timeout_us) return ERROR_TIMEOUT;
        /* A completing interrupt taken between the check above and the
         * sleep still ends kshim_wait_event() at once (it reports device
         * interrupts taken since the previous wait). */
        if (kshim_irq_ready()) kshim_wait_event(get_current_time() + 1);
        else (void)delay(IO_POLL_STEP_US);
    }
}

bool kshim_io_on_boot_cpu(void)
{
    return !BootCpuKnown || cpu_id() == BootCpu;
}

#ifndef KSHIM_HOST_TEST
/* One subsystem's mappings, rolled back when it fails so a broken optional
 * device never costs another subsystem its windows. */
typedef int (*io_map_fn)(struct kshim_resources *resources);
static void map_optional(struct kshim_resources *r, const char *name, io_map_fn map)
{
    static kshim_mmu_region_t saved[KSHIM_RESOURCE_MAX];
    size_t count = r->mapping_count;
    memcpy(saved, r->mappings, count * sizeof(*saved));
    int status = map(r);
    if (!status) return;
    memcpy(r->mappings, saved, count * sizeof(*saved));
    r->mapping_count = count;
    printf("I/O: %s resources unavailable (%d)\n", name, status);
}
#endif

int kshim_io_map_resources(struct kshim_resources *resources)
{
#ifndef KSHIM_HOST_TEST
    (void)map_optional;
    if (!resources) return 0;
#if CONFIG_KSHIM_GPIO
    map_optional(resources, "GPIO", kshim_gpio_map_resources);
#endif
#if CONFIG_KSHIM_CLK
    map_optional(resources, "clock", kshim_clk_map_resources);
#endif
#if CONFIG_KSHIM_REGULATOR
    map_optional(resources, "regulator", kshim_regulator_map_resources);
#endif
#if CONFIG_KSHIM_QUP
    map_optional(resources, "QUP", kshim_qup_map_resources);
#endif
#if CONFIG_KSHIM_PCI
    map_optional(resources, "PCI", kshim_pci_map_resources);
#endif
#else
    (void)resources;
#endif
    return 0;
}

static void io_step(const char *name, int status)
{
    if (status) printf("I/O: %s: %s (%d)\n", name, kshim_io_strerror(status), status);
}

int kshim_io_init(const void *fdt)
{
    (void)io_step;
    BootCpu = cpu_id();
    BootCpuKnown = true;
    if (!fdt) return 0;
#if CONFIG_KSHIM_GPIO
    io_step("GPIO", kshim_gpio_init(fdt));
#endif
#if CONFIG_KSHIM_CLK
    io_step("clock", kshim_clk_init(fdt));
#endif
#if CONFIG_KSHIM_REGULATOR
    io_step("regulator", kshim_regulator_init(fdt));
#endif
#if CONFIG_KSHIM_QUP
    io_step("QUP", kshim_qup_init(fdt));
#endif
#if CONFIG_KSHIM_USB
    /* Core, xHCI PCI driver and the input-source pump, then the class
     * drivers, all before PCI binding starts the controllers. */
    io_step("USB", kshim_usb_init(fdt));
#endif
#if CONFIG_KSHIM_USB_HUB
    io_step("USB hub", kshim_usb_hub_register());
#endif
#if CONFIG_KSHIM_USB_HID
    io_step("USB HID", kshim_usb_hid_register());
#endif
#if CONFIG_KSHIM_PCI
    io_step("PCI", kshim_pci_init(fdt));
#endif
#if CONFIG_KSHIM_USB
    io_step("USB enumeration", kshim_usb_settle(IO_USB_SETTLE_MS));
#endif
    return 0;
}

static void quiesce_step(int *result, const char *name, int status)
{
    if (!status) return;
    printf("I/O: %s quiesce failed: %s (%d)\n", name, kshim_io_strerror(status), status);
    if (!*result) *result = status;
}

int kshim_io_quiesce(void)
{
    int result = 0;
    (void)quiesce_step;
#if CONFIG_KSHIM_USB
    quiesce_step(&result, "USB", kshim_usb_quiesce());
    quiesce_step(&result, "USB host", kshim_usb_hcd_quiesce_all());
#endif
#if CONFIG_KSHIM_PCI
    quiesce_step(&result, "PCI", kshim_pci_quiesce());
#endif
#if CONFIG_KSHIM_TOUCH_FRAMEWORK
    quiesce_step(&result, "touch", kshim_touch_quiesce_all());
#endif
#if CONFIG_KSHIM_I2C
    quiesce_step(&result, "I2C", kshim_i2c_quiesce_all());
#endif
#if CONFIG_KSHIM_SPI
    quiesce_step(&result, "SPI", kshim_spi_quiesce_all());
#endif
#if CONFIG_KSHIM_QUP
    quiesce_step(&result, "QUP", kshim_qup_quiesce_all());
#endif
#if CONFIG_KSHIM_CLK
    quiesce_step(&result, "clock", kshim_clk_restore_all());
#endif
#if CONFIG_KSHIM_REGULATOR
    quiesce_step(&result, "regulator", kshim_regulator_restore_all());
#endif
#if CONFIG_KSHIM_GPIO
    quiesce_step(&result, "GPIO", kshim_gpio_restore_all());
#endif
    return result;
}
