/*
 * SPDX-FileCopyrightText: 2026 ESPaperPlay Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#include "esp_log.h"

#include "espaperplay_gui_lv.h"
#include "espaperplay_ui.h"

#include "lvgl.h"

static const char *TAG = "ESPaperPlay_UI";

/* ====================================================================
 * 页面栈
 * ====================================================================
 *
 * 单屏幕重建式导航：
 *   - push：当前页 exit（清理定时器等）-> 清空屏幕 -> 新页 enter 构建；
 *   - pop ：当前页 exit -> 清空屏幕 -> 栈顶上一页 enter 重建。
 * 每次只存在一个页面的 widget 树（内存最优）；页面自身状态由页面
 * 内部 static 持有，重建后重新初始化。所有操作经投递在 LVGL 线程
 * 串行执行（线程安全）。
 */

static espaperplay_ui_page_t s_stack[ESPAPERPLAY_UI_PAGE_MAX];
static uint8_t s_depth = 0;

/** 页面切换核心（LVGL 线程内）：exit 当前页 -> 清屏 -> enter 目标页。 */
static void ui_switch_inner(void *arg) {
    const bool to_push = (arg != NULL);
    const espaperplay_ui_page_t *target = (const espaperplay_ui_page_t *)arg;

    if (to_push) {
        if (s_depth > 0 && s_stack[s_depth - 1].exit != NULL) {
            s_stack[s_depth - 1].exit();
        }
        s_stack[s_depth++] = *target;
    } else {
        if (s_depth <= 1) {
            return; /* 根页面不可弹出 */
        }
        if (s_stack[s_depth - 1].exit != NULL) {
            s_stack[s_depth - 1].exit();
        }
        s_depth--;
    }

    lv_obj_clean(lv_screen_active());
    if (target->enter != NULL) {
        target->enter();
    }
    ESP_LOGI(TAG, "page switch: depth %u", (unsigned)s_depth);
}

esp_err_t espaperplay_ui_page_push(const espaperplay_ui_page_t *page) {
    if (page == NULL || page->enter == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_depth >= ESPAPERPLAY_UI_PAGE_MAX) {
        return ESP_ERR_NO_MEM; /* 栈满 */
    }
    return espaperplay_gui_lv_call(ui_switch_inner, (void *)page, 2000);
}

esp_err_t espaperplay_ui_page_pop(void) {
    if (s_depth <= 1) {
        return ESP_ERR_NOT_FOUND;
    }
    return espaperplay_gui_lv_call(ui_switch_inner, NULL, 2000);
}

uint8_t espaperplay_ui_page_depth(void) {
    return s_depth;
}
