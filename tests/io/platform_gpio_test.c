/* SPDX-License-Identifier: MIT */
/*
 * GPIO registry and pinctrl over a TLMM/PMIC register model, on the
 * Pocket S2 fixture: request/set/free with reference counts and owner
 * conflicts, level-before-OE ordering, inherited pads left unwritten,
 * active-low, SM8650 EGPIO claim, the Pocket S2 gamepad_pwr_en / touch /
 * i2c4 / pcie1 pinctrl states, snapshot restore newest first, PMIC GPIO
 * through PmicGpioLib (LV_VIN2 subtype), and the kona (SM8250 Android) layout.
 */
#include "platform_fake.h"

#include <kshim/gpio.h>
#include <kshim/pinctrl.h>
#include <kshim/soc_tables.h>
#include "../../kernel/drivers/gpio/internal.h"

#define TLMM 0xf100000UL
static uintptr_t cfg(unsigned pin) { return TLMM + pin * 0x1000UL; }
static uintptr_t io(unsigned pin) { return cfg(pin) + 4; }

/* Index of the first logged write to `address` at or after `from`. */
static int logged(uintptr_t address, unsigned from)
{
    for (unsigned i = from; i < FakeLogCount; ++i) if (FakeLog[i].address == address) return (int)i;
    return -1;
}

static int state_of(const void *f, const char *name)
{
    int n = fdt_subnode_offset(f, fake_path(f, "/soc@0/pinctrl@f100000"), name);
    assert(n >= 0);
    return n;
}

static void tlmm_consumers(void *f)
{
    int tlmm = fake_path(f, "/soc@0/pinctrl@f100000");
    struct kshim_gpio a, b, perst;
    /* Pad 123: inherited input with pull-down, output latch low. */
    fake_poke(cfg(123), 0x1);
    fake_poke(io(123), 0x0);
    assert(!kshim_gpio_from_tlmm_pin(f, tlmm, 123, false, &a));
    assert(!kshim_gpio_request(&a, "upd720201_avdd33") && a.slot >= 0);
    unsigned mark = FakeLogCount;
    assert(!kshim_gpio_direction_output(&a, true));
    int level = logged(io(123), mark), oe = logged(cfg(123), mark);
    assert(level >= 0 && oe > level); /* OUT before OE: no glitch */
    assert(fake_peek(io(123)) == KSHIM_TLMM_IO_OUT && fake_peek(cfg(123)) == (0x1 | KSHIM_TLMM_CFG_OE));
    bool on;
    fake_poke(io(123), KSHIM_TLMM_IO_OUT | KSHIM_TLMM_IO_IN);
    assert(!kshim_gpio_get(&a, &on) && on);
    /* Another owner cannot take a driven pin; the same owner can. */
    assert(!kshim_gpio_from_tlmm_pin(f, tlmm, 123, false, &b));
    assert(kshim_gpio_request(&b, "touch") == ERROR_BUSY && b.slot == -1);
    assert(!kshim_gpio_request(&b, "upd720201_avdd33"));
    assert(!kshim_gpio_set(&a, false) && !(fake_peek(io(123)) & KSHIM_TLMM_IO_OUT));
    assert(!kshim_gpio_free(&b) && kshim_gpio_snapshot_count() == 1);
    assert(fake_peek(cfg(123)) & KSHIM_TLMM_CFG_OE); /* one reference left */
    assert(!kshim_gpio_free(&a) && a.slot == -1);
    assert(fake_peek(cfg(123)) == 0x1 && (fake_peek(io(123)) & KSHIM_TLMM_IO_OUT) == 0);
    assert(kshim_gpio_snapshot_count() == 0);
    assert(kshim_gpio_free(&a) == ERROR_INVALID_PARAMETER);

    /* An inherited output at the wanted level: nothing is written. */
    fake_poke(cfg(122), KSHIM_TLMM_CFG_OE);
    fake_poke(io(122), KSHIM_TLMM_IO_OUT);
    assert(!kshim_gpio_from_tlmm_pin(f, tlmm, 122, false, &a) && !kshim_gpio_request(&a, "x"));
    unsigned writes = FakeWrites;
    assert(!kshim_gpio_direction_output(&a, true));
    assert(FakeWrites == writes);
    assert(!kshim_gpio_free(&a) && FakeWrites == writes);

    /* PERST# TLMM 97 is active-low in the DT: asserting drives it low. */
    int pcie1 = fake_path(f, "/soc@0/pcie@1c08000");
    fake_poke(cfg(97), 0x1);
    fake_poke(io(97), KSHIM_TLMM_IO_OUT);
    assert(!kshim_gpio_from_dt(f, pcie1, "perst-gpios", 0, &perst) && perst.active_low);
    assert(!kshim_gpio_request(&perst, "pcie1"));
    assert(!kshim_gpio_direction_output(&perst, true) && !(fake_peek(io(97)) & KSHIM_TLMM_IO_OUT));
    assert(!kshim_gpio_set(&perst, false) && (fake_peek(io(97)) & KSHIM_TLMM_IO_OUT));
    assert(!kshim_gpio_direction_input(&perst, KSHIM_GPIO_PULL_UP));
    assert(fake_peek(cfg(97)) == KSHIM_GPIO_PULL_UP);
    assert(!kshim_gpio_free(&perst) && fake_peek(cfg(97)) == 0x1);

    /* EGPIO-capable pad (gpio209): selecting GPIO claims it for apps. */
    fake_poke(cfg(209), KSHIM_TLMM_CFG_EGPIO_PRESENT | (10U << KSHIM_TLMM_CFG_FUNC_SHIFT));
    assert(!kshim_gpio_from_tlmm_pin(f, tlmm, 209, false, &a) && !kshim_gpio_request(&a, "egpio"));
    assert(!kshim_gpio_direction_input(&a, KSHIM_GPIO_PULL_KEEP));
    assert(fake_peek(cfg(209)) == (KSHIM_TLMM_CFG_EGPIO_PRESENT | KSHIM_TLMM_CFG_EGPIO_ENABLE));
    assert(!kshim_gpio_free(&a));
    assert(fake_peek(cfg(209)) == (KSHIM_TLMM_CFG_EGPIO_PRESENT | (10U << KSHIM_TLMM_CFG_FUNC_SHIFT)));
    assert(kshim_gpio_set(&a, true) == ERROR_INVALID_PARAMETER); /* not requested */
}

