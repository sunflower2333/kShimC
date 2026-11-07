/* SPDX-License-Identifier: MIT */
/*
 * Regulators from DT supplies (kshim/regulator.h): regulator-fixed with a
 * GPIO enable, always-on fixed rails, and RPMh VRM/XOB rails.
 *
 * Fixed rails: enable level from "enable-active-high" (low otherwise, the
 * regulator-fixed binding), "startup-delay-us", "off-on-delay-us",
 * "vin-supply" and pinctrl "default". A GPIO already at its enable level is
 * inherited-on and never written. kShimC's own enable is undone by the
 * last disable, and kshim_gpio_restore_all() puts the pad back at handoff.
 *
 * RPMh rails: the CMD DB address of the resource ("ldob14": mainline
 * "ldo"/"smp"/"bob" + qcom,pmic-id + index, as qcom-rpmh-regulator names
 * them; Android qcom,resource-name), then VRM writes at +0 voltage (mV),
 * +4 enable, +8 mode through CrDK RpmhLib's polled TCS API on the apps DRV.
 * Mode is voted for a mainline regulator-initial-mode, or for a consumer's
 * load hint, mapped to the PMIC5 hardware modes (qcom-rpmh-regulator facts:
 * LDO RET 3, LPM 4, HPM 7; SMPS RET 3, PFM 4, AUTO 6, PWM 7; BOB PFM 4,
 * AUTO 6, PWM 7; PMIC5 LDO hpm_min_load_uA 10000). Load facts: Android
 * rpmh-regulator nodes give qcom,regulator-type ("pmic5-ldo", "pmic5-hfsmps",
 * "pmic5-ftsmps", "pmic5-bob"), qcom,supported-modes (dt-bindings
 * RPMH_REGULATOR_MODE_RET 0, LPM 1, AUTO 2, HPM 3) and
 * qcom,mode-threshold-currents (minimum load in uA per listed mode; kona
 * ldoa13 <0 10000>); mainline nodes opt in with regulator-allow-set-load.
 *
 * Inherited state: before the first vote the apps DRV's own request for
 * the three registers is read back with an RPMh read command on a borrowed
 * idle ACTIVE TCS (claimed, snapshot and restored by CrRpmhPollClaim /
 * CrRpmhPollRestore; read MSGID = length 8 + response request, no write
 * bit; response in CMD0 RESP_DATA at +0x10, the rpmh-rsc register layout
 * CrDK uses). A rail observed enabled at a voltage inside its DT range is
 * inherited-on and not written. Every register kShimC changed goes back to
 * its observed value when the rail is released or at handoff; without an
 * observation the vote is kept and logged (CrDK's documented policy). CrDK
 * RpmhRailObserveLib is not usable here: it needs the interrupt-driven
 * RpmhLib core, which this port does not register.
 */
#include <kshim/regulator.h>
#include <kshim/gpio.h>
#include <kshim/io.h>
#include <kshim/pinctrl.h>
#include "../gpio/internal.h"

#include <Library/cr_resources.h>
#include <qcom_dt.h>
#include <stdio.h>
#include <string.h>

#define REG_POOL 16U
#define REG_NAME 24U
#define REG_MODES 4U
#define PMIC5_LDO_HPM_MIN_UA 10000U
#define VRM_VOLTAGE 0U
#define VRM_ENABLE 4U
#define VRM_MODE 8U
#define TCS_STRIDE 0x2a0U
#define TCS_ENABLE (1U << 16)
#define TCS_TRIGGER (1U << 24)
#define CMD_STATUS_DONE ((1U << 8) | (1U << 16))
#define RPMH_TIMEOUT_US 1000U

enum reg_type { REG_FIXED = 1, REG_RPMH };
enum { OBS_VOLTAGE, OBS_ENABLE, OBS_MODE, OBS_COUNT };

struct kshim_regulator {
    bool used;
    uint8_t type;
    int node;
    const char *name;
    unsigned refs;
    struct kshim_regulator *vin;
    uint32_t seq;
    /* fixed */
    bool has_gpio, active_high, gpio_held, ours;
    struct kshim_gpio gpio;
    uint32_t startup_us, off_on_us;
    uint64_t off_at;
    /* RPMh */
    bool xob, resolved;
    char resource[REG_NAME];
    uint32_t address;
    uint32_t min_mv, max_mv, want_mv;
    int mode;              /* hardware mode from the DT (initial), -1 none */
    /* Load-driven modes: hardware mode per threshold, ascending. */
    uint8_t mode_count;
    uint8_t load_modes[REG_MODES];
    uint32_t load_thresholds[REG_MODES];
    uint32_t load_ua;      /* largest consumer load hint */
    bool observed;
    uint32_t observed_value[OBS_COUNT];
    uint8_t changed;       /* bit per OBS_* register kShimC wrote */
    bool kept_logged;
};

