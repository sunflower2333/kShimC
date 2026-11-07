/* SPDX-License-Identifier: MIT */
#include <dt.h>
#include <string.h>

/*
 * One-pass index of the last validated blob. libfdt finds a node's parent and
 * a phandle's node by walking the whole structure block, and kShimC does such
 * lookups inside loops over nodes (interrupt parents, pinctrl, supplies). On
 * the 4291-node SM8450 Android tree that was O(n^2) work before the boot menu.
 * The index holds every node offset in tree order with its parent and its
 * compatible list, plus the phandles sorted; lookups are binary searches.
 * Compatible searches still scan, but over the index (no per-node property
 * search): libfdt's took ~30 ms per search on that tree, and key discovery
 * alone runs ten. Every successful dt_validate() rebuilds the index, and it
 * is ignored if the blob's header changed.
 */
#define DT_INDEX_MAX_NODES 8192
#define DT_INDEX_MAX_DEPTH 64
#define DT_TAGALIGN(x) (((x) + 3U) & ~3U)
struct dt_phandle { uint32_t phandle; int node; };
static struct {
    const void *fdt;
    uint32_t totalsize, size_struct, size_strings;
    uint64_t hash; /* of the validated blob: dt_validate() skips an unchanged one */
    int count, phandles;
    /* A phandle or linux,phandle that is not 4 bytes, or the two disagree:
     * dt_phandle() then trusts no reference, as its full walk did. */
    bool phandle_malformed;
    int offsets[DT_INDEX_MAX_NODES];
    int parents[DT_INDEX_MAX_NODES];
    /* "compatible" value: byte offset into the blob and length (0: none). */
    int compat_offset[DT_INDEX_MAX_NODES];
    int compat_length[DT_INDEX_MAX_NODES];
    struct dt_phandle by_phandle[DT_INDEX_MAX_NODES];
} Index;

static bool index_valid(const void *fdt)
{
    return fdt && Index.fdt == fdt && Index.totalsize == fdt_totalsize(fdt) &&
        Index.size_struct == fdt_size_dt_struct(fdt) &&
        Index.size_strings == fdt_size_dt_strings(fdt);
}

/* FNV-1a over 64-bit words; the blob is 8-byte aligned (dt_validate). Not
 * a guard against an adversary, only against in-place edits. */
static uint64_t blob_hash(const void *fdt, size_t size)
{
    const uint64_t *word = fdt;
    uint64_t hash = UINT64_C(0xcbf29ce484222325);
    size_t words = size / 8;
    for (size_t i = 0; i < words; ++i)
        hash = (hash ^ word[i]) * UINT64_C(0x100000001b3);
    for (size_t i = words * 8; i < size; ++i)
        hash = (hash ^ ((const uint8_t *)fdt)[i]) * UINT64_C(0x100000001b3);
    return hash ^ size;
}

/* Per-node property state while one node's properties are read. */
struct dt_props { const fdt32_t *a, *b; int alen, blen; };

static bool add_phandle(int *phandles, uint32_t phandle, int node)
{
    if (!phandle || phandle == UINT32_MAX) return true;
    if (*phandles >= DT_INDEX_MAX_NODES) return false;
    int at = (*phandles)++;
    /* Insertion keeps the table sorted (stable: tree order among equals);
     * phandles mostly ascend. */
    while (at > 0 && Index.by_phandle[at - 1].phandle > phandle) {
        Index.by_phandle[at] = Index.by_phandle[at - 1];
        --at;
    }
    Index.by_phandle[at] = (struct dt_phandle){phandle, node};
    return true;
}

/* A node's properties are complete: record its phandle the way
 * fdt_get_phandle() reads it, and anything dt_phandle() must refuse. */
static bool finish_node(int position, const struct dt_props *p, int *phandles, bool *malformed)
{
    if ((p->a && p->alen != 4) || (p->b && p->blen != 4) ||
        (p->a && p->b && fdt32_ld(p->a) != fdt32_ld(p->b)))
        *malformed = true;
    uint32_t phandle = p->a && p->alen == 4 ? fdt32_ld(p->a) :
                       p->b && p->blen == 4 ? fdt32_ld(p->b) : 0;
    return add_phandle(phandles, phandle, Index.offsets[position]);
}

