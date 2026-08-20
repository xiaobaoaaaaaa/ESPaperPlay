/*
 * SPDX-FileCopyrightText: 2026 ESPaperPlay Contributors
 *
 * SPDX-License-Identifier: MIT
 *
 * GENERATED FILE - DO NOT EDIT. Regenerate with:
 *   tools/fonttools-venv/bin/python tools/prepare_qweather_icons.py
 *
 * QWeather (和风天气) icons: LVGL A8 bitmaps (64x64 + 32x32).
 * Source: QWeather-Icons-1.8.0 (github.com/qwd/Icons, MIT).
 */

#pragma once

#include "lvgl.h"

/**
 * @brief 按和风天气 API 图标代码取 64x64 A8 图标描述符。
 *
 * @param code API icon 字段（如 "100"、"101"）；NULL 或未收录返回 NULL。
 * @return 图标描述符（可传给 lv_image_set_src）；未收录返回 NULL。
 */
const lv_image_dsc_t *qweather_icon_get(const char *code);

/**
 * @brief 按和风天气 API 图标代码取 32x32 A8 小图标描述符（列表/预报行用）。
 *
 * @param code API icon 字段；NULL 或未收录返回 NULL。
 * @return 32x32 图标描述符；未收录返回 NULL。
 */
const lv_image_dsc_t *qweather_icon_get_small(const char *code);
