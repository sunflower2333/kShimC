#include <stdint.h>
#include <asm/arm64_mmu.h>
#include <arm64/mmu.h>
#include <bitops.h>
#include <debug.h>

#ifdef CONFIG_ARM64_PAGE_SIZE_4K
typedef struct
{
    uint64_t attributes : 5;       // [63:59]
    uint64_t ignored1 : 8;         // [58:51]
    uint64_t res0_1 : 3;           // [50:48]
    uint64_t next_level_addr : 36; // [47:12]
    uint64_t ignored0 : 10;        // [11:2]
    uint64_t type : 2;             // should be 2'b11
} __attribute__((packed)) arm64_mmu_table_desc_t;
COMPILER_ASSERT(sizeof(arm64_mmu_table_desc_t) == 8);

typedef struct
{
    uint64_t upper_attributes : 14; // [63:50]
    uint64_t res0_2 : 2;            // [49:48]
    uint64_t output_addr : 9;       // [47:39]
    uint64_t res0_1 : 22;           // [38:17]
    uint64_t nT : 1;                // [16:16]
    uint64_t res0_0 : 3;            // [15:12]
    uint64_t lower_attributes : 10; // [11:2]
    uint64_t type : 2;              // should be 2'b01
} arm64_mmu_block_l0_t;
COMPILER_ASSERT(sizeof(arm64_mmu_block_l0_t) == 8);

typedef struct
{
    uint64_t upper_attributes : 14; // [63:50]
    uint64_t res0_2 : 2;            // [49:48]
    uint64_t output_addr : 18;      // [47:30]
    uint64_t res0_1 : 13;           // [29:17]
    uint64_t nT : 1;                // [16:16]
    uint64_t res0_0 : 3;            // [15:12]
    uint64_t lower_attributes : 10; // [11:2]
    uint64_t type : 2;              // should be 2'b01
} arm64_mmu_block_l1_t;
COMPILER_ASSERT(sizeof(arm64_mmu_block_l1_t) == 8);

typedef struct
{
    uint64_t upper_attributes : 14; // [63:50]
    uint64_t res0_2 : 2;            // [49:48]
    uint64_t output_addr : 27;      // [47:21]
    uint64_t res0_1 : 4;            // [20:17]
    uint64_t nT : 1;                // [16:16]
    uint64_t res0_0 : 3;            // [15:12]
    uint64_t lower_attributes : 10; // [11:2]
    uint64_t type : 2;              // should be 2'b01
} arm64_mmu_block_l2_t;
COMPILER_ASSERT(sizeof(arm64_mmu_block_l2_t) == 8);

typedef struct
{
    uint64_t upper_attributes : 14; // [63:50]
    uint64_t res0 : 2;              // [49:48]
    uint64_t output_addr : 36;      // [47:12]
    uint64_t lower_attributes : 10; // [11:2]
    uint64_t type : 2;              // should be 2'b11
} arm64_mmu_page_t;
COMPILER_ASSERT(sizeof(arm64_mmu_page_t) == 8);

#define NEXT_LEVEL_ADDRESS_L1(next_level_address) (((uint64_t)next_level_address) >> 12 & 0xFFFFFFFFFULL)
#define LVL1_BLK_SIZE SIZE_G(1)
#define LVL2_BLK_SIZE SIZE_M(2)

#elif defined(CONFIG_ARM64_PAGE_SIZE_16K)
typedef struct
{
    uint64_t attributes : 5;       // [63:59]
    uint64_t ignored1 : 8;         // [58:51]
    uint64_t res0_1 : 3;           // [50:48]
    uint64_t next_level_addr : 34; // [47:14]
    uint64_t res0 : 2;             // [12:13]
    uint64_t ignored0 : 10;        // [11:2]
    uint64_t type : 2;             // should be 2'b11
} arm64_mmu_table_desc_t;
COMPILER_ASSERT(sizeof(arm64_mmu_table_desc_t) == 8);

