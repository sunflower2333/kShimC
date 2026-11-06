/* SPDX-License-Identifier: MIT */
/* Host fakes for the touch framework's dependencies; see touch_fakes.h. */
#include "touch_fakes.h"

#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include <dt.h>
#include <kshim/time.h>

/* ---- Log ------------------------------------------------------------------ */

char FakeLog[16384];

void fake_log(const char *format, ...)
{
    size_t used = strlen(FakeLog);
    assert(used + 2 < sizeof(FakeLog));
    va_list args;
    va_start(args, format);
    vsnprintf(FakeLog + used, sizeof(FakeLog) - used - 1, format, args);
    va_end(args);
    used = strlen(FakeLog);
    FakeLog[used] = ' ';
    FakeLog[used + 1] = 0;
}

void fake_log_clear(void) { FakeLog[0] = 0; }

bool fake_log_has(const char *needle) { return strstr(FakeLog, needle) != NULL; }

bool fake_log_order(const char *const *needles)
{
    const char *at = FakeLog;
    for (; *needles; ++needles) {
        const char *hit = strstr(at, *needles);
        if (!hit) {
            fprintf(stderr, "log order: missing '%s' after offset %zu\nlog: %s\n", *needles,
                    (size_t)(at - FakeLog), FakeLog);
            return false;
        }
        at = hit + strlen(*needles);
    }
    return true;
}

unsigned fake_log_count(const char *needle)
{
    unsigned n = 0;
    for (const char *at = strstr(FakeLog, needle); at; at = strstr(at + 1, needle)) ++n;
    return n;
}

/* ---- Time and core services --------------------------------------------- */

#if TOUCH_FAKE_TLMM
static uint64_t NowUs;
uint64_t fake_now_us(void) { return NowUs; }
void fake_advance_us(uint64_t us) { NowUs += us; }
uint64_t get_current_time(void) { return NowUs / 1000U; }
bool FakeIrqReady;
bool kshim_irq_ready(void) { return FakeIrqReady; }
#else
#include "../irq_fake.h"
uint64_t fake_now_us(void) { return FakeNowUs; }
void fake_advance_us(uint64_t us) { FakeNowUs += us; }
#endif

uint64_t delay(uint64_t us)
{
    fake_log("delay=%llu", (unsigned long long)us);
    fake_advance_us(us);
    return us;
}

bool kshim_io_on_boot_cpu(void) { return true; }

const char *kshim_io_strerror(int status)
{
    static char text[32];
    snprintf(text, sizeof(text), "status %d", status);
    return text;
}

#if !TOUCH_REAL_PLATFORM
/* With TOUCH_REAL_PLATFORM the real GPIO/pinctrl and regulator drivers
 * run over the platform register models instead (touch_goodix_platform_test). */
/* ---- GPIO ----------------------------------------------------------------- */

static struct fake_pin Pins[32];
void (*FakePinHook)(uint32_t pin, bool level);
bool (*FakePinRead)(const void *fdt, const struct kshim_gpio *gpio, bool *level);
int FakeGpioFailRequest = -1;
static const void *GpioFdt;

struct fake_pin *fake_pin(uint32_t pin)
{
    for (unsigned i = 0; i < sizeof(Pins) / sizeof(Pins[0]); ++i)
        if (Pins[i].used && Pins[i].pin == pin) return &Pins[i];
    for (unsigned i = 0; i < sizeof(Pins) / sizeof(Pins[0]); ++i)
        if (!Pins[i].used) {
            Pins[i] = (struct fake_pin){.used = true, .pin = pin, .in_level = true};
            return &Pins[i];
        }
    assert(!"out of fake pins");
    return NULL;
}

void fake_pin_input(uint32_t pin, bool level) { fake_pin(pin)->in_level = level; }

bool fake_pin_level(uint32_t pin)
{
    struct fake_pin *p = fake_pin(pin);
    return p->output ? p->out_level : p->in_level;
}

