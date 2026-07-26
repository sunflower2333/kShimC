
#include <stdint.h>

// VA (Virtual Address) structure for ARM64 architecture. (48 bit MAX)
// #ifdef CONFIG_ARM64_VA_BITS_48
// typedef struct
// {
//     union
//     {
//         // 64-bit virtual address
//         uint64_t virtual_address;
//         // For 4KiB page size
//         struct VA4KiB
//         {
//             uint64_t block_offset : 12; // Block offset and PA [11:0]
//             uint64_t l3_index : 9;      // Level 3 Table index [20:12], 4 KiB per page
//             uint64_t l2_index : 9;      // Level 2 Table index [29:21], 4*512 KiB = 2 MiB per index
//             uint64_t l1_index : 9;      // Level 1 Table index [38:30], 1 GiB per index
//             uint64_t l0_index : 9;      // Level 0 Table index [47:39], 512 GiB per index
//             uint64_t reserved : 16;     // Reserved bits [63:48]
//         };
//         // For 16KiB page size
//         struct VA16KiB
//         {
//             uint64_t block_offset : 14; // Block offset and PA [13:0]
//             uint64_t l3_index : 11;     // Level 3 Table index [24:14], 16 KiB per page
//             uint64_t l2_index : 11;     // Level 2 Table index [35:25], 32 MiB per index
//             uint64_t l1_index : 11;     // Level 1 Table index [46:36], 64 GiB per index
//             uint64_t l0_index : 1;      // Level 0 Table index [47], 128 TiB per index
//             uint64_t reserved : 16;     // Reserved bits [63:48]
//         };
//         // For 64KiB page size
//         struct VA64KiB
//         {
//             uint64_t block_offset : 16; // Block offset and PA [15:0]
//             uint64_t l3_index : 13;     // Level 3 Table index [28:16], 64 KiB per page
//             uint64_t l2_index : 13;     // Level 2 Table index [41:29], 512 MiB per index
//             uint64_t l1_index : 6;      // Level 1 Table index [47:42], 1 TiB per index
//             uint64_t reserved : 16;     // Reserved bits [63:48]
//         };
//     };
// } arm64_va_t;
// #elif defined(CONFIG_ARM64_VA_BITS_52)
// #elif defined(CONFIG_ARM64_VA_BITS_56)
// #else
// #error "No VA bits configured"
// #endif

// typedef struct
// {
// } PageTableEntry;
