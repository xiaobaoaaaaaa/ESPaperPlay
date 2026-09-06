/*
 * SPDX-FileCopyrightText: 2026 ESPaperPlay Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_wifi.h"

#include "espaperplay_fonts.h"
#include "espaperplay_gui_lv.h"
#include "espaperplay_input.h"
#include "espaperplay_system.h"
#include "espaperplay_ui.h"
#include "espaperplay_ui_touch.h"
#include "espaperplay_wifi.h"
#include "icons_data.h"

#include "lvgl.h"

static const char *TAG = "ESPaperPlay_UI";

/* ====================================================================
 * WiFi 列表页（扫描附近网络并选择连接）
 * ====================================================================
 *
 * 从设置页「WiFi 网络」行进入。进入即自动扫描附近 AP（扫描在工作任务
 * 中执行，LVGL 线程仅读取结果缓存），按信号强度降序列出；点选网络：
 *   - 加密网络：弹出密码键盘模态（与引导页同一交互范式：底部面板 +
 *     lv_keyboard，密码显隐切换，光标禁闪烁防连续局刷）；
 *   - 开放网络：免密直接连接。
 *
 * 连接流程与引导页一致：worker 任务中 set_sta_credentials + set_wifi_
 * mode(STA) + wifi_start，随后 lv_timer 轮询 wifi_get_status；连接中
 * 切换到独立视图（大号状态 + 取消按钮），成功 / 失败后回到列表视图。
 *
 * 失败兜底：连接前记住系统配置中的原工作模式，取消 / 超时 / 应用失败
 * 且原模式为 AP 时自动恢复热点模式，保证设备仍可经 WebUI 访问（运行期
 * STA 重试耗尽不会自动回退 AP，若不恢复会与热点一起失联）。原模式为
 * STA 时不动，用户可在列表中改选其他网络（重新 wifi_start 即可）。
 *
 * 手动输入隐藏网络 SSID 不在本页（墨水屏长文本输入体验差），仍走设置
 * 页「WiFi 凭据」行的 Web 管理页配置。
 *
 * 线程模型（与设置页 / 引导页一致）：
 *   - NVS 写入 + wifi_start 必须离开 LVGL 任务（LVGL 任务栈在 PSRAM，
 *     flash 操作禁缓存期间访问 PSRAM 栈会崩溃）→ 投递到内部 RAM 栈
 *     worker 任务；worker 完成后经 espaperplay_gui_lv_call 回 LVGL；
 *   - 扫描（阻塞 2~4s）同样只在 worker 中执行；
 *   - 连接结果轮询用 lv_timer 在 LVGL 线程读 WiFi 服务内存态快照。
 */

#define WIFI_LIST_REF_H 800                /* 基准逻辑高度（缩放参考） */
#define WIFI_LIST_BAR_H 30                 /* 标题栏高度（基准） */
#define WIFI_LIST_MIN_H 24                 /* 标题栏最小高度 */
#define WIFI_LIST_MARGIN 16                /* 内容与屏幕边缘间距 */
#define WIFI_LIST_ROW_H 44                 /* 列表行高度（基准） */
#define WIFI_LIST_BTN_H 52                 /* 按钮高度（基准） */
#define WIFI_LIST_POLL_PERIOD_MS 500       /* 连接状态轮询周期 */
#define WIFI_LIST_CONNECT_TIMEOUT_MS 45000 /* 连接判定超时 */

/** 页面视图。 */
typedef enum {
    WIFI_LIST_VIEW_LIST = 0, /*!< 列表视图（扫描结果） */
    WIFI_LIST_VIEW_CONNECTING, /*!< 连接中视图 */
} wifi_list_view_t;

/** worker 操作类型（NVS 写入 / 服务应用全部离开 LVGL 线程）。 */
typedef enum {
    WIFI_LIST_OP_SCAN = 0,  /*!< 阻塞扫描附近 AP（结果写入 wifi 服务缓存） */
    WIFI_LIST_OP_CONNECT,   /*!< 保存 STA 凭据并切换连接 */
    WIFI_LIST_OP_RESTORE_AP, /*!< 恢复 AP 热点模式（连接失败兜底） */
} wifi_list_op_type_t;

typedef struct {
    wifi_list_op_type_t type;                  /*!< 操作类型 */
    char ssid[ESPAPERPLAY_SYSTEM_SSID_MAX_LEN];   /*!< CONNECT：目标网络名 */
    char pass[ESPAPERPLAY_SYSTEM_PASS_MAX_LEN];   /*!< CONNECT：目标网络密码 */
} wifi_list_op_t;

/* ------------------------------------------------------------------ */
/* 页面状态                                                             */
/* ------------------------------------------------------------------ */

static espaperplay_ui_status_bar_t *s_bar = NULL; /*!< 统一状态栏 */
static lv_obj_t *s_content = NULL;                /*!< 当前视图内容容器（切视图重建） */
static wifi_list_view_t s_view = WIFI_LIST_VIEW_LIST; /*!< 当前视图 */
static bool s_page_active = false;                /*!< 页面是否在栈顶（迟到回调守卫） */

static lv_obj_t *s_status_label = NULL; /*!< 状态提示行（扫描中 / 结果数 / 连接反馈） */
static lv_obj_t *s_list = NULL;         /*!< 扫描结果列表容器 */