typedef struct
{
    uint64_t upper_attributes : 14; // [63:50]
    uint64_t res0_2 : 2;            // [49:48]
    uint64_t output_addr : 12;      // [47:36]
    uint64_t res0_1 : 19;           // [35:17]
    uint64_t nT : 1;                // [16:16]
    uint64_t res0_0 : 4;            // [15:12]
    uint64_t lower_attributes : 10; // [11:2]
    uint64_t type : 2;              // should be 2'b01
} arm64_mmu_block_l1_t;
COMPILER_ASSERT(sizeof(arm64_mmu_block_l1_t) == 8);

typedef struct
{
    uint64_t upper_attributes : 14; // [63:50]
    uint64_t res0_2 : 2;            // [49:48]
    uint64_t output_addr : 23;      // [47:25]
    uint64_t res0_1 : 8;            // [24:17]
    uint64_t nT : 1;                // [16:16]
    uint64_t res0_0 : 4;            // [15:12]
    uint64_t lower_attributes : 10; // [11:2]
    uint64_t type : 2;              // should be 2'b01
} arm64_mmu_block_l2_t;
COMPILER_ASSERT(sizeof(arm64_mmu_block_l2_t) == 8);

#define NEXT_LEVEL_ADDRESS_L1(next_level_address) (((uint64_t)next_level_address) >> 14 & 0xFFFFFFFFCULL)
#define LVL1_BLK_SIZE SIZE_G(64)
#define LVL2_BLK_SIZE SIZE_M(32)

#elif defined(CONFIG_ARM64_PAGE_SIZE_64K)
typedef struct
{
    uint64_t attributes : 5;       // [63:59]
    uint64_t ignored1 : 8;         // [58:51]
    uint64_t res0_1 : 3;           // [50:48]
    uint64_t next_level_addr : 32; // [47:16]
    uint64_t res0 : 4;             // [12:15]
    uint64_t ignored0 : 10;        // [11:2]
    uint64_t type : 2;             // should be 2'b11
} arm64_mmu_table_desc_t;
COMPILER_ASSERT(sizeof(arm64_mmu_table_desc_t) == 8);

typedef struct
{
    uint64_t upper_attributes : 14; // [63:50]
    uint64_t res0_2 : 2;            // [49:48]
    uint64_t output_addr : 6;       // [47:42]
    uint64_t res0_1 : 25;           // [41:17]
    uint64_t nT : 1;                // [16:16]
    uint64_t res0_0 : 4;            // [15:12]
    uint64_t lower_attributes : 10; // [11:2]
    uint64_t type : 2;              // should be 2'b01
} arm64_mmu_block_l1_t;
COMPILER_ASSERT(sizeof(arm64_mmu_block_l1_t) == 8);

typedef struct
{
    uint64_t upper_attributes : 14; // [63:50]
    uint64_t res0_2 : 2;            // [49:48]
    uint64_t output_addr : 19;      // [47:29]
    uint64_t res0_1 : 13;           // [29:17]
    uint64_t nT : 1;                // [16:16]
    uint64_t res0_0 : 4;            // [15:12]
    uint64_t lower_attributes : 10; // [11:2]
    uint64_t type : 2;              // should be 2'b01
} arm64_mmu_block_l2_t;
COMPILER_ASSERT(sizeof(arm64_mmu_block_l2_t) == 8);

#define NEXT_LEVEL_ADDRESS_L2(next_level_address) (((uint64_t)next_level_address) >> 16 & 0xFFFFFFFFULL)
#define LVL1_BLK_SIZE SIZE_T(4)
#define LVL2_BLK_SIZE SIZE_M(512)

#else
#error "No page size configured"
#endif

typedef struct
{
    uint64_t pa_start;   // physical address start
    uint64_t va_start;   // virtual address start
    uint64_t size;       // size of the memory region
    uint32_t attributes; // memory attributes
} arm64_memory_descriptor_t;

