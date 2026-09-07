/*
 * SPDX-FileCopyrightText: 2026 ESPaperPlay Contributors
 *
 * SPDX-License-Identifier: MIT
 */

/*
 * ESPaperPlay - UI 模态框通用构件
 *
 * 覆盖层 / 居中卡片 / 标题消息 / 主次按钮 / 键盘输入模态的单一实现。
 * 此前这些样板在 files / wifi_list / setup / settings / reader_home 各屏
 * 各写一份（键盘输入模态三份 ~90 行逐行拷贝）；墨水屏相关特调（BW 模式
 * 透明覆盖层、光标闪烁禁用、密码可见性切换）在此收口。
 */
#pragma once

#include "lvgl.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------ */
/* 覆盖层 / 卡片 / 按钮构件                                               */
/* ------------------------------------------------------------------ */

/**
 * @brief 全屏透明覆盖层（拦截触摸；BW 模式下避免整页变白）。
 * @param click_close true=点击空白触发 click_cb（用于点空白关闭）；
 *                    false=仅拦截（输入模态防丢输入）。
 */
lv_obj_t *espaperplay_ui_modal_overlay_create(bool click_close, lv_event_cb_t click_cb);

/** @brief 居中卡片：白底 + 黑边 2px + 圆角 12 + 无内边距 + 禁滚动。 */
lv_obj_t *espaperplay_ui_modal_card_create(lv_obj_t *overlay, int w, int h);

/**
 * @brief 卡片标题（20px 黑字居中，DOT 截断）。
 * @param max_w 像素宽度上限；0 = 100% 卡片宽。
 */
void espaperplay_ui_modal_card_title(lv_obj_t *card, const char *text, int y, int max_w);

/** @brief 卡片消息（16px 黑字居中，自动换行）。 */
void espaperplay_ui_modal_card_msg(lv_obj_t *card, const char *text, int y, int w);

/**
 * @brief 主/次按钮工厂：primary=黑底白字（无边框），否则白底黑边 2px；
 * 圆角 8，20px 居中标签。
 */
lv_obj_t *espaperplay_ui_modal_button(lv_obj_t *parent, const char *text, int x, int y, int w,
                                      int h, bool primary, lv_event_cb_t cb, void *user_data);

/* ------------------------------------------------------------------ */
/* 键盘输入模态                                                          */
/* ------------------------------------------------------------------ */

/** 键盘输入模态配置。 */
typedef struct {
    const char *title;      /*!< 面板标题（DOT 截断） */
    const char *init_text;  /*!< 初始文本（NULL = 空） */
    int max_len;            /*!< 最大输入长度（字符） */
    bool password;          /*!< 密码模式（true 时自动加「显示/隐藏」切换按钮，
                                 标题宽度自动让位至右侧按钮） */
    const char *ok_text;    /*!< 确定按钮文案（NULL = "确定"） */
    lv_event_cb_t on_ok;    /*!< 「确定」回调（可 NULL，仅测试用） */
    lv_event_cb_t on_cancel; /*!< 「取消」回调（通常由页面关闭模态） */
    void *user_data;        /*!< 透传给两个回调 */
    lv_obj_t **ta_out;      /*!< 回写输入框指针（可 NULL；页面校验/取值用） */
    lv_obj_t **status_out;  /*!< 回写提示行指针（可 NULL；页面写校验提示用） */
    int margin;             /*!< 面板与屏幕左右边的间距（像素） */
} espaperplay_ui_kb_cfg_t;

/**
 * @brief 打开键盘输入模态（全屏覆盖层 + 底部面板 + 贴底键盘）。
 *
 * 布局为内容驱动的统一公式（标题/输入框/提示行/按钮区在上，键盘贴底），
 * 墨水屏光标闪烁在面板内禁用。返回模态根对象（调用方保存，关闭时
 * lv_obj_del 即可，输入框/提示行等子对象随之销毁）。
 *
 * @return 模态根对象（覆盖层）；内存不足返回 NULL。
 */
lv_obj_t *espaperplay_ui_kb_modal_open(const espaperplay_ui_kb_cfg_t *cfg);

/** 键盘面板标准按钮高度（缩放 + 下限；面板与确认框共用同一公式）。 */
int espaperplay_ui_modal_btn_h(void);

#ifdef __cplusplus
}
#endif
