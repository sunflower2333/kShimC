/* SPDX-License-Identifier: MIT */
/* Block core: registry, bounds, max_transfer_blocks splitting, unaligned
 * byte reads through the bounce buffer, partition windows and errors. */
#include "storage_test.h"

static uint8_t pat(uint64_t i) { return (uint8_t)((i * 131 + (i >> 9)) % 253); }

static void fill(uint8_t *p, size_t n)
{
    for (size_t i = 0; i < n; ++i)
        p[i] = pat(i);
}

static int negative_read(struct kshim_blkdev *dev, uint64_t lba, uint32_t count, void *buf)
{
    (void)dev; (void)lba; (void)count; (void)buf;
    return -5;
}

static void registry(void)
{
    static uint8_t a_data[8 * 512], b_data[8 * 512];
    struct kshim_memdisk a, b, dup;
    struct kshim_blkdev child = {0}, bad = {0};

    CHECK_EQ(kshim_memdisk_init(&a, "", a_data, sizeof(a_data), 512, 0, true), ERROR_INVALID_PARAMETER);
    CHECK_EQ(kshim_memdisk_init(&a, "x", a_data, 1000, 512, 0, true), ERROR_INVALID_PARAMETER);
    CHECK_EQ(kshim_memdisk_init(&a, "x", a_data, sizeof(a_data), 768, 0, true), ERROR_INVALID_PARAMETER);
    CHECK_EQ(kshim_memdisk_init(&a, "x", a_data, sizeof(a_data), 256, 0, true), ERROR_INVALID_PARAMETER);
    CHECK_EQ(kshim_memdisk_init(&a, "0123456789012345678901234567890123", a_data,
                                sizeof(a_data), 512, 0, true), ERROR_INVALID_PARAMETER);
    CHECK_EQ(kshim_memdisk_init(&a, "mem0", a_data, sizeof(a_data), 512, 0, true), 0);
    CHECK_EQ(kshim_memdisk_init(&b, "mem1", b_data, sizeof(b_data), 512, 0, false), 0);
    CHECK_EQ(kshim_memdisk_init(&dup, "mem0", b_data, sizeof(b_data), 512, 0, false), 0);
    CHECK(a.dev.kind == KSHIM_BLKDEV_MEMORY && a.dev.read_only && !a.dev.ops->write);
    CHECK(b.dev.ops->write != NULL);

    CHECK(kshim_blkdev_first() == NULL);
    CHECK_EQ(kshim_blkdev_register(NULL), ERROR_INVALID_PARAMETER);
    bad = a.dev;
    bad.block_size = 0;
    CHECK_EQ(kshim_blkdev_register(&bad), ERROR_INVALID_PARAMETER);
    bad = a.dev;
    bad.ops = NULL;
    CHECK_EQ(kshim_blkdev_register(&bad), ERROR_INVALID_PARAMETER);
    CHECK_EQ(kshim_blkdev_register(&a.dev), 0);
    CHECK_EQ(kshim_blkdev_register(&a.dev), KSHIM_ERR_EXISTS);
    CHECK_EQ(kshim_blkdev_register(&dup.dev), KSHIM_ERR_EXISTS);
    CHECK_EQ(kshim_blkdev_register(&b.dev), 0);
    CHECK(kshim_blkdev_first() == &a.dev && a.dev.next == &b.dev && !b.dev.next);
    CHECK(kshim_blkdev_find("mem1") == &b.dev && !kshim_blkdev_find("mem2"));

    /* Partition window: parent must be registered and the range inside it. */
    strcpy(child.name, "mem0:p1");
    child.kind = KSHIM_BLKDEV_PARTITION;
    child.block_size = 512;
    child.parent = &a.dev;
    child.start_lba = 6;
    child.num_blocks = 3;
    CHECK_EQ(kshim_blkdev_register(&child), ERROR_INVALID_PARAMETER);
    child.num_blocks = 2;
    child.block_size = 4096;
    CHECK_EQ(kshim_blkdev_register(&child), ERROR_INVALID_PARAMETER);
    child.block_size = 512;
    child.kind = KSHIM_BLKDEV_MEMORY;
    child.parent = NULL;
    CHECK_EQ(kshim_blkdev_register(&child), ERROR_INVALID_PARAMETER); /* start_lba without parent */
    child.kind = KSHIM_BLKDEV_PARTITION;
    child.parent = &a.dev;
    CHECK_EQ(kshim_blkdev_register(&child), 0);
    CHECK_EQ(kshim_blkdev_unregister(&a.dev), KSHIM_ERR_BUSY);
    {
        uint8_t block[512];

        for (size_t i = 0; i < sizeof(a_data); ++i)
            a_data[i] = (uint8_t)(i / 512);
        CHECK_EQ(kshim_blkdev_read(&child, 1, 1, block), 0);
        CHECK(block[0] == 7 && block[511] == 7);
        CHECK_EQ(kshim_blkdev_read(&child, 1, 2, block), ERROR_INVALID_PARAMETER);
    }
    CHECK_EQ(kshim_blkdev_unregister(&child), 0);
    CHECK_EQ(kshim_blkdev_unregister(&child), ERROR_NOT_FOUND);
    CHECK_EQ(kshim_blkdev_unregister(&a.dev), 0);
    CHECK_EQ(kshim_blkdev_unregister(&b.dev), 0);
    CHECK(kshim_blkdev_first() == NULL);
}

