# QUP/GENI discovery fixtures

Inputs of the `qup_dt_test` host test (owner: qup, `docs/io-stack.md`).

| File | Source |
| --- | --- |
| `sm8650-qrd.dtb` | Linux mainline `arch/arm64/boot/dts/qcom/sm8650-qrd.dts`, unmodified |
| `sm8650-ayaneo-pocket-s2-touch.dtb` | `sm8650-ayaneo-pocket-s2-touch.dts` here: mainline `sm8650-ayaneo-pocket-s2.dts` plus the `&i2c4` GT911 node and its two TLMM states from the RFC patch "sm8650-ayaneo-pocket-s2: add display nodes" (lkml 2604.3/08167) |
| `sm8250-hdk.dtb` | Linux mainline `sm8250-hdk.dts`, unmodified |
| `sm8150-hdk.dtb` | Linux mainline `sm8150-hdk.dts`, unmodified |
| `kona-qup.dts` | Focused Android-form SM8250 fixture written from the facts of the pinned OnePlus kona sources (see its header); compiled by the test build |

The Android SM8150 blobs (`andromeda-fdt.dtb`, `hdk8150-fdt.dtb`) are the
existing fixtures in `tests/fixtures/`.

Mainline tree: `/home/sunf/mu_aloha_platforms/linux`, commit
`40288c9206c17eb66a603262e06a58d300d0f279` (2026-09-18). The qcom DTS files
are BSD-3-Clause; only their compiled facts are used here.

```sh
L=/home/sunf/mu_aloha_platforms/linux
Q=$L/arch/arm64/boot/dts/qcom
build() {
  cpp -nostdinc -undef -D__DTS__ -x assembler-with-cpp \
      -I $L/include -I $L/arch/arm64/boot/dts -I $Q "$1" |
  dtc -q -I dts -O dtb -o "$2" -
}
build $Q/sm8650-qrd.dts sm8650-qrd.dtb
build sm8650-ayaneo-pocket-s2-touch.dts sm8650-ayaneo-pocket-s2-touch.dtb
build $Q/sm8250-hdk.dts sm8250-hdk.dtb
build $Q/sm8150-hdk.dts sm8150-hdk.dtb
```

SHA-256 of the generated blobs:

```
01c94cf491fa9f5b265fc087b68ae0aea2565b43bcbd8d169b9f463e897ad6bb  sm8650-qrd.dtb
7f35daf356d8adee29ba78b70b11b7cdd71dc176b72bedafbe97b566251d58ea  sm8650-ayaneo-pocket-s2-touch.dtb
23c0ac5bd125622266f112233dfd67757cfcd585f7489792630c014d825a98ce  sm8250-hdk.dtb
6966d93ba0896d4f1465d72d6379af35832689b9921054b23e644b32b8a0136f  sm8150-hdk.dtb
```

The RFC's `reset-gpios = <&tlmm 161 GPIO_ACTIVE_HIGH>` is reproduced as
written; its polarity is an open question for the touch-goodix owner.