static struct {
    const void *fdt;
    uint32_t seq;
    struct kshim_regulator pool[REG_POOL];
    /* apps RSC and CMD DB; each TCS transaction runs on a stack
     * CrQupContext (claim, program, complete, restore within one call). */
    bool rsc_ready, rsc_broken;
    struct CrQupResources rsc;
} R;

/* ---- RSC / CMD DB discovery --------------------------------------------- */

static bool is_rsc(const void *f, int n)
{
    return !fdt_node_check_compatible(f, n, "qcom,rpmh-rsc") ||
           !fdt_node_check_compatible(f, n, "qcom,tcs-drv");
}

/* The apps DRV and its ACTIVE TCS window (dt-bindings soc/qcom,rpmh-rsc.h:
 * SLEEP 0, WAKE 1, ACTIVE 2, CONTROL 3; TCS groups in qcom,tcs-config order). */
static int rsc_parse(const void *f, int n, struct CrQupResources *r)
{
    uint32_t drv;
    struct dt_range region, tcs;
    if (!dt_available(f, n) || dt_u32(f, n, "qcom,drv-id", &drv) || drv > 4) return -1;
    if (!fdt_node_check_compatible(f, n, "qcom,tcs-drv")) {
        if (dt_reg(f, n, 0, &region) || dt_reg(f, n, 1, &tcs) || tcs.base <= region.base ||
            tcs.base - region.base > UINT32_MAX) return -1;
        r->tcs_offset = (uint32_t)(tcs.base - region.base);
        r->rsc_size = r->tcs_offset + tcs.size;
    } else {
        char name[16];
        snprintf(name, sizeof(name), "drv-%u", (unsigned)drv);
        if (dt_reg_named(f, n, name, &region) || dt_u32(f, n, "qcom,tcs-offset", &r->tcs_offset)) return -1;
        r->rsc_size = region.size;
    }
    if (region.base > UINTPTR_MAX) return -1;
    r->rsc_base = (uintptr_t)region.base;
    r->drv_id = drv;
    int len;
    const fdt32_t *p = fdt_getprop(f, n, "qcom,tcs-config", &len);
    if (!p || len != 32) return -1;
    unsigned offset = 0, seen = 0;
    for (int i = 0; i < 8; i += 2) {
        unsigned type = fdt32_to_cpu(p[i]), count = fdt32_to_cpu(p[i + 1]);
        if (type > 3 || (seen & (1U << type)) || count > 32 - offset) return -1;
        seen |= 1U << type;
        if (type == 2) { r->active_offset = offset; r->active_count = count; }
        offset += count;
    }
    return r->active_count && r->tcs_offset <= r->rsc_size &&
        offset * TCS_STRIDE <= r->rsc_size - r->tcs_offset ? 0 : -1;
}

static int find_rsc(const void *f)
{
    int found = -1;
    for (int n = fdt_next_node(f, -1, NULL); n >= 0; n = fdt_next_node(f, n, NULL)) {
        const char *label = dt_string(f, n, "label");
        if (!label || strcmp(label, "apps_rsc") || !dt_available(f, n) || !is_rsc(f, n)) continue;
        if (found >= 0) return -1;
        found = n;
    }
    return found;
}

/* CMD DB: "qcom,cmd-db" reserved memory (mainline) or an 8-byte Android
 * dictionary holding the table's base and size. */
static int find_cmd_db(const void *f, struct dt_range *range)
{
    int db = -1;
    for (int n = dt_node_by_compatible(f, -1, "qcom,cmd-db"); n >= 0;
         n = dt_node_by_compatible(f, n, "qcom,cmd-db")) {
        if (!dt_available(f, n)) continue;
        if (db >= 0 || dt_reg(f, n, 0, range)) return -1;
        db = n;
    }
    return db;
}

static int rsc_setup(const void *f, bool map, struct kshim_resources *res)
{
    struct CrQupResources r;
    struct dt_range db;
    memset(&r, 0, sizeof(r));
    int rsc = find_rsc(f);
    if (rsc < 0 || rsc_parse(f, rsc, &r) || find_cmd_db(f, &db) < 0) return ERROR_NOT_FOUND;
    if (db.size == 8) {
        /* Android: resolve the dictionary now, before the MMU exists. */
        const struct CrIo *io = kshim_platform_io();
        if (db.base > UINTPTR_MAX - 8) return ERROR_NOT_SUPPORTED;
        uint32_t base = io->read32(io->context, (uintptr_t)db.base);
        uint32_t size = io->read32(io->context, (uintptr_t)db.base + 4);
        if ((base & 3) || size < 144 || size > 0x20000) return ERROR_NOT_SUPPORTED;
        r.cmd_db_base = base;
        r.cmd_db_size = size;
    } else {
        if (db.base > UINTPTR_MAX || db.size < 144 || db.size > 0x20000) return ERROR_NOT_SUPPORTED;
        r.cmd_db_base = (uintptr_t)db.base;
        r.cmd_db_size = (size_t)db.size;
    }
    if (map && (kshim_resource_map(res, r.rsc_base, r.rsc_size, KSHIM_MMU_DEVICE,
                                   KSHIM_MMU_READ | KSHIM_MMU_WRITE) ||
                kshim_resource_map(res, r.cmd_db_base, r.cmd_db_size, KSHIM_MMU_NORMAL_NC,
                                   KSHIM_MMU_READ)))
        return ERROR_NOT_SUPPORTED;
    R.rsc = r;
    R.rsc_ready = true;
    R.rsc_broken = false;
    return 0;
}

