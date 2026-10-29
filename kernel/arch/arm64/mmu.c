#include <kshim_mmu.h>

#include <config.h>
#include <dt.h>
#include <qcom_dt.h>
#include <kshim_console.h>
#include <libfdt.h>

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define MMU_PAGE_SHIFT 12U
#define MMU_PAGE_SIZE UINT64_C(0x1000)
#define MMU_TABLE_ENTRIES 512U
#define MMU_TABLE_COUNT 64U
#define MMU_MAX_REGIONS 128U
#define MMU_ADDRESS_MASK UINT64_C(0x0000fffffffff000)

#define DESC_INVALID UINT64_C(0)
#define DESC_BLOCK UINT64_C(1)
#define DESC_TABLE UINT64_C(3)
#define DESC_PAGE UINT64_C(3)
#define DESC_TYPE_MASK UINT64_C(3)
#define DESC_ATTR_INDEX(index) ((uint64_t)(index) << 2)
#define DESC_AP_RO (UINT64_C(1) << 7)
#define DESC_SH_OUTER (UINT64_C(2) << 8)
#define DESC_SH_INNER (UINT64_C(3) << 8)
#define DESC_AF (UINT64_C(1) << 10)
#define DESC_PXN (UINT64_C(1) << 53)
#define DESC_UXN (UINT64_C(1) << 54)

#define MAIR_NORMAL_WB UINT64_C(0xff)
#define MAIR_NORMAL_NC UINT64_C(0x44)
#define MAIR_DEVICE_nGnRnE UINT64_C(0x00)
#define MAIR_VALUE (MAIR_NORMAL_WB | (MAIR_NORMAL_NC << 8) | \
                    (MAIR_DEVICE_nGnRnE << 16))

#define SCTLR_M (UINT64_C(1) << 0)
#define SCTLR_C (UINT64_C(1) << 2)
#define SCTLR_SA (UINT64_C(1) << 3)
#define SCTLR_I (UINT64_C(1) << 12)

#define CURRENT_EL1 UINT64_C(4)
#define CURRENT_EL2 UINT64_C(8)

#ifndef CONFIG_KSHIM_FB_RENDER_ADDRESS
#define CONFIG_KSHIM_FB_RENDER_ADDRESS 0U
#endif
#ifndef CONFIG_KSHIM_FB_WIDTH
#define CONFIG_KSHIM_FB_WIDTH 0U
#endif
#ifndef CONFIG_KSHIM_FB_HEIGHT
#define CONFIG_KSHIM_FB_HEIGHT 0U
#endif
#ifndef CONFIG_KSHIM_FB_STRIDE
#define CONFIG_KSHIM_FB_STRIDE 0U
#endif
#ifndef CONFIG_KSHIM_FB_FORMAT
#define CONFIG_KSHIM_FB_FORMAT 0U
#endif
#ifndef CONFIG_KSHIM_UEFI_LOAD_ADDRESS
#define CONFIG_KSHIM_UEFI_LOAD_ADDRESS 0U
#endif
#ifndef CONFIG_KSHIM_UEFI_MAX_SIZE
#define CONFIG_KSHIM_UEFI_MAX_SIZE 0U
#endif
#ifndef CONFIG_KSHIM_QEMU_PLATFORM
#define CONFIG_KSHIM_QEMU_PLATFORM 0
#endif
#ifndef CONFIG_KSHIM_SMP_SELFTEST
#define CONFIG_KSHIM_SMP_SELFTEST 0
#endif

typedef uint64_t translation_table_t[MMU_TABLE_ENTRIES];

typedef struct {
    translation_table_t *root;
    size_t tables_used;
    kshim_mmu_region_t regions[MMU_MAX_REGIONS];
    size_t region_count;
    uint64_t entry_el;
    uint8_t configured;
} mmu_context_t;

static translation_table_t mTables[MMU_TABLE_COUNT]
    __attribute__((aligned(MMU_PAGE_SIZE), section(".mmu_tables")));
static mmu_context_t mMmu;

extern unsigned char __image_start_address__[];
extern unsigned char __image_end_address__[];

