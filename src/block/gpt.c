/* SPDX-License-Identifier: MIT */
/*
 * GPT scanner.  Validation mirrors CrDK FirmwareLoaderLib NhGptFind so the
 * registry and the firmware loader agree on which disks have a usable GPT:
 * both headers and both entry arrays must be present, CRC-valid and describe
 * the same layout; entries must lie in the usable range, must not overlap and
 * must have distinct unique GUIDs.  Nothing is registered unless the whole
 * table validates.  This file is independent of CrDK so CONFIG_BLOCK does not
 * require FirmwareLoaderLib.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <kshim/block.h>

#define GPT_TABLE_MAX (1024U * 1024U)
#define GPT_COMPARE_CHUNK (16U * 1024U)

struct gpt_header {
    uint64_t first, last, table;
    uint32_t count, stride, crc;
    uint8_t guid[16];
};

/* One record per scanned disk; owns its partition list. */
struct gpt_disk {
    struct kshim_blkdev *disk;
    struct kshim_partition *parts;
    struct gpt_disk *next;
};

static struct gpt_disk *scanned;

static uint16_t le16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }
static uint32_t le32(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}
static uint64_t le64(const uint8_t *p) { return le32(p) | (uint64_t)le32(p + 4) << 32; }

static uint32_t crc32_update(uint32_t crc, const uint8_t *p, size_t n)
{
    while (n--) {
        crc ^= *p++;
        for (unsigned i = 0; i < 8; i++)
            crc = (crc >> 1) ^ (0xedb88320U & (0U - (crc & 1)));
    }
    return crc;
}

static uint32_t crc32(const void *p, size_t n)
{
    return crc32_update(~0U, p, n) ^ ~0U;
}

static bool zero(const uint8_t *p, size_t n)
{
    for (size_t i = 0; i < n; ++i)
        if (p[i])
            return false;
    return true;
}

static int read_header(struct kshim_blkdev *disk, uint64_t lba, uint64_t other,
                       uint8_t *block, struct gpt_header *out)
{
    uint32_t sector = disk->block_size, length, crc;
    uint64_t blocks, last_lba = disk->num_blocks - 1;
    int status = kshim_blkdev_read(disk, lba, 1, block);

    if (status)
        return status;
    if (memcmp(block, "EFI PART", 8))
        return ERROR_NOT_SUPPORTED;
    length = le32(block + 12);
    crc = le32(block + 16);
    if (le32(block + 8) != 0x00010000 || length < 92 || length > sector ||
        le32(block + 20) || le64(block + 24) != lba || le64(block + 32) != other)
        return KSHIM_ERR_CORRUPT;
    memset(block + 16, 0, 4);
    if (crc32(block, length) != crc || !zero(block + 92, length - 92))
        return KSHIM_ERR_CORRUPT;
    out->first = le64(block + 40);
    out->last = le64(block + 48);
    out->table = le64(block + 72);
    out->count = le32(block + 80);
    out->stride = le32(block + 84);
    out->crc = le32(block + 88);
    memcpy(out->guid, block + 56, 16);
    if (!out->count || out->count > 4096 || out->stride < 128 || out->stride > 1024 ||
        (out->stride & (out->stride - 1)) || out->count > GPT_TABLE_MAX / out->stride ||
        out->first < 2 || out->first > out->last || out->last >= last_lba ||
        zero(out->guid, 16))
        return KSHIM_ERR_CORRUPT;
    blocks = ((uint64_t)out->count * out->stride + sector - 1) / sector;
    if (out->table < 2 || out->table >= last_lba || blocks > last_lba - out->table)
        return KSHIM_ERR_CORRUPT;
    /* Each array sits outside the usable range, next to its own header. */
    if (lba == 1) {
        if (out->table >= out->first || blocks > out->first - out->table)
            return KSHIM_ERR_CORRUPT;
    } else if (out->table <= out->last) {
        return KSHIM_ERR_CORRUPT;
    }
    return ERROR_NONE;
}

