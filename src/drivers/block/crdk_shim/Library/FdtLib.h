/* SPDX-License-Identifier: MIT */
#pragma once
/* kShimC shim: EDK2 FdtLib wrapper names used by CrDK UfsPowerFdtCrLib,
 * mapped onto the libfdt copy kShimC already links (lib/libfdt). */
#include <Uefi.h>
#include <libfdt.h>

typedef struct {
    UINT32 Tag;
    UINT32 Length;
    UINT32 NameOffset;
    CHAR8 Data[];
} FDT_PROPERTY;
_Static_assert(sizeof(FDT_PROPERTY) == sizeof(struct fdt_property), "fdt_property layout");

#define FdtCheckHeader(F)                    fdt_check_header(F)
#define FdtGetProp(F, N, Name, Len)          fdt_getprop((F), (N), (Name), (Len))
#define FdtParentOffset(F, N)                fdt_parent_offset((F), (N))
#define FdtNextNode(F, N, Depth)             fdt_next_node((F), (N), (Depth))
#define FdtGetName(F, N, Len)                fdt_get_name((F), (N), (Len))
#define FdtFirstPropertyOffset(F, N)         fdt_first_property_offset((F), (N))
#define FdtNextPropertyOffset(F, P)          fdt_next_property_offset((F), (P))
#define FdtGetPropertyByOffset(F, P, Len) \
    ((const FDT_PROPERTY *)fdt_get_property_by_offset((F), (P), (Len)))
#define FdtGetString(F, Off, Len)            fdt_get_string((F), (Off), (Len))
#define FdtNodeOffsetByPhandle(F, H)         fdt_node_offset_by_phandle((F), (H))
#define FdtGetNumberOfReserveMapEntries(F)   fdt_num_mem_rsv(F)
