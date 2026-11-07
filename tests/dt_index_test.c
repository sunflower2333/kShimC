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

/* dt_node_by_compatible() must walk the same chain as libfdt: every match
 * in tree order, the root included, for strings sampled from the tree
 * (each a single linear libfdt chain), plus fixed and absent ones. */
static void compare_compatible(const void *fdt)
{
    const char *names[48];
    unsigned count = 0, index = 0;
    names[count++] = "qcom,pm8941-pwrkey";
    names[count++] = "gpio-keys";
    names[count++] = "qcom,sde-kms";
    names[count++] = "kshim,absent";
    for (int node = 0; node >= 0 && count < 48; node = fdt_next_node(fdt, node, NULL), ++index) {
        const char *compatible = fdt_getprop(fdt, node, "compatible", NULL);
        if (compatible && (node == 0 || index % 113 == 0)) names[count++] = compatible;
    }
    for (unsigned i = 0; i < count; ++i) {
        int expected = -1, got = -1;
        do {
            expected = fdt_node_offset_by_compatible(fdt, expected, names[i]);
            got = dt_node_by_compatible(fdt, got, names[i]);
            assert(got == expected);
        } while (expected >= 0);
        assert(expected == -FDT_ERR_NOTFOUND);
    }
    /* A start offset that is not a node start falls back to libfdt. */
    assert(dt_node_by_compatible(fdt, 1, names[0]) == fdt_node_offset_by_compatible(fdt, 1, names[0]));
    /* Tree order, and per-node compatible checks (0 / 1 / -NOTFOUND), on
     * every node against libfdt: the node's own first string, the root's,
     * and an absent one. */
    const char *root = fdt_getprop(fdt, 0, "compatible", NULL);
    int expected = fdt_next_node(fdt, -1, NULL), got = dt_next_node(fdt, -1);
    for (; expected >= 0; expected = fdt_next_node(fdt, expected, NULL), got = dt_next_node(fdt, got)) {
        assert(got == expected);
        const char *own = fdt_getprop(fdt, expected, "compatible", NULL);
        const char *probe[] = {own ? own : "kshim,absent", root ? root : "kshim,absent", "kshim,absent"};
        for (unsigned k = 0; k < 3; ++k)
            assert(dt_node_check_compatible(fdt, expected, probe[k]) ==
                   fdt_node_check_compatible(fdt, expected, probe[k]));
    }
    assert(got == expected && expected == -FDT_ERR_NOTFOUND);
    /* Any-of iteration: the reference is a walk checking each name. */
    for (unsigned i = 0; i + 3 <= count; i += 3) {
        const char *set[3] = {names[i], names[i + 1], names[i + 2]};
        int n = -1, ref = -1;
        do {
            n = dt_node_by_compatibles(fdt, n, set, 3);
            do ref = fdt_next_node(fdt, ref, NULL);
            while (ref >= 0 && fdt_node_check_compatible(fdt, ref, set[0]) &&
                   fdt_node_check_compatible(fdt, ref, set[1]) && fdt_node_check_compatible(fdt, ref, set[2]));
            if (ref < 0) ref = -FDT_ERR_NOTFOUND;
            assert(n == ref);
        } while (ref >= 0);
    }
}