static void split_and_bounds(uint32_t bs)
{
    const size_t blocks = 64;
    uint8_t *data = malloc(blocks * bs), *buf = malloc(blocks * bs);
    struct kshim_memdisk disk;
    struct kshim_blkdev part = {0};

    fill(data, blocks * bs);
    CHECK_EQ(kshim_memdisk_init(&disk, "split", data, blocks * bs, bs, 3, true), 0);
    CHECK_EQ(kshim_blkdev_register(&disk.dev), 0);

    CHECK_EQ(kshim_blkdev_read(&disk.dev, 0, 0, NULL), 0);
    CHECK_EQ(kshim_blkdev_read(&disk.dev, 0, 1, NULL), ERROR_INVALID_PARAMETER);
    CHECK_EQ(kshim_blkdev_read(&disk.dev, blocks, 1, buf), ERROR_INVALID_PARAMETER);
    CHECK_EQ(kshim_blkdev_read(&disk.dev, blocks - 1, 2, buf), ERROR_INVALID_PARAMETER);
    CHECK_EQ(kshim_blkdev_read(&disk.dev, UINT64_MAX, 2, buf), ERROR_INVALID_PARAMETER);
    CHECK_EQ(kshim_blkdev_read(&disk.dev, blocks, 0, buf), 0);
    CHECK_EQ(disk.read_calls, 0);

    /* 10 blocks with a 3-block limit: 3+3+3+1 driver calls. */
    memset(buf, 0, blocks * bs);
    CHECK_EQ(kshim_blkdev_read(&disk.dev, 5, 10, buf), 0);
    CHECK_EQ(disk.read_calls, 4);
    CHECK_EQ(disk.largest_read, 3);
    CHECK(!memcmp(buf, data + 5 * bs, 10 * bs));

    /* Unlimited device: one call. */
    disk.dev.max_transfer_blocks = 0;
    disk.read_calls = 0;
    CHECK_EQ(kshim_blkdev_read(&disk.dev, 0, (uint32_t)blocks, buf), 0);
    CHECK_EQ(disk.read_calls, 1);
    disk.dev.max_transfer_blocks = 3;

    /* Driverless partition window splits by the parent's limit. */
    strcpy(part.name, "split:p1");
    part.kind = KSHIM_BLKDEV_PARTITION;
    part.block_size = bs;
    part.parent = &disk.dev;
    part.start_lba = 7;
    part.num_blocks = 20;
    CHECK_EQ(kshim_blkdev_register(&part), 0);
    disk.read_calls = disk.largest_read = 0;
    CHECK_EQ(kshim_blkdev_read(&part, 2, 7, buf), 0);
    CHECK(!memcmp(buf, data + 9 * bs, 7 * bs));
    CHECK_EQ(disk.read_calls, 3);
    CHECK_EQ(disk.largest_read, 3);
    CHECK_EQ(kshim_blkdev_read(&part, 15, 6, buf), ERROR_INVALID_PARAMETER);

    /* Unaligned byte reads: every head/tail combination, odd destinations,
     * and a misaligned middle bigger than the 64 KiB bounce limit. */
    {
        static const size_t lengths[] = {1, 2, 511, 512, 513, 4095, 4096, 4097, 9000, 70000, 140001};
        static const uint64_t offsets[] = {0, 1, 511, 512, 513, 4095, 4096, 4097, 12345};
        /* 64-byte aligned base: shift 0 takes the direct path, 1/63 bounce. */
        uint8_t *raw = aligned_alloc(64, blocks * bs + 128);

        for (size_t o = 0; o < sizeof(offsets) / sizeof(offsets[0]); ++o)
            for (size_t l = 0; l < sizeof(lengths) / sizeof(lengths[0]); ++l)
                for (size_t shift = 0; shift < 3; ++shift) {
                    uint64_t off = offsets[o];
                    size_t len = lengths[l];
                    uint8_t *dst = raw + (shift == 0 ? 0 : shift == 1 ? 1 : 63);

                    if (off + len > blocks * bs) {
                        CHECK_EQ(kshim_blkdev_read_bytes(&disk.dev, off, len, dst),
                                 ERROR_INVALID_PARAMETER);
                        continue;
                    }
                    memset(dst, 0xee, len);
                    CHECK_EQ(kshim_blkdev_read_bytes(&disk.dev, off, len, dst), 0);
                    CHECK(!memcmp(dst, data + off, len));
                }
        free(raw);
    }
    CHECK_EQ(kshim_blkdev_read_bytes(&disk.dev, 0, 0, NULL), 0);
    CHECK_EQ(kshim_blkdev_read_bytes(&disk.dev, blocks * bs, 1, buf), ERROR_INVALID_PARAMETER);
    CHECK_EQ(kshim_blkdev_read_bytes(&disk.dev, blocks * bs - 1, 2, buf), ERROR_INVALID_PARAMETER);
    CHECK_EQ(kshim_blkdev_read_bytes(&disk.dev, UINT64_MAX, 2, buf), ERROR_INVALID_PARAMETER);
    CHECK_EQ(kshim_blkdev_read_bytes(&part, (uint64_t)20 * bs - 3, 3, buf), 0);
    CHECK(!memcmp(buf, data + (7 + 20) * (uint64_t)bs - 3, 3));
    CHECK_EQ(kshim_blkdev_read_bytes(&part, (uint64_t)20 * bs - 3, 4, buf), ERROR_INVALID_PARAMETER);

    /* Driver errors propagate unchanged; negative ones become DEVICE_ERROR. */
    disk.fail_status = ERROR_TIMEOUT;
    disk.fail_lba = 9;
    disk.fail_count = 1;
    CHECK_EQ(kshim_blkdev_read(&disk.dev, 0, 8, buf), 0);
    CHECK_EQ(kshim_blkdev_read(&disk.dev, 0, 10, buf), ERROR_TIMEOUT);
    CHECK_EQ(kshim_blkdev_read_bytes(&part, (uint64_t)2 * bs + 5, 10, buf), ERROR_TIMEOUT);
    CHECK_EQ(kshim_blkdev_read_bytes(&disk.dev, 1, (size_t)20 * bs, buf + 1), ERROR_TIMEOUT);
    disk.fail_status = 0;
    {
        static const struct kshim_blkdev_ops neg = { .read = negative_read };
        const struct kshim_blkdev_ops *saved = disk.dev.ops;

        disk.dev.ops = &neg;
        CHECK_EQ(kshim_blkdev_read(&disk.dev, 0, 1, buf), ERROR_DEVICE_ERROR);
        disk.dev.ops = saved;
    }
    CHECK_EQ(kshim_blkdev_unregister(&part), 0);
    CHECK_EQ(kshim_blkdev_unregister(&disk.dev), 0);
    free(data);
    free(buf);
}

int main(void)
{
    registry();
    split_and_bounds(512);
    split_and_bounds(4096);
    return test_finish("blkdev_test");
}
