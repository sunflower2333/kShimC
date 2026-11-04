/* SPDX-License-Identifier: MIT */
/*
 * GENI I2C controller (CONFIG_KSHIM_I2C_GENI): an I2C adapter on a QUP
 * serial engine running the I2C firmware, probed on demand by
 * kshim_i2c_adapter_get() for "qcom,geni-i2c" and "qcom,i2c-geni" nodes.
 *
 * Transfers use CrDK's FIFO core (CrGeniI2cTransfer): consecutive messages
 * are joined by repeated START (STOP_STRETCH on all but the last), the last
 * message of a run gets STOP, an address or data NACK and a timeout run its
 * bounded cancel/abort and STOP recovery. KSHIM_I2C_M_STOP splits a list
 * into such runs. Bus rates are the FIFO core's SCL timing table entries
 * (100 kHz, 400 kHz, 1 MHz from the 19.2 MHz CXO); others are refused.
 */
#include <kshim/i2c.h>
#include <kshim/io.h>
#include "geni_engine.h"
#include <dt.h>
#include <stdio.h>
#include <string.h>

#define GENI_I2C_MAX 8U
#define GENI_I2C_MAX_TIMEOUT_US 1000000U

struct geni_i2c {
    struct kshim_i2c_adapter adapter;
    struct kshim_geni_engine engine;
    char name[24];
    bool used;
};

static struct geni_i2c Pool[GENI_I2C_MAX];

static int geni_i2c_xfer(struct kshim_i2c_adapter *adapter, const struct kshim_i2c_msg *msgs,
                         size_t count, uint32_t timeout_us)
{
    struct geni_i2c *g = adapter->priv;
    struct CrI2cMessage run[KSHIM_I2C_GENI_MAX_RUN];
    if (!count || !msgs) return ERROR_INVALID_PARAMETER;
    if (!timeout_us) timeout_us = KSHIM_I2C_DEFAULT_TIMEOUT_US;
    if (timeout_us > GENI_I2C_MAX_TIMEOUT_US) timeout_us = GENI_I2C_MAX_TIMEOUT_US;
    /* Validate the whole list before the first START. */
    size_t in_run = 0;
    for (size_t i = 0; i < count; ++i) {
        if (msgs[i].flags & KSHIM_I2C_M_TEN) return ERROR_NOT_SUPPORTED;
        if (!msgs[i].len) return ERROR_NOT_SUPPORTED; /* no zero-length probes */
        if (msgs[i].addr > 0x7fU || !msgs[i].buf) return ERROR_INVALID_PARAMETER;
        if (++in_run > KSHIM_I2C_GENI_MAX_RUN) return ERROR_INVALID_PARAMETER;
        if (msgs[i].flags & KSHIM_I2C_M_STOP) in_run = 0;
    }
    int status = kshim_geni_engine_prepare(&g->engine, adapter->bus_hz, 0, 0);
    if (status) return status;
    const struct CrIo *io = g->engine.mmio;
    uint64_t start = io->now_us(io->context);
    for (size_t i = 0; i < count;) {
        size_t n = 0;
        do {
            const struct kshim_i2c_msg *m = &msgs[i + n];
            run[n++] = (struct CrI2cMessage){m->addr, (m->flags & KSHIM_I2C_M_RD) != 0, m->buf, m->len};
        } while (i + n < count && !(msgs[i + n - 1].flags & KSHIM_I2C_M_STOP));
        /* Between runs the bus is idle (STOP sent): stop at the deadline. */
        uint64_t elapsed = io->now_us(io->context) - start;
        if (elapsed >= timeout_us) return ERROR_TIMEOUT;
        status = CrGeniI2cTransfer(&g->engine.bus, run, n, timeout_us - (uint32_t)elapsed);
        adapter->irq_driven = g->engine.irq_live;
        if (status) return kshim_io_err_from_bus(status);
        i += n;
    }
    return 0;
}

static int geni_i2c_quiesce(struct kshim_i2c_adapter *adapter)
{
    struct geni_i2c *g = adapter->priv;
    struct kshim_qup_se *se = g->engine.se;
    int status = kshim_geni_engine_stop(&g->engine);
    if (status) return status;
    adapter->irq_driven = false;
    return se ? kshim_qup_se_unclaim(se) : 0;
}

static const struct kshim_i2c_adapter_ops GeniI2cOps = {
    .xfer = geni_i2c_xfer,
    .quiesce = geni_i2c_quiesce,
};

int kshim_geni_i2c_probe(const void *fdt, int node, struct kshim_i2c_adapter **adapter)
{
    if (!fdt || node < 0 || !adapter) return ERROR_INVALID_PARAMETER;
    struct kshim_qup_se *se = kshim_qup_se_by_node(node);
    if (!se || kshim_dt_qup_protocol(fdt, node) != KSHIM_QUP_PROTOCOL_I2C) return ERROR_NOT_FOUND;
    struct geni_i2c *g = NULL;
    for (unsigned i = 0; i < GENI_I2C_MAX && !g; ++i)
        if (!Pool[i].used) g = &Pool[i];
    if (!g) return KSHIM_IO_ERR_NO_MEMORY;
    int status = kshim_qup_se_claim(se, KSHIM_QUP_OWNER_I2C, node, "i2c");
    if (status) {
        printf("I2C: engine %#llx is owned by %s\n", (unsigned long long)se->base,
               se->owner_name ? se->owner_name : "another driver");
        return status;
    }
    memset(g, 0, sizeof(*g));
    snprintf(g->name, sizeof(g->name), "i2c@%llx", (unsigned long long)se->base);
    g->adapter = (struct kshim_i2c_adapter){
        .name = g->name, .node = node, .bus_hz = kshim_dt_qup_i2c_bus_hz(fdt, node),
        .ops = &GeniI2cOps, .priv = g,
    };
    status = kshim_geni_engine_start(&g->engine, se, KSHIM_QUP_PROTOCOL_I2C, g->adapter.bus_hz, 0, 0);
    if (!status) {
        status = kshim_i2c_adapter_register(&g->adapter);
        if (status) (void)kshim_geni_engine_stop(&g->engine);
    }
    if (status) {
        (void)kshim_qup_se_unclaim(se);
        printf("I2C: %s: %s\n", g->name, kshim_io_strerror(status));
        return status;
    }
    g->used = true;
    g->adapter.irq_driven = g->engine.irq_live;
    printf("I2C: %s at %u Hz, %s%s\n", g->name, (unsigned)g->adapter.bus_hz,
           g->engine.irq_live ? "interrupt-driven" : "polled",
           se->inherited_active ? ", inherited clocks" : "");
    *adapter = &g->adapter;
    return 0;
}