static void pinctrl_states(void *f)
{
    /* Inherited pads before the states. */
    fake_poke(cfg(28), 0x1);              /* gamepad_pwr_en: input, pull-down */
    fake_poke(cfg(161), 0x3);
    fake_poke(cfg(162), 0x3);
    fake_poke(cfg(48), 0x0);
    fake_poke(cfg(49), 0x0);
    fake_poke(cfg(97), 0x0);
    fake_poke(cfg(98), 0x0);
    fake_poke(cfg(99), 0x0);
    uint32_t before[] = {fake_peek(cfg(28)), fake_peek(io(28)), fake_peek(cfg(161)), fake_peek(cfg(162)),
                         fake_peek(cfg(48)), fake_peek(cfg(49)), fake_peek(cfg(98))};

    /* uPD720201 node: gamepad_pwr_en = gpio28 gpio, 2 mA, bias-disable, output-high. */
    int ep = fake_path(f, "/soc@0/pcie@1c08000/pcie@0/usb-controller@0");
    unsigned mark = FakeLogCount;
    assert(!kshim_pinctrl_apply(f, ep, "default"));
    assert(fake_peek(cfg(28)) == KSHIM_TLMM_CFG_OE && fake_peek(io(28)) == KSHIM_TLMM_IO_OUT);
    assert(logged(io(28), mark) >= 0 && logged(cfg(28), mark) > logged(io(28), mark));
    assert(kshim_pinctrl_apply(f, ep, "sleep") == ERROR_NOT_FOUND);

    /* RFC touch states: 161 pull-down 8 mA, 162 bias-disable 8 mA. */
    int ts = fake_path(f, "/soc@0/geniqup@ac0000/i2c@a90000/touchscreen@5d");
    assert(!kshim_pinctrl_apply(f, ts, "default"));
    assert(fake_peek(cfg(161)) == ((3U << KSHIM_TLMM_CFG_DRV_SHIFT) | 1U));
    assert(fake_peek(cfg(162)) == (3U << KSHIM_TLMM_CFG_DRV_SHIFT));

    /* i2c4: gpio48/49 qup1_se4 (mux 1), 2 mA, pull-up. */
    int i2c4 = fake_path(f, "/soc@0/geniqup@ac0000/i2c@a90000");
    assert(!kshim_pinctrl_apply(f, i2c4, "default"));
    assert(fake_peek(cfg(48)) == ((1U << KSHIM_TLMM_CFG_FUNC_SHIFT) | 3U));
    assert(fake_peek(cfg(49)) == ((1U << KSHIM_TLMM_CFG_FUNC_SHIFT) | 3U));

    /* pcie1 default: perst (97) gpio pull-down, clkreq (98) mux 1 pull-up,
     * wake (99) pull-up; subnode form. */
    int pcie1 = fake_path(f, "/soc@0/pcie@1c08000");
    assert(!kshim_pinctrl_apply(f, pcie1, "default"));
    assert(fake_peek(cfg(97)) == 1U && fake_peek(cfg(99)) == 3U);
    assert(fake_peek(cfg(98)) == ((1U << KSHIM_TLMM_CFG_FUNC_SHIFT) | 3U));

    /* Re-applying writes nothing. */
    unsigned writes = FakeWrites;
    assert(!kshim_pinctrl_apply(f, ep, "default") && !kshim_pinctrl_apply(f, ts, "default") &&
           !kshim_pinctrl_apply(f, i2c4, "default") && !kshim_pinctrl_apply(f, pcie1, "default"));
    assert(FakeWrites == writes);

    /* A PMIC GPIO state (gpio-keys volume_up_n) stays inherited. */
    int keys = fake_path(f, "/gpio-keys");
    assert(kshim_pinctrl_apply(f, keys, "default") == ERROR_NOT_SUPPORTED);
    /* A state naming an unknown function is refused before any write. */
    int bad = state_of(f, "upd720201-avdd33-state");
    assert(!fdt_setprop_string(f, bad, "function", "qup1_se4"));
    assert(kshim_pinctrl_apply_state(f, bad) == ERROR_NOT_SUPPORTED);
    assert(!fdt_setprop_string(f, bad, "function", "gpio"));

    /* A consumer reference on a pinctrl pad: free keeps the pinctrl state. */
    struct kshim_gpio g;
    assert(!kshim_gpio_from_tlmm_pin(f, fake_path(f, "/soc@0/pinctrl@f100000"), 161, false, &g));
    assert(!kshim_gpio_request(&g, "touch") && !kshim_gpio_direction_output(&g, false));
    assert(!kshim_gpio_free(&g));
    assert(fake_peek(cfg(161)) & KSHIM_TLMM_CFG_OE);

    /* Handoff: every snapshot back, newest first (pcie1 pads before 28). */
    mark = FakeLogCount;
    assert(!kshim_gpio_restore_all());
    assert(kshim_gpio_snapshot_count() == 0);
    int first_pcie = logged(cfg(98), mark), gamepad = logged(cfg(28), mark);
    assert(first_pcie >= 0 && gamepad > first_pcie);
    /* An input pad drops OE before its level. */
    assert(logged(cfg(28), mark) < logged(io(28), mark));
    uint32_t after[] = {fake_peek(cfg(28)), fake_peek(io(28)), fake_peek(cfg(161)), fake_peek(cfg(162)),
                        fake_peek(cfg(48)), fake_peek(cfg(49)), fake_peek(cfg(98))};
    assert(!memcmp(before, after, sizeof(before)));
    writes = FakeWrites;
    assert(!kshim_gpio_restore_all() && FakeWrites == writes); /* idempotent */
}

