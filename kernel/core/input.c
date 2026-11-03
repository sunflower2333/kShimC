/* SPDX-License-Identifier: MIT */
/*
 * Kernel input services behind kshim/input.h: Qualcomm keys built from the
 * boot FDT, and the DT-selected FTM5 touch controller on a GENI QUP.
 */
#include <kshim.h>
#include <kshim/input.h>
#include <platform.h>
#include <qcom_dt.h>
#include <qcom_key_platform.h>
#include <qcom_keys.h>
#include <qcom_keys_fdt.h>
#include <config.h>
#if CONFIG_KSHIM_TOUCH
#include <st_ftm5.h>
#endif
#if CONFIG_KSHIM_INPUT_IRQ
#include <input_irq.h>
#include <qcom_irq.h>
#include <kshim/time.h>
#endif
#if CONFIG_KSHIM_INPUT_INJECT
#include <kshim/input_inject.h>
#endif

static QcomKeys Keys;

static int platform_keys_init(const void *fdt)
{
#if CONFIG_QCOM_KEYS
    QcomKeyDescriptor descriptors[QCOM_KEYS_MAX];
    size_t count = 0;
#if CONFIG_KSHIM_INPUT_IRQ
    /* A rebuild must release the previous descriptors' interrupts first. */
    if (kshim_keys_irq_stop(&Keys)) return -1;
#endif
    if (!fdt || QcomKeysBuildFromDeviceTree(fdt, descriptors, QCOM_KEYS_MAX, &count) || !count)
        return -1;
    int status = QcomKeysInitEx(&Keys, descriptors, count, kshim_qcom_key_read32,
                                kshim_qcom_key_read32_status, NULL);
    QcomKeysSetDebounce(&Keys, CONFIG_QCOM_KEY_DEBOUNCE_SAMPLES);
#if CONFIG_KSHIM_INPUT_IRQ
    /* Keys whose interrupt resolves are sampled by their handler; the rest
     * keep the debounced poll. */
    if (!status) {
        int driven = kshim_keys_irq_start(&Keys, fdt);
        if (kshim_irq_ready()) printf("Keys: %d of %u interrupt-driven\n", driven, (unsigned)count);
    }
#endif
    return status;
#else
    (void)fdt; return -1;
#endif
}

int kshim_input_keys_init(const void *fdt)
{
    int status = platform_keys_init(fdt);
#if CONFIG_KSHIM_INPUT_INJECT
    /* Injected keys (USB HID) are a key source of their own. */
    if (status && kshim_input_inject_has(KSHIM_INPUT_CAP_KEYS)) status = 0;
#endif
    return status;
}

/* docs/interrupts.md handoff step 1: input interrupts stop before the UI
 * and the secondary controllers are torn down. */
int kshim_input_quiesce(void)
{
    int status = 0;
#if CONFIG_KSHIM_INPUT_INJECT
    kshim_input_inject_quiesce();
#endif
#if CONFIG_KSHIM_TOUCH
    status = kshim_input_touch_stop();
#endif
#if CONFIG_KSHIM_INPUT_IRQ
    if (kshim_keys_irq_stop(&Keys) && !status) status = -1;
#endif
    return status;
}

kshim_key_source_t *kshim_input_keys(void)
{
    return QcomKeysSource(&Keys);
}

#if CONFIG_KSHIM_TOUCH
static kshim_touch_t Touch;
static struct CrQupContext TouchPlatform;
static kshim_ftm5_t Ftm5;
static int TouchAttempted, TouchReady;
static struct kshim_touch_resource *TouchResource;
static kshim_touch_event_fn TouchEmit;
static void *TouchEmitContext;
#if CONFIG_KSHIM_INPUT_IRQ
/* IRQ mode: the FTM5 FIFO is drained by the TLMM handler (level low while
 * events are pending) and the tracker's events are queued for the poll. */
