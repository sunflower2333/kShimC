#!/usr/bin/env python3
"""Compile/run the LVGL-independent geometry contract for every selected preset."""
import argparse
from pathlib import Path
import re
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--cc', default='gcc')
    parser.add_argument('--sanitize', action='store_true')
    args = parser.parse_args()
    header = (ROOT / 'src/ui/menu_design.h').read_text(encoding='utf-8')
    names = ['FLAT'] + re.findall(r'^#(?:if|elif) defined\(CONFIG_KSHIM_MENU_STYLE_([A-Z0-9_]+)\)', header, re.M)
    if len(names) != 25 or len(set(names)) != 25:
        raise SystemExit('Expected exactly 25 unique compile-time theme presets')
    with tempfile.TemporaryDirectory(prefix='kshim-design-') as temporary:
        for name in names:
            executable = Path(temporary) / name
            command = [args.cc, '-std=c11', '-O1', '-g', '-Wall', '-Wextra', '-Werror', '-pedantic',
                       f'-DCONFIG_KSHIM_MENU_STYLE_{name}=1', f'-DEXPECTED_STYLE="{name}"',
                       str(ROOT / 'tests/menu_design_contract_test.c'), '-o', str(executable)]
            if args.sanitize:
                command += ['-fsanitize=address,undefined', '-fno-omit-frame-pointer', '-no-pie']
            subprocess.run(command, check=True)
            subprocess.run([str(executable)], check=True)
    print(f'PASS: {len(names)} compile-time presets ({args.cc}, sanitizers={args.sanitize})')

if __name__ == '__main__':
    main()
