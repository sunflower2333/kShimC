/* SPDX-License-Identifier: MIT */
/*
 * GPIO consumer API (kshim/gpio.h) for TLMM pads and SPMI PMIC GPIOs.
 *
 * Every pin kShimC touches gets a registry slot. The slot holds the
 * consumer reference count and a snapshot taken before the first write:
 * TLMM CFG and IO words, or the PMIC GPIO configuration bytes. A freed pin
 * returns to its snapshot unless pinctrl also touched it; every snapshot
 * left goes back at kshim_gpio_restore_all(), newest first.
 *
 * TLMM register facts (CFG +0: PULL 1:0, FUNC 5:2, DRV 8:6, OE 9, SM8650
 * EGPIO present 11 / enable 12; IO +4: IN 0, OUT 1) are CrDK GpioLib
 * RegCfg[0] of Target/Sm8150, Sm8250 and Sm8650 CrGpioTarget.c. Selecting
 * any function on an EGPIO-capable SM8650 pad also claims it for the apps
 * TLMM (EGPIO enable), as pinctrl-sm8650's group facts describe. Values
 * that already match are never written, so inherited pads stay untouched.
 *
 * PMIC GPIOs are configured through CrDK PmicGpioLib (subtype detection,
 * LV/MV vs. legacy MODE_CTL encoding) over a one-pin "controller" for the
 * pin being changed, with the byte transport of kernel/drivers/qcom_keys.c.
 * Registers 0x40-0x46 and 0x4a (PmicGpioLib register facts) are the
 * snapshot. PMIC interrupts are not offered.
 */
#include "internal.h"

#include <kshim/io.h>
#include <kshim/irq.h>
#include <kshim/soc_tables.h>
#include <qcom_dt.h>
#include <config.h>
#include <stdio.h>
#include <string.h>
#if !defined(KSHIM_HOST_TEST) && CONFIG_QCOM_SPMI
#include <qcom_key_platform.h>
#endif

/* PmicGpioLib is built with CrDK SpmiLib (CRDK_SPMI = CONFIG_QCOM_SPMI). */
#if CONFIG_QCOM_SPMI
#define GPIO_PMIC 1
#else
#define GPIO_PMIC 0
#endif
#define GPIO_SLOTS 24U
#define TLMM_PROVIDERS 2U
/* PMIC GPIO peripheral registers (CrDK PmicGpioLib). */
#define PMIC_SNAPSHOT 8U
static const uint8_t PmicRegs[PMIC_SNAPSHOT] = {0x41, 0x42, 0x43, 0x45, 0x44, 0x4a, 0x40, 0x46};

struct slot {
    bool used, pinctrl, output, snapped;
    uint8_t chip;
    uint32_t pin;
    uintptr_t ctl;
    uint8_t controller, sid;
    uint16_t base;
    bool egpio;
    unsigned refs;
    const char *owner;
    uint32_t seq;
    uint32_t cfg, io;
    uint8_t pmic[PMIC_SNAPSHOT];
    uint8_t pmic_valid;  /* bit per PmicRegs entry read successfully */
};

static struct {
    const void *fdt;
    bool ready;
    uint32_t seq;
    struct slot slots[GPIO_SLOTS];
} G;

/* ---- MMIO and PMIC transport --------------------------------------------- */

static const struct CrIo *Io;
void kshim_platform_set_io(const struct CrIo *io) { Io = io; }
const struct CrIo *kshim_platform_io(void)
{
    if (!Io) Io = CrIoDefault();
    return Io;
}

#if !defined(KSHIM_HOST_TEST) && CONFIG_QCOM_SPMI
static CR_STATUS pmic_read(VOID *context, UINT8 sid, UINT16 address, UINT8 *buffer, UINTN length)
{
    if (!buffer || length != 1) return CR_INVALID_PARAMETER;
    return kshim_qcom_pmic_gpio_read8((uint8_t)(uintptr_t)context, sid, address, buffer)
        ? CR_DEVICE_ERROR : CR_SUCCESS;
}
static CR_STATUS pmic_write(VOID *context, UINT8 sid, UINT16 address, CONST UINT8 *buffer,
                            UINTN length)
{
    if (!buffer || length != 1) return CR_INVALID_PARAMETER;
    return kshim_qcom_pmic_gpio_write8((uint8_t)(uintptr_t)context, sid, address, *buffer)
        ? CR_DEVICE_ERROR : CR_SUCCESS;
}
static PmicGpioBusOps PmicBus = {.Read = pmic_read, .Write = pmic_write};
#else
static PmicGpioBusOps PmicBus;
#endif

