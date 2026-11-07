/* SPDX-License-Identifier: MIT */
/* The one-pass device-tree index (kernel/dt/dt.c) must give exactly libfdt's
 * answers: every node's parent and every phandle, on the real runtime DTBs,
 * and fall back correctly once the blob changes. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dt.h>

static void *load(const char *path, size_t *size)
{
    FILE *file = fopen(path, "rb");
    assert(file);
    assert(fseek(file, 0, SEEK_END) == 0);
    long length = ftell(file);
    assert(length > 0 && fseek(file, 0, SEEK_SET) == 0);
    size_t room = ((size_t)length + 4096 + 7) & ~(size_t)7;
    void *blob = aligned_alloc(8, room);
    assert(blob && fread(blob, 1, (size_t)length, file) == (size_t)length);
    fclose(file);
    *size = room;
    return blob;
}

static unsigned compare_all(const void *fdt)
{
    /* Without the index every lookup is libfdt's O(n) walk: sample then. */
    unsigned stride = dt_index_nodes(fdt) > 0 ? 1U : 97U;
    /* Expected parents from one independent depth-stack walk; libfdt's own
     * O(n) fdt_parent_offset() is compared on a sample to keep this linear. */
    int stack[64], depth = -1;
    unsigned nodes = 0, phandles = 0;
    for (int node = fdt_next_node(fdt, -1, &depth); node >= 0 && depth >= 0;
         node = fdt_next_node(fdt, node, &depth)) {
        assert(depth < 64);
        stack[depth] = node;
        int expected = depth ? stack[depth - 1] : -FDT_ERR_NOTFOUND;
        if (nodes % stride == 0)
            assert(dt_parent_offset(fdt, node) == expected);
        if (nodes % 61 == 0)
            assert(fdt_parent_offset(fdt, node) == expected);
        uint32_t phandle = fdt_get_phandle(fdt, node);
        if (phandle) {
            if (phandles % stride == 0)
                assert(dt_node_by_phandle(fdt, phandle) == node);
            if (phandles % 17 == 0)
                assert(fdt_node_offset_by_phandle(fdt, phandle) == node);
            ++phandles;
        }
        ++nodes;
    }
    assert(dt_node_by_phandle(fdt, 0x7ffffff0U) == -FDT_ERR_NOTFOUND);
    assert(dt_node_by_phandle(fdt, 0) < 0 && dt_node_by_phandle(fdt, UINT32_MAX) < 0);
    printf("  %u nodes, %u phandles\n", nodes, phandles);
    return nodes;
}

static void check(const char *path)
{
    size_t room;
    void *blob = load(path, &room);
    printf("%s\n", path);
    /* Validation builds the index: the indexed answers, for every node. */
    assert(dt_validate(blob, fdt_totalsize(blob)) == 0);
    unsigned nodes = compare_all(blob);
    assert(nodes > 1 && dt_index_nodes(blob) == (int)nodes);
    /* A changed header (grown blob) disables the index: libfdt fallback. */
    assert(fdt_open_into(blob, blob, (int)room) == 0);
    assert(dt_index_nodes(blob) == -1);
    assert(compare_all(blob) == nodes);
    /* Re-validating the edited blob indexes it again. */
    assert(dt_validate(blob, room) == 0);
    assert(dt_index_nodes(blob) == (int)nodes && compare_all(blob) == nodes);
    /* An in-place edit that shifts node offsets but keeps every header size:
     * one status grows "okay" -> "disabled", a later one shrinks back. */
    int grow = -1, shrink = -1;
    for (int node = fdt_next_node(blob, -1, NULL); node >= 0; node = fdt_next_node(blob, node, NULL)) {
        const char *status = fdt_getprop(blob, node, "status", NULL);
        if (status && grow < 0 && !strcmp(status, "okay")) grow = node;
        else if (status && grow >= 0 && !strcmp(status, "disabled")) { shrink = node; break; }
    }
    if (grow >= 0 && shrink >= 0) {
        char shrink_path[512];
        assert(fdt_get_path(blob, shrink, shrink_path, sizeof(shrink_path)) == 0);
        uint32_t sizes[3] = {fdt_totalsize(blob), fdt_size_dt_struct(blob), fdt_size_dt_strings(blob)};
        assert(fdt_setprop_string(blob, grow, "status", "disabled") == 0);
        shrink = fdt_path_offset(blob, shrink_path);
        assert(shrink >= 0 && fdt_setprop_string(blob, shrink, "status", "okay") == 0);
        assert(sizes[0] == fdt_totalsize(blob) && sizes[1] == fdt_size_dt_struct(blob) &&
               sizes[2] == fdt_size_dt_strings(blob));
        assert(dt_validate(blob, room) == 0);
        assert(dt_index_nodes(blob) == (int)nodes && compare_all(blob) == nodes);
        printf("  shifted-offset edit re-indexed\n");
    }
    free(blob);
}

int main(void)
{
    check(FIXTURES "/andromeda-fdt.dtb");
    check(FIXTURES "/hdk8150-fdt.dtb");
    check(FIXTURES "/io/sm8650-qrd.dtb");
    check(FIXTURES "/io/sm8650-ayaneo-pocket-s2.dtb");
    puts("dt_index_test: ok");
    return 0;
}
