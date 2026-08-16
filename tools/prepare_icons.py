#!/usr/bin/env python3
"""
Prepare app icon bitmaps for the home screen launcher.

Pipeline (per icon):
  1. Download the SVG from the Iconify API
     (https://api.iconify.design/{prefix}/{name}.svg, 64x64, black).
  2. Rasterize to a 64x64 grayscale PNG with ImageMagick (`magick`/`convert`).
  3. Convert to an LVGL A8 (8-bit alpha) bitmap array via Pillow:
     black glyph -> alpha 255, white background -> alpha 0, anti-aliased
     edge pixels map linearly (optional --binary keeps hard edges instead).
  4. Emit components/graphics/ui/src/icons_data.c + include/icons_data.h
     containing `const lv_image_dsc_t icon_<name>_64` descriptors.

LVGL renders A8 images as a mask with the default image recolor (black),
which is exactly what the e-paper needs.

Usage:
  tools/fonttools-venv/bin/python tools/prepare_icons.py [--binary]
"""

import argparse
import subprocess
import sys
import urllib.request
from pathlib import Path

TOOLS_DIR = Path(__file__).resolve().parent
PROJECT_DIR = TOOLS_DIR.parent
WORK_DIR = TOOLS_DIR / "icon_src"          # downloaded SVG/PNG, gitignored
SRC_OUT = PROJECT_DIR / "components/graphics/ui/src/icons_data.c"
HDR_OUT = PROJECT_DIR / "components/graphics/ui/include/icons_data.h"

ICON_SIZE = 64

# (iconify prefix, icon name, C variable suffix, comment)
ICONS = [
    ("mdi", "weather-partly-cloudy", "weather", "天气：多云转晴"),
    ("mdi", "book-open-page-variant", "reader", "阅读器：打开的书"),
    ("mdi", "bug", "debug", "测试/调试：bug"),
]


def fetch_svg(prefix: str, name: str, out: Path) -> None:
    url = (f"https://api.iconify.design/{prefix}/{name}.svg"
           f"?width={ICON_SIZE}&height={ICON_SIZE}&color=%23000000")
    print(f"  download {url}")
    req = urllib.request.Request(url, headers={
        "User-Agent": "Mozilla/5.0 (X11; Linux x86_64) ESPaperPlay-icon-tool/1.0",
    })
    with urllib.request.urlopen(req, timeout=30) as resp:
        out.write_bytes(resp.read())


def rasterize(svg: Path, png: Path) -> None:
    for tool in ("magick", "convert"):
        try:
            subprocess.run([tool, "-background", "white", "-alpha", "off",
                            str(svg), str(png)], check=True, capture_output=True)
            return
        except (FileNotFoundError, subprocess.CalledProcessError):
            continue
    print("ERROR: ImageMagick (magick/convert) not available", file=sys.stderr)
    sys.exit(1)


def make_alpha_array(png: Path, binary: bool) -> list:
    from PIL import Image
    im = Image.open(png).convert("L").resize((ICON_SIZE, ICON_SIZE))
    px = list(im.getdata())
    if binary:
        return [255 if v < 128 else 0 for v in px]
    # linear anti-aliased alpha: black glyph -> opaque
    return [255 - v for v in px]


def emit_c(alpha_map: dict, binary: bool) -> None:
    lines = [
        "/*",
        " * SPDX-FileCopyrightText: 2026 ESPaperPlay Contributors",
        " *",
        " * SPDX-License-Identifier: MIT",
        " *",
        " * GENERATED FILE - DO NOT EDIT. Regenerate with:",
        " *   tools/fonttools-venv/bin/python tools/prepare_icons.py"
        + (" --binary" if binary else ""),
        " *",
        " * App icon bitmaps (LVGL A8, 64x64) fetched from Iconify (icon-sets.iconify.design).",
        " */",
        "",
        '#include "icons_data.h"',
        "",
    ]
    for name in alpha_map:
        data = alpha_map[name]
        lines.append(f"static const uint8_t s_icon_{name}_px[{len(data)}] = {{")
        for i in range(0, len(data), 16):
            chunk = ", ".join(f"0x{v:02X}" for v in data[i:i + 16])
            lines.append(f"    {chunk},")
        lines.append("};")
        lines.append("")
    for name in alpha_map:
        lines.extend([
            f"const lv_image_dsc_t icon_{name}_{ICON_SIZE} = {{",
            "    .header = {",
            f"        .magic = LV_IMAGE_HEADER_MAGIC, .cf = LV_COLOR_FORMAT_A8,",
            f"        .w = {ICON_SIZE}, .h = {ICON_SIZE},",
            "    },",
            f"    .data_size = {ICON_SIZE * ICON_SIZE},",
            f"    .data = s_icon_{name}_px,",
            "};",
            "",
        ])
    SRC_OUT.write_text("\n".join(lines), encoding="utf-8")
    print(f"wrote {SRC_OUT}")


def emit_h(alpha_map: dict) -> None:
    lines = [
        "/*",
        " * SPDX-FileCopyrightText: 2026 ESPaperPlay Contributors",
        " *",
        " * SPDX-License-Identifier: MIT",
        " *",
        " * GENERATED FILE - DO NOT EDIT. Regenerate with:",
        " *   tools/fonttools-venv/bin/python tools/prepare_icons.py",
        " *",
        " * App icon bitmaps (LVGL A8, 64x64) fetched from Iconify (icon-sets.iconify.design).",
        " */",
        "",
        "#pragma once",
        "",
        '#include "lvgl.h"',
        "",
    ]
    for name in alpha_map:
        lines.append(
            f"/** {ICON_SIZE}x{ICON_SIZE} A8 图标（Iconify: {name}） */")
        lines.append(f"extern const lv_image_dsc_t icon_{name}_{ICON_SIZE};")
        lines.append("")
    HDR_OUT.write_text("\n".join(lines), encoding="utf-8")
    print(f"wrote {HDR_OUT}")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", action="store_true",
                        help="hard black/white edges instead of anti-aliased alpha")
    args = parser.parse_args()

    WORK_DIR.mkdir(parents=True, exist_ok=True)
    alpha_map = {}
    for prefix, name, var, comment in ICONS:
        svg = WORK_DIR / f"{var}.svg"
        png = WORK_DIR / f"{var}.png"
        print(f"[{var}] {comment}")
        fetch_svg(prefix, name, svg)
        rasterize(svg, png)
        alpha_map[var] = make_alpha_array(png, args.binary)

    emit_c(alpha_map, args.binary)
    emit_h(alpha_map)
    return 0


if __name__ == "__main__":
    sys.exit(main())