/*
 * One raw pass over the structure block, which fdt_check_full() has just
 * accepted. libfdt's per-node fdt_getprop() calls each rescanned the node's
 * properties: five of them per node made indexing the SM8450 tree ~60 ms on
 * the device. A property belongs to the innermost open node and precedes its
 * subnodes, so a node is complete at its first child or its end.
 */
static void index_build(const void *fdt, uint64_t hash)
{
    const char *base = (const char *)fdt + fdt_off_dt_struct(fdt);
    const char *strings = (const char *)fdt + fdt_off_dt_strings(fdt);
    uint32_t limit = fdt_version(fdt) >= 17 ? fdt_size_dt_struct(fdt) :
                     fdt_totalsize(fdt) - fdt_off_dt_struct(fdt);
    int stack[DT_INDEX_MAX_DEPTH], depth = -1, count = 0, phandles = 0, open = -1;
    bool malformed = false;
    struct dt_props props = {0};
    Index.fdt = NULL;
    if (limit < 4) return;
    for (uint32_t offset = 0; offset <= limit - 4;) {
        uint32_t tag = fdt32_ld((const fdt32_t *)(base + offset));
        if (tag == FDT_BEGIN_NODE) {
            if (open >= 0 && !finish_node(open, &props, &phandles, &malformed)) return;
            if (++depth >= DT_INDEX_MAX_DEPTH || count >= DT_INDEX_MAX_NODES)
                return; /* too large: lookups fall back to libfdt */
            Index.offsets[count] = (int)offset;
            Index.parents[count] = depth ? Index.offsets[stack[depth - 1]] : -FDT_ERR_NOTFOUND;
            Index.compat_offset[count] = Index.compat_length[count] = 0;
            stack[depth] = open = count++;
            props = (struct dt_props){0};
            offset += 4 + DT_TAGALIGN((uint32_t)strnlen(base + offset + 4, limit - offset - 4) + 1);
        } else if (tag == FDT_PROP) {
            if (depth < 0 || offset > limit - 12) return;
            uint32_t length = fdt32_ld((const fdt32_t *)(base + offset + 4));
            const char *name = strings + fdt32_ld((const fdt32_t *)(base + offset + 8));
            const char *value = base + offset + 12;
            int at = stack[depth];
            /* First occurrence wins, as with fdt_getprop(). */
            if (open == at && !strcmp(name, "compatible")) {
                if (!Index.compat_offset[at]) {
                    Index.compat_offset[at] = (int)(value - (const char *)fdt);
                    Index.compat_length[at] = (int)length;
                }
            } else if (open == at && !strcmp(name, "phandle")) {
                if (!props.a) props = (struct dt_props){(const fdt32_t *)value, props.b, (int)length, props.blen};
            } else if (open == at && !strcmp(name, "linux,phandle")) {
                if (!props.b) props = (struct dt_props){props.a, (const fdt32_t *)value, props.alen, (int)length};
            }
            offset += 12 + DT_TAGALIGN(length);
        } else if (tag == FDT_END_NODE) {
            if (open >= 0 && !finish_node(open, &props, &phandles, &malformed)) return;
            open = -1;
            offset += 4;
            if (--depth < 0) break; /* the root closed */
        } else if (tag == FDT_NOP) {
            offset += 4;
        } else {
            break; /* FDT_END */
        }
    }
    if (depth >= 0 || !count) return;
    Index.count = count;
    Index.phandles = phandles;
    Index.phandle_malformed = malformed;
    Index.hash = hash;
    Index.totalsize = fdt_totalsize(fdt);
    Index.size_struct = fdt_size_dt_struct(fdt);
    Index.size_strings = fdt_size_dt_strings(fdt);
    Index.fdt = fdt;
}

int dt_index_nodes(const void *fdt)
{
    return index_valid(fdt) ? Index.count : -1;
}

