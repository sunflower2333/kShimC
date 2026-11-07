/* SPDX-License-Identifier: MIT */
#pragma once
/*
 * USB CDC-ACM console backend over CrDK's UsbAcmCrLib (DWC3 device mode).
 *
 * The previous bootloader must leave the controller powered and clocked;
 * nothing here touches clocks, GDSCs, PHYs or resets. The backend mirrors
 * the DT-selected UART console (kernel/drivers/console.c owns both) and is
 * serviced by polling from the write path, input polling and idle delays.
 */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct CrIo;
struct kshim_resources;

/* The DWC3 core a CDC-ACM console can use, from the runtime DT. */
struct kshim_usb_acm_resource {
    int node;                  /* snps,dwc3 node */
    uint64_t base, size;       /* DWC3 core window (the xHCI base) */
    /*
     * The core's interrupt, parsed but not registered: the backend polls
     * until the port has a GIC-backed CrRegisterInterrupt. irq_spec holds
     * the raw specifier for irq_parent; intid/trigger are decoded when the
     * parent is an Arm GIC (SPI n -> 32 + n, PPI n -> 16 + n) or, as on
     * Android SM8150 DTs, a 3-cell Qualcomm PDC in front of it (pdc).
     */
    bool has_irq, gic, pdc;
    int irq_parent;
    uint32_t irq_cells, irq_spec[4];
    uint32_t intid, trigger;
};

/*
 * Pick the DWC3 core under a Qualcomm wrapper ("qcom,dwc3" mainline or
 * "qcom,dwc-usb3-msm" Android) with an available "snps,dwc3" child that is
 * not host-only. androidboot.usbcontroller=<base>.dwc3 selects among several;
 * otherwise the first in DT order is used. 0 on success.
 */
int kshim_dt_usb_acm(const void *fdt, struct kshim_usb_acm_resource *resource);

/* Reset and start the controller as a CDC-ACM device. 0 on success; -1 bad
 * resource or already active, -2 CrUsbAcmInit, -3 CrUsbAcmStart failed. */
int kshim_usb_acm_start(const struct kshim_usb_acm_resource *resource, const struct CrIo *io);
bool kshim_usb_acm_active(void);
const struct kshim_usb_acm_resource *kshim_usb_acm_resource(void);
/* Queue bytes (kept until a host reads) and service the controller. */
void kshim_usb_acm_write(const char *buffer, uint32_t length);
/* Non-blocking: 1 and one received byte, else 0. Services the controller. */
int kshim_usb_acm_read(char *character);
void kshim_usb_acm_poll(void);
/* A host terminal has the port open (configured, DTR set). */
bool kshim_usb_acm_open(void);
/* Flush up to 500 ms, end transfers, drop the pull-up, detach the event ring. */
void kshim_usb_acm_stop(void);
/* MMIO accessors the console starts the backend with (weak: CrIoDefault()). */
const struct CrIo *kshim_usb_acm_io(void);
