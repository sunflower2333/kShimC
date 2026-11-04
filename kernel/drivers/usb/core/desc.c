/* SPDX-License-Identifier: MIT */
/*
 * USB descriptor iteration (kshim/usb.h). Layout facts: USB 2.0 chapter 9
 * (9.5 descriptors are bLength/bDescriptorType prefixed and follow the
 * configuration descriptor in wTotalLength order), USB 3.2 9.6.7.
 */
#include <kshim/usb.h>

const uint8_t *kshim_usb_desc_next(const uint8_t *config, size_t length, size_t *offset)
{
    if (!config || !offset || *offset >= length) return NULL;
    size_t at = *offset;
    uint8_t len = config[at];
    if (len < 2 || len > length - at) return NULL;
    *offset = at + len;
    return config + at;
}

const uint8_t *kshim_usb_desc_find_in_interface(const uint8_t *config, size_t length,
                                                const uint8_t *from, uint8_t type)
{
    if (!config || !from || from < config || from >= config + length) return NULL;
    size_t offset = (size_t)(from - config);
    /* Skip `from` itself. */
    if (!kshim_usb_desc_next(config, length, &offset)) return NULL;
    const uint8_t *d;
    while ((d = kshim_usb_desc_next(config, length, &offset))) {
        if (d[1] == KSHIM_USB_DT_INTERFACE) return NULL;
        if (d[1] == type) return d;
    }
    return NULL;
}

const char *kshim_usb_speed_name(enum kshim_usb_speed speed)
{
    switch (speed) {
    case KSHIM_USB_SPEED_LOW: return "low";
    case KSHIM_USB_SPEED_FULL: return "full";
    case KSHIM_USB_SPEED_HIGH: return "high";
    case KSHIM_USB_SPEED_SUPER: return "super";
    case KSHIM_USB_SPEED_SUPER_PLUS: return "super+";
    default: return "unknown";
    }
}