static bool is_tlmm(const void *f, int provider)
{
    int len;
    const char *c = fdt_getprop(f, provider, "compatible", &len);
    return c && (strstr(c, "pinctrl") || strstr(c, "tlmm"));
}

int kshim_gpio_from_dt(const void *fdt, int node, const char *property, unsigned index,
                       struct kshim_gpio *gpio)
{
    struct dt_spec s;
    if (!fdt_getprop(fdt, node, property, NULL)) return ERROR_NOT_FOUND;
    if (dt_specifier(fdt, node, property, "#gpio-cells", index, &s) || s.count != 2)
        return ERROR_INVALID_PARAMETER;
    if (!is_tlmm(fdt, s.node)) return ERROR_NOT_SUPPORTED;
    GpioFdt = fdt;
    *gpio = (struct kshim_gpio){.chip = KSHIM_GPIO_CHIP_TLMM, .provider = s.node, .pin = s.args[0],
                                .dt_flags = s.args[1], .active_low = (s.args[1] & 1U) != 0,
                                .slot = -1};
    return 0;
}

int kshim_gpio_from_tlmm_pin(const void *fdt, int tlmm, uint32_t pin, bool active_low,
                             struct kshim_gpio *gpio)
{
    if (!is_tlmm(fdt, tlmm)) return ERROR_NOT_SUPPORTED;
    GpioFdt = fdt;
    *gpio = (struct kshim_gpio){.chip = KSHIM_GPIO_CHIP_TLMM, .provider = tlmm, .pin = pin,
                                .active_low = active_low, .slot = -1};
    return 0;
}

int kshim_gpio_request(struct kshim_gpio *gpio, const char *owner)
{
    assert(gpio && owner && gpio->chip == KSHIM_GPIO_CHIP_TLMM);
    if ((int)gpio->pin == FakeGpioFailRequest) return ERROR_BUSY;
    struct fake_pin *p = fake_pin(gpio->pin);
    if (!p->refs++) { p->snapshot_output = p->output; p->snapshot_level = p->out_level; }
    gpio->slot = (int)(p - Pins);
    fake_log("req%u", (unsigned)gpio->pin);
    return 0;
}

static void drive(struct fake_pin *p, bool level)
{
    bool changed = !p->output || p->out_level != level;
    p->output = true;
    p->out_level = level;
    if (changed && FakePinHook) FakePinHook(p->pin, level);
}

int kshim_gpio_direction_output(struct kshim_gpio *gpio, bool active)
{
    struct fake_pin *p = fake_pin(gpio->pin);
    assert(p->refs > 0);
    bool level = active != gpio->active_low;
    fake_log("gpio%u=%d", (unsigned)gpio->pin, level);
    drive(p, level);
    return 0;
}

int kshim_gpio_direction_input(struct kshim_gpio *gpio, enum kshim_gpio_pull pull)
{
    struct fake_pin *p = fake_pin(gpio->pin);
    assert(p->refs > 0);
    p->output = false;
    fake_log("gpio%u=in%s", (unsigned)gpio->pin, pull == KSHIM_GPIO_PULL_KEEP ? "" : "+pull");
    return 0;
}

int kshim_gpio_set(struct kshim_gpio *gpio, bool active)
{
    return kshim_gpio_direction_output(gpio, active);
}

int kshim_gpio_get(const struct kshim_gpio *gpio, bool *active)
{
    bool level;
    if (FakePinRead) {
        if (FakePinRead(GpioFdt, gpio, &level)) return ERROR_DEVICE_ERROR;
    } else {
        level = fake_pin_level(gpio->pin);
    }
    *active = level != gpio->active_low;
    return 0;
}

int kshim_gpio_free(struct kshim_gpio *gpio)
{
    struct fake_pin *p = fake_pin(gpio->pin);
    assert(p->refs > 0);
    fake_log("free%u", (unsigned)gpio->pin);
    if (!--p->refs) {
        /* Registry restore: the snapshot taken at the first request. */
        if (p->snapshot_output) drive(p, p->snapshot_level);
        else p->output = false;
    }
    gpio->slot = -1;
    return 0;
}

