#include <kshim.h>
#include <pl011_uart.h>
#include <kshim_console.h>
#include <timer.h>
#include <gicv3.h>
#include <arm64/boot.h>
#include <manifest.h>
#include <platform.h>
#include <dt.h>
#include <qcom_dt.h>

#define PL011_UART_BASE 0x09000000ULL
#ifndef CONFIG_KSHIM_QEMU_PLATFORM
#define CONFIG_KSHIM_QEMU_PLATFORM 0
#endif

typedef struct {
    const MANIFEST *manifest;
    const MANIFEST_ENTRY *entries[MANIFEST_MAX_ENTRIES];
} MANIFEST_VIEW;

static bool is_executable_type(uint32_t type)
{
    return type == MF_ENTRY_TYPE_LINUX ||
        type == MF_ENTRY_TYPE_FREE_EXEC ||
        type == MF_ENTRY_TYPE_SHIM;
}

static bool is_flattened_device_tree(uint64_t address, uint64_t size)
{
    return address && size <= SIZE_MAX && dt_validate((const void *)(uintptr_t)address, (size_t)size) == 0;
}

static bool parse_manifest_entries(uint64_t manifest_address,
                                   uint64_t manifest_size,
                                   MANIFEST_VIEW *view)
{
    const MANIFEST *manifest = (const MANIFEST *)manifest_address;
    uint64_t cursor = sizeof(MANIFEST_HEADER);
    for (uint32_t index = 0; index < manifest->header.entry_count; index++) {
        if (cursor > manifest_size ||
            sizeof(MANIFEST_ENTRY) > manifest_size - cursor)
            return false;
        const MANIFEST_ENTRY *entry =
            (const MANIFEST_ENTRY *)(manifest_address + cursor);
        if (entry->name_len == 0)
            return false;
        uint64_t record_size = sizeof(MANIFEST_ENTRY) + entry->name_len;
        record_size = (record_size + MANIFEST_ENTRY_ALIGNMENT - 1) &
            ~(uint64_t)(MANIFEST_ENTRY_ALIGNMENT - 1);
        if (record_size > manifest_size - cursor)
            return false;
        for (uint32_t name_index = 0; name_index < entry->name_len;
             name_index++) {
            uint8_t character = (uint8_t)entry->name[name_index];
            if (character < 0x20 || character == 0x7f || character == ';')
                return false;
        }
        view->entries[index] = entry;
        cursor += record_size;
    }
    return cursor == manifest_size;
}