/* "qcom,<pmic>-rpmh-regulators" (mainline regulator group). */
static bool rpmh_group(const char *compatible)
{
    static const char suffix[] = "-rpmh-regulators";
    size_t n = compatible ? strlen(compatible) : 0, k = sizeof(suffix) - 1;
    return n > k && !strncmp(compatible, "qcom,", 5) && !strcmp(compatible + n - k, suffix);
}

static bool any_rpmh(const void *f)
{
    for (int n = fdt_next_node(f, -1, NULL); n >= 0; n = fdt_next_node(f, n, NULL)) {
        const char *c = dt_string(f, n, "compatible");
        if (c && (rpmh_group(c) || !strcmp(c, "qcom,rpmh-vrm-regulator") ||
                  !strcmp(c, "qcom,rpmh-xob-regulator"))) return true;
    }
    return false;
}

int kshim_regulator_map_resources(struct kshim_resources *res)
{
    if (!res || !res->fdt) return ERROR_INVALID_PARAMETER;
    if (!any_rpmh(res->fdt)) return 0;
    int status = rsc_setup(res->fdt, true, res);
    /* No RSC/CMD DB: fixed rails still work; RPMh rails report NOT_READY. */
    return status == ERROR_NOT_FOUND ? 0 : status;
}

int kshim_regulator_init(const void *fdt)
{
    if (!fdt) return ERROR_INVALID_PARAMETER;
    if (R.fdt == fdt) return 0;
    for (unsigned i = 0; i < REG_POOL; ++i) if (R.pool[i].used) return ERROR_BUSY;
    R.fdt = fdt;
    if (!R.rsc_ready && any_rpmh(fdt)) (void)rsc_setup(fdt, false, NULL);
    return 0;
}

/* ---- RPMh transport ----------------------------------------------------- */

static struct CrQupContext *rpmh(struct CrQupContext *p)
{
    if (!R.rsc_ready || R.rsc_broken) return NULL;
    memset(p, 0, sizeof(*p));
    p->io = *kshim_platform_io();
    p->board = &R.rsc;
    return p;
}

static int rpmh_write(uint32_t address, uint32_t value)
{
    struct CrQupContext ctx, *p = rpmh(&ctx);
    if (!p) return ERROR_NOT_READY;
    int status = CrRpmhPollWrite(p, address, value);
    /* A triggered TCS that timed out stays claimed (a late completion must
     * not hit a reused TCS); a claim that failed before the trigger is
     * handed back to its snapshot now, since this context is short-lived. */
    if (status && p->tcs_pending) R.rsc_broken = true;
    else if (status && p->tcs_claimed && CrRpmhPollRestore(p)) R.rsc_broken = true;
    return kshim_io_err_from_bus(status);
}

static uint32_t rd(uintptr_t a) { return kshim_platform_read(a); }
static void wr(uintptr_t a, uint32_t v) { kshim_platform_write(a, v); }
static int wait_bits(uintptr_t a, uint32_t mask, uint32_t value)
{
    const struct CrIo *io = kshim_platform_io();
    uint64_t start = io->now_us(io->context);
    for (unsigned i = 0; i <= RPMH_TIMEOUT_US; ++i) {
        if ((rd(a) & mask) == value) return 0;
        if (io->now_us(io->context) - start >= RPMH_TIMEOUT_US) break;
        io->delay_us(io->context, 1);
    }
    return ERROR_TIMEOUT;
}

static int tcs_stop(uintptr_t control)
{
    uint32_t value = rd(control) & ~TCS_TRIGGER;
    wr(control, value);
    if (wait_bits(control, TCS_TRIGGER, 0)) return ERROR_TIMEOUT;
    wr(control, value & ~TCS_ENABLE);
    return wait_bits(control, TCS_TRIGGER | TCS_ENABLE, 0);
}