/* dt_phandle()'s full walk before the index: strict, unique, available. */
static int ref_phandle(const void *f, uint32_t handle)
{
    if (!handle || handle == UINT32_MAX) return -1;
    int found = -1;
    for (int n = fdt_next_node(f, -1, NULL); n >= 0; n = fdt_next_node(f, n, NULL)) {
        int alen, blen;
        const fdt32_t *a = fdt_getprop(f, n, "phandle", &alen);
        const fdt32_t *b = fdt_getprop(f, n, "linux,phandle", &blen);
        if ((a && alen != 4) || (b && blen != 4)) return -1;
        if (a && b && fdt32_to_cpu(*a) != fdt32_to_cpu(*b)) return -1;
        if ((a && fdt32_to_cpu(*a) == handle) || (b && fdt32_to_cpu(*b) == handle)) {
            if (found >= 0) return -1;
            found = n;
        }
    }
    return found >= 0 && dt_available(f, found) ? found : -1;
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
            if (phandles % 37 == 0)
                assert(dt_phandle(fdt, phandle) == ref_phandle(fdt, phandle));
            ++phandles;
        }
        ++nodes;
    }
    assert(dt_node_by_phandle(fdt, 0x7ffffff0U) == -FDT_ERR_NOTFOUND);
    assert(dt_node_by_phandle(fdt, 0) < 0 && dt_node_by_phandle(fdt, UINT32_MAX) < 0);
    compare_compatible(fdt);
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
    /* dt_phandle() refuses a duplicate: give a second node the first one's
     * phandle in place (no size changes; only the content hash sees it). */
    int first = -1, second = -1;
    for (int node = fdt_next_node(blob, -1, NULL); node >= 0 && second < 0; node = fdt_next_node(blob, node, NULL)) {
        int length;
        if (!fdt_getprop(blob, node, "phandle", &length) || length != 4 ||
            fdt_getprop(blob, node, "linux,phandle", NULL)) continue;
        if (first < 0) first = node; else second = node;
    }
    assert(first >= 0 && second >= 0);
    uint32_t p1 = fdt_get_phandle(blob, first), p2 = fdt_get_phandle(blob, second);
    assert(dt_validate(blob, room) == 0 && dt_phandle(blob, p1) == ref_phandle(blob, p1));
    assert(fdt_setprop_inplace_u32(blob, second, "phandle", p1) == 0);
    assert(dt_validate(blob, room) == 0);
    assert(ref_phandle(blob, p1) == -1 && dt_phandle(blob, p1) == -1);
    assert(fdt_setprop_inplace_u32(blob, second, "phandle", p2) == 0);
    assert(dt_validate(blob, room) == 0 && dt_phandle(blob, p2) == ref_phandle(blob, p2));
    /* A mismatched linux,phandle anywhere makes every reference untrusted. */
    assert(fdt_setprop_u32(blob, second, "linux,phandle", p2 + 1) == 0);
    assert(dt_validate(blob, room) == 0);
    assert(ref_phandle(blob, p1) == -1 && dt_phandle(blob, p1) == -1);
    for (int node = fdt_next_node(blob, -1, NULL); node >= 0; node = fdt_next_node(blob, node, NULL))
        if (fdt_getprop(blob, node, "linux,phandle", NULL) && fdt_get_phandle(blob, node) == p2) {
            assert(fdt_delprop(blob, node, "linux,phandle") == 0);
            break;
        }
    assert(dt_validate(blob, room) == 0 && dt_phandle(blob, p1) == ref_phandle(blob, p1));
    /* Corruption in place, every header size unchanged: a previously
     * accepted blob is still checked again, and refused. */
    first = fdt_path_offset(blob, "/");
    int victim = fdt_next_node(blob, first, NULL);
    assert(victim > 0);
    fdt32_t *tag = (fdt32_t *)((char *)blob + fdt_off_dt_struct(blob) + victim);
    fdt32_t saved = *tag;
    *tag = cpu_to_fdt32(0x7fffffffU);
    assert(dt_validate(blob, room) != 0 && dt_index_nodes(blob) == -1);
    *tag = saved;
    assert(dt_validate(blob, room) == 0 && dt_index_nodes(blob) == (int)nodes);
    printf("  phandle uniqueness and in-place corruption checked\n");
    /* A present but empty compatible is "not compatible" (1), an absent one
     * -FDT_ERR_NOTFOUND: the index keeps the two apart. */
    int bare = -1;
    for (int node = fdt_next_node(blob, -1, NULL); node >= 0; node = fdt_next_node(blob, node, NULL))
        if (node && !fdt_getprop(blob, node, "compatible", NULL)) { bare = node; break; }
    assert(bare > 0);
    assert(dt_node_check_compatible(blob, bare, "kshim,absent") == -FDT_ERR_NOTFOUND);
    assert(fdt_setprop(blob, bare, "compatible", "", 0) == 0);
    assert(dt_validate(blob, room) == 0 && dt_index_nodes(blob) == (int)nodes);
    assert(dt_node_check_compatible(blob, bare, "kshim,absent") == 1 &&
           fdt_node_check_compatible(blob, bare, "kshim,absent") == 1);
    assert(compare_all(blob) == nodes);
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
