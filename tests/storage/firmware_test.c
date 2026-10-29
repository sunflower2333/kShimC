/* SPDX-License-Identifier: MIT */
/*
 * End to end: memory-backed UFS-like LUNs -> GPT (NhGptFind) -> liblp super
 * -> ext4/F2FS/FAT -> CrDK NhPolicyRead/NhBundleRead -> kshim_firmware_*.
 * Also the boot-slot authority (cmdline, bootconfig trailer, FDT).
 */
#include <libfdt.h>
#include <kshim/firmware.h>

#include "storage_test.h"

static const char *fixtures;
static struct image lun0_img, lun4_img;
static struct kshim_memdisk lun0, lun1, lun4, sd;
static uint8_t raw_lun[64 * 4096];

static void check_payload(const void *data, size_t size, const char *rel)
{
    size_t ref_size = 0;
    uint8_t *ref = host_file(fixtures, rel, &ref_size);

    CHECK(ref != NULL);
    if (ref) {
        CHECK_EQ(size, ref_size);
        CHECK(size == ref_size && !memcmp(data, ref, size));
    }
    free(ref);
}

static int request(const char *name, const struct kshim_fw_request *req, struct kshim_firmware *fw)
{
    int status = kshim_firmware_request(name, req, fw);

    printf("  request %-12s -> %s\n", name, kshim_storage_strerror(status));
    return status;
}

static void setup_storage(void)
{
    CHECK_EQ(image_map(&lun0_img, fixtures, "lun0.img"), 0);
    CHECK_EQ(image_map(&lun4_img, fixtures, "lun4.img"), 0);
    CHECK_EQ(kshim_memdisk_init(&lun0, "mem0:lu0", lun0_img.data, lun0_img.size, 4096, 32, true), 0);
    CHECK_EQ(kshim_memdisk_init(&lun1, "mem0:lu1", raw_lun, sizeof(raw_lun), 4096, 0, true), 0);
    CHECK_EQ(kshim_memdisk_init(&lun4, "mem0:lu4", lun4_img.data, lun4_img.size, 512, 64, true), 0);
    /* An SD card carrying the same partitions is never the firmware disk. */
    CHECK_EQ(kshim_memdisk_init(&sd, "sdhc1", lun4_img.data, lun4_img.size, 512, 0, true), 0);
    lun0.dev.kind = lun1.dev.kind = lun4.dev.kind = KSHIM_BLKDEV_UFS_LU;
    sd.dev.kind = KSHIM_BLKDEV_SD;
    CHECK_EQ(kshim_blkdev_register(&lun0.dev), 0);
    CHECK_EQ(kshim_blkdev_register(&lun1.dev), 0); /* raw boot LUN, no GPT */
    CHECK_EQ(kshim_blkdev_register(&lun4.dev), 0);
    CHECK_EQ(kshim_blkdev_register(&sd.dev), 0);
    /* Registry partitions coexist with the loader's raw GPT lookups. */
    CHECK_EQ(kshim_storage_init(NULL), 2 + 5 + 5);
}

static void pil_from_super(uint32_t slot, const char *payload)
{
    struct kshim_fw_request req = { .kind = KSHIM_FW_PIL };
    struct kshim_firmware fw;
    char rel[256];

    CHECK_EQ(kshim_bootslot_set(slot, KSHIM_BOOTSLOT_CMDLINE), 0);
    lun0.largest_read = lun4.largest_read = 0;
    CHECK_EQ(request("adsp.mdt", &req, &fw), 0);
    CHECK_EQ(fw.origin, KSHIM_FW_ORIGIN_ANDROID);
    CHECK_EQ(fw.location_index, 0);
    CHECK_EQ(fw.slot, slot);
    CHECK_EQ(fw.part_count, 2);
    CHECK(!strcmp(fw.name, "adsp.mdt"));
    snprintf(rel, sizeof(rel), "%s/firmware/adsp.mdt", payload);
    check_payload(fw.data, fw.size, rel);
    for (uint32_t i = 0; i < 6; ++i) {
        const void *data;
        size_t size;
        int status = kshim_firmware_part(&fw, i, &data, &size);

        if (i == 2 || i == 3) {
            CHECK_EQ(status, 0);
            snprintf(rel, sizeof(rel), "%s/firmware/adsp.b%02u", payload, (unsigned)i);
            if (!status)
                check_payload(data, size, rel);
        } else {
            CHECK_EQ(status, ERROR_NOT_FOUND); /* hash/header carried in the MDT */
        }
    }
    CHECK_EQ(kshim_firmware_part(&fw, 100, &(const void *){0}, &(size_t){0}), ERROR_INVALID_PARAMETER);
    kshim_firmware_release(&fw);
    CHECK(fw.priv == NULL);
    /* lun0 has a 32-block transfer limit; it must have been honoured. */
    CHECK(lun0.largest_read <= 32 && lun0.largest_read > 0);
}

