/* SPDX-License-Identifier: MIT */
/*
 * Firmware loader: kShimC callbacks for CrDK FirmwareLoaderLib NhPolicyRead.
 *
 * The selection policy itself (candidate syntax and validation of the whole
 * list before any I/O, {slot} expansion, "only NH_NOT_FOUND advances",
 * re-validation before success or fallback, package fallback after absence,
 * MDT/bXX group consistency) is CrDK's shared code.  This file supplies what
 * CrDK leaves to adapters, modelled on Driver/NhlosPartitionCrDxe: binding
 * one internal storage device, raw GPT lookup across its LUNs (NhGptFind),
 * Virtual A/B admission before every super access, filesystem sources and
 * stability rechecks.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <Library/FirmwareLoaderLib/NhPolicy.h>
#include <kshim/firmware.h>

#include "../fs/nh_glue.h"

#define FW_MAX_DISKS 16U
#define MISC_VIRTUAL_AB_OFFSET 32768U
#define FW_MODE_TYPE 0170000U
#define FW_MODE_REG 0100000U

_Static_assert(KSHIM_FW_MAX_LOCATIONS * 2 <= NH_MAX_CANDIDATES, "candidate bound");
_Static_assert(KSHIM_SLOT_UNKNOWN == NH_SLOT_UNKNOWN, "slot sentinel follows CrDK");
_Static_assert(KSHIM_FW_ORIGIN_ANDROID == NH_ORIGIN_ANDROID &&
               KSHIM_FW_ORIGIN_BUILTIN == NH_ORIGIN_PACKAGE, "origins follow CrDK");

const char *const kshim_firmware_default_locations[] = {
    "super|vendor{slot}|firmware",
    "super|vendor{slot}|firmware/image",
    "modem{slot}||image",
};
const unsigned kshim_firmware_default_location_count =
    sizeof(kshim_firmware_default_locations) / sizeof(kshim_firmware_default_locations[0]);

static struct kshim_fw_builtin *builtins;

struct fw_disk {
    struct kshim_blkdev *dev;
    uint32_t block_size;
    uint64_t num_blocks;
    struct kshim_nh_window window;
    NH_READER reader;
};

struct fw_partition {
    const struct fw_disk *disk;
    uint64_t offset, bytes;
    char name[KSHIM_GPT_NAME_MAX + 1];
    struct kshim_nh_window window;
    NH_READER reader;
};

struct fw_context {
    const struct kshim_fw_request *req;
    size_t max_size;
    struct fw_disk disks[FW_MAX_DISKS];
    unsigned disk_count;
    uint32_t slot;
    /* Virtual A/B admission, established on the first super access. */
    bool admitted;
    uint8_t snapshot[64];
    struct fw_partition misc;
    /* The one open Android source, and the last selected partition. */
    struct fw_partition part;
    bool view_open, fs_open;
    NH_VIEW view;
    NH_FS fs;
    struct fw_partition selected;
    bool user_used;
};

struct fw_result {
    NH_BUNDLE *bundle;
    void *user_buffer;
};

/* ---- allocation ----------------------------------------------------------- */

static void *fw_alloc(void *context, size_t bytes)
{
    (void)context;
    return malloc(bytes ? bytes : 1);
}

static void fw_free(void *context, void *pointer)
{
    struct fw_context *ctx = context;

    if (pointer && (!ctx->req || pointer != ctx->req->buffer))
        free(pointer);
}

static void result_free(void *context, void *pointer)
{
    if (pointer && pointer != context)
        free(pointer);
}

/* ---- storage binding ------------------------------------------------------ */

static size_t physical_length(const char *name)
{
    const char *colon = strchr(name, ':');

    return colon ? (size_t)(colon - name) : strlen(name);
}

static bool internal_kind(enum kshim_blkdev_kind kind)
{
    /* Removable SD cards are never inferred to be the Android firmware disk. */
    return kind == KSHIM_BLKDEV_UFS_LU || kind == KSHIM_BLKDEV_EMMC ||
           kind == KSHIM_BLKDEV_MEMORY;
}

