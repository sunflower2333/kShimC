/* SPDX-License-Identifier: MIT */
#pragma once
/*
 * PCI core, host-bridge interface and driver registry (CONFIG_KSHIM_PCI).
 * Owner: pci (kernel/drivers/pci/). See docs/io-stack.md.
 *
 * Host bridges (one struct kshim_pci_host each):
 *  - generic ECAM, "pci-host-ecam-generic" (QEMU virt gpex), CONFIG_KSHIM_PCI_ECAM;
 *  - Qualcomm DesignWare RC, "qcom,pcie-sm8650"/"qcom,pcie-sm8550"/
 *    "qcom,pcie-sm8250"/..., CONFIG_KSHIM_PCI_QCOM: inherit a live link
 *    (LTSSM L0, DBI/iATU readable) and own only an iATU CFG window, else cold
 *    init through CrDK PcieLib (PcieLibInit/PcieInitializePort/
 *    PcieConfigAccess/PcieShutdownPort) with a PcieIoOps adapter onto
 *    kshim_clk/kshim_regulator/kshim_gpio and per-SoC PcieTargetContext data.
 *
 * kshim_pci_init() order per host: parse DT (bus-range, linux,pci-domain,
 * ranges -> windows, dma-coherent, interrupt-map), power the DT endpoints
 * under its root ports (every "*-supply" and pinctrl "default" of child
 * nodes, kshim_regulator_bulk_enable/kshim_pinctrl_apply) while PERST is
 * still asserted, ops->start (link), scan, configure bridges, size and
 * assign BARs, resolve INTx, bind drivers.
 *
 * Inheritance: a device or bridge whose BARs/windows/bus numbers the
 * previous firmware assigned inside this host's windows keeps them. Only
 * unassigned or conflicting resources are (re)assigned.
 *
 * Config writes through kshim_pci_write_config*() are journaled: the first
 * write to each dword records its inherited value, and kshim_pci_quiesce()
 * replays the journal newest first after the drivers quiesced. That covers
 * COMMAND (MEM/IO/BME/INTx disable), BARs, bridge bus numbers and windows,
 * MSI. kshim_pci_write_config_raw*() bypasses the journal for registers
 * whose write has a side effect that must not be undone (for example the
 * Renesas uPD720201 firmware download registers 0xf4-0xfc).
 */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "error_no.h"
#include <kshim/irq.h>

#define KSHIM_PCI_MAX_HOSTS   4U
#define KSHIM_PCI_MAX_WINDOWS 4U
#define KSHIM_PCI_MAX_DEVICES 32U
#define KSHIM_PCI_ANY_ID      0xffffU

#define KSHIM_PCI_DEVFN(dev, fn) ((uint8_t)((((dev) & 0x1fU) << 3) | ((fn) & 0x7U)))
#define KSHIM_PCI_SLOT(devfn)    (((devfn) >> 3) & 0x1fU)
#define KSHIM_PCI_FUNC(devfn)    ((devfn) & 0x7U)

/* Standard header offsets used across subsystems. */
#define KSHIM_PCI_VENDOR_ID      0x00
#define KSHIM_PCI_DEVICE_ID      0x02
#define KSHIM_PCI_COMMAND        0x04
#define KSHIM_PCI_COMMAND_IO     0x0001U
#define KSHIM_PCI_COMMAND_MEMORY 0x0002U
#define KSHIM_PCI_COMMAND_MASTER 0x0004U
#define KSHIM_PCI_COMMAND_INTX_DISABLE 0x0400U
#define KSHIM_PCI_STATUS         0x06
#define KSHIM_PCI_CLASS_REVISION 0x08
#define KSHIM_PCI_HEADER_TYPE    0x0e
#define KSHIM_PCI_BAR0           0x10
#define KSHIM_PCI_INTERRUPT_PIN  0x3d
#define KSHIM_PCI_CAP_ID_PM      0x01
#define KSHIM_PCI_CAP_ID_MSI     0x05
#define KSHIM_PCI_CAP_ID_EXP     0x10
#define KSHIM_PCI_CAP_ID_MSIX    0x11

/* class_code = base << 16 | sub << 8 | prog-if. */
#define KSHIM_PCI_CLASS_SERIAL_USB_XHCI 0x0c0330U
#define KSHIM_PCI_CLASS_BRIDGE_PCI      0x060400U

enum kshim_pci_window_type {
    KSHIM_PCI_WINDOW_IO = 1,
    KSHIM_PCI_WINDOW_MEM32,
    KSHIM_PCI_WINDOW_MEM64,   /* 64-bit; prefetchable when .prefetchable */
};

