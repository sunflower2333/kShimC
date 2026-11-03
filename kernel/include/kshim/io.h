/* SPDX-License-Identifier: MIT */
#pragma once
/*
 * I/O stack core (docs/io-stack.md): status codes shared by every I/O
 * subsystem, the completion object used for interrupt-driven waits with a
 * polling fallback, and the boot/handoff orchestration hooks.
 *
 * Conventions for every kshim/ I/O header (qup, i2c, spi, uart, gpio,
 * pinctrl, clk, regulator, pci, usb, hid, input_inject, touch_driver, dma):
 *  - Return 0 on success, otherwise a positive ERROR_* (error_no.h) or
 *    KSHIM_IO_ERR_* code. Never a negative value. CrDK CR_BUS_* results are
 *    converted with kshim_io_err_from_bus().
 *  - "Thread context" means the boot CPU with IRQs unmasked, outside any
 *    interrupt handler. Bus transfers (I2C, SPI), USB synchronous requests,
 *    PCI enumeration, regulator/clock changes and every init/quiesce call
 *    are thread-context only. Only functions documented "IRQ-safe" may be
 *    called from a handler.
 *  - Interrupt handlers only acknowledge/mask their source and record that
 *    work is pending; the work runs in thread context (the deferred,
 *    "threaded IRQ" model). The IRQ ends kshim_wait_event()/WFI, so the
 *    event loop or a kshim_completion_wait() picks the work up at once.
 *    Every driver keeps a polling fallback for an interrupt that does not
 *    resolve, is sealed by its cascade, or is never delivered.
 *  - Everything a subsystem changes is snapshot before the first write and
 *    restored by its quiesce hook (see kshim_io_quiesce()). Quiesce hooks
 *    are idempotent and must succeed before payload handoff.
 *  - All memory is identity-mapped (VA == PA == bus address on these SoCs;
 *    PCI windows translate through the host bridge "ranges").
 */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "error_no.h"

/* ---- Status codes ------------------------------------------------------- */

/* Codes below KSHIM_IO_ERR_BASE are ERROR_* (error_no.h); 64..95 belong to
 * the storage layer (kshim/storage.h). */
#define KSHIM_IO_ERR_BASE 96
enum {
    KSHIM_IO_ERR_NACK = KSHIM_IO_ERR_BASE, /* I2C address or data NACK */
    KSHIM_IO_ERR_NO_MEMORY,                /* allocation failed */
    KSHIM_IO_ERR_NO_DEVICE,                /* nothing answered (PCI all-ones, no USB response) */
    KSHIM_IO_ERR_STALL,                    /* USB endpoint STALL */
    KSHIM_IO_ERR_SHORT,                    /* short transfer where not allowed */
    KSHIM_IO_ERR_CANCELLED,                /* request cancelled (disconnect, quiesce) */
    KSHIM_IO_ERR_OVERFLOW,                 /* babble, buffer or queue overrun */
    KSHIM_IO_ERR_PROTOCOL,                 /* malformed data from a device */
    KSHIM_IO_ERR_EXISTS,                   /* already registered */
    KSHIM_IO_ERR_QUIESCED,                 /* subsystem already quiesced for handoff */
    KSHIM_IO_ERR_PENDING,                  /* request still in flight */
};

/* CrDK enum CrIoResult (Library/cr_geni.h) to a kShimC code. The literal
 * values keep this header free of CrDK includes. */
static inline int kshim_io_err_from_bus(int bus_status)
{
    switch (bus_status) {
    case 0: return 0;
    case -121: return KSHIM_IO_ERR_NACK;      /* CR_BUS_NACK */
    case -110: return ERROR_TIMEOUT;          /* CR_BUS_TIMEOUT */
    case -16: return ERROR_BUSY;              /* CR_BUS_BUSY */
    case -22: return ERROR_INVALID_PARAMETER; /* CR_BUS_INVALID */
    case -95: return ERROR_NOT_SUPPORTED;     /* CR_BUS_UNSUPPORTED */
    default: return ERROR_DEVICE_ERROR;       /* CR_BUS_IO, CR_BUS_AGAIN, ... */
    }
}

/* Static, never-NULL name of an ERROR_* or KSHIM_IO_ERR_* code. */
const char *kshim_io_strerror(int status);

/* ---- Completion --------------------------------------------------------- */

struct kshim_completion {
    volatile uint32_t done;
    volatile int status;
};

static inline void kshim_completion_init(struct kshim_completion *c)
{
    c->done = 0;
    c->status = 0;
}

static inline bool kshim_completion_done(const struct kshim_completion *c)
{
    return c->done != 0;
}

/* IRQ-safe, any CPU: record status and mark done (with a release barrier). */
void kshim_completion_complete(struct kshim_completion *c, int status);

typedef void (*kshim_io_poll_fn)(void *context);

/* Thread context. Wait until c is done and return its status, or return
 * ERROR_TIMEOUT after timeout_us (the request is then still pending and the
 * caller must cancel it). poll(context), when given, runs before every check:
 * it is the polling fallback and the event pump (for example
 * kshim_usb_poll_events). Between checks the CPU sleeps in WFI for at most
 * 1 ms while interrupts are live, so a completing interrupt ends the wait at
 * once; without interrupts it busy-waits in short steps. */
int kshim_completion_wait(struct kshim_completion *c, uint32_t timeout_us,
                          kshim_io_poll_fn poll, void *context);

/* True on the CPU that ran kshim_io_init() (the boot CPU). Pumps and
 * thread-context-only APIs return early elsewhere. */
bool kshim_io_on_boot_cpu(void);

/* ---- Orchestration (kernel/core/io.c) ----------------------------------- */

struct kshim_resources;

/* kshim_resources_init(), before the GIC and before the MMU is built: each
 * configured subsystem adds its MMIO windows (DT only, no device access).
 * A subsystem whose mapping fails is disabled and its partial mappings are
 * rolled back; the others continue. Always returns 0. Order: GPIO/pinctrl,
 * clocks, regulators, QUP, PCI. */
int kshim_io_map_resources(struct kshim_resources *resources);

/* kshim_main(), after storage (the Renesas xHCI firmware comes from the
 * Android partitions): GPIO, clocks, regulators, QUP discovery, USB core
 * and class drivers, then PCI enumeration and driver binding (which starts
 * the xHCI and the initial bounded USB enumeration). Touch is not started
 * here; kshim_input_touch_start() does that for the menu. Failures are
 * logged and leave the failing subsystem off. Returns 0. */
int kshim_io_init(const void *fdt);

/* kshim_platform_quiesce() step 3a, after input quiesce, LVGL and worker
 * shutdown, before the irqchip restore and kshim_irq_teardown():
 *   USB devices/class drivers -> USB host controllers (xHCI)
 *   -> PCI drivers, config journal and host bridges
 *   -> touch controllers -> I2C -> SPI -> QUP serial engines (not the console)
 *   -> clocks -> regulators -> GPIO/pinctrl snapshots.
 * Idempotent. Non-zero means some step failed and payload handoff must be
 * refused (every later step still runs). */
int kshim_io_quiesce(void);
