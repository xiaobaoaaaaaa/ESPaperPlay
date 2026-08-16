#!/usr/bin/env python3
"""
Prepare QWeather (和风天气) icon bitmaps for LVGL.

Pipeline (per icon code):
  1. Obtain the source SVG set: use the local checkout
     (project root /QWeather-Icons-1.8.0/icons, if present) or download the
     official release zip into tools/qweather_icons_src/ (gitignored):
     https://github.com/qwd/Icons/releases/download/v1.8.0/QWeather-Icons-1.8.0.zip
  2. Take {code}-fill.svg (fill variant preferred, fallback {code}.svg).
  3. Rasterize to a 64x64 grayscale PNG with ImageMagick (`magick`/`convert`).
  4. Convert to an LVGL A8 (8-bit alpha) bitmap via Pillow:
     black glyph -> alpha 255, white background -> alpha 0, anti-aliased.
  5. Emit components/graphics/ui/src/qweather_icons.c + include/qweather_icons.h
     with a lookup: const lv_image_dsc_t *qweather_icon_get(const char *code)
     (returns NULL for unknown codes / weather not available).

Icon codes cover the QWeather API `icon` field (day/night + rain/snow/fog
variants). Source package license: MIT (QWeather-Icons-1.8.0/LICENSE).

Usage:
  tools/fonttools-venv/bin/python tools/prepare_qweather_icons.py
"""

import subprocess
import sys
import urllib.request
import zipfile
from pathlib import Path

TOOLS_DIR = Path(__file__).resolve().parent
PROJECT_DIR = TOOLS_DIR.parent
LOCAL_ICON_DIR = PROJECT_DIR / "QWeather-Icons-1.8.0" / "icons"
DL_DIR = TOOLS_DIR / "qweather_icons_src"        # downloaded zip + unpack, gitignored
DL_ZIP = DL_DIR / "QWeather-Icons-1.8.0.zip"
DL_UNPACK = DL_DIR / "unpacked"
ICON_ZIP_URL = ("https://github.com/qwd/Icons/releases/download/v1.8.0/"
                "QWeather-Icons-1.8.0.zip")
WORK_DIR = TOOLS_DIR / "icon_src"            # intermediate PNGs, gitignored
SRC_OUT = PROJECT_DIR / "components/graphics/ui/src/qweather_icons.c"
HDR_OUT = PROJECT_DIR / "components/graphics/ui/include/qweather_icons.h"

ICON_SIZE = 64

# QWeather API icon codes (dev.qweather.com `icon` field), grouped:
# 100-104/150-154: 晴/多云/阴 (day/night)
# 300-399: 雨 (incl. night 350/351)
# 400-499: 雪 (incl. night 450/451)
# 500-515: 雾/霾/沙尘
# 900/901/999: 热/冷/未知
ICON_CODES = [
    "100", "101", "102", "103", "104",
    "150", "151", "152", "153", "154",
    "300", "301", "302", "303", "304", "305", "306", "307", "308", "309",
    "310", "311", "312", "313", "314", "315", "316", "317", "318",
    "350", "351", "399",
    "400", "401", "402", "403", "404", "405", "406", "407", "408", "409",
    "410", "450", "451", "499",
    "500", "501", "502", "503", "504", "507", "508", "509", "510", "511",
    "512", "513", "514", "515",
    "900", "901", "999",
]


def ensure_icon_dir() -> Path:
    """本地图标包目录优先；缺失时下载官方 release zip 并解压（均不进入 git）。"""
    if LOCAL_ICON_DIR.exists():
        print(f"使用本地图标包: {LOCAL_ICON_DIR}")
        return LOCAL_ICON_DIR

    if not (DL_UNPACK / "icons").exists():
        DL_DIR.mkdir(parents=True, exist_ok=True)
        if not DL_ZIP.exists():
            print(f"下载图标包: {ICON_ZIP_URL}")
            req = urllib.request.Request(ICON_ZIP_URL, headers={
                "User-Agent": "Mozilla/5.0 (X11; Linux x86_64) ESPaperPlay-icon-tool/1.0",
            })
            with urllib.request.urlopen(req, timeout=60) as resp:
                DL_ZIP.write_bytes(resp.read())
        print(f"解压: {DL_ZIP}")
        with zipfile.ZipFile(DL_ZIP) as zf:
            zf.extractall(DL_UNPACK)
    # zip 内可能带顶层目录（如 QWeather-Icons-1.8.0/icons），递归查找 icons/
    icons = next((p for p in DL_UNPACK.glob("**/icons") if p.is_dir()), None)
    if icons is None:
        print("ERROR: 解压后未找到 icons/ 目录", file=sys.stderr)
        sys.exit(1)
    return icons


def pick_svg(icon_dir: Path, code: str) -> Path | None:
    fill = icon_dir / f"{code}-fill.svg"
    if fill.exists():
        return fill
    plain = icon_dir / f"{code}.svg"
    return plain if plain.exists() else None


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