int kshim_gpio_tlmm_pin(const struct kshim_gpio *gpio, uint32_t *pin)
{
    if (gpio->chip != KSHIM_GPIO_CHIP_TLMM) return ERROR_NOT_SUPPORTED;
    *pin = gpio->pin;
    return 0;
}

int kshim_pinctrl_apply(const void *fdt, int node, const char *name)
{
    int index = 0;
    if (fdt_getprop(fdt, node, "pinctrl-names", NULL)) {
        index = dt_string_index(fdt, node, "pinctrl-names", name);
        if (index < 0) return ERROR_NOT_FOUND;
    } else if (strcmp(name, "default")) {
        return ERROR_NOT_FOUND;
    }
    char property[32];
    snprintf(property, sizeof(property), "pinctrl-%d", index);
    if (!fdt_getprop(fdt, node, property, NULL)) return ERROR_NOT_FOUND;
    fake_log("pinctrl=%s", name);
    return 0;
}

/* ---- Regulators ----------------------------------------------------------- */

struct kshim_regulator { struct fake_regulator r; };
static struct kshim_regulator Regulators[16];
const char *FakeRegulatorFail;

struct fake_regulator *fake_regulator_named(const char *name)
{
    for (unsigned i = 0; i < sizeof(Regulators) / sizeof(Regulators[0]); ++i)
        if (Regulators[i].r.used && !strcmp(Regulators[i].r.name, name)) return &Regulators[i].r;
    return NULL;
}

int kshim_regulator_get(const void *fdt, int consumer, const char *supply,
                        struct kshim_regulator **regulator)
{
    char property[64];
    snprintf(property, sizeof(property), "%s-supply", supply);
    if (!fdt_getprop(fdt, consumer, property, NULL)) return ERROR_NOT_FOUND;
    int node = dt_reference(fdt, consumer, property, 0);
    if (node < 0) return ERROR_INVALID_PARAMETER;
    if (!fdt_node_check_compatible(fdt, node, "test,unsupported")) return ERROR_NOT_SUPPORTED;
    unsigned free_slot = sizeof(Regulators) / sizeof(Regulators[0]);
    for (unsigned i = 0; i < sizeof(Regulators) / sizeof(Regulators[0]); ++i) {
        if (Regulators[i].r.used && Regulators[i].r.node == node) { *regulator = &Regulators[i]; return 0; }
        if (!Regulators[i].r.used && free_slot == sizeof(Regulators) / sizeof(Regulators[0])) free_slot = i;
    }
    assert(free_slot < sizeof(Regulators) / sizeof(Regulators[0]));
    struct fake_regulator *r = &Regulators[free_slot].r;
    *r = (struct fake_regulator){.used = true, .node = node};
    const char *name = dt_string(fdt, node, "regulator-name");
    snprintf(r->name, sizeof(r->name), "%s", name ? name : fdt_get_name(fdt, node, NULL));
    *regulator = &Regulators[free_slot];
    return 0;
}

int kshim_regulator_enable(struct kshim_regulator *regulator)
{
    struct fake_regulator *r = &regulator->r;
    if (FakeRegulatorFail && !strcmp(FakeRegulatorFail, r->name)) {
        fake_log("%s=fail", r->name);
        return ERROR_TIMEOUT;
    }
    ++r->enables;
    ++r->refs;
    fake_log("%s=on", r->name);
    return 0;
}

int kshim_regulator_disable(struct kshim_regulator *regulator)
{
    struct fake_regulator *r = &regulator->r;
    assert(r->refs > 0);
    --r->refs;
    ++r->disables;
    fake_log("%s=off", r->name);
    return 0;
}

int kshim_regulator_set_load(struct kshim_regulator *regulator, uint32_t load_ua)
{
    struct fake_regulator *r = &regulator->r;
    if (load_ua > r->load_ua) r->load_ua = load_ua;
    fake_log("%s=load%u", r->name, (unsigned)load_ua);
    return 0;
}

