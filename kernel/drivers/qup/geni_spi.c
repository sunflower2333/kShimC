/* SPDX-License-Identifier: MIT */
/*
 * GENI SPI controller (CONFIG_KSHIM_SPI_GENI): an SPI master on a QUP serial
 * engine running the SPI firmware, probed on demand by
 * kshim_spi_controller_get() for "qcom,geni-spi" and "qcom,spi-geni" nodes.
 *
 * Transfers use CrDK's FIFO core (CrGeniSpiTransfer): CS_ASSERT, one
 * TX/RX/full-duplex command per segment with FRAGMENTATION (CS held), an
 * optional CS hold delay after a segment, then CS_DEASSERT, with bounded
 * cancel/abort recovery. Speed, mode and chip select are engine
 * configuration (CrGeniInit); a message for a device with different
 * settings reprograms the engine first.
 */
#include <kshim/spi.h>
#include <kshim/io.h>
#include "geni_engine.h"
#include <dt.h>
#include <stdio.h>
#include <string.h>

#define GENI_SPI_MAX 4U
#define GENI_SPI_CHIP_SELECTS 4U
#define GENI_SPI_MAX_LEN 0x00ffffffU
#define GENI_SPI_MAX_TIMEOUT_US 1000000U

struct geni_spi {
    struct kshim_spi_controller controller;
    struct kshim_geni_engine engine;
    char name[24];
    bool used;
};

static struct geni_spi Pool[GENI_SPI_MAX];
/* Clocked out by a segment that has neither a TX nor an RX buffer. */
static const uint8_t Zeros[256];

static int geni_spi_transfer(struct kshim_spi_controller *controller, const struct kshim_spi_device *device,
                             const struct kshim_spi_xfer *xfers, size_t count, uint32_t timeout_us)
{
    struct geni_spi *g = controller->priv;
    struct CrSpiSegment segments[KSHIM_SPI_GENI_MAX_SEGMENTS];
    if (!device || !xfers || !count || count > KSHIM_SPI_GENI_MAX_SEGMENTS) return ERROR_INVALID_PARAMETER;
    if ((device->bits_per_word && device->bits_per_word != 8) ||
        (device->mode & ~(KSHIM_SPI_CPHA | KSHIM_SPI_CPOL)))
        return ERROR_NOT_SUPPORTED;
    if (device->chip_select >= GENI_SPI_CHIP_SELECTS || !device->max_speed_hz) return ERROR_INVALID_PARAMETER;
    if (!timeout_us) timeout_us = KSHIM_SPI_DEFAULT_TIMEOUT_US;
    if (timeout_us > GENI_SPI_MAX_TIMEOUT_US) timeout_us = GENI_SPI_MAX_TIMEOUT_US;
    for (size_t i = 0; i < count; ++i) {
        const struct kshim_spi_xfer *x = &xfers[i];
        if (!x->len || x->len > GENI_SPI_MAX_LEN || x->delay_us > 1000U) return ERROR_INVALID_PARAMETER;
        const uint8_t *tx = x->tx;
        if (!tx && !x->rx) {
            if (x->len > sizeof(Zeros)) return ERROR_NOT_SUPPORTED;
            tx = Zeros;
        }
        segments[i] = (struct CrSpiSegment){tx, x->rx, x->len, x->delay_us};
    }
    int status = kshim_geni_engine_prepare(&g->engine, device->max_speed_hz,
                                           device->mode & (KSHIM_SPI_CPHA | KSHIM_SPI_CPOL),
                                           device->chip_select);
    if (status) return status;
    status = CrGeniSpiTransfer(&g->engine.bus, segments, count, timeout_us);
    controller->irq_driven = g->engine.irq_live;
    return kshim_io_err_from_bus(status);
}

static int geni_spi_quiesce(struct kshim_spi_controller *controller)
{
    struct geni_spi *g = controller->priv;
    struct kshim_qup_se *se = g->engine.se;
    int status = kshim_geni_engine_stop(&g->engine);
    if (status) return status;
    controller->irq_driven = false;
    return se ? kshim_qup_se_unclaim(se) : 0;
}

static const struct kshim_spi_controller_ops GeniSpiOps = {
    .transfer = geni_spi_transfer,
    .quiesce = geni_spi_quiesce,
};

/* Initial engine settings: the first available child's, else mode 0 on
 * CS 0 at 1 MHz (reprogrammed per device before each message). */
static void first_device(const void *fdt, int node, uint32_t *hz, uint8_t *mode, uint8_t *cs)
{
    int child;
    *hz = 1000000U; *mode = 0; *cs = 0;
    fdt_for_each_subnode(child, fdt, node) {
        if (!dt_available(fdt, child)) continue;
        uint32_t reg, max;
        if (dt_u32(fdt, child, "reg", &reg) || reg >= GENI_SPI_CHIP_SELECTS ||
            dt_u32(fdt, child, "spi-max-frequency", &max) || !max) continue;
        bool cpha = false, cpol = false;
        (void)dt_bool(fdt, child, "spi-cpha", &cpha);
        (void)dt_bool(fdt, child, "spi-cpol", &cpol);
        *hz = max;
        *mode = (cpha ? KSHIM_SPI_CPHA : 0) | (cpol ? KSHIM_SPI_CPOL : 0);
        *cs = (uint8_t)reg;
        return;
    }
}

int kshim_geni_spi_probe(const void *fdt, int node, struct kshim_spi_controller **controller)
{
    if (!fdt || node < 0 || !controller) return ERROR_INVALID_PARAMETER;
    struct kshim_qup_se *se = kshim_qup_se_by_node(node);
    if (!se || kshim_dt_qup_protocol(fdt, node) != KSHIM_QUP_PROTOCOL_SPI) return ERROR_NOT_FOUND;
    struct geni_spi *g = NULL;
    for (unsigned i = 0; i < GENI_SPI_MAX && !g; ++i)
        if (!Pool[i].used) g = &Pool[i];
    if (!g) return KSHIM_IO_ERR_NO_MEMORY;
    int status = kshim_qup_se_claim(se, KSHIM_QUP_OWNER_SPI, node, "spi");
    if (status) {
        printf("SPI: engine %#llx is owned by %s\n", (unsigned long long)se->base,
               se->owner_name ? se->owner_name : "another driver");
        return status;
    }
    memset(g, 0, sizeof(*g));
    snprintf(g->name, sizeof(g->name), "spi@%llx", (unsigned long long)se->base);
    g->controller = (struct kshim_spi_controller){
        .name = g->name, .node = node, .num_chipselect = GENI_SPI_CHIP_SELECTS,
        .ops = &GeniSpiOps, .priv = g,
    };
    uint32_t hz;
    uint8_t mode, cs;
    first_device(fdt, node, &hz, &mode, &cs);
    status = kshim_geni_engine_start(&g->engine, se, KSHIM_QUP_PROTOCOL_SPI, hz, mode, cs);
    if (!status) {
        status = kshim_spi_controller_register(&g->controller);
        if (status) (void)kshim_geni_engine_stop(&g->engine);
    }
    if (status) {
        (void)kshim_qup_se_unclaim(se);
        printf("SPI: %s: %s\n", g->name, kshim_io_strerror(status));
        return status;
    }
    g->used = true;
    g->controller.irq_driven = g->engine.irq_live;
    printf("SPI: %s, %s%s\n", g->name, g->engine.irq_live ? "interrupt-driven" : "polled",
           se->inherited_active ? ", inherited clocks" : "");
    *controller = &g->controller;
    return 0;
}
