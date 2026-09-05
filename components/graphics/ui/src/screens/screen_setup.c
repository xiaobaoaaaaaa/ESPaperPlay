/*
 * SPDX-FileCopyrightText: 2026 ESPaperPlay Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_wifi.h"

#include "espaperplay_fonts.h"
#include "espaperplay_gui_lv.h"
#include "espaperplay_epd.h"
#include "espaperplay_power.h"
#include "espaperplay_system.h"
#include "espaperplay_ui.h"
#include "espaperplay_wifi.h"
#include "icons_data.h"

#include "lvgl.h"

static const char *TAG = "ESPaperPlay_UI";

/* ====================================================================
 * 首次开机引导页（分步交互式初始化配置）
 * ====================================================================
 *
 * 出厂状态（system 配置 setup_done=false）首次开机时由 main 推入本页
 * （替代主界面作为根页面）。此时设备按出厂默认以 AP 热点模式运行，
 * 引导用户二选一完成基本配置：
 *
 *   路径 A（推荐）联网配置：展示设备热点名与 WebUI 地址二维码，用户
 *     手机连接热点后扫码（或手动输入地址）打开 Web 管理页，在网页
 *     分步向导中完成全部配置；回到本页点「完成配置」即结束引导。
 *   路径 B 本机配置：自动扫描附近网络并列出（信号强度 / 是否加密），
 *     点选后输入密码（开放网络免密直连）；扫描不到的网络（如隐藏 SSID）
 *     可手动输入名称与密码，保存后设备切换 STA 模式连接，成功后结束引导。
 *
 * 也可随时「跳过引导」直接开始使用（稍后可在设置页重新运行引导，或经
 * Web 管理页配置）。完成 / 跳过都会把 setup_done 持久化到 NVS，下次
 * 开机不再进入本页；恢复出厂默认会重新清零。
 *
 * 本机连接失败时（密码错误 / 信号弱 / 重试耗尽），WiFi 服务会自动回退
 * 回 AP 热点模式，本页据此转入失败步骤，可重试或改走联网配置路径。
 *
 * 布局自适应与设置页同方案：行高/间距按屏高缩放（基准 800），字号固定
 * 档位（16/20/24/32）避免 FreeType 字体缓存被挤爆。控件少，直接用
 * LVGL 原生按钮点击（无需整行手势判定）；页面禁滚动防误滑。
 *
 * 线程模型（与设置页一致）：
 *   - NVS 写入 + wifi_start 必须离开 LVGL 任务执行（LVGL 任务栈在
 *     PSRAM，flash 操作禁缓存期间访问 PSRAM 栈会触发断言崩溃），投递到
 *     内部 RAM 栈的 worker 任务；
 *   - 连接结果轮询用 lv_timer 在 LVGL 线程读 WiFi 服务内存态快照；
 *   - worker 完成后经 espaperplay_gui_lv_call 回 LVGL 线程更新界面。
 */

#define SETUP_REF_H 800              /* 基准逻辑高度（缩放参考） */
#define SETUP_BAR_H 30               /* 标题栏高度（基准） */
#define SETUP_MIN_H 24               /* 标题栏最小高度 */
#define SETUP_MARGIN 16              /* 内容与屏幕边缘间距 */
#define SETUP_BTN_H 52               /* 主按钮高度（基准） */
#define SETUP_BTN_GAP 16             /* 按钮纵向间距（基准） */
#define SETUP_POLL_PERIOD_MS 500     /* 连接状态轮询周期 */
#define SETUP_CONNECT_TIMEOUT_MS 90000 /* 连接判定超时（覆盖 STA 重试耗尽 + AP 回退全程） */

/** 引导步骤。 */
typedef enum {
    SETUP_STEP_WELCOME = 0, /*!< 欢迎：选择配置方式 */
    SETUP_STEP_WEB,         /*!< 联网配置：热点名 + 二维码 + 步骤说明 */
    SETUP_STEP_INPUT,       /*!< 本机配置：键盘模态输入中（背景为提示内容） */
    SETUP_STEP_CONNECTING,  /*!< 正在连接目标网络 */
    SETUP_STEP_DONE,        /*!< 配置完成 */
    SETUP_STEP_FAIL,        /*!< 连接失败（重试 / 改联网配置 / 跳过） */
} setup_step_t;

/** worker 操作类型（NVS 写入 / 服务应用全部离开 LVGL 线程）。 */
typedef enum {
    SETUP_OP_CONNECT = 0,   /*!< 保存 STA 凭据并切换连接 */
    SETUP_OP_RESTORE_AP,    /*!< 取消连接：恢复 AP 热点模式 */
    SETUP_OP_FINISH,        /*!< 标记引导完成（NVS 持久化） */
    SETUP_OP_SCAN,          /*!< 阻塞扫描附近 AP（本机配置列表） */
} setup_op_type_t;

typedef struct {
    setup_op_type_t type;                                 /*!< 操作类型 */
    char ssid[ESPAPERPLAY_SYSTEM_SSID_MAX_LEN];           /*!< CONNECT：目标网络名 */
    char pass[ESPAPERPLAY_SYSTEM_PASS_MAX_LEN];           /*!< CONNECT：目标网络密码 */
} setup_op_t;

/* ------------------------------------------------------------------ */
/* 页面状态                                                             */
/* ------------------------------------------------------------------ */

static espaperplay_ui_status_bar_t *s_bar = NULL; /*!< 统一状态栏 */
static lv_obj_t *s_content = NULL;                /*!< 当前步骤内容容器（切步重建） */
static setup_step_t s_step = SETUP_STEP_WELCOME;  /*!< 当前步骤 */

static char s_ssid[ESPAPERPLAY_SYSTEM_SSID_MAX_LEN];      /*!< 本机配置：输入的 SSID */
static char s_pass[ESPAPERPLAY_SYSTEM_PASS_MAX_LEN];      /*!< 本机配置：输入的密码 */
static bool s_ssid_auth = false;      /*!< 选定网络是否加密（空密码校验用；手动输入按未加密处理） */
static char s_fail_reason[96];                            /*!< 失败步骤的原因文案 */

/* 本机配置扫描列表（LVGL 线程内重建时拷贝，行点击经下标取回）。 */
static lv_obj_t *s_input_list = NULL;   /*!< 扫描结果列表容器 */
static lv_obj_t *s_input_status = NULL; /*!< 扫描状态提示行 */
static bool s_scan_busy = false;        /*!< 已投递扫描、尚未回 LVGL（防重复触发） */
static espaperplay_wifi_scan_item_t
    s_scan_items[ESPAPERPLAY_WIFI_SCAN_MAX]; /*!< 扫描结果快照 */
static size_t s_scan_count = 0;              /*!< 快照条数 */