/* One RPMh read of the apps DRV's own request at `address`. */
static int rpmh_read(uint32_t address, uint32_t *value)
{
    struct CrQupContext ctx, *p = rpmh(&ctx);
    if (!p) return ERROR_NOT_READY;
    int status = CrRpmhPollClaim(p);
    if (status) return kshim_io_err_from_bus(status);
    uintptr_t irq = R.rsc.rsc_base + R.rsc.tcs_offset;
    uintptr_t tcs = irq + p->tcs_slot * TCS_STRIDE;
    uintptr_t control = tcs + (p->rsc_major == 2 ? 0x14U : 0x24U);
    uintptr_t cmd = tcs + (p->rsc_major == 2 ? 0x30U : 0x34U);
    uint32_t bit = 1U << p->tcs_slot;
    wr(irq, rd(irq) & ~bit); /* completion stays readable with the IRQ masked */
    if (tcs_stop(control)) goto stuck;
    wr(control + 8U, 0);
    wr(cmd, 8U | (1U << 8));
    wr(cmd + 4U, address);
    wr(cmd + 8U, 0);
    wr(control - 4U, 1);
    wr(control + 8U, 1);
    wr(control, TCS_ENABLE);
    if (wait_bits(control, TCS_ENABLE, TCS_ENABLE)) goto stuck;
    p->tcs_pending = true;
    wr(control, TCS_ENABLE | TCS_TRIGGER);
    if (wait_bits(irq + 4U, bit, bit)) goto stuck;
    status = (rd(cmd + 12U) & CMD_STATUS_DONE) == CMD_STATUS_DONE ? 0 : ERROR_DEVICE_ERROR;
    *value = rd(cmd + 16U);
    if (tcs_stop(control)) goto stuck;
    wr(irq + 8U, bit);
    p->tcs_pending = false;
    int restored = CrRpmhPollRestore(p);
    return status ? status : kshim_io_err_from_bus(restored);
stuck:
    /* Keep the TCS claimed: a late completion must not hit a reused TCS. */
    p->tcs_pending = true;
    R.rsc_broken = true;
    return ERROR_TIMEOUT;
}

/* ---- Providers ------------------------------------------------------------ */

static const char *node_name(const void *f, int node)
{
    const char *name = dt_string(f, node, "regulator-name");
    return name ? name : fdt_get_name(f, node, NULL);
}

static bool fixed(const void *f, int n)
{
    return !fdt_node_check_compatible(f, n, "regulator-fixed");
}

/* PMIC5 hardware mode for a DT regulator-initial-mode (RET 0, LPM 1,
 * AUTO 2, HPM 3 in dt-bindings/regulator/qcom,rpmh-regulator.h). */
static int pmic5_mode(const char *prefix, uint32_t dt_mode)
{
    static const int8_t ldo[4] = {3, 4, -1, 7}, smps[4] = {3, 4, 6, 7}, bob[4] = {-1, 4, 6, 7};
    if (dt_mode > 3) return -1;
    if (!strcmp(prefix, "ldo")) return ldo[dt_mode];
    if (!strcmp(prefix, "smp")) return smps[dt_mode];
    if (!strcmp(prefix, "bob")) return bob[dt_mode];
    return -1;
}

static bool pmic5(const void *f, int parent)
{
    static const char *const ids[] = {"qcom,pm8550-rpmh-regulators", "qcom,pm8550ve-rpmh-regulators",
        "qcom,pm8550vs-rpmh-regulators", "qcom,pmr735d-rpmh-regulators", "qcom,pm8010-rpmh-regulators",
        "qcom,pmc8380-rpmh-regulators", "qcom,pm8150-rpmh-regulators", "qcom,pm8150l-rpmh-regulators",
        "qcom,pm8350-rpmh-regulators", "qcom,pm8350c-rpmh-regulators", "qcom,pm8450-rpmh-regulators"};
    for (unsigned i = 0; i < sizeof(ids) / sizeof(ids[0]); ++i)
        if (!fdt_node_check_compatible(f, parent, ids[i])) return true;
    return false;
}

/* Mainline: parent "qcom,<pmic>-rpmh-regulators" with qcom,pmic-id, child
 * ldoN/smpsN/bobN -> "ldo"/"smp"/"bob" + id + N. */
static int mainline_rpmh(const void *f, int n, struct kshim_regulator *r)
{
    int parent = dt_parent_offset(f, n);
    const char *compatible = parent >= 0 ? dt_string(f, parent, "compatible") : NULL;
    const char *id = parent >= 0 ? dt_string(f, parent, "qcom,pmic-id") : NULL;
    const char *name = fdt_get_name(f, n, NULL);
    if (!rpmh_group(compatible) || !id || !name) return ERROR_NOT_FOUND;
    static const struct { const char *node, *resource; } kinds[] = {
        {"ldo", "ldo"}, {"smps", "smp"}, {"bob", "bob"}};
    for (unsigned i = 0; i < 3; ++i) {
        size_t len = strlen(kinds[i].node);
        if (strncmp(name, kinds[i].node, len) || name[len] < '1' || name[len] > '9') continue;
        const char *digits = name + len;
        for (const char *d = digits; *d; ++d) if (*d < '0' || *d > '9') return ERROR_NOT_SUPPORTED;
        int written = snprintf(r->resource, sizeof(r->resource), "%s%s%s", kinds[i].resource, id, digits);
        if (written <= 0 || (size_t)written >= sizeof(r->resource) || written > 8) return ERROR_NOT_SUPPORTED;
        uint32_t mode;
        r->mode = -1;
        if (!dt_u32(f, n, "regulator-initial-mode", &mode) && pmic5(f, parent))
            r->mode = pmic5_mode(kinds[i].resource, mode);
        /* qcom-rpmh-regulator: a PMIC5 LDO takes HPM from hpm_min_load_uA;
         * Linux only honours loads with regulator-allow-set-load. */
        if (!strcmp(kinds[i].resource, "ldo") && pmic5(f, parent) &&
            fdt_getprop(f, n, "regulator-allow-set-load", NULL)) {
            r->load_modes[0] = (uint8_t)(r->mode >= 0 ? r->mode : 4);
            r->load_thresholds[0] = 0;
            r->load_modes[1] = 7;
            r->load_thresholds[1] = PMIC5_LDO_HPM_MIN_UA;
            r->mode_count = 2;
        }
        return 0;
    }
    return ERROR_NOT_SUPPORTED;
}