void kshim_gpio_set_pmic_bus(const PmicGpioBusOps *bus)
{
    if (bus) PmicBus = *bus;
    else memset(&PmicBus, 0, sizeof(PmicBus));
}

static bool pmic_bus_ready(void) { return GPIO_PMIC && PmicBus.Read && PmicBus.Write; }

static PmicGpioBusOps pmic_bus(uint8_t controller)
{
    PmicGpioBusOps bus = PmicBus;
    if (!bus.Context) bus.Context = (VOID *)(uintptr_t)controller;
    return bus;
}

/* ---- TLMM provider facts ------------------------------------------------- */

unsigned kshim_gpio_tlmm_soc(const void *f, int provider)
{
    if (!f || provider < 0 || !fdt_getprop(f, provider, "gpio-controller", NULL)) return 0;
    unsigned soc = kshim_dt_soc(f, provider);
    return kshim_soc_tlmm(soc) ? soc : 0;
}

bool kshim_gpio_pin_reserved(const void *f, int provider, uint32_t pin)
{
    int len;
    const fdt32_t *p = fdt_getprop(f, provider, "gpio-reserved-ranges", &len);
    if (!p || len <= 0) return false;
    for (int i = 0; i + 1 < len / 4; i += 2) {
        uint32_t start = fdt32_to_cpu(p[i]), count = fdt32_to_cpu(p[i + 1]);
        if (pin >= start && pin - start < count) return true;
    }
    return false;
}

static bool is_pmic_gpio(const void *f, int provider)
{
    return !dt_node_check_compatible(f, provider, "qcom,spmi-gpio") &&
           fdt_getprop(f, provider, "gpio-controller", NULL);
}

int kshim_gpio_init(const void *fdt)
{
    if (!fdt) return ERROR_INVALID_PARAMETER;
    if (G.ready && G.fdt == fdt) return 0;
    for (unsigned i = 0; i < GPIO_SLOTS; ++i)
        if (G.slots[i].used) return ERROR_BUSY;
    G.fdt = fdt;
    G.ready = true;
    return 0;
}

int kshim_gpio_map_resources(struct kshim_resources *r)
{
    if (!r || !r->fdt) return ERROR_INVALID_PARAMETER;
    const void *f = r->fdt;
    unsigned mapped = 0;
    for (int n = dt_next_node(f, -1); n >= 0; n = dt_next_node(f, n)) {
        if (!dt_available(f, n) || !kshim_gpio_tlmm_soc(f, n)) continue;
        struct dt_range reg;
        if (mapped == TLMM_PROVIDERS || dt_reg(f, n, 0, &reg) ||
            kshim_resource_map(r, reg.base, reg.size, KSHIM_MMU_DEVICE,
                               KSHIM_MMU_READ | KSHIM_MMU_WRITE)) return ERROR_NOT_SUPPORTED;
        ++mapped;
    }
    return 0;
}

/* ---- Descriptors --------------------------------------------------------- */

static int tlmm_descriptor(const void *f, int provider, uint32_t pin, uint32_t flags,
                           struct kshim_gpio *g)
{
    uintptr_t ctl;
    if (!kshim_gpio_tlmm_soc(f, provider) || kshim_dt_gpio(f, provider, pin, &ctl, NULL, NULL))
        return ERROR_NOT_SUPPORTED;
    if (kshim_gpio_pin_reserved(f, provider, pin)) {
        printf("GPIO: TLMM pin %u is reserved (gpio-reserved-ranges)\n", (unsigned)pin);
        return ERROR_NOT_SUPPORTED;
    }
    *g = (struct kshim_gpio){.chip = KSHIM_GPIO_CHIP_TLMM, .provider = provider, .pin = pin,
        .dt_flags = flags, .active_low = (flags & KSHIM_GPIO_DT_ACTIVE_LOW) != 0, .ctl = ctl,
        .slot = -1};
    return 0;
}

