/* SPDX-License-Identifier: MIT */
#pragma once
/*
 * Register access macros on top of the single-instruction accessors in
 * mmio.h (see there for why plain volatile dereferences are not used).
 * Read-modify-write forms evaluate addr once.
 */
#include <stdint.h>
#include "mmio.h"

#define KSHIM_REG_RMW(bits, addr, expr)                                  \
    ({                                                                   \
        uintptr_t _kshim_reg = (uintptr_t)(addr);                        \
        uint##bits##_t _kshim_old = kshim_mmio_read##bits(_kshim_reg);   \
        (void)_kshim_old;                                                \
        kshim_mmio_write##bits(_kshim_reg, (uint##bits##_t)(expr));      \
    })

// 8-bit MMIO operations
#define REG8_OUT(addr) kshim_mmio_read8((uintptr_t)(addr))
#define REG8_IN(addr, val) kshim_mmio_write8((uintptr_t)(addr), (uint8_t)(val))
#define REG8_IN_MEM_BARRIER(addr, val) \
    {                                  \
        REG8_IN(addr, val);            \
        asm volatile("" ::: "memory"); \
    }
#define REG8_MSK_SET(addr, msk) KSHIM_REG_RMW(8, addr, _kshim_old | (uint8_t)(msk))
#define REG8_MSK_CLR(addr, msk) KSHIM_REG_RMW(8, addr, _kshim_old & ~(uint8_t)(msk))
#define REG8_MSK_OUT(addr, msk) (REG8_OUT(addr) & (uint8_t)(msk))
#define REG8_MSK_SFT_OUT(addr, msk, sft) ((REG8_OUT(addr) & (uint8_t)(msk)) >> (sft))
#define REG8_SFT_SET(addr, sft, val) KSHIM_REG_RMW(8, addr, _kshim_old | ((uint8_t)(val) << (sft)))

// 16-bit MMIO operations
#define REG16_OUT(addr) kshim_mmio_read16((uintptr_t)(addr))
#define REG16_IN(addr, val) kshim_mmio_write16((uintptr_t)(addr), (uint16_t)(val))
#define REG16_IN_MEM_BARRIER(addr, val) \
    {                                   \
        REG16_IN(addr, val);            \
        asm volatile("" ::: "memory");  \
    }
#define REG16_MSK_SET(addr, msk) KSHIM_REG_RMW(16, addr, _kshim_old | (uint16_t)(msk))
#define REG16_MSK_CLR(addr, msk) KSHIM_REG_RMW(16, addr, _kshim_old & ~(uint16_t)(msk))
#define REG16_MSK_OUT(addr, msk) (REG16_OUT(addr) & (uint16_t)(msk))
#define REG16_MSK_SFT_OUT(addr, msk, sft) ((REG16_OUT(addr) & (uint16_t)(msk)) >> (sft))
#define REG16_SFT_SET(addr, sft, val) KSHIM_REG_RMW(16, addr, _kshim_old | ((uint16_t)(val) << (sft)))

// 32-bit MMIO operations
#define REG32_OUT(addr) kshim_mmio_read32((uintptr_t)(addr))
#define REG32_IN(addr, val) kshim_mmio_write32((uintptr_t)(addr), (uint32_t)(val))
#define REG32_IN_MEM_BARRIER(addr, val) \
    {                                   \
        REG32_IN(addr, val);            \
        asm volatile("" ::: "memory");  \
    }
#define REG32_MSK_SET(addr, msk) KSHIM_REG_RMW(32, addr, _kshim_old | (uint32_t)(msk))
#define REG32_MSK_CLR(addr, msk) KSHIM_REG_RMW(32, addr, _kshim_old & ~(uint32_t)(msk))
#define REG32_MSK_CLR_SET(addr, msk, val) \
    KSHIM_REG_RMW(32, addr, (_kshim_old & ~(uint32_t)(msk)) | (uint32_t)(val))
#define REG32_MSK_OUT(addr, msk) (REG32_OUT(addr) & (uint32_t)(msk))
#define REG32_MSK_SFT_OUT(addr, msk, sft) ((REG32_OUT(addr) & (uint32_t)(msk)) >> (sft))
#define REG32_SFT_SET(addr, sft, val) KSHIM_REG_RMW(32, addr, _kshim_old | ((uint32_t)(val) << (sft)))

// 64-bit MMIO operations
#define REG64_OUT(addr) kshim_mmio_read64((uintptr_t)(addr))
#define REG64_IN(addr, val) kshim_mmio_write64((uintptr_t)(addr), (uint64_t)(val))
#define REG64_IN_MEM_BARRIER(addr, val) \
    {                                   \
        REG64_IN(addr, val);            \
        asm volatile("" ::: "memory");  \
    }
#define REG64_MSK_SET(addr, msk) KSHIM_REG_RMW(64, addr, _kshim_old | (uint64_t)(msk))
#define REG64_MSK_CLR(addr, msk) KSHIM_REG_RMW(64, addr, _kshim_old & ~(uint64_t)(msk))
#define REG64_MSK_OUT(addr, msk) (REG64_OUT(addr) & (uint64_t)(msk))
#define REG64_MSK_SFT_OUT(addr, msk, sft) ((REG64_OUT(addr) & (uint64_t)(msk)) >> (sft))
#define REG64_SFT_SET(addr, sft, val) KSHIM_REG_RMW(64, addr, _kshim_old | ((uint64_t)(val) << (sft)))
