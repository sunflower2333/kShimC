/* SPDX-License-Identifier: MIT */
/*
 * HID report descriptor parser (kshim/hid.h).
 *
 * Written from the Device Class Definition for HID 1.11, section 6.2.2
 * (item format: prefix bits 1:0 bSize 0/1/2/4 bytes, 3:2 bType main/global/
 * local, 7:4 bTag; long items start with 0xfe) and the item tag tables of
 * 6.2.2.4 (main), 6.2.2.7 (global) and 6.2.2.8 (local). Facts only.
 *
 * The descriptor is flattened: a variable field becomes one item per
 * element with its own usage (the last usage repeats when the field has more
 * elements than usages, 6.2.2.8), an array field becomes one item with its
 * usage range or explicit usage list. Bit offsets are per report (ID and
 * type), after the report ID byte.
 */
#include <kshim/hid.h>
#include <kshim/io.h>
#include <string.h>

/* Item types and tags (HID 1.11 6.2.2.2, 6.2.2.4, 6.2.2.7, 6.2.2.8). */
#define ITEM_MAIN   0U
#define ITEM_GLOBAL 1U
#define ITEM_LOCAL  2U
#define ITEM_LONG_PREFIX 0xfeU

#define MAIN_INPUT          0x8U
#define MAIN_OUTPUT         0x9U
#define MAIN_COLLECTION     0xaU
#define MAIN_FEATURE        0xbU
#define MAIN_END_COLLECTION 0xcU

#define GLOBAL_USAGE_PAGE   0x0U
#define GLOBAL_LOGICAL_MIN  0x1U
#define GLOBAL_LOGICAL_MAX  0x2U
#define GLOBAL_REPORT_SIZE  0x7U
#define GLOBAL_REPORT_ID    0x8U
#define GLOBAL_REPORT_COUNT 0x9U
#define GLOBAL_PUSH         0xaU
#define GLOBAL_POP          0xbU

#define LOCAL_USAGE     0x0U
#define LOCAL_USAGE_MIN 0x1U
#define LOCAL_USAGE_MAX 0x2U

#define COLLECTION_APPLICATION 0x01U

#define PUSH_DEPTH 4U
#define COLLECTION_DEPTH 8U
/* Usages a single main item may declare (ranges count once). */
#define LOCAL_USAGES 64U
/* Vendor-defined usage pages (HID Usage Tables: 0xff00-0xffff). */
#define VENDOR_PAGE_FIRST 0xff00U

struct global_state {
    uint32_t usage_page;
    int32_t logical_min, logical_max;
    uint8_t logical_min_size, logical_max_size;
    uint32_t report_size, report_count;
    uint8_t report_id;
};

/* One local usage entry: a single usage (min == max) or a range. */
struct local_usage {
    uint32_t min, max;
};

struct local_state {
    struct local_usage usages[LOCAL_USAGES];
    unsigned count;
    bool overflow;
    /* Usage Minimum seen without its Usage Maximum yet. */
    bool have_min;
    uint32_t pending_min;
};

struct parser {
    struct kshim_hid_desc *desc;
    struct global_state global;
    struct global_state stack[PUSH_DEPTH];
    unsigned stack_depth;
    struct local_state local;
    uint32_t collection_application[COLLECTION_DEPTH];
    unsigned collection_depth;
    uint32_t application; /* innermost Application collection usage */
};

static uint32_t item_unsigned(const uint8_t *data, unsigned size)
{
    uint32_t value = 0;
    for (unsigned i = 0; i < size; ++i) value |= (uint32_t)data[i] << (8U * i);
    return value;
}

static int32_t item_signed(const uint8_t *data, unsigned size)
{
    uint32_t value = item_unsigned(data, size);
    if (size == 1) return (int8_t)value;
    if (size == 2) return (int16_t)value;
    return (int32_t)value;
}

/* Local usages with a 1/2-byte value take the current Usage Page; a 4-byte
 * value is an extended usage (6.2.2.8). */
static uint32_t extended_usage(const struct parser *p, uint32_t value, unsigned size)
{
    if (size == 4) return value;
    return KSHIM_HID_USAGE(p->global.usage_page, value);
}

static void local_add(struct local_state *local, uint32_t min, uint32_t max)
{
    if (local->count == LOCAL_USAGES) { local->overflow = true; return; }
    local->usages[local->count++] = (struct local_usage){min, max};
}

static void local_reset(struct local_state *local)
{
    local->count = 0;
    local->overflow = false;
    local->have_min = false;
}

