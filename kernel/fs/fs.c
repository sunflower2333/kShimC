/* SPDX-License-Identifier: MIT */
/* Read-only filesystem adapter over CrDK FirmwareLoaderLib Nh* readers. */
#include <stdlib.h>
#include <string.h>

#include <config.h>
#include <kshim/fs.h>

#include "nh_glue.h"

_Static_assert(KSHIM_FS_EXT4 == NH_FS_EXT4 && KSHIM_FS_FAT == NH_FS_FAT &&
               KSHIM_FS_EROFS == NH_FS_EROFS && KSHIM_FS_F2FS == NH_FS_F2FS,
               "filesystem identifiers follow FirmwareLoaderLib");
_Static_assert(KSHIM_FS_FILE_MAX == NH_MAX_FILE && KSHIM_FS_PATH_MAX == NH_MAX_PATH,
               "bounds follow FirmwareLoaderLib");

struct kshim_fs {
    NH_FS nh;
    struct kshim_nh_window window;
};

struct kshim_file {
    NH_FILE nh;
    struct kshim_fs *fs;
};

/* ---- glue --------------------------------------------------------------- */

static NH_STATUS window_read(void *context, uint64_t offset, void *buffer, size_t bytes)
{
    struct kshim_nh_window *window = context;
    int status;

    if (offset > window->bytes || bytes > window->bytes - offset)
        return NH_CORRUPT;
    status = kshim_blkdev_read_bytes(window->dev, window->base + offset, bytes, buffer);
    if (status) {
        window->last_error = status;
        return status == KSHIM_ERR_NO_MEMORY ? NH_MEMORY : NH_IO;
    }
    return NH_OK;
}

void *kshim_nh_alloc(void *context, size_t bytes)
{
    (void)context;
    return malloc(bytes ? bytes : 1);
}

void kshim_nh_free(void *context, void *pointer)
{
    (void)context;
    free(pointer);
}

void kshim_nh_reader(struct kshim_nh_window *window, NH_READER *reader)
{
    reader->Context = window;
    reader->Read = window_read;
    reader->Bytes = window->bytes;
    reader->Alloc = kshim_nh_alloc;
    reader->Free = kshim_nh_free;
}

int kshim_nh_status(NH_STATUS status, int io_error)
{
    switch (status) {
    case NH_OK: return ERROR_NONE;
    case NH_NOT_FOUND: return ERROR_NOT_FOUND;
    case NH_CORRUPT: return KSHIM_ERR_CORRUPT;
    case NH_UNSUPPORTED: return ERROR_NOT_SUPPORTED;
    case NH_IO: return io_error ? io_error : ERROR_DEVICE_ERROR;
    case NH_MEMORY: return KSHIM_ERR_NO_MEMORY;
    case NH_INVALID: return ERROR_INVALID_PARAMETER;
    case NH_NOT_READY: return KSHIM_ERR_NOT_READY;
    case NH_TOO_LARGE: return KSHIM_ERR_TOO_LARGE;
    default: return ERROR_DEVICE_ERROR;
    }
}

int kshim_nh_path(const char *path, const char **out)
{
    if (!path || !out)
        return ERROR_INVALID_PARAMETER;
    while (*path == '/')
        ++path;
    if (NhPathCheck(path, 1) != NH_OK)
        return ERROR_INVALID_PARAMETER;
    *out = path;
    return ERROR_NONE;
}

/* ---- public API ----------------------------------------------------------- */

bool kshim_fs_supported(enum kshim_fs_type type)
{
    switch (type) {
#ifdef CONFIG_FS_EXT4
    case KSHIM_FS_EXT4: return true;
#endif
#ifdef CONFIG_FS_FAT
    case KSHIM_FS_FAT: return true;
#endif
#ifdef CONFIG_FS_EROFS
    case KSHIM_FS_EROFS: return true;
#endif
#ifdef CONFIG_FS_F2FS
    case KSHIM_FS_F2FS: return true;
#endif
    default: return false;
    }
}

const char *kshim_fs_type_name(enum kshim_fs_type type)
{
    switch (type) {
    case KSHIM_FS_EXT4: return "ext4";
    case KSHIM_FS_FAT: return "fat";
    case KSHIM_FS_EROFS: return "erofs";
    case KSHIM_FS_F2FS: return "f2fs";
    default: return "none";
    }
}

static int fs_result(struct kshim_fs *fs, NH_STATUS status)
{
    return kshim_nh_status(status, fs->window.last_error);
}

int kshim_fs_mount(struct kshim_blkdev *dev, struct kshim_fs **out)
{
    struct kshim_fs *fs;
    NH_READER reader;
    NH_STATUS status;
    int result;

    if (!out)
        return ERROR_INVALID_PARAMETER;
    *out = NULL;
    if (!dev || !kshim_blkdev_bytes(dev))
        return ERROR_INVALID_PARAMETER;
    fs = calloc(1, sizeof(*fs));
    if (!fs)
        return KSHIM_ERR_NO_MEMORY;
    fs->window.dev = dev;
    fs->window.bytes = kshim_blkdev_bytes(dev);
    kshim_nh_reader(&fs->window, &reader);
    status = NhFsOpen(&reader, &fs->nh);
    result = fs_result(fs, status);
    if (result) {
        free(fs);
        return result;
    }
    *out = fs;
    return ERROR_NONE;
}

