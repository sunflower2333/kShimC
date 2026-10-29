/* SPDX-License-Identifier: MIT */
#pragma once
/*
 * Firmware loader.  Resolves firmware exactly the way CrDK's FirmwareLoaderLib
 * NhPolicyRead/NhBundleRead do: an ordered list of explicit Android locations
 * on the board's internal storage, current-boot A/B slot substitution, only
 * genuine absence advancing to the next location, all members of a PIL image
 * group taken from the one selected location, and an optional kShimC-packaged
 * (builtin) fallback that is consulted only after confirmed absence.  See
 * docs/firmware-loader.md for the search order.  Read-only; boot CPU only.
 */
#include <kshim/fs.h>

/* ---- Current Android boot slot (immutable bootloader handoff) ---------- */

#define KSHIM_SLOT_UNKNOWN UINT32_MAX
/* Evidence sources, bit values follow CrDK NH_BOOT_SLOT_* */
#define KSHIM_BOOTSLOT_CMDLINE 1U
#define KSHIM_BOOTSLOT_BOOTCONFIG 2U
#define KSHIM_BOOTSLOT_FDT 4U

struct kshim_bootslot_sources {
    const char *cmdline;          /* kernel command line, may be NULL */
    size_t cmdline_len;
    const char *bootconfig;       /* verified bootconfig text, may be NULL */
    size_t bootconfig_len;
    const char *fdt_slot_suffix;  /* /firmware/android "slot_suffix" ("_a") */
    const char *fdt_slot;         /* /firmware/android "slot" ("a") */
};

/*
 * Reconcile every supplied source like CrDK AndroidBootCr: exact
 * androidboot.slot_suffix=_a|_b or androidboot.slot=a|b (cmdline/bootconfig)
 * and slot_suffix/slot (FDT).  All present values must agree.  No disk
 * metadata (misc boot control, GPT attributes) is consulted.
 *   ERROR_NOT_FOUND          every source valid, none names a slot
 *   KSHIM_ERR_NOT_READY      cmdline requests bootconfig, none was supplied
 *                            and no other source names a slot
 *   ERROR_INVALID_PARAMETER  malformed slot value or bootconfig syntax
 *   KSHIM_ERR_CORRUPT        sources contradict each other
 */
int kshim_bootslot_parse(const struct kshim_bootslot_sources *sources,
                         uint32_t *slot, uint32_t *evidence);

/* Locate and checksum-verify an Android bootconfig trailer at the end of an
 * initrd image ("#BOOTCONFIG\n", up to 3 bytes of padding).  ERROR_NOT_FOUND
 * when there is no trailer. */
int kshim_bootconfig_from_initrd(const void *initrd, size_t size,
                                 const char **text, size_t *length);

/* Parse `sources` and, on success, record the current slot.  A failed capture
 * clears any previous slot. */
int kshim_bootslot_capture(const struct kshim_bootslot_sources *sources);
/* Capture from /chosen/bootargs and /firmware/android of a validated FDT.
 * The initrd is not mapped here, so bootconfig-only slots stay NOT_READY
 * unless a caller that mapped it uses kshim_bootslot_capture(). */
int kshim_bootslot_capture_fdt(const void *fdt);
/* Record an externally established slot (0/1), or KSHIM_SLOT_UNKNOWN to clear. */
int kshim_bootslot_set(uint32_t slot, uint32_t evidence);
/* KSHIM_ERR_NOT_READY when no current slot has been established. */
int kshim_bootslot_get(uint32_t *slot);

/* ---- Builtin (kShimC-packaged) fallback images ------------------------- */

struct kshim_fw_builtin {
    const char *path;   /* relative path, matched exactly ("adsp.mdt") */
    const void *data;
    size_t size;
    struct kshim_fw_builtin *next; /* owned by the loader */
};
/* Register a packaged image; the entry and data must stay valid. */
int kshim_firmware_builtin_register(struct kshim_fw_builtin *entry);
void kshim_firmware_builtin_clear(void);

/* ---- Requests ------------------------------------------------------------ */

enum kshim_fw_kind {
    KSHIM_FW_FILE = 0, /* one regular file */
    KSHIM_FW_PIL = 1,  /* ELF .mdt/.mbn header plus any split .bXX members */
};

#define KSHIM_FW_ORIGIN_ANDROID 1U
#define KSHIM_FW_ORIGIN_BUILTIN 2U
#define KSHIM_FW_MAX_LOCATIONS 32U
#define KSHIM_FW_MAX_PARTS 100U

/* CrDK's default PIL location list (Driver/PILCrDxe), in CrDK candidate
 * syntax "physical|logical-or-empty|directory": */
extern const char *const kshim_firmware_default_locations[];
extern const unsigned kshim_firmware_default_location_count;

struct kshim_fw_request {
    /* Ordered locations "physical|logical|dir"; NULL selects the defaults.
     * "{slot}" may end a partition name and expands to the current slot.
     * A non-NULL list with location_count 0 selects builtin-only (CrDK
     * package-only), which requires allow_builtin. */
    const char *const *locations;
    unsigned location_count;
    /* Optional same-location alternative name, tried only when the primary
     * is absent there (CrDK PIL: "adsp.mdt" then "adsp.mbn"). */
    const char *alternate;
    enum kshim_fw_kind kind;
    /* Per-file bound, 0 = KSHIM_FS_FILE_MAX.  Oversize is an error. */
    size_t max_size;
    /* Optional caller memory for KSHIM_FW_FILE; otherwise heap. */
    void *buffer;
    size_t buffer_size;
    /* Consult builtin images after confirmed absence everywhere. */
    bool allow_builtin;
    /* Physical device prefix ("ufs0"); NULL = the unique internal device. */
    const char *device;
};

struct kshim_firmware {
    const void *data;        /* file, or PIL header (.mdt/.mbn) */
    size_t size;
    uint32_t origin;         /* KSHIM_FW_ORIGIN_* */
    uint32_t location_index; /* index into the location (or builtin) list */
    uint32_t slot;           /* slot used, KSHIM_SLOT_UNKNOWN if none */
    uint32_t part_count;     /* PIL split members loaded (.bXX) */
    char name[KSHIM_FS_PATH_MAX]; /* path that was read, relative to source */
    void *priv;
};

/*
 * Load firmware `name` (a relative file name, e.g. "adsp.mdt").  With
 * req == NULL: KSHIM_FW_FILE, default locations, no alternate or builtin.
 *   ERROR_NOT_FOUND     absent from every location (and builtin if allowed)
 *   KSHIM_ERR_NOT_READY storage not registered, slot or Virtual A/B state
 *                       unavailable -- never permission to use a fallback
 *   KSHIM_ERR_CORRUPT   malformed metadata, missing PIL member, ambiguous
 *                       partition, non-regular file
 *   KSHIM_ERR_TOO_LARGE file above max_size / buffer_size
 * On success release the result with kshim_firmware_release().
 */
int kshim_firmware_request(const char *name, const struct kshim_fw_request *req,
                           struct kshim_firmware *fw);
/* PIL split member `index` (0..99, i.e. ".bXX"); ERROR_NOT_FOUND if the
 * member is carried inside the header or was not required. */
int kshim_firmware_part(const struct kshim_firmware *fw, uint32_t index,
                        const void **data, size_t *size);
void kshim_firmware_release(struct kshim_firmware *fw);

/* Convenience: whole file into caller memory, default locations. */
int kshim_firmware_load(const char *name, void *buffer, size_t cap, size_t *size);