/* Element `index` of a variable field: ranges expand, and the last usage
 * applies to the remaining elements. */
static uint32_t variable_usage(const struct local_state *local, unsigned index)
{
    if (!local->count) return 0;
    for (unsigned i = 0; i < local->count; ++i) {
        uint32_t span = local->usages[i].max - local->usages[i].min + 1U;
        if (index < span) return local->usages[i].min + index;
        index -= span;
    }
    return local->usages[local->count - 1U].max;
}

static struct kshim_hid_report *report_entry(struct kshim_hid_desc *desc, uint8_t id, uint8_t type)
{
    for (unsigned i = 0; i < desc->report_count; ++i)
        if (desc->reports[i].id == id && desc->reports[i].type == type) return &desc->reports[i];
    if (desc->report_count == KSHIM_HID_MAX_REPORTS) return NULL;
    struct kshim_hid_report *r = &desc->reports[desc->report_count++];
    *r = (struct kshim_hid_report){.id = id, .type = type, .bits = 0};
    return r;
}

static int add_item(struct kshim_hid_desc *desc, const struct kshim_hid_item *item)
{
    if (desc->item_count == KSHIM_HID_MAX_ITEMS) return KSHIM_IO_ERR_OVERFLOW;
    desc->items[desc->item_count++] = *item;
    return 0;
}

/* HID 1.11 6.2.2.7: Logical Minimum/Maximum are signed. Descriptors in the
 * field write an unsigned maximum in its shortest form (0x25 0xff for 255);
 * read such a maximum as unsigned when the minimum is not negative. */
static void logical_range(const struct global_state *g, int32_t *min, int32_t *max)
{
    *min = g->logical_min;
    *max = g->logical_max;
    if (*min >= 0 && *max < *min && g->logical_max_size && g->logical_max_size < 4) {
        uint32_t mask = (1U << (8U * g->logical_max_size)) - 1U;
        *max = (int32_t)((uint32_t)*max & mask);
    }
}

static int main_field(struct parser *p, uint8_t type, uint32_t flags)
{
    struct kshim_hid_desc *desc = p->desc;
    const struct global_state *g = &p->global;
    if (p->local.overflow) return KSHIM_IO_ERR_OVERFLOW;
    uint64_t bits = (uint64_t)g->report_size * g->report_count;
    struct kshim_hid_report *report = report_entry(desc, g->report_id, type);
    if (!report) return KSHIM_IO_ERR_OVERFLOW;
    uint32_t offset = report->bits;
    if (offset + bits > UINT16_MAX) return KSHIM_IO_ERR_OVERFLOW;
    report->bits = (uint16_t)(offset + bits);
    if (!bits || (flags & KSHIM_HID_MAIN_CONSTANT)) return 0;
    if (g->report_size > 32U) return 0; /* opaque data (no mapping reads it) */

    int32_t lmin, lmax;
    logical_range(g, &lmin, &lmax);
    struct kshim_hid_item item = {
        .application = p->application,
        .bit_size = (uint8_t)g->report_size,
        .report_id = g->report_id,
        .type = type,
        .flags = (uint16_t)flags,
        .logical_min = lmin,
        .logical_max = lmax,
    };

    if (flags & KSHIM_HID_MAIN_VARIABLE) {
        for (uint32_t i = 0; i < g->report_count; ++i) {
            uint32_t usage = variable_usage(&p->local, i);
            if (KSHIM_HID_USAGE_PAGE(usage) >= VENDOR_PAGE_FIRST) continue;
            item.usage = item.usage_max = usage;
            item.bit_offset = offset + i * g->report_size;
            item.count = 1;
            int status = add_item(desc, &item);
            if (status) return status;
        }
        return 0;
    }

    /* Array: one item; its elements select usages. */
    if (g->report_count > UINT8_MAX) return KSHIM_IO_ERR_OVERFLOW;
    if (!p->local.count) return 0; /* no usage: nothing can be selected */
    uint32_t first = p->local.usages[0].min;
    uint32_t last = p->local.usages[p->local.count - 1U].max;
    if (KSHIM_HID_USAGE_PAGE(first) >= VENDOR_PAGE_FIRST) return 0;
    item.usage = first;
    item.usage_max = last;
    item.bit_offset = offset;
    item.count = (uint8_t)g->report_count;
    /* A single contiguous range keeps usage..usage_max; anything else
     * (several usages or ranges) is an explicit list. */
    if (p->local.count > 1U) {
        uint32_t total = 0;
        for (unsigned i = 0; i < p->local.count; ++i)
            total += p->local.usages[i].max - p->local.usages[i].min + 1U;
        if (desc->usage_count + total > KSHIM_HID_MAX_ARRAY_USAGES || total > UINT8_MAX)
            return KSHIM_IO_ERR_OVERFLOW;
        item.usage_index = (uint8_t)desc->usage_count;
        item.usage_count = (uint8_t)total;
        for (unsigned i = 0; i < total; ++i)
            desc->usages[desc->usage_count++] = variable_usage(&p->local, i);
    }
    return add_item(desc, &item);
}

