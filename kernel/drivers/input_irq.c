/* SPDX-License-Identifier: MIT */
/*
 * Interrupt wiring for Qualcomm keys (CONFIG_KSHIM_INPUT_IRQ). Touch
 * controllers take their TLMM interrupt through the touch framework
 * (kernel/drivers/input/touch/core.c). Key levels are sampled by the handlers through
 * the same QcomKeys read path as the poller (SPMI RT_STS, TLMM GPIO_IN), so
 * polarity and gen3 bit selection are unchanged.
 */
#include <input_irq.h>
#include <qcom_irq.h>
#include <dt.h>
#include <string.h>
#if !defined(KSHIM_HOST_TEST)
#include <config.h>
#endif

struct key_irq {
    QcomKeys *keys;
    uint8_t index;
    bool spmi, gpio;
    unsigned controller;
    uint8_t sid, peripheral, irq;
    uint32_t pin;
};
static struct key_irq KeyIrqs[QCOM_KEYS_MAX];

#if CONFIG_QCOM_SPMI
static void key_spmi_irq(void *context)
{
    struct key_irq *k = context;
    QcomKeysIrqSample(k->keys, k->index);
}
#endif

static void key_gpio_irq(uint32_t pin, void *context)
{
    (void)pin;
    struct key_irq *k = context;
    QcomKeysIrqSample(k->keys, k->index);
}

static int key_alive(void *context, size_t index)
{
    struct key_irq *k = &((struct key_irq *)context)[index];
#if CONFIG_QCOM_SPMI
    if (k->spmi) return kshim_spmi_irq_live(k->controller);
#endif
    if (k->gpio) return kshim_gpio_irq_live(k->pin);
    return 0;
}

static unsigned ctz32(uint32_t v)
{
    unsigned n = 0;
    while (!(v & 1U)) { v >>= 1; ++n; }
    return n;
}

/* debounce-interval of the gpio-keys child whose `gpios` is this TLMM pin. */
static uint32_t gpio_key_debounce(const void *fdt, uint32_t pin)
{
    for (int parent = dt_node_by_compatible(fdt, -1, "gpio-keys"); parent >= 0;
         parent = dt_node_by_compatible(fdt, parent, "gpio-keys")) {
        int child;
        fdt_for_each_subnode(child, fdt, parent) {
            struct dt_spec spec;
            uint32_t ms;
            if (dt_specifier(fdt, child, "gpios", "#gpio-cells", 0, &spec) || spec.count < 1 ||
                spec.args[0] != pin || kshim_dt_irq_kind(fdt, spec.node) != KSHIM_DT_IRQ_TLMM) continue;
            if (!dt_u32(fdt, child, "debounce-interval", &ms) && ms <= 1000U) return ms;
        }
    }
    return KSHIM_KEYS_IRQ_DEBOUNCE_MS;
}

