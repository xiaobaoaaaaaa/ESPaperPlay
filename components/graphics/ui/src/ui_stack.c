/*
 * SPDX-FileCopyrightText: 2026 ESPaperPlay Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#include "esp_log.h"
#include "esp_timer.h"

#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "espaperplay_clock.h" /* 状态栏时间 */
#include "espaperplay_fonts.h" /* FreeType 字体加载 */
#include "espaperplay_gui.h"
#include "espaperplay_gui_lv.h"
#include "espaperplay_input.h"
#include "espaperplay_system.h"
#include "espaperplay_ui.h"
#include "espaperplay_wifi.h" /* WiFi 状态/强度图标 */
#include "icons_data.h"

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

/* 当前页状态栏（页面创建时登记，页面切换清屏后置 NULL）；统一调度定时器
 * 周期刷新它。声明前置：ui_switch_inner 在下方定义并引用 s_current_bar。 */
static espaperplay_ui_status_bar_t *s_current_bar = NULL;
static lv_timer_t *s_status_bar_timer = NULL;

/** 页面切换模式。 */
typedef enum {
    UI_SWITCH_POP = 0, /*!< 弹出栈顶并重建上一页 */
    UI_SWITCH_PUSH,    /*!< 压入新页 */
    UI_SWITCH_REPLACE, /*!< 替换栈顶页（栈深不变；空栈时等价压入） */
} ui_switch_mode_t;

/** 页面切换核心（LVGL 线程内）：exit 当前页 -> 清屏 -> enter 目标页。 */
static void ui_switch_inner(const espaperplay_ui_page_t *target, ui_switch_mode_t mode) {
    const espaperplay_ui_page_t *next = NULL;

    /* 旧页状态栏即将随清屏被删除：先释放其结构体并解除当前页登记，避免
     * 统一调度定时器在切换窗口内访问已释放对象（LVGL 单线程，定时器与本
     * 函数同线程，但清屏后到新页 enter 前存在空窗）。LVGL 对象由
     * lv_obj_clean 删除，此处仅释放 malloc 出的状态栏结构体。 */
    if (s_current_bar != NULL) {
        free(s_current_bar);
        s_current_bar = NULL;
    }

    if (mode == UI_SWITCH_PUSH) {
        if (s_depth > 0 && s_stack[s_depth - 1].exit != NULL) {
            s_stack[s_depth - 1].exit();
        }
        s_stack[s_depth++] = *target;
        next = &s_stack[s_depth - 1]; /* 新页（栈内副本） */
    } else if (mode == UI_SWITCH_POP) {
        if (s_depth <= 1) {
            return; /* 根页面不可弹出 */
        }
        if (s_stack[s_depth - 1].exit != NULL) {
            s_stack[s_depth - 1].exit();
        }
        s_depth--;
        next = &s_stack[s_depth - 1]; /* 重建栈顶上一页（pop 时 target 为 NULL，勿解引用） */
    } else { /* UI_SWITCH_REPLACE：替换栈顶（引导页完成后以主界面替换自身） */
        if (s_depth > 0 && s_stack[s_depth - 1].exit != NULL) {
            s_stack[s_depth - 1].exit();
        }
        if (s_depth == 0) {
            s_depth++; /* 空栈时等价于压入 */
        }
        s_stack[s_depth - 1] = *target;
        next = &s_stack[s_depth - 1];
    }

    lv_obj_clean(lv_screen_active());
    if (next->enter != NULL) {
        next->enter();
    }
    ESP_LOGI(TAG, "page switch: depth %u", (unsigned)s_depth);
}

/** 跨线程投递用包装（LVGL 线程内执行）。 */
static void ui_switch_push_cb(void *arg) {
    ui_switch_inner((const espaperplay_ui_page_t *)arg, UI_SWITCH_PUSH);
}

static void ui_switch_pop_cb(void *arg) {
    (void)arg;
    ui_switch_inner(NULL, UI_SWITCH_POP);
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
    ui_switch_inner(NULL, UI_SWITCH_POP);
    return ESP_OK;
}