// add a memory mapping entry to the page table
void arm64_mmu_add_entry()
{
    // 对于内核空间，应当能够访问所有的RAM区域，这需要从FDT解析可用内存
    // 可以先使用简易版本
}

enum MMFR_PARange
{
    MMFR_PAR_32BIT, // 4GB
    MMFR_PAR_36BIT, // 64GB
    MMFR_PAR_40BIT, // 1TB
    MMFR_PAR_42BIT, // 4TB
    MMFR_PAR_44BIT, // 16TB
    MMFR_PAR_48BIT, // 256TB
    MMFR_PAR_52BIT, // 4PB
    MMFR_PAR_56BIT, // 64PB
    MMFR_PAR_INVALID = 0xF
};

// Device Memory: 0b0000dd00
#define MAIR_ATTR_DEVICE_nGnRnE 0x00 // Device nGnRnE, dd=2'b00
#define MAIR_ATTR_DEVICE_nGnRE 0x04  // Device nGnRE, dd=2'b01
#define MAIR_ATTR_DEVICE_nGRE 0x08   // Device nGRE, dd=2'b10
#define MAIR_ATTR_DEVICE_GRE 0x0C    // Device GRE, dd=2'b11

// Normal Memory
#define MAIR_ATTR_NORMAL_POLICY_NRNW 0b00                                                    // RW Allocate Not Allowed
#define MAIR_ATTR_NORMAL_POLICY_WO 0b01                                                      // Allow Write Allocate
#define MAIR_ATTR_NORMAL_POLICY_RO 0b10                                                      // Allow Read Allocate
#define MAIR_ATTR_NORMAL_POLICY_RW (MAIR_ATTR_NORMAL_POLICY_WO | MAIR_ATTR_NORMAL_POLICY_RO) // Allow RW Allocate

#define MAIR_ATTR_NORMAL_POLICY_WT(RW_POLICY) ((0b00 << 2) | (RW_POLICY))  // Write-Through Transient
#define MAIR_ATTR_NORMAL_POLICY_NC (0b01 << 2)                             // Non-Cacheable
#define MAIR_ATTR_NORMAL_POLICY_WB(RW_POLICY) ((0b01 << 2) | (RW_POLICY))  // Write-Back Transient
#define MAIR_ATTR_NORMAL_POLICY_WTN(RW_POLICY) ((0b10 << 2) | (RW_POLICY)) // Write-Through Non-Transient
#define MAIR_ATTR_NORMAL_POLICY_WBN(RW_POLICY) ((0b11 << 2) | (RW_POLICY)) // Write-Back Non-Transient
#define MAIR_ATTR_NORMAL_OUTER_SHIFT(POLICY) ((POLICY) << 4)               // Outer Cache Policy Shift

// Normal Non-Cacheable
#define MAIR_ATTR_NORMAL_NC                                       \
    ((MAIR_ATTR_NORMAL_OUTER_SHIFT(MAIR_ATTR_NORMAL_POLICY_NC)) | \
     (MAIR_ATTR_NORMAL_POLICY_NC))
// Normal Write-Back Transient
#define MAIR_ATTR_NORMAL_WB(RW_POLICY)                                       \
    ((MAIR_ATTR_NORMAL_OUTER_SHIFT(MAIR_ATTR_NORMAL_POLICY_WB(RW_POLICY))) | \
     (MAIR_ATTR_NORMAL_POLICY_WB(RW_POLICY)))
// Normal Write-Through Transient
#define MAIR_ATTR_NORMAL_WT(RW_POLICY)                                       \
    ((MAIR_ATTR_NORMAL_OUTER_SHIFT(MAIR_ATTR_NORMAL_POLICY_WT(RW_POLICY))) | \
     (MAIR_ATTR_NORMAL_POLICY_WT(RW_POLICY)))
// Normal Write-Through Non-Transient
#define MAIR_ATTR_NORMAL_WTN(RW_POLICY)                                       \
    ((MAIR_ATTR_NORMAL_OUTER_SHIFT(MAIR_ATTR_NORMAL_POLICY_WTN(RW_POLICY))) | \
     (MAIR_ATTR_NORMAL_POLICY_WTN(RW_POLICY)))
