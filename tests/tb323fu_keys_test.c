/* tests/tb323fu_keys_test.c - red test: tb323fu SPMI regions + key descriptors */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <qcom_dt.h>
#include <qcom_keys_fdt.h>

#ifndef FIXTURE_PATH
#error "FIXTURE_PATH must be defined"
#endif
static void *load_fixture(void)
{
    FILE *fp = fopen(FIXTURE_PATH, "rb");
    assert(fp);
    fseek(fp, 0, SEEK_END);
    long n = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    assert(n > 0);
    size_t total = (size_t)n + 4096;
    void *buf = malloc(total);
    assert(buf);
    assert(fread(buf, 1, (size_t)n, fp) == (size_t)n);
    fclose(fp);
    assert(fdt_open_into(buf, buf, (int)total) == 0);
    return buf;
}
static struct kshim_resources *new_resources(void)
{
    struct kshim_resources *r = calloc(1, sizeof(*r));
    assert(r);
    return r;
}
static void test_positive(void)
{
    void *fdt = load_fixture();
    struct kshim_resources *r = new_resources();
    assert(kshim_dt_spmi(fdt, r) == 0 && r->spmi_count == 1);
    const uint32_t exp[7] = { 0xc400000u, 0xc900000u, 0xc4c0000u, 0xc8c0000u,
                              0xc426000u, 0xc403000u, 0xc42a000u };
    for (int i = 0; i < 7; i++)
        assert(r->spmi[0].controller.Regions[i].BaseAddress == exp[i]);
    int pon = fdt_path_offset(fdt, "/arbiter@c400000/spmi@c426000/pmk8850@0/pon_hlos@1300");
    uint8_t ctrl = 0xff, bus = 0xff, sid = 0xff;
    uint16_t addr = 0xffff;
    assert(pon >= 0);
    assert(kshim_dt_spmi_identity(fdt, pon, &ctrl, &bus, &sid) == 0);
    assert(ctrl == 0 && bus == 0 && sid == 0);
    assert(kshim_dt_spmi_reg(fdt, pon, &addr) == 0 && addr == 0x1300);
    QcomKeyDescriptor keys[QCOM_KEYS_MAX];
    size_t count = 0;
    assert(QcomKeysBuildFromDeviceTree(fdt, keys, QCOM_KEYS_MAX, &count) == 0);
    assert(count == 3);
    const QcomKeyDescriptor *up = NULL, *down = NULL, *sel = NULL;
    for (size_t i = 0; i < count; i++) {
        if (keys[i].Action == QCOM_KEY_ACTION_UP) up = &keys[i];
        else if (keys[i].Action == QCOM_KEY_ACTION_DOWN) down = &keys[i];
        else if (keys[i].Action == QCOM_KEY_ACTION_SELECT) sel = &keys[i];
    }
    assert(up && down && sel);
    assert((uint64_t)up->Base + (uint64_t)up->RegisterOffset == 0xf065004u);
    assert(QCOM_KEY_IS_SPMI_TOKEN(sel->Base) && QCOM_KEY_SPMI_TOKEN_ADDRESS(sel->Base) == 0x1300);
    assert(QCOM_KEY_SPMI_TOKEN_SID(sel->Base) == 0 && sel->RegisterOffset == 0x10 && sel->Mask == 128);
    assert(QCOM_KEY_IS_SPMI_TOKEN(down->Base) && QCOM_KEY_SPMI_TOKEN_ADDRESS(down->Base) == 0x1300);
    assert(QCOM_KEY_SPMI_TOKEN_SID(down->Base) == 0 && down->RegisterOffset == 0x10 && down->Mask == 64);
    free(r);
    free(fdt);
}
static void test_negative_disabled(void)
{
    void *fdt = load_fixture();
    int arb = fdt_path_offset(fdt, "/arbiter@c400000");
    struct kshim_resources *r = new_resources();
    assert(arb >= 0);
    assert(fdt_setprop_string(fdt, arb, "status", "disabled") == 0);
    assert(kshim_dt_spmi(fdt, r) != 0);
    free(r);
    free(fdt);
    fdt = load_fixture();
    int spmi = fdt_path_offset(fdt, "/arbiter@c400000/spmi@c426000");
    r = new_resources();
    assert(spmi >= 0);
    assert(fdt_setprop_string(fdt, spmi, "status", "disabled") == 0);
    assert(kshim_dt_spmi(fdt, r) != 0);
    free(r);
    free(fdt);
}
static void test_negative_regnames(void)
{
    void *fdt = load_fixture();
    int arb = fdt_path_offset(fdt, "/arbiter@c400000");
    static const char bad[] = "missing\0chnls\0obsrvr\0chnl_map";
    assert(arb >= 0);
    assert(fdt_setprop(fdt, arb, "reg-names", bad, sizeof(bad)) == 0);
    struct kshim_resources *r = new_resources();
    assert(kshim_dt_spmi(fdt, r) != 0);
    free(r);
    free(fdt);
}
static void test_negative_pmic_reg(void)
{
    void *fdt = load_fixture();
    int pmic = fdt_path_offset(fdt, "/arbiter@c400000/spmi@c426000/pmk8850@0");
    int pon = fdt_path_offset(fdt, "/arbiter@c400000/spmi@c426000/pmk8850@0/pon_hlos@1300");
    fdt32_t reg[2] = { cpu_to_fdt32(16), 0 };
    uint8_t ctrl = 0xff, bus = 0xff, sid = 0xff;
    assert(pmic >= 0 && pon >= 0);
    assert(fdt_setprop(fdt, pmic, "reg", reg, sizeof(reg)) == 0);
    assert(kshim_dt_spmi_identity(fdt, pon, &ctrl, &bus, &sid) != 0);
    free(fdt);
}
static int fixture_up_present(uint32_t pin)
{
    void *fdt = load_fixture();
    int vol = fdt_path_offset(fdt, "/gpio_keys/vol_up");
    int has = 0, rc;
    size_t count = 0;
    QcomKeyDescriptor keys[QCOM_KEYS_MAX];
    assert(vol >= 0);
    const fdt32_t *gpios = fdt_getprop(fdt, vol, "gpios", NULL);
    assert(gpios);
    assert(fdt32_to_cpu(gpios[1]) == 101 && fdt32_to_cpu(gpios[2]) == 1);
    fdt32_t cells[3] = { gpios[0], cpu_to_fdt32(pin), gpios[2] };
    assert(fdt_setprop(fdt, vol, "gpios", cells, sizeof(cells)) == 0);
    rc = QcomKeysBuildFromDeviceTree(fdt, keys, QCOM_KEYS_MAX, &count);
    (void)rc; /* success with only the 2 PMIC keys is fine here */
    for (size_t i = 0; i < count; i++)
        if (keys[i].Action == QCOM_KEY_ACTION_UP)
            has = 1;
    free(fdt);
    return has;
}
static void test_gpio_boundary(void)
{
    assert(fixture_up_present(216) == 1);
    assert(fixture_up_present(217) == 0);
    assert(fixture_up_present(218) == 0);
}
int main(void)
{
    test_positive();
    test_negative_disabled();
    test_negative_regnames();
    test_negative_pmic_reg();
    test_gpio_boundary();
    printf("tb323fu keys test: PASS\n");
    return 0;
}
