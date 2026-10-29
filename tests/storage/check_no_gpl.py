#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Fail if GPL-2.0-or-later FirmwareLoaderLib code (NhErofs.c) is compiled
into, archived in, or linked into an artifact that must stay MIT.

  --compile-commands F  every translation unit of NhErofs.c must belong to an
                        --allow-target (CMake object dir CMakeFiles/<t>.dir)
  --archive A           no member of static library A may come from NhErofs
  --elf E               linked image E (built with -g) must not reference the
                        NhErofs.c compilation unit; requires debug info
"""
import argparse
import json
import subprocess
import sys

GPL_SOURCES = ("NhErofs.c",)


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--compile-commands")
    p.add_argument("--archive", action="append", default=[])
    p.add_argument("--elf", action="append", default=[])
    p.add_argument("--allow-target", action="append", default=[])
    p.add_argument("--ar", default="ar")
    a = p.parse_args()
    problems, checked = [], []

    if a.compile_commands:
        units = json.load(open(a.compile_commands))
        for unit in units:
            if not unit["file"].endswith(GPL_SOURCES):
                continue
            command = unit.get("command") or " ".join(unit.get("arguments", []))
            output = unit.get("output", "") + " " + command
            if not any(f"CMakeFiles/{t}.dir/" in output for t in a.allow_target):
                problems.append(f"compile_commands: GPL source compiled: {unit['file']}")
        checked.append(f"{len(units)} translation units")

    for archive in a.archive:
        members = subprocess.run([a.ar, "t", archive], check=True, capture_output=True,
                                 text=True).stdout.split()
        bad = [m for m in members if m.startswith("NhErofs")]
        if bad:
            problems.append(f"{archive}: GPL members {bad}")
        checked.append(f"{archive}: {len(members)} members")

    for elf in a.elf:
        data = open(elf, "rb").read()
        if b".debug_info" not in data and b".debug_str" not in data:
            problems.append(f"{elf}: no debug info, cannot audit")
        if b"NhErofs.c" in data:
            problems.append(f"{elf}: contains the NhErofs.c compilation unit")
        checked.append(f"{elf}: {len(data)} bytes")

    for line in checked:
        print("checked", line)
    if problems:
        print("\n".join(problems), file=sys.stderr)
        return 1
    print("PASS no GPL-2.0 FirmwareLoaderLib code in MIT artifacts")
    return 0


if __name__ == "__main__":
    sys.exit(main())
