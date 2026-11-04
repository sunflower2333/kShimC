/* SPDX-License-Identifier: MIT */
/*
 * Generic ECAM host bridge, "pci-host-ecam-generic" (QEMU virt gpex)
 * (CONFIG_KSHIM_PCI_ECAM). Configuration space is memory mapped at
 * reg[0] + (bus - bus_start) << 20 | devfn << 12 | register (PCI Express
 * Base 7.2.2, Linux host-generic-pci.yaml). Absent functions read all-ones
 * (the host returns UR completions as all-ones). No MSI: the GICv3 ITS
 * that "msi-map" points at is not driven by kShimC, so functions use INTx.
 */
#include "pci_internal.h"

#include <config.h>
#include <dt.h>
#include <stdio.h>
#include <string.h>

#define ECAM_COMPATIBLE "pci-host-ecam-generic"

struct ecam_host {
    struct kshim_pci_host host;
    uint64_t base, size;
};

static struct ecam_host Ecam[KSHIM_PCI_MAX_HOSTS];
static unsigned EcamCount;

static struct ecam_host *ecam(struct kshim_pci_host *host)
{
    return (struct ecam_host *)host;
}

static int address(struct kshim_pci_host *host, uint8_t bus, uint8_t devfn, uint16_t reg,
                   unsigned width, uintptr_t *out)
{
    struct ecam_host *e = ecam(host);
    if ((width != 1 && width != 2 && width != 4) || reg >= 0x1000U || (reg & (width - 1U)))
        return ERROR_INVALID_PARAMETER;
    if (bus < host->bus_start || bus > host->bus_end) return ERROR_NOT_FOUND;
    uint64_t offset = ((uint64_t)(bus - host->bus_start) << 20) | ((uint64_t)devfn << 12) | reg;
    if (offset >= e->size) return ERROR_NOT_FOUND;
    *out = (uintptr_t)(e->base + offset);
    return 0;
}

static int ecam_read(struct kshim_pci_host *host, uint8_t bus, uint8_t devfn, uint16_t reg,
                     unsigned width, uint32_t *value)
{
    uintptr_t a;
    int status = address(host, bus, devfn, reg, width, &a);
    if (status) return status;
    *value = pci_mmio_read(a, width);
    return 0;
}

static int ecam_write(struct kshim_pci_host *host, uint8_t bus, uint8_t devfn, uint16_t reg,
                      unsigned width, uint32_t value)
{
    uintptr_t a;
    int status = address(host, bus, devfn, reg, width, &a);
    if (status) return status;
    pci_mmio_write(a, width, value);
    return 0;
}

static int ecam_start(struct kshim_pci_host *host)
{
    (void)host;
    return 0;
}

static int ecam_quiesce(struct kshim_pci_host *host)
{
    (void)host;
    return 0;
}

static const struct kshim_pci_host_ops EcamOps = {
    .start = ecam_start,
    .read = ecam_read,
    .write = ecam_write,
    .quiesce = ecam_quiesce,
};

/* reg[0] limited to the buses of "bus-range" (1 MiB each). */
static int parse(const void *f, int node, struct ecam_host *e)
{
    memset(e, 0, sizeof(*e));
    struct dt_range reg;
    if (dt_reg(f, node, 0, &reg)) return ERROR_INVALID_PARAMETER;
    int status = kshim_pci_host_parse_dt(f, node, &e->host);
    if (status) return status;
    uint64_t buses = (uint64_t)(e->host.bus_end - e->host.bus_start) + 1U;
    e->base = reg.base;
    e->size = reg.size < buses * KSHIM_PCI_ECAM_BUS_SIZE ? reg.size : buses * KSHIM_PCI_ECAM_BUS_SIZE;
    if (e->size < KSHIM_PCI_ECAM_BUS_SIZE) return ERROR_INVALID_PARAMETER;
    /* A window shorter than bus-range limits the reachable buses. */
    uint64_t reachable = e->size / KSHIM_PCI_ECAM_BUS_SIZE;
    if (reachable < buses) e->host.bus_end = (uint8_t)(e->host.bus_start + reachable - 1U);
    e->host.name = fdt_get_name(f, node, NULL);
    e->host.ops = &EcamOps;
    e->host.inherited = false;
    return 0;
}

static int map_one(struct kshim_resources *r, void *context)
{
    struct ecam_host *e = context;
    int status = kshim_pci_map_device(r, e->base, e->size);
    return status ? status : kshim_pci_map_windows(r, &e->host);
}

int kshim_pci_ecam_map(const void *f, struct kshim_resources *r)
{
    struct ecam_host scratch;
    for (int n = fdt_node_offset_by_compatible(f, -1, ECAM_COMPATIBLE); n >= 0;
         n = fdt_node_offset_by_compatible(f, n, ECAM_COMPATIBLE)) {
        if (!dt_available(f, n)) continue;
        if (parse(f, n, &scratch)) {
            kshim_pci_mark_node(n, false);
            continue;
        }
        if (kshim_pci_map_guarded(r, n, map_one, &scratch))
            printf("PCI: %s: windows not mapped, host skipped\n", scratch.host.name);
    }
    return 0;
}

int kshim_pci_ecam_probe(const void *f)
{
    int result = 0;
    for (int n = fdt_node_offset_by_compatible(f, -1, ECAM_COMPATIBLE); n >= 0;
         n = fdt_node_offset_by_compatible(f, n, ECAM_COMPATIBLE)) {
        if (!dt_available(f, n) || kshim_pci_node_unmapped(n)) continue;
        if (EcamCount == KSHIM_PCI_MAX_HOSTS) break;
        struct ecam_host *e = &Ecam[EcamCount];
        int status = parse(f, n, e);
        if (!status) status = kshim_pci_host_register(&e->host);
        if (status) {
            printf("PCI: %s: ECAM host not registered (%d)\n", fdt_get_name(f, n, NULL), status);
            result = status;
            continue;
        }
        printf("PCI: %s: ECAM 0x%llx+0x%llx buses %02x-%02x, %u windows\n", e->host.name,
               (unsigned long long)e->base, (unsigned long long)e->size, e->host.bus_start,
               e->host.bus_end, e->host.window_count);
        ++EcamCount;
    }
    return result;
}