/* Position of the node last looked up: tree-order walks ask for it or the
 * next one, so most lookups skip the binary search. Only a hint: always
 * checked against the offsets. */
static int Hint;

/* Position of node in the index, or -1. */
static int index_position(int node)
{
    int hint = Hint;
    if (hint >= 0 && hint < Index.count && Index.offsets[hint] == node) return hint;
    if (hint >= 0 && hint + 1 < Index.count && Index.offsets[hint + 1] == node) {
        Hint = hint + 1;
        return hint + 1;
    }
    int lo = 0, hi = Index.count - 1;
    while (lo <= hi) {
        int mid = lo + (hi - lo) / 2;
        if (Index.offsets[mid] == node) { Hint = mid; return mid; }
        if (Index.offsets[mid] < node) lo = mid + 1;
        else hi = mid - 1;
    }
    return -1;
}

int dt_next_node(const void *fdt, int node)
{
    if (index_valid(fdt)) {
        int at = node < 0 ? -1 : index_position(node);
        if (node < 0 || at >= 0) {
            if (at + 1 >= Index.count) return -FDT_ERR_NOTFOUND;
            Hint = at + 1;
            return Index.offsets[at + 1];
        }
    }
    return fdt_next_node(fdt, node, NULL);
}

/* Exactly fdt_node_check_compatible()'s test on a present property, even
 * an empty one ("compatible;" on /cpus matches "" in libfdt too). */
static bool index_compatible(const void *fdt, int at, const char *compatible)
{
    return Index.compat_offset[at] &&
        fdt_stringlist_contains((const char *)fdt + Index.compat_offset[at],
                                Index.compat_length[at], compatible);
}

int dt_node_check_compatible(const void *fdt, int node, const char *compatible)
{
    if (compatible && node >= 0 && index_valid(fdt)) {
        int at = index_position(node);
        if (at >= 0) {
            if (!Index.compat_offset[at]) return -FDT_ERR_NOTFOUND;
            return !index_compatible(fdt, at, compatible);
        }
    }
    return fdt_node_check_compatible(fdt, node, compatible);
}

int dt_node_by_compatibles(const void *fdt, int start, const char *const *names, unsigned count)
{
    if (!names || !count) return -FDT_ERR_NOTFOUND;
    if (index_valid(fdt)) {
        int at = start < 0 ? -1 : index_position(start);
        if (start < 0 || at >= 0) {
            for (int i = at + 1; i < Index.count; ++i)
                for (unsigned k = 0; k < count; ++k)
                    if (index_compatible(fdt, i, names[k])) {
                        Hint = i;
                        return Index.offsets[i];
                    }
            return -FDT_ERR_NOTFOUND;
        }
    }
    for (int n = fdt_next_node(fdt, start, NULL); n >= 0; n = fdt_next_node(fdt, n, NULL))
        for (unsigned k = 0; k < count; ++k)
            if (!fdt_node_check_compatible(fdt, n, names[k])) return n;
    return -FDT_ERR_NOTFOUND;
}

int dt_parent_offset(const void *fdt, int node)
{
    if (node >= 0 && index_valid(fdt)) {
        int at = index_position(node);
        if (at >= 0) return Index.parents[at];
    }
    return fdt_parent_offset(fdt, node);
}

int dt_node_by_compatible(const void *fdt, int start, const char *compatible)
{
    if (compatible && index_valid(fdt)) {
        int at = start < 0 ? -1 : index_position(start);
        if (start < 0 || at >= 0) {
            for (int i = at + 1; i < Index.count; ++i)
                if (index_compatible(fdt, i, compatible)) {
                    Hint = i;
                    return Index.offsets[i];
                }
            return -FDT_ERR_NOTFOUND;
        }
    }
    return fdt_node_offset_by_compatible(fdt, start, compatible);
}

/* Position of one by_phandle entry for phandle, or -1. */
static int phandle_position(uint32_t phandle)
{
    int lo = 0, hi = Index.phandles - 1;
    while (lo <= hi) {
        int mid = lo + (hi - lo) / 2;
        if (Index.by_phandle[mid].phandle == phandle) return mid;
        if (Index.by_phandle[mid].phandle < phandle) lo = mid + 1;
        else hi = mid - 1;
    }
    return -1;
}