static bool hard_error(int status)
{
    return status && status != KSHIM_ERR_CORRUPT && status != ERROR_NOT_SUPPORTED;
}

static struct gpt_disk *record_of(const struct kshim_blkdev *disk)
{
    for (struct gpt_disk *it = scanned; it; it = it->next)
        if (it->disk == disk)
            return it;
    return NULL;
}

static void label_from_entry(struct kshim_partition *part, const uint8_t *entry)
{
    size_t n = 0;
    bool ascii = true;

    for (; n < KSHIM_GPT_NAME_MAX; ++n) {
        uint16_t c = le16(entry + 56 + n * 2);

        if (!c)
            break;
        if (c < 0x20 || c > 0x7e) {
            ascii = false;
            c = '?';
        }
        part->label[n] = (char)c;
    }
    part->label[n] = 0;
    /* Units after the terminator must be zero for an exact match. */
    for (size_t i = n; i < KSHIM_GPT_NAME_MAX; ++i)
        if (le16(entry + 56 + i * 2))
            ascii = false;
    part->label_ascii = ascii && n > 0;
}

static int name_partition(struct kshim_partition *part, const struct kshim_blkdev *disk)
{
    int length = -1;

    if (part->label_ascii && !strchr(part->label, ':'))
        length = snprintf(part->dev.name, sizeof(part->dev.name), "%s:%s",
                          disk->name, part->label);
    if (length >= 0 && (size_t)length < sizeof(part->dev.name) &&
        !kshim_blkdev_find(part->dev.name))
        return ERROR_NONE;
    length = snprintf(part->dev.name, sizeof(part->dev.name), "%s:p%u", disk->name,
                      (unsigned)part->index);
    /* A disk name too long for "<disk>:p<N>" cannot carry partitions. */
    if (length < 0 || (size_t)length >= sizeof(part->dev.name))
        return KSHIM_ERR_TOO_LARGE;
    return ERROR_NONE;
}

static void free_parts(struct kshim_partition *parts)
{
    while (parts) {
        struct kshim_partition *next = parts->scan_next;

        (void)kshim_blkdev_unregister(&parts->dev); /* NOT_FOUND if never registered */
        free(parts);
        parts = next;
    }
}

static int register_entries(struct kshim_blkdev *disk, const uint8_t *table,
                            const struct gpt_header *h, struct gpt_disk *record,
                            unsigned *count)
{
    struct kshim_partition **tail = &record->parts;

    for (uint32_t i = 0; i < h->count; ++i) {
        const uint8_t *entry = table + (size_t)i * h->stride;
        struct kshim_partition *part;
        int status;

        if (zero(entry, 16))
            continue;
        part = calloc(1, sizeof(*part));
        if (!part)
            return KSHIM_ERR_NO_MEMORY;
        *tail = part;
        tail = &part->scan_next;
        part->index = i + 1;
        memcpy(part->type_guid, entry, 16);
        memcpy(part->unique_guid, entry + 16, 16);
        part->attributes = le64(entry + 48);
        label_from_entry(part, entry);
        status = name_partition(part, disk);
        if (status)
            return status;
        part->dev.kind = KSHIM_BLKDEV_PARTITION;
        part->dev.block_size = disk->block_size;
        part->dev.start_lba = le64(entry + 32);
        part->dev.num_blocks = le64(entry + 40) - part->dev.start_lba + 1;
        part->dev.max_transfer_blocks = disk->max_transfer_blocks;
        part->dev.read_only = true; /* the storage layer never writes */
        part->dev.ops = NULL;       /* core forwards to the parent */
        part->dev.parent = disk;
        part->dev.priv = part;
        status = kshim_blkdev_register(&part->dev);
        if (status)
            return status;
        ++*count;
    }
    return ERROR_NONE;
}

