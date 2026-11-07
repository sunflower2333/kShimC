/* SPDX-License-Identifier: MIT */
/*
 * I2C bus core (kshim/i2c.h): adapter registry, on-demand controller probe
 * by DT node, DT clients ("reg" = 7-bit address, or 10-bit with
 * I2C_TEN_BIT_ADDRESS 0x80000000 as in Linux dt-bindings/i2c/i2c.h) and
 * serialised transfers. Thread context on the boot CPU only.
 */
#include <kshim/i2c.h>
#include <kshim/io.h>
#include <config.h>
#include <dt.h>
#include <stdio.h>

#define I2C_TEN_BIT_ADDRESS 0x80000000U
#define I2C_OWN_SLAVE_ADDRESS 0x40000000U

static struct kshim_i2c_adapter *Adapters; /* newest first */
static bool Quiesced;

int kshim_i2c_adapter_register(struct kshim_i2c_adapter *adapter)
{
    if (!adapter || !adapter->ops || !adapter->ops->xfer) return ERROR_INVALID_PARAMETER;
    if (Quiesced) return KSHIM_IO_ERR_QUIESCED;
    for (struct kshim_i2c_adapter *a = Adapters; a; a = a->next)
        if (a == adapter || (adapter->node >= 0 && a->node == adapter->node)) return KSHIM_IO_ERR_EXISTS;
    if (!adapter->bus_hz) adapter->bus_hz = 100000U;
    adapter->busy = false;
    adapter->quiesced = false;
    adapter->next = Adapters;
    Adapters = adapter;
    return 0;
}

static struct kshim_i2c_adapter *find(int node)
{
    for (struct kshim_i2c_adapter *a = Adapters; a; a = a->next)
        if (a->node == node) return a;
    return NULL;
}

int kshim_i2c_adapter_get(const void *fdt, int node, struct kshim_i2c_adapter **adapter)
{
    if (!adapter || node < 0) return ERROR_INVALID_PARAMETER;
    struct kshim_i2c_adapter *a = find(node);
    if (a) {
        *adapter = a;
        return a->quiesced ? KSHIM_IO_ERR_QUIESCED : 0;
    }
    if (Quiesced) return KSHIM_IO_ERR_QUIESCED;
    if (!fdt) return ERROR_INVALID_PARAMETER;
    if (!kshim_io_on_boot_cpu()) return ERROR_BUSY;
    if (!dt_available(fdt, node)) return ERROR_NOT_FOUND;
#if CONFIG_KSHIM_I2C_GENI
    if (!dt_node_check_compatible(fdt, node, "qcom,geni-i2c") ||
        !dt_node_check_compatible(fdt, node, "qcom,i2c-geni"))
        return kshim_geni_i2c_probe(fdt, node, adapter);
#endif
    return ERROR_NOT_FOUND;
}

int kshim_i2c_client_from_node(const void *fdt, int node, struct kshim_i2c_client *client)
{
    if (!fdt || node < 0 || !client) return ERROR_INVALID_PARAMETER;
    uint32_t reg;
    int len;
    const fdt32_t *p = fdt_getprop(fdt, node, "reg", &len);
    if (!p || len != 4) return ERROR_INVALID_PARAMETER;
    reg = fdt32_to_cpu(*p);
    uint16_t flags = 0;
    if (reg & I2C_OWN_SLAVE_ADDRESS) return ERROR_INVALID_PARAMETER;
    if (reg & I2C_TEN_BIT_ADDRESS) {
        reg &= ~I2C_TEN_BIT_ADDRESS;
        if (reg > 0x3ffU) return ERROR_INVALID_PARAMETER;
        flags = KSHIM_I2C_M_TEN;
    } else if (reg > 0x7fU) {
        return ERROR_INVALID_PARAMETER;
    }
    struct kshim_i2c_adapter *adapter;
    int status = kshim_i2c_adapter_get(fdt, dt_parent_offset(fdt, node), &adapter);
    if (status) return status;
    *client = (struct kshim_i2c_client){adapter, node, (uint16_t)reg, flags};
    return 0;
}

