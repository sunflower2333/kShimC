/* SPDX-License-Identifier: MIT */
/*
 * Current Android boot slot, captured once from the bootloader handoff.
 * Mirrors CrDK AndroidBootCr slot semantics: exact androidboot.slot_suffix /
 * androidboot.slot keys in the command line and verified bootconfig, plus
 * slot_suffix / slot under /firmware/android; every present value must agree.
 * Disk metadata (misc bootloader_control, GPT A/B attributes) describes the
 * next boot and is deliberately never consulted.
 */
#include <string.h>

#include <libfdt.h>
#include <Library/FirmwareLoaderLib/NhPolicy.h>
#include <kshim/firmware.h>

#define BOOTCONFIG_MAX (64U * 1024U)
#define BOOTCONFIG_KEY_MAX 256U
#define BOOTCONFIG_DEPTH_MAX 16U

static uint32_t current_slot = KSHIM_SLOT_UNKNOWN;
static uint32_t current_evidence;

struct slot_value {
    bool present;
    uint32_t slot;
};

/* key: 1 = *slot_suffix ("_a"), 2 = *slot ("a") */
static int set_slot(struct slot_value *value, unsigned key, const char *text, size_t length)
{
    uint32_t number;

    if (key == 1) {
        if (length != 2 || text[0] != '_')
            return ERROR_INVALID_PARAMETER;
        ++text;
        --length;
    }
    if (length != 1 || (text[0] != 'a' && text[0] != 'b'))
        return ERROR_INVALID_PARAMETER;
    number = (uint32_t)(text[0] - 'a');
    if (value->present && value->slot != number)
        return KSHIM_ERR_CORRUPT;
    value->present = true;
    value->slot = number;
    return ERROR_NONE;
}

static bool text_is(const char *text, size_t length, const char *want)
{
    return strlen(want) == length && !memcmp(text, want, length);
}

static unsigned android_key(const char *key, size_t length)
{
    if (text_is(key, length, "androidboot.slot_suffix"))
        return 1;
    if (text_is(key, length, "androidboot.slot"))
        return 2;
    return 0;
}