static void files_and_order(void)
{
    struct kshim_fw_request req = { .kind = KSHIM_FW_FILE };
    struct kshim_firmware fw;
    uint8_t *buffer = malloc(12345);
    size_t size = 0;

    CHECK_EQ(kshim_bootslot_set(0, KSHIM_BOOTSLOT_CMDLINE), 0);
    /* Caller memory, exact size. */
    req.buffer = buffer;
    req.buffer_size = 12345;
    CHECK_EQ(request("wlan.bin", &req, &fw), 0);
    CHECK(fw.data == buffer && fw.size == 12345);
    check_payload(buffer, 12345, "payload/vendor_a/firmware/wlan.bin");
    kshim_firmware_release(&fw); /* must not free the caller's buffer */
    req.buffer_size = 12344;
    CHECK_EQ(request("wlan.bin", &req, &fw), KSHIM_ERR_TOO_LARGE);
    CHECK_EQ(kshim_firmware_load("wlan.bin", buffer, 12345, &size), 0);
    CHECK_EQ(size, 12345);
    CHECK_EQ(kshim_firmware_load("b_only.bin", buffer, 12345, &size), ERROR_NOT_FOUND);
    CHECK_EQ(kshim_bootslot_set(1, KSHIM_BOOTSLOT_CMDLINE), 0);
    CHECK_EQ(kshim_firmware_load("b_only.bin", buffer, 12345, &size), 0);
    CHECK(size == 14 && !memcmp(buffer, "vendor_b only\n", 14));
    CHECK_EQ(kshim_bootslot_set(0, KSHIM_BOOTSLOT_CMDLINE), 0);
    free(buffer);

    /* Absent from vendor: falls through to modem{slot}/image (index 2). */
    req = (struct kshim_fw_request){ .kind = KSHIM_FW_PIL };
    CHECK_EQ(request("modem.mdt", &req, &fw), 0);
    CHECK_EQ(fw.location_index, 2);
    CHECK_EQ(fw.part_count, 3);
    check_payload(fw.data, fw.size, "payload/modem_a/image/modem.mdt");
    {
        const void *data;
        size_t part_size;

        CHECK_EQ(kshim_firmware_part(&fw, 4, &data, &part_size), 0);
        CHECK_EQ(part_size, 3);
        check_payload(data, part_size, "payload/modem_a/image/modem.b04");
    }
    kshim_firmware_release(&fw);
    CHECK_EQ(kshim_bootslot_set(1, KSHIM_BOOTSLOT_CMDLINE), 0);
    CHECK_EQ(request("modem.mdt", &req, &fw), 0);
    check_payload(fw.data, fw.size, "payload/modem_b/image/modem.mdt");
    kshim_firmware_release(&fw);
    CHECK_EQ(kshim_bootslot_set(0, KSHIM_BOOTSLOT_CMDLINE), 0);

    /* Same-location alternate (MDT -> MBN). */
    req = (struct kshim_fw_request){ .kind = KSHIM_FW_FILE, .alternate = "venus.mbn" };
    CHECK_EQ(request("venus.mdt", &req, &fw), 0);
    CHECK(!strcmp(fw.name, "venus.mbn") && fw.location_index == 2);
    check_payload(fw.data, fw.size, "payload/modem_a/image/venus.mbn");
    kshim_firmware_release(&fw);

    /* Missing split member is corruption, even with a builtin available. */
    {
        static const uint8_t blob[4] = "CDSP";
        struct kshim_fw_builtin cdsp = { "cdsp.mdt", blob, sizeof(blob), NULL };

        CHECK_EQ(kshim_firmware_builtin_register(&cdsp), 0);
        req = (struct kshim_fw_request){ .kind = KSHIM_FW_PIL, .allow_builtin = true };
        CHECK_EQ(request("cdsp.mdt", &req, &fw), KSHIM_ERR_CORRUPT);
        kshim_firmware_builtin_clear();
    }

    /* Oversize is an error, not a reason to keep searching. */
    req = (struct kshim_fw_request){ .kind = KSHIM_FW_FILE, .max_size = 1024 * 1024 };
    CHECK_EQ(request("big.bin", &req, &fw), KSHIM_ERR_TOO_LARGE);
    req.max_size = 0;
    CHECK_EQ(request("big.bin", &req, &fw), 0);
    CHECK_EQ(fw.size, 2 * 1024 * 1024);
    kshim_firmware_release(&fw);

    /* Absent everywhere; builtin only after confirmed absence. */
    CHECK_EQ(request("nothing.bin", NULL, &fw), ERROR_NOT_FOUND);
    {
        static const char blob[] = "packaged";
        struct kshim_fw_builtin entry = { "nothing.bin", blob, sizeof(blob), NULL };
        struct kshim_fw_builtin dup = { "nothing.bin", blob, 1, NULL };
        uint8_t mem[16];

        CHECK_EQ(kshim_firmware_builtin_register(&entry), 0);
        CHECK_EQ(kshim_firmware_builtin_register(&dup), KSHIM_ERR_EXISTS);
        CHECK_EQ(request("nothing.bin", NULL, &fw), ERROR_NOT_FOUND); /* not allowed */
        req = (struct kshim_fw_request){ .kind = KSHIM_FW_FILE, .allow_builtin = true,
                                         .buffer = mem, .buffer_size = sizeof(mem) };
        CHECK_EQ(request("nothing.bin", &req, &fw), 0);
        CHECK(fw.origin == KSHIM_FW_ORIGIN_BUILTIN && fw.data == mem &&
              fw.size == sizeof(blob) && !memcmp(mem, blob, sizeof(blob)));
        CHECK_EQ(fw.slot, KSHIM_SLOT_UNKNOWN);
        kshim_firmware_release(&fw);
        /* Found on Android storage: builtin never consulted. */
        CHECK_EQ(request("wlan.bin", &req, &fw), KSHIM_ERR_TOO_LARGE);
        /* Package-only: an explicit empty location list. */
        req.locations = (const char *const[]){ NULL };
        req.location_count = 0;
        CHECK_EQ(kshim_bootslot_set(KSHIM_SLOT_UNKNOWN, 0), 0); /* no storage access */
        CHECK_EQ(request("nothing.bin", &req, &fw), 0);
        CHECK(fw.origin == KSHIM_FW_ORIGIN_BUILTIN);
        kshim_firmware_release(&fw);
        req.allow_builtin = false;
        CHECK_EQ(request("nothing.bin", &req, &fw), ERROR_INVALID_PARAMETER);
        CHECK_EQ(kshim_bootslot_set(0, KSHIM_BOOTSLOT_CMDLINE), 0);
        kshim_firmware_builtin_clear();
    }
}

