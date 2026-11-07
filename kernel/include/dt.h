/* SPDX-License-Identifier: MIT */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <libfdt.h>
struct dt_range { uint64_t base, size; };
struct dt_spec { int node; unsigned count; uint32_t args[8]; };
/* All other APIs require a blob accepted by dt_validate first. */
int dt_validate(const void *fdt, size_t available);
/* fdt_parent_offset()/fdt_node_offset_by_phandle() through the one-pass
 * index of the last validated blob (libfdt fallback otherwise). */
int dt_parent_offset(const void *fdt, int node);
int dt_node_by_phandle(const void *fdt, uint32_t phandle);
/* fdt_node_offset_by_compatible() over the same index. */
int dt_node_by_compatible(const void *fdt, int start, const char *compatible);
/* The next node after start (-1: from the root), in tree order, whose
 * compatible list holds any of names[0..count). */
int dt_node_by_compatibles(const void *fdt, int start, const char *const *names, unsigned count);
/* fdt_next_node(fdt, node, NULL) and fdt_node_check_compatible() (0 match,
 * 1 no match, -FDT_ERR_NOTFOUND without "compatible") over the index. */
int dt_next_node(const void *fdt, int node);
int dt_node_check_compatible(const void *fdt, int node, const char *compatible);
/* Nodes in the live index for fdt, or -1 when lookups use libfdt. */
int dt_index_nodes(const void *fdt);
bool dt_available(const void *fdt, int node);
int dt_u32(const void *fdt, int node, const char *property, uint32_t *value);
int dt_bool(const void *fdt, int node, const char *property, bool *value);
const char *dt_string(const void *fdt, int node, const char *property);
int dt_string_index(const void *fdt, int node, const char *property, const char *name);
int dt_phandle(const void *fdt, uint32_t handle);
int dt_reference(const void *fdt, int node, const char *property, int index);
int dt_specifier(const void *fdt, int node, const char *property,
                 const char *cells_property, unsigned index, struct dt_spec *spec);
int dt_reg(const void *fdt, int node, unsigned index, struct dt_range *range);
int dt_reg_named(const void *fdt, int node, const char *name, struct dt_range *range);
int dt_raw_reg(const void *fdt, int node, unsigned index,
               uint64_t *address, uint64_t *size);
bool dt_contains(struct dt_range range, uint64_t address, uint64_t size);
int dt_reservations(const void *fdt, struct dt_range *ranges, bool *no_map,
                    size_t capacity, size_t *count);
