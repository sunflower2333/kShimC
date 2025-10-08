#include <arm64_mmu.h>

.section .data
.align 12

pg_table_l0:
    .space ARM64_PAGE_TABLE_SIZE
pg_table_l1:
    .space ARM64_PAGE_TABLE_SIZE
pg_table_l2:
    .space ARM64_PAGE_TABLE_SIZE
pg_table_l3:
    .space ARM64_PAGE_SIZE

// Call after stack is set up
// void _arm64_mmu_pt_setup(uint64_t *image_start_addr)
_arm64_mmu_setup:
    // Setup page tables in C
    bl arm64_mmu_pt_setup
    // enable the MMU
    bl arm64_unset_alignment_check
    bl arm64_set_stack_alignment_check
    bl arm64_set_instruction_cache
    bl arm64_set_data_cache
    bl arm64_enable_mmu
    ret
