#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Fail on raw volatile MMIO dereferences outside kernel/include/mmio.h.

A trapping hypervisor (the SM8450 vGIC) can only emulate a single LDR/STR
without writeback; a volatile dereference lets the compiler emit post-indexed,
pair or vector forms. Device register accesses must go through
kshim_mmio_read*/kshim_mmio_write* (or the REG*_ macros built on them).
"""
import re
import sys
from pathlib import Path

root = Path(sys.argv[1] if len(sys.argv) > 1 else Path(__file__).resolve().parents[1])
pattern = re.compile(r"\*\s*\(\s*volatile\s+(?:const\s+)?(?:uint(?:8|16|32|64)_t|UINT(?:8|16|32|64))\s*\*\s*\)")
allowed = {root / "kernel/include/mmio.h"}
bad = []
for directory in ("kernel", "ports/crdk"):
    for path in sorted((root / directory).rglob("*")):
        if path.suffix not in (".c", ".h") or path in allowed:
            continue
        for number, line in enumerate(path.read_text(errors="replace").splitlines(), 1):
            if pattern.search(line):
                bad.append(f"{path.relative_to(root)}:{number}: {line.strip()}")
if bad:
    print("raw volatile MMIO dereference (use kshim_mmio_* from mmio.h):")
    print("\n".join(bad))
    sys.exit(1)
print("mmio_lint: ok")