/* Android rpmh-regulator load facts on the VRM node: qcom,regulator-type
 * names the PMIC5 kind, qcom,supported-modes the modes (RPMH_REGULATOR_MODE_*)
 * and qcom,mode-threshold-currents each mode's minimum load. */
static void android_load_modes(const void *f, int m, struct kshim_regulator *r)
{
    const char *type = dt_string(f, m, "qcom,regulator-type");
    const char *kind = !type ? NULL : !strcmp(type, "pmic5-ldo") ? "ldo" :
                       (!strcmp(type, "pmic5-hfsmps") || !strcmp(type, "pmic5-ftsmps")) ? "smp" :
                       !strcmp(type, "pmic5-bob") ? "bob" : NULL;
    int mlen, tlen;
    const fdt32_t *modes = fdt_getprop(f, m, "qcom,supported-modes", &mlen);
    const fdt32_t *thresholds = fdt_getprop(f, m, "qcom,mode-threshold-currents", &tlen);
    if (!kind || !modes || !thresholds || mlen != tlen || mlen <= 0 || mlen % 4 ||
        (unsigned)mlen / 4U > REG_MODES)
        return;
    unsigned count = (unsigned)mlen / 4U;
    for (unsigned i = 0; i < count; ++i) {
        int hw = pmic5_mode(kind, fdt32_to_cpu(modes[i]));
        uint32_t t = fdt32_to_cpu(thresholds[i]);
        if (hw < 0 || (i && t < r->load_thresholds[i - 1])) { r->mode_count = 0; return; }
        r->load_modes[i] = (uint8_t)hw;
        r->load_thresholds[i] = t;
    }
    r->mode_count = (uint8_t)count;
}

/* Android: the node or its parent is qcom,rpmh-vrm/xob-regulator with
 * qcom,resource-name. */
static int android_rpmh(const void *f, int n, struct kshim_regulator *r)
{
    for (int m = n, hop = 0; m >= 0 && hop < 2; m = dt_parent_offset(f, m), ++hop) {
        bool vrm = !fdt_node_check_compatible(f, m, "qcom,rpmh-vrm-regulator");
        bool xob = !fdt_node_check_compatible(f, m, "qcom,rpmh-xob-regulator");
        const char *resource = dt_string(f, m, "qcom,resource-name");
        if (!(vrm || xob) || !resource) continue;
        if (strlen(resource) >= sizeof(r->resource) || strlen(resource) > 8) return ERROR_NOT_SUPPORTED;
        strcpy(r->resource, resource);
        r->xob = xob;
        r->mode = -1;
        if (vrm) android_load_modes(f, m, r);
        return 0;
    }
    return ERROR_NOT_FOUND;
}

int kshim_regulator_get_node(const void *f, int provider, struct kshim_regulator **out)
{
    if (!f || provider < 0 || !out) return ERROR_INVALID_PARAMETER;
    if (R.fdt != f && kshim_regulator_init(f)) return ERROR_BUSY;
    struct kshim_regulator *slot = NULL;
    for (unsigned i = 0; i < REG_POOL; ++i) {
        if (R.pool[i].used && R.pool[i].node == provider) { *out = &R.pool[i]; return 0; }
        if (!R.pool[i].used && !slot) slot = &R.pool[i];
    }
    if (!slot) return KSHIM_IO_ERR_NO_MEMORY;
    if (!dt_available(f, provider)) return ERROR_NOT_SUPPORTED;
    struct kshim_regulator r = {.used = true, .node = provider, .name = node_name(f, provider), .mode = -1};
    uint32_t uv;
    if (fixed(f, provider)) {
        r.type = REG_FIXED;
        int status = kshim_gpio_from_dt(f, provider, "gpio", 0, &r.gpio);
        if (status == ERROR_NOT_FOUND) status = kshim_gpio_from_dt(f, provider, "gpios", 0, &r.gpio);
        if (status && status != ERROR_NOT_FOUND) return status;
        r.has_gpio = !status;
        r.active_high = fdt_getprop(f, provider, "enable-active-high", NULL) != NULL;
        r.gpio.active_low = !r.active_high;
        (void)dt_u32(f, provider, "startup-delay-us", &r.startup_us);
        (void)dt_u32(f, provider, "off-on-delay-us", &r.off_on_us);
    } else {
        int status = mainline_rpmh(f, provider, &r);
        if (status == ERROR_NOT_FOUND) status = android_rpmh(f, provider, &r);
        if (status) return status == ERROR_NOT_FOUND ? ERROR_NOT_SUPPORTED : status;
        r.type = REG_RPMH;
        if (!r.xob && !dt_u32(f, provider, "regulator-min-microvolt", &uv)) r.min_mv = uv / 1000U;
        if (!r.xob && !dt_u32(f, provider, "regulator-max-microvolt", &uv)) r.max_mv = uv / 1000U;
        if (r.max_mv < r.min_mv) return ERROR_INVALID_PARAMETER;
        r.want_mv = r.min_mv;
    }
    *slot = r;
    *out = slot;
    return 0;
}