// Normal Write-Back Non-Transient
#define MAIR_ATTR_NORMAL_WBN(RW_POLICY)                                       \
    ((MAIR_ATTR_NORMAL_OUTER_SHIFT(MAIR_ATTR_NORMAL_POLICY_WBN(RW_POLICY))) | \
     (MAIR_ATTR_NORMAL_POLICY_WBN(RW_POLICY)))

// MAIR attribute id
#define MAIR_ATTR_ID_DEVICE_nGnRnE 0
#define MAIR_ATTR_ID_DEVICE_nGnRE 1
#define MAIR_ATTR_ID_NORMAL_NC 2
#define MAIR_ATTR_ID_NORMAL_WB 3
#define MAIR_ATTR_ID_NORMAL_WT 4
#define MAIR_ATTR_ID_NORMAL_WBN 5
#define MAIR_ATTR_ID_NORMAL_WTN 6

// Calculate MAIR attribute
#define MAIR_ATTR_WITH_OFFSET(ATTR, ID) ((ATTR) << ((ID) * 8))

#define TCR_TnSZ_CALC(ADDR_BITS) (64 - (ADDR_BITS))
#define TCR_TG0_SHIFTER(VAL) ((VAL) << 14)
#define TCR_T1SZ_SHIFTER(VAL) ((VAL) << 16)
#define TCR_TG1_SHIFTER(VAL) ((VAL) << 30)
#define TCR_IPS_SHIFTER(VAL) ((VAL) << 32)
#define TCR_SH0_SHIFTER(VAL) ((VAL) << 12)
#define TCR_SH1_SHIFTER(VAL) ((VAL) << 28)
#define TCR_ORGN0_SHIFTER(VAL) ((VAL) << 10)
#define TCR_ORGN1_SHIFTER(VAL) ((VAL) << 8)

extern uint64_t arm64_read_mmfr0_el1(void);
extern uint64_t arm64_write_tcr(uint64_t tcr);
extern uint64_t arm64_write_mair(uint64_t mair);

//  4KiB granule needs 5-level(-1~3) page table in 52-bit VA mode with FEAT_LVA
//  4KiB granule needs 4-level(-1~3) page table
//  16KiB granule needs 4-level(0~3) page table
//  64KiB granule needs 3-level(1~3) page table

extern uint8_t pg_table_l0[ARM64_PAGE_TABLE_SIZE];
extern uint8_t pg_table_l1[ARM64_PAGE_TABLE_SIZE];
extern uint8_t pg_table_l2[ARM64_PAGE_TABLE_SIZE];
#ifndef CONFIG_ARM64_PAGE_SIZE_64K
// 64K only use 3-level page table
extern uint8_t pg_table_l3[ARM64_PAGE_TABLE_SIZE];
#endif /* CONFIG_ARM64_PAGE_SIZE_64K */

// Table Attributes [63:59]
#define TABLE_ATTR_PXN_BTI BIT(0)
#define TABLE_ATTR_UXN_BIT BIT(1)
#define TABLE_ATTR_AP_BITS GENBITS(3, 2) // 00: EL
#define TABLE_ATTR_NS_BIT BIT(4)