static NH_STATUS bind_disks(struct fw_context *ctx)
{
    const char *device = ctx->req->device, *first = NULL;
    size_t first_length = 0;

    ctx->disk_count = 0;
    for (struct kshim_blkdev *it = kshim_blkdev_first(); it; it = it->next) {
        size_t length = physical_length(it->name);
        struct fw_disk *disk;

        if (it->parent || it->kind == KSHIM_BLKDEV_PARTITION || !kshim_blkdev_bytes(it))
            continue;
        if (device) {
            if (length != strlen(device) || memcmp(it->name, device, length))
                continue;
        } else {
            if (!internal_kind(it->kind))
                continue;
            if (!first) {
                first = it->name;
                first_length = length;
            } else if (length != first_length || memcmp(it->name, first, length)) {
                printf("firmware: ambiguous storage %.*s and %.*s\n",
                       (int)first_length, first, (int)length, it->name);
                return NH_CORRUPT;
            }
        }
        if (ctx->disk_count == FW_MAX_DISKS)
            return NH_UNSUPPORTED;
        disk = &ctx->disks[ctx->disk_count++];
        disk->dev = it;
        disk->block_size = it->block_size;
        disk->num_blocks = it->num_blocks;
        disk->window = (struct kshim_nh_window){ it, 0, kshim_blkdev_bytes(it), 0 };
        kshim_nh_reader(&disk->window, &disk->reader);
    }
    return ctx->disk_count ? NH_OK : NH_NOT_READY;
}

/* Raw GPT lookup on every LUN, like NhlosPartitionCrDxe FwPartition: a valid
 * GPT without the name proves absence; no GPT anywhere is NOT_READY; a name
 * on two LUNs is corruption. */
static NH_STATUS find_partition(struct fw_context *ctx, const char *name,
                                struct fw_partition *out)
{
    const struct fw_disk *found = NULL;
    uint64_t offset = 0, bytes = 0;
    bool gpt_seen = false;

    for (unsigned i = 0; i < ctx->disk_count; ++i) {
        struct fw_disk *disk = &ctx->disks[i];
        uint64_t at, length;
        NH_STATUS status;

        if (disk->block_size != 512 && disk->block_size != 4096)
            continue;
        status = NhGptFind(&disk->reader, disk->block_size, name, &at, &length);
        if (status == NH_UNSUPPORTED)
            continue; /* raw, non-GPT LUN */
        if (status == NH_NOT_FOUND) {
            gpt_seen = true;
            continue;
        }
        if (status != NH_OK)
            return status;
        if (found)
            return NH_CORRUPT;
        found = disk;
        offset = at;
        bytes = length;
        gpt_seen = true;
    }
    if (!found)
        return gpt_seen ? NH_NOT_FOUND : NH_NOT_READY;
    memset(out, 0, sizeof(*out));
    out->disk = found;
    out->offset = offset;
    out->bytes = bytes;
    strncpy(out->name, name, sizeof(out->name) - 1);
    out->window = (struct kshim_nh_window){ found->dev, offset, bytes, 0 };
    kshim_nh_reader(&out->window, &out->reader);
    return NH_OK;
}

static NH_STATUS read_snapshot(struct fw_context *ctx, uint8_t message[64])
{
    uint8_t again[64];

    NH_TRY(NhRead(&ctx->misc.reader, MISC_VIRTUAL_AB_OFFSET, message, 64));
    NH_TRY(NhSnapshotCheck(message));
    NH_TRY(NhRead(&ctx->misc.reader, MISC_VIRTUAL_AB_OFFSET, again, 64));
    return memcmp(message, again, 64) ? NH_NOT_READY : NH_OK;
}

/* Every super access needs a recognised Virtual A/B message in state NONE;
 * absent misc or unknown state is NOT_READY, never permission to fall back. */
static NH_STATUS admit_super(struct fw_context *ctx)
{
    NH_STATUS status;

    if (ctx->admitted)
        return NH_OK;
    status = find_partition(ctx, "misc", &ctx->misc);
    if (status == NH_NOT_FOUND)
        return NH_NOT_READY;
    NH_TRY(status);
    NH_TRY(read_snapshot(ctx, ctx->snapshot));
    ctx->admitted = true;
    return NH_OK;
}

/* ---- sources -------------------------------------------------------------- */

static NH_STATUS fill_buffer(struct fw_context *ctx, uint64_t size, void **data)
{
    if (size > ctx->max_size || size > UINT32_MAX)
        return NH_TOO_LARGE;
    if (ctx->req->kind == KSHIM_FW_FILE && ctx->req->buffer && !ctx->user_used) {
        if (size > ctx->req->buffer_size)
            return NH_TOO_LARGE;
        ctx->user_used = true;
        *data = ctx->req->buffer;
        return NH_OK;
    }
    *data = fw_alloc(ctx, (size_t)size);
    return *data ? NH_OK : NH_MEMORY;
}