static int collection(struct parser *p, uint32_t kind)
{
    if (p->collection_depth == COLLECTION_DEPTH) return KSHIM_IO_ERR_OVERFLOW;
    p->collection_application[p->collection_depth++] = p->application;
    if (kind != COLLECTION_APPLICATION) return 0;
    /* The Application collection's usage is the usage declared before it. */
    uint32_t usage = p->local.count ? p->local.usages[0].min : 0;
    p->application = usage;
    struct kshim_hid_desc *desc = p->desc;
    if (desc->application_count == KSHIM_HID_MAX_APPLICATIONS) return KSHIM_IO_ERR_OVERFLOW;
    desc->applications[desc->application_count++] = usage;
    return 0;
}

static int main_item(struct parser *p, unsigned tag, uint32_t value)
{
    int status = 0;
    switch (tag) {
    case MAIN_INPUT: status = main_field(p, KSHIM_HID_REPORT_INPUT, value); break;
    case MAIN_OUTPUT: status = main_field(p, KSHIM_HID_REPORT_OUTPUT, value); break;
    case MAIN_FEATURE: status = main_field(p, KSHIM_HID_REPORT_FEATURE, value); break;
    case MAIN_COLLECTION: status = collection(p, value & 0xffU); break;
    case MAIN_END_COLLECTION:
        if (!p->collection_depth) return KSHIM_IO_ERR_PROTOCOL;
        p->application = p->collection_application[--p->collection_depth];
        break;
    default: return KSHIM_IO_ERR_PROTOCOL;
    }
    local_reset(&p->local);
    return status;
}

static int global_item(struct parser *p, unsigned tag, const uint8_t *data, unsigned size)
{
    struct global_state *g = &p->global;
    uint32_t value = item_unsigned(data, size);
    switch (tag) {
    case GLOBAL_USAGE_PAGE: g->usage_page = value & 0xffffU; break;
    case GLOBAL_LOGICAL_MIN: g->logical_min = item_signed(data, size); g->logical_min_size = (uint8_t)size; break;
    case GLOBAL_LOGICAL_MAX: g->logical_max = item_signed(data, size); g->logical_max_size = (uint8_t)size; break;
    case GLOBAL_REPORT_SIZE: g->report_size = value; break;
    case GLOBAL_REPORT_COUNT: g->report_count = value; break;
    case GLOBAL_REPORT_ID:
        if (!value || value > UINT8_MAX) return KSHIM_IO_ERR_PROTOCOL;
        g->report_id = (uint8_t)value;
        p->desc->report_ids = true;
        break;
    case GLOBAL_PUSH:
        if (p->stack_depth == PUSH_DEPTH) return KSHIM_IO_ERR_OVERFLOW;
        p->stack[p->stack_depth++] = *g;
        break;
    case GLOBAL_POP:
        if (!p->stack_depth) return KSHIM_IO_ERR_PROTOCOL;
        *g = p->stack[--p->stack_depth];
        break;
    default: break; /* physical range, unit, exponent: not used */
    }
    return 0;
}

static void local_item(struct parser *p, unsigned tag, uint32_t value, unsigned size)
{
    struct local_state *local = &p->local;
    switch (tag) {
    case LOCAL_USAGE: local_add(local, extended_usage(p, value, size), extended_usage(p, value, size)); break;
    case LOCAL_USAGE_MIN:
        local->have_min = true;
        local->pending_min = extended_usage(p, value, size);
        break;
    case LOCAL_USAGE_MAX: {
        uint32_t max = extended_usage(p, value, size);
        uint32_t min = local->have_min ? local->pending_min : max;
        /* A minimum on another page or above the maximum is malformed;
         * keep the maximum alone rather than a huge range. */
        if (KSHIM_HID_USAGE_PAGE(min) != KSHIM_HID_USAGE_PAGE(max) || min > max) min = max;
        local_add(local, min, max);
        local->have_min = false;
        break;
    }
    default: break; /* designators, strings, delimiters: not used */
    }
}

