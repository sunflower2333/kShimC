/* SPDX-License-Identifier: MIT */
#pragma once
/*
 * Single-instruction MMIO accessors. Every device register access in kShimC
 * goes through these.
 *
 * Under a hypervisor (Qualcomm's on SM8450 and later), trapped regions such as
 * the vGIC distributor and redistributors are emulated from the data-abort
 * syndrome. The syndrome is only valid (ESR_ELx.ISV=1) for a single LDR/STR
 * without writeback. A plain volatile dereference lets the compiler pick
 * post-indexed, pair or vector forms: gicv3_init's ISENABLER save loop
 * compiled to "ldr w2, [x22], #4", and the SM8450 hypervisor answered with a
 * synchronous external abort. Like Linux's __raw_readl/__raw_writel, the
 * accessors are inline asm with a plain base register.
 *
 * Host tests (KSHIM_HOST_TEST) and non-AArch64 builds fall back to volatile
 * accesses, so register models in host memory keep working.
 */
#include <stdint.h>

#if defined(__aarch64__) && !defined(KSHIM_HOST_TEST)
static inline uint8_t kshim_mmio_read8(uintptr_t address)
{
    uint8_t value;
    __asm__ volatile("ldrb %w0, [%1]" : "=r"(value) : "r"(address) : "memory");
    return value;
}
static inline uint16_t kshim_mmio_read16(uintptr_t address)
{
    uint16_t value;
    __asm__ volatile("ldrh %w0, [%1]" : "=r"(value) : "r"(address) : "memory");
    return value;
}
static inline uint32_t kshim_mmio_read32(uintptr_t address)
{
    uint32_t value;
    __asm__ volatile("ldr %w0, [%1]" : "=r"(value) : "r"(address) : "memory");
    return value;
}
static inline uint64_t kshim_mmio_read64(uintptr_t address)
{
    uint64_t value;
    __asm__ volatile("ldr %0, [%1]" : "=r"(value) : "r"(address) : "memory");
    return value;
}
static inline void kshim_mmio_write8(uintptr_t address, uint8_t value)
{
    __asm__ volatile("strb %w0, [%1]" : : "rZ"(value), "r"(address) : "memory");
}
static inline void kshim_mmio_write16(uintptr_t address, uint16_t value)
{
    __asm__ volatile("strh %w0, [%1]" : : "rZ"(value), "r"(address) : "memory");
}
static inline void kshim_mmio_write32(uintptr_t address, uint32_t value)
{
    __asm__ volatile("str %w0, [%1]" : : "rZ"(value), "r"(address) : "memory");
}
static inline void kshim_mmio_write64(uintptr_t address, uint64_t value)
{
    __asm__ volatile("str %x0, [%1]" : : "rZ"(value), "r"(address) : "memory");
}
#else
static inline uint8_t kshim_mmio_read8(uintptr_t address) { return *(volatile uint8_t *)address; }
static inline uint16_t kshim_mmio_read16(uintptr_t address) { return *(volatile uint16_t *)address; }
static inline uint32_t kshim_mmio_read32(uintptr_t address) { return *(volatile uint32_t *)address; }
static inline uint64_t kshim_mmio_read64(uintptr_t address) { return *(volatile uint64_t *)address; }
static inline void kshim_mmio_write8(uintptr_t address, uint8_t value) { *(volatile uint8_t *)address = value; }
static inline void kshim_mmio_write16(uintptr_t address, uint16_t value) { *(volatile uint16_t *)address = value; }
static inline void kshim_mmio_write32(uintptr_t address, uint32_t value) { *(volatile uint32_t *)address = value; }
static inline void kshim_mmio_write64(uintptr_t address, uint64_t value) { *(volatile uint64_t *)address = value; }
#endif
