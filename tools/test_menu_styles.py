#!/usr/bin/env python3
"""编译六种菜单预设，检查兼容默认值、颜色对比度与非法多选配置。"""
from __future__ import annotations

import argparse
import itertools
import os
from pathlib import Path
import shlex
import subprocess
import tempfile

STYLES = ("FLAT", "CLASSIC", "CARDS", "TERMINAL", "MINIMAL", "HIGH_CONTRAST")
ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / "tests/menu_style_tokens_test.c"
FIELDS = ("screen", "panel", "text", "muted", "item", "focus", "focus_text",
          "boot", "boot_pressed", "boot_text", "border")


# 执行命令并保留完整编译诊断，防止负向测试掩盖无关编译错误。
def run(command: list[str]) -> subprocess.CompletedProcess[str]:
    return subprocess.run(command, text=True, capture_output=True, timeout=45, check=False)


# 根据 sRGB 分量计算相对亮度。
def luminance(color: int) -> float:
    components = [((color >> shift) & 255) / 255.0 for shift in (16, 8, 0)]
    linear = [v / 12.92 if v <= 0.04045 else ((v + 0.055) / 1.055) ** 2.4
              for v in components]
    return sum(a * b for a, b in zip(linear, (0.2126, 0.7152, 0.0722)))


# 计算两个不透明颜色的对比度。
def contrast(first: int, second: int) -> float:
    low, high = sorted((luminance(first), luminance(second)))
    return (high + 0.05) / (low + 0.05)


# 编译并运行一次真实的预处理配置，返回编译结果中的预设数据。
def check_preset(
    compiler: list[str],
    temporary: Path,
    expected: str,
    flags: list[str],
) -> dict[str, int]:
    executable = temporary / "menu-style-test"
    command = compiler + ["-std=c11", "-Wall", "-Wextra", "-Werror", "-pedantic",
                          "-UNDEBUG", *flags, str(SOURCE), "-o", str(executable)]
    result = run(command)
    if result.returncode:
        raise RuntimeError(f"{expected} compile failed:\n{result.stdout}{result.stderr}")
    result = run([str(executable)])
    if result.returncode:
        raise RuntimeError(f"{expected} contract failed:\n{result.stdout}{result.stderr}")
    values = result.stdout.split()
    if len(values) != len(FIELDS) + 1 or values[0] != expected:
        raise RuntimeError(f"Expected {expected}, got {result.stdout!r}")
    return dict(zip(FIELDS, (int(v, 16) for v in values[1:])))


# 验证真实 Kconfig choice、旧配置默认值、生成头文件与禁用 LVGL 的行为。
def check_kconfig(compiler: list[str], temporary: Path) -> None:
    try:
        import kconfiglib
    except ImportError as error:
        raise RuntimeError("--kconfig requires: python -m pip install kconfiglib") from error
    os.environ.update(srctree=str(ROOT / "src"), HOST_ARCH="x86_64", ARCH="aarch64",
                      HOST_INFO="menu-style-test", BUILD_TIME="test", VERSION="test")
    config = kconfiglib.Kconfig(str(ROOT / "Kconfig"))
    choice = config.named_choices["KSHIM_MENU_STYLE"]
    if choice.selection.name != "KSHIM_MENU_STYLE_FLAT":
        raise RuntimeError("Old/default configuration no longer selects FLAT")
    header = temporary / "config.h"
    for name in STYLES:
        config.syms[f"KSHIM_MENU_STYLE_{name}"].set_value(2)
        selected = [n for n in STYLES if config.syms[f"KSHIM_MENU_STYLE_{n}"].tri_value == 2]
        if selected != [name]:
            raise RuntimeError(f"Kconfig selection is not one-hot: {selected}")
        config.write_autoconf(str(header))
        check_preset(compiler, temporary, name, ["-include", str(header)])
    config.syms["KSHIM_LVGL"].set_value(0)
    if choice.selection is not None or any(config.syms[f"KSHIM_MENU_STYLE_{n}"].tri_value
                                          for n in STYLES):
        raise RuntimeError("Style choice remains enabled without LVGL")
    print("PASS: Kconfig default, six generated headers, one-hot selection and LVGL-off")


# 运行完整的轻量预设矩阵；实际 LVGL 渲染测试由 tests/CMakeLists.txt 提供。
def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cc", default=os.environ.get("CC", "cc"))
    parser.add_argument("--kconfig", action="store_true")
    args = parser.parse_args()
    compiler = shlex.split(args.cc)
    if not compiler:
        parser.error("--cc must identify a C compiler")
    try:
        with tempfile.TemporaryDirectory(prefix="kshim-menu-styles-") as directory:
            temporary = Path(directory)
            default = check_preset(compiler, temporary, "FLAT", [])
            palettes = []
            for name in STYLES:
                colors = check_preset(compiler, temporary, name,
                                      [f"-DCONFIG_KSHIM_MENU_STYLE_{name}=1"])
                palettes.append(tuple(colors.values()))
                if name == "FLAT":
                    if colors != default:
                        raise RuntimeError("Implicit and explicit FLAT differ")
                    expected = (0x13235a, 0x161c3e, 0xffffff, 0xc9d1ee, 0xffffff,
                                0x2f6bf0, 0xffffff, 0xf4f7ff, 0xdfe5fb, 0x161c3e, 0xc9d1ee)
                    if tuple(colors.values()) != expected:
                        raise RuntimeError("FLAT palette changed")
                    print("PASS: FLAT legacy colors, opacity, radii and implicit default")
                    continue
                pairs = (("text", "panel"), ("muted", "panel"), ("muted", "item"),
                         ("focus_text", "focus"), ("boot_text", "boot"),
                         ("boot_text", "boot_pressed"))
                minimum = min(contrast(colors[a], colors[b]) for a, b in pairs)
                if minimum < 4.5:
                    raise RuntimeError(f"{name}: opaque text contrast is only {minimum:.2f}:1")
                print(f"PASS: {name}, minimum opaque text contrast {minimum:.2f}:1")
            if len(set(palettes)) != len(STYLES):
                raise RuntimeError("Two presets have identical palettes")
            check_preset(compiler, temporary, "FLAT",
                         [f"-DCONFIG_KSHIM_MENU_STYLE_{n}=0" for n in STYLES])
            for name in STYLES:
                check_preset(compiler, temporary, name,
                             [f"-DCONFIG_KSHIM_MENU_STYLE_{n}={int(n == name)}" for n in STYLES])
            for first, second in itertools.combinations(STYLES, 2):
                result = run(compiler + ["-std=c11", "-fsyntax-only", str(SOURCE),
                                        f"-DCONFIG_KSHIM_MENU_STYLE_{first}=1",
                                        f"-DCONFIG_KSHIM_MENU_STYLE_{second}=1"])
                if result.returncode == 0 or "Select exactly one KSHIM_MENU_STYLE preset" not in result.stderr:
                    raise RuntimeError(f"Invalid multi-selection not diagnosed: {first}, {second}")
            print("PASS: zero-valued symbols, explicit 0/1 symbols and all 15 invalid pairs")
            if args.kconfig:
                check_kconfig(compiler, temporary)
        return 0
    except (OSError, RuntimeError, subprocess.TimeoutExpired) as error:
        parser.exit(1, f"FAIL: {error}\n")


if __name__ == "__main__":
    raise SystemExit(main())
