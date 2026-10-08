#!/usr/bin/env python3
"""Build the menu's embedded Noto Sans CJK SC subset (src/ui/fonts/).

The subset keeps the outlines (CFF) of printable ASCII, Latin-1 and Latin
Extended-A, general and CJK punctuation, kana, full-width forms and the
3,755 GB2312 level-1 hanzi; LVGL's TinyTTF rasterizes it at any size. It
is stored LZ4-compressed (LVGL's decoder) behind a small header and is
inflated into RAM outside the runtime slot at boot (src/ui/font.c).

Needs fonttools and lz4 (pip install fonttools lz4). The source font is
downloaded unless --source is given; its SHA-256 is recorded either way.
"""
import argparse
import hashlib
import io
import json
import struct
import urllib.request
import zlib
from pathlib import Path

import lz4.block
from fontTools import subset
from fontTools.ttLib import TTFont

SOURCE_URL = ("https://github.com/notofonts/noto-cjk/raw/main/"
              "Sans/OTF/SimplifiedChinese/NotoSansCJKsc-Regular.otf")
LICENSE_URL = "https://github.com/notofonts/noto-cjk/raw/main/Sans/LICENSE"
MAGIC = b"KSF1"  # magic, raw size, compressed size, CRC-32 of the raw font


def coverage():
    codepoints = set(range(0x20, 0x7F)) | set(range(0xA0, 0x180))
    codepoints |= set(range(0x2000, 0x2070))   # general punctuation
    codepoints |= set(range(0x3000, 0x3100))   # CJK punctuation, kana
    codepoints |= set(range(0xFF00, 0xFFEF))   # full-width forms
    for high in range(0xB0, 0xD8):             # GB2312 level 1
        for low in range(0xA1, 0xFF):
            try:
                codepoints.add(ord(bytes([high, low]).decode("gb2312")))
            except UnicodeDecodeError:
                pass
    return codepoints


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", type=Path, help="NotoSansCJKsc-Regular.otf")
    parser.add_argument("--out", type=Path,
                        default=Path(__file__).resolve().parents[1] / "src/ui/fonts")
    args = parser.parse_args()
    source = args.source.read_bytes() if args.source else \
        urllib.request.urlopen(SOURCE_URL).read()

    options = subset.Options()
    options.layout_features = []
    options.hinting = False
    options.notdef_outline = True
    options.name_IDs = [0, 1, 2, 3, 4, 5, 6, 13, 14]   # keep the OFL notice
    options.drop_tables += ["GSUB", "GPOS", "GDEF", "BASE", "VORG", "vhea",
                            "vmtx", "DSIG"]
    font = TTFont(io.BytesIO(source))
    subsetter = subset.Subsetter(options)
    subsetter.populate(unicodes=coverage())
    subsetter.subset(font)
    raw = io.BytesIO()
    font.save(raw)
    raw = raw.getvalue()
    packed = lz4.block.compress(raw, mode="high_compression", compression=12,
                                store_size=False)

    args.out.mkdir(parents=True, exist_ok=True)
    blob = MAGIC + struct.pack("<III", len(raw), len(packed),
                               zlib.crc32(raw) & 0xFFFFFFFF) + packed
    (args.out / "noto_sans_cjk_sc.ksf").write_bytes(blob)
    if not (args.out / "OFL.txt").exists():
        (args.out / "OFL.txt").write_bytes(urllib.request.urlopen(LICENSE_URL).read())
    (args.out / "provenance.json").write_text(json.dumps({
        "source": SOURCE_URL,
        "source_sha256": hashlib.sha256(source).hexdigest(),
        "license": "SIL Open Font License 1.1 (OFL.txt)",
        "coverage": "ASCII, Latin-1, Latin Extended-A, general and CJK "
                    "punctuation, kana, full-width forms, GB2312 level 1",
        "glyphs": len(font.getGlyphOrder()),
        "raw_bytes": len(raw), "compressed_bytes": len(packed),
        "blob_sha256": hashlib.sha256(blob).hexdigest(),
        "generator": "tools/gen_ui_font.py",
    }, indent=2) + "\n")
    print(f"{len(font.getGlyphOrder())} glyphs, {len(raw)} -> {len(packed)} bytes")


if __name__ == "__main__":
    main()