static int pmic_descriptor(const void *f, int provider, uint32_t pin, uint32_t flags,
                           struct kshim_gpio *g)
{
    uint8_t controller, bus, sid;
    uint16_t base;
    if (!pin || pin > 64 || kshim_dt_spmi_identity(f, provider, &controller, &bus, &sid) ||
        kshim_dt_spmi_reg(f, provider, &base) || base + (pin - 1U) * 0x100U > 0xff00U)
        return ERROR_NOT_SUPPORTED;
    *g = (struct kshim_gpio){.chip = KSHIM_GPIO_CHIP_PMIC, .provider = provider, .pin = pin,
        .dt_flags = flags, .active_low = (flags & KSHIM_GPIO_DT_ACTIVE_LOW) != 0,
        .spmi_controller = controller, .sid = sid,
        .base = (uint16_t)(base + (pin - 1U) * 0x100U), .slot = -1};
    return 0;
}

int kshim_gpio_from_dt(const void *f, int node, const char *property, unsigned index,
                       struct kshim_gpio *g)
{
    struct dt_spec spec;
    if (!f || node < 0 || !property || !g) return ERROR_INVALID_PARAMETER;
    if (!fdt_getprop(f, node, property, NULL)) return ERROR_NOT_FOUND;
    if (dt_specifier(f, node, property, "#gpio-cells", index, &spec) || spec.count < 1)
        return index ? ERROR_NOT_FOUND : ERROR_INVALID_PARAMETER;
    if (!dt_available(f, spec.node)) return ERROR_NOT_SUPPORTED;
    uint32_t flags = spec.count > 1 ? spec.args[1] : 0;
    if (kshim_gpio_tlmm_soc(f, spec.node)) return tlmm_descriptor(f, spec.node, spec.args[0], flags, g);
    if (is_pmic_gpio(f, spec.node)) return pmic_descriptor(f, spec.node, spec.args[0], flags, g);
    return ERROR_NOT_SUPPORTED;
}

int kshim_gpio_from_tlmm_pin(const void *f, int tlmm, uint32_t pin, bool active_low,
                             struct kshim_gpio *g)
{
    if (!f || tlmm < 0 || !g) return ERROR_INVALID_PARAMETER;
    return tlmm_descriptor(f, tlmm, pin, active_low ? KSHIM_GPIO_DT_ACTIVE_LOW : 0, g);
}

int kshim_gpio_tlmm_pin(const struct kshim_gpio *g, uint32_t *pin)
{
    if (!g || !pin) return ERROR_INVALID_PARAMETER;
    if (g->chip != KSHIM_GPIO_CHIP_TLMM) return ERROR_NOT_SUPPORTED;
    *pin = g->pin;
    return 0;
}

/* ---- Registry ------------------------------------------------------------ */

static bool same(const struct slot *s, const struct kshim_gpio *g)
{
    if (!s->used || s->chip != g->chip) return false;
    if (g->chip == KSHIM_GPIO_CHIP_TLMM) return s->ctl == g->ctl;
    return s->controller == g->spmi_controller && s->sid == g->sid && s->base == g->base;
}

static struct slot *find(const struct kshim_gpio *g)
{
    for (unsigned i = 0; i < GPIO_SLOTS; ++i)
        if (same(&G.slots[i], g)) return &G.slots[i];
    return NULL;
}

static struct slot *slot_of(const struct kshim_gpio *g)
{
    if (!g || g->slot < 0 || (unsigned)g->slot >= GPIO_SLOTS) return NULL;
    struct slot *s = &G.slots[g->slot];
    return same(s, g) && s->refs ? s : NULL;
}

/* TLMM word update: read-modify-write with IRQs masked so an IRQ-context
 * kshim_gpio_set() of the same pin cannot interleave; equal values are
 * not written. */
static void tlmm_update(uintptr_t address, uint32_t mask, uint32_t value)
{
    unsigned long flags = kshim_irq_save();
    uint32_t old = kshim_platform_read(address), now = (old & ~mask) | (value & mask);
    if (now != old) kshim_platform_write(address, now);
    kshim_irq_restore(flags);
}

#if GPIO_PMIC
/* PmicGpioLib over one single-pin "controller", the slot's pin; the
 * context lives on the caller's stack for one operation (PMIC GPIO calls
 * are rare and thread context only). */
struct pmic_op {
    PmicGpioDeviceContext ctx;
    PmicGpioTargetController target;
    PmicGpioTargetContext targets;
};
static int pmic_open(const struct slot *s, struct pmic_op *op)
{
    op->target = (PmicGpioTargetController){"kshim", "qcom,spmi-gpio", s->sid, s->base, 1};
    op->targets = (PmicGpioTargetContext){&op->target, 1};
    PmicGpioBusOps bus = pmic_bus(s->controller);
    return PmicGpioInitialize(&op->ctx, &op->targets, &bus) == CR_SUCCESS ? 0 : ERROR_DEVICE_ERROR;
}
#endif