static lv_obj_t *s_status_label = NULL;           /*!< 连接中步骤的状态行 */
static lv_timer_t *s_poll_timer = NULL;           /*!< 连接状态轮询定时器 */
static uint32_t s_poll_elapsed_ms = 0;            /*!< 已等待毫秒数 */
static bool s_connect_applied = false;            /*!< worker 已确认 wifi_start 成功 */

/* 键盘输入模态 */
static lv_obj_t *s_modal = NULL;                  /*!< 全屏覆盖层（NULL=未打开） */
static lv_obj_t *s_kb_ta = NULL;                  /*!< 输入框 */
static lv_obj_t *s_kb_hint = NULL;                /*!< 校验提示行 */
static bool s_kb_for_pass = false;                /*!< 当前输入的是否为密码 */

static QueueHandle_t s_op_queue = NULL;           /*!< 设置应用操作队列 */
static TaskHandle_t s_worker_task = NULL;         /*!< 设置应用任务（内部 RAM 栈） */

/* 前向声明 */
static void setup_show_step(setup_step_t step);
static void setup_modal_close(void);
static void setup_poll_cb(lv_timer_t *timer);
static void setup_open_keyboard(bool for_pass);
static void setup_btn_web_cb(lv_event_t *e);
static void setup_btn_local_cb(lv_event_t *e);
static void setup_btn_back_cb(lv_event_t *e);
static void setup_btn_skip_cb(lv_event_t *e);
static void setup_btn_retry_cb(lv_event_t *e);
static void setup_btn_cancel_connect_cb(lv_event_t *e);
static void setup_btn_done_cb(lv_event_t *e);
static void setup_btn_manual_cb(lv_event_t *e);
static void setup_btn_rescan_cb(lv_event_t *e);
static void setup_scan_row_cb(lv_event_t *e);
static void setup_scan_done_lv(void *arg);
static void setup_input_rebuild(void);
static void setup_finish_and_leave(void);
static void setup_leave_page_only(void);

/* ------------------------------------------------------------------ */
/* 工具函数                                                             */
/* ------------------------------------------------------------------ */

/** FreeType 字体按需加载（16/20/24/32 固定档位）。 */
static lv_font_t *setup_font(int size_px) {
    return espaperplay_fonts_load(espaperplay_system_get_config()->selected_font,
                                  (uint32_t)size_px, ESPAPERPLAY_FONT_STYLE_NORMAL);
}

/** 通用标签：白底黑字 + FreeType 字体 + 禁用滚动。 */
static lv_obj_t *setup_label_create(lv_obj_t *parent, const char *text, int font_px,
                                    lv_text_align_t align) {
    lv_obj_t *label = lv_label_create(parent);
    lv_label_set_text(label, text);
    lv_obj_set_style_text_color(label, lv_color_black(), 0);
    lv_font_t *font = setup_font(font_px);
    if (font != NULL) {
        lv_obj_set_style_text_font(label, font, 0);
    }
    lv_obj_set_style_text_align(label, align, 0);
    lv_obj_set_width(label, LV_PCT(100));
    lv_obj_remove_flag(label, LV_OBJ_FLAG_SCROLLABLE);
    return label;
}

/** 逻辑分辨率（旋转后）。 */
static void setup_screen_size(int32_t *out_w, int32_t *out_h) {
    lv_display_t *disp = lv_display_get_default();
    *out_w = lv_display_get_horizontal_resolution(disp);
    *out_h = lv_display_get_vertical_resolution(disp);
}

static float s_scale = 1.0f; /*!< 屏高缩放因子（enter 时计算） */

/** 内容区尺寸（s_content 容器尺寸，已扣除标题栏；垂直定位以此为基准）。 */
static int32_t s_content_w = 0;
static int32_t s_content_h = 0;

/** 基准值按屏高缩放（取整）。 */
static int setup_scaled(int v) { return (int)(v * s_scale); }

/** 标题栏高度（缩放 + 下限）。 */
static int setup_bar_h(void) {
    const int h = setup_scaled(SETUP_BAR_H);
    return h < SETUP_MIN_H ? SETUP_MIN_H : h;
}

/** 按钮高度（缩放 + 下限，保证可点按面积）。 */
static int setup_btn_h(void) {
    const int h = setup_scaled(SETUP_BTN_H);
    return h < 40 ? 40 : h;
}

/** 创建一个按钮（primary: true=黑底白字主按钮，false=白底黑边次按钮）。 */
static lv_obj_t *setup_button(lv_obj_t *parent, const char *text, int x, int y, int w, int h,
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
    lv_obj_set_style_text_font(label, setup_font(20), 0);
    lv_obj_center(label);
    if (cb != NULL) {
        lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, NULL);
    }
    return btn;
}

/** 获取 WebUI 访问地址（https://<当前 IP>；IP 未就绪时回退 AP 默认网段）。 */
static void setup_web_url(char *buf, size_t size) {
    const char *ip = "192.168.4.1";
    espaperplay_wifi_status_t st;
    if (espaperplay_wifi_get_status(&st) == ESP_OK && st.ip[0] != '\0' &&
        strcmp(st.ip, "0.0.0.0") != 0) {
        ip = st.ip;
    }
    snprintf(buf, size, "https://%s", ip);
}

/* ------------------------------------------------------------------ */
/* 设置应用任务（NVS 写入必须在内部 RAM 栈任务中执行）                     */
/* ------------------------------------------------------------------ */

/** 投递结果回 LVGL：wifi_start 已成功，开始轮询连接状态。 */
static void setup_connect_started_lv(void *arg) {
    (void)arg;
    if (s_step != SETUP_STEP_CONNECTING) {
        return; /* 用户已取消 / 离开步骤：忽略迟到回调 */
    }
    s_connect_applied = true;
    s_poll_elapsed_ms = 0;
    if (s_status_label != NULL) {
        lv_label_set_text(s_status_label, "正在连接，请稍候…");
    }
    if (s_poll_timer == NULL) {
        s_poll_timer = lv_timer_create(setup_poll_cb, SETUP_POLL_PERIOD_MS, NULL);
    }
}

/** 投递结果回 LVGL：应用配置失败（arg 为 esp_err_t）。 */
static void setup_connect_failed_lv(void *arg) {
    const esp_err_t err = (esp_err_t)(intptr_t)arg;
    if (s_step != SETUP_STEP_CONNECTING) {
        return;
    }
    snprintf(s_fail_reason, sizeof(s_fail_reason), "应用网络配置失败：%s", esp_err_to_name(err));
    setup_show_step(SETUP_STEP_FAIL);
}