esp_err_t espaperplay_ui_page_replace_lv(const espaperplay_ui_page_t *page) {
    if (page == NULL || page->enter == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    ui_switch_inner(page, UI_SWITCH_REPLACE);
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
            ui_switch_inner(NULL, UI_SWITCH_POP);
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
        ESP_LOGW(TAG, "page handle_touch: dropped event=%p depth=%u", (void *)event,
                 (unsigned)s_depth);
        return;
    }
    const espaperplay_ui_page_t *top = &s_stack[s_depth - 1];
    ESP_LOGD(TAG, "page handle_touch: depth=%u has_on_touch=%d seq=%u pressed=%u (%u,%u)",
             (unsigned)s_depth, top->on_touch != NULL, event->touch_seq, event->touch_pressed,
             event->point.x, event->point.y);
    if (top->on_touch != NULL) {
        const uint32_t start_ms = (uint32_t)(esp_timer_get_time() / 1000);
        top->on_touch(event);
        const uint32_t cost_ms = (uint32_t)(esp_timer_get_time() / 1000) - start_ms;
        ESP_LOGD(TAG, "page handle_touch: on_touch done cost %u ms seq=%u", (unsigned)cost_ms,
                 event->touch_seq);
        if (cost_ms > 50) {
            ESP_LOGW(TAG, "page handle_touch: on_touch cost %u ms seq=%u — 可能阻塞 LVGL 线程",
                     (unsigned)cost_ms, event->touch_seq);
        }
    } else {
        ESP_LOGD(TAG, "page handle_touch: no on_touch handler, event ignored seq=%u",
                 event->touch_seq);
    }
}

uint8_t espaperplay_ui_page_depth(void) { return s_depth; }

/* ====================================================================
 * 睡眠/节能指示图标
 * ====================================================================
 * 由 power 任务在进睡前设置、用户唤醒时清除（状态存于 input 组件，
 * 避免 ui<->power 循环依赖）。图标创建于各页面状态/标题栏，刷新时
 * 依据当前指示状态显隐；定时器唤醒的局部刷新不会重建页面，故图标
 * 状态得以保留。
 *
 * 进睡前绘制：power 不能直接依赖 ui（否则 power->ui->power 循环依赖），
 * 故经 input 注册回调；回调在 LVGL 线程内显示图标并强制渲染 + 等待
 * EPD 刷新完成，确保睡眠前图标已落到屏上（睡眠期间屏幕冻结）。
 */

/* ====================================================================
 * 统一状态栏（顶栏）
 * ====================================================================
 * 各页面（测试页除外）共用同一套顶栏：左侧时间、居中标题、右侧 WiFi
 * 强度图标与睡眠/节能指示图标。睡眠图标位置随右侧图标数量动态调整
 * （仅 WiFi 时靠右；叠加睡眠图标时睡眠图标左移）。
 *
 * 统一调度：espaperplay_ui_status_bar_init() 创建 1s 周期定时器，刷新
 * "当前页状态栏"（s_current_bar）。睡眠指示标志由 power 在进睡前置位、
 * 用户唤醒时清除（状态存于 input 组件，避免 ui<->power 循环依赖），
 * 定时器在标志变化后 ~1s 内刷新图标，不受各页面自身刷新间隔影响。
 *
 * 进睡前绘制：power 不能直接依赖 ui（否则 power->ui->power 循环依赖），
 * 故经 input 注册回调；回调在 LVGL 线程内显示图标并强制渲染 + 等待 EPD
 * 刷新完成，确保睡眠前图标已落到屏上（睡眠期间屏幕冻结）。
 */

struct espaperplay_ui_status_bar_t {
    lv_obj_t *bar;                   /*!< 顶栏容器 */
    lv_obj_t *time;                  /*!< 左侧时间 HH:MM */
    lv_obj_t *title;                 /*!< 居中标题（无标题时为 NULL） */
    lv_obj_t *wifi;                  /*!< 右侧 WiFi 强度图标 */
    lv_obj_t *sleep;                 /*!< 右侧睡眠/节能指示图标 */
    char prev_time[8];               /*!< 上次显示的时间（变化才重绘） */
    const lv_image_dsc_t *prev_wifi; /*!< 上次显示的 WiFi 图标（变化才重绘） */
    char prev_title[64];             /*!< 上次显示的标题（变化才重绘） */
    bool prev_sleep_shown;           /*!< 上次睡眠图标显隐状态（变化才重布局） */
    bool live_clock;                 /*!< 睡眠期间时钟是否仍实时（主界面=true） */
};

