/* SPDX-License-Identifier: MIT */
/* Filesystem adapter over images made by mkfs.vfat/mtools, mkfs.ext4 -d and
 * mkfs.f2fs/sload.f2fs (and mkfs.erofs when present): contents, unaligned
 * reads with tiny transfer limits, absence, symlinks, bounds, corruption,
 * I/O errors and configured-out readers. */
#include <kshim/fs.h>

#include "storage_test.h"

static const char *fixtures;

struct fs_case {
    const char *image;
    const char *payload;      /* reference tree under payload/ */
    enum kshim_fs_type type;
    uint32_t block_size;
    bool symlinks;
    /* byte to flip in the primary superblock, expected status */
    size_t corrupt_at;
};

static const struct fs_case cases[] = {
    { "fat12.img", "payload/fsfat", KSHIM_FS_FAT, 512, false, 11 },
    { "fat16.img", "payload/fsfat", KSHIM_FS_FAT, 512, false, 11 },
    { "fat32.img", "payload/fsfat", KSHIM_FS_FAT, 4096, false, 11 },
    { "ext4.img", "payload/fs", KSHIM_FS_EXT4, 4096, true, 1024 + 768 },
    { "f2fs.img", "payload/fs", KSHIM_FS_F2FS, 4096, true, 1024 + 100 },
    { "erofs.img", "payload/fs", KSHIM_FS_EROFS, 4096, true, 1024 + 200 },
};

static const char *const files[] = {
    "firmware/deep/pattern-0.bin", "firmware/deep/pattern-1.bin",
    "firmware/deep/pattern-4095.bin", "firmware/deep/pattern-4096.bin",
    "firmware/deep/pattern-4097.bin", "firmware/deep/pattern-65537.bin",
    "firmware/deep/pattern-1048591.bin", "top.txt",
};

static void contents(struct kshim_fs *fs, const struct fs_case *c)
{
    char payload[1024];

    snprintf(payload, sizeof(payload), "%s/%s", fixtures, c->payload);
    for (size_t i = 0; i < sizeof(files) / sizeof(files[0]); ++i) {
        size_t ref_size = 0, size = 0;
        uint8_t *ref = host_file(payload, files[i], &ref_size);
        uint8_t *buf = malloc(ref_size + 1);
        void *loaded = NULL;
        struct kshim_fs_stat st;
        char slash[600];

        CHECK(ref != NULL);
        CHECK_EQ(kshim_fs_stat(fs, files[i], &st), 0);
        CHECK_EQ(st.size, ref_size);
        CHECK_EQ(st.mode & KSHIM_FS_MODE_TYPE, KSHIM_FS_MODE_REG);
        CHECK_EQ(kshim_fs_read_file(fs, files[i], buf, ref_size, &size), 0);
        CHECK_EQ(size, ref_size);
        CHECK(!memcmp(buf, ref, ref_size));
        snprintf(slash, sizeof(slash), "/%s", files[i]); /* leading '/' accepted */
        CHECK_EQ(kshim_fs_load_file(fs, slash, 0, &loaded, &size), 0);
        CHECK(size == ref_size && loaded && !memcmp(loaded, ref, ref_size));
        if (ref_size) {
            CHECK_EQ(kshim_fs_read_file(fs, files[i], buf, ref_size - 1, &size), KSHIM_ERR_TOO_LARGE);
            CHECK_EQ(size, ref_size);
        }
        free(loaded);
        free(buf);
        free(ref);
    }

    /* Unaligned ranges in one large file. */
    {
        const char *rel = "firmware/deep/pattern-1048591.bin";
        size_t ref_size;
        uint8_t *ref = host_file(payload, rel, &ref_size);
        struct kshim_file *file = NULL;
        static const uint64_t offsets[] = {0, 1, 511, 4095, 4097, 65535, 1048590, 777777};
        static const size_t lengths[] = {1, 3, 512, 4097, 70001};
        uint8_t *buf = malloc(70001 + 1);

        CHECK_EQ(kshim_fs_open(fs, rel, &file), 0);
        if (file && ref) {
            CHECK_EQ(kshim_file_size(file), ref_size);
            for (size_t o = 0; o < sizeof(offsets) / sizeof(offsets[0]); ++o)
                for (size_t l = 0; l < sizeof(lengths) / sizeof(lengths[0]); ++l) {
                    uint64_t off = offsets[o];
                    size_t len = lengths[l];

                    if (off + len > ref_size) {
                        CHECK_EQ(kshim_file_read(file, off, buf + 1, len), ERROR_INVALID_PARAMETER);
                        continue;
                    }
                    CHECK_EQ(kshim_file_read(file, off, buf + 1, len), 0);
                    CHECK(!memcmp(buf + 1, ref + off, len));
                }
            CHECK_EQ(kshim_file_read(file, ref_size, buf, 0), 0);
            CHECK_EQ(kshim_file_read(file, ref_size, buf, 1), ERROR_INVALID_PARAMETER);
        }
        kshim_file_close(file);
        free(buf);
        free(ref);
    }
}

