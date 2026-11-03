/* SPDX-License-Identifier: MIT */
#pragma once
/*
 * SPI bus core and controller interface (CONFIG_KSHIM_SPI).
 * Owner: qup (kernel/drivers/spi/core.c; GENI controller in
 * kernel/drivers/qup/geni_spi.c, CONFIG_KSHIM_SPI_GENI). See docs/io-stack.md.
 *
 * Same model as kshim/i2c.h: controllers register, consumers look up by DT
 * node with on-demand probe of "qcom,geni-spi"/"qcom,spi-geni". Devices are
 * DT children: "reg" = chip select, "spi-max-frequency", "spi-cpha",
 * "spi-cpol", "spi-cs-high", "spi-lsb-first" (GENI: CPHA/CPOL only, 8-bit
 * words; other modes are ERROR_NOT_SUPPORTED).
 *
 * A message is a list of full-duplex segments under one chip-select
 * assertion (CrDK CrGeniSpiTransfer semantics: CS is held across segments;
 * delay_us holds CS after a segment). Thread context only.
 */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "error_no.h"

#define KSHIM_SPI_CPHA      0x01U
#define KSHIM_SPI_CPOL      0x02U
#define KSHIM_SPI_MODE_0    0x00U
#define KSHIM_SPI_MODE_3    (KSHIM_SPI_CPHA | KSHIM_SPI_CPOL)
#define KSHIM_SPI_CS_HIGH   0x04U
#define KSHIM_SPI_LSB_FIRST 0x08U

#define KSHIM_SPI_DEFAULT_TIMEOUT_US 20000U

/* One full-duplex segment: tx NULL clocks out zeros, rx NULL discards. */
struct kshim_spi_xfer {
    const void *tx;
    void *rx;
    uint32_t len;
    uint16_t delay_us;   /* hold CS this long after the segment, <= 1000 */
};

struct kshim_spi_controller;
struct kshim_spi_device {
    struct kshim_spi_controller *controller;
    int node;                /* DT child node */
    uint8_t chip_select;
    uint8_t mode;            /* KSHIM_SPI_* */
    uint8_t bits_per_word;   /* 8 */
    uint32_t max_speed_hz;   /* spi-max-frequency (required) */
};

struct kshim_spi_controller_ops {
    /* Assert CS, run the segments, deassert CS. Returns 0, ERROR_TIMEOUT,
     * ERROR_NOT_SUPPORTED (mode/word size), ERROR_DEVICE_ERROR. */
    int (*transfer)(struct kshim_spi_controller *controller, const struct kshim_spi_device *device,
                    const struct kshim_spi_xfer *xfers, size_t count, uint32_t timeout_us);
    int (*quiesce)(struct kshim_spi_controller *controller);
};

struct kshim_spi_controller {
    const char *name;
    int node;
    uint8_t num_chipselect;
    const struct kshim_spi_controller_ops *ops;
    void *priv;
    bool irq_driven;
    /* Owned by the core. */
    bool busy, quiesced;
    struct kshim_spi_controller *next;
};

int kshim_spi_controller_register(struct kshim_spi_controller *controller);
int kshim_spi_controller_get(const void *fdt, int node, struct kshim_spi_controller **controller);
int kshim_spi_device_from_node(const void *fdt, int node, struct kshim_spi_device *device);
int kshim_spi_for_each_child(const void *fdt, struct kshim_spi_controller *controller,
                             int (*fn)(const struct kshim_spi_device *device, void *context),
                             void *context);

int kshim_spi_sync(const struct kshim_spi_device *device, const struct kshim_spi_xfer *xfers,
                   size_t count, uint32_t timeout_us);
/* Two segments under one CS: tx_length bytes out, then rx_length bytes in. */
int kshim_spi_write_then_read(const struct kshim_spi_device *device, const void *tx, size_t tx_length,
                              void *rx, size_t rx_length, uint32_t timeout_us);

int kshim_spi_quiesce_all(void);
