#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Generate kernel/soc/sm8650_tables.c from a Linux tree.

Only hardware facts are extracted (register offsets, enable bits, mux
assignments, dt-binding IDs); no Linux code is copied. Inputs:

  drivers/pinctrl/qcom/pinctrl-sm8650.c      TLMM mux slots per pad
  drivers/clk/qcom/gcc-sm8650.c              GCC branches, RCGs, GDSCs, BCRs
  drivers/clk/qcom/tcsrcc-sm8650.c           TCSR clkref branches
  include/dt-bindings/clock/qcom,sm8650-gcc.h   GCC clock/reset/GDSC IDs
  include/dt-bindings/clock/qcom,sm8650-tcsr.h  TCSR clock IDs

Usage: tools/gen_sm8650_tables.py <linux-tree> [output.c]
The source hashes and the Linux revision are written into the output.
"""
import hashlib
import os
import re
import subprocess
import sys

SOURCES = {
    "pinctrl": "drivers/pinctrl/qcom/pinctrl-sm8650.c",
    "gcc": "drivers/clk/qcom/gcc-sm8650.c",
    "tcsr": "drivers/clk/qcom/tcsrcc-sm8650.c",
    "gcc_ids": "include/dt-bindings/clock/qcom,sm8650-gcc.h",
    "tcsr_ids": "include/dt-bindings/clock/qcom,sm8650-tcsr.h",
}
NORMAL_PINS = 210

KIND = {"branch": 1, "vote": 2, "gdsc": 3, "reset": 4}


def read(root, key):
    with open(os.path.join(root, SOURCES[key]), encoding="utf-8") as f:
        return f.read()


def ids(text):
    return {m.group(1): int(m.group(2), 0)
            for m in re.finditer(r"^#define\s+(\w+)\s+(\d+)\s*$", text, re.M)}


def number(text):
    return int(text, 0)


def bit(text):
    m = re.fullmatch(r"BIT\((\d+)\)", text.strip())
    return 1 << int(m.group(1)) if m else number(text)


def blocks(text, kind):
    """Yield (name, body) for 'static struct <kind> name = { ... };'."""
    for m in re.finditer(r"static (?:const )?struct %s (\w+) = \{" % kind, text):
        depth, i = 1, m.end()
        while depth:
            depth += {"{": 1, "}": -1}.get(text[i], 0)
            i += 1
        yield m.group(1), text[m.end():i - 1]


def field(body, name):
    m = re.search(r"\.%s = ([^,\n]+)," % re.escape(name), body)
    return m.group(1).strip() if m else None


def pins(text):
    groups = {}
    for m in re.finditer(r"\[(\d+)\] = PINGROUP\(\d+,([^)]*)\)", text):
        pin = int(m.group(1))
        if pin < NORMAL_PINS:
            groups[pin] = [f.strip() for f in m.group(2).split(",")]
    assert sorted(groups) == list(range(NORMAL_PINS)), "missing PINGROUP"
    for funcs in groups.values():
        assert len(funcs) == 10
    names = sorted({f for funcs in groups.values() for f in funcs if f != "_"})
    return groups, names


def branches(text):
    rcgs = {name: number(field(body, "cmd_rcgr")) for name, body in blocks(text, "clk_rcg2")}
    leaves = {}
    for name, body in blocks(text, "clk_branch"):
        enable_reg = number(field(body, "enable_reg"))
        raw_halt = field(body, "halt_reg")
        halt_reg = number(raw_halt) if raw_halt else enable_reg
        check = field(body, "halt_check") or "BRANCH_HALT"
        if not raw_halt:
            check = "BRANCH_HALT_SKIP"
        mask = bit(field(body, "enable_mask"))
        parent = re.search(r"&(\w+)\.clkr\.hw", body)
        rcg = rcgs.get(parent.group(1), 0) if parent else 0
        kind = "branch" if enable_reg == halt_reg and mask == 1 else "vote"
        halt = 1 if check in ("BRANCH_HALT", "BRANCH_HALT_VOTED") else 0
        leaves[name] = (KIND[kind], halt, enable_reg, mask, halt_reg, rcg)
    return leaves


def gdscs(text):
    out = {}
    for name, body in blocks(text, "gdsc"):
        gdscr = number(field(body, "gdscr"))
        ctrl, cmask = field(body, "collapse_ctrl"), field(body, "collapse_mask")
        if ctrl and cmask:
            out[name] = (KIND["gdsc"], 1, number(ctrl), bit(cmask), gdscr, 0)
        else:
            out[name] = (KIND["gdsc"], 1, gdscr, 1, gdscr, 0)
    return out


def table_entries(text, array, pattern):
    m = re.search(r"%s\[\] = \{(.*?)\n\};" % re.escape(array), text, re.S)
    assert m, array
    return re.findall(pattern, m.group(1))


def controller(text, idmap, prefix):
    leaves = branches(text)
    rows = []
    for ident, name in table_entries(text, "%s_clocks" % prefix, r"\[(\w+)\] = &(\w+)\.clkr,"):
        if name in leaves:
            rows.append((idmap[ident], name, leaves[name]))
    rows.sort()
    out = [(0, i, n, v) for i, n, v in rows]
    if "%s_gdscs[]" % prefix in text:
        g = gdscs(text)
        for ident, name in table_entries(text, "%s_gdscs" % prefix, r"\[(\w+)\] = &(\w+),"):
            out.append((1, idmap[ident], name, g[name]))
    if "%s_resets[]" % prefix in text:
        for ident, body in table_entries(text, "%s_resets" % prefix, r"\[(\w+)\] = \{([^}]*)\}"):
            parts = [p.strip() for p in body.split(",") if p.strip()]
            named = dict(p[1:].split(" = ") for p in parts if p.startswith("."))
            if named:
                reg, b = number(named["reg"]), number(named.get("bit", "0"))
            else:
                reg = number(parts[0])
                b = number(parts[1]) if len(parts) > 1 else 0
            out.append((2, idmap[ident], ident.lower(), (KIND["reset"], 0, reg, 1 << b, reg, 0)))
    return out


def emit_leaves(lines, symbol, rows):
    lines.append("const struct kshim_soc_clk %s[] = {" % symbol)
    for space, ident, name, (kind, halt, en, mask, halt_reg, rcg) in rows:
        lines.append('    {"%s", %d, %d, %d, %d, 0x%x, 0x%x, 0x%x, 0x%x},' %
                     (name, space, ident, kind, halt, en, mask, halt_reg, rcg))
    lines.append("};")
    lines.append("const size_t %s_count = sizeof(%s) / sizeof(%s[0]);" % (symbol, symbol, symbol))


def main():
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    root = sys.argv[1]
    out = sys.argv[2] if len(sys.argv) > 2 else os.path.join(
        os.path.dirname(__file__), "..", "kernel", "soc", "sm8650_tables.c")
    texts = {k: read(root, k) for k in SOURCES}
    rev = subprocess.run(["git", "-C", root, "rev-parse", "HEAD"], capture_output=True,
                         text=True).stdout.strip() or "unknown"
    groups, names = pins(texts["pinctrl"])
    gcc = controller(texts["gcc"], ids(texts["gcc_ids"]), "gcc_sm8650")
    tcsr = controller(texts["tcsr"], ids(texts["tcsr_ids"]), "tcsr_cc_sm8650")

    lines = ["/* SPDX-License-Identifier: MIT */", "/*",
             " * SM8650 hardware facts generated by tools/gen_sm8650_tables.py. Do not edit.",
             " * Register offsets, enable bits, mux slot assignments and dt-binding IDs",
             " * extracted from Linux %s (facts only, no code):" % rev]
    for key, path in SOURCES.items():
        digest = hashlib.sha256(texts[key].encode()).hexdigest()
        lines.append(" *   %s sha256 %s" % (path, digest))
    lines += [" * TLMM: mux slot 0 is \"gpio\"; slots 1..10 index kshim_sm8650_pin_names",
              " * (0 = no function). Clock rows: name, id space (0 clock, 1 GDSC, 2 reset),",
              " * dt-binding id, kind (1 branch, 2 vote, 3 GDSC, 4 BCR), halt check",
              " * (1 = CLK_OFF/PWR_ON bit 31), enable register and mask, halt register",
              " * and the CMD_RCGR of the parent RCG (0 = none).", " */",
              '#include <kshim/soc_tables.h>', "",
              "const char *const kshim_sm8650_pin_names[] = {", '    NULL,']
    lines += ['    "%s",' % n for n in names]
    lines += ["};", "const uint8_t kshim_sm8650_pin_mux[%d][10] = {" % NORMAL_PINS]
    index = {n: i + 1 for i, n in enumerate(names)}
    for pin in range(NORMAL_PINS):
        slots = ", ".join(str(index.get(f, 0)) for f in groups[pin])
        lines.append("    {%s}, /* gpio%d */" % (slots, pin))
    lines += ["};", "const uint16_t kshim_sm8650_pin_name_count = %d;" % (len(names) + 1), ""]
    emit_leaves(lines, "kshim_sm8650_gcc", gcc)
    lines.append("")
    emit_leaves(lines, "kshim_sm8650_tcsr", tcsr)
    with open(out, "w", encoding="utf-8") as f:
        f.write("\n".join(lines) + "\n")
    print("%s: %d pins, %d functions, %d GCC rows, %d TCSR rows" %
          (out, NORMAL_PINS, len(names), len(gcc), len(tcsr)))


if __name__ == "__main__":
    main()