static int snapshot(struct slot *s)
{
    if (s->chip == KSHIM_GPIO_CHIP_TLMM) {
        s->cfg = kshim_platform_read(s->ctl);
        s->io = kshim_platform_read(s->ctl + 4);
    } else {
        PmicGpioBusOps bus = pmic_bus(s->controller);
        s->pmic_valid = 0;
        for (unsigned i = 0; i < PMIC_SNAPSHOT; ++i)
            if (bus.Read(bus.Context, s->sid, (UINT16)(s->base + PmicRegs[i]), &s->pmic[i], 1) == CR_SUCCESS)
                s->pmic_valid |= (uint8_t)(1U << i);
        /* EN_CTL and MODE_CTL must be restorable. */
        if ((s->pmic_valid & 0xc0U) != 0xc0U) return ERROR_DEVICE_ERROR;
    }
    s->snapped = true;
    s->seq = ++G.seq;
    return 0;
}

static int restore(struct slot *s)
{
    int status = 0;
    if (!s->snapped) return 0;
    if (s->chip == KSHIM_GPIO_CHIP_TLMM) {
        /* Output pads get their level before OE; inputs drop OE first. */
        if (s->cfg & KSHIM_TLMM_CFG_OE) {
            tlmm_update(s->ctl + 4, KSHIM_TLMM_IO_OUT, s->io);
            tlmm_update(s->ctl, UINT32_MAX, s->cfg);
        } else {
            tlmm_update(s->ctl, UINT32_MAX, s->cfg);
            tlmm_update(s->ctl + 4, KSHIM_TLMM_IO_OUT, s->io);
        }
    } else {
        PmicGpioBusOps bus = pmic_bus(s->controller);
        /* PmicRegs order: configuration, then MODE_CTL, then EN_CTL last. */
        for (unsigned i = 0; i < PMIC_SNAPSHOT; ++i)
            if ((s->pmic_valid & (1U << i)) &&
                bus.Write(bus.Context, s->sid, (UINT16)(s->base + PmicRegs[i]), &s->pmic[i], 1) != CR_SUCCESS)
                status = ERROR_DEVICE_ERROR;
    }
    s->snapped = false;
    return status;
}

static void release(struct slot *s) { memset(s, 0, sizeof(*s)); }

/* Find or create the slot for `g`, snapshotting a new one. */
static int attach(struct kshim_gpio *g, struct slot **out)
{
    struct slot *s = find(g);
    if (!s) {
        if (g->chip == KSHIM_GPIO_CHIP_PMIC && !pmic_bus_ready()) return ERROR_NOT_SUPPORTED;
        for (unsigned i = 0; i < GPIO_SLOTS && !s; ++i)
            if (!G.slots[i].used) s = &G.slots[i];
        if (!s) return KSHIM_IO_ERR_NO_MEMORY;
        *s = (struct slot){.used = true, .chip = (uint8_t)g->chip, .pin = g->pin, .ctl = g->ctl,
            .controller = g->spmi_controller, .sid = g->sid, .base = g->base};
        if (g->chip == KSHIM_GPIO_CHIP_TLMM) {
            const struct kshim_soc_tlmm *t = G.fdt ? kshim_soc_tlmm(kshim_dt_soc(G.fdt, g->provider)) : NULL;
            s->egpio = t && t->egpio;
        }
        int status = snapshot(s);
#if GPIO_PMIC
        if (!status && g->chip == KSHIM_GPIO_CHIP_PMIC) {
            struct pmic_op op; /* the peripheral must probe as a GPIO */
            status = pmic_open(s, &op);
        }
#endif
        if (status) {
            memset(s, 0, sizeof(*s));
            return status;
        }
    }
    *out = s;
    return 0;
}

static bool valid(const struct kshim_gpio *g)
{
    if (!g) return false;
    if (g->chip == KSHIM_GPIO_CHIP_TLMM) return g->ctl != 0;
    return g->chip == KSHIM_GPIO_CHIP_PMIC && g->base;
}

