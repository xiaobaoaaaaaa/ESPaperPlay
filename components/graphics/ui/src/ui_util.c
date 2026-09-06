/*
 * ESPaperPlay - UI 屏幕通用工具（实现）
 * 见 include/espaperplay_ui_util.h 的接口说明。
 */
#include "espaperplay_ui_util.h"

#include "espaperplay_fonts.h"
#include "espaperplay_fs.h"
#include "espaperplay_system.h"
#include "icons_data.h"

#include <string.h>

/* ------------------------------------------------------------------ */
/* 屏幕尺寸 / 屏高缩放                                                   */
/* ------------------------------------------------------------------ */

void espaperplay_ui_screen_size(int32_t *out_w, int32_t *out_h) {
    lv_display_t *disp = lv_display_get_default();
    *out_w = lv_display_get_horizontal_resolution(disp);
    *out_h = lv_display_get_vertical_resolution(disp);
}

static float s_scale = 1.0f; /*!< 屏高缩放因子（页面 enter 时设置，全局单份） */

void espaperplay_ui_scale_init(int32_t ref_h) {
    int32_t scr_w = 0;
    int32_t scr_h = 0;
    espaperplay_ui_screen_size(&scr_w, &scr_h);
    s_scale = ref_h > 0 ? (float)scr_h / (float)ref_h : 1.0f;
}

int espaperplay_ui_scaled(int v) { return (int)(v * s_scale); }

/* ------------------------------------------------------------------ */
/* 字体 / 标签                                                           */
/* ------------------------------------------------------------------ */

lv_font_t *espaperplay_ui_font(int size_px) {
    const char *name = espaperplay_system_get_config()->selected_font;
    return espaperplay_fonts_load(name[0] ? name : ESPAPERPLAY_FONTS_DEFAULT_NAME,
                                  (uint32_t)size_px, ESPAPERPLAY_FONT_STYLE_NORMAL);
}

lv_obj_t *espaperplay_ui_label_create(lv_obj_t *parent, const char *text, int font_px,
                                      lv_text_align_t align) {
    lv_obj_t *label = lv_label_create(parent);
    lv_label_set_text(label, text);
    lv_obj_set_style_text_color(label, lv_color_black(), 0);
    lv_font_t *font = espaperplay_ui_font(font_px);
    if (font != NULL) {
        lv_obj_set_style_text_font(label, font, 0);
    }
    lv_obj_set_style_text_align(label, align, 0);
    lv_obj_set_width(label, LV_PCT(100));
    lv_obj_remove_flag(label, LV_OBJ_FLAG_SCROLLABLE);
    return label;
}

bool espaperplay_ui_label_set_text_dedup(lv_obj_t *label, const char *text) {
    if (label == NULL || text == NULL) {
        return false;
    }
    const char *cur = lv_label_get_text(label);
    if (cur != NULL && strcmp(cur, text) == 0) {
        return false;
    }
    lv_label_set_text(label, text);
    return true;
}

/* ------------------------------------------------------------------ */
/* 命中检测 / 屏幕坐标                                                   */
/* ------------------------------------------------------------------ */

bool espaperplay_ui_point_in(const lv_point_t *p, int x, int y, int w, int h) {
    return p->x >= x && p->x < x + w && p->y >= y && p->y < y + h;
}

static int obj_screen_axis(const lv_obj_t *obj, bool is_x) {
    int acc = 0;
    const lv_obj_t *p = obj;
    while (p != NULL && lv_obj_get_parent(p) != NULL) {
        acc += is_x ? (int)lv_obj_get_x(p) : (int)lv_obj_get_y(p);
        p = lv_obj_get_parent(p);
    }
    return acc;
}

int espaperplay_ui_obj_screen_x(const lv_obj_t *obj) { return obj_screen_axis(obj, true); }

int espaperplay_ui_obj_screen_y(const lv_obj_t *obj) { return obj_screen_axis(obj, false); }

/* ------------------------------------------------------------------ */
/* 文本 / 路径                                                           */
/* ------------------------------------------------------------------ */

void espaperplay_ui_utf8_truncate(const char *src, char *dst, size_t n) {
    size_t len = strlen(src);
    bool truncated = false;
    if (len > n - 4) {
        len = n - 4;
        while (len > 0 && (((unsigned char)src[len] & 0xC0) == 0x80)) {
            len--;
        }
        truncated = true;
    }
    memcpy(dst, src, len);
    dst[len] = '\0';
    if (truncated) {
        strlcat(dst, "…", n);
    }
}

bool espaperplay_ui_path_join(char *dst, size_t n, const char *a, const char *b) {
    return espaperplay_fs_join(dst, n, a, b);
}

const char *espaperplay_ui_path_basename(const char *path) {
    return espaperplay_fs_basename(path);
}

/* ------------------------------------------------------------------ */
/* WiFi 信号分档图标                                                     */
/* ------------------------------------------------------------------ */

const lv_image_dsc_t *espaperplay_ui_wifi_rssi_icon(int rssi) {
    if (rssi >= -60) {
        return &icon_wifi4_16;
    }
    if (rssi >= -70) {
        return &icon_wifi3_16;
    }
    if (rssi >= -80) {
        return &icon_wifi2_16;
    }
    return &icon_wifi1_16;
}
