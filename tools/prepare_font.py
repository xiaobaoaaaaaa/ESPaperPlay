#!/usr/bin/env python3
"""
Prepare the project font asset: Noto Sans SC (Chinese) subset TTF.

Pipeline:
  1. Download the Noto Sans SC variable font (TrueType flavor) from google/fonts
     into tools/font_src/ (gitignored) if not already present.
  2. Instantiate the static Regular (wght=400) instance.
  3. Build a charset: GB2312 level-1 + level-2 (6763 common Chinese chars)
     plus ASCII and common CJK punctuation.
  4. Subset with pyftsubset -> assets/fonts/NotoSansSC-Regular.ttf

Requires the local venv:  tools/fonttools-venv  (see README or run:
  python3 -m venv tools/fonttools-venv
  tools/fonttools-venv/bin/pip install fonttools)
"""

import argparse
import sys
from pathlib import Path

TOOLS_DIR = Path(__file__).resolve().parent
PROJECT_DIR = TOOLS_DIR.parent
SRC_DIR = TOOLS_DIR / "font_src"
OUT_DIR = PROJECT_DIR / "assets" / "fonts"

FONT_URL = "https://github.com/google/fonts/raw/main/ofl/notosanssc/NotoSansSC%5Bwght%5D.ttf"
VAR_FONT = SRC_DIR / "NotoSansSC[wght].ttf"
OUT_FONT = OUT_DIR / "NotoSansSC_Regular.ttf"


def build_charset() -> str:
    """GB2312 L1+L2 + ASCII + common CJK punctuation."""
    chars = set()

    # ASCII printable
    chars.update(chr(c) for c in range(0x20, 0x7F))

    # GB2312 two-byte region: L1 0xB0-0xD7, L2 0xD8-0xF7 (row) x 0xA1-0xFE (cell)
    # 注意：位码合法范围是 0xA1-0xFE，range 上界必须取 0xFF 才能覆盖到 0xFE
    for b1 in range(0xB0, 0xF8):
        for b2 in range(0xA1, 0xFF):
            try:
                chars.update(bytes([b1, b2]).decode("gb2312"))
            except UnicodeDecodeError:
                pass  # undefined slot

    # Common CJK punctuation / symbols not fully covered by GB2312
    chars.update(
        "，。、；：？！“”‘’（）【】《》〈〉「」『』〖〗〔〕…—·～￥×÷"
        "··’‘’“”"
    )
    chars.update(chr(c) for c in range(0x3000, 0x3040))   # CJK symbols & punctuation
    chars.update(chr(c) for c in range(0xFF01, 0xFF5F))   # fullwidth forms
    chars.update(["\u00b7", "\u00b0", "\u2013", "\u2014", "\u2018", "\u2019",
                  "\u201c", "\u201d", "\u2026", "\u2044", "\u20ac", "\u2103",
                  "\u26a0"])

    return "".join(sorted(chars))


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--skip-download", action="store_true",
                        help="fail if the source variable font is missing instead of downloading")
    args = parser.parse_args()

    try:
        from fontTools import subset
        from fontTools.ttLib import TTFont
        from fontTools.varLib import instancer
    except ImportError:
        print("fontTools not available. Run: tools/fonttools-venv/bin/pip install fonttools",
              file=sys.stderr)
        return 1

    SRC_DIR.mkdir(parents=True, exist_ok=True)
    OUT_DIR.mkdir(parents=True, exist_ok=True)

    # 1. source font
    if not VAR_FONT.exists():
        if args.skip_download:
            print(f"missing source font: {VAR_FONT} (run without --skip-download)", file=sys.stderr)
            return 1
        import urllib.request
        print(f"downloading {FONT_URL}")
        urllib.request.urlretrieve(FONT_URL, VAR_FONT)
    print(f"source: {VAR_FONT} ({VAR_FONT.stat().st_size / 1e6:.1f} MB)")

    # 2. static instance (Regular)
    tmp_regular = SRC_DIR / "NotoSansSC-Regular-static.ttf"
    print("instancing wght=400 ...")
    var_font = TTFont(str(VAR_FONT))
    static_font = instancer.instantiateVariableFont(var_font, {"wght": 400}, inplace=False)
    static_font.save(tmp_regular)

    # 3. charset
    chars = build_charset()
    tmp_chars = SRC_DIR / "charset.txt"
    tmp_chars.write_text("".join(chars), encoding="utf-8")
    print(f"charset: {len(chars)} code points")

    # 4. subset
    print(f"subsetting -> {OUT_FONT}")
    opts = subset.Options()
    opts.hinting = False            # Noto has no useful TrueType hints; saves space
    opts.recalc_bounds = True
    opts.desubroutinize = False     # TTF: no-op, kept for clarity
    opts.name_IDs = [1, 2, 3, 4, 6]  # family/subfamily/full/ps name + trademark
    subset.main([
        str(tmp_regular),
        f"--output-file={OUT_FONT}",
        f"--text-file={tmp_chars}",
        "--no-hinting",
        "--layout-features=*",
        "--name-IDs=1,2,3,4,6",
        "--glyph-names",
    ])

    print(f"done: {OUT_FONT} ({OUT_FONT.stat().st_size / 1e6:.2f} MB)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
