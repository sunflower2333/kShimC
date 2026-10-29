/* SPDX-License-Identifier: MIT */
/* GPT scanning against sgdisk (512-byte) and an independent 4096-byte writer,
 * cross-checked with CrDK NhGptFind; corruption, raw LUNs, naming. */
#include <Library/FirmwareLoaderLib/NhFs.h>

#include "storage_test.h"

static NH_STATUS mem_read(void *ctx, uint64_t off, void *buf, size_t n)
{
    struct kshim_memdisk *disk = ctx;

    return kshim_blkdev_read_bytes(&disk->dev, off, n, buf) ? NH_IO : NH_OK;
}
static void *mem_alloc(void *ctx, size_t n) { (void)ctx; return malloc(n ? n : 1); }
static void mem_free(void *ctx, void *p) { (void)ctx; free(p); }

static void check_manifest(const char *dir, const char *disk_tag, struct kshim_memdisk *disk)
{
    char path[1024], tag[32], label[64];
    unsigned long long first, blocks;
    unsigned matched = 0;
    FILE *f;
    NH_READER reader = { disk, mem_read, kshim_blkdev_bytes(&disk->dev), mem_alloc, mem_free };

    snprintf(path, sizeof(path), "%s/manifest.txt", dir);
    f = fopen(path, "r");
    CHECK(f != NULL);
    if (!f)
        return;
    while (fscanf(f, "%31s %63s %llu %llu", tag, label, &first, &blocks) == 4) {
        struct kshim_partition *part = NULL;
        uint64_t nh_offset = 0, nh_bytes = 0;
        char name[64];
        int n;

        if (strcmp(tag, disk_tag))
            continue;
        CHECK_EQ(kshim_partition_find(label, &disk->dev, &part), 0);
        if (!part)
            continue;
        CHECK_EQ(part->dev.start_lba, first);
        CHECK_EQ(part->dev.num_blocks, blocks);
        CHECK(part->dev.parent == &disk->dev && part->dev.kind == KSHIM_BLKDEV_PARTITION);
        CHECK(part->dev.read_only);
        n = snprintf(name, sizeof(name), "%s:%s", disk->dev.name, label);
        CHECK(n > 0 && (size_t)n < sizeof(name));
        CHECK(kshim_blkdev_find(name) == &part->dev);
        CHECK(kshim_partition_of(&part->dev) == part);
        /* Independent implementation agrees on every partition. */
        CHECK_EQ(NhGptFind(&reader, disk->dev.block_size, label, &nh_offset, &nh_bytes), NH_OK);
        CHECK_EQ(nh_offset, first * disk->dev.block_size);
        CHECK_EQ(nh_bytes, blocks * disk->dev.block_size);
        matched++;
    }
    fclose(f);
    CHECK(matched >= 2);
}

static void scan_fixture(const char *dir, const char *file, const char *tag, uint32_t bs,
                         unsigned expected)
{
    struct image img;
    struct kshim_memdisk disk;
    struct kshim_partition *part;
    unsigned count = 99;

    CHECK_EQ(image_map(&img, dir, file), 0);
    if (!img.data)
        return;
    CHECK_EQ(kshim_memdisk_init(&disk, tag[3] == '0' ? "mem0:lu0" : "mem0:lu4", img.data,
                                img.size, bs, 8, true), 0);
    CHECK_EQ(kshim_gpt_scan(&disk.dev, &count), ERROR_INVALID_PARAMETER); /* unregistered */
    CHECK_EQ(kshim_blkdev_register(&disk.dev), 0);
    CHECK_EQ(kshim_gpt_scan(&disk.dev, &count), 0);
    CHECK_EQ(count, expected);
    check_manifest(dir, tag, &disk);
    CHECK_EQ(kshim_gpt_scan(&disk.dev, &count), KSHIM_ERR_EXISTS);
    CHECK_EQ(count, 0);

    if (bs == 512) {
        /* sgdisk wrote "rés": kept, but not matchable and named by index. */
        struct kshim_blkdev *p5 = kshim_blkdev_find("mem0:lu4:p5");
        struct kshim_partition *odd = kshim_partition_of(p5);

        CHECK(odd && !odd->label_ascii && !strcmp(odd->label, "r?s"));
        CHECK_EQ(kshim_partition_find("r?s", NULL, &part), ERROR_NOT_FOUND);
        CHECK_EQ(kshim_partition_find_slot("modem", 1, NULL, &part), 0);
        CHECK(part && !strcmp(part->label, "modem_b"));
        CHECK_EQ(kshim_partition_find_slot("modem", 2, NULL, &part), ERROR_INVALID_PARAMETER);
        CHECK_EQ(kshim_partition_find("modem", NULL, &part), ERROR_NOT_FOUND);
        /* Reading through the partition is a window onto the disk. */
        CHECK_EQ(kshim_partition_find("modem_a", NULL, &part), 0);
        if (part) {
            uint8_t sector[512];

            CHECK_EQ(kshim_blkdev_read(&part->dev, 0, 1, sector), 0);
            CHECK(!memcmp(sector, img.data + part->dev.start_lba * 512, 512));
            CHECK(sector[510] == 0x55 && sector[511] == 0xaa); /* FAT boot sector */
        }
        /* The same LUN registered twice: the label becomes ambiguous. */
        {
            struct kshim_memdisk twin;

            CHECK_EQ(kshim_memdisk_init(&twin, "mem0:lu5", img.data, img.size, 512, 0, true), 0);
            CHECK_EQ(kshim_blkdev_register(&twin.dev), 0);
            CHECK_EQ(kshim_gpt_scan_all(), expected);
            CHECK_EQ(kshim_partition_find("modem_a", NULL, &part), KSHIM_ERR_CORRUPT);
            CHECK_EQ(kshim_partition_find("modem_a", &twin.dev, &part), 0);
            CHECK(part && part->dev.parent == &twin.dev);
            CHECK_EQ(kshim_blkdev_unregister(&twin.dev), KSHIM_ERR_BUSY);
            CHECK_EQ(kshim_gpt_release(&twin.dev), 0);
            CHECK_EQ(kshim_blkdev_unregister(&twin.dev), 0);
        }
    }
    CHECK_EQ(kshim_gpt_release(&disk.dev), 0);
    CHECK_EQ(kshim_gpt_release(&disk.dev), ERROR_NOT_FOUND);
    CHECK(kshim_blkdev_first() == &disk.dev && !disk.dev.next);
    /* Long disk names fall back to "<disk>:p<N>". */
    CHECK_EQ(kshim_blkdev_unregister(&disk.dev), 0);
    strcpy(disk.dev.name, "mem0:a-very-long-lun-name-xx");
    CHECK_EQ(kshim_blkdev_register(&disk.dev), 0);
    CHECK_EQ(kshim_gpt_scan(&disk.dev, &count), 0);
    CHECK(kshim_blkdev_find("mem0:a-very-long-lun-name-xx:p1") != NULL);
    CHECK_EQ(kshim_partition_find(bs == 512 ? "modem_a" : "misc", NULL, &part), 0);
    CHECK_EQ(kshim_gpt_release(&disk.dev), 0);
    CHECK_EQ(kshim_blkdev_unregister(&disk.dev), 0);
    image_unmap(&img);
}