int kshim_regulator_get(const void *f, int consumer, const char *supply,
                        struct kshim_regulator **out)
{
    char property[48];
    if (!f || consumer < 0 || !supply || !out) return ERROR_INVALID_PARAMETER;
    int written = snprintf(property, sizeof(property), "%s-supply", supply);
    if (written <= 0 || (size_t)written >= sizeof(property)) return ERROR_INVALID_PARAMETER;
    int len;
    const fdt32_t *p = fdt_getprop(f, consumer, property, &len);
    if (!p) return ERROR_NOT_FOUND;
    if (len != 4) return ERROR_INVALID_PARAMETER;
    int provider = dt_phandle(f, fdt32_to_cpu(*p));
    if (provider < 0) return ERROR_INVALID_PARAMETER;
    return kshim_regulator_get_node(f, provider, out);
}

const char *kshim_regulator_name(const struct kshim_regulator *r)
{
    return r && r->name ? r->name : "regulator";
}

/* ---- Enable / disable ----------------------------------------------------- */

static void wait_us(uint32_t us)
{
    const struct CrIo *io = kshim_platform_io();
    if (us && io->delay_us) io->delay_us(io->context, us);
}

static uint64_t now(void)
{
    const struct CrIo *io = kshim_platform_io();
    return io->now_us ? io->now_us(io->context) : 0;
}

static int fixed_on(struct kshim_regulator *r)
{
    (void)kshim_pinctrl_apply(R.fdt, r->node, "default");
    if (!r->has_gpio) return 0; /* always-on rail */
    int status = kshim_gpio_request(&r->gpio, r->name);
    if (status) return status;
    r->gpio_held = true;
    bool on = false;
    if (!kshim_gpio_get(&r->gpio, &on) && on) { r->ours = false; return 0; } /* inherited-on */
    if (r->off_at && r->off_on_us && now() - r->off_at < r->off_on_us)
        wait_us(r->off_on_us - (uint32_t)(now() - r->off_at));
    status = kshim_gpio_direction_output(&r->gpio, true);
    if (status) {
        (void)kshim_gpio_free(&r->gpio);
        r->gpio_held = false;
        return status;
    }
    r->ours = true;
    r->seq = ++R.seq;
    wait_us(r->startup_us);
    return 0;
}

static void fixed_off(struct kshim_regulator *r)
{
    if (!r->gpio_held) return;
    if (r->ours) {
        (void)kshim_gpio_set(&r->gpio, false);
        r->off_at = now();
    }
    (void)kshim_gpio_free(&r->gpio);
    r->gpio_held = r->ours = false;
}

static int rpmh_resolve(struct kshim_regulator *r)
{
    if (r->resolved) return 0;
    if (!R.rsc_ready || R.rsc_broken) return ERROR_NOT_READY;
    uint32_t address;
    if (CrCmdDbLookup(kshim_platform_io(), R.rsc.cmd_db_base, R.rsc.cmd_db_size, r->resource, &address) ||
        ((address >> 16) & 7U) != 4U) {
        printf("regulator: CMD DB has no VRM %s\n", r->resource);
        return ERROR_NOT_FOUND;
    }
    r->address = address;
    r->resolved = true;
    /* Own-DRV request before any vote of ours. */
    uint32_t v[OBS_COUNT];
    r->observed = !rpmh_read(address + VRM_VOLTAGE, &v[OBS_VOLTAGE]) &&
                  !rpmh_read(address + VRM_ENABLE, &v[OBS_ENABLE]) &&
                  !rpmh_read(address + VRM_MODE, &v[OBS_MODE]);
    if (r->observed) memcpy(r->observed_value, v, sizeof(v));
    return 0;
}