/* One "ranges" entry: CPU address cpu_base maps to bus address pci_base. */
struct kshim_pci_window {
    uint8_t type;             /* enum kshim_pci_window_type */
    bool prefetchable;
    uint64_t cpu_base, pci_base, size;
    uint64_t next;            /* allocator cursor (bus address), owned by the core */
};

struct kshim_pci_host;
struct kshim_pci_dev;

struct kshim_pci_host_ops {
    /* Bring the link up or verify the inherited one. Called after the
     * endpoints were powered. ERROR_NOT_READY: no link (host stays
     * registered, nothing is scanned below the root port). */
    int (*start)(struct kshim_pci_host *host);
    /* Config access, width 1/2/4, naturally aligned, reg < 0x1000. An absent
     * function reads all-ones and returns 0 (the core checks the vendor ID);
     * ERROR_NOT_FOUND only for a BDF the host cannot address. */
    int (*read)(struct kshim_pci_host *host, uint8_t bus, uint8_t devfn, uint16_t reg,
                unsigned width, uint32_t *value);
    int (*write)(struct kshim_pci_host *host, uint8_t bus, uint8_t devfn, uint16_t reg,
                 unsigned width, uint32_t value);
    /* Optional MSI through the host (DesignWare internal MSI controller on
     * the "msi0".."msi7" SPIs). Program nothing on the device: return the
     * MSI address/data and dispatch handler(intid, context) on that vector.
     * NULL: INTx only (QEMU: the GICv3 ITS is not driven by kShimC). */
    int (*msi_setup)(struct kshim_pci_host *host, struct kshim_pci_dev *dev,
                     kshim_irq_handler_t handler, void *context,
                     uint64_t *address, uint32_t *data);
    void (*msi_teardown)(struct kshim_pci_host *host, struct kshim_pci_dev *dev);
    /* Restore the host after every device quiesced: inherited link -> put the
     * iATU/PARF snapshot back; cold-initialised -> PcieShutdownPort (PERST
     * asserted, resources released). Idempotent. */
    int (*quiesce)(struct kshim_pci_host *host);
};

struct kshim_pci_host {
    const char *name;
    int node;                   /* host bridge DT node */
    uint16_t domain;            /* linux,pci-domain (else registration index) */
    uint8_t bus_start, bus_end; /* bus-range */
    struct kshim_pci_window windows[KSHIM_PCI_MAX_WINDOWS];
    unsigned window_count;
    bool dma_coherent;
    bool inherited;             /* link/config space left live by the previous firmware */
    const struct kshim_pci_host_ops *ops;
    void *priv;
    /* Owned by the core. */
    bool started, quiesced;
    struct kshim_pci_host *next;
};

#define KSHIM_PCI_BAR_IO       0x01U
#define KSHIM_PCI_BAR_MEM64    0x02U
#define KSHIM_PCI_BAR_PREFETCH 0x04U

struct kshim_pci_bar {
    uint64_t cpu_addr;          /* CPU (identity VA) address, 0 when unassigned */
    uint64_t bus_addr;          /* value programmed into the BAR */
    uint64_t size;
    uint8_t flags;              /* KSHIM_PCI_BAR_* */
    bool inherited;             /* left as the previous firmware assigned it */
};

struct kshim_pci_driver;
struct kshim_pci_dev {
    struct kshim_pci_host *host;
    struct kshim_pci_dev *bridge;   /* upstream bridge, NULL on the root bus */
    uint8_t bus, devfn;
    uint16_t vendor, device;
    uint16_t subsystem_vendor, subsystem_device;
    uint32_t class_code;            /* base << 16 | sub << 8 | prog-if */
    uint8_t revision, header_type;
    uint8_t secondary_bus, subordinate_bus; /* bridges */
    struct kshim_pci_bar bars[6];
    uint8_t cap_pm, cap_msi, cap_msix, cap_exp; /* capability offsets, 0 = absent */
    uint8_t irq_pin;                /* 1..4 = INTA..INTD, 0 = none */
    uint32_t intx_intid;            /* resolved GIC INTID (after swizzle + interrupt-map), 0 = none */
    unsigned intx_trigger;          /* enum kshim_irq_trigger */
    int node;                       /* matching DT node (PCI unit address), -1 = none */
    const struct kshim_pci_driver *driver;
    void *driver_data;
    /* Owned by the core. */
    bool enabled, msi, irq_requested;
    struct kshim_pci_dev *next;
};

/* Matching: vendor/device (KSHIM_PCI_ANY_ID wildcards) and class_code under
 * class_mask (0 = any class). The table ends with an all-zero entry. */