static uint64_t align_down(uint64_t value, uint64_t alignment)
{
    return value & ~(alignment - 1U);
}

static int align_up(uint64_t value, uint64_t alignment, uint64_t *result)
{
    if (value > UINT64_MAX - (alignment - 1U))
        return -1;
    *result = (value + alignment - 1U) & ~(alignment - 1U);
    return 0;
}

static uint64_t block_size(unsigned level)
{
    return UINT64_C(1) << (MMU_PAGE_SHIFT + 9U * (3U - level));
}

static unsigned table_index(uint64_t address, unsigned level)
{
    return (unsigned)((address >> (MMU_PAGE_SHIFT + 9U * (3U - level))) &
                      UINT64_C(0x1ff));
}

static translation_table_t *allocate_table(void)
{
    translation_table_t *table;

    if (mMmu.tables_used >= MMU_TABLE_COUNT)
        return NULL;
    table = &mTables[mMmu.tables_used++];
    memset(table, 0, sizeof(*table));
    return table;
}

static translation_table_t *next_table(uint64_t *entry, unsigned level)
{
    uint64_t descriptor = *entry;
    translation_table_t *table;

    if ((descriptor & DESC_TYPE_MASK) == DESC_TABLE)
        return (translation_table_t *)(uintptr_t)(descriptor & MMU_ADDRESS_MASK);

    table = allocate_table();
    if (table == NULL)
        return NULL;

    if ((descriptor & DESC_TYPE_MASK) == DESC_BLOCK) {
        uint64_t base = descriptor & MMU_ADDRESS_MASK;
        uint64_t attributes = descriptor &
            ~(MMU_ADDRESS_MASK | DESC_TYPE_MASK);
        uint64_t size = block_size(level + 1U);
        uint64_t type = level + 1U == 3U ? DESC_PAGE : DESC_BLOCK;

        for (unsigned index = 0; index < MMU_TABLE_ENTRIES; index++)
            (*table)[index] = (base + (uint64_t)index * size) |
                              attributes | type;
    }

    *entry = ((uint64_t)(uintptr_t)table & MMU_ADDRESS_MASK) | DESC_TABLE;
    return table;
}

static uint64_t descriptor_attributes(
    kshim_mmu_memory_type_t type, uint32_t permissions)
{
    uint64_t attributes = DESC_AF;

    if (type == KSHIM_MMU_NORMAL_WB)
        attributes |= DESC_ATTR_INDEX(0U) | DESC_SH_INNER;
    else if (type == KSHIM_MMU_NORMAL_NC)
        attributes |= DESC_ATTR_INDEX(1U) | DESC_SH_OUTER;
    else
        attributes |= DESC_ATTR_INDEX(2U) | DESC_SH_OUTER;

    if ((permissions & KSHIM_MMU_WRITE) == 0U)
        attributes |= DESC_AP_RO;
    if ((permissions & KSHIM_MMU_EXECUTE) == 0U)
        attributes |= DESC_PXN | DESC_UXN;
    return attributes;
}

static int map_range(
    uint64_t base, uint64_t size, kshim_mmu_memory_type_t type,
    uint32_t permissions)
{
    uint64_t end;
    uint64_t address;
    uint64_t attributes;

    if (size == 0U || base > UINT64_MAX - size)
        return -1;
    address = align_down(base, MMU_PAGE_SIZE);
    if (align_up(base + size, MMU_PAGE_SIZE, &end) != 0 ||
        end <= address || end > (UINT64_C(1) << 48))
        return -1;
    attributes = descriptor_attributes(type, permissions);

    while (address < end) {
        translation_table_t *table = mMmu.root;
        uint64_t remaining = end - address;

        for (unsigned level = 0; level <= 3U; level++) {
            uint64_t span = block_size(level);
            uint64_t *entry = &(*table)[table_index(address, level)];
            uint64_t entry_type = *entry & DESC_TYPE_MASK;

            if (level >= 1U && (entry_type != DESC_TABLE || level == 3U) &&
                (address & (span - 1U)) == 0U && remaining >= span) {
                uint64_t descriptor_type = level == 3U ? DESC_PAGE : DESC_BLOCK;
                *entry = (address & MMU_ADDRESS_MASK) |
                         attributes | descriptor_type;
                address += span;
                break;
            }
            if (level == 3U)
                return -1;
            table = next_table(entry, level);
            if (table == NULL)
                return -2;
        }
    }
    return 0;
}