static void negatives(struct kshim_fs *fs, const struct fs_case *c)
{
    struct kshim_fs_stat st;
    struct kshim_file *file = (struct kshim_file *)1;
    uint8_t small[16];
    size_t size = 0;
    void *data = (void *)1;

    CHECK_EQ(kshim_fs_stat(fs, "firmware/deep/absent.bin", &st), ERROR_NOT_FOUND);
    CHECK_EQ(kshim_fs_stat(fs, "nope/pattern-1.bin", &st), ERROR_NOT_FOUND);
    CHECK_EQ(kshim_fs_open(fs, "firmware/deep/absent.bin", &file), ERROR_NOT_FOUND);
    CHECK(file == NULL);
    CHECK_EQ(kshim_fs_stat(fs, "", &st), 0);
    CHECK_EQ(st.mode & KSHIM_FS_MODE_TYPE, KSHIM_FS_MODE_DIR);
    CHECK_EQ(kshim_fs_stat(fs, "firmware/deep", &st), 0);
    CHECK_EQ(st.mode & KSHIM_FS_MODE_TYPE, KSHIM_FS_MODE_DIR);
    CHECK_EQ(kshim_fs_open(fs, "firmware/deep", &file), ERROR_NOT_SUPPORTED);
    CHECK_EQ(kshim_fs_read_file(fs, "firmware", small, sizeof(small), &size), ERROR_NOT_SUPPORTED);
    /* A regular file used as a directory is metadata misuse, never success. */
    CHECK(kshim_fs_stat(fs, "top.txt/x", &st) != 0);
    CHECK_EQ(kshim_fs_stat(fs, "../top.txt", &st), ERROR_INVALID_PARAMETER);
    CHECK_EQ(kshim_fs_stat(fs, "firmware//deep", &st), ERROR_INVALID_PARAMETER);
    CHECK_EQ(kshim_fs_stat(fs, "firmware/./deep", &st), ERROR_INVALID_PARAMETER);
    CHECK_EQ(kshim_fs_stat(fs, "firmware/deep/", &st), ERROR_INVALID_PARAMETER);
    CHECK_EQ(kshim_fs_stat(fs, NULL, &st), ERROR_INVALID_PARAMETER);
    if (c->symlinks)
        CHECK_EQ(kshim_fs_stat(fs, "firmware/deep/relative-link", &st), ERROR_NOT_SUPPORTED);
    /* Oversize: 3 MiB file against 1 MiB bounds. */
    CHECK_EQ(kshim_fs_read_file(fs, "firmware/big.bin", small, sizeof(small), &size),
             KSHIM_ERR_TOO_LARGE);
    CHECK_EQ(size, 3 * 1024 * 1024);
    CHECK_EQ(kshim_fs_load_file(fs, "firmware/big.bin", 1024 * 1024, &data, &size),
             KSHIM_ERR_TOO_LARGE);
    CHECK(data == NULL && size == 3 * 1024 * 1024);
    CHECK_EQ(kshim_fs_load_file(fs, "firmware/big.bin", 0, &data, &size), 0);
    CHECK(size == 3 * 1024 * 1024);
    free(data);
    {
        char longname[256];

        snprintf(longname, sizeof(longname), "firmware/deep/long-%0180d.bin", 0);
        memset(longname + 19, 'x', 180);
        CHECK_EQ(kshim_fs_read_file(fs, longname, small, sizeof(small), &size), 0);
        CHECK(size == 9 && !memcmp(small, "long name", 9));
    }
}

