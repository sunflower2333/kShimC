/* SPDX-License-Identifier: MIT */
/*
 * SPI bus core (kshim/spi.h): controller registry, on-demand controller
 * probe by DT node, DT devices (Linux spi-controller/spi-peripheral
 * binding properties: "reg", "spi-max-frequency", "spi-cpha", "spi-cpol",
 * "spi-cs-high", "spi-lsb-first") and serialised messages. Thread context
 * on the boot CPU only.
 */
#include <kshim/spi.h>
#include <kshim/io.h>
#include <config.h>
#include <dt.h>
#include <stdio.h>

static struct kshim_spi_controller *Controllers; /* newest first */
static bool Quiesced;

int kshim_spi_controller_register(struct kshim_spi_controller *controller)
{
    if (!controller || !controller->ops || !controller->ops->transfer) return ERROR_INVALID_PARAMETER;
    if (Quiesced) return KSHIM_IO_ERR_QUIESCED;
    for (struct kshim_spi_controller *c = Controllers; c; c = c->next)
        if (c == controller || (controller->node >= 0 && c->node == controller->node))
            return KSHIM_IO_ERR_EXISTS;
    controller->busy = false;
    controller->quiesced = false;
    controller->next = Controllers;
    Controllers = controller;
    return 0;
}

int kshim_spi_controller_get(const void *fdt, int node, struct kshim_spi_controller **controller)
{
    if (!controller || node < 0) return ERROR_INVALID_PARAMETER;
    for (struct kshim_spi_controller *c = Controllers; c; c = c->next)
        if (c->node == node) {
            *controller = c;
            return c->quiesced ? KSHIM_IO_ERR_QUIESCED : 0;
        }
    if (Quiesced) return KSHIM_IO_ERR_QUIESCED;
    if (!fdt) return ERROR_INVALID_PARAMETER;
    if (!kshim_io_on_boot_cpu()) return ERROR_BUSY;
    if (!dt_available(fdt, node)) return ERROR_NOT_FOUND;
#if CONFIG_KSHIM_SPI_GENI
    if (!dt_node_check_compatible(fdt, node, "qcom,geni-spi") ||
        !dt_node_check_compatible(fdt, node, "qcom,spi-geni"))
        return kshim_geni_spi_probe(fdt, node, controller);
#endif
    return ERROR_NOT_FOUND;
}

int kshim_spi_device_from_node(const void *fdt, int node, struct kshim_spi_device *device)
{
    if (!fdt || node < 0 || !device) return ERROR_INVALID_PARAMETER;
    uint32_t reg, hz;
    bool cpha = false, cpol = false, cs_high = false, lsb = false;
    if (dt_u32(fdt, node, "reg", &reg) || reg > UINT8_MAX ||
        dt_u32(fdt, node, "spi-max-frequency", &hz) || !hz ||
        dt_bool(fdt, node, "spi-cpha", &cpha) || dt_bool(fdt, node, "spi-cpol", &cpol) ||
        dt_bool(fdt, node, "spi-cs-high", &cs_high) || dt_bool(fdt, node, "spi-lsb-first", &lsb))
        return ERROR_INVALID_PARAMETER;
    struct kshim_spi_controller *controller;
    int status = kshim_spi_controller_get(fdt, dt_parent_offset(fdt, node), &controller);
    if (status) return status;
    if (controller->num_chipselect && reg >= controller->num_chipselect) return ERROR_INVALID_PARAMETER;
    *device = (struct kshim_spi_device){
        .controller = controller, .node = node, .chip_select = (uint8_t)reg,
        .mode = (uint8_t)((cpha ? KSHIM_SPI_CPHA : 0) | (cpol ? KSHIM_SPI_CPOL : 0) |
                          (cs_high ? KSHIM_SPI_CS_HIGH : 0) | (lsb ? KSHIM_SPI_LSB_FIRST : 0)),
        .bits_per_word = 8, .max_speed_hz = hz,
    };
    return 0;
}

int kshim_spi_for_each_child(const void *fdt, struct kshim_spi_controller *controller,
                             int (*fn)(const struct kshim_spi_device *device, void *context),
                             void *context)
{
    if (!fdt || !controller || !fn) return ERROR_INVALID_PARAMETER;
    int child;
    fdt_for_each_subnode(child, fdt, controller->node) {
        struct kshim_spi_device device;
        if (!dt_available(fdt, child) || kshim_spi_device_from_node(fdt, child, &device)) continue;
        int status = fn(&device, context);
        if (status) return status;
    }
    return 0;
}

int kshim_spi_sync(const struct kshim_spi_device *device, const struct kshim_spi_xfer *xfers,
                   size_t count, uint32_t timeout_us)
{
    if (!device || !device->controller || !xfers || !count) return ERROR_INVALID_PARAMETER;
    struct kshim_spi_controller *c = device->controller;
    if (c->quiesced || Quiesced) return KSHIM_IO_ERR_QUIESCED;
    if (c->busy || !kshim_io_on_boot_cpu()) return ERROR_BUSY;
    c->busy = true;
    int status = c->ops->transfer(c, device, xfers, count,
                                  timeout_us ? timeout_us : KSHIM_SPI_DEFAULT_TIMEOUT_US);
    c->busy = false;
    return status;
}

int kshim_spi_write_then_read(const struct kshim_spi_device *device, const void *tx, size_t tx_length,
                              void *rx, size_t rx_length, uint32_t timeout_us)
{
    if (tx_length > UINT32_MAX || rx_length > UINT32_MAX) return ERROR_INVALID_PARAMETER;
    struct kshim_spi_xfer xfers[2];
    size_t count = 0;
    if (tx_length) xfers[count++] = (struct kshim_spi_xfer){tx, NULL, (uint32_t)tx_length, 0};
    if (rx_length) xfers[count++] = (struct kshim_spi_xfer){NULL, rx, (uint32_t)rx_length, 0};
    if (!count) return ERROR_INVALID_PARAMETER;
    return kshim_spi_sync(device, xfers, count, timeout_us);
}

int kshim_spi_quiesce_all(void)
{
    int result = 0;
    Quiesced = true;
    for (struct kshim_spi_controller *c = Controllers; c; c = c->next) {
        if (c->quiesced) continue;
        int status = c->ops->quiesce ? c->ops->quiesce(c) : 0;
        if (status) {
            printf("SPI: %s quiesce: %s\n", c->name ? c->name : "controller", kshim_io_strerror(status));
            if (!result) result = status;
            continue;
        }
        c->quiesced = true;
    }
    return result;
}