static int add_region(const kshim_mmu_region_t *region)
{
    int status;

    if (region == NULL || region->size == 0U)
        return 0;
    if (region->type > KSHIM_MMU_DEVICE ||
        (region->permissions & KSHIM_MMU_READ) == 0U ||
        mMmu.region_count >= MMU_MAX_REGIONS)
        return -1;
    uint64_t end;
    if (region->base > UINT64_MAX - region->size ||
        align_up(region->base + region->size, MMU_PAGE_SIZE, &end)) return -1;
    uint64_t start = align_down(region->base, MMU_PAGE_SIZE);
    for (size_t i = 0; i < mMmu.region_count; ++i) {
        const kshim_mmu_region_t *old = &mMmu.regions[i];
        uint64_t old_end;
        if (align_up(old->base + old->size, MMU_PAGE_SIZE, &old_end)) return -1;
        if (start < old_end && align_down(old->base, MMU_PAGE_SIZE) < end &&
            (old->type != region->type || old->permissions != region->permissions)) return -1;
    }
    status = map_range(
        region->base, region->size, region->type, region->permissions);
    if (status != 0)
        return status;
    mMmu.regions[mMmu.region_count++] = *region;
    return 0;
}

static int range_within(
    uint64_t base, uint64_t size, uint64_t outer_base, uint64_t outer_size)
{
    return size <= outer_size && base >= outer_base &&
        base - outer_base <= outer_size - size;
}

static int ranges_overlap(
    uint64_t first_base, uint64_t first_size,
    uint64_t second_base, uint64_t second_size)
{
    if (first_size == 0U || second_size == 0U ||
        first_base > UINT64_MAX - first_size ||
        second_base > UINT64_MAX - second_size)
        return 0;
    return first_base < second_base + second_size &&
        second_base < first_base + first_size;
}

/* Read a big-endian FDT cell tuple without consulting UEFI's memory map. */
static uint64_t current_el(void)
{
#if defined(__aarch64__)
    uint64_t value;
    __asm__ volatile("mrs %0, CurrentEL" : "=r"(value));
    return value & UINT64_C(0xc);
#else
    return CURRENT_EL1;
#endif
}

#if defined(__aarch64__)
static uint64_t physical_size_encoding(void)
{
#if defined(__aarch64__)
    uint64_t mmfr0;
    __asm__ volatile("mrs %0, id_aa64mmfr0_el1" : "=r"(mmfr0));
    mmfr0 &= UINT64_C(0xf);
    return mmfr0 > 5U ? 5U : mmfr0;
#else
    return 5U;
#endif
}

#endif