static void one_case(const struct fs_case *c)
{
    struct image img;
    struct kshim_memdisk disk;
    struct kshim_fs *fs = NULL;
    int status;

    if (image_map(&img, fixtures, c->image)) {
        if (c->type == KSHIM_FS_EROFS && kshim_fs_supported(KSHIM_FS_EROFS))
            printf("SKIP %s: mkfs.erofs not available\n", c->image);
        else if (c->type != KSHIM_FS_EROFS)
            CHECK(!"fixture missing");
        return;
    }
    /* max_transfer_blocks = 1 forces every multi-block read to split. */
    CHECK_EQ(kshim_memdisk_init(&disk, "img", img.data, img.size, c->block_size, 1, true), 0);
    status = kshim_fs_mount(&disk.dev, &fs);
    if (!kshim_fs_supported(c->type)) {
        /* Configured-out reader: recognised, refused, never misread. */
        if (c->type == KSHIM_FS_FAT)
            CHECK(status != 0);
        else
            CHECK_EQ(status, ERROR_NOT_SUPPORTED);
        CHECK(fs == NULL);
        printf("  %s: %s reader not built: %s\n", c->image, kshim_fs_type_name(c->type),
               kshim_storage_strerror(status));
        image_unmap(&img);
        return;
    }
    CHECK_EQ(status, 0);
    if (!fs) {
        image_unmap(&img);
        return;
    }
    CHECK_EQ(kshim_fs_get_type(fs), c->type);
    contents(fs, c);
    negatives(fs, c);
    CHECK(disk.largest_read == 1 && disk.read_calls > 100);

    /* Device error mid-file propagates the driver's code. */
    {
        struct kshim_file *file = NULL;
        uint8_t *buf = malloc(1048591);

        CHECK_EQ(kshim_fs_open(fs, "firmware/deep/pattern-1048591.bin", &file), 0);
        disk.fail_status = ERROR_TIMEOUT;
        disk.fail_lba = 0;
        disk.fail_count = disk.dev.num_blocks;
        CHECK_EQ(kshim_file_read(file, 0, buf, 1048591), ERROR_TIMEOUT);
        disk.fail_status = 0;
        CHECK_EQ(kshim_file_read(file, 0, buf, 1048591), 0);
        kshim_file_close(file);
        free(buf);
    }
    kshim_fs_unmount(fs);

    /* Corrupt superblock: an error, never a mount or "absent". */
    img.data[c->corrupt_at] ^= 0x01;
    status = kshim_fs_mount(&disk.dev, &fs);
    printf("  %s: corrupt superblock byte %zu -> %s\n", c->image, c->corrupt_at,
           kshim_storage_strerror(status));
    CHECK(status == KSHIM_ERR_CORRUPT || status == ERROR_NOT_SUPPORTED);
    CHECK(fs == NULL);
    img.data[c->corrupt_at] ^= 0x01;
    /* Device smaller than the declared filesystem.  ext4/F2FS refuse to
     * mount; CrDK's FAT reader deliberately admits factory images without
     * unused tail bytes, but must then refuse any read past the device. */
    {
        uint64_t keep = 0;
        struct kshim_file *file = NULL;
        uint8_t byte;

        for (uint64_t b = disk.dev.num_blocks; b > 0; b /= 2) {
            disk.dev.num_blocks = b;
            if (kshim_fs_mount(&disk.dev, &fs) == 0) {
                keep = b;
                if (kshim_fs_open(fs, "firmware/deep/pattern-1048591.bin", &file) == 0) {
                    uint64_t size = kshim_file_size(file);

                    status = kshim_file_read(file, size - 1, &byte, 1);
                    /* Either the last byte is inside the device and correct,
                     * or the read is refused. */
                    if (!status)
                        CHECK_EQ(byte, (uint8_t)(((size - 1) * 31 + (size - 1) / 173) % 251));
                    kshim_file_close(file);
                }
                kshim_fs_unmount(fs);
                fs = NULL;
            } else if (c->type != KSHIM_FS_FAT) {
                break;
            }
        }
        if (c->type != KSHIM_FS_FAT)
            CHECK(keep == 0 || keep == img.size / c->block_size);
        printf("  %s: smallest mountable device %llu blocks\n", c->image,
               (unsigned long long)keep);
    }
    image_unmap(&img);
    printf("  %s: ok (%s)\n", c->image, kshim_fs_type_name(c->type));
}

int main(int argc, char **argv)
{
    fixtures = fixture_dir(argc, argv);
    if (!fixtures) {
        printf("SKIP fs_test: fixtures not generated\n");
        return SKIP_CODE;
    }
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i)
        one_case(&cases[i]);
    /* Garbage is not a filesystem. */
    {
        uint8_t *junk = malloc(1024 * 1024);
        struct kshim_memdisk disk;
        struct kshim_fs *fs = NULL;

        for (size_t i = 0; i < 1024 * 1024; ++i)
            junk[i] = (uint8_t)(i * 7 + 3);
        CHECK_EQ(kshim_memdisk_init(&disk, "junk", junk, 1024 * 1024, 512, 0, true), 0);
        CHECK(kshim_fs_mount(&disk.dev, &fs) != 0 && fs == NULL);
        free(junk);
    }
    return test_finish("fs_test");
}