static struct kshim_touch_queue TouchQueue;
static uint32_t TouchPin;
static volatile bool TouchIrq;
static volatile int TouchIrqError;
static volatile uint32_t TouchIrqCount;
static uint32_t TouchStuckCount;
static uint64_t TouchStuckSince;
static void touch_irq(uint32_t pin, void *context)
{
    (void)context;
    ++TouchIrqCount;
    int status = kshim_ftm5_poll(&Ftm5);
    /* A failed read cannot deassert the level line: mask it and report the
     * error from the next poll. */
    if (status < 0) { TouchIrqError = status; kshim_gpio_irq_mask(pin, true); }
}
static void touch_irq_stop(void)
{
    if (!TouchIrq) return;
    if (!kshim_gpio_irq_free(TouchPin)) TouchIrq = false;
    kshim_touch_queue_reset(&TouchQueue);
}
static bool TouchIrqTried;
/* Once, as soon as the GIC core is up (here or from a later poll). */
static void touch_irq_start(struct kshim_resources *r)
{
    unsigned trigger;
    if (TouchIrqTried || !kshim_irq_ready()) return;
    TouchIrqTried = true;
    if (kshim_touch_irq_resolve(r->fdt, r->touch.node, r->touch.qup.irq_pin,
            r->touch.qup.irq_active_low, &TouchPin, &trigger)) return;
    kshim_touch_queue_reset(&TouchQueue);
    TouchIrqError = 0;
    TouchStuckSince = 0;
    TouchIrq = true;
    if (kshim_gpio_irq_request(TouchPin, trigger, touch_irq, NULL)) TouchIrq = false;
    printf("Touch: %s\n", TouchIrq ? "interrupt-driven" : "polled");
}
#endif
/* Tracker output: queued in IRQ mode, delivered directly when polled. */
static void touch_deliver(void *context, const kshim_touch_event_t *event)
{
    (void)context;
#if CONFIG_KSHIM_INPUT_IRQ
    if (TouchIrq) { kshim_touch_queue_push(&TouchQueue, event); return; }
#endif
    if (TouchEmit) TouchEmit(TouchEmitContext, event);
}
static int touch_transfer(void *context, const uint8_t *command, size_t length,
                          uint8_t *response, size_t count)
{
    (void)context;
    struct CrI2cMessage messages[] = {{TouchResource->address, false, (uint8_t *)(uintptr_t)command, length},
                                    {TouchResource->address, true, response, count}};
    return CrGeniI2cTransfer(&TouchPlatform.i2c, messages, count ? 2 : 1, 20000);
}
static void touch_delay(void *context, uint32_t us)
{ (void)context; TouchPlatform.io.delay_us(TouchPlatform.io.context, us); }
static int touch_quiesce(void)
{
#if CONFIG_KSHIM_INPUT_IRQ
    touch_irq_stop();
    if (TouchIrq) return -1;
#endif
    if (!TouchAttempted) return 0;
    int status = kshim_ftm5_quiesce(&Ftm5);
    if (status || CrQupQuiesce(&TouchPlatform)) return -1;
    TouchAttempted = TouchReady = 0;
    return 0;
}
int kshim_input_touch_start(uint32_t output_width, uint32_t output_height,
                            kshim_touch_event_fn emit, void *context)
{
    struct kshim_resources *r = kshim_resources_get();
    /* Injected pointer events (docs/io-stack.md) use the same emit callback
     * and surface, with or without a touch controller. */
    TouchEmit = emit; TouchEmitContext = context;
#if CONFIG_KSHIM_INPUT_INJECT
    kshim_input_inject_set_surface(output_width, output_height);
#endif
    if (r->touch_status) return 1;
    /* Only the FTM5 generation has a runtime driver; unsupported generations
     * fall back to the volume-key menu without touching MMIO. */
    if (r->touch.generation != 5) return 1;
    TouchResource = &r->touch;
    kshim_touch_config_t cfg = {.source_width = r->touch.width, .source_height = r->touch.height,
        .output_width = output_width, .output_height = output_height,
        .flip_x = r->touch.flip_x, .flip_y = r->touch.flip_y,
        .emit = touch_deliver, .emit_context = NULL};
    int status = kshim_touch_init(&Touch, &cfg);
    if (!status) status = CrQupInit(&TouchPlatform, NULL, &r->touch.qup);
    TouchAttempted = 1;
    if (!status) {
        kshim_ftm5_transport_t t = {NULL, touch_transfer, touch_delay, KSHIM_FTM5_BUS_I2C};
        status = kshim_ftm5_init(&Ftm5, &t, &Touch);
    }
    TouchReady = status == 0;
    printf("Touch: FTM%u status %d\n", r->touch.generation, status);
    if (!TouchReady) { (void)touch_quiesce(); return status < 0 ? status : -1; }
#if CONFIG_KSHIM_INPUT_IRQ
    TouchIrqTried = false;
    touch_irq_start(r);
#endif
    return 0;
}
int kshim_input_touch_poll(void)
{
#if CONFIG_KSHIM_INPUT_INJECT
    kshim_touch_event_t injected;
    kshim_input_service();
    while (kshim_input_inject_poll_touch(&injected))
        if (TouchEmit) TouchEmit(TouchEmitContext, &injected);
#endif
    if (!TouchReady) return 0;
#if CONFIG_KSHIM_INPUT_IRQ
    if (TouchIrq) {
        kshim_touch_event_t event;
        while (kshim_touch_queue_pop(&TouchQueue, &event))
            if (TouchEmit) TouchEmit(TouchEmitContext, &event);
        int error = TouchIrqError;
        /* The level line held for 100 ms with no interrupt taken means the
         * interrupt resolves but is not delivered: poll the line instead. */
        uint64_t now = get_current_time();
        if (!error && CrQupIrq(&TouchPlatform) > 0) {
            if (!TouchStuckSince) { TouchStuckSince = now; TouchStuckCount = TouchIrqCount; }
            else if (now - TouchStuckSince >= 100 && TouchStuckCount == TouchIrqCount) {
                touch_irq_stop();
                printf("Touch: interrupt not delivered, polling\n");
                return 0;
            }
        } else TouchStuckSince = 0;
        if (!error) return 0;
        TouchReady = 0;
        touch_irq_stop();
        kshim_touch_cancel(&Touch);
        printf("Touch: input status %d\n", error);
        return error;
    }
    touch_irq_start(kshim_resources_get());
    if (TouchIrq) return 0;
#endif
    int status = CrQupIrq(&TouchPlatform);
    if (status > 0) status = kshim_ftm5_poll(&Ftm5);
    if (status < 0) {
        TouchReady = 0; kshim_touch_cancel(&Touch);
        printf("Touch: input status %d\n", status);
        return status;
    }
    return 0;
}
int kshim_input_touch_stop(void)
{
    TouchReady = 0;
    return touch_quiesce();
}
#endif