static int enable_at_el(uint64_t exception_level)
{
#if defined(__aarch64__)
    uint64_t root;
    uint64_t tcr;
    uint64_t sctlr;
    uint64_t ps = physical_size_encoding();

    if (!mMmu.configured ||
        (exception_level != CURRENT_EL1 && exception_level != CURRENT_EL2))
        return -1;
    root = (uint64_t)(uintptr_t)mMmu.root;
    __asm__ volatile("dsb ishst" ::: "memory");

    if (exception_level == CURRENT_EL1) {
        /* 48-bit TTBR0, WB table walks, inner-shareable, TTBR1 disabled. */
        tcr = UINT64_C(16) | (UINT64_C(1) << 8) |
              (UINT64_C(1) << 10) | (UINT64_C(3) << 12) |
              (UINT64_C(1) << 23) | (UINT64_C(16) << 16) |
              (UINT64_C(2) << 30) | (ps << 32);
        __asm__ volatile(
            "tlbi vmalle1\n"
            "dsb ish\n"
            "isb\n"
            "msr mair_el1, %0\n"
            "msr tcr_el1, %1\n"
            "msr ttbr0_el1, %2\n"
            "isb"
            : : "r"(MAIR_VALUE), "r"(tcr), "r"(root) : "memory");
        __asm__ volatile("mrs %0, sctlr_el1" : "=r"(sctlr));
        sctlr |= SCTLR_M | SCTLR_C | SCTLR_I | SCTLR_SA;
        __asm__ volatile("msr sctlr_el1, %0\nisb" : : "r"(sctlr) : "memory");
    } else {
        uint64_t hcr;

        /* Use the non-VHE EL2 translation regime and keep execution at EL2. */
        __asm__ volatile("mrs %0, hcr_el2" : "=r"(hcr));
        hcr &= ~((UINT64_C(1) << 34) | (UINT64_C(1) << 27));
        __asm__ volatile("msr hcr_el2, %0\nisb" : : "r"(hcr) : "memory");
        tcr = UINT64_C(16) | (UINT64_C(1) << 8) |
              (UINT64_C(1) << 10) | (UINT64_C(3) << 12) |
              (ps << 16) | (UINT64_C(1) << 23) |
              (UINT64_C(1) << 31);
        __asm__ volatile(
            "tlbi alle2\n"
            "dsb ish\n"
            "isb\n"
            "msr mair_el2, %0\n"
            "msr tcr_el2, %1\n"
            "msr ttbr0_el2, %2\n"
            "isb"
            : : "r"(MAIR_VALUE), "r"(tcr), "r"(root) : "memory");
        __asm__ volatile("mrs %0, sctlr_el2" : "=r"(sctlr));
        sctlr |= SCTLR_M | SCTLR_C | SCTLR_I | SCTLR_SA;
        __asm__ volatile("msr sctlr_el2, %0\nisb" : : "r"(sctlr) : "memory");
    }
    return 0;
#else
    (void)exception_level;
    return mMmu.configured ? 0 : -1;
#endif
}

static void clean_range(uint64_t base, uint64_t size)
{
#if defined(__aarch64__)
    uint64_t ctr;
    uint64_t line_size;
    uint64_t end;

    if (size == 0U || base > UINT64_MAX - size)
        return;
    __asm__ volatile("mrs %0, ctr_el0" : "=r"(ctr));
    line_size = UINT64_C(4) << ((ctr >> 16) & UINT64_C(0xf));
    end = base + size;
    for (uint64_t address = align_down(base, line_size);
         address < end; address += line_size)
        __asm__ volatile("dc civac, %0" : : "r"(address) : "memory");
#else
    (void)base;
    (void)size;
#endif
}

static void disable_at_el(uint64_t exception_level)
{
#if defined(__aarch64__)
    uint64_t sctlr;

    __asm__ volatile("dsb sy" ::: "memory");
    if (exception_level == CURRENT_EL1) {
        __asm__ volatile("mrs %0, sctlr_el1" : "=r"(sctlr));
        sctlr &= ~(SCTLR_M | SCTLR_C | SCTLR_I);
        __asm__ volatile(
            "msr sctlr_el1, %0\n"
            "isb\n"
            "tlbi vmalle1\n"
            "dsb sy\n"
            "isb"
            : : "r"(sctlr) : "memory");
    } else if (exception_level == CURRENT_EL2) {
        __asm__ volatile("mrs %0, sctlr_el2" : "=r"(sctlr));
        sctlr &= ~(SCTLR_M | SCTLR_C | SCTLR_I);
        __asm__ volatile(
            "msr sctlr_el2, %0\n"
            "isb\n"
            "tlbi alle2\n"
            "dsb sy\n"
            "isb"
            : : "r"(sctlr) : "memory");
    }
#else
    (void)exception_level;
#endif
}