int kshim_i2c_for_each_child(const void *fdt, struct kshim_i2c_adapter *adapter,
                             int (*fn)(const struct kshim_i2c_client *client, void *context),
                             void *context)
{
    if (!fdt || !adapter || !fn) return ERROR_INVALID_PARAMETER;
    int child;
    fdt_for_each_subnode(child, fdt, adapter->node) {
        struct kshim_i2c_client client;
        if (!dt_available(fdt, child) || kshim_i2c_client_from_node(fdt, child, &client)) continue;
        int status = fn(&client, context);
        if (status) return status;
    }
    return 0;
}

int kshim_i2c_transfer(struct kshim_i2c_adapter *adapter, const struct kshim_i2c_msg *msgs,
                       size_t count, uint32_t timeout_us)
{
    if (!adapter || !msgs || !count) return ERROR_INVALID_PARAMETER;
    for (size_t i = 0; i < count; ++i)
        if (msgs[i].len && !msgs[i].buf) return ERROR_INVALID_PARAMETER;
    if (adapter->quiesced || Quiesced) return KSHIM_IO_ERR_QUIESCED;
    /* Re-entry (an interrupt handler, a nested pump) or another CPU. */
    if (adapter->busy || !kshim_io_on_boot_cpu()) return ERROR_BUSY;
    adapter->busy = true;
    int status = adapter->ops->xfer(adapter, msgs, count,
                                    timeout_us ? timeout_us : KSHIM_I2C_DEFAULT_TIMEOUT_US);
    adapter->busy = false;
    return status;
}

int kshim_i2c_write(const struct kshim_i2c_client *client, const void *data, size_t length,
                    uint32_t timeout_us)
{
    if (!client || length > UINT16_MAX) return ERROR_INVALID_PARAMETER;
    struct kshim_i2c_msg msg = {client->addr, client->flags & KSHIM_I2C_M_TEN, (uint16_t)length,
                                (uint8_t *)(uintptr_t)data};
    return kshim_i2c_transfer(client->adapter, &msg, 1, timeout_us);
}

int kshim_i2c_read(const struct kshim_i2c_client *client, void *data, size_t length,
                   uint32_t timeout_us)
{
    if (!client || length > UINT16_MAX) return ERROR_INVALID_PARAMETER;
    struct kshim_i2c_msg msg = {client->addr, (uint16_t)((client->flags & KSHIM_I2C_M_TEN) | KSHIM_I2C_M_RD),
                                (uint16_t)length, data};
    return kshim_i2c_transfer(client->adapter, &msg, 1, timeout_us);
}

int kshim_i2c_write_read(const struct kshim_i2c_client *client, const void *tx, size_t tx_length,
                         void *rx, size_t rx_length, uint32_t timeout_us)
{
    if (!client || tx_length > UINT16_MAX || rx_length > UINT16_MAX) return ERROR_INVALID_PARAMETER;
    uint16_t ten = client->flags & KSHIM_I2C_M_TEN;
    struct kshim_i2c_msg msgs[2] = {
        {client->addr, ten, (uint16_t)tx_length, (uint8_t *)(uintptr_t)tx},
        {client->addr, (uint16_t)(ten | KSHIM_I2C_M_RD), (uint16_t)rx_length, rx},
    };
    if (!rx_length) return kshim_i2c_transfer(client->adapter, msgs, 1, timeout_us);
    if (!tx_length) return kshim_i2c_transfer(client->adapter, &msgs[1], 1, timeout_us);
    return kshim_i2c_transfer(client->adapter, msgs, 2, timeout_us);
}

int kshim_i2c_quiesce_all(void)
{
    int result = 0;
    Quiesced = true;
    for (struct kshim_i2c_adapter *a = Adapters; a; a = a->next) {
        if (a->quiesced) continue;
        int status = a->ops->quiesce ? a->ops->quiesce(a) : 0;
        if (status) {
            printf("I2C: %s quiesce: %s\n", a->name ? a->name : "adapter", kshim_io_strerror(status));
            if (!result) result = status;
            continue;
        }
        a->quiesced = true;
    }
    return result;
}
