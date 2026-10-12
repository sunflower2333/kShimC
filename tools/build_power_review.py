#!/usr/bin/env python3
"""Build an offline power-menu gallery from verified, original CI render frames."""
from pathlib import Path
import argparse
import json
from build_theme_review import read_set, write_review

STAGES = ('power-controls', 'restart-options', 'edl-confirm', 'shutdown-confirm')

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--input', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--expected-source')
    args = parser.parse_args()
    data = read_set(args.input, stages=STAGES, preview_dir='power-previews')
    if args.expected_source is not None and data['source'] != args.expected_source:
        raise ValueError('Unexpected rendered checkout')
    data['power_ui'] = True
    print(json.dumps(write_review(data, args.output), indent=2))

if __name__ == '__main__':
    main()
