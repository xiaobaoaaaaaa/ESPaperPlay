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

# (iconify prefix, icon name, C variable suffix, comment) —— 64x64 应用图标
ICONS = [
    ("mdi", "weather-partly-cloudy", "weather", "天气：多云转晴"),
    ("mdi", "book-open-page-variant", "reader", "阅读器：打开的书"),
    ("mdi", "cog", "settings", "设置：齿轮"),
    ("mdi", "bug", "debug", "测试/调试：bug"),
]

# 小图标（16x16）：状态栏等 UI 元素
SMALL_ICON_SIZE = 16
SMALL_ICONS = [
    ("mdi", "wifi-strength-4", "wifi4", "WiFi 信号强"),
    ("mdi", "wifi-strength-3", "wifi3", "WiFi 信号中"),
    ("mdi", "wifi-strength-2", "wifi2", "WiFi 信号弱"),
    ("mdi", "wifi-strength-1", "wifi1", "WiFi 信号极弱"),
    ("mdi", "wifi-off", "wifi_off", "WiFi 未连接"),
    ("mdi", "wifi", "wifi", "WiFi（默认）"),
    ("mdi", "access-point", "wifi_ap", "AP 热点"),
    ("mdi", "water-percent", "humidity", "湿度"),
    ("mdi", "weather-windy", "wind", "风"),
    ("mdi", "weather-rainy", "rain", "降水"),
    ("mdi", "gauge", "pressure", "气压"),
    ("mdi", "eye-outline", "visibility", "能见度"),
    ("mdi", "thermometer", "thermo", "体感"),
]


def fetch_svg(prefix: str, name: str, out: Path, size: int) -> None:
    url = (f"https://api.iconify.design/{prefix}/{name}.svg"
           f"?width={size}&height={size}&color=%23000000")
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


def make_alpha_array(png: Path, binary: bool, size: int) -> list:
    from PIL import Image
    im = Image.open(png).convert("L").resize((size, size))
    px = list(im.getdata())
    if binary:
        return [255 if v < 128 else 0 for v in px]
    # linear anti-aliased alpha: black glyph -> opaque
    return [255 - v for v in px]


def emit_c(items: list, binary: bool) -> None:
    """items: [(var 名, 尺寸, alpha 数组), ...]"""
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
        " * App icon bitmaps (LVGL A8) fetched from Iconify (icon-sets.iconify.design).",
        " */",
        "",
        '#include "icons_data.h"',
        "",
    ]
    for name, size, data in items:
        lines.append(f"static const uint8_t s_icon_{name}_px[{len(data)}] = {{")
        for i in range(0, len(data), 16):
            chunk = ", ".join(f"0x{v:02X}" for v in data[i:i + 16])
            lines.append(f"    {chunk},")
        lines.append("};")
        lines.append("")
    for name, size, data in items:
        lines.extend([
            f"const lv_image_dsc_t icon_{name}_{size} = {{",
            "    .header = {",
            f"        .magic = LV_IMAGE_HEADER_MAGIC, .cf = LV_COLOR_FORMAT_A8,",
            f"        .w = {size}, .h = {size},",
            "    },",
            f"    .data_size = {size * size},",
            f"    .data = s_icon_{name}_px,",
            "};",
            "",
        ])
    SRC_OUT.write_text("\n".join(lines), encoding="utf-8")
    print(f"wrote {SRC_OUT}")


def emit_h(items: list) -> None:
    lines = [
        "/*",
        " * SPDX-FileCopyrightText: 2026 ESPaperPlay Contributors",
        " *",
        " * SPDX-License-Identifier: MIT",
        " *",
        " * GENERATED FILE - DO NOT EDIT. Regenerate with:",
        " *   tools/fonttools-venv/bin/python tools/prepare_icons.py",
        " *",
        " * App icon bitmaps (LVGL A8) fetched from Iconify (icon-sets.iconify.design).",
        " */",
        "",
        "#pragma once",
        "",
        '#include "lvgl.h"',
        "",
    ]
    for name, size, _data in items:
        lines.append(
            f"/** {size}x{size} A8 图标（Iconify: {name}） */")
        lines.append(f"extern const lv_image_dsc_t icon_{name}_{size};")
        lines.append("")
    HDR_OUT.write_text("\n".join(lines), encoding="utf-8")
    print(f"wrote {HDR_OUT}")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", action="store_true",
                        help="hard black/white edges instead of anti-aliased alpha")
    args = parser.parse_args()

    WORK_DIR.mkdir(parents=True, exist_ok=True)
    items = []

    # 应用图标 64x64
    for prefix, name, var, comment in ICONS:
        svg = WORK_DIR / f"{var}.svg"
        png = WORK_DIR / f"{var}.png"
        print(f"[{var}] {comment}")
        fetch_svg(prefix, name, svg, ICON_SIZE)
        rasterize(svg, png)
        items.append((var, ICON_SIZE, make_alpha_array(png, args.binary, ICON_SIZE)))

    # 状态栏等小图标 16x16
    for prefix, name, var, comment in SMALL_ICONS:
        svg = WORK_DIR / f"{var}.svg"
        png = WORK_DIR / f"{var}.png"
        print(f"[{var}] {comment}")
        fetch_svg(prefix, name, svg, SMALL_ICON_SIZE)
        rasterize(svg, png)
        items.append((var, SMALL_ICON_SIZE,
                      make_alpha_array(png, args.binary, SMALL_ICON_SIZE)))

    emit_c(items, args.binary)
    emit_h(items)
    return 0


if __name__ == "__main__":
    sys.exit(main())