int kshim_gpt_scan(struct kshim_blkdev *disk, unsigned *registered)
{
    struct gpt_header a, b;
    struct gpt_disk *record = NULL;
    uint8_t *block = NULL, *table = NULL;
    uint32_t sector;
    uint64_t last;
    unsigned count = 0;
    int status, sa, sb;

    if (registered)
        *registered = 0;
    if (!disk || disk->parent || disk->kind == KSHIM_BLKDEV_PARTITION ||
        kshim_blkdev_find(disk->name) != disk)
        return ERROR_INVALID_PARAMETER;
    if (record_of(disk))
        return KSHIM_ERR_EXISTS;
    sector = disk->block_size;
    if (sector < 512 || disk->num_blocks < 6)
        return ERROR_INVALID_PARAMETER;
    last = disk->num_blocks - 1;
    block = aligned_alloc(KSHIM_BLKDEV_DMA_ALIGN,
                          sector > GPT_COMPARE_CHUNK ? sector : GPT_COMPARE_CHUNK);
    if (!block)
        return KSHIM_ERR_NO_MEMORY;

    sa = read_header(disk, 1, last, block, &a);
    sb = read_header(disk, last, 1, block, &b);
    if (hard_error(sa) || hard_error(sb)) {
        status = hard_error(sa) ? sa : sb;
        goto done;
    }
    if (sa == ERROR_NOT_SUPPORTED && sb == ERROR_NOT_SUPPORTED) {
        /* A protective MBR proves a (damaged) GPT disk, not a raw LUN. */
        status = kshim_blkdev_read_bytes(disk, 0, 512, block);
        if (status)
            goto done;
        status = ERROR_NOT_SUPPORTED;
        if (le16(block + 510) == 0xaa55)
            for (unsigned i = 0; i < 4; ++i)
                if (block[446 + i * 16 + 4] == 0xee)
                    status = KSHIM_ERR_CORRUPT;
        goto done;
    }
    if (sa || sb || a.first != b.first || a.last != b.last || a.count != b.count ||
        a.stride != b.stride || a.crc != b.crc || memcmp(a.guid, b.guid, 16)) {
        status = KSHIM_ERR_CORRUPT;
        goto done;
    }

    {
        size_t length = (size_t)a.count * a.stride;

        table = malloc(length);
        if (!table) {
            status = KSHIM_ERR_NO_MEMORY;
            goto done;
        }
        status = kshim_blkdev_read_bytes(disk, a.table * sector, length, table);
        if (status)
            goto done;
        if (crc32(table, length) != a.crc) {
            status = KSHIM_ERR_CORRUPT;
            goto done;
        }
        for (size_t at = 0; at < length; at += GPT_COMPARE_CHUNK) {
            size_t take = length - at < GPT_COMPARE_CHUNK ? length - at : GPT_COMPARE_CHUNK;

            status = kshim_blkdev_read_bytes(disk, b.table * sector + at, take, block);
            if (status)
                goto done;
            if (memcmp(table + at, block, take)) {
                status = KSHIM_ERR_CORRUPT;
                goto done;
            }
        }
    }

    for (uint32_t i = 0; i < a.count; ++i) {
        const uint8_t *entry = table + (size_t)i * a.stride;
        uint64_t first, end;

        if (zero(entry, 16))
            continue;
        first = le64(entry + 32);
        end = le64(entry + 40);
        if (first < a.first || first > end || end > a.last || zero(entry + 16, 16)) {
            status = KSHIM_ERR_CORRUPT;
            goto done;
        }
        for (uint32_t j = 0; j < i; ++j) {
            const uint8_t *prev = table + (size_t)j * a.stride;

            if (zero(prev, 16))
                continue;
            if ((first <= le64(prev + 40) && le64(prev + 32) <= end) ||
                !memcmp(entry + 16, prev + 16, 16)) {
                status = KSHIM_ERR_CORRUPT;
                goto done;
            }
        }
    }

    record = calloc(1, sizeof(*record));
    if (!record) {
        status = KSHIM_ERR_NO_MEMORY;
        goto done;
    }
    record->disk = disk;
    status = register_entries(disk, table, &a, record, &count);
    if (status) {
        free_parts(record->parts);
        free(record);
        record = NULL;
        count = 0;
        goto done;
    }
    record->next = scanned;
    scanned = record;
done:
    free(table);
    free(block);
    if (registered)
        *registered = count;
    return status;
}