static int rpmh_vote(struct kshim_regulator *r, unsigned which, uint32_t value)
{
    static const uint32_t offsets[OBS_COUNT] = {VRM_VOLTAGE, VRM_ENABLE, VRM_MODE};
    if (r->observed && !(r->changed & (1U << which)) && r->observed_value[which] == value) return 0;
    int status = rpmh_write(r->address + offsets[which], value);
    if (status) return status;
    if (!(r->changed)) r->seq = ++R.seq;
    r->changed |= (uint8_t)(1U << which);
    return 0;
}

/* The DT's initial mode, overridden by the mode the consumers' load needs:
 * the highest listed mode whose threshold the load reaches. */
static int wanted_mode(const struct kshim_regulator *r)
{
    if (r->type != REG_RPMH || r->xob) return -1;
    if (!r->load_ua || !r->mode_count) return r->mode;
    int mode = r->mode;
    for (unsigned i = 0; i < r->mode_count; ++i)
        if (r->load_ua >= r->load_thresholds[i]) mode = r->load_modes[i];
    return mode;
}

int kshim_regulator_mode(const struct kshim_regulator *r)
{
    return r && r->used ? wanted_mode(r) : -1;
}

static bool rpmh_inherited_on(const struct kshim_regulator *r)
{
    if (!r->observed || (r->observed_value[OBS_ENABLE] & 1U) != 1U) return false;
    uint32_t mv = r->observed_value[OBS_VOLTAGE] & 0x1fffU;
    if (!r->xob && r->min_mv && (mv < r->min_mv || (r->max_mv && mv > r->max_mv))) return false;
    int mode = wanted_mode(r);
    return mode < 0 || (r->observed_value[OBS_MODE] & 7U) == (uint32_t)mode;
}

static int rpmh_on(struct kshim_regulator *r)
{
    int status = rpmh_resolve(r);
    if (status) return status;
    if (!r->changed && rpmh_inherited_on(r)) return 0;
    int mode = wanted_mode(r);
    if (!r->xob && r->want_mv && (status = rpmh_vote(r, OBS_VOLTAGE, r->want_mv))) return status;
    if (!r->xob && mode >= 0 && (status = rpmh_vote(r, OBS_MODE, (uint32_t)mode))) return status;
    return rpmh_vote(r, OBS_ENABLE, 1);
}

/* Return every register kShimC changed to the observed own-DRV request,
 * newest write last-in-first-out: enable, mode, voltage. */
static int rpmh_release(struct kshim_regulator *r)
{
    static const uint32_t offsets[OBS_COUNT] = {VRM_VOLTAGE, VRM_ENABLE, VRM_MODE};
    static const unsigned order[OBS_COUNT] = {OBS_ENABLE, OBS_MODE, OBS_VOLTAGE};
    if (!r->changed) return 0;
    if (!r->observed) {
        if (!r->kept_logged)
            printf("regulator: %s (%s) vote kept: inherited request unknown\n", r->name, r->resource);
        r->kept_logged = true;
        return 0;
    }
    int result = 0;
    for (unsigned i = 0; i < OBS_COUNT; ++i) {
        unsigned which = order[i];
        if (!(r->changed & (1U << which))) continue;
        int status = rpmh_write(r->address + offsets[which], r->observed_value[which]);
        if (status) { if (!result) result = status; continue; }
        r->changed &= (uint8_t)~(1U << which);
    }
    return result;
}

int kshim_regulator_enable(struct kshim_regulator *r)
{
    if (!r || !r->used) return ERROR_INVALID_PARAMETER;
    if (r->refs) { ++r->refs; return 0; }
    int status = 0;
    if (r->type == REG_FIXED && !r->vin) {
        int vs = kshim_regulator_get(R.fdt, r->node, "vin", &r->vin);
        if (vs && vs != ERROR_NOT_FOUND) printf("regulator: %s vin-supply unsupported (%d)\n", r->name, vs);
        if (vs) r->vin = NULL;
    }
    if (r->vin && r->vin != r && (status = kshim_regulator_enable(r->vin))) return status;
    status = r->type == REG_FIXED ? fixed_on(r) : rpmh_on(r);
    if (status) {
        printf("regulator: %s enable failed (%d)\n", r->name, status);
        if (r->vin && r->vin != r) (void)kshim_regulator_disable(r->vin);
        return status;
    }
    r->refs = 1;
    return 0;
}

int kshim_regulator_disable(struct kshim_regulator *r)
{
    if (!r || !r->used || !r->refs) return ERROR_INVALID_PARAMETER;
    if (--r->refs) return 0;
    int status = 0;
    if (r->type == REG_FIXED) fixed_off(r);
    else status = rpmh_release(r);
    if (r->vin && r->vin != r) (void)kshim_regulator_disable(r->vin);
    return status;
}