static void drop_buffer(struct fw_context *ctx, void *data)
{
    if (data == ctx->req->buffer)
        ctx->user_used = false;
    else
        fw_free(ctx, data);
}

static NH_STATUS android_load(void *context, const char *path, void **data, uint32_t *bytes)
{
    struct fw_context *ctx = context;
    NH_FILE file;
    NH_STATUS status;

    *data = NULL;
    *bytes = 0;
    if (!ctx->fs_open)
        return NH_INVALID;
    NH_TRY(NhFileOpen(&ctx->fs, path, &file));
    if ((file.Mode & FW_MODE_TYPE) != FW_MODE_REG)
        return NH_CORRUPT; /* a non-file at a firmware path is not absence */
    NH_TRY(fill_buffer(ctx, file.Size, data));
    status = NhFileRead(&file, 0, *data, (size_t)file.Size);
    if (status != NH_OK) {
        drop_buffer(ctx, *data);
        *data = NULL;
        return status;
    }
    *bytes = (uint32_t)file.Size;
    return NH_OK;
}

static void android_close(void *context)
{
    struct fw_context *ctx = context;

    if (ctx->fs_open)
        NhFsClose(&ctx->fs);
    if (ctx->view_open)
        NhViewClose(&ctx->view);
    ctx->fs_open = ctx->view_open = false;
}

static NH_STATUS policy_begin(void *context, uint32_t needs, uint32_t *slot)
{
    struct fw_context *ctx = context;

    NH_TRY(bind_disks(ctx));
    ctx->slot = NH_SLOT_UNKNOWN;
    if (needs) {
        uint32_t current;

        if (kshim_bootslot_get(&current))
            return NH_NOT_READY;
        ctx->slot = current;
    }
    *slot = ctx->slot;
    return NH_OK;
}

static NH_STATUS policy_open_android(void *context, const NH_CANDIDATE *candidate,
                                     NH_SOURCE *source)
{
    struct fw_context *ctx = context;
    const NH_READER *reader;
    NH_STATUS status;

    android_close(ctx);
    NH_TRY(find_partition(ctx, candidate->Partition, &ctx->part));
    reader = &ctx->part.reader;
    if (candidate->Logical[0]) {
        NH_TRY(admit_super(ctx));
        status = NhSuperOpen(reader, ctx->slot, candidate->Logical, &ctx->view);
        if (status != NH_OK) {
            NhViewClose(&ctx->view);
            return status;
        }
        ctx->view_open = true;
        reader = &ctx->view.Reader;
    }
    status = NhFsOpen(reader, &ctx->fs);
    if (status != NH_OK) {
        android_close(ctx);
        return status;
    }
    ctx->fs_open = true;
    ctx->selected = ctx->part;
    *source = (NH_SOURCE){ ctx, android_load, android_close };
    return NH_OK;
}

/* Recheck storage, slot, Virtual A/B state and the last selected partition
 * before a result or an absence verdict is returned. */
static NH_STATUS policy_stable(void *context)
{
    struct fw_context *ctx = context;

    for (unsigned i = 0; i < ctx->disk_count; ++i) {
        const struct fw_disk *disk = &ctx->disks[i];

        if (kshim_blkdev_find(disk->dev->name) != disk->dev ||
            disk->dev->block_size != disk->block_size ||
            disk->dev->num_blocks != disk->num_blocks)
            return NH_NOT_READY;
    }
    if (ctx->slot != NH_SLOT_UNKNOWN) {
        uint32_t current;

        if (kshim_bootslot_get(&current) || current != ctx->slot)
            return NH_NOT_READY;
    }
    if (ctx->admitted) {
        uint8_t message[64];

        NH_TRY(read_snapshot(ctx, message));
        if (memcmp(message, ctx->snapshot, sizeof(message)))
            return NH_NOT_READY;
    }
    if (ctx->selected.disk) {
        struct fw_partition again;
        NH_STATUS status = find_partition(ctx, ctx->selected.name, &again);

        if (status == NH_NOT_FOUND)
            return NH_NOT_READY;
        NH_TRY(status);
        if (again.disk != ctx->selected.disk || again.offset != ctx->selected.offset ||
            again.bytes != ctx->selected.bytes)
            return NH_NOT_READY;
    }
    return NH_OK;
}

