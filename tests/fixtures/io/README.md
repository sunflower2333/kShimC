# I/O stack fixtures

## SM8650 (owner: sm8650)

| File | Source |
| --- | --- |
| `sm8650-qrd.dtb` | Linux `arch/arm64/boot/dts/qcom/sm8650-qrd.dts` (BSD-3-Clause) |
| `sm8650-ayaneo-pocket-s2.dtb` | `sm8650-ayaneo-pocket-s2-rfc-touch.dts` here: Linux `sm8650-ayaneo-pocket-s2.dts` plus the touch node of the RFC "add display nodes" patch (lkml 2604.3/08167) |

Generated with `tools/gen_sm8650_fixtures.sh <linux-tree>` from Linux
`40288c9206c17eb66a603262e06a58d300d0f279` (2026-09) with DTC 1.7.0:

```sh
cpp -nostdinc -undef -D__DTS__ -x assembler-with-cpp \
    -I <linux>/include -I <linux>/arch/arm64/boot/dts -I <linux>/arch/arm64/boot/dts/qcom \
    <dts> | dtc -q -I dts -O dtb -o <out> -
```

Hashes are in `UPSTREAM.json`. The QRD blob is byte-identical to the one CrDK
`Target/Sm8650/Sm8650ClockSourceFacts.json` records as `dt_sha256`.

The RFC touch node is restated from the facts listed in `docs/io-stack.md`
section 5 (GT911 at 0x5d on `&i2c4` at 400 kHz, IRQ TLMM 162 falling, reset
TLMM 161 active-high as written in the RFC, VDDIO/AVDD28 from
`vreg_l14b_3p2`, 1440x2560, `ts_reset_default`/`ts_irq_default`). The
reset polarity is unverified on hardware.

Neither blob is a hardware capture: `memory@a0000000` has a zero size (the
bootloader fills it) and nothing here proves board wiring.

## Licence of the compiled blobs

The DTBs here and in `qup/` and `goodix/` are binary builds of BSD-3-Clause
Linux DTS files. `LICENSE.BSD-3-Clause` reproduces the copyright notices,
the conditions and the disclaimer that a binary redistribution must carry,
and lists the covered blobs; each blob has a REUSE-style `<file>.license`
sidecar. `sm8650-ayaneo-pocket-s2.dtb` also contains the RFC touch node
restated in the MIT `sm8650-ayaneo-pocket-s2-rfc-touch.dts`.

The QUP tests use the two SM8650 blobs here; their former byte-identical
copies under `qup/` were removed.