int kshim_gpio_request(struct kshim_gpio *g, const char *owner)
{
    if (!valid(g) || !owner) return ERROR_INVALID_PARAMETER;
    struct slot *s = find(g);
    if (s && s->refs && s->output && s->owner && strcmp(s->owner, owner)) {
        printf("GPIO: pin %u driven by %s, %s refused\n", (unsigned)g->pin, s->owner, owner);
        return ERROR_BUSY;
    }
    int status = attach(g, &s);
    if (status) return status;
    if (!s->refs) s->owner = owner;
    ++s->refs;
    g->slot = (int)(s - G.slots);
    return 0;
}

static uint32_t egpio_claim(const struct slot *s, uint32_t cfg)
{
    return s->egpio && (cfg & KSHIM_TLMM_CFG_EGPIO_PRESENT) ? KSHIM_TLMM_CFG_EGPIO_ENABLE : 0;
}

int kshim_gpio_direction_output(struct kshim_gpio *g, bool active)
{
    struct slot *s = slot_of(g);
    if (!s) return ERROR_INVALID_PARAMETER;
    bool level = active != g->active_low;
    if (s->chip == KSHIM_GPIO_CHIP_TLMM) {
        tlmm_update(s->ctl + 4, KSHIM_TLMM_IO_OUT, level ? KSHIM_TLMM_IO_OUT : 0);
        uint32_t cfg = kshim_platform_read(s->ctl), claim = egpio_claim(s, cfg);
        tlmm_update(s->ctl, KSHIM_TLMM_CFG_FUNC_MASK | KSHIM_TLMM_CFG_OE | claim,
                    KSHIM_TLMM_CFG_OE | claim);
    } else {
#if GPIO_PMIC
        struct pmic_op op;
        PmicGpioConfig c;
        if (pmic_open(s, &op) || PmicGpioGetConfig(&op.ctx, 0, 1, &c) != CR_SUCCESS) return ERROR_DEVICE_ERROR;
        if (!c.Enabled || c.Direction != PMIC_GPIO_DIRECTION_OUTPUT ||
            c.Function != PMIC_GPIO_FUNCTION_NORMAL || c.OutputValue != level) {
            c.Enabled = TRUE;
            c.Direction = PMIC_GPIO_DIRECTION_OUTPUT;
            c.Function = PMIC_GPIO_FUNCTION_NORMAL;
            c.OutputValue = level;
            if (PmicGpioConfigure(&op.ctx, 0, 1, &c) != CR_SUCCESS)
                return ERROR_DEVICE_ERROR;
        }
#else
        return ERROR_NOT_SUPPORTED;
#endif
    }
    s->output = true;
    return 0;
}

int kshim_gpio_direction_input(struct kshim_gpio *g, enum kshim_gpio_pull pull)
{
    struct slot *s = slot_of(g);
    if (!s || (pull > KSHIM_GPIO_PULL_UP && pull != KSHIM_GPIO_PULL_KEEP)) return ERROR_INVALID_PARAMETER;
    if (s->chip == KSHIM_GPIO_CHIP_TLMM) {
        uint32_t cfg = kshim_platform_read(s->ctl), claim = egpio_claim(s, cfg);
        uint32_t mask = KSHIM_TLMM_CFG_FUNC_MASK | KSHIM_TLMM_CFG_OE | claim, value = claim;
        if (pull != KSHIM_GPIO_PULL_KEEP) { mask |= KSHIM_TLMM_CFG_PULL_MASK; value |= (uint32_t)pull; }
        tlmm_update(s->ctl, mask, value);
    } else {
#if GPIO_PMIC
        /* PMIC pulls: TLMM-style none/down/up onto PmicGpioLib's values. */
        struct pmic_op op;
        PmicGpioConfig c;
        if (pmic_open(s, &op) || PmicGpioGetConfig(&op.ctx, 0, 1, &c) != CR_SUCCESS) return ERROR_DEVICE_ERROR;
        PmicGpioConfig want = c;
        want.Enabled = TRUE;
        want.Direction = PMIC_GPIO_DIRECTION_INPUT;
        want.Function = PMIC_GPIO_FUNCTION_NORMAL;
        if (pull == KSHIM_GPIO_PULL_NONE) want.Pull = PMIC_GPIO_PULL_NONE;
        else if (pull == KSHIM_GPIO_PULL_DOWN) want.Pull = PMIC_GPIO_PULL_DOWN_10UA;
        else if (pull == KSHIM_GPIO_PULL_UP) want.Pull = PMIC_GPIO_PULL_UP_30UA;
        else if (pull == KSHIM_GPIO_PULL_KEEPER) return ERROR_NOT_SUPPORTED;
        if (memcmp(&want, &c, sizeof(c)) &&
            PmicGpioConfigure(&op.ctx, 0, 1, &want) != CR_SUCCESS)
            return ERROR_DEVICE_ERROR;
#else
        return ERROR_NOT_SUPPORTED;
#endif
    }
    s->output = false;
    return 0;
}