static NH_STATUS builtin_load(void *context, const char *path, void **data, uint32_t *bytes)
{
    struct fw_context *ctx = context;

    *data = NULL;
    *bytes = 0;
    for (const struct kshim_fw_builtin *it = builtins; it; it = it->next) {
        if (strcmp(it->path, path))
            continue;
        NH_TRY(fill_buffer(ctx, it->size, data));
        memcpy(*data, it->data, it->size);
        *bytes = (uint32_t)it->size;
        return NH_OK;
    }
    return NH_NOT_FOUND;
}

static void builtin_close(void *context)
{
    (void)context;
}

static NH_STATUS policy_open_package(void *context, NH_SOURCE *source)
{
    *source = (NH_SOURCE){ context, builtin_load, builtin_close };
    return NH_OK;
}

static void policy_end(void *context)
{
    android_close(context);
}

/* ---- public API ------------------------------------------------------------ */

int kshim_firmware_builtin_register(struct kshim_fw_builtin *entry)
{
    if (!entry || !entry->path || (!entry->data && entry->size) ||
        NhPathCheck(entry->path, 0) != NH_OK || entry->size > KSHIM_FS_FILE_MAX)
        return ERROR_INVALID_PARAMETER;
    for (const struct kshim_fw_builtin *it = builtins; it; it = it->next)
        if (it == entry || !strcmp(it->path, entry->path))
            return KSHIM_ERR_EXISTS;
    entry->next = builtins;
    builtins = entry;
    return ERROR_NONE;
}

void kshim_firmware_builtin_clear(void)
{
    builtins = NULL;
}

static int io_error(const struct fw_context *ctx)
{
    if (ctx->part.window.last_error)
        return ctx->part.window.last_error;
    if (ctx->misc.window.last_error)
        return ctx->misc.window.last_error;
    for (unsigned i = 0; i < ctx->disk_count; ++i)
        if (ctx->disks[i].window.last_error)
            return ctx->disks[i].window.last_error;
    return 0;
}

static char *join_candidate(const char *location, const char *name)
{
    size_t a = strlen(location), b = strlen(name);
    bool bare = a && location[a - 1] == '|';
    char *text;

    if (a + b + 2 > NH_MAX_PATH)
        return NULL;
    text = malloc(a + b + 2);
    if (!text)
        return NULL;
    memcpy(text, location, a);
    if (!bare)
        text[a++] = '/';
    memcpy(text + a, name, b + 1);
    return text;
}

int kshim_firmware_request(const char *name, const struct kshim_fw_request *req,
                           struct kshim_firmware *fw)
{
    static const struct kshim_fw_request defaults = { .kind = KSHIM_FW_FILE };
    const char *const *locations;
    const char *package[2];
    char *android[KSHIM_FW_MAX_LOCATIONS * 2] = { NULL };
    unsigned count, stride, android_count = 0;
    struct fw_context *ctx;
    struct fw_result *result;
    NH_BUNDLE *bundle = NULL;
    uint32_t index = UINT32_MAX;
    NH_STATUS nh;
    int status;

    if (!fw)
        return ERROR_INVALID_PARAMETER;
    memset(fw, 0, sizeof(*fw));
    fw->slot = KSHIM_SLOT_UNKNOWN;
    if (!req)
        req = &defaults;
    if (!name || NhPathCheck(name, 0) != NH_OK ||
        (req->alternate && NhPathCheck(req->alternate, 0) != NH_OK) ||
        (req->kind != KSHIM_FW_FILE && req->kind != KSHIM_FW_PIL) ||
        (req->buffer && !req->buffer_size) ||
        (req->locations && !req->location_count && !req->allow_builtin))
        return ERROR_INVALID_PARAMETER;
    locations = req->locations ? req->locations : kshim_firmware_default_locations;
    count = req->locations ? req->location_count : kshim_firmware_default_location_count;
    if (!req->locations && req->location_count)
        return ERROR_INVALID_PARAMETER;
    if (count > KSHIM_FW_MAX_LOCATIONS)
        return KSHIM_ERR_TOO_LARGE;
    stride = req->alternate ? 2 : 1;

