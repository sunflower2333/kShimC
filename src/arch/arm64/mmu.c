#include <stdint.h>
#include "arm64_mmu.h"

// TODO: Parse from previous devicetree
#include <stdint.h>

typedef struct
{
    uint64_t base;
    uint64_t size;
    uint64_t attributes;
} ARM64MMUMemoryEntry;

ARM64MMUMemoryEntry arm64_device_memory_map[] = {
    {0x40000000, 0x40000000, 0x00000004}, // 1GB of RAM in qemu
};

void arm64_mmu_pt_setup (
    uint64_t *image_phy_base, // kernel physical base address
    uint64_t *pt0_base,
    uint64_t *pt1_base,
    uint64_t *pt2_base,
    uint64_t *pt3_base)
{
    // Map kernel to 0xffff000000000000UL on arm64.
    
}