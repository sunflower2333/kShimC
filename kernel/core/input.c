/* SPDX-License-Identifier: MIT */
/*
 * Kernel input services behind kshim/input.h: Qualcomm keys built from the
 * boot FDT, and the touch service: controllers from the touch framework
 * (CONFIG_KSHIM_TOUCH_FRAMEWORK, kshim/touch_driver.h) and injected pointer
 * events (USB HID), both to the same emit callback.
 */
#include <kshim.h>
#include <kshim/input.h>
#include <platform.h>
#include <qcom_dt.h>
#include <qcom_key_platform.h>
#include <qcom_keys.h>
#include <qcom_keys_fdt.h>
#include <config.h>
#if CONFIG_KSHIM_TOUCH_FRAMEWORK
#include <kshim/touch_driver.h>
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
static kshim_touch_event_fn TouchEmit;
static void *TouchEmitContext;
/* docs/io-stack.md 3.10: selection, power, deferred IRQ and polling live in
 * kernel/drivers/input/touch/core.c; this is the public service. Without a
 * KSHIM_TOUCH_<chip> it only carries injected pointer events. */
int kshim_input_touch_start(uint32_t output_width, uint32_t output_height,
                            kshim_touch_event_fn emit, void *context)
{
    /* Injected pointer events (docs/io-stack.md) use the same emit callback
     * and surface, with or without a touch controller. */
    TouchEmit = emit; TouchEmitContext = context;
#if CONFIG_KSHIM_INPUT_INJECT
    kshim_input_inject_set_surface(output_width, output_height);
#endif
#if CONFIG_KSHIM_TOUCH_FRAMEWORK
    struct kshim_resources *r = kshim_resources_get();
    if (!r->fdt) return 1;
    int status = kshim_touch_framework_start(r->fdt, output_width, output_height, emit, context);
    if (status == ERROR_NOT_FOUND) return 1;   /* no supported controller, nothing touched */
    return status ? -status : 0;
#else
    (void)output_width; (void)output_height;
    return 1;
#endif
}
int kshim_input_touch_poll(void)
{
#if CONFIG_KSHIM_INPUT_INJECT
    kshim_touch_event_t injected;
    kshim_input_service();
    while (kshim_input_inject_poll_touch(&injected))
        if (TouchEmit) TouchEmit(TouchEmitContext, &injected);
#endif
#if CONFIG_KSHIM_TOUCH_FRAMEWORK
    int status = kshim_touch_framework_poll();
    return status ? -status : 0;
#else
    return 0;
#endif
}
int kshim_input_touch_stop(void)
{
#if CONFIG_KSHIM_TOUCH_FRAMEWORK
    return kshim_touch_framework_stop() ? -1 : 0;
#else
    return 0;
#endif
}
#endif /* CONFIG_KSHIM_TOUCH */
