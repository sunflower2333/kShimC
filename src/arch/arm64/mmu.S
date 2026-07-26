#include <asm/asm_macro.h>
#include <asm/arm64_mmu.h>

.section .data

pg_table_l0:
#ifdef CONFIG_ARM64_PAGE_SIZE_4K
    .align 12 // Aligned to 4Kib
#elif defined(CONFIG_ARM64_PAGE_SIZE_16K)
    .align 14 // Aligned to 16Kib
#elif defined(CONFIG_ARM64_PAGE_SIZE_64K)
    .align 16 // Aligned to 64Kib
#else
#error "Unknown page size"
#endif
    .space ARM64_PAGE_TABLE_SIZE
pg_table_l1:
#ifdef CONFIG_ARM64_PAGE_SIZE_4K
    .align 12 // Aligned to 4Kib
#elif defined(CONFIG_ARM64_PAGE_SIZE_16K)
    .align 14 // Aligned to 16Kib
#elif defined(CONFIG_ARM64_PAGE_SIZE_64K)
    .align 16 // Aligned to 64Kib
#else
#error "Unknown page size"
#endif
    .space ARM64_PAGE_TABLE_SIZE
pg_table_l2:
#ifdef CONFIG_ARM64_PAGE_SIZE_4K
    .align 12 // Aligned to 4Kib
#elif defined(CONFIG_ARM64_PAGE_SIZE_16K)
    .align 14 // Aligned to 16Kib
#elif defined(CONFIG_ARM64_PAGE_SIZE_64K)
    .align 16 // Aligned to 64Kib
#else
#error "Unknown page size"
#endif

    .space ARM64_PAGE_TABLE_SIZE
#ifndef CONFIG_ARM64_PAGE_SIZE_64K
// 64K only use 3-level page table
pg_table_l3:
#ifdef CONFIG_ARM64_PAGE_SIZE_4K
    .align 12 // Aligned to 4Kib
#elif defined(CONFIG_ARM64_PAGE_SIZE_16K)
    .align 14 // Aligned to 16Kib
#elif defined(CONFIG_ARM64_PAGE_SIZE_64K)
    .align 16 // Aligned to 64Kib
#else
#error "Unknown page size"
#endif
    .space ARM64_PAGE_TABLE_SIZE
#endif /* CONFIG_ARM64_PAGE_SIZE_64K */

// Call after stack setting up
ASM_FUNC(_arm64_mmu_setup)
    // Set ttbr1_el1 to L-1 table address
    adrp x0, pg_table_root
    add x0, x0, :lo12:pg_table_root
    msr ttbr1_el1, x0
    // Setup page tables in C
    // Get kernel start physical address
    adrp x0, __image_start_address__         // load address of program
    add x0, x0, :lo12:__image_start_address__
    // Get kernel end physical address
    adrp x1, __image_end_address__         // load address of program
    add x1, x1, :lo12:__image_end_address__
    bl arm64_mmu_setup
    // enable the MMU
    // bl arm64_unset_alignment_check
    // bl arm64_set_stack_alignment_check
    // bl arm64_set_instruction_cache
    // bl arm64_set_data_cache
    // bl arm64_enable_mmu
    ret