/** 设置应用任务：从队列取操作，执行 NVS 写入 + 服务应用。 */
static void setup_worker_task(void *arg) {
    (void)arg;
    setup_op_t op;
    for (;;) {
        if (xQueueReceive(s_op_queue, &op, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        switch (op.type) {
        case SETUP_OP_CONNECT: {
            esp_err_t err = espaperplay_system_set_sta_credentials(op.ssid, op.pass);
            if (err == ESP_OK) {
                err = espaperplay_system_set_wifi_mode(ESPAPERPLAY_WIFI_MODE_STA);
            }
            if (err == ESP_OK) {
                err = espaperplay_wifi_start();
            }
            if (err == ESP_OK) {
                ESP_LOGI(TAG, "setup: connecting to \"%s\"", op.ssid);
                espaperplay_gui_lv_call(setup_connect_started_lv, NULL, 1000);
            } else {
                ESP_LOGW(TAG, "setup: apply wifi failed: %s", esp_err_to_name(err));
                espaperplay_gui_lv_call(setup_connect_failed_lv, (void *)(intptr_t)err, 1000);
            }
            break;
        }
        case SETUP_OP_RESTORE_AP:
            /* 用户取消本机连接：恢复 AP 热点，保证仍可经 WebUI 配置。 */
            if (espaperplay_system_set_wifi_mode(ESPAPERPLAY_WIFI_MODE_AP) == ESP_OK) {
                espaperplay_wifi_start();
            }
            break;
        case SETUP_OP_SCAN: {
            esp_err_t err = espaperplay_wifi_scan_start();
            if (err != ESP_OK) {
                ESP_LOGW(TAG, "setup: scan failed: %s", esp_err_to_name(err));
            }
            espaperplay_gui_lv_call(setup_scan_done_lv, (void *)(intptr_t)err, 2000);
            break;
        }
        case SETUP_OP_FINISH: {
            esp_err_t err = espaperplay_system_mark_setup_done();
            if (err != ESP_OK) {
                ESP_LOGW(TAG, "setup: mark done failed: %s", esp_err_to_name(err));
            }
            break;
        }
        default:
            break;
        }
    }
}

/** 确保设置应用任务已创建（首次使用时创建，常驻程序生命周期）。 */
static void setup_worker_ensure(void) {
    if (s_op_queue != NULL && s_worker_task != NULL) {
        return;
    }
    if (s_op_queue == NULL) {
        s_op_queue = xQueueCreate(4, sizeof(setup_op_t));
    }
    if (s_op_queue != NULL && s_worker_task == NULL) {
        /* 栈必须放内部 RAM（NVS/flash 操作禁用缓存期间可访问）；CONFIG_SPIRAM_USE_MALLOC
         * 下 xTaskCreate 默认栈在 PSRAM，须用 xTaskCreateWithCaps 显式指定。 */
        if (xTaskCreateWithCaps(setup_worker_task, "ui_setup", 4096, NULL, 4, &s_worker_task,
                                MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT) != pdPASS) {
            ESP_LOGE(TAG, "setup: worker task create failed");
        }
    }
}

/** 投递一个设置应用操作（LVGL 线程内调用，非阻塞）。 */
static void setup_op_post(const setup_op_t *op) {
    setup_worker_ensure();
    if (s_op_queue == NULL || xQueueSend(s_op_queue, op, 0) != pdTRUE) {
        ESP_LOGW(TAG, "setup: op queue unavailable/full");
    }
}

/* ------------------------------------------------------------------ */
/* 步骤构建                                                             */
/* ------------------------------------------------------------------ */

/** 底部主行动按钮 y 坐标（各步骤统一：贴底留边，基于内容区高度）。 */
static int setup_bottom_btn_y(void) { return s_content_h - setup_btn_h() - setup_scaled(20); }

/** 欢迎步骤：标题 + 说明 + 两个路径按钮 + 跳过入口。 */
static void setup_build_welcome(lv_obj_t *parent, int32_t scr_w, int32_t scr_h) {
    (void)scr_h;
    const int btn_w = scr_w - 2 * SETUP_MARGIN;

    lv_obj_t *title =
        setup_label_create(parent, "欢迎使用 ESPaperPlay", 24, LV_TEXT_ALIGN_CENTER);
    lv_obj_set_pos(title, 0, setup_scaled(56));

    lv_obj_t *body = setup_label_create(
        parent, "首次使用需要先完成基本配置。\n设备已开启热点，请选择一种配置方式：", 16,
        LV_TEXT_ALIGN_CENTER);
    lv_obj_set_pos(body, 0, setup_scaled(110));

    /* 两个路径按钮靠下排布（跳过入口贴底，基于内容区高度）。 */
    const int skip_y = s_content_h - setup_scaled(52);
    const int btn2_y = skip_y - setup_scaled(60);
    const int btn1_y = btn2_y - setup_btn_h() - SETUP_BTN_GAP;

    setup_button(parent, "联网配置（推荐）", SETUP_MARGIN, btn1_y, btn_w, setup_btn_h(), true,
                 setup_btn_web_cb);
    setup_button(parent, "在本机输入 WiFi", SETUP_MARGIN, btn2_y, btn_w, setup_btn_h(), false,
                 setup_btn_local_cb);

    /* 跳过：无边框文字按钮。 */
    lv_obj_t *skip = lv_button_create(parent);
    lv_obj_set_size(skip, btn_w, setup_scaled(36));
    lv_obj_set_pos(skip, SETUP_MARGIN, skip_y);
    lv_obj_set_style_bg_color(skip, lv_color_white(), 0);
    lv_obj_set_style_border_width(skip, 0, 0);
    lv_obj_set_style_radius(skip, 8, 0);
    lv_obj_t *skip_label = lv_label_create(skip);
    lv_label_set_text(skip_label, "跳过引导，直接开始");
    lv_obj_set_style_text_color(skip_label, lv_color_black(), 0);
    lv_obj_set_style_text_font(skip_label, setup_font(16), 0);
    lv_obj_center(skip_label);
    lv_obj_add_event_cb(skip, setup_btn_skip_cb, LV_EVENT_CLICKED, NULL);
}

/** 联网配置步骤：二维码 + 三步说明 + 完成/返回。 */
static void setup_build_web(lv_obj_t *parent, int32_t scr_w, int32_t scr_h) {
    (void)scr_h;
    char url[32];
    setup_web_url(url, sizeof(url));

    char ap_line[ESPAPERPLAY_SYSTEM_SSID_MAX_LEN + 48];
    snprintf(ap_line, sizeof(ap_line), "1. 手机连接设备热点「%s」",
             espaperplay_system_get_config()->ap_ssid);

    const int btn_y = setup_bottom_btn_y();
    const int btn_w = (scr_w - 2 * SETUP_MARGIN - 12) / 2;

    /* 自上而下顺序布局；空间不足时收缩二维码。 */
    int y = setup_scaled(10);
    lv_obj_t *title = setup_label_create(parent, "联网配置", 20, LV_TEXT_ALIGN_CENTER);
    lv_obj_set_pos(title, 0, y);
    y += setup_scaled(36);

    int qr_size = setup_scaled(190);
    if (qr_size < 140) {
        qr_size = 140;
    }
    /* 二维码 + 说明 + 按钮总高超出可用区时压缩二维码（横屏小屏兜底）。 */
    const int needed = qr_size + setup_scaled(150) + setup_btn_h() + setup_scaled(50);
    const int avail = btn_y - y - 4;
    if (needed > avail && avail > setup_scaled(260)) {
        qr_size = avail - setup_scaled(150) - setup_btn_h() - setup_scaled(50);
        if (qr_size < 120) {
            qr_size = 120;
        }
    }

#if CONFIG_LV_USE_QRCODE
    lv_obj_t *qr = lv_qrcode_create(parent);
    lv_qrcode_set_size(qr, qr_size);
    lv_qrcode_set_dark_color(qr, lv_color_black());
    lv_qrcode_set_light_color(qr, lv_color_white());
    lv_qrcode_update(qr, url, strlen(url));
    lv_obj_set_pos(qr, (scr_w - qr_size) / 2, y);
#else
    (void)qr_size;
#endif
    y += qr_size + setup_scaled(12);

    lv_obj_t *url_label = setup_label_create(parent, url, 20, LV_TEXT_ALIGN_CENTER);
    lv_obj_set_pos(url_label, 0, y);
    y += setup_scaled(34);

    char steps[256];
    snprintf(steps, sizeof(steps),
             "%s\n2. 扫描二维码，或浏览器打开上方地址\n3. 在网页向导中完成配置后，回到这里点「完成配置」",
             ap_line);
    lv_obj_t *steps_label = setup_label_create(parent, steps, 16, LV_TEXT_ALIGN_LEFT);
    lv_obj_set_style_pad_left(steps_label, SETUP_MARGIN, 0);
    lv_obj_set_pos(steps_label, 0, y);

    setup_button(parent, "返回", SETUP_MARGIN, btn_y, btn_w, setup_btn_h(), false,
                 setup_btn_back_cb);
    setup_button(parent, "完成配置", SETUP_MARGIN + btn_w + 12, btn_y, btn_w, setup_btn_h(), true,
                 setup_btn_skip_cb);
}

/** 扫描结果 RSSI 分档图标（与状态栏同款素材）。 */
static const lv_image_dsc_t *setup_rssi_icon(int8_t rssi) {
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

/** 重建本机配置扫描列表（LVGL 线程内；从 wifi 服务缓存拷贝快照）。 */
static void setup_input_rebuild(void) {
    if (s_input_list == NULL) {
        return;
    }
    lv_obj_clean(s_input_list);
    s_scan_count = espaperplay_wifi_scan_get_results(s_scan_items, ESPAPERPLAY_WIFI_SCAN_MAX);

    if (s_scan_count == 0) {
        if (!s_scan_busy && s_input_status != NULL) {
            lv_label_set_text(s_input_status, "未发现附近网络，可手动输入或重新扫描");
        }
        return;
    }

    const int32_t list_w = lv_obj_get_width(s_input_list);
    const int row_h = setup_scaled(44) < 36 ? 36 : setup_scaled(44);

    for (size_t i = 0; i < s_scan_count; i++) {
        lv_obj_t *row = lv_obj_create(s_input_list);
        lv_obj_set_size(row, list_w, row_h);
        lv_obj_set_pos(row, 0, (int)(i * (row_h + 4)));
        lv_obj_set_style_bg_color(row, lv_color_white(), 0);
        lv_obj_set_style_border_color(row, lv_color_black(), 0);
        lv_obj_set_style_border_width(row, 1, 0);
        lv_obj_set_style_radius(row, 6, 0);
        lv_obj_set_style_pad_all(row, 0, 0);
        lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(row, setup_scan_row_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);

        /* SSID（左，超长省略） */
        lv_obj_t *label = lv_label_create(row);
        lv_label_set_text(label, s_scan_items[i].ssid);
        lv_obj_set_style_text_color(label, lv_color_black(), 0);
        lv_obj_set_style_text_font(label, setup_font(16), 0);
        lv_obj_align(label, LV_ALIGN_LEFT_MID, 10, 0);
        lv_obj_set_width(label, list_w - 110);
        lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);

        /* 加密标记 + 信号强度图标（右） */
        if (s_scan_items[i].auth) {
            lv_obj_t *lock = lv_label_create(row);
            lv_label_set_text(lock, "加密");
            lv_obj_set_style_text_color(lock, lv_color_black(), 0);
            lv_obj_set_style_text_font(lock, setup_font(16), 0);
            lv_obj_align(lock, LV_ALIGN_RIGHT_MID, -30, 0);
        }
        lv_obj_t *icon = lv_image_create(row);
        lv_image_set_src(icon, setup_rssi_icon(s_scan_items[i].rssi));
        lv_obj_align(icon, LV_ALIGN_RIGHT_MID, -10, 0);
    }

    if (!s_scan_busy && s_input_status != NULL) {
        char buf[48];
        snprintf(buf, sizeof(buf), "已发现 %u 个网络，点选即可连接", (unsigned)s_scan_count);
        lv_label_set_text(s_input_status, buf);
    }
    ESP_LOGI(TAG, "setup: %u networks listed", (unsigned)s_scan_count);
}

/** 扫描完成回调（LVGL 线程，arg 为 esp_err_t）：仍处于本机配置步骤才重建。 */
static void setup_scan_done_lv(void *arg) {
    const esp_err_t err = (esp_err_t)(intptr_t)arg;
    s_scan_busy = false;
    if (s_step != SETUP_STEP_INPUT) {
        return; /* 已离开本机配置步骤：忽略迟到回调 */
    }
    if (err != ESP_OK && s_input_status != NULL) {
        if (err == ESP_ERR_WIFI_STATE) {
            lv_label_set_text(s_input_status, "WiFi 正忙，请稍后重新扫描");
        } else {
            char buf[96];
            snprintf(buf, sizeof(buf), "扫描失败：%s", esp_err_to_name(err));
            lv_label_set_text(s_input_status, buf);
        }
        return;
    }
    setup_input_rebuild();
}

/** 扫描行点击：加密网络 -> 密码键盘；开放网络 -> 免密直连。 */
static void setup_scan_row_cb(lv_event_t *e) {
    lv_event_stop_bubbling(e);
    if (s_step != SETUP_STEP_INPUT || s_modal != NULL || s_scan_busy) {
        return;
    }
    const intptr_t idx = (intptr_t)lv_event_get_user_data(e);
    if (idx < 0 || (size_t)idx >= s_scan_count) {
        return;
    }
    strlcpy(s_ssid, s_scan_items[idx].ssid, sizeof(s_ssid));
    s_ssid_auth = s_scan_items[idx].auth;
    if (s_ssid_auth) {
        setup_open_keyboard(true);
        return;
    }
    /* 开放网络：免密直接连接。 */
    setup_show_step(SETUP_STEP_CONNECTING);
    setup_op_t op = {.type = SETUP_OP_CONNECT};
    strlcpy(op.ssid, s_ssid, sizeof(op.ssid));
    op.pass[0] = '\0';
    setup_op_post(&op);
}

/** 「手动输入」：弹出 SSID 键盘（隐藏网络等扫描不到的场景）。 */
static void setup_btn_manual_cb(lv_event_t *e) {
    lv_event_stop_bubbling(e);
    if (s_step != SETUP_STEP_INPUT || s_modal != NULL) {
        return;
    }
    s_ssid_auth = false; /* 手动输入的网络加密状态未知：允许空密码 */
    setup_open_keyboard(false);
}

/** 「重新扫描」：重新投递扫描操作。 */
static void setup_btn_rescan_cb(lv_event_t *e) {
    lv_event_stop_bubbling(e);
    if (s_step != SETUP_STEP_INPUT || s_scan_busy || s_modal != NULL) {
        return;
    }
    s_scan_busy = true;
    if (s_input_status != NULL) {
        lv_label_set_text(s_input_status, "正在扫描附近网络…");
    }
    setup_op_t op = {.type = SETUP_OP_SCAN};
    setup_op_post(&op);
}

/** 本机配置步骤：扫描列表 + 手动输入兜底（键盘模态打开时的底层）。 */
static void setup_build_input(lv_obj_t *parent, int32_t scr_w, int32_t scr_h) {
    (void)scr_h;
    lv_obj_t *title = setup_label_create(parent, "本机配置", 24, LV_TEXT_ALIGN_CENTER);
    lv_obj_set_pos(title, 0, setup_scaled(20));

    s_input_status = setup_label_create(
        parent, s_scan_busy ? "正在扫描附近网络…" : "选择要连接的网络", 16, LV_TEXT_ALIGN_CENTER);
    lv_obj_set_pos(s_input_status, 0, setup_scaled(66));

    const int btn_h = setup_btn_h();
    const int btn_y = s_content_h - btn_h - setup_scaled(20);

    /* 扫描结果列表（可纵向滚动；EPD 禁弹性滚动与滚动条）。 */
    s_input_list = lv_obj_create(parent);
    lv_obj_set_size(s_input_list, scr_w - 2 * SETUP_MARGIN, btn_y - setup_scaled(106));
    lv_obj_set_pos(s_input_list, SETUP_MARGIN, setup_scaled(106));
    lv_obj_set_style_bg_color(s_input_list, lv_color_white(), 0);
    lv_obj_set_style_border_width(s_input_list, 0, 0);
    lv_obj_set_style_radius(s_input_list, 0, 0);
    lv_obj_set_style_pad_all(s_input_list, 0, 0);
    lv_obj_set_scroll_dir(s_input_list, LV_DIR_VER);
    lv_obj_add_flag(s_input_list, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(s_input_list, LV_SCROLLBAR_MODE_OFF);
    lv_obj_remove_flag(s_input_list, LV_OBJ_FLAG_SCROLL_ELASTIC);

    const int btn_w = (scr_w - 2 * SETUP_MARGIN - 12) / 2;
    setup_button(parent, "手动输入", SETUP_MARGIN, btn_y, btn_w, btn_h, false,
                 setup_btn_manual_cb);
    setup_button(parent, "重新扫描", SETUP_MARGIN + btn_w + 12, btn_y, btn_w, btn_h, false,
                 setup_btn_rescan_cb);

    /* 扫描进行中保持空列表等回调；否则用缓存结果即时重建。 */
    if (!s_scan_busy) {
        setup_input_rebuild();
    }
}

/** 连接中步骤：目标网络 + 动态状态行 + 取消。 */
static void setup_build_connecting(lv_obj_t *parent, int32_t scr_w, int32_t scr_h) {
    (void)scr_w;
    (void)scr_h;
    lv_obj_t *title = setup_label_create(parent, "正在连接网络", 24, LV_TEXT_ALIGN_CENTER);
    lv_obj_set_pos(title, 0, setup_scaled(70));

    char ssid_line[ESPAPERPLAY_SYSTEM_SSID_MAX_LEN + 16];
    snprintf(ssid_line, sizeof(ssid_line), "「%s」", s_ssid);
    lv_obj_t *ssid_label = setup_label_create(parent, ssid_line, 20, LV_TEXT_ALIGN_CENTER);
    lv_obj_set_pos(ssid_label, 0, setup_scaled(130));

    s_status_label = setup_label_create(parent, "正在应用网络配置…", 16, LV_TEXT_ALIGN_CENTER);
    lv_obj_set_pos(s_status_label, 0, setup_scaled(170));

    const int btn_w = scr_w - 2 * SETUP_MARGIN;
    setup_button(parent, "取消并返回", SETUP_MARGIN, setup_bottom_btn_y(), btn_w,
                 setup_btn_h(), false, setup_btn_cancel_connect_cb);
}

/** 完成步骤：成功提示 + 开始使用。 */
static void setup_build_done(lv_obj_t *parent, int32_t scr_w, int32_t scr_h) {
    (void)scr_w;
    (void)scr_h;
    lv_obj_t *title = setup_label_create(parent, "配置完成！", 32, LV_TEXT_ALIGN_CENTER);
    lv_obj_set_pos(title, 0, setup_scaled(80));

    char line[ESPAPERPLAY_SYSTEM_SSID_MAX_LEN + 48];
    espaperplay_wifi_status_t st;
    if (espaperplay_wifi_get_status(&st) == ESP_OK && st.connected) {
        snprintf(line, sizeof(line), "设备已连接到「%s」（%s）", s_ssid, st.ip);
    } else {
        snprintf(line, sizeof(line), "设备已连接到「%s」", s_ssid);
    }
    lv_obj_t *body = setup_label_create(parent, line, 16, LV_TEXT_ALIGN_CENTER);
    lv_obj_set_pos(body, 0, setup_scaled(150));

    const int btn_w = scr_w - 2 * SETUP_MARGIN;
    setup_button(parent, "开始使用", SETUP_MARGIN, setup_bottom_btn_y(), btn_w, setup_btn_h(),
                 true, setup_btn_done_cb);
}

/** 失败步骤：原因 + 重试 / 改联网配置 / 跳过。 */
static void setup_build_fail(lv_obj_t *parent, int32_t scr_w, int32_t scr_h) {
    (void)scr_h;
    lv_obj_t *title = setup_label_create(parent, "连接失败", 24, LV_TEXT_ALIGN_CENTER);
    lv_obj_set_pos(title, 0, setup_scaled(56));

    char line[ESPAPERPLAY_SYSTEM_SSID_MAX_LEN + 32];
    snprintf(line, sizeof(line), "无法连接到「%s」", s_ssid);
    lv_obj_t *ssid_label = setup_label_create(parent, line, 16, LV_TEXT_ALIGN_CENTER);
    lv_obj_set_pos(ssid_label, 0, setup_scaled(104));

    lv_obj_t *reason = setup_label_create(
        parent, s_fail_reason[0] ? s_fail_reason : "请检查密码是否正确、信号是否可用。", 16,
        LV_TEXT_ALIGN_CENTER);
    lv_obj_set_pos(reason, 0, setup_scaled(136));

    const int btn_y = setup_bottom_btn_y();
    const int btn_w = (scr_w - 2 * SETUP_MARGIN - 12) / 2;
    setup_button(parent, "重试", SETUP_MARGIN, btn_y - setup_btn_h() - SETUP_BTN_GAP, btn_w,
                 setup_btn_h(), true, setup_btn_retry_cb);
    setup_button(parent, "改用联网配置", SETUP_MARGIN + btn_w + 12, btn_y - setup_btn_h() - SETUP_BTN_GAP,
                 btn_w, setup_btn_h(), false, setup_btn_web_cb);
    setup_button(parent, "跳过引导", SETUP_MARGIN, btn_y, scr_w - 2 * SETUP_MARGIN,
                 setup_scaled(40), false, setup_btn_skip_cb);
}

/** 构建当前步骤内容（s_content 已重建）。 */
static void setup_build_step_lv(void) {
    int32_t scr_w = 0;
    int32_t scr_h = 0;
    setup_screen_size(&scr_w, &scr_h);
    s_status_label = NULL;
    s_input_list = NULL;
    s_input_status = NULL;

    switch (s_step) {
    case SETUP_STEP_WELCOME:
        setup_build_welcome(s_content, scr_w, scr_h);
        break;
    case SETUP_STEP_WEB:
        setup_build_web(s_content, scr_w, scr_h);
        break;
    case SETUP_STEP_INPUT:
        setup_build_input(s_content, scr_w, scr_h);
        break;
    case SETUP_STEP_CONNECTING:
        setup_build_connecting(s_content, scr_w, scr_h);
        break;
    case SETUP_STEP_DONE:
        setup_build_done(s_content, scr_w, scr_h);
        break;
    case SETUP_STEP_FAIL:
        setup_build_fail(s_content, scr_w, scr_h);
        break;
    default:
        break;
    }
}

/** 切换步骤：销毁旧内容容器并重建（EPD 只刷变化区域）。 */
static void setup_show_step(setup_step_t step) {
    s_step = step;
    if (s_content != NULL) {
        lv_obj_del(s_content);
        s_content = NULL;
    }
    s_content = lv_obj_create(lv_screen_active());
    lv_obj_set_size(s_content, lv_display_get_horizontal_resolution(lv_display_get_default()),
                    lv_display_get_vertical_resolution(lv_display_get_default()) -
                        setup_bar_h());
    lv_obj_set_pos(s_content, 0, setup_bar_h());
    s_content_w = lv_display_get_horizontal_resolution(lv_display_get_default());
    s_content_h = lv_display_get_vertical_resolution(lv_display_get_default()) - setup_bar_h();
    lv_obj_set_style_bg_color(s_content, lv_color_white(), 0);
    lv_obj_set_style_border_width(s_content, 0, 0);
    lv_obj_set_style_radius(s_content, 0, 0);
    lv_obj_set_style_pad_all(s_content, 0, 0);
    lv_obj_remove_flag(s_content, LV_OBJ_FLAG_SCROLLABLE);
    setup_build_step_lv();
}

/* ------------------------------------------------------------------ */
/* 连接轮询                                                             */
/* ------------------------------------------------------------------ */

/** 结束轮询并进入失败步骤。 */
static void setup_connect_fail(const char *reason) {
    if (s_poll_timer != NULL) {
        lv_timer_delete(s_poll_timer);
        s_poll_timer = NULL;
    }
    strlcpy(s_fail_reason, reason, sizeof(s_fail_reason));
    setup_show_step(SETUP_STEP_FAIL);
}

/** 轮询定时器（LVGL 线程）：STA 获得 IP -> 完成；回退 AP / 超时 -> 失败。 */
static void setup_poll_cb(lv_timer_t *timer) {
    (void)timer;
    if (s_step != SETUP_STEP_CONNECTING) {
        return;
    }
    s_poll_elapsed_ms += SETUP_POLL_PERIOD_MS;

    espaperplay_wifi_status_t st;
    if (espaperplay_wifi_get_status(&st) == ESP_OK) {
        if (st.mode == ESPAPERPLAY_WIFI_MODE_STA && st.connected) {
            /* 成功：停止轮询，标记完成并进入完成步骤。 */
            if (s_poll_timer != NULL) {
                lv_timer_delete(s_poll_timer);
                s_poll_timer = NULL;
            }
            setup_op_t op = {.type = SETUP_OP_FINISH};
            setup_op_post(&op);
            setup_show_step(SETUP_STEP_DONE);
            return;
        }
        /* wifi_start 成功后又回到 AP 模式 = 启动期回退已发生（重试耗尽）。 */
        if (s_connect_applied && st.mode == ESPAPERPLAY_WIFI_MODE_AP && st.started) {
            setup_connect_fail("多次重试失败，设备已回退到热点模式。");
            return;
        }
    }

    if (s_status_label != NULL) {
        char buf[64];
        snprintf(buf, sizeof(buf), "正在连接，请稍候…（%u 秒）",
                 (unsigned)(s_poll_elapsed_ms / 1000));
        lv_label_set_text(s_status_label, buf);
    }

    if (s_poll_elapsed_ms >= SETUP_CONNECT_TIMEOUT_MS) {
        setup_connect_fail("连接超时，请检查密码与路由器状态。");
    }
}

/* ------------------------------------------------------------------ */
/* 键盘输入模态（SSID / 密码共用）                                        */
/* ------------------------------------------------------------------ */

/** 密码可见性切换。 */
static void setup_kb_toggle_cb(lv_event_t *e) {
    lv_event_stop_bubbling(e);
    if (s_kb_ta == NULL) {
        return;
    }
    const bool hidden = lv_textarea_get_password_mode(s_kb_ta);
    lv_textarea_set_password_mode(s_kb_ta, !hidden);
    lv_label_set_text(lv_obj_get_child(lv_event_get_current_target(e), 0), hidden ? "隐藏" : "显示");
}

/** 键盘模态 取消 按钮：关闭并回到本机配置列表（不重新扫描）。 */
static void setup_kb_cancel_cb(lv_event_t *e) {
    lv_event_stop_bubbling(e);
    setup_modal_close();
    setup_show_step(SETUP_STEP_INPUT);
}

/** 键盘模态 确定 按钮：校验 -> 下一步（SSID -> 密码 -> 发起连接）。 */
static void setup_kb_ok_cb(lv_event_t *e) {
    lv_event_stop_bubbling(e);
    if (s_kb_ta == NULL) {
        return;
    }
    const char *text = lv_textarea_get_text(s_kb_ta);
    const size_t len = strlen(text);

    if (!s_kb_for_pass && len == 0) {
        if (s_kb_hint != NULL) {
            lv_label_set_text(s_kb_hint, "网络名称不能为空");
        }
        return;
    }
    if (!s_kb_for_pass && len >= ESPAPERPLAY_SYSTEM_SSID_MAX_LEN) {
        if (s_kb_hint != NULL) {
            lv_label_set_text(s_kb_hint, "网络名称过长");
        }
        return;
    }
    if (len >= ESPAPERPLAY_SYSTEM_PASS_MAX_LEN) {
        if (s_kb_hint != NULL) {
            lv_label_set_text(s_kb_hint, "密码过长");
        }
        return;
    }

    if (s_kb_for_pass) {
        /* 扫描选定的加密网络必须输密码；手动输入（加密状态未知）允许留空按开放网络连接。 */
        if (len == 0 && s_ssid_auth) {
            if (s_kb_hint != NULL) {
                lv_label_set_text(s_kb_hint, "请输入密码");
            }
            return;
        }
        strlcpy(s_pass, text, sizeof(s_pass));
        setup_modal_close();
        /* 发起连接：切连接中步骤 + 投递 worker。 */
        setup_show_step(SETUP_STEP_CONNECTING);
        setup_op_t op = {.type = SETUP_OP_CONNECT};
        strlcpy(op.ssid, s_ssid, sizeof(op.ssid));
        strlcpy(op.pass, s_pass, sizeof(op.pass));
        setup_op_post(&op);
    } else {
        strlcpy(s_ssid, text, sizeof(s_ssid));
        setup_open_keyboard(true); /* 直接切换到密码输入 */
    }
}

/**
 * 打开文本输入模态（底部面板：标题 + 输入框 + 提示 + 取消/确定 + 键盘）。
 * 输入模态不可点空白关闭（防误触丢输入）。
 *
 * @param for_pass true=输入密码（密码模式显示 + 可见性切换），false=输入 SSID。
 */
static void setup_open_keyboard(bool for_pass) {
    s_kb_for_pass = for_pass;

    int32_t scr_w = 0;
    int32_t scr_h = 0;
    setup_screen_size(&scr_w, &scr_h);

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
    const int panel_w = scr_w - 2 * SETUP_MARGIN;
    const int pad = 10;
    const int title_h = 30;
    const int ta_h = setup_scaled(52) < 40 ? 40 : setup_scaled(52);
    const int hint_h = 22;
    const int bh = setup_btn_h() < 38 ? 38 : setup_btn_h();
    const int kb_h = setup_scaled(240) < 170 ? 170 : setup_scaled(240);
    /* 面板仅含标题/输入框/提示/按钮（不含键盘，键盘单独挂全屏 modal 贴底，避免被面板裁剪）。 */
    const int panel_h = pad + title_h + 6 + ta_h + 4 + hint_h + 6 + bh + pad;
    const int kb_y = scr_h - kb_h - 6;       /* 键盘贴底 */
    const int panel_y = kb_y - panel_h - 6;  /* 面板位于键盘上方 */

    lv_obj_t *panel = lv_obj_create(s_modal);
    lv_obj_set_size(panel, panel_w, panel_h);
    lv_obj_set_pos(panel, SETUP_MARGIN, panel_y);
    lv_obj_set_style_bg_color(panel, lv_color_white(), 0);
    lv_obj_set_style_border_color(panel, lv_color_black(), 0);
    lv_obj_set_style_border_width(panel, 2, 0);
    lv_obj_set_style_radius(panel, 12, 0);
    lv_obj_set_style_pad_all(panel, 0, 0);
    lv_obj_remove_flag(panel, LV_OBJ_FLAG_SCROLLABLE);

    /* 标题。 */
    lv_obj_t *title = setup_label_create(panel, for_pass ? "输入 WiFi 密码" : "输入 WiFi 名称",
                                         20, LV_TEXT_ALIGN_CENTER);
    lv_obj_set_width(title, panel_w - 140);
    lv_obj_set_pos(title, 0, pad);

    /* 输入框（一行）。 */
    lv_obj_t *ta = lv_textarea_create(panel);
    s_kb_ta = ta;
    lv_obj_set_size(ta, panel_w - 2 * pad, ta_h);
    lv_obj_set_pos(ta, pad, pad + title_h + 6);
    lv_textarea_set_one_line(ta, true);
    lv_textarea_set_max_length(ta, (for_pass ? ESPAPERPLAY_SYSTEM_PASS_MAX_LEN
                                             : ESPAPERPLAY_SYSTEM_SSID_MAX_LEN) -
                                        1);
    lv_textarea_set_password_mode(ta, for_pass);
    lv_textarea_set_text(ta, "");
    lv_obj_set_style_text_color(ta, lv_color_black(), 0);
    lv_obj_set_style_text_font(ta, setup_font(20), 0);
    lv_obj_set_style_border_color(ta, lv_color_black(), 0);
    lv_obj_set_style_border_width(ta, 2, 0);
    lv_obj_set_style_radius(ta, 6, 0);
    lv_obj_set_style_pad_left(ta, 8, 0);
    /* 墨水屏：光标闪烁会触发连续局部刷新，禁用（anim_duration=0 即不闪烁）。
     * 默认主题在 LV_PART_CURSOR|LV_STATE_FOCUSED 上设了 400ms，故默认态与聚焦态都要覆盖。 */
    lv_obj_set_style_anim_duration(ta, 0, LV_PART_CURSOR);
    lv_obj_set_style_anim_duration(ta, 0, LV_PART_CURSOR | LV_STATE_FOCUSED);

    /* 校验提示（默认空）。 */
    lv_obj_t *hint = setup_label_create(panel, "", 16, LV_TEXT_ALIGN_LEFT);
    s_kb_hint = hint;
    lv_obj_set_width(hint, LV_PCT(100));
    lv_label_set_long_mode(hint, LV_LABEL_LONG_DOT);
    lv_obj_set_pos(hint, pad + 2, pad + title_h + 6 + ta_h + 4);

    /* 取消 / 确定。 */
    const int bw = (panel_w - 2 * pad - 12) / 2;
    const int btn_y = pad + title_h + 6 + ta_h + 4 + hint_h + 6;
    setup_button(panel, "取消", pad, btn_y, bw, bh, false, setup_kb_cancel_cb);
    setup_button(panel, "确定", pad + bw + 12, btn_y, bw, bh, true, setup_kb_ok_cb);

    /* 密码可见性切换（仅密码步骤；置于标题右侧）。 */
    if (for_pass) {
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
        lv_obj_set_style_text_font(tl, setup_font(16), 0);
        lv_obj_center(tl);
        lv_obj_add_event_cb(toggle, setup_kb_toggle_cb, LV_EVENT_CLICKED, NULL);
    }

    /* 键盘直接挂在全屏 modal 上并贴底，避免被面板裁剪（面板仅含标题/输入框/提示/按钮）。 */
    lv_obj_t *kb = lv_keyboard_create(s_modal);
    lv_obj_set_size(kb, panel_w, kb_h);
    lv_obj_align(kb, LV_ALIGN_TOP_LEFT, SETUP_MARGIN, kb_y);
    lv_keyboard_set_mode(kb, LV_KEYBOARD_MODE_TEXT_LOWER);
    lv_keyboard_set_textarea(kb, ta);

    ESP_LOGI(TAG, "setup: keyboard modal open (%s)", for_pass ? "password" : "ssid");
}

/** 关闭键盘模态（LVGL 线程内）。 */
static void setup_modal_close(void) {
    if (s_modal != NULL) {
        lv_obj_del(s_modal);
        s_modal = NULL;
    }
    s_kb_ta = NULL;
    s_kb_hint = NULL;
}

/* ------------------------------------------------------------------ */
/* 按钮事件回调                                                         */
/* ------------------------------------------------------------------ */

/** 「联网配置（推荐）/ 改用联网配置」：进入二维码步骤。 */
static void setup_btn_web_cb(lv_event_t *e) {
    (void)e;
    setup_show_step(SETUP_STEP_WEB);
}

/** 「在本机输入 WiFi」：进入本机配置（自动扫描附近网络）。 */
static void setup_btn_local_cb(lv_event_t *e) {
    (void)e;
    /* 先置扫描中标志：步骤构建即显示扫描提示、保持空列表（不闪旧结果）。 */
    s_scan_busy = true;
    setup_show_step(SETUP_STEP_INPUT);
    setup_op_t op = {.type = SETUP_OP_SCAN};
    setup_op_post(&op);
}

/** 「返回」：回到欢迎步骤。 */
static void setup_btn_back_cb(lv_event_t *e) {
    (void)e;
    setup_show_step(SETUP_STEP_WELCOME);
}

/** 「跳过引导 / 完成配置」：标记完成并离开引导页。 */
static void setup_btn_skip_cb(lv_event_t *e) {
    (void)e;
    setup_finish_and_leave();
}

/** 「重试」：以相同凭据再次发起连接。 */
static void setup_btn_retry_cb(lv_event_t *e) {
    (void)e;
    setup_show_step(SETUP_STEP_CONNECTING);
    setup_op_t op = {.type = SETUP_OP_CONNECT};
    strlcpy(op.ssid, s_ssid, sizeof(op.ssid));
    strlcpy(op.pass, s_pass, sizeof(op.pass));
    setup_op_post(&op);
}

/** 「取消并返回」：停止轮询，恢复 AP 并回到欢迎步骤。 */
static void setup_btn_cancel_connect_cb(lv_event_t *e) {
    (void)e;
    if (s_poll_timer != NULL) {
        lv_timer_delete(s_poll_timer);
        s_poll_timer = NULL;
    }
    setup_op_t op = {.type = SETUP_OP_RESTORE_AP};
    setup_op_post(&op);
    setup_show_step(SETUP_STEP_WELCOME);
}

/** 「开始使用」：离开引导页（完成标记已在进入完成步骤时投递）。 */
static void setup_btn_done_cb(lv_event_t *e) {
    (void)e;
    setup_leave_page_only();
}

/* ------------------------------------------------------------------ */
/* 页面生命周期                                                         */
/* ------------------------------------------------------------------ */

/** 标记完成并离开引导页（根页面时替换为主界面，否则弹回调用方页面）。 */
static void setup_finish_and_leave(void) {
    setup_op_t op = {.type = SETUP_OP_FINISH};
    setup_op_post(&op);
    setup_leave_page_only();
}

/** 仅离开引导页（不动完成标记）。 */
static void setup_leave_page_only(void) {
    if (espaperplay_ui_page_depth() > 1) {
        espaperplay_ui_page_pop_lv(); /* 从设置页等入口进入：弹回调用方 */
    } else {
        espaperplay_ui_page_replace_lv(&espaperplay_ui_page_home); /* 开机根页面：换主界面 */
    }
}

/** 引导页构建（页面 enter：屏幕已由页面栈清空）。 */
static void setup_enter(void) {
    lv_obj_t *scr = lv_screen_active();
    lv_obj_set_style_bg_color(scr, lv_color_white(), 0);
    /* 防止整个屏幕被 LVGL 滚动（步骤切换由按钮驱动）。 */
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    s_scale = (float)lv_display_get_vertical_resolution(lv_display_get_default()) /
              (float)SETUP_REF_H;
    s_bar = espaperplay_ui_status_bar_create(scr, setup_bar_h(), "开机引导", false);

    /* 引导期间禁止设备睡眠：用户可能在手机上经 WebUI 完成配置，睡眠会导致设备不可达。 */
    espaperplay_epd_set_idle_sleep_timeout_ms(0);
    espaperplay_power_set_auto_sleep_timeout_ms(0);

    s_ssid[0] = '\0';
    s_pass[0] = '\0';
    s_ssid_auth = false;
    s_fail_reason[0] = '\0';
    s_scan_busy = false;
    s_scan_count = 0;
    s_modal = NULL;
    s_kb_ta = NULL;
    s_kb_hint = NULL;
    s_poll_timer = NULL;
    s_poll_elapsed_ms = 0;
    s_connect_applied = false;

    setup_show_step(SETUP_STEP_WELCOME);
    ESP_LOGI(TAG, "setup wizard entered");
}

/** 引导页退出（页面 exit：删除定时器与模态，清空对象指针）。 */
static void setup_exit(void) {
    if (s_poll_timer != NULL) {
        lv_timer_delete(s_poll_timer);
        s_poll_timer = NULL;
    }
    setup_modal_close();
    s_content = NULL; /* 页面对象随清屏删除 */
    s_status_label = NULL;
    s_input_list = NULL;
    s_input_status = NULL;
    /* 恢复睡眠超时（以当前系统配置为准；引导期间曾被禁用）。 */
    espaperplay_epd_set_idle_sleep_timeout_ms(
        espaperplay_system_get_config()->epd_idle_sleep_timeout_ms);
    espaperplay_power_set_auto_sleep_timeout_ms(
        espaperplay_system_get_config()->auto_sleep_timeout_ms);
    ESP_LOGI(TAG, "setup wizard exited");
}

/** 按键处理：模态打开时单击关闭模态（键盘从本机配置打开则回到列表），
 * 其余无操作。 */
static void setup_on_key(const espaperplay_input_event_t *event) {
    if (event->key_action != ESPAPERPLAY_INPUT_KEY_ACTION_SINGLE_CLICK) {
        return;
    }
    if (s_modal != NULL) {
        setup_modal_close();
        setup_show_step(s_step == SETUP_STEP_INPUT ? SETUP_STEP_INPUT : SETUP_STEP_WELCOME);
    }
}

/** 引导页页面实例（页面栈用）。 */
const espaperplay_ui_page_t espaperplay_ui_page_setup = {setup_enter, setup_exit, setup_on_key,
                                                         NULL};