/** 状态栏字体（与全局选用字体一致）。 */
static lv_font_t *ui_bar_font(uint32_t size_px) {
    return espaperplay_fonts_load(espaperplay_system_get_config()->selected_font, size_px,
                                  ESPAPERPLAY_FONT_STYLE_NORMAL);
}

espaperplay_ui_status_bar_t *espaperplay_ui_status_bar_create(lv_obj_t *scr, int height_px,
                                                              const char *title, bool live_clock) {
    if (scr == NULL) {
        return NULL;
    }
    espaperplay_ui_status_bar_t *bar = malloc(sizeof(*bar));
    if (bar == NULL) {
        return NULL;
    }
    memset(bar, 0, sizeof(*bar));
    bar->live_clock = live_clock;

    bar->bar = lv_obj_create(scr);
    lv_obj_set_size(bar->bar, LV_PCT(100), height_px);
    lv_obj_set_pos(bar->bar, 0, 0);
    lv_obj_set_style_bg_color(bar->bar, lv_color_white(), 0);
    lv_obj_set_style_border_width(bar->bar, 0, 0);
    lv_obj_set_style_radius(bar->bar, 0, 0);
    lv_obj_set_style_pad_all(bar->bar, 0, 0);
    lv_obj_remove_flag(bar->bar, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *line = lv_obj_create(scr);
    lv_obj_set_size(line, LV_PCT(100), 2);
    lv_obj_set_pos(line, 0, height_px - 2);
    lv_obj_set_style_bg_color(line, lv_color_black(), 0);
    lv_obj_set_style_border_width(line, 0, 0);
    lv_obj_set_style_radius(line, 0, 0);

    /* 左侧时间（16px，左对齐） */
    bar->time = lv_label_create(bar->bar);
    lv_label_set_text(bar->time, "--:--");
    lv_obj_set_style_text_color(bar->time, lv_color_black(), 0);
    lv_obj_set_style_text_font(bar->time, ui_bar_font(16), 0);
    lv_obj_set_style_text_align(bar->time, LV_TEXT_ALIGN_LEFT, 0);
    lv_obj_set_width(bar->time, LV_PCT(100));
    lv_obj_set_pos(bar->time, 12, 4);

    /* 居中标题（20px）：始终创建标签，便于页面后续经 set_title 动态更新
     * （如天气页位置名）。无初始标题时文本为空，不占用视觉空间。 */
    bar->title = lv_label_create(bar->bar);
    lv_label_set_text(bar->title, (title != NULL && title[0] != '\0') ? title : "");
    lv_obj_set_style_text_color(bar->title, lv_color_black(), 0);
    lv_obj_set_style_text_font(bar->title, ui_bar_font(20), 0);
    lv_obj_set_style_text_align(bar->title, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(bar->title, LV_PCT(100));
    lv_obj_set_pos(bar->title, 0, 3);
    if (title != NULL && title[0] != '\0') {
        strlcpy(bar->prev_title, title, sizeof(bar->prev_title));
    }

    /* 右侧 WiFi 强度图标（16x16 A8，默认黑色绘制） */
    bar->wifi = lv_image_create(bar->bar);
    lv_image_set_src(bar->wifi, &icon_wifi_off_16);
    lv_obj_align(bar->wifi, LV_ALIGN_TOP_RIGHT, -12, 7);
    bar->prev_wifi = &icon_wifi_off_16;

    /* 右侧睡眠/节能指示图标（默认隐藏，位于 WiFi 左侧） */
    bar->sleep = lv_image_create(bar->bar);
    lv_image_set_src(bar->sleep, &icon_leaf_16);
    lv_obj_align(bar->sleep, LV_ALIGN_TOP_RIGHT, -32, 7);
    lv_obj_add_flag(bar->sleep, LV_OBJ_FLAG_HIDDEN);
    bar->prev_sleep_shown = false;

    s_current_bar = bar; /* 登记为当前页状态栏（统一调度） */
    return bar;
}

/** 按右侧图标显隐重布局（仅睡眠图标显隐变化时调用，避免无谓重绘）。 */
static void ui_bar_layout_icons(espaperplay_ui_status_bar_t *bar) {
    lv_obj_align(bar->wifi, LV_ALIGN_TOP_RIGHT, -12, 7);
    if (bar->prev_sleep_shown) {
        lv_obj_align(bar->sleep, LV_ALIGN_TOP_RIGHT, -32, 7);
    }
}

void espaperplay_ui_status_bar_refresh(espaperplay_ui_status_bar_t *bar) {
    if (bar == NULL) {
        return;
    }
    char buf[16];
    struct tm tm;
    const bool sleeping = espaperplay_input_is_sleep_indicator();
    if (sleeping && !bar->live_clock) {
        /* 睡眠且本页无定时器唤醒（非主界面）：时钟冻结，显示占位避免误导。 */
        snprintf(buf, sizeof(buf), "--:--");
    } else if (espaperplay_clock_get_local_time(&tm) == ESP_OK && tm.tm_year >= 124) {
        snprintf(buf, sizeof(buf), "%02d:%02d", tm.tm_hour, tm.tm_min);
    } else {
        snprintf(buf, sizeof(buf), "--:--");
    }
    if (strcmp(bar->prev_time, buf) != 0) {
        strlcpy(bar->prev_time, buf, sizeof(bar->prev_time));
        lv_label_set_text(bar->time, buf);
    }

    /* WiFi 强度图标：AP 热点 / STA 按 RSSI 分档 / 未连接 */
    const lv_image_dsc_t *wifi_icon = &icon_wifi_off_16;
    espaperplay_wifi_status_t ws;
    if (espaperplay_wifi_get_status(&ws) == ESP_OK && ws.started) {
        if (ws.mode == ESPAPERPLAY_WIFI_MODE_AP) {
            wifi_icon = &icon_wifi_ap_16;
        } else if (ws.connected) {
            int rssi = 0;
            if (espaperplay_wifi_get_rssi(&rssi) == ESP_OK) {
                if (rssi >= -50) {
                    wifi_icon = &icon_wifi4_16;
                } else if (rssi >= -60) {
                    wifi_icon = &icon_wifi3_16;
                } else if (rssi >= -70) {
                    wifi_icon = &icon_wifi2_16;
                } else {
                    wifi_icon = &icon_wifi1_16;
                }
            } else {
                wifi_icon = &icon_wifi_16;
            }
        }
    }
    if (bar->prev_wifi != wifi_icon) {
        bar->prev_wifi = wifi_icon;
        lv_image_set_src(bar->wifi, wifi_icon);
    }

    /* 睡眠/节能指示图标：显隐变化时才改标志 + 重布局（避免无谓重绘/局刷） */
    const bool sleep_shown = espaperplay_input_is_sleep_indicator();
    if (sleep_shown != bar->prev_sleep_shown) {
        bar->prev_sleep_shown = sleep_shown;
        if (sleep_shown) {
            lv_obj_remove_flag(bar->sleep, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(bar->sleep, LV_OBJ_FLAG_HIDDEN);
        }
        ui_bar_layout_icons(bar);
    }
}

void espaperplay_ui_status_bar_set_title(espaperplay_ui_status_bar_t *bar, const char *title) {
    if (bar == NULL || bar->title == NULL) {
        return;
    }
    if (title == NULL) {
        title = "";
    }
    if (strcmp(bar->prev_title, title) != 0) {
        strlcpy(bar->prev_title, title, sizeof(bar->prev_title));
        lv_label_set_text(bar->title, title);
    }
}

/** 统一调度定时器：1s 刷新当前页状态栏（时间/WiFi/睡眠图标）。
 * 睡眠图标与 WiFi 图标走同一局部刷新路径；进睡/唤醒前由电源管理留出约
 * 2s 窗口，使定时器把图标显隐真正绘制到屏上，无需额外全刷同步。 */
static void ui_status_bar_timer_cb(lv_timer_t *timer) {
    (void)timer;
    if (s_current_bar != NULL) {
        espaperplay_ui_status_bar_refresh(s_current_bar);
    }
}

void espaperplay_ui_status_bar_init(void) {
    if (s_status_bar_timer == NULL) {
        s_status_bar_timer = lv_timer_create(ui_status_bar_timer_cb, 1000, NULL);
        if (s_status_bar_timer == NULL) {
            ESP_LOGW(TAG, "status bar scheduler timer create failed");
        }
    }
}
