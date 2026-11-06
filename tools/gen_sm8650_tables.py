#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Generate kernel/soc/sm8650_tables.c from a Linux tree.

Only hardware facts are extracted (register offsets, enable bits, mux
assignments, dt-binding IDs); no Linux code is copied. Inputs:

  drivers/pinctrl/qcom/pinctrl-sm8650.c      TLMM mux slots per pad
  drivers/clk/qcom/gcc-sm8650.c              GCC branches, RCGs, muxes, GDSCs,
                                             BCRs, GPLL0 and RCG frequency tables
  drivers/clk/qcom/clk-alpha-pll.c           Lucid OLE PLL register layout
  drivers/clk/qcom/clk-regmap-phy-mux.c      PHY mux field and source values
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
    "alpha_pll": "drivers/clk/qcom/clk-alpha-pll.c",
    "phy_mux": "drivers/clk/qcom/clk-regmap-phy-mux.c",
    "tcsr": "drivers/clk/qcom/tcsrcc-sm8650.c",
    "gcc_ids": "include/dt-bindings/clock/qcom,sm8650-gcc.h",
    "tcsr_ids": "include/dt-bindings/clock/qcom,sm8650-tcsr.h",
}
NORMAL_PINS = 210

KIND = {"branch": 1, "vote": 2, "gdsc": 3, "reset": 4, "mux": 5}
# RCGs that get rate rows (the QMP PCIe PHY rchng clocks: sm8650.dtsi assigns
# GCC_PCIE_n_PHY_RCHNG_CLK 100 MHz, which CXO cannot give).
RATE_RCGS = ["gcc_pcie_0_phy_rchng_clk_src", "gcc_pcie_1_phy_rchng_clk_src"]


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


def genmask(shift, width):
    return ((1 << width) - 1) << shift


def muxes(text, phy_mux_text):
    """clk_regmap_mux (.reg/.shift/.width) and clk_regmap_phy_mux (.reg with
    the PHY_MUX_MASK field of clk-regmap-phy-mux.c)."""
    m = re.search(r"#define PHY_MUX_MASK\s+GENMASK\((\d+),\s*(\d+)\)", phy_mux_text)
    assert m, "PHY_MUX_MASK"
    phy_mask = genmask(int(m.group(2)), int(m.group(1)) - int(m.group(2)) + 1)
    out = {}
    for name, body in blocks(text, "clk_regmap_mux"):
        reg = number(field(body, "reg"))
        mask = genmask(number(field(body, "shift")), number(field(body, "width")))
        out[name] = (KIND["mux"], 0, reg, mask, reg, 0)
    for name, body in blocks(text, "clk_regmap_phy_mux"):
        reg = number(field(body, "reg"))
        out[name] = (KIND["mux"], 0, reg, phy_mask, reg, 0)
    return out


def gpll0_even_checks(text, pll_text):
    """Register checks showing GPLL0's even output runs and stays on: the
    APPS vote bit of gcc_gpll0 (kShimC adds no vote of its own), PLL_MODE
    LOCK_DET (what Linux waits for after voting a Lucid EVO/OLE PLL), and in
    PLL_USER_CTL the even output enable (bit 1 of PLL_OUT_MASK's main/even/odd
    outputs; CrDK ClockLib fixed_pll.c, MIT, reads the same Lucid OLE USER
    register with MAIN at bit 0) with the post divider the GCC table
    expects."""
    pll = dict(blocks(text, "clk_alpha_pll"))["gcc_gpll0"]
    even = dict(blocks(text, "clk_alpha_pll_postdiv"))["gcc_gpll0_out_even"]
    assert "CLK_ALPHA_PLL_TYPE_LUCID_OLE" in field(pll, "regs")
    offset = number(field(pll, "offset"))
    m = re.search(r"\[CLK_ALPHA_PLL_TYPE_LUCID_OLE\] = \{(.*?)\}", pll_text, re.S)
    user_ctl = number(re.search(r"\[PLL_OFF_USER_CTL\] = (0x[0-9a-f]+)", m.group(1)).group(1))
    mode = bit(re.search(r"#\s*define PLL_LOCK_DET\s+(BIT\(\d+\))", pll_text).group(1))
    vote_reg, vote_mask = number(field(pll, "enable_reg")), bit(field(pll, "enable_mask"))
    assert number(re.search(r"#define PLL_OUT_MASK\s+(0x[0-9a-f]+)", pll_text).group(1)) == 0x7
    shift, width = number(field(even, "post_div_shift")), number(field(even, "width"))
    table = re.search(r"%s\[\] = \{\s*\{ (0x[0-9a-f]+), (\d+) \}," % field(even, "post_div_table"), text)
    even_out = 1 << 1
    user_mask = genmask(shift, width) | even_out
    user_value = (number(table.group(1)) << shift) | even_out
    return [(vote_reg, vote_mask, vote_mask), (offset, mode, mode),
            (offset + user_ctl, user_mask, user_value)], \
        int(table.group(2))