bool kshim_regulator_is_enabled(const struct kshim_regulator *regulator) { return regulator->r.refs > 0; }
const char *kshim_regulator_name(const struct kshim_regulator *regulator) { return regulator->r.name; }

#endif /* !TOUCH_REAL_PLATFORM */

/* ---- I2C ------------------------------------------------------------------ */

fake_i2c_fn FakeI2cDevice;
int FakeI2cGetStatus;
unsigned FakeI2cProbes, FakeI2cTransfers;
bool FakeBusForbidden;
static struct kshim_i2c_adapter Adapters[4];
static unsigned AdapterCount;

int kshim_i2c_adapter_get(const void *fdt, int node, struct kshim_i2c_adapter **adapter)
{
    if ((fdt_node_check_compatible(fdt, node, "qcom,geni-i2c") &&
         fdt_node_check_compatible(fdt, node, "qcom,i2c-geni")) || !dt_available(fdt, node))
        return ERROR_NOT_FOUND;
    if (FakeI2cGetStatus) return FakeI2cGetStatus;
    for (unsigned i = 0; i < AdapterCount; ++i)
        if (Adapters[i].node == node) { *adapter = &Adapters[i]; return 0; }
    assert(AdapterCount < sizeof(Adapters) / sizeof(Adapters[0]));
    ++FakeI2cProbes;
    fake_log("i2c-probe=%s", fdt_get_name(fdt, node, NULL));
    Adapters[AdapterCount] = (struct kshim_i2c_adapter){.name = "fake-i2c", .node = node, .bus_hz = 400000};
    *adapter = &Adapters[AdapterCount++];
    return 0;
}

int kshim_i2c_client_from_node(const void *fdt, int node, struct kshim_i2c_client *client)
{
    struct kshim_i2c_adapter *adapter;
    uint32_t reg;
    int status = kshim_i2c_adapter_get(fdt, fdt_parent_offset(fdt, node), &adapter);
    if (status) return status;
    if (dt_u32(fdt, node, "reg", &reg) || reg > 0x7f) return ERROR_INVALID_PARAMETER;
    *client = (struct kshim_i2c_client){.adapter = adapter, .node = node, .addr = (uint16_t)reg};
    return 0;
}

static int i2c_device(const struct kshim_i2c_client *client, const void *tx, size_t tx_length,
                      void *rx, size_t rx_length)
{
    assert(!FakeBusForbidden);
    ++FakeI2cTransfers;
    if (!FakeI2cDevice) return KSHIM_IO_ERR_NACK;
    return FakeI2cDevice(client->addr, tx, tx_length, rx, rx_length);
}

int kshim_i2c_write(const struct kshim_i2c_client *client, const void *data, size_t length,
                    uint32_t timeout_us)
{
    (void)timeout_us;
    return i2c_device(client, data, length, NULL, 0);
}

int kshim_i2c_write_read(const struct kshim_i2c_client *client, const void *tx, size_t tx_length,
                         void *rx, size_t rx_length, uint32_t timeout_us)
{
    (void)timeout_us;
    return i2c_device(client, tx, tx_length, rx, rx_length);
}

/* ---- SPI ------------------------------------------------------------------ */

fake_spi_fn FakeSpiDevice;
static struct kshim_spi_controller Controller;

int kshim_spi_controller_get(const void *fdt, int node, struct kshim_spi_controller **controller)
{
    if ((fdt_node_check_compatible(fdt, node, "qcom,geni-spi") &&
         fdt_node_check_compatible(fdt, node, "qcom,spi-geni")) || !dt_available(fdt, node))
        return ERROR_NOT_FOUND;
    Controller = (struct kshim_spi_controller){.name = "fake-spi", .node = node, .num_chipselect = 4};
    *controller = &Controller;
    fake_log("spi-probe=%s", fdt_get_name(fdt, node, NULL));
    return 0;
}