int kshim_mmu_configure(const kshim_mmu_config_t *config)
{
    kshim_mmu_region_t region;
    uint64_t image_base = (uint64_t)(uintptr_t)__image_start_address__;
    uint64_t image_end = (uint64_t)(uintptr_t)__image_end_address__;

    if (config == NULL || config->extra_region_count > KSHIM_MMU_MAX_EXTRA_REGIONS ||
        (config->extra_region_count != 0U && config->extra_regions == NULL) ||
        image_end <= image_base)
        return -1;

    memset(&mMmu, 0, sizeof(mMmu));
    memset(mTables, 0, sizeof(mTables));
    mMmu.root = allocate_table();
    if (mMmu.root == NULL)
        return -2;

    /* Map only memory owned by this runtime and explicit device resources.
     * Reservation entries and no-map pools never become an implicit RAM map. */
    if (config->fdt) {
        struct dt_range reservations[128]; bool no_map[128]; size_t count;
        if (dt_validate(config->fdt, fdt_totalsize(config->fdt)) ||
            dt_reservations(config->fdt, reservations, no_map, 128, &count)) return -1;
        for (size_t i = 0; i < count; ++i) {
            for (size_t j = 0; j < config->extra_region_count; ++j) {
                const kshim_mmu_region_t *extra = &config->extra_regions[j];
                if (no_map[i] && extra->type == KSHIM_MMU_NORMAL_WB &&
                    ranges_overlap(reservations[i].base, reservations[i].size, extra->base, extra->size)) return -1;
            }
        }
    }

    region = (kshim_mmu_region_t){
        .base = image_base,
        .size = image_end - image_base,
        .type = KSHIM_MMU_NORMAL_WB,
        .permissions = KSHIM_MMU_READ | KSHIM_MMU_WRITE | KSHIM_MMU_EXECUTE,
    };
    if (add_region(&region) != 0)
        return -2;

    if (config->fdt != NULL && fdt_check_header(config->fdt) == 0) {
        uint64_t fdt_base = (uint64_t)(uintptr_t)config->fdt;
        uint64_t fdt_size = (uint32_t)fdt_totalsize(config->fdt);

        if (!range_within(
                fdt_base, fdt_size, image_base, image_end - image_base)) {
            region = (kshim_mmu_region_t){
                .base = fdt_base,
                .size = fdt_size,
                .type = KSHIM_MMU_NORMAL_WB,
                .permissions = KSHIM_MMU_READ,
            };
            if (add_region(&region) != 0)
                return -2;
        }
    }

    region = (kshim_mmu_region_t){
        .base = config->framebuffer_base,
        .size = config->framebuffer_size,
        .type = KSHIM_MMU_NORMAL_NC,
        .permissions = KSHIM_MMU_READ | KSHIM_MMU_WRITE,
    };
    if (add_region(&region) != 0)
        return -2;

    region = (kshim_mmu_region_t){
        .base = config->uefi_base,
        .size = config->uefi_size,
        .type = KSHIM_MMU_NORMAL_WB,
        .permissions = KSHIM_MMU_READ | KSHIM_MMU_WRITE | KSHIM_MMU_EXECUTE,
    };
    if (add_region(&region) != 0)
        return -2;

    for (size_t index = 0; index < config->extra_region_count; index++) {
        if (add_region(&config->extra_regions[index]) != 0)
            return -2;
    }

    if (config->map_qemu_test_windows) {
        const kshim_mmu_region_t qemu_regions[] = {
            {UINT64_C(0x08000000), UINT64_C(0x00010000), KSHIM_MMU_DEVICE,
             KSHIM_MMU_READ | KSHIM_MMU_WRITE},
            {UINT64_C(0x080a0000), UINT64_C(0x00100000), KSHIM_MMU_DEVICE,
             KSHIM_MMU_READ | KSHIM_MMU_WRITE},
            {UINT64_C(0x44000000), UINT64_C(0x00c00000), KSHIM_MMU_NORMAL_NC,
             KSHIM_MMU_READ | KSHIM_MMU_WRITE},
            {UINT64_C(0x46000000), MMU_PAGE_SIZE, KSHIM_MMU_NORMAL_WB,
             KSHIM_MMU_READ | KSHIM_MMU_WRITE},
        };

        for (size_t index = 0;
             index < sizeof(qemu_regions) / sizeof(qemu_regions[0]); index++) {
            /* The configured scanout mapping supersedes the broad QEMU
             * fallback window. Mapping both creates conflicting leaf entries
             * and prevents SMP initialization in the UI profile. */
            if (index == 2U && ranges_overlap(
                    config->framebuffer_base, config->framebuffer_size,
                    qemu_regions[index].base, qemu_regions[index].size))
                continue;
            if (add_region(&qemu_regions[index]) != 0)
                return -2;
        }
    }

    mMmu.entry_el = current_el();
    mMmu.configured = 1U;
#if defined(__aarch64__)
    clean_range((uint64_t)(uintptr_t)&mMmu, sizeof(mMmu));
    clean_range((uint64_t)(uintptr_t)mTables, sizeof(mTables));
    __asm__ volatile("dsb sy" ::: "memory");
#endif
    return 0;
}