int dt_node_by_phandle(const void *fdt, uint32_t phandle)
{
    if (index_valid(fdt)) {
        if (!phandle || phandle == UINT32_MAX) return -FDT_ERR_BADPHANDLE;
        int at = phandle_position(phandle);
        return at >= 0 ? Index.by_phandle[at].node : -FDT_ERR_NOTFOUND;
    }
    return fdt_node_offset_by_phandle(fdt, phandle);
}

int dt_validate(const void *fdt, size_t available)
{
    if (!fdt || ((uintptr_t)fdt & 7) || available < sizeof(struct fdt_header))
        return -FDT_ERR_TRUNCATED;
    if (fdt_check_header(fdt)) return -FDT_ERR_BADMAGIC;
    if (fdt_totalsize(fdt) > available) return -FDT_ERR_TRUNCATED;
    /* Subsystems re-validate the runtime FDT on entry; on the SM8450 tree a
     * full check plus re-index costs ~36 ms on the device. The same bytes
     * as the last accepted blob need neither. Any in-place edit, including
     * one that shifts node offsets while every header size stays equal,
     * changes the hash and is checked and indexed again. */
    uint64_t hash = blob_hash(fdt, fdt_totalsize(fdt));
    if (index_valid(fdt) && Index.hash == hash) return 0;
    int status = fdt_check_full(fdt, available);
    if (!status) index_build(fdt, hash);
    else if (Index.fdt == fdt) Index.fdt = NULL;
    return status;
}
const char *dt_string(const void *f, int node, const char *property)
{
    int len;
    const char *p = fdt_getprop(f, node, property, &len);
    if (!p || len <= 0 || p[len - 1] || memchr(p, 0, len - 1)) return NULL;
    return p;
}
int dt_string_index(const void *f, int node, const char *property, const char *name)
{
    int len;
    const char *p = fdt_getprop(f, node, property, &len);
    if (!p || len <= 0 || p[len - 1]) return -1;
    int result = -1, index = 0;
    for (int off = 0; off < len; ++index) {
        size_t n = strlen(p + off);
        if (!n) return -1;
        if (!strcmp(p + off, name)) {
            if (result >= 0) return -1;
            result = index;
        }
        off += (int)n + 1;
    }
    return result;
}
bool dt_available(const void *f, int node)
{
    while (node >= 0) {
        int len;
        const void *status = fdt_getprop(f, node, "status", &len);
        if (status) {
            const char *s = dt_string(f, node, "status");
            if (!s || (strcmp(s, "okay") && strcmp(s, "ok"))) return false;
        } else if (len != -FDT_ERR_NOTFOUND) return false;
        if (!node) return true;
        node = dt_parent_offset(f, node);
    }
    return false;
}
int dt_u32(const void *f, int node, const char *property, uint32_t *value)
{
    int len;
    const fdt32_t *p = fdt_getprop(f, node, property, &len);
    if (!p || len != 4 || !value) return -1;
    *value = fdt32_to_cpu(*p);
    return 0;
}
int dt_bool(const void *f, int node, const char *property, bool *value)
{
    int len;
    const void *p = fdt_getprop(f, node, property, &len);
    if (!value || (p && len != 0) || (!p && len != -FDT_ERR_NOTFOUND)) return -1;
    *value = p != NULL;
    return 0;
}
int dt_phandle(const void *f, uint32_t handle)
{
    if (!handle || handle == UINT32_MAX) return -1;
    if (index_valid(f)) {
        if (Index.phandle_malformed) return -1;
        int at = phandle_position(handle);
        if (at < 0 || (at > 0 && Index.by_phandle[at - 1].phandle == handle) ||
            (at + 1 < Index.phandles && Index.by_phandle[at + 1].phandle == handle))
            return -1; /* absent, or not unique */
        int node = Index.by_phandle[at].node;
        return dt_available(f, node) ? node : -1;
    }
    int found = -1;
    for (int n = fdt_next_node(f, -1, NULL); n >= 0; n = fdt_next_node(f, n, NULL)) {
        uint32_t h, legacy;
        int alen, blen;
        const void *ap = fdt_getprop(f, n, "phandle", &alen);
        const void *bp = fdt_getprop(f, n, "linux,phandle", &blen);
        if ((ap && alen != 4) || (bp && blen != 4)) return -1;
        bool a = dt_u32(f, n, "phandle", &h) == 0;
        bool b = dt_u32(f, n, "linux,phandle", &legacy) == 0;
        if (a && b && h != legacy) return -1;
        if ((a && h == handle) || (b && legacy == handle)) {
            if (found >= 0) return -1;
            found = n;
        }
    }
    return found >= 0 && dt_available(f, found) ? found : -1;
}
int dt_reference(const void *f, int node, const char *prop, int index)
{
    int len;
    const fdt32_t *p = fdt_getprop(f, node, prop, &len);
    if (!dt_available(f, node) || !p || len <= 0 || len % 4 || index < 0 || index >= len / 4)
        return -1;
    return dt_phandle(f, fdt32_to_cpu(p[index]));
}
int dt_specifier(const void *f, int node, const char *prop,
                 const char *cells_property, unsigned index, struct dt_spec *spec)
{
    int len;
    const fdt32_t *p = fdt_getprop(f, node, prop, &len);
    if (!spec || !dt_available(f, node) || !p || len <= 0 || len % 4) return -1;
    bool found = false;
    for (unsigned off = 0, item = 0; off < (unsigned)len / 4; ++item) {
        uint32_t cells;
        int provider = dt_phandle(f, fdt32_to_cpu(p[off++]));
        if (provider < 0 || dt_u32(f, provider, cells_property, &cells) ||
            cells > 8 || cells > (unsigned)len / 4 - off) return -1;
        if (item == index) {
            spec->node = provider; spec->count = cells;
            for (unsigned i = 0; i < cells; ++i) spec->args[i] = fdt32_to_cpu(p[off + i]);
            found = true;
        }
        off += cells;
    }
    return found ? 0 : -1;
}
static int cells(const void *f, int node, const char *prop, int fallback)
{
    int len;
    const fdt32_t *p = fdt_getprop(f, node, prop, &len);
    if (!p) return len == -FDT_ERR_NOTFOUND ? fallback : -1;
    if (len != 4 || fdt32_to_cpu(*p) > 2) return -1;
    return (int)fdt32_to_cpu(*p);
}
static uint64_t value(const fdt32_t *p, int n)
{
    uint64_t v = 0;
    while (n--) v = (v << 32) | fdt32_to_cpu(*p++);
    return v;
}
bool dt_contains(struct dt_range r, uint64_t a, uint64_t s)
{
    return r.size && s && r.base <= UINT64_MAX - r.size && a >= r.base &&
        s <= r.size && a - r.base <= r.size - s;
}
int dt_raw_reg(const void *f, int node, unsigned index, uint64_t *a, uint64_t *s)
{
    if (!a || !s || !dt_available(f, node)) return -1;
    int parent = dt_parent_offset(f, node), len;
    if (parent < 0) return -1;
    int ac = cells(f, parent, "#address-cells", 2), sc = cells(f, parent, "#size-cells", 1);
    if (ac < 1 || sc < 0) return -1;
    const fdt32_t *p = fdt_getprop(f, node, "reg", &len);
    int tuple = ac + sc;
    if (!p || len <= 0 || len % (tuple * 4) || index >= (unsigned)len / (tuple * 4)) return -1;
    p += index * tuple;
    *a = value(p, ac); *s = value(p + ac, sc);
    return *a <= UINT64_MAX - *s ? 0 : -1;
}
int dt_reg(const void *f, int node, unsigned index, struct dt_range *r)
{
    if (!r || dt_raw_reg(f, node, index, &r->base, &r->size) || !r->size) return -1;
    for (int bus = dt_parent_offset(f, node); bus > 0; bus = dt_parent_offset(f, bus)) {
        int parent = dt_parent_offset(f, bus), len;
        int ac = cells(f, bus, "#address-cells", 2), sc = cells(f, bus, "#size-cells", 1);
        int pc = cells(f, parent, "#address-cells", 2);
        if (ac < 1 || sc < 1 || pc < 1) return -1;
        const fdt32_t *p = fdt_getprop(f, bus, "ranges", &len);
        if (!p) return -1; /* An absent ranges property is not identity mapping. */
        if (!len) continue;
        int tuple = ac + pc + sc;
        if (len < 0 || len % (tuple * 4)) return -1;
        bool found = false;
        uint64_t translated = 0;
        for (int off = 0; off < len / 4; off += tuple) {
            struct dt_range window = {value(p + off, ac), value(p + off + ac + pc, sc)};
            uint64_t target = value(p + off + ac, pc);
            if (!dt_contains(window, r->base, r->size)) continue;
            if (found || target > UINT64_MAX - window.size) return -1;
            translated = target + r->base - window.base;
            found = true;
        }
        if (!found) return -1;
        r->base = translated;
    }
    return 0;
}
int dt_reg_named(const void *f, int node, const char *name, struct dt_range *r)
{
    int index = dt_string_index(f, node, "reg-names", name);
    int names = fdt_stringlist_count(f, node, "reg-names"), len;
    int parent = dt_parent_offset(f, node);
    int ac = cells(f, parent, "#address-cells", 2), sc = cells(f, parent, "#size-cells", 1);
    const void *reg = fdt_getprop(f, node, "reg", &len);
    if (index < 0 || names < 1 || !reg || ac < 1 || sc < 0 ||
        len <= 0 || len % (4 * (ac + sc)) || names != len / (4 * (ac + sc))) return -1;
    /* Reject duplicate names even when the requested entry itself is unique. */
    for (int i = 0; i < names; ++i) {
        const char *entry = fdt_stringlist_get(f, node, "reg-names", i, NULL);
        if (!entry || dt_string_index(f, node, "reg-names", entry) != i) return -1;
    }
    return dt_reg(f, node, (unsigned)index, r);
}
int dt_reservations(const void *f, struct dt_range *ranges, bool *no_map,
                    size_t capacity, size_t *count)
{
    if (!ranges || !no_map || !count) return -1;
    *count = 0;
    int n = fdt_num_mem_rsv(f);
    if (n < 0) return -1;
    for (int i = 0; i < n; ++i) {
        if (*count == capacity || fdt_get_mem_rsv(f, i, &ranges[*count].base, &ranges[*count].size)) return -1;
        if (!ranges[*count].size || ranges[*count].base > UINT64_MAX - ranges[*count].size) return -1;
        no_map[(*count)++] = true;
    }
    int parent = fdt_path_offset(f, "/reserved-memory");
    if (parent < 0) return parent == -FDT_ERR_NOTFOUND ? 0 : -1;
    if (!dt_available(f, parent)) return 0;
    int node;
    fdt_for_each_subnode(node, f, parent) {
        if (!dt_available(f, node)) continue;
        bool flag;
        if (dt_bool(f, node, "no-map", &flag)) return -1;
        int len;
        if (!fdt_getprop(f, node, "reg", &len)) {
            if (len != -FDT_ERR_NOTFOUND) return -1;
            continue; /* Dynamic pools have not been allocated by this shim. */
        }
        int ac = cells(f, parent, "#address-cells", 2), sc = cells(f, parent, "#size-cells", 1);
        if (ac < 1 || sc < 1 || len <= 0 || len % (4 * (ac + sc))) return -1;
        for (int i = 0; i < len / (4 * (ac + sc)); ++i) {
            uint64_t base, size;
            if (dt_raw_reg(f, node, i, &base, &size)) return -1;
            /* Zero-size placeholders reserve nothing (Linux skips them too);
             * SM8450 Android DTs carry demura_region_N with reg = <0 0 0 0>. */
            if (!size) continue;
            if (*count == capacity || dt_reg(f, node, i, &ranges[*count])) return -1;
            no_map[(*count)++] = flag;
        }
    }
    return 0;
}