def rates(text, pll_text):
    checks, even_div = gpll0_even_checks(text, pll_text)
    rows = []
    rcgs = dict(blocks(text, "clk_rcg2"))
    for name in RATE_RCGS:
        body = rcgs[name]
        assert number(field(body, "mnd_width")) == 0
        pmap = dict((p, int(v)) for p, v in table_entries(
            text, field(body, "parent_map"), r"\{ (\w+), (\d+) \}"))
        for hz, src, div, m, n in table_entries(
                text, field(body, "freq_tbl"),
                r"F\((\d+), (\w+), ([\d.]+), (\d+), (\d+)\)"):
            if src != "P_GCC_GPLL0_OUT_EVEN" or int(m) or int(n):
                continue
            assert int(hz) == 600000000 // even_div // float(div)
            rows.append((number(field(body, "cmd_rcgr")), int(hz), pmap[src],
                         int(2 * float(div) - 1), "gcc_gpll0_out_even", checks))
    return rows


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


def controller(text, idmap, prefix, extra=None):
    leaves = branches(text)
    leaves.update(extra or {})
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


def emit_rates(lines, symbol, rows):
    lines.append("const struct kshim_soc_clk_rate %s[] = {" % symbol)
    for rcg, hz, src, div, parent, checks in rows:
        pll = ", ".join("{0x%x, 0x%x, 0x%x}" % c for c in checks)
        lines.append('    {0x%x, %d, %d, %d, "%s", {%s}},' % (rcg, hz, src, div, parent, pll))
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
    gcc = controller(texts["gcc"], ids(texts["gcc_ids"]), "gcc_sm8650",
                     muxes(texts["gcc"], texts["phy_mux"]))
    gcc_rates = rates(texts["gcc"], texts["alpha_pll"])
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
              " * dt-binding id, kind (1 branch, 2 vote, 3 GDSC, 4 BCR, 5 mux), halt check",
              " * (1 = CLK_OFF/PWR_ON bit 31), enable register and mask (a mux: its",
              " * register and source field), halt register and the CMD_RCGR of the",
              " * parent RCG (0 = none). Rate rows: CMD_RCGR, Hz, CFG_RCGR SRC_SEL and",
              " * SRC_DIV (2 x div - 1), the parent PLL output and the register checks",
              " * (offset, mask, value) that show it runs.", " */",
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
    emit_rates(lines, "kshim_sm8650_gcc_rates", gcc_rates)
    lines.append("")
    emit_leaves(lines, "kshim_sm8650_tcsr", tcsr)
    with open(out, "w", encoding="utf-8") as f:
        f.write("\n".join(lines) + "\n")
    print("%s: %d pins, %d functions, %d GCC rows, %d GCC rates, %d TCSR rows" %
          (out, NORMAL_PINS, len(names), len(gcc), len(gcc_rates), len(tcsr)))


if __name__ == "__main__":
    main()