/* Each corruption must reject the whole table and register nothing. */
static void corruption(const char *dir)
{
    struct image img;
    struct kshim_memdisk disk;
    unsigned count;
    struct { const char *what; bool from_end; size_t offset; } cases[] = {
        { "primary header CRC field", false, 512 + 16 },
        { "primary header usable range", false, 512 + 40 },
        { "primary entry name", false, 1024 + 56 },
        { "backup header LBA", true, 512 - 24 },
        { "backup entry array", true, 512 + 32 * 512 - 70 },
    };

    CHECK_EQ(image_map(&img, dir, "lun4.img"), 0);
    if (!img.data)
        return;
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        size_t at = cases[i].from_end ? img.size - cases[i].offset : cases[i].offset;
        int status;

        img.data[at] ^= 0x40;
        CHECK_EQ(kshim_memdisk_init(&disk, "mem9", img.data, img.size, 512, 0, true), 0);
        CHECK_EQ(kshim_blkdev_register(&disk.dev), 0);
        count = 99;
        status = kshim_gpt_scan(&disk.dev, &count);
        if (status != KSHIM_ERR_CORRUPT)
            fprintf(stderr, "corruption case: %s\n", cases[i].what);
        CHECK_EQ(status, KSHIM_ERR_CORRUPT);
        CHECK_EQ(count, 0);
        CHECK(!disk.dev.next && kshim_blkdev_first() == &disk.dev);
        if (!status)
            kshim_gpt_release(&disk.dev);
        CHECK_EQ(kshim_blkdev_unregister(&disk.dev), 0);
        img.data[at] ^= 0x40;
    }
    /* I/O failure while reading the entry array: error, nothing registered. */
    CHECK_EQ(kshim_memdisk_init(&disk, "mem9", img.data, img.size, 512, 0, true), 0);
    CHECK_EQ(kshim_blkdev_register(&disk.dev), 0);
    disk.fail_status = ERROR_TIMEOUT;
    disk.fail_lba = 3;
    disk.fail_count = 1;
    CHECK_EQ(kshim_gpt_scan(&disk.dev, &count), ERROR_TIMEOUT);
    CHECK(!disk.dev.next);
    disk.fail_status = 0;
    CHECK_EQ(kshim_gpt_scan(&disk.dev, &count), 0);
    CHECK_EQ(kshim_gpt_release(&disk.dev), 0);
    CHECK_EQ(kshim_blkdev_unregister(&disk.dev), 0);

    /* Raw LUN: no GPT, no protective MBR. */
    {
        uint8_t *raw = calloc(1, 64 * 512);

        CHECK_EQ(kshim_memdisk_init(&disk, "mem0:lu1", raw, 64 * 512, 512, 0, true), 0);
        CHECK_EQ(kshim_blkdev_register(&disk.dev), 0);
        CHECK_EQ(kshim_gpt_scan(&disk.dev, &count), ERROR_NOT_SUPPORTED);
        CHECK_EQ(kshim_gpt_scan_all(), 0);
        /* A protective MBR without either header is a damaged GPT disk. */
        memcpy(raw, img.data, 512);
        CHECK_EQ(kshim_gpt_scan(&disk.dev, &count), KSHIM_ERR_CORRUPT);
        CHECK_EQ(kshim_blkdev_unregister(&disk.dev), 0);
        free(raw);
    }
    image_unmap(&img);
}

int main(int argc, char **argv)
{
    const char *dir = fixture_dir(argc, argv);

    if (!dir) {
        printf("SKIP gpt_test: fixtures not generated\n");
        return SKIP_CODE;
    }
    scan_fixture(dir, "lun4.img", "lun4", 512, 5);
    scan_fixture(dir, "lun0.img", "lun0", 4096, 2);
    corruption(dir);
    return test_finish("gpt_test");
}