int kshim_hid_parse(const uint8_t *descriptor, size_t length, struct kshim_hid_desc *desc)
{
    if (!descriptor || !desc || !length) return KSHIM_IO_ERR_PROTOCOL;
    static const unsigned sizes[4] = {0, 1, 2, 4};
    struct parser p;
    memset(desc, 0, sizeof(*desc));
    memset(&p, 0, sizeof(p));
    p.desc = desc;
    size_t at = 0;
    while (at < length) {
        uint8_t prefix = descriptor[at++];
        if (prefix == ITEM_LONG_PREFIX) {
            /* bDataSize, bLongItemTag, data: no long item is defined. */
            if (at + 2 > length) return KSHIM_IO_ERR_PROTOCOL;
            size_t skip = 2U + descriptor[at];
            if (at + skip > length) return KSHIM_IO_ERR_PROTOCOL;
            at += skip;
            continue;
        }
        unsigned size = sizes[prefix & 3U];
        unsigned type = (prefix >> 2) & 3U;
        unsigned tag = prefix >> 4;
        if (at + size > length) return KSHIM_IO_ERR_PROTOCOL;
        const uint8_t *data = &descriptor[at];
        at += size;
        int status = 0;
        switch (type) {
        case ITEM_MAIN: status = main_item(&p, tag, item_unsigned(data, size)); break;
        case ITEM_GLOBAL: status = global_item(&p, tag, data, size); break;
        case ITEM_LOCAL: local_item(&p, tag, item_unsigned(data, size), size); break;
        default: status = KSHIM_IO_ERR_PROTOCOL; break; /* reserved type */
        }
        if (status) return status;
    }
    return 0;
}

static bool item_has_usage(const struct kshim_hid_desc *desc, const struct kshim_hid_item *item,
                           uint32_t usage)
{
    if (item->usage_count) {
        for (unsigned i = 0; i < item->usage_count; ++i)
            if (desc->usages[item->usage_index + i] == usage) return true;
        return false;
    }
    return usage >= item->usage && usage <= item->usage_max;
}

const struct kshim_hid_item *kshim_hid_find(const struct kshim_hid_desc *desc, uint8_t type,
                                            uint32_t usage, int report_id)
{
    if (!desc) return NULL;
    for (unsigned i = 0; i < desc->item_count; ++i) {
        const struct kshim_hid_item *item = &desc->items[i];
        if (item->type != type) continue;
        if (report_id >= 0 && item->report_id != (uint8_t)report_id) continue;
        if (item_has_usage(desc, item, usage)) return item;
    }
    return NULL;
}

bool kshim_hid_get_value(const struct kshim_hid_item *item, const uint8_t *data, size_t length,
                         unsigned index, int32_t *value)
{
    if (!item || !data || !value || index >= item->count || !item->bit_size || item->bit_size > 32)
        return false;
    uint64_t start = (uint64_t)item->bit_offset + (uint64_t)index * item->bit_size;
    if (start + item->bit_size > (uint64_t)length * 8U) return false;
    uint32_t raw = 0;
    /* Little-endian bit order (HID 1.11 5.8: least significant bit first). */
    for (unsigned bit = 0; bit < item->bit_size; ++bit) {
        uint64_t at = start + bit;
        if (data[at >> 3] & (1U << (at & 7U))) raw |= 1U << bit;
    }
    if (item->logical_min < 0 && item->bit_size < 32 && (raw & (1U << (item->bit_size - 1U))))
        raw |= ~0U << item->bit_size;
    *value = (int32_t)raw;
    return true;
}

bool kshim_hid_has_application(const struct kshim_hid_desc *desc, uint32_t usage)
{
    if (!desc) return false;
    for (unsigned i = 0; i < desc->application_count; ++i)
        if (desc->applications[i] == usage) return true;
    return false;
}

bool kshim_hid_array_usage(const struct kshim_hid_desc *desc, const struct kshim_hid_item *item,
                           int32_t value, uint32_t *usage)
{
    if (!desc || !item || !usage) return false;
    if (value < item->logical_min || value > item->logical_max) return false;
    uint32_t index = (uint32_t)((int64_t)value - item->logical_min);
    if (item->usage_count) {
        if (index >= item->usage_count) return false;
        *usage = desc->usages[item->usage_index + index];
        return true;
    }
    if (index > item->usage_max - item->usage) return false;
    *usage = item->usage + index;
    return true;
}