def make_alpha_array(png: Path) -> list:
    from PIL import Image
    im = Image.open(png).convert("L").resize((ICON_SIZE, ICON_SIZE))
    # black glyph -> opaque; keep anti-aliased edges linear
    return [255 - v for v in list(im.getdata())]


def emit(alpha_map: dict, used_fill: dict) -> None:
    hdr = [
        "/*",
        " * SPDX-FileCopyrightText: 2026 ESPaperPlay Contributors",
        " *",
        " * SPDX-License-Identifier: MIT",
        " *",
        " * GENERATED FILE - DO NOT EDIT. Regenerate with:",
        " *   tools/fonttools-venv/bin/python tools/prepare_qweather_icons.py",
        " *",
        " * QWeather (和风天气) icons: LVGL A8 bitmaps, 64x64.",
        " * Source: QWeather-Icons-1.8.0 (github.com/qwd/Icons, MIT).",
        " */",
        "",
        "#pragma once",
        "",
        '#include "lvgl.h"',
        "",
        "/**",
        " * @brief 按和风天气 API 图标代码取 64x64 A8 图标描述符。",
        " *",
        " * @param code API icon 字段（如 \"100\"、\"101\"）；NULL 或未收录返回 NULL。",
        " * @return 图标描述符（可传给 lv_image_set_src）；未收录返回 NULL。",
        " */",
        "const lv_image_dsc_t *qweather_icon_get(const char *code);",
        "",
    ]
    HDR_OUT.write_text("\n".join(hdr), encoding="utf-8")
    print(f"wrote {HDR_OUT}")

    lines = [
        "/*",
        " * SPDX-FileCopyrightText: 2026 ESPaperPlay Contributors",
        " *",
        " * SPDX-License-Identifier: MIT",
        " *",
        " * GENERATED FILE - DO NOT EDIT. Regenerate with:",
        " *   tools/fonttools-venv/bin/python tools/prepare_qweather_icons.py",
        " */",
        "",
        '#include <string.h>',
        "",
        '#include "qweather_icons.h"',
        "",
    ]
    for code in alpha_map:
        data = alpha_map[code]
        lines.append(f"static const uint8_t s_qw_{code}_px[{len(data)}] = {{")
        for i in range(0, len(data), 16):
            lines.append("    " + ", ".join(f"0x{v:02X}" for v in data[i:i + 16]) + ",")
        lines.append("};")
        lines.append("")
    for code in alpha_map:
        lines.extend([
            f"static const lv_image_dsc_t s_qw_{code} = {{",
            "    .header = {",
            f"        .magic = LV_IMAGE_HEADER_MAGIC, .cf = LV_COLOR_FORMAT_A8,",
            f"        .w = {ICON_SIZE}, .h = {ICON_SIZE},",
            "    },",
            f"    .data_size = {ICON_SIZE * ICON_SIZE},",
            f"    .data = s_qw_{code}_px,",
            "};",
            "",
        ])
    lines.extend([
        "static const struct {",
        "    const char *code;",
        "    const lv_image_dsc_t *icon;",
        "} s_qw_map[] = {",
    ])
    for code in alpha_map:
        lines.append(f'    {{"{code}", &s_qw_{code}}},')
    lines.extend([
        "};",
        "",
        "const lv_image_dsc_t *qweather_icon_get(const char *code)",
        "{",
        "    if (code == NULL) {",
        "        return NULL;",
        "    }",
        "    for (size_t i = 0; i < sizeof(s_qw_map) / sizeof(s_qw_map[0]); i++) {",
        "        if (strcmp(code, s_qw_map[i].code) == 0) {",
        "            return s_qw_map[i].icon;",
        "        }",
        "    }",
        "    return NULL;",
        "}",
        "",
    ])
    SRC_OUT.write_text("\n".join(lines), encoding="utf-8")
    print(f"wrote {SRC_OUT}")

    missing = [c for c in ICON_CODES if c not in alpha_map]
    if missing:
        print(f"WARN: 未收录（包内无 SVG）: {missing}")


def main() -> int:
    icon_dir = ensure_icon_dir()
    WORK_DIR.mkdir(parents=True, exist_ok=True)
    alpha_map = {}
    used_fill = {}
    for code in ICON_CODES:
        svg = pick_svg(icon_dir, code)
        if svg is None:
            continue
        png = WORK_DIR / f"qw_{code}.png"
        rasterize(svg, png)
        alpha_map[code] = make_alpha_array(png)
        used_fill[code] = "-fill" in svg.name
    print(f"generated {len(alpha_map)} icons (fill variants: {sum(used_fill.values())})")
    emit(alpha_map, used_fill)
    return 0


if __name__ == "__main__":
    sys.exit(main())
