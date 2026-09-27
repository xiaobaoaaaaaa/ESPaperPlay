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

void espaperplay_ui_card_style(lv_obj_t *obj) {
    lv_obj_set_style_bg_color(obj, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(obj, lv_color_black(), 0);
    lv_obj_set_style_border_width(obj, ESPAPERPLAY_UI_BORDER, 0);
    lv_obj_set_style_radius(obj, ESPAPERPLAY_UI_RADIUS, 0);
    lv_obj_set_style_shadow_width(obj, 0, 0);
    lv_obj_set_style_pad_all(obj, 0, 0);
    lv_obj_remove_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
}

static void ui_icon_line(lv_obj_t *parent, const lv_point_precise_t *points, int count) {
    lv_obj_t *line = lv_line_create(parent);
    lv_line_set_points(line, points, count);
    lv_obj_set_style_line_color(line, lv_color_black(), 0);
    lv_obj_set_style_line_width(line, 2, 0);
    lv_obj_set_style_line_rounded(line, true, 0);
}

void espaperplay_ui_chevron_create(lv_obj_t *parent, int x, int y, bool back) {
    static const lv_point_precise_t left[] = {{14, 0}, {4, 10}, {14, 20}};
    static const lv_point_precise_t right[] = {{4, 0}, {14, 10}, {4, 20}};
    lv_obj_t *line = lv_line_create(parent);
    lv_line_set_points(line, back ? left : right, 3);
    lv_obj_set_pos(line, x, y);
    lv_obj_set_style_line_color(line, lv_color_black(), 0);
    lv_obj_set_style_line_width(line, 2, 0);
    lv_obj_set_style_line_rounded(line, true, 0);
}

void espaperplay_ui_file_icon_create(lv_obj_t *parent, int x, int y, bool folder) {
    static const lv_point_precise_t dir[] = {{1,8},{1,29},{33,29},{33,6},{17,6},{13,1},{1,1},{1,8}};
    static const lv_point_precise_t file[] = {{4,1},{22,1},{30,9},{30,33},{4,33},{4,1}};
    static const lv_point_precise_t fold[] = {{22,1},{22,9},{30,9}};
    lv_obj_t *art = lv_obj_create(parent);
    lv_obj_set_pos(art, x, y); lv_obj_set_size(art, 36, 36);
    espaperplay_ui_card_style(art);
    lv_obj_set_style_border_width(art, 0, 0);
    lv_obj_remove_flag(art, LV_OBJ_FLAG_CLICKABLE);
    ui_icon_line(art, folder ? dir : file, folder ? 8 : 6);
    if (!folder) ui_icon_line(art, fold, 3);
}

lv_obj_t *espaperplay_ui_nav_button(lv_obj_t *parent, const char *text,
    espaperplay_ui_icon_t icon, int x, int y, int w, int h,
    lv_event_cb_t cb, void *user_data) {
    lv_obj_t *btn = lv_button_create(parent);
    lv_obj_set_pos(btn, x, y);
    lv_obj_set_size(btn, w, h);
    espaperplay_ui_card_style(btn);
    const lv_state_t states[] = {LV_STATE_DEFAULT, LV_STATE_PRESSED, LV_STATE_DISABLED, LV_STATE_FOCUSED};
    for (unsigned i = 0; i < sizeof(states) / sizeof(states[0]); i++) {
        lv_obj_set_style_border_width(btn, 0, states[i]);
        lv_obj_set_style_bg_color(btn, lv_color_white(), states[i]);
        lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, states[i]);
        lv_obj_set_style_opa(btn, LV_OPA_COVER, states[i]);
        lv_obj_set_style_recolor_opa(btn, LV_OPA_TRANSP, states[i]);
        lv_obj_set_style_shadow_width(btn, 0, states[i]);
        lv_obj_set_style_outline_width(btn, 0, states[i]);
    }
    if (cb) lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, user_data);
    /* 标签固定为首个子对象，供动态按钮文案更新。 */
    lv_obj_t *label = espaperplay_ui_label_create(btn, text, 16, LV_TEXT_ALIGN_CENTER);
    lv_obj_set_size(label, w - 8, 24);
    lv_obj_align(label, LV_ALIGN_BOTTOM_MID, 0, -5);
    lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);
    lv_obj_t *art = lv_obj_create(btn);
    lv_obj_set_size(art, 28, 28);
    lv_obj_align(art, LV_ALIGN_TOP_MID, 0, 5);
    espaperplay_ui_card_style(art);
    lv_obj_set_style_border_width(art, 0, 0);
    lv_obj_remove_flag(art, LV_OBJ_FLAG_CLICKABLE);
    static const lv_point_precise_t folder[] = {{2,8},{2,24},{26,24},{26,6},{13,6},{10,3},{2,3},{2,8}};
    static const lv_point_precise_t up[] = {{9,15},{14,10},{19,15},{14,10},{14,21}};
    static const lv_point_precise_t plus[] = {{8,15},{20,15},{14,15},{14,9},{14,21}};
    static const lv_point_precise_t book[] = {{14,6},{8,3},{2,3},{2,23},{8,23},{14,26},{20,23},{26,23},{26,3},{20,3},{14,6},{14,26}};
    static const lv_point_precise_t clock[] = {{14,6},{14,14},{20,17}};
    static const lv_point_precise_t back[] = {{14,5},{5,14},{14,23},{5,14},{25,14}};
    static const lv_point_precise_t list1[] = {{3,6},{25,6}};
    static const lv_point_precise_t list2[] = {{3,14},{25,14}};
    static const lv_point_precise_t list3[] = {{3,22},{25,22}};
    switch (icon) {
    case ESPAPERPLAY_UI_ICON_FOLDER_UP:
    case ESPAPERPLAY_UI_ICON_FOLDER_ADD:
        ui_icon_line(art, folder, 8);
        ui_icon_line(art, icon == ESPAPERPLAY_UI_ICON_FOLDER_UP ? up : plus, 5);
        break;
    case ESPAPERPLAY_UI_ICON_BOOK: ui_icon_line(art, book, 12); break;
    case ESPAPERPLAY_UI_ICON_BACK: ui_icon_line(art, back, 5); break;
    case ESPAPERPLAY_UI_ICON_LIST:
        ui_icon_line(art, list1, 2); ui_icon_line(art, list2, 2); ui_icon_line(art, list3, 2); break;
    case ESPAPERPLAY_UI_ICON_HISTORY:
    case ESPAPERPLAY_UI_ICON_CONTRAST: {
        lv_obj_t *circle = lv_obj_create(art);
        lv_obj_set_pos(circle, 2, 2); lv_obj_set_size(circle, 24, 24);
        espaperplay_ui_card_style(circle);
        lv_obj_remove_flag(circle, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_style_radius(circle, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_border_width(circle, 2, 0);
        if (icon == ESPAPERPLAY_UI_ICON_HISTORY) ui_icon_line(art, clock, 3);
        else {
            lv_obj_t *half = lv_obj_create(circle);
            lv_obj_set_size(half, 12, 24); lv_obj_set_pos(half, -2, -2);
            lv_obj_set_style_bg_color(half, lv_color_black(), 0);
            lv_obj_set_style_border_width(half, 0, 0);
            lv_obj_set_style_radius(half, 0, 0);
            lv_obj_remove_flag(half, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
        }
        break;
    }
    default: {
        lv_obj_t *a = espaperplay_ui_label_create(art,
            icon == ESPAPERPLAY_UI_ICON_SMALL_TEXT ? "A-" : "A+", 24, LV_TEXT_ALIGN_CENTER);
        lv_obj_set_size(a, 28, 32); lv_obj_set_pos(a, 0, -3);
        break;
    }
    }
    lv_obj_t *indicator = lv_obj_create(btn);
    lv_obj_set_size(indicator, 32, 3);
    lv_obj_align(indicator, LV_ALIGN_BOTTOM_MID, 0, -1);
    lv_obj_set_style_border_width(indicator, 0, 0);
    lv_obj_set_style_bg_color(indicator, lv_color_black(), 0);
    lv_obj_set_style_radius(indicator, 2, 0);
    lv_obj_remove_flag(indicator, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(indicator, LV_OBJ_FLAG_HIDDEN);
    return btn;
}

void espaperplay_ui_nav_select(lv_obj_t *button, bool selected) {
    lv_obj_t *indicator = lv_obj_get_child(button, 2);
    if (selected) lv_obj_remove_flag(indicator, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_add_flag(indicator, LV_OBJ_FLAG_HIDDEN);
}

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
/* 页点指示器（分页）                                                    */
/* ------------------------------------------------------------------ */

void espaperplay_ui_pager_dots_create(lv_obj_t **dots, int count, int32_t scr_w, int y,
                                      int spacing) {
    if (dots == NULL || count <= 0) {
        return;
    }
    if (spacing <= 0) {
        spacing = 24;
    }
    if (count > 1 && (count - 1) * spacing > scr_w - 2 * ESPAPERPLAY_UI_MARGIN - 10) {
        spacing = (scr_w - 2 * ESPAPERPLAY_UI_MARGIN - 10) / (count - 1);
    }
    for (int i = 0; i < count; i++) {
        dots[i] = lv_obj_create(lv_screen_active());
        lv_obj_set_size(dots[i], 10, 10);
        lv_obj_set_pos(dots[i], ((int)scr_w - (count - 1) * spacing) / 2 + i * spacing - 5, y);
        lv_obj_set_style_radius(dots[i], LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_border_width(dots[i], 1, 0);
        lv_obj_set_style_border_color(dots[i], lv_color_black(), 0);
        lv_obj_remove_flag(dots[i], LV_OBJ_FLAG_SCROLLABLE);
    }
}

void espaperplay_ui_pager_dots_set(lv_obj_t **dots, int count, int current) {
    if (dots == NULL) {
        return;
    }
    for (int i = 0; i < count; i++) {
        lv_obj_set_style_bg_color(dots[i], i == current ? lv_color_black() : lv_color_white(), 0);
    }
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

void espaperplay_ui_page_heading(lv_obj_t *parent, const char *text,
                                 const char *subtitle, lv_event_cb_t back) {
    lv_obj_t *title = espaperplay_ui_label_create(parent, text, 32, LV_TEXT_ALIGN_LEFT);
    lv_obj_set_pos(title, back ? 76 : 24, 18);
    lv_obj_set_width(title, lv_obj_get_width(parent) - (back ? 100 : 48));
    lv_label_set_long_mode(title, LV_LABEL_LONG_DOT);
    if (subtitle) {
        lv_obj_t *hint = espaperplay_ui_label_create(parent, subtitle, 16, LV_TEXT_ALIGN_LEFT);
        lv_obj_set_pos(hint, 24, 80);
        lv_obj_set_width(hint, lv_obj_get_width(parent) - 48);
        lv_label_set_long_mode(hint, LV_LABEL_LONG_WRAP);
    }
    if (back) {
        lv_obj_t *button = lv_obj_create(parent);
        espaperplay_ui_card_style(button);
        lv_obj_set_style_border_width(button, 0, 0);
        lv_obj_set_size(button, 64, 64);
        lv_obj_set_pos(button, 4, 8);
        espaperplay_ui_chevron_create(button, 22, 22, true);
        lv_obj_add_event_cb(button, back, LV_EVENT_CLICKED, NULL);
    }
}

void espaperplay_ui_network_row(lv_obj_t *parent, const char *ssid, const char *detail,
                                int8_t rssi, int y, int height, lv_event_cb_t cb, void *user_data) {
    const int width = lv_obj_get_width(parent);
    lv_obj_t *row = lv_obj_create(parent);
    espaperplay_ui_card_style(row);
    lv_obj_set_pos(row, 0, y);
    lv_obj_set_size(row, width, height);
    lv_obj_set_style_radius(row, 0, 0);
    lv_obj_set_style_border_side(row, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_add_event_cb(row, cb, LV_EVENT_CLICKED, user_data);
    lv_obj_t *icon = lv_image_create(row);
    lv_image_set_src(icon, espaperplay_ui_wifi_rssi_icon(rssi));
    lv_image_set_pivot(icon, 0, 0);
    lv_image_set_scale(icon, 384);
    lv_obj_set_pos(icon, 12, (height - 24) / 2);
    lv_obj_t *name = espaperplay_ui_label_create(row, ssid, 20, LV_TEXT_ALIGN_LEFT);
    lv_obj_set_pos(name, 56, 12);
    lv_obj_set_width(name, width - 88);
    lv_label_set_long_mode(name, LV_LABEL_LONG_DOT);
    lv_obj_t *sub = espaperplay_ui_label_create(row, detail, 16, LV_TEXT_ALIGN_LEFT);
    lv_obj_set_pos(sub, 56, 46);
    lv_obj_set_width(sub, width - 88);
    lv_label_set_long_mode(sub, LV_LABEL_LONG_DOT);
    espaperplay_ui_chevron_create(row, width - 26, (height - 20) / 2, false);
}