    ctx = calloc(1, sizeof(*ctx));
    result = calloc(1, sizeof(*result));
    if (!ctx || !result) {
        status = KSHIM_ERR_NO_MEMORY;
        goto out;
    }
    ctx->req = req;
    ctx->max_size = req->max_size && req->max_size < KSHIM_FS_FILE_MAX ?
                    req->max_size : KSHIM_FS_FILE_MAX;
    for (unsigned i = 0; i < count; ++i) {
        if (!locations[i]) {
            status = ERROR_INVALID_PARAMETER;
            goto out;
        }
        for (unsigned j = 0; j < stride; ++j) {
            android[android_count] = join_candidate(locations[i], j ? req->alternate : name);
            if (!android[android_count++]) {
                status = ERROR_INVALID_PARAMETER;
                goto out;
            }
        }
    }
    package[0] = name;
    package[1] = req->alternate;
    {
        const NH_POLICY policy = {
            .AndroidPaths = (const char *const *)android,
            .AndroidCount = android_count,
            .PackagePaths = package,
            .PackageCount = req->allow_builtin ? stride : 0,
            .Kind = req->kind == KSHIM_FW_PIL ? NH_BUNDLE_PIL : NH_BUNDLE_FILE,
        };
        const NH_POLICY_IO io = {
            .Context = ctx, .Alloc = fw_alloc, .Free = fw_free,
            .Begin = policy_begin, .OpenAndroid = policy_open_android,
            .Stable = policy_stable, .OpenPackage = policy_open_package,
            .End = policy_end,
        };

        nh = NhPolicyRead(&policy, &io, &bundle, &index);
    }
    status = kshim_nh_status(nh, io_error(ctx));
    if (status)
        goto out;

    result->bundle = bundle;
    result->user_buffer = ctx->user_used ? req->buffer : NULL;
    fw->data = bundle->Parts[NH_HEADER_PART].Data;
    fw->size = bundle->Parts[NH_HEADER_PART].Bytes;
    fw->origin = bundle->Origin;
    fw->part_count = bundle->Count ? bundle->Count - 1 : 0;
    fw->location_index = bundle->Origin == NH_ORIGIN_ANDROID ? index / stride : index;
    /* NH_SLOT_UNKNOWN == KSHIM_SLOT_UNKNOWN (UINT32_MAX) */
    fw->slot = bundle->Origin == NH_ORIGIN_ANDROID ? ctx->slot : KSHIM_SLOT_UNKNOWN;
    strncpy(fw->name, index % stride ? req->alternate : name, sizeof(fw->name) - 1);
    fw->priv = result;
    result = NULL;
    if (fw->origin == KSHIM_FW_ORIGIN_ANDROID)
        printf("firmware: %s from %s (%u bytes, %u parts)\n", fw->name,
               locations[fw->location_index], (unsigned)fw->size, (unsigned)fw->part_count);
    else
        printf("firmware: %s from builtin (%u bytes)\n", fw->name, (unsigned)fw->size);
out:
    for (unsigned i = 0; i < android_count; ++i)
        free(android[i]);
    free(result);
    free(ctx);
    return status;
}

int kshim_firmware_part(const struct kshim_firmware *fw, uint32_t index,
                        const void **data, size_t *size)
{
    const struct fw_result *result;

    if (!fw || !fw->priv || !data || !size || index >= KSHIM_FW_MAX_PARTS)
        return ERROR_INVALID_PARAMETER;
    result = fw->priv;
    *data = result->bundle->Parts[index].Data;
    *size = result->bundle->Parts[index].Bytes;
    return *data ? ERROR_NONE : ERROR_NOT_FOUND;
}

void kshim_firmware_release(struct kshim_firmware *fw)
{
    struct fw_result *result;

    if (!fw || !fw->priv)
        return;
    result = fw->priv;
    NhBundleFree(result->user_buffer, result_free, result->bundle);
    free(result);
    memset(fw, 0, sizeof(*fw));
    fw->slot = KSHIM_SLOT_UNKNOWN;
}

int kshim_firmware_load(const char *name, void *buffer, size_t cap, size_t *size)
{
    struct kshim_fw_request req = {
        .kind = KSHIM_FW_FILE, .buffer = buffer, .buffer_size = cap,
    };
    struct kshim_firmware fw;
    int status;

    if (size)
        *size = 0;
    if (!buffer || !cap)
        return ERROR_INVALID_PARAMETER;
    status = kshim_firmware_request(name, &req, &fw);
    if (status)
        return status;
    if (size)
        *size = fw.size;
    kshim_firmware_release(&fw);
    return ERROR_NONE;
}