void arm64_mmu_setup(
    uint64_t image_start_pa, // kernel physical start address
    uint64_t image_end_pa    // kernel physical end address
)
{
    // Read MMFR0
    uint64_t mmfr0 = arm64_read_mmfr0_el1();
    uint8_t max_address_bits = 0;

    // Check PARange
    printf("mmfr0: 0x%lx\n", mmfr0);
    ASSERT(mmfr0 & 0xF < 7);

    // Get PA Range
    max_address_bits = mmfr0 & 0xF; // [3:0] PARange

#if 0
    // TODO: Complete and validate the MMU register and page-table setup.
    // Calculate and set TCR
    arm64_write_tcr(
        (TCR_TnSZ_CALC(max_address_bits)) |                   // T0SZ
        (TCR_T1SZ_SHIFTER(TCR_TnSZ_CALC(max_address_bits))) | // T1SZ
        (TCR_TG0_SHIFTER(ARM64_TCR_TG(0))) |                  // TG0
        (TCR_TG1_SHIFTER(ARM64_TCR_TG(1))) |                  // TG1
        (TCR_IPS_SHIFTER(max_address_bits)) |                 // IPS
        (TCR_SH1_SHIFTER(0b11)) |                             // SH1 Inner Shareable
        (TCR_SH0_SHIFTER(0b11)) |                             // SH0 Inner Shareable
        (TCR_ORGN0_SHIFTER(0b01)) |                           // ORGN0 Outer Write-Back Read-Allocate Write-Allocate Cacheable
        (TCR_ORGN1_SHIFTER(0b01))                             // ORGN1 Inner Write-Back Read-Allocate Write-Allocate Cacheable
    );

    // Setup MAIR
    arm64_write_mair(
        MAIR_ATTR_WITH_OFFSET(MAIR_ATTR_DEVICE_nGnRnE, MAIR_ATTR_ID_DEVICE_nGnRnE) |
        MAIR_ATTR_WITH_OFFSET(MAIR_ATTR_DEVICE_nGnRE, MAIR_ATTR_ID_DEVICE_nGnRE) |
        MAIR_ATTR_WITH_OFFSET(MAIR_ATTR_NORMAL_NC, MAIR_ATTR_ID_NORMAL_NC) |
        MAIR_ATTR_WITH_OFFSET(MAIR_ATTR_NORMAL_WB(MAIR_ATTR_NORMAL_POLICY_RW), MAIR_ATTR_ID_NORMAL_WB) |
        MAIR_ATTR_WITH_OFFSET(MAIR_ATTR_NORMAL_WT(MAIR_ATTR_NORMAL_POLICY_RW), MAIR_ATTR_ID_NORMAL_WT) |
        MAIR_ATTR_WITH_OFFSET(MAIR_ATTR_NORMAL_WBN(MAIR_ATTR_NORMAL_POLICY_RW), MAIR_ATTR_ID_NORMAL_WBN) |
        MAIR_ATTR_WITH_OFFSET(MAIR_ATTR_NORMAL_WTN(MAIR_ATTR_NORMAL_POLICY_RW), MAIR_ATTR_ID_NORMAL_WTN));

    // For 4KiB granule, we need 4-level page table
    arm64_mmu_table_desc_t *table_level_0[ARM64_PAGE_TABLE_SIZE / sizeof(arm64_mmu_table_desc_t)] = (arm64_mmu_table_desc_t *)pg_table_l0;
    arm64_mmu_table_desc_t *table_level_1[ARM64_PAGE_TABLE_SIZE / sizeof(arm64_mmu_table_desc_t)] = (arm64_mmu_table_desc_t *)pg_table_l1;
    arm64_mmu_table_desc_t *table_level_2[ARM64_PAGE_TABLE_SIZE / sizeof(arm64_mmu_table_desc_t)] = (arm64_mmu_table_desc_t *)pg_table_l2;
#ifndef CONFIG_ARM64_PAGE_SIZE_64K
    arm64_mmu_table_desc_t *table_level_3[ARM64_PAGE_TABLE_SIZE / sizeof(arm64_mmu_table_desc_t)] = (arm64_mmu_table_desc_t *)pg_table_l3;
#endif

    // Clear root page table
    memset(table_level_0, 0, sizeof(table_level_0));
    memset(table_level_1, 0, sizeof(table_level_1));
    memset(table_level_2, 0, sizeof(table_level_2));
#ifndef CONFIG_ARM64_PAGE_SIZE_64K
    memset(table_level_3, 0, sizeof(table_level_3));
#endif

    // Point to next level page table
    table_level_0[0]->type = 0b11; // table descriptor
    table_level_0[0]->next_level_addr = NEXT_LEVEL_ADDRESS_L1(pg_table_l1);

    // Set default table attributes
    table_level_0[0]->attributes = (TABLE_ATTR_PXN_BTI | // 1
                                    TABLE_ATTR_UXN_BIT | // 1
                                    // set TABLE_ATTR_AP_BITS to 00
                                    TABLE_ATTR_NS_BIT // 1
    );

    // 4KiB Block Size:
    //  Level 1: 1GB
    //  Level 2: 2MB
    // 16KiB Block Size:
    //  Level 1: 64GB
    //  Level 2: 32MB
    // 64KiB Block Size:
    //  Level 0: 4TB
    //  Level 1: 512MB
    uint64_t image_size = image_start_pa - image_end_pa;

    if (image_start_pa % ARM64_PAGE_TABLE_SIZE)
    {
        printf("[WARNING] arm64_mmu: pa not aligned");
        // round to lower aligned address
        image_size += (image_start_pa % ARM64_PAGE_TABLE_SIZE);
        image_start_pa -= (image_start_pa % ARM64_PAGE_TABLE_SIZE);
    }
    ASSERT(image_size != 0);
    if (image_size % ARM64_PAGE_TABLE_SIZE)
    {
        printf("[WARNING] arm64_mmu: size not aligned");
        // round to larger aligned address
        image_size += ARM64_PAGE_TABLE_SIZE - (image_size % ARM64_PAGE_TABLE_SIZE);
    }

    // Try map region block
    if ((image_size >= LVL1_BLK_SIZE) && (image_size - (image_start_pa % LVL1_BLK_SIZE) >= LVL1_BLK_SIZE))
    {
        // request region size is larger than block size after base PA aligned
        uint64_t size_to_align_l1 = image_start_pa % LVL1_BLK_SIZE;
        if (size_to_align_l1)
        {
            // base address not aligned to blk size
            // map unaligned space to next level
            if ((image_size >= LVL2_BLK_SIZE) && (image_size - (image_start_pa % LVL2_BLK_SIZE) >= LVL2_BLK_SIZE))
            {
                // request region size is larger than block size after base PA aligned
                uint64_t size_to_align_l2 = image_start_pa % LVL2_BLK_SIZE;
                if (size_to_align_l2)
                {
                    // base address not aligned to smaller blk size
                    // map unaligned space to next level pages
                    ASSERT((image_size - (image_start_pa % ARM64_PAGE_TABLE_SIZE) >= ARM64_PAGE_TABLE_SIZE));
                    ASSERT(image_start_pa % ARM64_PAGE_TABLE_SIZE == 0);
                    ASSERT((image_size % ARM64_PAGE_TABLE_SIZE == 0) && (image_size >= ARM64_PAGE_TABLE_SIZE));
                    uint64_t count_pages = size_to_align_l2 / ARM64_PAGE_TABLE_SIZE;
                    ASSERT(count_pages <= ARM64_PAGE_TABLE_SIZE / sizeof(arm64_mmu_page_t));
                    // Allocate memory store pages
                    arm64_mmu_page_t *pages = malloc(count_pages * sizeof(arm64_mmu_page_t) + ARM64_PAGE_TABLE_SIZE);
                    while ((uint64_t)pages % ARM64_PAGE_TABLE_SIZE)
                    {
                        pages = (uint64_t)pages++;
                    }
                    for (int i = 0; i < count_pages; i++){
                        pages->lower_attributes = ;
                        pages->upper_attributes = ;
                        pages->output_addr = (image_start_pa+i*ARM64_PAGE_TABLE_SIZE);
                        pages->type = 0b11;
                    }
                }
                else
                {
                }
            }
            else
            {
            }
        }
        else
        {
            // Aligned
        }
    }
#else
    (void)image_start_pa;
    (void)image_end_pa;
    (void)max_address_bits;
#endif
}