/* 扫描结果快照（LVGL 线程内重建列表时拷贝，行点击经下标取回）。 */
static espaperplay_wifi_scan_item_t s_items[ESPAPERPLAY_WIFI_SCAN_MAX];
static size_t s_item_count = 0;
static bool s_scan_busy = false; /*!< 已投递扫描、尚未回 LVGL（防重复触发） */

/* 连接状态 */
static char s_target_ssid[ESPAPERPLAY_SYSTEM_SSID_MAX_LEN]; /*!< 正在连接的目标网络 */
static espaperplay_wifi_mode_t s_prev_mode = ESPAPERPLAY_WIFI_MODE_AP; /*!< 连接前的工作模式 */
static lv_timer_t *s_poll_timer = NULL; /*!< 连接状态轮询定时器 */
static uint32_t s_poll_elapsed_ms = 0;  /*!< 已等待毫秒数 */

/* 密码输入模态 */
static lv_obj_t *s_modal = NULL; /*!< 全屏覆盖层（NULL=未打开） */
static lv_obj_t *s_kb_ta = NULL; /*!< 输入框 */
static lv_obj_t *s_kb_hint = NULL; /*!< 校验提示行 */

static QueueHandle_t s_op_queue = NULL;   /*!< 操作队列 */
static TaskHandle_t s_worker_task = NULL; /*!< 工作任务（内部 RAM 栈） */

/* 手势跟踪（边缘 / 横滑返回） */
static bool s_touch_down = false;
static lv_point_t s_touch_start = {0, 0};
static lv_point_t s_touch_last = {0, 0};

/* 前向声明 */
static void wifi_list_show_view(wifi_list_view_t view);
static void wifi_list_rebuild_list(void);
static void wifi_list_modal_close(void);
static void wifi_list_open_keyboard(void);
static void wifi_list_connect_start(const char *pass);
static void wifi_list_poll_cb(lv_timer_t *timer);
static void wifi_list_status_set(const char *text);

/* ------------------------------------------------------------------ */
/* 工具函数                                                             */
/* ------------------------------------------------------------------ */

/** FreeType 字体按需加载（16/20/24 固定档位）。 */
static lv_font_t *wifi_list_font(int size_px) {
    return espaperplay_fonts_load(espaperplay_system_get_config()->selected_font,
                                  (uint32_t)size_px, ESPAPERPLAY_FONT_STYLE_NORMAL);
}

/** 通用标签：白底黑字 + FreeType 字体 + 禁用滚动。 */
static lv_obj_t *wifi_list_label_create(lv_obj_t *parent, const char *text, int font_px,
                                        lv_text_align_t align) {
    lv_obj_t *label = lv_label_create(parent);
    lv_label_set_text(label, text);
    lv_obj_set_style_text_color(label, lv_color_black(), 0);
    lv_font_t *font = wifi_list_font(font_px);
    if (font != NULL) {
        lv_obj_set_style_text_font(label, font, 0);
    }
    lv_obj_set_style_text_align(label, align, 0);
    lv_obj_set_width(label, LV_PCT(100));
    lv_obj_remove_flag(label, LV_OBJ_FLAG_SCROLLABLE);
    return label;
}

static float s_scale = 1.0f; /*!< 屏高缩放因子（enter 时计算） */

/** 基准值按屏高缩放（取整）。 */
static int wifi_list_scaled(int v) { return (int)(v * s_scale); }

/** 按钮高度（缩放 + 下限，保证可点按面积）。 */
static int wifi_list_btn_h(void) {
    const int h = wifi_list_scaled(WIFI_LIST_BTN_H);
    return h < 40 ? 40 : h;
}

/** 标题栏高度（缩放 + 下限）。 */
static int wifi_list_bar_h(void) {
    const int h = wifi_list_scaled(WIFI_LIST_BAR_H);
    return h < WIFI_LIST_MIN_H ? WIFI_LIST_MIN_H : h;
}

/** 创建一个按钮（primary: true=黑底白字主按钮，false=白底黑边次按钮）。 */
static lv_obj_t *wifi_list_button(lv_obj_t *parent, const char *text, int x, int y, int w, int h,
                                  bool primary, lv_event_cb_t cb) {
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
    lv_obj_set_style_text_font(label, wifi_list_font(20), 0);
    lv_obj_center(label);
    if (cb != NULL) {
        lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, NULL);
    }
    return btn;
}