static bool get_manifest(uint64_t linux_base,
                         uint64_t manifest_address,
                         uint64_t manifest_size,
                         MANIFEST_VIEW *view)
{
    if (manifest_address < linux_base ||
        manifest_size < sizeof(MANIFEST_HEADER))
        return false;

    const MANIFEST *manifest = (const MANIFEST *)manifest_address;
    if (manifest->header.magic != MANIFEST_MAGIC ||
        manifest->header.version.major != MANIFEST_VERSION_MAJOR ||
        manifest->header.version.minor != MANIFEST_VERSION_MINOR ||
        manifest->header.entry_count < 2 ||
        manifest->header.entry_count > MANIFEST_MAX_ENTRIES ||
        manifest->header.default_entry >= manifest->header.entry_count)
        return false;

    uint64_t manifest_offset = manifest_address - linux_base;
    if (manifest_address > UINT64_MAX - manifest_size ||
        !parse_manifest_entries(manifest_address, manifest_size, view))
        return false;
    view->manifest = manifest;

    uint32_t attached_shim_index = manifest->header.entry_count - 1;
    const MANIFEST_ENTRY *attached_shim = view->entries[attached_shim_index];
    uint64_t attached_shim_offset =
        *(const uint64_t *)(linux_base + 0x30);
    if (manifest->header.default_entry == attached_shim_index ||
        attached_shim->type != MF_ENTRY_TYPE_SHIM ||
        attached_shim->flags != MF_ENTRY_FLAG_EXECUTABLE ||
        attached_shim->offset != attached_shim_offset ||
        attached_shim->size == 0 || attached_shim->entry_offset != 0)
        return false;

    const MANIFEST_ENTRY *base_entry = view->entries[0];
    if (base_entry->type != MF_ENTRY_TYPE_LINUX ||
        (base_entry->flags &
         (MF_ENTRY_FLAG_BASE_IMAGE | MF_ENTRY_FLAG_EXECUTABLE)) !=
            (MF_ENTRY_FLAG_BASE_IMAGE | MF_ENTRY_FLAG_EXECUTABLE) ||
        (base_entry->flags & ~(MF_ENTRY_FLAG_BASE_IMAGE |
                               MF_ENTRY_FLAG_EXECUTABLE |
                               MF_ENTRY_FLAG_HAS_DEVICE_TREE_BLOB)) !=
            0 ||
        base_entry->offset != 0)
        return false;

    uint64_t runtime_image_size = *(const uint64_t *)(linux_base + 0x10);
    if (linux_base > UINT64_MAX - runtime_image_size)
        return false;
    uint64_t packed_image_end = linux_base + runtime_image_size;

    for (uint32_t index = 0; index < manifest->header.entry_count; index++) {
        const MANIFEST_ENTRY *entry = view->entries[index];
        bool executable =
            (entry->flags & MF_ENTRY_FLAG_EXECUTABLE) != 0;
        if (executable != is_executable_type(entry->type))
            return false;
        bool copy_image = (entry->flags & MF_ENTRY_FLAG_COPY) != 0;
        bool has_dtb =
            (entry->flags & MF_ENTRY_FLAG_HAS_DEVICE_TREE_BLOB) != 0;
        uint32_t allowed_flags = index == 0
            ? MF_ENTRY_FLAG_BASE_IMAGE | MF_ENTRY_FLAG_EXECUTABLE |
              MF_ENTRY_FLAG_HAS_DEVICE_TREE_BLOB
            : MF_ENTRY_FLAG_COPY | MF_ENTRY_FLAG_EXECUTABLE |
              MF_ENTRY_FLAG_HAS_DEVICE_TREE_BLOB |
              MF_ENTRY_FLAG_HAS_QC_ABOOT_HEADER;
        if ((entry->flags & ~allowed_flags) != 0 ||
            entry->offset > manifest_offset ||
            entry->size > manifest_offset - entry->offset)
            return false;
        if (entry->alignment < MANIFEST_ALIGNMENT_SHIFT_MIN ||
            entry->alignment > MANIFEST_ALIGNMENT_SHIFT_MAX)
            return false;
        if ((entry->flags & MF_ENTRY_FLAG_HAS_QC_ABOOT_HEADER) &&
            entry->type != MF_ENTRY_TYPE_LINUX)
            return false;
        if (executable &&
            (entry->entry_offset > entry->size ||
             entry->size - entry->entry_offset < 4 ||
             (entry->entry_offset & 3) != 0))
            return false;
        if (!has_dtb && entry->device_tree_blob_entry != 0)
            return false;
        if (!executable &&
            (copy_image || entry->entry_offset != 0 || has_dtb))
            return false;
        if (copy_image) {
            if (!executable || entry->load_address == 0 ||
                (entry->load_address & 3) != 0 ||
                entry->copy_size_max == 0 ||
                entry->size > entry->copy_size_max)
                return false;
            if (entry->load_address > UINT64_MAX - entry->size)
                return false;
            uint64_t copy_end = entry->load_address + entry->size;
            if (entry->load_address < packed_image_end &&
                copy_end > linux_base)
                return false;
        } else if (entry->load_address != 0 || entry->copy_size_max != 0) {
            return false;
        }
        if (entry->type == MF_ENTRY_TYPE_DTB &&
            !is_flattened_device_tree(linux_base + entry->offset,
                                      entry->size)) {
            return false;
        }
        if (has_dtb) {
            if (!executable ||
                entry->device_tree_blob_entry >=
                    manifest->header.entry_count) {
                return false;
            }
            const MANIFEST_ENTRY *dtb =
                view->entries[entry->device_tree_blob_entry];
            if (dtb->type != MF_ENTRY_TYPE_DTB || dtb->flags != 0 ||
                !is_flattened_device_tree(linux_base + dtb->offset,
                                          dtb->size)) {
                return false;
            }
        }
    }

    if ((view->entries[manifest->header.default_entry]->flags &
            MF_ENTRY_FLAG_EXECUTABLE) == 0)
        return false;

    return true;
}

static void sync_image_cache(uint64_t address, uint64_t size)
{
    uint64_t ctr_el0 = 0;
    asm volatile("mrs %0, ctr_el0" : "=r"(ctr_el0));

    uint64_t data_line_size = 4ULL << ((ctr_el0 >> 16) & 0xf);
    uint64_t instruction_line_size = 4ULL << (ctr_el0 & 0xf);
    uint64_t end = address + size;

    for (uint64_t current = address & ~(data_line_size - 1);
         current < end; current += data_line_size)
        asm volatile("dc cvau, %0" : : "r"(current) : "memory");
    asm volatile("dsb ish" : : : "memory");

    for (uint64_t current = address & ~(instruction_line_size - 1);
         current < end; current += instruction_line_size)
        asm volatile("ic ivau, %0" : : "r"(current) : "memory");
    asm volatile("dsb ish\nisb" : : : "memory");
}

static void print_entry_name(const MANIFEST_ENTRY *entry)
{
    uint32_t remaining = entry->name_len;
    const char *current = entry->name;
    while (remaining != 0) {
        int chunk = remaining > 1024 ? 1024 : (int)remaining;
        kshim_console_write(current, chunk);
        current += chunk;
        remaining -= (uint32_t)chunk;
    }
}