unsigned kshim_gpt_scan_all(void)
{
    unsigned total = 0;

    for (struct kshim_blkdev *it = kshim_blkdev_first(); it; it = it->next) {
        unsigned count = 0;
        int status;

        if (it->parent || it->kind == KSHIM_BLKDEV_PARTITION || record_of(it))
            continue;
        status = kshim_gpt_scan(it, &count);
        if (status == ERROR_NONE)
            printf("block: %s: %u GPT partitions\n", it->name, count);
        else if (status != ERROR_NOT_SUPPORTED)
            printf("block: %s: GPT scan failed: %s\n", it->name,
                   kshim_storage_strerror(status));
        total += count;
    }
    return total;
}

int kshim_gpt_release(struct kshim_blkdev *disk)
{
    for (struct gpt_disk **link = &scanned; *link; link = &(*link)->next) {
        struct gpt_disk *record = *link;

        if (record->disk != disk)
            continue;
        /* Refuse before freeing anything a registered device still uses. */
        for (struct kshim_blkdev *it = kshim_blkdev_first(); it; it = it->next)
            for (struct kshim_partition *part = record->parts; part; part = part->scan_next)
                if (it->parent == &part->dev)
                    return KSHIM_ERR_BUSY;
        *link = record->next;
        free_parts(record->parts);
        free(record);
        return ERROR_NONE;
    }
    return ERROR_NOT_FOUND;
}

struct kshim_partition *kshim_partition_of(struct kshim_blkdev *dev)
{
    if (!dev || dev->kind != KSHIM_BLKDEV_PARTITION || !dev->parent)
        return NULL;
    for (struct gpt_disk *it = scanned; it; it = it->next)
        for (struct kshim_partition *part = it->parts; part; part = part->scan_next)
            if (&part->dev == dev)
                return part;
    return NULL;
}

int kshim_partition_find(const char *label, struct kshim_blkdev *disk,
                         struct kshim_partition **out)
{
    struct kshim_partition *found = NULL;

    if (!label || !out || !*label || strlen(label) > KSHIM_GPT_NAME_MAX)
        return ERROR_INVALID_PARAMETER;
    *out = NULL;
    for (struct gpt_disk *it = scanned; it; it = it->next) {
        if (disk && it->disk != disk)
            continue;
        for (struct kshim_partition *part = it->parts; part; part = part->scan_next) {
            if (!part->label_ascii || strcmp(part->label, label))
                continue;
            if (found)
                return KSHIM_ERR_CORRUPT;
            found = part;
        }
    }
    if (!found)
        return ERROR_NOT_FOUND;
    *out = found;
    return ERROR_NONE;
}

int kshim_slot_name(char *out, size_t cap, const char *base, uint32_t slot)
{
    size_t length;

    if (!out || !base || slot > 1)
        return ERROR_INVALID_PARAMETER;
    length = strlen(base);
    if (!length || length + 3 > cap)
        return KSHIM_ERR_TOO_LARGE;
    memcpy(out, base, length);
    out[length] = '_';
    out[length + 1] = (char)('a' + slot);
    out[length + 2] = 0;
    return ERROR_NONE;
}

int kshim_partition_find_slot(const char *base, uint32_t slot,
                              struct kshim_blkdev *disk,
                              struct kshim_partition **out)
{
    char name[KSHIM_GPT_NAME_MAX + 1];
    int status = kshim_slot_name(name, sizeof(name), base, slot);

    if (status)
        return status;
    return kshim_partition_find(name, disk, out);
}