/** RSSI 分档图标（与状态栏同款素材）。 */
static const lv_image_dsc_t *wifi_list_rssi_icon(int8_t rssi) {
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

/** 更新状态提示行（仅文本变化时写入，EPD 上避免无谓刷新）。 */
static void wifi_list_status_set(const char *text) {
    if (s_status_label != NULL && strcmp(lv_label_get_text(s_status_label), text) != 0) {
        lv_label_set_text(s_status_label, text);
    }
}

/* ------------------------------------------------------------------ */
/* 工作任务（NVS 写入 / 阻塞扫描必须在内部 RAM 栈任务中执行）              */
/* ------------------------------------------------------------------ */

/** 投递结果回 LVGL：扫描完成（arg 为 esp_err_t）。 */
static void wifi_list_scan_done_lv(void *arg) {
    const esp_err_t err = (esp_err_t)(intptr_t)arg;
    s_scan_busy = false;
    if (!s_page_active || s_view != WIFI_LIST_VIEW_LIST) {
        return; /* 已离开页面 / 切到连接视图：忽略迟到回调 */
    }
    if (err != ESP_OK) {
        char buf[96];
        if (err == ESP_ERR_WIFI_STATE) {
            snprintf(buf, sizeof(buf), "WiFi 正忙，请稍后重新扫描");
        } else {
            snprintf(buf, sizeof(buf), "扫描失败：%s", esp_err_to_name(err));
        }
        wifi_list_status_set(buf);
        return;
    }
    wifi_list_rebuild_list();
}

/** 投递结果回 LVGL：wifi_start 已成功，开始轮询连接状态。 */
static void wifi_list_connect_started_lv(void *arg) {
    (void)arg;
    if (!s_page_active || s_view != WIFI_LIST_VIEW_CONNECTING) {
        return;
    }
    s_poll_elapsed_ms = 0;
    if (s_status_label != NULL) {
        lv_label_set_text(s_status_label, "正在连接，请稍候…");
    }
    if (s_poll_timer == NULL) {
        s_poll_timer = lv_timer_create(wifi_list_poll_cb, WIFI_LIST_POLL_PERIOD_MS, NULL);
    }
}

/** 投递结果回 LVGL：应用配置失败（arg 为 esp_err_t）。 */
static void wifi_list_connect_failed_lv(void *arg) {
    const esp_err_t err = (esp_err_t)(intptr_t)arg;
    if (!s_page_active || s_view != WIFI_LIST_VIEW_CONNECTING) {
        return;
    }
    /* 应用失败（参数 / NVS 错误）时驱动未启动，无需恢复 AP；原模式为 AP
     * 也不会被改动（set_wifi_mode 未执行）。直接回列表视图提示。 */
    char buf[96];
    snprintf(buf, sizeof(buf), "应用网络配置失败：%s", esp_err_to_name(err));
    wifi_list_show_view(WIFI_LIST_VIEW_LIST);
    wifi_list_status_set(buf);
}

/** 工作任务：从队列取操作，执行扫描 / NVS 写入 + 服务应用。 */
static void wifi_list_worker_task(void *arg) {
    (void)arg;
    wifi_list_op_t op;
    for (;;) {
        if (xQueueReceive(s_op_queue, &op, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        switch (op.type) {
        case WIFI_LIST_OP_SCAN: {
            esp_err_t err = espaperplay_wifi_scan_start();
            if (err != ESP_OK) {
                ESP_LOGW(TAG, "wifi_list: scan failed: %s", esp_err_to_name(err));
            }
            espaperplay_gui_lv_call(wifi_list_scan_done_lv, (void *)(intptr_t)err, 2000);
            break;
        }
        case WIFI_LIST_OP_CONNECT: {
            esp_err_t err = espaperplay_system_set_sta_credentials(op.ssid, op.pass);
            if (err == ESP_OK) {
                err = espaperplay_system_set_wifi_mode(ESPAPERPLAY_WIFI_MODE_STA);
            }
            if (err == ESP_OK) {
                err = espaperplay_wifi_start();
            }
            if (err == ESP_OK) {
                ESP_LOGI(TAG, "wifi_list: connecting to \"%s\"", op.ssid);
                espaperplay_gui_lv_call(wifi_list_connect_started_lv, NULL, 1000);
            } else {
                ESP_LOGW(TAG, "wifi_list: apply wifi failed: %s", esp_err_to_name(err));
                espaperplay_gui_lv_call(wifi_list_connect_failed_lv, (void *)(intptr_t)err, 1000);
            }
            break;
        }
        case WIFI_LIST_OP_RESTORE_AP:
            /* 连接失败兜底：恢复 AP 热点，保证设备仍可经 WebUI 配置。 */
            if (espaperplay_system_set_wifi_mode(ESPAPERPLAY_WIFI_MODE_AP) == ESP_OK) {
                espaperplay_wifi_start();
            }
            break;
        default:
            break;
        }
    }
}

/** 确保工作任务已创建（首次使用时创建，常驻程序生命周期）。 */
static void wifi_list_worker_ensure(void) {
    if (s_op_queue != NULL && s_worker_task != NULL) {
        return;
    }
    if (s_op_queue == NULL) {
        s_op_queue = xQueueCreate(4, sizeof(wifi_list_op_t));
    }
    if (s_op_queue != NULL && s_worker_task == NULL) {
        /* 栈必须放内部 RAM（NVS/flash 操作禁用缓存期间可访问）；CONFIG_SPIRAM_USE_MALLOC
         * 下 xTaskCreate 默认栈在 PSRAM，须用 xTaskCreateWithCaps 显式指定。 */
        if (xTaskCreateWithCaps(wifi_list_worker_task, "ui_wifi", 4096, NULL, 4, &s_worker_task,
                                MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT) != pdPASS) {
            ESP_LOGE(TAG, "wifi_list: worker task create failed");
        }
    }
}

/** 投递一个操作（LVGL 线程内调用，非阻塞）。 */
static void wifi_list_op_post(const wifi_list_op_t *op) {
    wifi_list_worker_ensure();
    if (s_op_queue == NULL || xQueueSend(s_op_queue, op, 0) != pdTRUE) {
        ESP_LOGW(TAG, "wifi_list: op queue unavailable/full");
    }
}

/* ------------------------------------------------------------------ */
/* 连接轮询                                                             */
/* ------------------------------------------------------------------ */

/** 结束轮询（LVGL 线程内）。 */
static void wifi_list_poll_stop(void) {
    if (s_poll_timer != NULL) {
        lv_timer_delete(s_poll_timer);
        s_poll_timer = NULL;
    }
}

/** 连接失败收尾：原模式为 AP 时恢复热点，回列表视图并提示。 */
static void wifi_list_connect_fail(const char *reason) {
    wifi_list_poll_stop();
    const bool restore_ap = (s_prev_mode == ESPAPERPLAY_WIFI_MODE_AP);
    if (restore_ap) {
        wifi_list_op_t op = {.type = WIFI_LIST_OP_RESTORE_AP};
        wifi_list_op_post(&op);
    }
    wifi_list_show_view(WIFI_LIST_VIEW_LIST);
    wifi_list_status_set(reason);
}

/** 轮询定时器（LVGL 线程）：STA 获得 IP -> 成功；超时 -> 失败兜底。 */
static void wifi_list_poll_cb(lv_timer_t *timer) {
    (void)timer;
    if (!s_page_active || s_view != WIFI_LIST_VIEW_CONNECTING) {
        return;
    }
    s_poll_elapsed_ms += WIFI_LIST_POLL_PERIOD_MS;

    espaperplay_wifi_status_t st;
    if (espaperplay_wifi_get_status(&st) == ESP_OK &&
        st.mode == ESPAPERPLAY_WIFI_MODE_STA && st.connected) {
        wifi_list_poll_stop();
        wifi_list_show_view(WIFI_LIST_VIEW_LIST);
        char buf[ESPAPERPLAY_SYSTEM_SSID_MAX_LEN + 48];
        snprintf(buf, sizeof(buf), "已连接到「%s」（%s）", s_target_ssid, st.ip);
        wifi_list_status_set(buf);
        return;
    }

    if (s_status_label != NULL) {
        char buf[64];
        snprintf(buf, sizeof(buf), "正在连接，请稍候…（%u 秒）",
                 (unsigned)(s_poll_elapsed_ms / 1000));
        lv_label_set_text(s_status_label, buf);
    }

    if (s_poll_elapsed_ms >= WIFI_LIST_CONNECT_TIMEOUT_MS) {
        wifi_list_connect_fail(s_prev_mode == ESPAPERPLAY_WIFI_MODE_AP
                                   ? "连接超时，已恢复热点模式。"
                                   : "连接超时，请检查密码后重试。");
    }
}

/* ------------------------------------------------------------------ */
/* 视图构建                                                             */
/* ------------------------------------------------------------------ */

/** 扫描行点击（LVGL 线程）：加密 -> 密码模态；开放 -> 免密直连。 */
static void wifi_list_row_cb(lv_event_t *e) {
    lv_event_stop_bubbling(e);
    if (!s_page_active || s_view != WIFI_LIST_VIEW_LIST || s_modal != NULL || s_scan_busy) {
        return;
    }
    const intptr_t idx = (intptr_t)lv_event_get_user_data(e);
    if (idx < 0 || (size_t)idx >= s_item_count) {
        return;
    }
    strlcpy(s_target_ssid, s_items[idx].ssid, sizeof(s_target_ssid));
    if (s_items[idx].auth) {
        wifi_list_open_keyboard();
    } else {
        wifi_list_connect_start("");
    }
}

/** 重建扫描列表（LVGL 线程内，仅列表视图）。 */
static void wifi_list_rebuild_list(void) {
    if (s_list == NULL) {
        return;
    }
    lv_obj_clean(s_list);

    s_item_count =
        espaperplay_wifi_scan_get_results(s_items, ESPAPERPLAY_WIFI_SCAN_MAX);

    espaperplay_wifi_status_t st;
    const bool have_sta = (espaperplay_wifi_get_status(&st) == ESP_OK &&
                           st.mode == ESPAPERPLAY_WIFI_MODE_STA && st.connected);

    if (s_item_count == 0) {
        wifi_list_status_set("未发现附近网络，可稍后重新扫描");
        return;
    }

    const int32_t list_w = lv_obj_get_width(s_list);
    const int row_h = wifi_list_scaled(WIFI_LIST_ROW_H) < 36 ? 36
                                                             : wifi_list_scaled(WIFI_LIST_ROW_H);

    for (size_t i = 0; i < s_item_count; i++) {
        const bool is_current = (have_sta && strcmp(s_items[i].ssid, st.ssid) == 0);

        lv_obj_t *row = lv_obj_create(s_list);
        lv_obj_set_size(row, list_w, row_h);
        lv_obj_set_pos(row, 0, (int)(i * (row_h + 4)));
        lv_obj_set_style_bg_color(row, is_current ? lv_color_black() : lv_color_white(), 0);
        lv_obj_set_style_border_color(row, lv_color_black(), 0);
        lv_obj_set_style_border_width(row, is_current ? 0 : 1, 0);
        lv_obj_set_style_radius(row, 6, 0);
        lv_obj_set_style_pad_all(row, 0, 0);
        lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(row, wifi_list_row_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);

        /* SSID（左，超长省略） */
        lv_obj_t *label = lv_label_create(row);
        lv_label_set_text(label, s_items[i].ssid);
        lv_obj_set_style_text_color(label, is_current ? lv_color_white() : lv_color_black(), 0);
        lv_obj_set_style_text_font(label, wifi_list_font(16), 0);
        lv_obj_align(label, LV_ALIGN_LEFT_MID, 10, 0);
        lv_obj_set_width(label, list_w - 130);
        lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);

        /* 信道（信号图标左侧小字，补充强度信息便于区分同名网络） */
        char ch_buf[12];
        snprintf(ch_buf, sizeof(ch_buf), "CH%u", (unsigned)s_items[i].channel);
        lv_obj_t *ch = lv_label_create(row);
        lv_label_set_text(ch, ch_buf);
        lv_obj_set_style_text_color(ch, is_current ? lv_color_white() : lv_color_black(), 0);
        lv_obj_set_style_text_font(ch, wifi_list_font(16), 0);
        lv_obj_align(ch, LV_ALIGN_RIGHT_MID, -30, 0);

        /* 信号强度图标 */
        lv_obj_t *icon = lv_image_create(row);
        lv_image_set_src(icon, wifi_list_rssi_icon(s_items[i].rssi));
        lv_obj_set_style_image_recolor(icon, is_current ? lv_color_white() : lv_color_black(), 0);
        lv_obj_set_style_image_recolor_opa(icon, LV_OPA_COVER, 0);
        lv_obj_align(icon, LV_ALIGN_RIGHT_MID, -10, 0);

        /* 加密标记（信道文字前） */
        if (s_items[i].auth) {
            lv_obj_t *lock = lv_label_create(row);
            lv_label_set_text(lock, "加密");
            lv_obj_set_style_text_color(lock, is_current ? lv_color_white() : lv_color_black(), 0);
            lv_obj_set_style_text_font(lock, wifi_list_font(16), 0);
            lv_obj_align(lock, LV_ALIGN_RIGHT_MID, -76, 0);
        }
    }

    char buf[48];
    snprintf(buf, sizeof(buf), "已发现 %u 个网络，点选即可连接", (unsigned)s_item_count);
    wifi_list_status_set(buf);
    ESP_LOGI(TAG, "wifi_list: %u networks listed", (unsigned)s_item_count);
}

/** 「重新扫描」按钮点击。 */
static void wifi_list_rescan_cb(lv_event_t *e) {
    lv_event_stop_bubbling(e);
    if (!s_page_active || s_view != WIFI_LIST_VIEW_LIST || s_scan_busy || s_modal != NULL) {
        return;
    }
    s_scan_busy = true;
    wifi_list_status_set("正在扫描附近网络…");
    wifi_list_op_t op = {.type = WIFI_LIST_OP_SCAN};
    wifi_list_op_post(&op);
}

/** 构建列表视图（s_content 已重建）。 */
static void wifi_list_build_list_view(lv_obj_t *parent, int32_t scr_w, int32_t scr_h) {
    const int btn_h = wifi_list_btn_h();
    const int btn_y = scr_h - btn_h - wifi_list_scaled(16);

    s_status_label = wifi_list_label_create(parent, "正在扫描附近网络…", 16, LV_TEXT_ALIGN_CENTER);
    lv_obj_set_pos(s_status_label, 0, wifi_list_scaled(6));

    /* 扫描结果列表（可纵向滚动；EPD 禁弹性滚动与滚动条）。 */
    s_list = lv_obj_create(parent);
    lv_obj_set_size(s_list, scr_w - 2 * WIFI_LIST_MARGIN, btn_y - wifi_list_scaled(40));
    lv_obj_set_pos(s_list, WIFI_LIST_MARGIN, wifi_list_scaled(40));
    lv_obj_set_style_bg_color(s_list, lv_color_white(), 0);
    lv_obj_set_style_border_width(s_list, 0, 0);
    lv_obj_set_style_radius(s_list, 0, 0);
    lv_obj_set_style_pad_all(s_list, 0, 0);
    lv_obj_set_scroll_dir(s_list, LV_DIR_VER);
    lv_obj_add_flag(s_list, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(s_list, LV_SCROLLBAR_MODE_OFF);
    lv_obj_remove_flag(s_list, LV_OBJ_FLAG_SCROLL_ELASTIC);

    wifi_list_button(parent, "重新扫描", WIFI_LIST_MARGIN, btn_y, scr_w - 2 * WIFI_LIST_MARGIN,
                     btn_h, false, wifi_list_rescan_cb);

    /* 首次进入（扫描进行中）：保持空列表与扫描提示，等扫描完成回调重建；
     * 从连接视图返回：s_scan_busy=false，用缓存结果即时重建。 */
    if (!s_scan_busy) {
        wifi_list_rebuild_list();
    }
}

/** 「取消并返回」按钮点击：停止轮询，原模式为 AP 时恢复热点。 */
static void wifi_list_cancel_connect_cb(lv_event_t *e) {
    if (e != NULL) {
        lv_event_stop_bubbling(e);
    }
    if (!s_page_active) {
        return;
    }
    wifi_list_poll_stop();
    const bool restore_ap = (s_prev_mode == ESPAPERPLAY_WIFI_MODE_AP);
    if (restore_ap) {
        wifi_list_op_t op = {.type = WIFI_LIST_OP_RESTORE_AP};
        wifi_list_op_post(&op);
    }
    wifi_list_show_view(WIFI_LIST_VIEW_LIST);
    wifi_list_status_set(restore_ap ? "已取消连接，恢复热点模式" : "已取消连接");
}

/** 构建连接中视图。 */
static void wifi_list_build_connecting_view(lv_obj_t *parent, int32_t scr_w, int32_t scr_h) {
    (void)scr_h;
    lv_obj_t *title = wifi_list_label_create(parent, "正在连接网络", 24, LV_TEXT_ALIGN_CENTER);
    lv_obj_set_pos(title, 0, wifi_list_scaled(70));

    char ssid_line[ESPAPERPLAY_SYSTEM_SSID_MAX_LEN + 16];
    snprintf(ssid_line, sizeof(ssid_line), "「%s」", s_target_ssid);
    lv_obj_t *ssid_label = wifi_list_label_create(parent, ssid_line, 20, LV_TEXT_ALIGN_CENTER);
    lv_obj_set_pos(ssid_label, 0, wifi_list_scaled(130));

    s_status_label =
        wifi_list_label_create(parent, "正在应用网络配置…", 16, LV_TEXT_ALIGN_CENTER);
    lv_obj_set_pos(s_status_label, 0, wifi_list_scaled(170));

    wifi_list_button(parent, "取消并返回", WIFI_LIST_MARGIN, scr_h - wifi_list_btn_h() -
                                                                 wifi_list_scaled(16),
                     scr_w - 2 * WIFI_LIST_MARGIN, wifi_list_btn_h(), false,
                     wifi_list_cancel_connect_cb);
}

/** 切换视图：销毁旧内容容器并重建（EPD 只刷变化区域）。 */
static void wifi_list_show_view(wifi_list_view_t view) {
    s_view = view;
    s_status_label = NULL;
    s_list = NULL;
    if (s_content != NULL) {
        lv_obj_del(s_content);
        s_content = NULL;
    }
    int32_t scr_w = 0;
    int32_t scr_h = 0;
    lv_display_t *disp = lv_display_get_default();
    scr_w = lv_display_get_horizontal_resolution(disp);
    scr_h = lv_display_get_vertical_resolution(disp);

    s_content = lv_obj_create(lv_screen_active());
    lv_obj_set_size(s_content, scr_w, scr_h - wifi_list_bar_h());
    lv_obj_set_pos(s_content, 0, wifi_list_bar_h());
    lv_obj_set_style_bg_color(s_content, lv_color_white(), 0);
    lv_obj_set_style_border_width(s_content, 0, 0);
    lv_obj_set_style_radius(s_content, 0, 0);
    lv_obj_set_style_pad_all(s_content, 0, 0);
    lv_obj_remove_flag(s_content, LV_OBJ_FLAG_SCROLLABLE);

    if (view == WIFI_LIST_VIEW_LIST) {
        wifi_list_build_list_view(s_content, scr_w, scr_h - wifi_list_bar_h());
    } else {
        wifi_list_build_connecting_view(s_content, scr_w, scr_h - wifi_list_bar_h());
    }
}

/* ------------------------------------------------------------------ */
/* 密码输入模态（与引导页同一交互范式，仅密码一种）                        */
/* ------------------------------------------------------------------ */

/** 密码可见性切换。 */
static void wifi_list_kb_toggle_cb(lv_event_t *e) {
    lv_event_stop_bubbling(e);
    if (s_kb_ta == NULL) {
        return;
    }
    const bool hidden = lv_textarea_get_password_mode(s_kb_ta);
    lv_textarea_set_password_mode(s_kb_ta, !hidden);
    lv_label_set_text(lv_obj_get_child(lv_event_get_current_target(e), 0), hidden ? "隐藏" : "显示");
}

/** 密码模态 取消 按钮：关闭模态，留在列表。 */
static void wifi_list_kb_cancel_cb(lv_event_t *e) {
    lv_event_stop_bubbling(e);
    wifi_list_modal_close();
}

/** 密码模态 连接 按钮：校验非空 -> 发起连接。 */
static void wifi_list_kb_ok_cb(lv_event_t *e) {
    lv_event_stop_bubbling(e);
    if (s_kb_ta == NULL) {
        return;
    }
    const char *text = lv_textarea_get_text(s_kb_ta);
    if (strlen(text) >= ESPAPERPLAY_SYSTEM_PASS_MAX_LEN) {
        if (s_kb_hint != NULL) {
            lv_label_set_text(s_kb_hint, "密码过长");
        }
        return;
    }
    if (strlen(text) == 0) {
        if (s_kb_hint != NULL) {
            lv_label_set_text(s_kb_hint, "请输入密码");
        }
        return;
    }
    char pass[ESPAPERPLAY_SYSTEM_PASS_MAX_LEN];
    strlcpy(pass, text, sizeof(pass));
    wifi_list_modal_close();
    wifi_list_connect_start(pass);
}

/**
 * 打开密码输入模态（底部面板 + 键盘贴底，与引导页一致）。
 * 输入模态不可点空白关闭（防误触丢输入）。
 */
static void wifi_list_open_keyboard(void) {
    int32_t scr_w = 0;
    int32_t scr_h = 0;
    lv_display_t *disp = lv_display_get_default();
    scr_w = lv_display_get_horizontal_resolution(disp);
    scr_h = lv_display_get_vertical_resolution(disp);

    /* 全屏覆盖层（不点空白关闭），背景透明避免 BW 模式下整页变白。 */
    s_modal = lv_obj_create(lv_screen_active());
    lv_obj_set_size(s_modal, scr_w, scr_h);
    lv_obj_set_pos(s_modal, 0, 0);
    lv_obj_set_style_bg_opa(s_modal, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_modal, 0, 0);
    lv_obj_set_style_radius(s_modal, 0, 0);
    lv_obj_set_style_pad_all(s_modal, 0, 0);
    lv_obj_remove_flag(s_modal, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_modal, LV_OBJ_FLAG_CLICKABLE);

    /* 底部面板尺寸（内容驱动，横竖屏自适应）。 */
    const int panel_w = scr_w - 2 * WIFI_LIST_MARGIN;
    const int pad = 10;
    const int title_h = 30;
    const int ta_h = wifi_list_scaled(52) < 40 ? 40 : wifi_list_scaled(52);
    const int hint_h = 22;
    const int bh = wifi_list_btn_h() < 38 ? 38 : wifi_list_btn_h();
    const int kb_h = wifi_list_scaled(240) < 170 ? 170 : wifi_list_scaled(240);
    const int panel_h = pad + title_h + 6 + ta_h + 4 + hint_h + 6 + bh + pad;
    const int kb_y = scr_h - kb_h - 6;      /* 键盘贴底 */
    const int panel_y = kb_y - panel_h - 6; /* 面板位于键盘上方 */

    lv_obj_t *panel = lv_obj_create(s_modal);
    lv_obj_set_size(panel, panel_w, panel_h);
    lv_obj_set_pos(panel, WIFI_LIST_MARGIN, panel_y);
    lv_obj_set_style_bg_color(panel, lv_color_white(), 0);
    lv_obj_set_style_border_color(panel, lv_color_black(), 0);
    lv_obj_set_style_border_width(panel, 2, 0);
    lv_obj_set_style_radius(panel, 12, 0);
    lv_obj_set_style_pad_all(panel, 0, 0);
    lv_obj_remove_flag(panel, LV_OBJ_FLAG_SCROLLABLE);

    /* 标题（含目标网络名）。 */
    char title_buf[ESPAPERPLAY_SYSTEM_SSID_MAX_LEN + 32];
    snprintf(title_buf, sizeof(title_buf), "连接「%s」", s_target_ssid);
    lv_obj_t *title = wifi_list_label_create(panel, title_buf, 20, LV_TEXT_ALIGN_CENTER);
    lv_obj_set_width(title, panel_w - 140);
    lv_obj_set_pos(title, 0, pad);
    lv_label_set_long_mode(title, LV_LABEL_LONG_DOT);

    /* 输入框（一行，密码模式）。 */
    lv_obj_t *ta = lv_textarea_create(panel);
    s_kb_ta = ta;
    lv_obj_set_size(ta, panel_w - 2 * pad, ta_h);
    lv_obj_set_pos(ta, pad, pad + title_h + 6);
    lv_textarea_set_one_line(ta, true);
    lv_textarea_set_max_length(ta, ESPAPERPLAY_SYSTEM_PASS_MAX_LEN - 1);
    lv_textarea_set_password_mode(ta, true);
    lv_textarea_set_text(ta, "");
    lv_obj_set_style_text_color(ta, lv_color_black(), 0);
    lv_obj_set_style_text_font(ta, wifi_list_font(20), 0);
    lv_obj_set_style_border_color(ta, lv_color_black(), 0);
    lv_obj_set_style_border_width(ta, 2, 0);
    lv_obj_set_style_radius(ta, 6, 0);
    lv_obj_set_style_pad_left(ta, 8, 0);
    /* 墨水屏：光标闪烁会触发连续局部刷新，禁用（anim_duration=0 即不闪烁）。 */
    lv_obj_set_style_anim_duration(ta, 0, LV_PART_CURSOR);
    lv_obj_set_style_anim_duration(ta, 0, LV_PART_CURSOR | LV_STATE_FOCUSED);

    /* 校验提示（默认空）。 */
    lv_obj_t *hint = wifi_list_label_create(panel, "", 16, LV_TEXT_ALIGN_LEFT);
    s_kb_hint = hint;
    lv_obj_set_width(hint, LV_PCT(100));
    lv_label_set_long_mode(hint, LV_LABEL_LONG_DOT);
    lv_obj_set_pos(hint, pad + 2, pad + title_h + 6 + ta_h + 4);

    /* 取消 / 连接。 */
    const int bw = (panel_w - 2 * pad - 12) / 2;
    const int btn_y = pad + title_h + 6 + ta_h + 4 + hint_h + 6;
    wifi_list_button(panel, "取消", pad, btn_y, bw, bh, false, wifi_list_kb_cancel_cb);
    wifi_list_button(panel, "连接", pad + bw + 12, btn_y, bw, bh, true, wifi_list_kb_ok_cb);

    /* 密码可见性切换（标题右侧）。 */
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
    lv_obj_set_style_text_font(tl, wifi_list_font(16), 0);
    lv_obj_center(tl);
    lv_obj_add_event_cb(toggle, wifi_list_kb_toggle_cb, LV_EVENT_CLICKED, NULL);

    /* 键盘挂在全屏 modal 上贴底，避免被面板裁剪。 */
    lv_obj_t *kb = lv_keyboard_create(s_modal);
    lv_obj_set_size(kb, panel_w, kb_h);
    lv_obj_align(kb, LV_ALIGN_TOP_LEFT, WIFI_LIST_MARGIN, kb_y);
    lv_keyboard_set_mode(kb, LV_KEYBOARD_MODE_TEXT_LOWER);
    lv_keyboard_set_textarea(kb, ta);

    ESP_LOGI(TAG, "wifi_list: password modal open (\"%s\")", s_target_ssid);
}

/** 关闭密码模态（LVGL 线程内）。 */
static void wifi_list_modal_close(void) {
    if (s_modal != NULL) {
        lv_obj_del(s_modal);
        s_modal = NULL;
    }
    s_kb_ta = NULL;
    s_kb_hint = NULL;
}

/* ------------------------------------------------------------------ */
/* 发起连接                                                             */
/* ------------------------------------------------------------------ */

/** 发起连接（LVGL 线程内）：记录原模式，切连接中视图并投递 worker。 */
static void wifi_list_connect_start(const char *pass) {
    s_prev_mode = espaperplay_system_get_config()->wifi_mode;
    s_poll_elapsed_ms = 0;
    wifi_list_show_view(WIFI_LIST_VIEW_CONNECTING);

    wifi_list_op_t op = {.type = WIFI_LIST_OP_CONNECT};
    strlcpy(op.ssid, s_target_ssid, sizeof(op.ssid));
    strlcpy(op.pass, pass, sizeof(op.pass));
    wifi_list_op_post(&op);
}

/* ------------------------------------------------------------------ */
/* 页面生命周期                                                         */
/* ------------------------------------------------------------------ */

/** 列表页构建（页面 enter：屏幕已由页面栈清空）。 */
static void wifi_list_enter(void) {
    lv_obj_t *scr = lv_screen_active();
    lv_obj_set_style_bg_color(scr, lv_color_white(), 0);
    /* 防止整个屏幕被 LVGL 滚动（列表容器自行滚动）。 */
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    lv_display_t *disp = lv_display_get_default();
    s_scale = (float)lv_display_get_vertical_resolution(disp) / (float)WIFI_LIST_REF_H;
    const int bar_h = wifi_list_bar_h();
    s_bar = espaperplay_ui_status_bar_create(scr, bar_h, "WiFi 网络", false);

    s_page_active = true;
    s_view = WIFI_LIST_VIEW_LIST;
    s_modal = NULL;
    s_kb_ta = NULL;
    s_kb_hint = NULL;
    s_poll_timer = NULL;
    s_poll_elapsed_ms = 0;
    s_item_count = 0;
    s_target_ssid[0] = '\0';

    /* 进入即自动扫描（页面关闭后迟到回调经 s_page_active 拦截）。 */
    s_scan_busy = true;
    wifi_list_show_view(WIFI_LIST_VIEW_LIST);
    wifi_list_op_t op = {.type = WIFI_LIST_OP_SCAN};
    wifi_list_op_post(&op);

    ESP_LOGI(TAG, "wifi list page entered");
}

/** 列表页退出（页面 exit：删除定时器与模态，清空对象指针）。 */
static void wifi_list_exit(void) {
    s_page_active = false;
    wifi_list_poll_stop();
    wifi_list_modal_close();
    s_content = NULL; /* 页面对象随清屏删除 */
    s_status_label = NULL;
    s_list = NULL;
    ESP_LOGI(TAG, "wifi list page exited");
}

/** 触摸处理：横向滑动返回设置页（列表纵向滚动交给 LVGL 容器）。 */
static void wifi_list_on_touch(const espaperplay_input_event_t *event) {
    if (s_modal != NULL) {
        return; /* 模态打开：点击由覆盖层 / 按钮处理 */
    }

    lv_point_t p;
    espaperplay_ui_touch_map_to_lv(event->point.x, event->point.y, &p);

    if (event->touch_pressed) {
        if (!s_touch_down) {
            s_touch_down = true;
            s_touch_start = p;
        }
        s_touch_last = p;
    } else if (s_touch_down) {
        s_touch_down = false;
        const int dx = s_touch_last.x - s_touch_start.x;
        const int dy = s_touch_last.y - s_touch_start.y;
        const int adx = abs(dx);
        const int ady = abs(dy);
        if (adx > 70 && adx > ady * 1.2f && s_view == WIFI_LIST_VIEW_LIST) {
            if (espaperplay_ui_page_depth() > 1) {
                ESP_LOGI(TAG, "wifi_list: swipe -> pop back");
                espaperplay_ui_page_pop_lv();
            }
        }
    }
}

/** 按键处理：模态打开时单击关闭模态；连接中视图单击取消；列表单击返回。 */
static void wifi_list_on_key(const espaperplay_input_event_t *event) {
    if (event->key_action != ESPAPERPLAY_INPUT_KEY_ACTION_SINGLE_CLICK) {
        return;
    }
    if (s_modal != NULL) {
        wifi_list_modal_close();
        return;
    }
    if (s_view == WIFI_LIST_VIEW_CONNECTING) {
        wifi_list_cancel_connect_cb(NULL);
        return;
    }
    if (espaperplay_ui_page_depth() > 1) {
        ESP_LOGI(TAG, "wifi_list: single click -> pop back");
        espaperplay_ui_page_pop_lv();
    }
}

/** 列表页页面实例（页面栈用）。 */
const espaperplay_ui_page_t espaperplay_ui_page_wifi_list = {wifi_list_enter, wifi_list_exit,
                                                             wifi_list_on_key,
                                                             wifi_list_on_touch};