static uint32_t select_image(const MANIFEST_VIEW *view, uint64_t fdt)
{
    uint32_t indexes[MANIFEST_MAX_ENTRIES];
    char labels[MANIFEST_MAX_ENTRIES][96];
    const char *names[MANIFEST_MAX_ENTRIES];
    size_t count = 0, initial = 0, selected = 0;
    for (uint32_t i = 0; i + 1 < view->manifest->header.entry_count; ++i) {
        const MANIFEST_ENTRY *entry = view->entries[i];
        if (!(entry->flags & MF_ENTRY_FLAG_EXECUTABLE)) continue;
        size_t n = entry->name_len < sizeof(labels[0]) ? entry->name_len : sizeof(labels[0]) - 1;
        memcpy(labels[count], entry->name, n); labels[count][n] = 0;
        indexes[count] = i; names[count] = labels[count];
        if (i == view->manifest->header.default_entry) initial = count;
        ++count;
    }
    if (kshim_platform_select(fdt, names, count, initial, view->manifest->header.timeout_ms, &selected)) {
        printf("Unsafe UI shutdown; refusing payload handoff\n");
        for (;;) (void)delay(100000);
    }
    return indexes[selected];
}

extern void _arm64_mmu_setup(void);

/* Last operation before leaving the runtime, after all boot diagnostics. */
static void prepare_boot(void)
{
    if (kshim_platform_prepare_handoff() || kshim_console_quiesce()) {
        printf("Device shutdown incomplete; refusing payload handoff\n");
        for (;;) (void)delay(100000);
    }
}

int kshim_main(
    uint64_t device_tree_address,
    uint64_t kernel_arg1,
    uint64_t kernel_arg2,
    uint64_t kernel_arg3,
    uint64_t linux_base,
    uint64_t manifest_address,
    uint64_t manifest_size)
{
    const void *runtime_fdt = (const void *)(uintptr_t)device_tree_address;
    if (!runtime_fdt || dt_validate(runtime_fdt, fdt_totalsize(runtime_fdt))) {
        runtime_fdt = NULL;
        device_tree_address = 0;
    }
    (void)kshim_console_init(runtime_fdt);
    if (runtime_fdt && kshim_resources_init(runtime_fdt, fdt_totalsize(runtime_fdt))) {
        printf("Invalid runtime device resources\n");
        for (;;) (void)delay(100000);
    }
#if CONFIG_ENABLE_BANNER
    printf(CONFIG_BANNER "\n");
#endif
#if KSHIM_STANDALONE
    return kshim_platform_main(device_tree_address, kernel_arg1, kernel_arg2, kernel_arg3);
#endif
    if (linux_base != 0) {
        MANIFEST_VIEW view = {0};
        if (!get_manifest(
                linux_base, manifest_address, manifest_size, &view)) {
            printf("Manifest not found, booting base image\n");
            prepare_boot();
            arm64_boot_linux(linux_base, device_tree_address, 4);
        }

        uint32_t selected_index = select_image(&view, device_tree_address);
        const MANIFEST_ENTRY *entry = view.entries[selected_index];
        uint64_t selected_device_tree = device_tree_address;
        if (entry->flags & MF_ENTRY_FLAG_HAS_DEVICE_TREE_BLOB) {
            const MANIFEST_ENTRY *dtb =
                view.entries[entry->device_tree_blob_entry];
            selected_device_tree = linux_base + dtb->offset;
        }
        if (entry->flags & MF_ENTRY_FLAG_COPY) {
            uint64_t source_address = linux_base + entry->offset;
            printf("Copying ");
            print_entry_name(entry);
            printf(" from 0x%lx to 0x%lx (0x%lx bytes)\n", source_address,
                   entry->load_address, entry->size);
            memmove((void *)entry->load_address, (const void *)source_address,
                    entry->size);
            sync_image_cache(entry->load_address, entry->size);
            prepare_boot();
            if (entry->type == MF_ENTRY_TYPE_LINUX) {
                arm64_boot_linux(entry->load_address, selected_device_tree,
                                 entry->entry_offset);
            }
            arm64_boot_image(entry->load_address + entry->entry_offset,
                             selected_device_tree, kernel_arg1, kernel_arg2,
                             kernel_arg3);
        }

        if (entry->type == MF_ENTRY_TYPE_LINUX) {
            uint64_t entry_offset = entry->flags & MF_ENTRY_FLAG_BASE_IMAGE
                ? 4 : entry->entry_offset;
            printf("Booting ");
            print_entry_name(entry);
            printf(" at 0x%lx with DTB at 0x%lx\n",
                   linux_base + entry->offset, selected_device_tree);
            prepare_boot();
            arm64_boot_linux(linux_base + entry->offset,
                             selected_device_tree, entry_offset);
        }
        prepare_boot();
        arm64_boot_image(linux_base + entry->offset + entry->entry_offset,
                         selected_device_tree, kernel_arg1, kernel_arg2,
                         kernel_arg3);
    }
    return kshim_platform_main(device_tree_address, kernel_arg1, kernel_arg2, kernel_arg3);
}