int kshim_regulator_set_voltage(struct kshim_regulator *r, uint32_t min_uv, uint32_t max_uv)
{
    if (!r || !r->used || min_uv > max_uv) return ERROR_INVALID_PARAMETER;
    uint32_t lo = min_uv / 1000U, hi = max_uv / 1000U;
    if (r->type == REG_FIXED || r->xob) {
        /* Fixed rails: accept a range containing their voltage. */
        uint32_t uv;
        if (R.fdt && !dt_u32(R.fdt, r->node, "regulator-min-microvolt", &uv) &&
            (uv < min_uv || uv > max_uv)) return ERROR_NOT_SUPPORTED;
        return 0;
    }
    if (r->min_mv && lo < r->min_mv) lo = r->min_mv;
    if (r->max_mv && hi > r->max_mv) hi = r->max_mv;
    if (lo > hi || !lo) return ERROR_NOT_SUPPORTED;
    r->want_mv = lo;
    if (!r->refs) return 0;
    int status = rpmh_resolve(r);
    return status ? status : rpmh_vote(r, OBS_VOLTAGE, lo);
}

int kshim_regulator_set_load(struct kshim_regulator *r, uint32_t load_ua)
{
    if (!r || !r->used) return ERROR_INVALID_PARAMETER;
    if (r->type != REG_RPMH || r->xob || load_ua <= r->load_ua) return 0;
    int before = wanted_mode(r);
    r->load_ua = load_ua;
    int after = wanted_mode(r);
    if (!r->refs || after < 0 || after == before) return 0;
    int status = rpmh_resolve(r);
    return status ? status : rpmh_vote(r, OBS_MODE, (uint32_t)after);
}

bool kshim_regulator_is_enabled(const struct kshim_regulator *r)
{
    if (!r || !r->used) return false;
    if (r->refs) return true;
    if (r->type == REG_FIXED) {
        bool on = !r->has_gpio;
        if (r->has_gpio) (void)kshim_gpio_get(&r->gpio, &on);
        return on;
    }
    /* Inherit checks (a controller the previous stage left powered) ask
     * before any enable: observe the own-DRV request now. The pool object
     * only caches the read-back here; no vote is written. */
    if (!r->resolved) (void)rpmh_resolve((struct kshim_regulator *)(uintptr_t)r);
    return r->observed && (r->observed_value[OBS_ENABLE] & 1U);
}

/* ---- Bulk ----------------------------------------------------------------- */

static int supply_at(const void *f, int node, int offset, char *name, size_t size)
{
    const char *property;
    int len;
    if (!fdt_getprop_by_offset(f, offset, &property, &len)) return -1;
    size_t n = strlen(property);
    if (n <= 7 || strcmp(property + n - 7, "-supply") || n - 7 >= size) return -1;
    (void)node;
    memcpy(name, property, n - 7);
    name[n - 7] = 0;
    return 0;
}

int kshim_regulator_bulk_enable(const void *f, int node)
{
    if (!f || node < 0) return ERROR_INVALID_PARAMETER;
    struct kshim_regulator *done[8];
    unsigned count = 0;
    int offset, status = 0;
    fdt_for_each_property_offset(offset, f, node) {
        char name[40];
        struct kshim_regulator *r;
        if (supply_at(f, node, offset, name, sizeof(name))) continue;
        status = kshim_regulator_get(f, node, name, &r);
        if (!status && count == 8) status = KSHIM_IO_ERR_NO_MEMORY;
        if (!status) status = kshim_regulator_enable(r);
        if (status) break;
        done[count++] = r;
    }
    if (status)
        while (count) (void)kshim_regulator_disable(done[--count]);
    return status;
}

int kshim_regulator_bulk_disable(const void *f, int node)
{
    if (!f || node < 0) return ERROR_INVALID_PARAMETER;
    struct kshim_regulator *list[8];
    unsigned count = 0;
    int offset, result = 0;
    fdt_for_each_property_offset(offset, f, node) {
        char name[40];
        struct kshim_regulator *r;
        if (supply_at(f, node, offset, name, sizeof(name)) || count == 8 ||
            kshim_regulator_get(f, node, name, &r)) continue;
        list[count++] = r;
    }
    while (count) {
        int status = kshim_regulator_disable(list[--count]);
        if (status && !result) result = status;
    }
    return result;
}

/* ---- Handoff -------------------------------------------------------------- */

int kshim_regulator_restore_all(void)
{
    int result = 0;
    for (;;) {
        struct kshim_regulator *newest = NULL;
        for (unsigned i = 0; i < REG_POOL; ++i) {
            struct kshim_regulator *r = &R.pool[i];
            bool pending = r->used && (r->type == REG_FIXED ? r->gpio_held : r->changed && !r->kept_logged);
            if (pending && (!newest || r->seq > newest->seq)) newest = r;
        }
        if (!newest) break;
        int status = 0;
        if (newest->type == REG_FIXED) fixed_off(newest);
        else {
            status = rpmh_release(newest);
            if (status) newest->kept_logged = true; /* do not retry forever */
        }
        if (status && !result) result = status;
    }
    for (unsigned i = 0; i < REG_POOL; ++i) R.pool[i].refs = 0;
    return result;
}