int kshim_spi_device_from_node(const void *fdt, int node, struct kshim_spi_device *device)
{
    uint32_t reg, hz;
    if (dt_u32(fdt, node, "reg", &reg) || dt_u32(fdt, node, "spi-max-frequency", &hz))
        return ERROR_INVALID_PARAMETER;
    *device = (struct kshim_spi_device){.controller = &Controller, .node = node,
                                        .chip_select = (uint8_t)reg, .bits_per_word = 8,
                                        .max_speed_hz = hz};
    return 0;
}

int kshim_spi_sync(const struct kshim_spi_device *device, const struct kshim_spi_xfer *xfers,
                   size_t count, uint32_t timeout_us)
{
    (void)timeout_us;
    assert(!FakeBusForbidden);
    return FakeSpiDevice ? FakeSpiDevice(device, xfers, count) : ERROR_DEVICE_ERROR;
}

/* ---- TLMM interrupt -------------------------------------------------------- */

#if TOUCH_FAKE_TLMM
struct fake_tlmm_irq FakeTlmmIrq;

int kshim_tlmm_irq_init(const void *fdt) { (void)fdt; return 0; }

int kshim_gpio_irq_request(uint32_t pin, unsigned trigger, kshim_gpio_irq_handler_t handler,
                           void *context)
{
    struct fake_tlmm_irq *q = &FakeTlmmIrq;
    if (q->fail) return q->fail;
    assert(!q->requested && handler);
    q->requested = true; q->masked = false;
    q->pin = pin; q->trigger = trigger; q->handler = handler; q->context = context;
    ++q->requests;
    fake_log("irq-req%u/%u", (unsigned)pin, trigger);
    return 0;
}

int kshim_gpio_irq_free(uint32_t pin)
{
    struct fake_tlmm_irq *q = &FakeTlmmIrq;
    if (!q->requested || q->pin != pin) return ERROR_NOT_FOUND;
    q->requested = false;
    ++q->frees;
    fake_log("irq-free%u", (unsigned)pin);
    return 0;
}

void kshim_gpio_irq_mask(uint32_t pin, bool masked)
{
    struct fake_tlmm_irq *q = &FakeTlmmIrq;
    assert(q->requested && q->pin == pin);
    q->masked = masked;
    if (masked) ++q->masks; else ++q->unmasks;
}

void (*FakeIrqLiveHook)(void);

bool kshim_gpio_irq_live(uint32_t pin)
{
    if (FakeIrqLiveHook) FakeIrqLiveHook();
    struct fake_tlmm_irq *q = &FakeTlmmIrq;
    return q->requested && q->pin == pin && !q->masked && !q->sealed;
}

bool fake_irq_fire(void)
{
    struct fake_tlmm_irq *q = &FakeTlmmIrq;
    if (!q->requested || q->masked || q->sealed) return false;
    FakeBusForbidden = true;
    q->handler(q->pin, q->context);
    FakeBusForbidden = false;
    ++q->delivered;
    return true;
}
#endif

void fake_touch_reset(void)
{
    fake_log_clear();
#if !TOUCH_REAL_PLATFORM
    memset(Pins, 0, sizeof(Pins));
    memset(Regulators, 0, sizeof(Regulators));
#endif
    memset(Adapters, 0, sizeof(Adapters));
    AdapterCount = 0;
#if !TOUCH_REAL_PLATFORM
    FakePinHook = NULL;
    FakePinRead = NULL;
    FakeGpioFailRequest = -1;
    FakeRegulatorFail = NULL;
#endif
    FakeI2cDevice = NULL;
    FakeI2cGetStatus = 0;
    FakeI2cProbes = FakeI2cTransfers = 0;
    FakeBusForbidden = false;
    FakeSpiDevice = NULL;
#if TOUCH_FAKE_TLMM
    memset(&FakeTlmmIrq, 0, sizeof(FakeTlmmIrq));
    FakeIrqLiveHook = NULL;
    FakeIrqReady = true;
#endif
}