/* ---- PMIC GPIO over a byte model ------------------------------------- */

static uint8_t PmicMem[16][0x10000];
static unsigned PmicWrites;
static CR_STATUS pmic_read(VOID *c, UINT8 sid, UINT16 address, UINT8 *buffer, UINTN length)
{
    (void)c;
    assert(length == 1 && sid < 16);
    *buffer = PmicMem[sid][address];
    if ((address & 0xff) == 0x10) *buffer = PmicMem[sid][address & 0xff00]; /* RT_STS mirrors pad */
    return CR_SUCCESS;
}
static CR_STATUS pmic_write(VOID *c, UINT8 sid, UINT16 address, CONST UINT8 *buffer, UINTN length)
{
    (void)c;
    assert(length == 1 && sid < 16);
    unsigned reg = address & 0xff;
    assert((reg >= 0x40 && reg <= 0x46) || reg == 0x4a); /* the transport's whitelist */
    PmicMem[sid][address] = *buffer;
    ++PmicWrites;
    return CR_SUCCESS;
}

static void pmic_gpio(void *f)
{
    static const PmicGpioBusOps bus = {.Read = pmic_read, .Write = pmic_write};
    int key = fake_path(f, "/gpio-keys/key-volume-up");
    struct kshim_gpio g;
    assert(!kshim_gpio_from_dt(f, key, "gpios", 0, &g) && g.chip == KSHIM_GPIO_CHIP_PMIC);
    assert(kshim_gpio_request(&g, "key") == ERROR_NOT_SUPPORTED); /* no transport */
    kshim_gpio_set_pmic_bus(&bus);
    uint8_t *p = &PmicMem[1][0x8d00];
    p[0x04] = 0x10; p[0x05] = 0x12;           /* GPIO, LV_VIN2 subtype */
    p[0x40] = 0x00; p[0x41] = 0x01; p[0x42] = 0x00; p[0x44] = 0x00; p[0x45] = 0x01; p[0x46] = 0x80;
    p[0x4a] = 0x00;
    uint8_t snapshot[0x50];
    memcpy(snapshot, p, sizeof(snapshot));
    p[0x00] = 1; /* pad high: vol+ released (active low) */
    bool pressed;
    assert(!kshim_gpio_get(&g, &pressed) && !pressed);
    p[0x00] = 0;
    assert(!kshim_gpio_get(&g, &pressed) && pressed);
    assert(!kshim_gpio_request(&g, "test"));
    assert(!kshim_gpio_direction_output(&g, false)); /* logical off = high */
    assert((p[0x40] & 3) == 1 && (p[0x44] & 0x80) && p[0x46] == 0x80);
    assert(!kshim_gpio_set(&g, true) && !(p[0x44] & 0x80));
    assert(!kshim_gpio_direction_input(&g, KSHIM_GPIO_PULL_UP));
    assert((p[0x40] & 3) == 0 && p[0x42] == 0);
    assert(kshim_gpio_tlmm_pin(&g, &(uint32_t){0}) == ERROR_NOT_SUPPORTED);
    assert(!kshim_gpio_free(&g));
    p[0x00] = snapshot[0x00];
    assert(!memcmp(p, snapshot, sizeof(snapshot)));
    assert(PmicWrites > 0);
    kshim_gpio_set_pmic_bus(NULL);
}