static int key_request(QcomKeys *keys, const void *fdt, size_t i)
{
    const QcomKeyDescriptor *d = &keys->Descriptors[i];
    struct key_irq *k = &KeyIrqs[i];
    /* One status bit per interrupt: both PON and PMIC GPIO keys read the
     * peripheral's RT_STS, whose bit n is QPNPINT interrupt n. */
    if (d->Mask == 0U || (d->Mask & (d->Mask - 1U))) return ERROR_NOT_SUPPORTED;
    *k = (struct key_irq){.keys = keys, .index = (uint8_t)i};
    if (QCOM_KEY_IS_SPMI_TOKEN(d->Base)) {
#if !CONFIG_QCOM_SPMI
        return ERROR_NOT_SUPPORTED;
#else
        uint32_t address = QCOM_KEY_SPMI_TOKEN_ADDRESS(d->Base) + d->RegisterOffset;
        unsigned trigger;
        if ((address & 0xffU) != QCOM_PON_RT_STS_OFFSET || address > UINT16_MAX ||
            d->Mask > 0xffU) return ERROR_NOT_SUPPORTED;
        k->controller = QCOM_KEY_SPMI_TOKEN_CONTROLLER(d->Base);
        k->sid = QCOM_KEY_SPMI_TOKEN_SID(d->Base);
        k->peripheral = (uint8_t)(address >> 8);
        k->irq = (uint8_t)ctz32(d->Mask);
        /* The DT must describe this PMIC interrupt (PON "kpdpwr"/"resin",
         * PMIC GPIO "pm8150_gpioN"); its type cell is NONE on Android. Keys
         * need both edges whatever it says, for press and release. */
        if (kshim_dt_spmi_irq_find(fdt, k->controller, k->sid, k->peripheral, k->irq, &trigger) < 0)
            return ERROR_NOT_FOUND;
        int status = kshim_spmi_irq_request(fdt, k->controller, k->sid, k->peripheral, k->irq,
                                            KSHIM_IRQ_TYPE_EDGE_BOTH, key_spmi_irq, k);
        if (status) return status;
        k->spmi = true;
        return 0;
#endif
    }
    if (d->Source != QCOM_KEY_SOURCE_GPIO || d->Base + d->RegisterOffset < QCOM_TLMM_GPIO_IN_OFFSET)
        return ERROR_NOT_SUPPORTED;
    int status = kshim_tlmm_irq_init(fdt);
    if (!status) status = kshim_tlmm_pin((uintptr_t)(d->Base + d->RegisterOffset - QCOM_TLMM_GPIO_IN_OFFSET), &k->pin);
    if (!status) status = kshim_gpio_irq_request(k->pin, KSHIM_IRQ_TYPE_EDGE_BOTH, key_gpio_irq, k);
    if (status) return status;
    k->gpio = true;
    return 0;
}

static const void *DeferredFdt;
static int keys_irq_deferred(void *keys) { return kshim_keys_irq_start(keys, DeferredFdt); }

int kshim_keys_irq_start(QcomKeys *keys, const void *fdt)
{
    if (!keys || !fdt || !keys->Count) return 0;
    (void)kshim_keys_irq_stop(keys);
    if (!kshim_irq_ready()) {
        /* The GIC core can come up after the keys (it needs the MMU map that
         * kshim_platform_accelerate() builds): the first poll after
         * kshim_irq_ready() finishes the setup. */
        DeferredFdt = fdt;
        keys->IrqStart = keys_irq_deferred;
        return 0;
    }
    int driven = 0;
    uint32_t debounce = KSHIM_KEYS_IRQ_DEBOUNCE_MS;
    for (size_t i = 0; i < keys->Count; ++i) {
        if (key_request(keys, fdt, i)) { KeyIrqs[i] = (struct key_irq){0}; continue; }
        if (KeyIrqs[i].gpio) {
            uint32_t ms = gpio_key_debounce(fdt, KeyIrqs[i].pin);
            if (ms > debounce) debounce = ms;
        }
        ++driven;
    }
    if (!driven) return 0;
    keys->IrqDebounceMs = debounce;
    keys->IrqAlive = key_alive;
    keys->IrqContext = KeyIrqs;
    for (size_t i = 0; i < keys->Count; ++i)
        if (KeyIrqs[i].spmi || KeyIrqs[i].gpio) QcomKeysIrqSet(keys, i, 1U);
    return driven;
}

int kshim_keys_irq_stop(QcomKeys *keys)
{
    int first = 0;
    if (keys) keys->IrqStart = NULL; /* a stopped source never starts again */
    for (size_t i = 0; i < QCOM_KEYS_MAX; ++i) {
        struct key_irq *k = &KeyIrqs[i];
        if (!k->spmi && !k->gpio) continue;
        if (keys && k->keys == keys) QcomKeysIrqSet(keys, i, 0U);
#if CONFIG_QCOM_SPMI
        int status = k->spmi ? kshim_spmi_irq_free(k->controller, k->sid, k->peripheral, k->irq)
                             : kshim_gpio_irq_free(k->pin);
#else
        int status = kshim_gpio_irq_free(k->pin);
#endif
        if (status) { if (!first) first = status; continue; }
        *k = (struct key_irq){0};
    }
    if (keys && !first) { keys->IrqAlive = NULL; keys->IrqContext = NULL; }
    return first;
}