int kshim_mmu_configure_default(const void *fdt)
{
    struct kshim_resources *r = kshim_resources_get();
    kshim_mmu_region_t extras[KSHIM_MMU_MAX_EXTRA_REGIONS];
    size_t count = r->mapping_count;
    if (count + 1 > KSHIM_MMU_MAX_EXTRA_REGIONS) return -1;
    memcpy(extras, r->mappings, count * sizeof(*extras));
    if (kshim_console_base()) extras[count++] = (kshim_mmu_region_t){
        kshim_console_base(), kshim_console_size(), KSHIM_MMU_DEVICE, KSHIM_MMU_READ | KSHIM_MMU_WRITE};
    kshim_mmu_config_t config = {
        .fdt = fdt,
        .framebuffer_base = r->framebuffer.render_address,
        .framebuffer_size = r->framebuffer.buffer_size ? r->framebuffer.buffer_size :
            (uint64_t)(r->framebuffer.stride ? r->framebuffer.stride :
                r->framebuffer.width * r->framebuffer.bpp / 8) * r->framebuffer.height,
        .uefi_base = CONFIG_KSHIM_UEFI_LOAD_ADDRESS, .uefi_size = CONFIG_KSHIM_UEFI_MAX_SIZE,
        .extra_regions = extras, .extra_region_count = count,
        .map_qemu_test_windows = CONFIG_KSHIM_QEMU_PLATFORM || CONFIG_KSHIM_SMP_SELFTEST,
    };
    return kshim_mmu_configure(&config);
}

int kshim_mmu_enable_current_cpu(void)
{
    return enable_at_el(current_el());
}

int kshim_mmu_enable_secondary(uint64_t entry_el)
{
    if (entry_el != mMmu.entry_el)
        return -1;
    return enable_at_el(entry_el);
}

int kshim_mmu_is_enabled(void)
{
#if defined(__aarch64__)
    uint64_t value;
    uint64_t exception_level = current_el();

    if (exception_level == CURRENT_EL1)
        __asm__ volatile("mrs %0, sctlr_el1" : "=r"(value));
    else if (exception_level == CURRENT_EL2)
        __asm__ volatile("mrs %0, sctlr_el2" : "=r"(value));
    else
        return 0;
    return (value & SCTLR_M) != 0U;
#else
    return mMmu.configured != 0U;
#endif
}

uint64_t kshim_mmu_entry_el(void)
{
    return mMmu.entry_el;
}

uint64_t kshim_mmu_root_table(void)
{
    return (uint64_t)(uintptr_t)mMmu.root;
}

int kshim_mmu_prepare_handoff(void)
{
    if (!mMmu.configured)
        return 0;
    for (size_t index = 0; index < mMmu.region_count; index++) {
        if (mMmu.regions[index].type == KSHIM_MMU_NORMAL_WB &&
            (mMmu.regions[index].permissions & KSHIM_MMU_WRITE))
            clean_range(mMmu.regions[index].base, mMmu.regions[index].size);
    }
#if defined(__aarch64__)
    __asm__ volatile("dsb sy\nic iallu\ndsb sy\nisb" ::: "memory");
#endif
    disable_at_el(current_el());
    return 0;
}

void kshim_mmu_cpu_off_prepare(void)
{
    (void)kshim_mmu_prepare_handoff();
}

void _arm64_mmu_setup(void)
{
    if (!mMmu.configured && kshim_mmu_configure_default(NULL) != 0)
        return;
    (void)kshim_mmu_enable_current_cpu();
}