struct kshim_pci_device_id {
    uint16_t vendor, device;
    uint32_t class_code, class_mask;
    uintptr_t data;
};

struct kshim_pci_driver {
    const char *name;
    const struct kshim_pci_device_id *ids;
    /* Thread context, once per matching function; non-zero leaves the
     * function unbound (the next matching driver is tried). */
    int (*probe)(struct kshim_pci_dev *dev, const struct kshim_pci_device_id *id);
    /* Stop DMA and interrupts (handoff). The core then frees IRQs, replays
     * the config journal and quiesces the host. Idempotent. */
    int (*quiesce)(struct kshim_pci_dev *dev);
    struct kshim_pci_driver *next; /* owned by the core */
};

/* Drivers register before kshim_pci_init() binds (kshim_io_init orders
 * this); a driver registered later is bound immediately. First matching
 * driver in registration order wins. */
int kshim_pci_driver_register(struct kshim_pci_driver *driver);

/* Host providers: parse the generic DT properties into `host`, then register. */
int kshim_pci_host_parse_dt(const void *fdt, int node, struct kshim_pci_host *host);
int kshim_pci_host_register(struct kshim_pci_host *host);

/* kshim_io_init(): probe every available host node, start, enumerate, bind. */
int kshim_pci_init(const void *fdt);
struct kshim_resources;
/* Config window (ECAM limited to bus-range x 1 MiB, or DBI/ELBI/ATU/config
 * and PARF/PHY for Qualcomm) and every "ranges" window, device RW. */
int kshim_pci_map_resources(struct kshim_resources *resources);

/* Lookup over enumerated functions; from = NULL starts at the first. */
struct kshim_pci_dev *kshim_pci_find_device(uint16_t vendor, uint16_t device,
                                            struct kshim_pci_dev *from);
struct kshim_pci_dev *kshim_pci_find_class(uint32_t class_code, uint32_t class_mask,
                                           struct kshim_pci_dev *from);

/* Config access (thread context). Writes are journaled; *_raw are not. */
int kshim_pci_read_config8(struct kshim_pci_dev *dev, uint16_t reg, uint8_t *value);
int kshim_pci_read_config16(struct kshim_pci_dev *dev, uint16_t reg, uint16_t *value);
int kshim_pci_read_config32(struct kshim_pci_dev *dev, uint16_t reg, uint32_t *value);
int kshim_pci_write_config8(struct kshim_pci_dev *dev, uint16_t reg, uint8_t value);
int kshim_pci_write_config16(struct kshim_pci_dev *dev, uint16_t reg, uint16_t value);
int kshim_pci_write_config32(struct kshim_pci_dev *dev, uint16_t reg, uint32_t value);
int kshim_pci_write_config_raw32(struct kshim_pci_dev *dev, uint16_t reg, uint32_t value);

/* Offset of a standard / extended capability, 0 when absent. */
uint16_t kshim_pci_find_capability(struct kshim_pci_dev *dev, uint8_t id);
uint16_t kshim_pci_find_ext_capability(struct kshim_pci_dev *dev, uint16_t id);

/* Set MEMORY (and IO when the function has IO BARs) and, with bus_master,
 * MASTER in COMMAND (journaled). */
int kshim_pci_enable_device(struct kshim_pci_dev *dev, bool bus_master);
/* CPU pointer to an assigned memory BAR (identity mapping), NULL otherwise. */
volatile void *kshim_pci_bar_map(struct kshim_pci_dev *dev, unsigned bar);
/* Device-visible address of DMA memory at bus address `bus` (kshim_dma_alloc),
 * applying the host's dma-ranges translation (identity today). */
uint64_t kshim_pci_dma_addr(const struct kshim_pci_dev *dev, uint64_t bus);

#define KSHIM_PCI_IRQ_INTX 0x1U
#define KSHIM_PCI_IRQ_MSI  0x2U
/* Request the function's interrupt: MSI when allowed and the host offers
 * it, else INTx (the core shares one GIC handler per INTx line and calls
 * every registered function's handler; handlers must tolerate spurious
 * calls). *intid receives the INTID for diagnostics. ERROR_NOT_SUPPORTED:
 * nothing resolves, the driver must poll. */
int kshim_pci_request_irq(struct kshim_pci_dev *dev, unsigned allowed,
                          kshim_irq_handler_t handler, void *context, uint32_t *intid);
int kshim_pci_free_irq(struct kshim_pci_dev *dev);

/* Handoff (kshim_io_quiesce, after USB): drivers' quiesce newest first,
 * IRQs freed, config journal replayed, hosts quiesced. Idempotent. */
int kshim_pci_quiesce(void);
