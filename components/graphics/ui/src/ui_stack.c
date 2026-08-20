/*
 * SPDX-FileCopyrightText: 2026 ESPaperPlay Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#include "esp_log.h"

#include "espaperplay_gui.h"
#include "espaperplay_gui_lv.h"
#include "espaperplay_system.h"
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
static void ui_switch_inner(const espaperplay_ui_page_t *target, bool to_push) {
    const espaperplay_ui_page_t *next = NULL;

    if (to_push) {
        if (s_depth > 0 && s_stack[s_depth - 1].exit != NULL) {
            s_stack[s_depth - 1].exit();
        }
        s_stack[s_depth++] = *target;
        next = &s_stack[s_depth - 1]; /* 新页（栈内副本） */
    } else {
        if (s_depth <= 1) {
            return; /* 根页面不可弹出 */
        }
        if (s_stack[s_depth - 1].exit != NULL) {
            s_stack[s_depth - 1].exit();
        }
        s_depth--;
        next = &s_stack[s_depth - 1]; /* 重建栈顶上一页（pop 时 target 为 NULL，勿解引用） */
    }

    lv_obj_clean(lv_screen_active());
    if (next->enter != NULL) {
        next->enter();
    }
    ESP_LOGI(TAG, "page switch: depth %u", (unsigned)s_depth);
}

/** 跨线程投递用包装（LVGL 线程内执行）。 */
static void ui_switch_push_cb(void *arg) {
    ui_switch_inner((const espaperplay_ui_page_t *)arg, true);
}

static void ui_switch_pop_cb(void *arg) {
    (void)arg;
    ui_switch_inner(NULL, false);
}

esp_err_t espaperplay_ui_page_push(const espaperplay_ui_page_t *page) {
    if (page == NULL || page->enter == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_depth >= ESPAPERPLAY_UI_PAGE_MAX) {
        return ESP_ERR_NO_MEM; /* 栈满 */
    }
    return espaperplay_gui_lv_call(ui_switch_push_cb, (void *)page, 2000);
}

esp_err_t espaperplay_ui_page_pop(void) {
    if (s_depth <= 1) {
        return ESP_ERR_NOT_FOUND;
    }
    return espaperplay_gui_lv_call(ui_switch_pop_cb, NULL, 2000);
}

esp_err_t espaperplay_ui_page_push_lv(const espaperplay_ui_page_t *page) {
    if (page == NULL || page->enter == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_depth >= ESPAPERPLAY_UI_PAGE_MAX) {
        return ESP_ERR_NO_MEM; /* 栈满 */
    }
    ui_switch_inner(page, true);
    return ESP_OK;
}

esp_err_t espaperplay_ui_page_pop_lv(void) {
    if (s_depth <= 1) {
        return ESP_ERR_NOT_FOUND;
    }
    ui_switch_inner(NULL, false);
    return ESP_OK;
}

/**
 * @brief BOOT 键长按的全局默认动作（须在 LVGL 线程内调用）。
 *
 * 在 LONG_PRESS_START 时刻响应一次（不等松开、HOLD/松开事件不再触发，
 * 不重复响应），动作经 Web 管理页配置（NVS 持久化，默认全屏刷新）：
 *   - FULL_REFRESH：对当前帧整屏深刷新清残影（异步排队，不阻塞 LVGL）；
 *   - BACK：返回上一页（根页面不弹）；
 *   - NONE：无操作。
 * 对所有页面统一生效（主界面、天气页及后续新增页面），页面自身的 on_key
 * 仍会收到该事件，可做额外处理。
 */
static void ui_boot_long_press_default(void) {
    switch (espaperplay_system_get_boot_long_press_action()) {
    case ESPAPERPLAY_BOOT_LONG_PRESS_FULL_REFRESH:
        ESP_LOGI(TAG, "boot long press: full refresh (queued)");
        (void)espaperplay_gui_full_refresh();
        break;
    case ESPAPERPLAY_BOOT_LONG_PRESS_BACK:
        if (s_depth > 1) {
            ESP_LOGI(TAG, "boot long press: pop back");
            ui_switch_inner(NULL, false);
        }
        break;
    case ESPAPERPLAY_BOOT_LONG_PRESS_NONE:
    default:
        break;
    }
}

void espaperplay_ui_page_handle_key_lv(const espaperplay_input_event_t *event) {
    if (event == NULL || s_depth == 0) {
        return;
    }
    /* 全局默认长按功能：BOOT 键 LONG_PRESS_START 时刻响应一次（避免重复
     * 响应：仅挂钩 START，HOLD / UP 不触发）。随后事件照常转发给当前页
     * 的 on_key。 */
    if (event->type == ESPAPERPLAY_INPUT_EVENT_KEY &&
        event->key_id == ESPAPERPLAY_INPUT_KEY_ID_BOOT &&
        event->key_action == ESPAPERPLAY_INPUT_KEY_ACTION_LONG_PRESS_START) {
        ui_boot_long_press_default();
    }
    const espaperplay_ui_page_t *top = &s_stack[s_depth - 1];
    if (top->on_key != NULL) {
        top->on_key(event);
    }
}

void espaperplay_ui_page_handle_touch_lv(const espaperplay_input_event_t *event) {
    if (event == NULL || s_depth == 0) {
        return;
    }
    const espaperplay_ui_page_t *top = &s_stack[s_depth - 1];
    if (top->on_touch != NULL) {
        top->on_touch(event);
    }
}

uint8_t espaperplay_ui_page_depth(void) { return s_depth; }