void kshim_fs_unmount(struct kshim_fs *fs)
{
    if (!fs)
        return;
    NhFsClose(&fs->nh);
    free(fs);
}

enum kshim_fs_type kshim_fs_get_type(const struct kshim_fs *fs)
{
    return fs ? (enum kshim_fs_type)fs->nh.Type : KSHIM_FS_NONE;
}

static int lookup(struct kshim_fs *fs, const char *path, NH_FILE *file)
{
    const char *relative;
    int status;

    if (!fs || !file)
        return ERROR_INVALID_PARAMETER;
    status = kshim_nh_path(path, &relative);
    if (status)
        return status;
    fs->window.last_error = 0;
    return fs_result(fs, NhFileOpen(&fs->nh, relative, file));
}

int kshim_fs_stat(struct kshim_fs *fs, const char *path, struct kshim_fs_stat *st)
{
    NH_FILE file;
    int status;

    if (!st)
        return ERROR_INVALID_PARAMETER;
    status = lookup(fs, path, &file);
    if (status)
        return status;
    st->size = file.Size;
    st->mode = file.Mode;
    st->inode = file.Id;
    return ERROR_NONE;
}

int kshim_fs_open(struct kshim_fs *fs, const char *path, struct kshim_file **out)
{
    struct kshim_file *file;
    int status;

    if (!out)
        return ERROR_INVALID_PARAMETER;
    *out = NULL;
    file = calloc(1, sizeof(*file));
    if (!file)
        return KSHIM_ERR_NO_MEMORY;
    status = lookup(fs, path, &file->nh);
    if (!status && (file->nh.Mode & KSHIM_FS_MODE_TYPE) != KSHIM_FS_MODE_REG)
        status = ERROR_NOT_SUPPORTED;
    if (!status && file->nh.Size > KSHIM_FS_FILE_MAX)
        status = KSHIM_ERR_TOO_LARGE;
    if (status) {
        free(file);
        return status;
    }
    file->fs = fs;
    *out = file;
    return ERROR_NONE;
}

uint64_t kshim_file_size(const struct kshim_file *file)
{
    return file ? file->nh.Size : 0;
}

int kshim_file_read(struct kshim_file *file, uint64_t offset, void *buf, size_t len)
{
    if (!file || (!buf && len) || offset > file->nh.Size || len > file->nh.Size - offset)
        return ERROR_INVALID_PARAMETER;
    if (!len)
        return ERROR_NONE;
    file->fs->window.last_error = 0;
    return fs_result(file->fs, NhFileRead(&file->nh, offset, buf, len));
}

void kshim_file_close(struct kshim_file *file)
{
    free(file);
}

int kshim_fs_read_file(struct kshim_fs *fs, const char *path,
                       void *buf, size_t cap, size_t *size)
{
    struct kshim_file *file;
    int status;

    if (size)
        *size = 0;
    if (!buf && cap)
        return ERROR_INVALID_PARAMETER;
    status = kshim_fs_open(fs, path, &file);
    if (status)
        return status;
    if (size)
        *size = (size_t)file->nh.Size;
    if (file->nh.Size > cap)
        status = KSHIM_ERR_TOO_LARGE;
    else
        status = kshim_file_read(file, 0, buf, (size_t)file->nh.Size);
    kshim_file_close(file);
    return status;
}

int kshim_fs_load_file(struct kshim_fs *fs, const char *path, size_t max_size,
                       void **data, size_t *size)
{
    struct kshim_file *file;
    void *buffer;
    int status;

    if (!data || !size)
        return ERROR_INVALID_PARAMETER;
    *data = NULL;
    *size = 0;
    if (!max_size || max_size > KSHIM_FS_FILE_MAX)
        max_size = KSHIM_FS_FILE_MAX;
    status = kshim_fs_open(fs, path, &file);
    if (status)
        return status;
    if (file->nh.Size > max_size) {
        *size = (size_t)file->nh.Size;
        kshim_file_close(file);
        return KSHIM_ERR_TOO_LARGE;
    }
    buffer = malloc(file->nh.Size ? (size_t)file->nh.Size : 1);
    if (!buffer) {
        kshim_file_close(file);
        return KSHIM_ERR_NO_MEMORY;
    }
    status = kshim_file_read(file, 0, buffer, (size_t)file->nh.Size);
    if (!status) {
        *data = buffer;
        *size = (size_t)file->nh.Size;
    } else {
        free(buffer);
    }
    kshim_file_close(file);
    return status;
}