int kshim_gpio_set(struct kshim_gpio *g, bool active)
{
    if (!g || g->slot < 0 || (unsigned)g->slot >= GPIO_SLOTS) return ERROR_INVALID_PARAMETER;
    struct slot *s = &G.slots[g->slot];
    if (!same(s, g)) return ERROR_INVALID_PARAMETER;
    bool level = active != g->active_low;
    if (s->chip == KSHIM_GPIO_CHIP_TLMM) {
        tlmm_update(s->ctl + 4, KSHIM_TLMM_IO_OUT, level ? KSHIM_TLMM_IO_OUT : 0);
        return 0;
    }
#if GPIO_PMIC
    struct pmic_op op;
    if (pmic_open(s, &op)) return ERROR_DEVICE_ERROR;
    return PmicGpioSetValue(&op.ctx, 0, 1, level) == CR_SUCCESS ? 0 : ERROR_DEVICE_ERROR;
#else
    return ERROR_NOT_SUPPORTED;
#endif
}

int kshim_gpio_get(const struct kshim_gpio *g, bool *active)
{
    if (!valid(g) || !active) return ERROR_INVALID_PARAMETER;
    bool level;
    if (g->chip == KSHIM_GPIO_CHIP_TLMM) {
        level = (kshim_platform_read(g->ctl + 4) & KSHIM_TLMM_IO_IN) != 0;
    } else {
#if GPIO_PMIC
        if (!pmic_bus_ready()) return ERROR_NOT_SUPPORTED;
        PmicGpioBusOps bus = pmic_bus(g->spmi_controller);
        UINT8 status;
        if (PmicGpioReadStatus(&bus, g->sid, g->base, &status) != CR_SUCCESS) return ERROR_DEVICE_ERROR;
        level = (status & 1U) != 0;
#else
        return ERROR_NOT_SUPPORTED;
#endif
    }
    *active = level != g->active_low;
    return 0;
}

int kshim_gpio_free(struct kshim_gpio *g)
{
    struct slot *s = slot_of(g);
    if (!s) return ERROR_INVALID_PARAMETER;
    g->slot = -1;
    if (--s->refs) return 0;
    s->output = false;
    s->owner = NULL;
    if (s->pinctrl) return 0; /* restored with the pinctrl snapshot at handoff */
    int status = restore(s);
    release(s);
    return status;
}

int kshim_gpio_pinctrl_apply(struct kshim_gpio *g, uint32_t cfg, uint32_t cfg_mask, int out_level)
{
    if (!valid(g) || g->chip != KSHIM_GPIO_CHIP_TLMM) return ERROR_NOT_SUPPORTED;
    struct slot *s;
    int status = attach(g, &s);
    if (status) return status;
    s->pinctrl = true;
    if (out_level >= 0) tlmm_update(s->ctl + 4, KSHIM_TLMM_IO_OUT, out_level ? KSHIM_TLMM_IO_OUT : 0);
    uint32_t claim = (cfg_mask & KSHIM_TLMM_CFG_FUNC_MASK) ? egpio_claim(s, kshim_platform_read(s->ctl)) : 0;
    tlmm_update(s->ctl, cfg_mask | claim, cfg | claim);
    return 0;
}

unsigned kshim_gpio_snapshot_count(void)
{
    unsigned count = 0;
    for (unsigned i = 0; i < GPIO_SLOTS; ++i) count += G.slots[i].used && G.slots[i].snapped;
    return count;
}

int kshim_gpio_restore_all(void)
{
    int result = 0;
    for (;;) {
        struct slot *newest = NULL;
        for (unsigned i = 0; i < GPIO_SLOTS; ++i) {
            struct slot *s = &G.slots[i];
            if (s->used && (!newest || s->seq > newest->seq)) newest = s;
        }
        if (!newest) break;
        int status = restore(newest);
        if (status && !result) result = status;
        memset(newest, 0, sizeof(*newest));
    }
    return result;
}