static void custom_locations(void)
{
    static const char *const persist[] = { "persist||touch" };
    static const char *const opposite[] = { "modem_b||image" };
    static const char *const corrupt_first[] = { "garbage||", "modem{slot}||image" };
    static const char *const short_syntax[] = { "super|vendor_a" };
    static const char *const raw_name[] = { "modem{slot}||image/../x" };
    struct kshim_fw_request req = { .kind = KSHIM_FW_FILE };
    struct kshim_firmware fw;
    const char *many[KSHIM_FW_MAX_LOCATIONS + 1];

    /* No {slot}: works without any slot authority. */
    CHECK_EQ(kshim_bootslot_set(KSHIM_SLOT_UNKNOWN, 0), 0);
    req.locations = persist;
    req.location_count = 1;
    CHECK_EQ(request("cal.bin", &req, &fw), 0);
    check_payload(fw.data, fw.size, "payload/persist/touch/cal.bin");
    CHECK_EQ(fw.slot, KSHIM_SLOT_UNKNOWN);
    kshim_firmware_release(&fw);
    /* Default list needs the slot: NOT_READY, never fallback. */
    CHECK_EQ(request("wlan.bin", NULL, &fw), KSHIM_ERR_NOT_READY);

    CHECK_EQ(kshim_bootslot_set(0, KSHIM_BOOTSLOT_FDT), 0);
    req.locations = opposite; /* the other slot explicitly: refused */
    CHECK_EQ(request("modem.mdt", &req, &fw), ERROR_INVALID_PARAMETER);
    req.locations = corrupt_first; /* undecodable source stops the search */
    req.location_count = 2;
    CHECK(request("modem.mdt", &req, &fw) != 0);
    req.locations = short_syntax;
    req.location_count = 1;
    CHECK_EQ(request("x.bin", &req, &fw), ERROR_INVALID_PARAMETER);
    req.locations = raw_name;
    CHECK_EQ(request("x.bin", &req, &fw), ERROR_INVALID_PARAMETER);
    CHECK_EQ(request("../x.bin", NULL, &fw), ERROR_INVALID_PARAMETER);
    for (unsigned i = 0; i <= KSHIM_FW_MAX_LOCATIONS; ++i)
        many[i] = "persist||touch";
    req.locations = many;
    req.location_count = KSHIM_FW_MAX_LOCATIONS + 1;
    CHECK_EQ(request("cal.bin", &req, &fw), KSHIM_ERR_TOO_LARGE);
}

