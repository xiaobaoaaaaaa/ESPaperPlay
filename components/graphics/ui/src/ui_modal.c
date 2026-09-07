/*
 * SPDX-FileCopyrightText: 2026 ESPaperPlay Contributors
 *
 * SPDX-License-Identifier: MIT
 */

/*
 * ESPaperPlay - UI 模态框通用构件（实现）
 * 见 include/espaperplay_ui_modal.h 的接口说明。
 */
#include "espaperplay_ui_modal.h"

#include "espaperplay_ui_util.h"

#include <stddef.h>

/* ------------------------------------------------------------------ */
/* 覆盖层 / 卡片 / 按钮构件                                               */
/* ------------------------------------------------------------------ */

lv_obj_t *espaperplay_ui_modal_overlay_create(bool click_close, lv_event_cb_t click_cb) {
    int32_t scr_w = 0;
    int32_t scr_h = 0;
    espaperplay_ui_screen_size(&scr_w, &scr_h);

    /* 全屏覆盖层：背景透明——卡片靠黑边框区分，避免 BW 模式下浅灰背景
     * 渲染成纯白把页面内容整片盖掉（与阅读器底边栏同款修复）。 */
    lv_obj_t *overlay = lv_obj_create(lv_screen_active());
    lv_obj_set_size(overlay, scr_w, scr_h);
    lv_obj_set_pos(overlay, 0, 0);
    lv_obj_set_style_bg_opa(overlay, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(overlay, 0, 0);
    lv_obj_set_style_radius(overlay, 0, 0);
    lv_obj_set_style_pad_all(overlay, 0, 0);
    lv_obj_remove_flag(overlay, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(overlay, LV_OBJ_FLAG_CLICKABLE);
    if (click_close && click_cb != NULL) {
        lv_obj_add_event_cb(overlay, click_cb, LV_EVENT_CLICKED, NULL);
    }
    return overlay;
}

lv_obj_t *espaperplay_ui_modal_card_create(lv_obj_t *overlay, int w, int h) {
    lv_obj_t *card = lv_obj_create(overlay);
    lv_obj_set_size(card, w, h);
    lv_obj_center(card);
    lv_obj_set_style_bg_color(card, lv_color_white(), 0);
    lv_obj_set_style_border_color(card, lv_color_black(), 0);
    lv_obj_set_style_border_width(card, 2, 0);
    lv_obj_set_style_radius(card, 12, 0);
    lv_obj_set_style_pad_all(card, 0, 0);
    lv_obj_remove_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    return card;
}

void espaperplay_ui_modal_card_title(lv_obj_t *card, const char *text, int y, int max_w) {
    lv_obj_t *title = espaperplay_ui_label_create(card, text, 20, LV_TEXT_ALIGN_CENTER);
    lv_obj_set_width(title, max_w > 0 ? max_w : LV_PCT(100));
    lv_label_set_long_mode(title, LV_LABEL_LONG_DOT);
    lv_obj_set_pos(title, 0, y);
}

void espaperplay_ui_modal_card_msg(lv_obj_t *card, const char *text, int y, int w) {
    lv_obj_t *msg = espaperplay_ui_label_create(card, text, 16, LV_TEXT_ALIGN_CENTER);
    lv_obj_set_width(msg, w);
    lv_obj_set_pos(msg, 0, y);
    lv_label_set_long_mode(msg, LV_LABEL_LONG_WRAP);
}

lv_obj_t *espaperplay_ui_modal_button(lv_obj_t *parent, const char *text, int x, int y, int w,
                                      int h, bool primary, lv_event_cb_t cb, void *user_data) {
    lv_obj_t *btn = lv_button_create(parent);
    lv_obj_set_size(btn, w, h);
    lv_obj_set_pos(btn, x, y);
    if (primary) {
        lv_obj_set_style_bg_color(btn, lv_color_black(), 0);
        lv_obj_set_style_border_width(btn, 0, 0);
    } else {
        lv_obj_set_style_bg_color(btn, lv_color_white(), 0);
        lv_obj_set_style_border_color(btn, lv_color_black(), 0);
        lv_obj_set_style_border_width(btn, 2, 0);
    }
    lv_obj_set_style_radius(btn, 8, 0);
    lv_obj_t *label = lv_label_create(btn);
    lv_label_set_text(label, text);
    lv_obj_set_style_text_color(label, primary ? lv_color_white() : lv_color_black(), 0);
    lv_obj_set_style_text_font(label, espaperplay_ui_font(20), 0);
    lv_obj_center(label);
    if (cb != NULL) {
        lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, user_data);
    }
    return btn;
}

int espaperplay_ui_modal_btn_h(void) {
    const int h = espaperplay_ui_scaled(44);
    return h < 38 ? 38 : h;
}

/* ------------------------------------------------------------------ */
/* 键盘输入模态                                                          */
/* ------------------------------------------------------------------ */

/** 密码可见性切换：切换密码模式并改按钮文案（ta 指针经 user_data 传递）。 */
static void kb_toggle_cb(lv_event_t *e) {
    lv_event_stop_bubbling(e);
    lv_obj_t *ta = (lv_obj_t *)lv_event_get_user_data(e);
    if (ta == NULL) {
        return;
    }
    const bool hide = lv_textarea_get_password_mode(ta);
    lv_textarea_set_password_mode(ta, !hide);
    lv_obj_t *label = lv_obj_get_child(lv_event_get_current_target(e), 0);
    if (label != NULL) {
        lv_label_set_text(label, hide ? "隐藏" : "显示");
    }
}

/** 面板样式（白底黑边圆角、无内边距、禁滚动）。 */
static void kb_panel_style(lv_obj_t *panel) {
    lv_obj_set_style_bg_color(panel, lv_color_white(), 0);
    lv_obj_set_style_border_color(panel, lv_color_black(), 0);
    lv_obj_set_style_border_width(panel, 2, 0);
    lv_obj_set_style_radius(panel, 12, 0);
    lv_obj_set_style_pad_all(panel, 0, 0);
    lv_obj_remove_flag(panel, LV_OBJ_FLAG_SCROLLABLE);
}

lv_obj_t *espaperplay_ui_kb_modal_open(const espaperplay_ui_kb_cfg_t *cfg) {
    if (cfg == NULL || cfg->title == NULL || cfg->on_cancel == NULL) {
        return NULL;
    }
    int32_t scr_w = 0;
    int32_t scr_h = 0;
    espaperplay_ui_screen_size(&scr_w, &scr_h);

    /* 全屏覆盖层（不点空白关闭），背景透明避免 BW 模式下整页变白。 */
    lv_obj_t *modal = espaperplay_ui_modal_overlay_create(false, NULL);

    /* 面板尺寸（内容驱动，横竖屏自适应）。键盘单独挂全屏 modal 贴底，
     * 避免被面板裁剪（面板仅含标题/输入框/提示/按钮）。 */
    const int panel_w = scr_w - 2 * cfg->margin;
    const int pad = 10;
    const int title_h = 30;
    const int ta_h = espaperplay_ui_scaled(52) < 40 ? 40 : espaperplay_ui_scaled(52);
    const int status_h = 22;
    const int bh = espaperplay_ui_modal_btn_h();
    const int kb_h = espaperplay_ui_scaled(240) < 170 ? 170 : espaperplay_ui_scaled(240);
    const int panel_h = pad + title_h + 6 + ta_h + 4 + status_h + 6 + bh + pad;
    const int kb_y = scr_h - kb_h - 6;      /* 键盘贴底 */
    const int panel_y = kb_y - panel_h - 6; /* 面板位于键盘上方 */

    lv_obj_t *panel = lv_obj_create(modal);
    lv_obj_set_size(panel, panel_w, panel_h);
    lv_obj_set_pos(panel, cfg->margin, panel_y);
    kb_panel_style(panel);

    /* 标题（密码模式时右侧有切换按钮，标题让位）。 */
    espaperplay_ui_modal_card_title(panel, cfg->title, pad,
                                    cfg->password ? panel_w - 140 : 0);

    /* 输入框（一行；FreeType 字体保证已有中文名可见）。 */
    lv_obj_t *ta = lv_textarea_create(panel);
    lv_obj_set_size(ta, panel_w - 2 * pad, ta_h);
    lv_obj_set_pos(ta, pad, pad + title_h + 6);
    lv_textarea_set_one_line(ta, true);
    lv_textarea_set_max_length(ta, cfg->max_len);
    lv_textarea_set_password_mode(ta, cfg->password);
    lv_textarea_set_text(ta, cfg->init_text != NULL ? cfg->init_text : "");
    lv_obj_set_style_text_color(ta, lv_color_black(), 0);
    lv_font_t *ta_font = espaperplay_ui_font(20);
    if (ta_font != NULL) {
        lv_obj_set_style_text_font(ta, ta_font, 0);
    }
    lv_obj_set_style_border_color(ta, lv_color_black(), 0);
    lv_obj_set_style_border_width(ta, 2, 0);
    lv_obj_set_style_radius(ta, 6, 0);
    lv_obj_set_style_pad_left(ta, 8, 0);
    /* 墨水屏：光标闪烁会触发连续局部刷新，禁用（anim_duration=0 即不闪烁）。
     * 默认主题在 LV_PART_CURSOR|LV_STATE_FOCUSED 上设了 400ms，故默认态与
     * 聚焦态都要覆盖。 */
    lv_obj_set_style_anim_duration(ta, 0, LV_PART_CURSOR);
    lv_obj_set_style_anim_duration(ta, 0, LV_PART_CURSOR | LV_STATE_FOCUSED);

    /* 提示/校验行（默认空，页面经 status_out 指针写入）。 */
    lv_obj_t *status = espaperplay_ui_label_create(panel, "", 16, LV_TEXT_ALIGN_LEFT);
    lv_obj_set_width(status, LV_PCT(100));
    lv_label_set_long_mode(status, LV_LABEL_LONG_DOT);
    lv_obj_set_pos(status, pad + 2, pad + title_h + 6 + ta_h + 4);

    /* 取消 / 确定。 */
    const int bw = (panel_w - 2 * pad - 12) / 2;
    const int btn_y = pad + title_h + 6 + ta_h + 4 + status_h + 6;
    espaperplay_ui_modal_button(panel, "取消", pad, btn_y, bw, bh, false, cfg->on_cancel,
                                cfg->user_data);
    espaperplay_ui_modal_button(panel, cfg->ok_text != NULL ? cfg->ok_text : "确定",
                                pad + bw + 12, btn_y, bw, bh, true, cfg->on_ok, cfg->user_data);

    /* 密码可见性切换（标题右侧）。 */
    if (cfg->password) {
        lv_obj_t *toggle = lv_button_create(panel);
        lv_obj_set_size(toggle, 120, 30);
        lv_obj_set_pos(toggle, panel_w - 124, pad);
        lv_obj_set_style_bg_color(toggle, lv_color_white(), 0);
        lv_obj_set_style_border_color(toggle, lv_color_black(), 0);
        lv_obj_set_style_border_width(toggle, 2, 0);
        lv_obj_set_style_radius(toggle, 6, 0);
        lv_obj_t *tl = lv_label_create(toggle);
        lv_label_set_text(tl, "显示");
        lv_obj_set_style_text_color(tl, lv_color_black(), 0);
        lv_obj_set_style_text_font(tl, espaperplay_ui_font(16), 0);
        lv_obj_center(tl);
        lv_obj_add_event_cb(toggle, kb_toggle_cb, LV_EVENT_CLICKED, ta);
    }

    /* 键盘贴底。 */
    lv_obj_t *kb = lv_keyboard_create(modal);
    lv_obj_set_size(kb, panel_w, kb_h);
    lv_obj_align(kb, LV_ALIGN_TOP_LEFT, cfg->margin, kb_y);
    lv_keyboard_set_mode(kb, LV_KEYBOARD_MODE_TEXT_LOWER);
    lv_keyboard_set_textarea(kb, ta);

    if (cfg->ta_out != NULL) {
        *cfg->ta_out = ta;
    }
    if (cfg->status_out != NULL) {
        *cfg->status_out = status;
    }
    return modal;
}