/* SM8250 Android layout: CrAndroidPin offsets and function names. */
static void kona(void)
{
    char path[1024];
    snprintf(path, sizeof(path), "%s/kona-resources.dtb", GENERATED_FIXTURES);
    FILE *fp = fopen(path, "rb");
    assert(fp);
    static uint8_t blob[65536];
    size_t size = fread(blob, 1, sizeof(blob), fp);
    fclose(fp);
    assert(size > 0 && !dt_validate(blob, size));
    int tlmm = -1;
    for (int n = fdt_next_node(blob, -1, NULL); n >= 0; n = fdt_next_node(blob, n, NULL))
        if (kshim_gpio_tlmm_soc(blob, n) == KSHIM_SOC_SM8250) tlmm = n;
    assert(tlmm >= 0);
    struct kshim_gpio g;
    assert(!kshim_gpio_from_tlmm_pin(blob, tlmm, 39, true, &g) && g.active_low);
    uint32_t offset, mux;
    assert(kshim_soc_pin_offset(KSHIM_SOC_SM8250, 39, &offset));
    struct dt_range reg;
    assert(!dt_reg(blob, tlmm, 0, &reg) && g.ctl == reg.base + offset);
    assert(kshim_soc_pin_function(KSHIM_SOC_SM8250, 36, "qup13", &mux) && mux == 1);
    assert(!kshim_soc_pin_offset(KSHIM_SOC_SM8250, 180, &offset));
}

int main(void)
{
    fake_reset();
    kshim_platform_set_io(&FakeIo);
    void *f = fake_fixture("io/sm8650-ayaneo-pocket-s2.dtb");
    assert(!kshim_gpio_init(f) && !kshim_gpio_init(f));
    tlmm_consumers(f);
    pinctrl_states(f);
    pmic_gpio(f);
    assert(!kshim_gpio_restore_all());
    kona();
    free(f);
    puts("platform_gpio_test: ok");
    return 0;
}