static void admission_and_binding(void)
{
    struct kshim_fw_builtin entry = { "adsp.mdt", "x", 1, NULL };
    struct kshim_fw_request req = { .kind = KSHIM_FW_FILE, .allow_builtin = true };
    struct kshim_firmware fw;
    /* misc is at LBA 256 of the 4 KiB LUN; message at misc + 32768. */
    uint8_t *merge = lun0_img.data + 256 * 4096 + 32768 + 5;
    struct kshim_memdisk other;
    static uint8_t other_data[64 * 512];

    CHECK_EQ(kshim_bootslot_set(0, KSHIM_BOOTSLOT_CMDLINE), 0);
    CHECK_EQ(kshim_firmware_builtin_register(&entry), 0);
    /* Virtual A/B merge in progress: no super reads, and no fallback. */
    *merge = 1;
    CHECK_EQ(request("adsp.mdt", &req, &fw), KSHIM_ERR_NOT_READY);
    CHECK_EQ(request("modem.mdt", &req, &fw), KSHIM_ERR_NOT_READY);
    *merge = 0;
    lun0_img.data[256 * 4096 + 32768] = 0; /* all-zero/unknown version */
    CHECK_EQ(request("adsp.mdt", &req, &fw), KSHIM_ERR_NOT_READY);
    lun0_img.data[256 * 4096 + 32768] = 2;
    CHECK_EQ(request("adsp.mdt", &req, &fw), 0);
    CHECK_EQ(fw.origin, KSHIM_FW_ORIGIN_ANDROID);
    kshim_firmware_release(&fw);

    /* I/O error inside the selected filesystem propagates. */
    lun0.fail_status = ERROR_TIMEOUT;
    lun0.fail_lba = 512 + 600;  /* inside super, past its metadata */
    lun0.fail_count = 20000;
    CHECK_EQ(request("adsp.mdt", &req, &fw), ERROR_TIMEOUT);
    lun0.fail_status = 0;

    /* A second internal device makes the binding ambiguous. */
    CHECK_EQ(kshim_memdisk_init(&other, "mem1", other_data, sizeof(other_data), 512, 0, true), 0);
    other.dev.kind = KSHIM_BLKDEV_EMMC;
    CHECK_EQ(kshim_blkdev_register(&other.dev), 0);
    CHECK_EQ(request("adsp.mdt", &req, &fw), KSHIM_ERR_CORRUPT);
    req.device = "mem0";
    CHECK_EQ(request("adsp.mdt", &req, &fw), 0);
    kshim_firmware_release(&fw);
    req.device = "mem1"; /* bound to a disk without GPT: not ready */
    CHECK_EQ(request("adsp.mdt", &req, &fw), KSHIM_ERR_NOT_READY);
    req.device = "sdhc1"; /* named explicitly, an SD card may be used */
    req.kind = KSHIM_FW_PIL;
    req.allow_builtin = false;
    CHECK_EQ(request("modem.mdt", &req, &fw), 0); /* no super there: modem_a */
    CHECK_EQ(fw.location_index, 2);
    kshim_firmware_release(&fw);
    req.kind = KSHIM_FW_FILE;
    req.device = NULL;
    CHECK_EQ(kshim_blkdev_unregister(&other.dev), 0);
    kshim_firmware_builtin_clear();
}

