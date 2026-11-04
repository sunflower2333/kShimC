/* SPDX-License-Identifier: MIT */
#pragma once
/*
 * I2C bus core and controller interface (CONFIG_KSHIM_I2C).
 * Owner: qup (kernel/drivers/i2c/core.c; GENI controller in
 * kernel/drivers/qup/geni_i2c.c, CONFIG_KSHIM_I2C_GENI). See docs/io-stack.md.
 *
 * Controllers register adapters; consumers look an adapter up by its DT
 * node, which probes a known controller on demand (only buses that
 * something uses are touched). Clients are DT children with "reg" = 7-bit
 * address (10-bit with the I2C_TEN_BIT_ADDRESS flag 0x80000000 in reg).
 *
 * Transfers are thread context only and may sleep (WFI) while the
 * controller's interrupt completes them; without a usable interrupt the
 * controller polls. A transfer is atomic on its adapter (the core
 * serialises callers and refuses re-entry with ERROR_BUSY).
 */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "error_no.h"

#define KSHIM_I2C_M_RD    0x0001U /* read into buf */
#define KSHIM_I2C_M_TEN   0x0010U /* 10-bit address */
#define KSHIM_I2C_M_STOP  0x8000U /* STOP after this message (default: repeated START) */

/* One message. Consecutive messages are joined by a repeated START; the
 * last message always ends with STOP. */
struct kshim_i2c_msg {
    uint16_t addr;
    uint16_t flags;
    uint16_t len;     /* 1..65535; 0 only for a write probe where supported */
    uint8_t *buf;
};

#define KSHIM_I2C_DEFAULT_TIMEOUT_US 20000U

struct kshim_i2c_adapter;
struct kshim_i2c_adapter_ops {
    /* count >= 1. timeout_us bounds the whole sequence (0 = default).
     * Returns 0, KSHIM_IO_ERR_NACK (address or data), ERROR_TIMEOUT
     * (controller recovered and idle again), ERROR_DEVICE_ERROR. */
    int (*xfer)(struct kshim_i2c_adapter *adapter, const struct kshim_i2c_msg *msgs,
                size_t count, uint32_t timeout_us);
    /* Stop the engine and restore the controller (CrGeniQuiesce), then
     * release its serial engine. Idempotent. */
    int (*quiesce)(struct kshim_i2c_adapter *adapter);
};

struct kshim_i2c_adapter {
    const char *name;          /* "i2c@a90000" */
    int node;                  /* controller DT node */
    uint32_t bus_hz;           /* DT clock-frequency, default 100000 (Android binding: 400000) */
    const struct kshim_i2c_adapter_ops *ops;
    void *priv;
    bool irq_driven;           /* completion interrupt live (diagnostics) */
    /* Owned by the core. */
    bool busy, quiesced;
    struct kshim_i2c_adapter *next;
};

struct kshim_i2c_client {
    struct kshim_i2c_adapter *adapter;
    int node;                  /* DT child node */
    uint16_t addr;
    uint16_t flags;            /* KSHIM_I2C_M_TEN */
};

/* Register a started adapter (controller drivers, host-test fakes).
 * KSHIM_IO_ERR_EXISTS for a second adapter on one node. */
int kshim_i2c_adapter_register(struct kshim_i2c_adapter *adapter);
/* Registered adapter for controller node `node`, probing a known controller
 * compatible ("qcom,geni-i2c", "qcom,i2c-geni") on demand. ERROR_NOT_FOUND
 * for a node that is not an available I2C controller; the controller's own
 * error (ERROR_BUSY when the SE is owned elsewhere, ERROR_NOT_SUPPORTED for
 * a wrong firmware protocol) otherwise. Thread context. */
int kshim_i2c_adapter_get(const void *fdt, int node, struct kshim_i2c_adapter **adapter);

/* Client for DT child `node` (its parent must resolve with
 * kshim_i2c_adapter_get). ERROR_INVALID_PARAMETER for a bad "reg". */
int kshim_i2c_client_from_node(const void *fdt, int node, struct kshim_i2c_client *client);
/* Call fn for every available child of the adapter's node; stops at the
 * first non-zero return and returns it. */
int kshim_i2c_for_each_child(const void *fdt, struct kshim_i2c_adapter *adapter,
                             int (*fn)(const struct kshim_i2c_client *client, void *context),
                             void *context);

int kshim_i2c_transfer(struct kshim_i2c_adapter *adapter, const struct kshim_i2c_msg *msgs,
                       size_t count, uint32_t timeout_us);
/* Convenience: one write; one read; write then read with repeated START. */
int kshim_i2c_write(const struct kshim_i2c_client *client, const void *data, size_t length,
                    uint32_t timeout_us);
int kshim_i2c_read(const struct kshim_i2c_client *client, void *data, size_t length,
                   uint32_t timeout_us);
int kshim_i2c_write_read(const struct kshim_i2c_client *client, const void *tx, size_t tx_length,
                         void *rx, size_t rx_length, uint32_t timeout_us);

/* Handoff: quiesce every registered adapter, newest first. Later transfers
 * fail with KSHIM_IO_ERR_QUIESCED. Idempotent. */
int kshim_i2c_quiesce_all(void);

/* ---- Additions by the qup owner (additive, docs/qup.md) ----------------- */

/* Limits of the GENI controller (CrDK GeniSeLib FIFO core): 7-bit addresses
 * (KSHIM_I2C_M_TEN is ERROR_NOT_SUPPORTED), len >= 1, at most 32 messages
 * between two STOPs (KSHIM_I2C_M_STOP splits a list), timeout <= 1 s. */
#define KSHIM_I2C_GENI_MAX_RUN 32U

/* GENI controller probe behind kshim_i2c_adapter_get() (CONFIG_KSHIM_I2C_GENI,
 * kernel/drivers/qup/geni_i2c.c): claims the SE for KSHIM_QUP_OWNER_I2C,
 * acquires it, starts the FIFO engine and registers the adapter. */
int kshim_geni_i2c_probe(const void *fdt, int node, struct kshim_i2c_adapter **adapter);