static bool space(char c)
{
    return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

/* Linux treats a "bootconfig" kernel parameter (before "--") as a request
 * for the initrd bootconfig. */
static bool cmdline_wants_bootconfig(const char *p, size_t n)
{
    size_t at = 0;

    while (at < n && p[at]) {
        size_t start, end;

        while (at < n && p[at] && space(p[at]))
            ++at;
        start = at;
        while (at < n && p[at] && !space(p[at]))
            ++at;
        end = at;
        if (text_is(p + start, end - start, "--"))
            return false;
        if (text_is(p + start, end - start, "bootconfig") ||
            (end - start > 11 && !memcmp(p + start, "bootconfig=", 11)))
            return true;
    }
    return false;
}

static bool keyword(char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
           (c >= '0' && c <= '9') || c == '_' || c == '-';
}

static void skip(const char *p, size_t n, size_t *at, bool newlines)
{
    while (*at < n) {
        char c = p[*at];

        if (c == '#') {
            while (*at < n && p[*at] != '\n')
                ++*at;
            if (!newlines)
                return;
        } else if (c == ' ' || c == '\t' || c == '\r' || (newlines && c == '\n')) {
            ++*at;
        } else {
            return;
        }
    }
}

static int bootconfig_value(const char *p, size_t n, size_t *at,
                            const char **value, size_t *length)
{
    skip(p, n, at, false);
    if (*at < n && (p[*at] == '"' || p[*at] == '\'')) {
        char quote = p[(*at)++];
        size_t start = *at;

        while (*at < n && p[*at] != quote)
            ++*at;
        if (*at == n)
            return ERROR_INVALID_PARAMETER;
        *value = p + start;
        *length = *at - start;
        ++*at;
    } else {
        size_t start = *at;

        while (*at < n && !space(p[*at]) && !strchr(",;#}", p[*at]))
            ++*at;
        *value = p + start;
        *length = *at - start;
    }
    skip(p, n, at, false);
    return ERROR_NONE;
}

/* Linux extra bootconfig (XBC) subset: dotted keys, nested braces, scalar
 * and array values, comments.  Slot keys must have exactly one value and
 * may not use the := / += operators or open a block. */
static int bootconfig_parse(const char *p, size_t n, struct slot_value *slot)
{
    char path[BOOTCONFIG_KEY_MAX];
    size_t saved[BOOTCONFIG_DEPTH_MAX], at = 0, prefix = 0, depth = 0;

    while (n && !p[n - 1])
        --n;
    if (n > BOOTCONFIG_MAX || memchr(p, 0, n))
        return ERROR_INVALID_PARAMETER;
    while (at < n) {
        size_t length = prefix;
        unsigned key;
        char op;

        skip(p, n, &at, true);
        if (at == n)
            break;
        if (p[at] == ';') {
            ++at;
            continue;
        }
        if (p[at] == '}') {
            if (!depth)
                return ERROR_INVALID_PARAMETER;
            prefix = saved[--depth];
            ++at;
            continue;
        }
        if (length)
            path[length++] = '.';
        for (;;) {
            size_t start = at;

            while (at < n && keyword(p[at])) {
                if (length >= sizeof(path) - 1)
                    return ERROR_INVALID_PARAMETER;
                path[length++] = p[at++];
            }
            if (at == start)
                return ERROR_INVALID_PARAMETER;
            skip(p, n, &at, false);
            if (at < n && p[at] == '.') {
                if (length >= sizeof(path) - 1)
                    return ERROR_INVALID_PARAMETER;
                path[length++] = '.';
                ++at;
                skip(p, n, &at, false);
                continue;
            }
            break;
        }
        key = android_key(path, length);
        if (at < n && p[at] == '{') {
            if (depth == BOOTCONFIG_DEPTH_MAX || key)
                return ERROR_INVALID_PARAMETER;
            saved[depth++] = prefix;
            prefix = length;
            ++at;
            continue;
        }
        if (at == n || strchr(";\n#}", p[at])) {
            if (key)
                return ERROR_INVALID_PARAMETER;
            continue;
        }
        op = p[at++];
        if (op == ':' || op == '+') {
            if (at == n || p[at++] != '=' || key)
                return ERROR_INVALID_PARAMETER;
        } else if (op != '=') {
            return ERROR_INVALID_PARAMETER;
        }
        for (unsigned count = 0;; ++count) {
            const char *value;
            size_t value_length;
            int status;

            if (count)
                skip(p, n, &at, true);
            status = bootconfig_value(p, n, &at, &value, &value_length);
            if (status)
                return status;
            if (key) {
                if (count)
                    return ERROR_INVALID_PARAMETER;
                status = set_slot(slot, key, value, value_length);
                if (status)
                    return status;
            }
            if (at == n || p[at] != ',')
                break;
            ++at;
            if (key)
                return ERROR_INVALID_PARAMETER;
        }
    }
    return depth ? ERROR_INVALID_PARAMETER : ERROR_NONE;
}

int kshim_bootconfig_from_initrd(const void *initrd, size_t size,
                                 const char **text, size_t *length)
{
    static const char magic[] = "#BOOTCONFIG\n";
    const uint8_t *data = initrd;
    size_t padding;
    uint32_t bytes, checksum, sum = 0;

    if (!text || !length || (!initrd && size))
        return ERROR_INVALID_PARAMETER;
    *text = NULL;
    *length = 0;
    /* Linux accepts the magic at the end or up to three bytes earlier. */
    for (padding = 0; padding < 4; ++padding) {
        if (size < 20 + padding)
            return ERROR_NOT_FOUND;
        if (!memcmp(data + size - 12 - padding, magic, 12))
            break;
    }
    if (padding == 4)
        return ERROR_NOT_FOUND;
    bytes = (uint32_t)data[size - 20 - padding] | (uint32_t)data[size - 19 - padding] << 8 |
            (uint32_t)data[size - 18 - padding] << 16 | (uint32_t)data[size - 17 - padding] << 24;
    checksum = (uint32_t)data[size - 16 - padding] | (uint32_t)data[size - 15 - padding] << 8 |
               (uint32_t)data[size - 14 - padding] << 16 | (uint32_t)data[size - 13 - padding] << 24;
    if (!bytes || bytes > BOOTCONFIG_MAX || bytes > size - 20 - padding)
        return ERROR_INVALID_PARAMETER;
    data += size - 20 - padding - bytes;
    for (uint32_t i = 0; i < bytes; ++i)
        sum += data[i];
    if (sum != checksum)
        return ERROR_INVALID_PARAMETER;
    *text = (const char *)data;
    *length = bytes;
    return ERROR_NONE;
}

int kshim_bootslot_parse(const struct kshim_bootslot_sources *src,
                         uint32_t *slot, uint32_t *evidence)
{
    struct slot_value values[3] = {{0}}; /* cmdline, bootconfig, fdt */
    uint32_t found = 0, chosen = KSHIM_SLOT_UNKNOWN;
    bool wants_bootconfig = false;
    int status;

    if (!src || !slot)
        return ERROR_INVALID_PARAMETER;
    *slot = KSHIM_SLOT_UNKNOWN;
    if (evidence)
        *evidence = 0;
    if (src->cmdline && src->cmdline_len) {
        uint32_t value;
        NH_STATUS nh = NhBootArgsSlot(src->cmdline, src->cmdline_len, &value);

        if (nh == NH_OK) {
            values[0].present = true;
            values[0].slot = value;
        } else if (nh == NH_INVALID) {
            return ERROR_INVALID_PARAMETER;
        } else if (nh != NH_NOT_FOUND) {
            return KSHIM_ERR_CORRUPT; /* malformed or contradictory values */
        }
        wants_bootconfig = cmdline_wants_bootconfig(src->cmdline, src->cmdline_len);
    }
    if (src->bootconfig) {
        status = bootconfig_parse(src->bootconfig, src->bootconfig_len, &values[1]);
        if (status)
            return status;
    }
    if (src->fdt_slot_suffix) {
        status = set_slot(&values[2], 1, src->fdt_slot_suffix, strlen(src->fdt_slot_suffix));
        if (status)
            return status;
    }
    if (src->fdt_slot) {
        status = set_slot(&values[2], 2, src->fdt_slot, strlen(src->fdt_slot));
        if (status)
            return status;
    }
    for (unsigned i = 0; i < 3; ++i) {
        if (!values[i].present)
            continue;
        if (chosen != KSHIM_SLOT_UNKNOWN && chosen != values[i].slot)
            return KSHIM_ERR_CORRUPT;
        chosen = values[i].slot;
        found |= 1U << i;
    }
    if (!found)
        return wants_bootconfig && !src->bootconfig ? KSHIM_ERR_NOT_READY : ERROR_NOT_FOUND;
    *slot = chosen;
    if (evidence)
        *evidence = found;
    return ERROR_NONE;
}

int kshim_bootslot_set(uint32_t slot, uint32_t evidence)
{
    if (slot != KSHIM_SLOT_UNKNOWN && slot > 1)
        return ERROR_INVALID_PARAMETER;
    current_slot = slot;
    current_evidence = slot == KSHIM_SLOT_UNKNOWN ? 0 : evidence;
    return ERROR_NONE;
}

int kshim_bootslot_get(uint32_t *slot)
{
    if (!slot)
        return ERROR_INVALID_PARAMETER;
    *slot = current_slot;
    return current_slot == KSHIM_SLOT_UNKNOWN ? KSHIM_ERR_NOT_READY : ERROR_NONE;
}

int kshim_bootslot_capture(const struct kshim_bootslot_sources *sources)
{
    uint32_t slot, evidence;
    int status = kshim_bootslot_parse(sources, &slot, &evidence);

    (void)kshim_bootslot_set(status ? KSHIM_SLOT_UNKNOWN : slot, evidence);
    return status;
}

static int fdt_text_prop(const void *fdt, int node, const char *name, const char **out)
{
    int length;
    const char *value = fdt_getprop(fdt, node, name, &length);

    *out = NULL;
    if (!value)
        return length == -FDT_ERR_NOTFOUND ? ERROR_NONE : ERROR_INVALID_PARAMETER;
    if (length < 1 || value[length - 1] || strlen(value) != (size_t)length - 1)
        return ERROR_INVALID_PARAMETER;
    *out = value;
    return ERROR_NONE;
}

int kshim_bootslot_capture_fdt(const void *fdt)
{
    struct kshim_bootslot_sources sources = {0};
    int node, status;

    if (!fdt) {
        (void)kshim_bootslot_set(KSHIM_SLOT_UNKNOWN, 0);
        return KSHIM_ERR_NOT_READY;
    }
    node = fdt_path_offset(fdt, "/chosen");
    if (node >= 0) {
        status = fdt_text_prop(fdt, node, "bootargs", &sources.cmdline);
        if (status)
            goto fail;
        if (sources.cmdline)
            sources.cmdline_len = strlen(sources.cmdline);
    }
    node = fdt_path_offset(fdt, "/firmware/android");
    if (node >= 0) {
        status = fdt_text_prop(fdt, node, "slot_suffix", &sources.fdt_slot_suffix);
        if (!status)
            status = fdt_text_prop(fdt, node, "slot", &sources.fdt_slot);
        if (status)
            goto fail;
    }
    return kshim_bootslot_capture(&sources);
fail:
    (void)kshim_bootslot_set(KSHIM_SLOT_UNKNOWN, 0);
    return status;
}
