# Goodix touch fixtures

Used by `tests/io/touch_goodix_dt_test.c` (owner: touch-goodix). The DTB
sources are BSD-3-Clause Linux DTS files; the blobs are build products.

| File | What | sha256 |
| --- | --- | --- |
| `sm8650-qrd.dtb` | mainline `arch/arm64/boot/dts/qcom/sm8650-qrd.dts` | `3a16f452a024fd885827cb5195148135dfb27b567a48b4acda72863bcbd799e9` |
| `sm8650-ayaneo-pocket-s2.dtb` | mainline `arch/arm64/boot/dts/qcom/sm8650-ayaneo-pocket-s2.dts` | `c669ef76fc0692162958db3f521a7b3b2a6e23d3bca2a29fe832af310d864ac6` |
| `pocket-s2-touch.dtso` | Pocket S2 touch node and pinctrl states from the RFC "add display nodes" patch, as an overlay (MIT; DT values only) | `c22f5aa3b1dbf2892a5bc41c0f28faa0f9fc0e0d095a4c7d56c1bd8531e8f6b4` |

## How the DTBs were made

Linux tree `/home/sunf/mu_aloha_platforms/linux` at
`40288c9206c17eb66a603262e06a58d300d0f279` (2026-09-18), DTC 1.7.0. `-@`
keeps `__symbols__` so the overlay can resolve `&i2c4`, `&tlmm` and
`&vreg_l14b_3p2`.

```sh
L=/home/sunf/mu_aloha_platforms/linux
for dts in sm8650-qrd sm8650-ayaneo-pocket-s2; do
  cpp -nostdinc -undef -D__DTS__ -x assembler-with-cpp \
      -I $L/include -I $L/arch/arm64/boot/dts -I $L/arch/arm64/boot/dts/qcom \
      $L/arch/arm64/boot/dts/qcom/$dts.dts |
    dtc -q -@ -I dts -O dtb -o $dts.dtb -
done
```

Source hashes: `sm8650-qrd.dts`
`a069075b57c23fe88f786deaa3c35d535cf3f77121a9f364214ceeb628857684`,
`sm8650-ayaneo-pocket-s2.dts`
`44e838b69efbb9bfdc39fba68a3e12b1eedce54fef4061a83f2b545e1f3c475b`,
`sm8650.dtsi` `8c1bf591b95d22a4d120351d4d720713b38236629564dde92613aea6852a3b2a`.

## The overlay

`pocket-s2-touch.dtso` transcribes the `&i2c4` / `touchscreen@5d` node and
the `ts_irq_default` / `ts_reset_default` states of "[PATCH RFC] arm64: dts:
qcom: sm8650-ayaneo-pocket-s2: add display nodes"
(<https://lkml.iu.edu/hypermail/linux/kernel/2604.3/08167.html>). The node is
unchanged up to v5 (<https://lkml.iu.edu/2607.3/07518.html>), which is not in
mainline as of the tree above. The build compiles it with `dtc -@`
(`tests/io/touch_goodix.cmake`), and the test applies it with libfdt
`fdt_overlay_apply()`. The same merge on the command line:

```sh
dtc -@ -I dts -O dtb -o pocket-s2-touch.dtbo pocket-s2-touch.dtso
fdtoverlay -i sm8650-ayaneo-pocket-s2.dtb -o pocket-s2+touch.dtb pocket-s2-touch.dtbo
```

The sm8650 implementer owns `tests/fixtures/io/sm8650-*`. These copies keep
the Goodix tests independent of that branch. After both merge, the Goodix
test can switch to those fixtures if they are built with `-@`.
