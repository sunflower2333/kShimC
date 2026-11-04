#!/bin/sh
# SPDX-License-Identifier: MIT
#
# Regenerate the SM8650 DTB fixtures under tests/fixtures/io/ from a mainline
# Linux tree (BSD-3-Clause DTS sources). Usage:
#   tools/gen_sm8650_fixtures.sh <linux-tree> [output-dir]
# The fixtures in the tree were generated from Linux
# 40288c9206c17eb66a603262e06a58d300d0f279 (2026-09); see
# tests/fixtures/io/README.md and UPSTREAM.json there.
set -eu
linux=${1:?usage: $0 <linux-tree> [output-dir]}
root=$(cd "$(dirname "$0")/.." && pwd)
out=${2:-$root/tests/fixtures/io}
qcom="$linux/arch/arm64/boot/dts/qcom"

build() { # <dts> <dtb>
    cpp -nostdinc -undef -D__DTS__ -x assembler-with-cpp \
        -I "$linux/include" -I "$linux/arch/arm64/boot/dts" -I "$qcom" "$1" |
        dtc -q -I dts -O dtb -o "$2" -
}

build "$qcom/sm8650-qrd.dts" "$out/sm8650-qrd.dtb"
build "$root/tests/fixtures/io/sm8650-ayaneo-pocket-s2-rfc-touch.dts" \
    "$out/sm8650-ayaneo-pocket-s2.dtb"
for f in sm8650-qrd.dtb sm8650-ayaneo-pocket-s2.dtb; do
    printf '%s  %s\n' "$(sha256sum "$out/$f" | cut -d' ' -f1)" "$f"
done
