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

static QcomKeys Keys;

int kshim_input_keys_init(const void *fdt)
{
#if CONFIG_QCOM_KEYS
    QcomKeyDescriptor descriptors[QCOM_KEYS_MAX];
    size_t count = 0;
    if (!fdt || QcomKeysBuildFromDeviceTree(fdt, descriptors, QCOM_KEYS_MAX, &count) || !count)
        return -1;
    int status = QcomKeysInitEx(&Keys, descriptors, count, kshim_qcom_key_read32,
                                kshim_qcom_key_read32_status, NULL);
    QcomKeysSetDebounce(&Keys, CONFIG_QCOM_KEY_DEBOUNCE_SAMPLES);
    return status;
#else
    (void)fdt; return -1;
#endif
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
    if (r->touch_status) return 1;
    /* Only the FTM5 generation has a runtime driver; unsupported generations
     * fall back to the volume-key menu without touching MMIO. */
    if (r->touch.generation != 5) return 1;
    TouchResource = &r->touch;
    kshim_touch_config_t cfg = {.source_width = r->touch.width, .source_height = r->touch.height,
        .output_width = output_width, .output_height = output_height,
        .flip_x = r->touch.flip_x, .flip_y = r->touch.flip_y,
        .emit = emit, .emit_context = context};
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
    return 0;
}
int kshim_input_touch_poll(void)
{
    if (!TouchReady) return 0;
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