static void bootslot(void)
{
    struct kshim_bootslot_sources s = {0};
    uint32_t slot, evidence;
    static const char cmd_b[] = "console=ttyMSM0 androidboot.slot_suffix=_b quiet";
    static const char cmd_conflict[] = "androidboot.slot=a androidboot.slot_suffix=_b";
    static const char cmd_bootconfig[] = "console=ttyMSM0 bootconfig";
    static const char bc_flat[] = "androidboot.hardware = qcom\nandroidboot.slot_suffix = \"_a\"\n";
    static const char bc_nested[] = "# comment\nandroidboot {\n  serialno = 1234\n  slot = b\n}\n";
    static const char bc_array[] = "androidboot.slot_suffix = _a, _b\n";
    static const char bc_bad[] = "androidboot.slot_suffix = _c\n";

    CHECK_EQ(kshim_bootslot_parse(&s, &slot, &evidence), ERROR_NOT_FOUND);
    s.cmdline = cmd_b;
    s.cmdline_len = sizeof(cmd_b) - 1;
    CHECK_EQ(kshim_bootslot_parse(&s, &slot, &evidence), 0);
    CHECK(slot == 1 && evidence == KSHIM_BOOTSLOT_CMDLINE);
    s.fdt_slot_suffix = "_b";
    CHECK_EQ(kshim_bootslot_parse(&s, &slot, &evidence), 0);
    CHECK(slot == 1 && evidence == (KSHIM_BOOTSLOT_CMDLINE | KSHIM_BOOTSLOT_FDT));
    s.fdt_slot_suffix = "_a";
    CHECK_EQ(kshim_bootslot_parse(&s, &slot, &evidence), KSHIM_ERR_CORRUPT);
    CHECK_EQ(slot, KSHIM_SLOT_UNKNOWN);
    s.fdt_slot_suffix = "b"; /* slot_suffix needs the underscore */
    CHECK_EQ(kshim_bootslot_parse(&s, &slot, &evidence), ERROR_INVALID_PARAMETER);
    s = (struct kshim_bootslot_sources){ .cmdline = cmd_conflict, .cmdline_len = sizeof(cmd_conflict) - 1 };
    CHECK_EQ(kshim_bootslot_parse(&s, &slot, &evidence), KSHIM_ERR_CORRUPT);
    s = (struct kshim_bootslot_sources){ .cmdline = cmd_bootconfig, .cmdline_len = sizeof(cmd_bootconfig) - 1 };
    CHECK_EQ(kshim_bootslot_parse(&s, &slot, &evidence), KSHIM_ERR_NOT_READY);
    s.bootconfig = bc_flat;
    s.bootconfig_len = sizeof(bc_flat) - 1;
    CHECK_EQ(kshim_bootslot_parse(&s, &slot, &evidence), 0);
    CHECK(slot == 0 && evidence == KSHIM_BOOTSLOT_BOOTCONFIG);
    s.bootconfig = bc_nested;
    s.bootconfig_len = sizeof(bc_nested) - 1;
    CHECK_EQ(kshim_bootslot_parse(&s, &slot, &evidence), 0);
    CHECK_EQ(slot, 1);
    s.bootconfig = bc_array;
    s.bootconfig_len = sizeof(bc_array) - 1;
    CHECK_EQ(kshim_bootslot_parse(&s, &slot, &evidence), ERROR_INVALID_PARAMETER);
    s.bootconfig = bc_bad;
    s.bootconfig_len = sizeof(bc_bad) - 1;
    CHECK_EQ(kshim_bootslot_parse(&s, &slot, &evidence), ERROR_INVALID_PARAMETER);

    /* Bootconfig trailer on an initrd. */
    {
        uint8_t initrd[256] = "\x07\x07\x07 cpio...";
        size_t text_len = sizeof(bc_flat) - 1, at = 16, size;
        uint32_t sum = 0;
        const char *text;
        size_t length;

        memcpy(initrd + at, bc_flat, text_len);
        for (size_t i = 0; i < text_len; ++i)
            sum += (uint8_t)bc_flat[i];
        at += text_len;
        for (int i = 0; i < 4; ++i)
            initrd[at + i] = (uint8_t)(text_len >> (8 * i));
        for (int i = 0; i < 4; ++i)
            initrd[at + 4 + i] = (uint8_t)(sum >> (8 * i));
        memcpy(initrd + at + 8, "#BOOTCONFIG\n", 12);
        size = at + 20;
        CHECK_EQ(kshim_bootconfig_from_initrd(initrd, size, &text, &length), 0);
        CHECK(length == text_len && !memcmp(text, bc_flat, length));
        CHECK_EQ(kshim_bootconfig_from_initrd(initrd, size + 2, &text, &length), 0); /* padding */
        CHECK_EQ(kshim_bootconfig_from_initrd(initrd, size + 4, &text, &length), ERROR_NOT_FOUND);
        initrd[16] ^= 1;
        CHECK_EQ(kshim_bootconfig_from_initrd(initrd, size, &text, &length), ERROR_INVALID_PARAMETER);
        CHECK_EQ(kshim_bootconfig_from_initrd(initrd, 10, &text, &length), ERROR_NOT_FOUND);
    }

    /* FDT capture: /chosen/bootargs and /firmware/android. */
    {
        static uint8_t fdt[4096];
        int chosen, fw, android;

        CHECK_EQ(fdt_create_empty_tree(fdt, sizeof(fdt)), 0);
        chosen = fdt_add_subnode(fdt, 0, "chosen");
        fw = fdt_add_subnode(fdt, 0, "firmware");
        android = fdt_add_subnode(fdt, fw, "android");
        CHECK(chosen >= 0 && android >= 0);
        CHECK_EQ(fdt_setprop_string(fdt, chosen, "bootargs", cmd_b), 0);
        android = fdt_path_offset(fdt, "/firmware/android");
        CHECK_EQ(fdt_setprop_string(fdt, android, "slot_suffix", "_b"), 0);
        CHECK_EQ(kshim_bootslot_capture_fdt(fdt), 0);
        CHECK_EQ(kshim_bootslot_get(&slot), 0);
        CHECK_EQ(slot, 1);
        android = fdt_path_offset(fdt, "/firmware/android");
        CHECK_EQ(fdt_setprop_string(fdt, android, "slot", "a"), 0);
        CHECK_EQ(kshim_bootslot_capture_fdt(fdt), KSHIM_ERR_CORRUPT);
        CHECK_EQ(kshim_bootslot_get(&slot), KSHIM_ERR_NOT_READY); /* cleared */
        CHECK_EQ(kshim_bootslot_capture_fdt(NULL), KSHIM_ERR_NOT_READY);
    }
    CHECK_EQ(kshim_bootslot_set(2, 0), ERROR_INVALID_PARAMETER);
}

static void no_storage(void)
{
    struct kshim_firmware fw;

    CHECK_EQ(kshim_bootslot_set(0, KSHIM_BOOTSLOT_CMDLINE), 0);
    kshim_gpt_release(&lun0.dev);
    kshim_gpt_release(&lun4.dev);
    kshim_gpt_release(&sd.dev);
    CHECK_EQ(kshim_blkdev_unregister(&lun0.dev), 0);
    CHECK_EQ(kshim_blkdev_unregister(&lun1.dev), 0);
    CHECK_EQ(kshim_blkdev_unregister(&lun4.dev), 0);
    CHECK_EQ(kshim_blkdev_unregister(&sd.dev), 0);
    CHECK(kshim_blkdev_first() == NULL);
    CHECK_EQ(request("adsp.mdt", NULL, &fw), KSHIM_ERR_NOT_READY);
}

int main(int argc, char **argv)
{
    fixtures = fixture_dir(argc, argv);
    if (!fixtures) {
        printf("SKIP firmware_test: fixtures not generated\n");
        return SKIP_CODE;
    }
    bootslot();
    setup_storage();
    {
        struct kshim_firmware fw;

        CHECK_EQ(kshim_bootslot_set(KSHIM_SLOT_UNKNOWN, 0), 0);
        CHECK_EQ(request("adsp.mdt", NULL, &fw), KSHIM_ERR_NOT_READY);
    }
    pil_from_super(0, "payload/vendor_a"); /* ext4, two extents */
    pil_from_super(1, "payload/vendor_b"); /* F2FS */
    files_and_order();
    custom_locations();
    admission_and_binding();
    no_storage();
    image_unmap(&lun0_img);
    image_unmap(&lun4_img);
    return test_finish("firmware_test");
}
