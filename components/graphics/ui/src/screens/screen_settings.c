/*
 * SPDX-FileCopyrightText: 2026 ESPaperPlay Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#include "esp_heap_caps.h"
#include "esp_log.h"

#include "espaperplay_config.h"
#include "espaperplay_ui_util.h"
#include "espaperplay_ui_gesture.h"
#include "espaperplay_ui_modal.h"
#include "espaperplay_diaglog.h"
#include "espaperplay_sd_fonts.h"
#include "espaperplay_epd.h"
#include "espaperplay_fonts.h"
#include "espaperplay_gui.h"
#include "espaperplay_gui_lv.h"
#include "espaperplay_input.h"
#include "espaperplay_storage.h"
#include "espaperplay_system.h"
#include "espaperplay_ui.h"
#include "espaperplay_ui_touch.h"
#include "espaperplay_wifi.h"

#include "lvgl.h"
#include "icons_data.h"

static const char *TAG = "ESPaperPlay_UI";

/* 分类列表和二级详情沿用 Android 设置的阅读顺序。
 * 不使用惯性滚动：每类设置在一屏内呈现，返回先回分类，再回首页。
 * 设置应用仍由内部 RAM 栈任务执行，编辑弹窗确认后才持久化。 */
#define SETTINGS_MARGIN ESPAPERPLAY_UI_MARGIN
#define SETTINGS_REF_H 800
#define SETTINGS_UI_PERIOD_MS 5000

#define SETTINGS_FONT_MAX 16 /* 字体列表上限（内置 + SD 卡） */

#define SETTINGS_FONT_NAME                                                                         \
    (espaperplay_system_get_config()                                                               \
         ->selected_font) /* 当前选用字体（SD 优先，缺则回退 Flash 子集） */

/* ------------------------------------------------------------------ */
/* 行类型 / 行定义                                                       */
/* ------------------------------------------------------------------ */

typedef enum {
    SETTINGS_ROW_STEPPER = 0, /* 数值步进：点击打开编辑模态 */
    SETTINGS_ROW_CYCLE,       /* 循环切换：点击切到下一选项 */
    SETTINGS_ROW_WEB,         /* 仅展示：提示到 Web 管理页配置 */
    SETTINGS_ROW_INFO,        /* 仅展示：信息（如软件版本） */
    SETTINGS_ROW_FONT,        /* 字体选择：点击打开字体列表 */
    SETTINGS_ROW_ACTION,      /* 动作：点击执行（进入测试页） */
} settings_row_type_t;

typedef struct {
    const char *name;                                     /*!< 设置项名称 */
    settings_row_type_t type;                             /*!< 行类型 */
    lv_obj_t *row_obj;                                    /*!< 行对象（命中检测用，构建时填充） */
    lv_obj_t *value_label;                                /*!< 值标签（构建时填充） */
    char last_value[64]; /* 原始值缓存：LVGL 的省略号模式会临时改写标签文本。 */
    int32_t min, max, step;                               /*!< STEPPER：范围与步进 */
    int32_t (*get_value)(void);                           /*!< STEPPER/CYCLE：读取当前值 */
    esp_err_t (*set_value)(int32_t);                      /*!< STEPPER/CYCLE：写入新值 */
    void (*fmt_value)(int32_t v, char *buf, size_t size); /*!< 格式化值显示 */
    const char *const *options;                           /*!< CYCLE：选项文本 */
    int option_count;                                     /*!< CYCLE：选项数 */
    bool apply_wifi;         /*!< CYCLE：切换后重新应用 WiFi（WiFi 模式） */
    void (*on_action)(void); /*!< ACTION：点击回调 */
} settings_row_t;

/* ---- 设备组：显示 ---- */

static int32_t settings_get_sleep_timeout(void) {
    return (int32_t)(espaperplay_system_get_config()->epd_idle_sleep_timeout_ms / 1000);
}

static esp_err_t settings_set_sleep_timeout(int32_t v) {
    esp_err_t err = espaperplay_system_set_epd_idle_sleep_timeout_ms((uint32_t)v * 1000);
    if (err == ESP_OK) {
        /* 立即应用到驱动（不必等重启）。 */
        err = espaperplay_epd_set_idle_sleep_timeout_ms((uint32_t)v * 1000);
    }
    return err;
}

static void settings_fmt_sleep_timeout(int32_t v, char *buf, size_t size) {
    if (v == 0) {
        snprintf(buf, size, "关闭");
    } else {
        snprintf(buf, size, "%d 秒", (int)v);
    }
}

static int32_t settings_get_full_force(void) {
    return (int32_t)espaperplay_system_get_config()->gui_full_force_after;
}

static esp_err_t settings_set_full_force(int32_t v) {
    esp_err_t err = espaperplay_system_set_gui_full_force_after((uint32_t)v);
    if (err == ESP_OK) {
        /* 立即应用到渲染后端（不必等重启）。 */
        err = espaperplay_gui_set_full_force_after((uint32_t)v);
    }
    return err;
}

static void settings_fmt_full_force(int32_t v, char *buf, size_t size) {
    if (v == 0) {
        snprintf(buf, size, "禁用");
    } else {
        snprintf(buf, size, "%d 次", (int)v);
    }
}

/* ---- 设备组：按键 ---- */

static const char *const s_boot_lp_options[] = {"全屏刷新", "返回上一页", "无操作"};

static int32_t settings_get_boot_lp_action(void) {
    return (int32_t)espaperplay_system_get_config()->boot_long_press_action;
}

static esp_err_t settings_set_boot_lp_action(int32_t v) {
    return espaperplay_system_set_boot_long_press_action((espaperplay_boot_long_press_action_t)v);
}

static void settings_fmt_boot_lp_action(int32_t v, char *buf, size_t size) {
    if (v >= 0 && v < (int32_t)(sizeof(s_boot_lp_options) / sizeof(s_boot_lp_options[0]))) {
        snprintf(buf, size, "%s", s_boot_lp_options[v]);
    } else {
        snprintf(buf, size, "未知");
    }
}

static int32_t settings_get_boot_lp_time(void) {
    return (int32_t)espaperplay_system_get_config()->boot_long_press_time_ms;
}

static esp_err_t settings_set_boot_lp_time(int32_t v) {
    esp_err_t err = espaperplay_system_set_boot_long_press_time_ms((uint32_t)v);
    if (err == ESP_OK) {
        /* 立即应用到按键驱动（不必等重启）。Web 服务早于 input 启动时按键
         * 尚未创建（INVALID_STATE）：初始值已在 input_init 读取配置，仅告警。 */
        const esp_err_t apply_err = espaperplay_input_set_boot_long_press_time_ms((uint32_t)v);
        if (apply_err != ESP_OK && apply_err != ESP_ERR_INVALID_STATE) {
            ESP_LOGW(TAG, "apply boot long-press time failed: %s", esp_err_to_name(apply_err));
        }
    }
    return err;
}

static void settings_fmt_boot_lp_time(int32_t v, char *buf, size_t size) {
    snprintf(buf, size, "%d ms", (int)v);
}

/* ---- 设备组：阅读器 ---- */

static const char *const s_reader_img_gray4_options[] = {"开", "关"};

static int32_t settings_get_reader_img_gray4(void) {
    return espaperplay_system_get_reader_img_gray4() ? 1 : 0;
}

static esp_err_t settings_set_reader_img_gray4(int32_t v) {
    return espaperplay_system_set_reader_img_gray4(v != 0);
}

static void settings_fmt_reader_img_gray4(int32_t v, char *buf, size_t size) {
    if (v >= 0 && v < (int32_t)(sizeof(s_reader_img_gray4_options) / sizeof(s_reader_img_gray4_options[0]))) {
        snprintf(buf, size, "%s", s_reader_img_gray4_options[v]);
    } else {
        snprintf(buf, size, "未知");
    }
}

/* ---- 系统组：网络 ---- */

static const char *const s_wifi_mode_options[] = {"热点 (AP)", "站点 (STA)"};

static int32_t settings_get_wifi_mode(void) {
    return (int32_t)espaperplay_system_get_config()->wifi_mode;
}

static void settings_fmt_wifi_mode(int32_t v, char *buf, size_t size) {
    (void)v;
    const espaperplay_system_config_t *cfg = espaperplay_system_get_config();
    snprintf(buf, size, "%s",
             cfg->wifi_mode == ESPAPERPLAY_WIFI_MODE_AP ? "热点 (AP)" : "站点 (STA)");
}

static void settings_fmt_wifi_cred(int32_t v, char *buf, size_t size) {
    (void)v;
    snprintf(buf, size, "Web 配置");
}

/* ---- 系统组：字体 / 版本 / 开发者 ---- */

static void settings_fmt_font(int32_t v, char *buf, size_t size) {
    (void)v;
    const espaperplay_system_config_t *cfg = espaperplay_system_get_config();
    snprintf(buf, size, "%s",
             cfg->selected_font[0] ? cfg->selected_font : ESPAPERPLAY_FONTS_DEFAULT_NAME);
}

static void settings_fmt_version(int32_t v, char *buf, size_t size) {
    (void)v;
    snprintf(buf, size, "v%s", ESPAPERPLAY_VERSION);
}

/* ---- 系统组：SD 卡日志等级 ---- */

static const char *const s_sd_log_level_options[] = {"错误", "警告", "信息"};

static int32_t settings_get_sd_log_level(void) {
    const int32_t v = (int32_t)espaperplay_diaglog_get_level() - ESP_LOG_ERROR;
    if (v < 0 || v >= (int32_t)(sizeof(s_sd_log_level_options) / sizeof(s_sd_log_level_options[0]))) {
        return (int32_t)ESPAPERPLAY_SYSTEM_DEFAULT_SD_LOG_LEVEL - ESP_LOG_ERROR;
    }
    return v;
}

static esp_err_t settings_set_sd_log_level(int32_t v) {
    const esp_log_level_t level = (esp_log_level_t)(v + ESP_LOG_ERROR);
    esp_err_t err = espaperplay_system_set_sd_log_level(level);
    if (err == ESP_OK) {
        /* 立即应用到日志捕获（不必等重启）。 */
        espaperplay_diaglog_set_level(level);
    }
    return err;
}

static void settings_fmt_sd_log_level(int32_t v, char *buf, size_t size) {
    if (v >= 0 && v < (int32_t)(sizeof(s_sd_log_level_options) / sizeof(s_sd_log_level_options[0]))) {
        snprintf(buf, size, "%s 及以上", s_sd_log_level_options[v]);
    } else {
        snprintf(buf, size, "未知");
    }
}

static void settings_open_test(void) {
    ESP_LOGI(TAG, "settings: open test page");
    espaperplay_ui_page_push_lv(&espaperplay_ui_page_test);
}

/* 预览不修改自动睡眠配置；任意触摸即可返回设置页。 */
static void settings_open_screensaver(void) {
    espaperplay_ui_page_push_lv(&espaperplay_ui_page_screensaver);
}

static void settings_open_setup(void) {
    ESP_LOGI(TAG, "settings: open setup wizard");
    espaperplay_ui_page_push_lv(&espaperplay_ui_page_setup);
}

static void settings_open_wifi_list(void) {
    ESP_LOGI(TAG, "settings: open wifi list");
    espaperplay_ui_page_push_lv(&espaperplay_ui_page_wifi_list);
}

/* ---- 服务组：天气 ---- */

static void settings_fmt_weather_key(int32_t v, char *buf, size_t size) {
    (void)v;
    const espaperplay_system_config_t *cfg = espaperplay_system_get_config();
    snprintf(buf, size, "%s", cfg->weather_api_key[0] ? "已配置" : "未配置");
}

static void settings_fmt_weather_loc(int32_t v, char *buf, size_t size) {
    (void)v;
    const espaperplay_system_config_t *cfg = espaperplay_system_get_config();
    if (cfg->weather_location[0]) {
        snprintf(buf, size, "%s", cfg->weather_location);
    } else {
        snprintf(buf, size, "自动定位");
    }
}

static void settings_fmt_weather_host(int32_t v, char *buf, size_t size) {
    (void)v;
    const espaperplay_system_config_t *cfg = espaperplay_system_get_config();
    snprintf(buf, size, "%s", cfg->weather_api_host[0] ? "已配置" : "公共地址");
}

/* 行定义（顺序与分页一致，见 s_pages）。 */
static settings_row_t s_rows[] = {
    /* 设备：显示 */
    {.name = "显示驱动休眠",
     .type = SETTINGS_ROW_STEPPER,
     .min = 0,
     .max = 86400,
     .step = 30,
     .get_value = settings_get_sleep_timeout,
     .set_value = settings_set_sleep_timeout,
     .fmt_value = settings_fmt_sleep_timeout},
    {.name = "连续局刷强制全刷",
     .type = SETTINGS_ROW_STEPPER,
     .min = 0,
     .max = 255,
     .step = 1,
     .get_value = settings_get_full_force,
     .set_value = settings_set_full_force,
     .fmt_value = settings_fmt_full_force},
    /* 设备：按键 */
    {.name = "BOOT 键长按动作",
     .type = SETTINGS_ROW_CYCLE,
     .get_value = settings_get_boot_lp_action,
     .set_value = settings_set_boot_lp_action,
     .fmt_value = settings_fmt_boot_lp_action,
     .options = s_boot_lp_options,
     .option_count = (int)(sizeof(s_boot_lp_options) / sizeof(s_boot_lp_options[0]))},
    {.name = "BOOT 键长按判定时间",
     .type = SETTINGS_ROW_STEPPER,
     .min = (int32_t)ESPAPERPLAY_SYSTEM_BOOT_LONG_PRESS_TIME_MIN_MS,
     .max = (int32_t)ESPAPERPLAY_SYSTEM_BOOT_LONG_PRESS_TIME_MAX_MS,
     .step = 100,
     .get_value = settings_get_boot_lp_time,
     .set_value = settings_set_boot_lp_time,
     .fmt_value = settings_fmt_boot_lp_time},
    /* 设备：阅读器 */
    {.name = "插图灰度刷新",
     .type = SETTINGS_ROW_CYCLE,
     .get_value = settings_get_reader_img_gray4,
     .set_value = settings_set_reader_img_gray4,
     .fmt_value = settings_fmt_reader_img_gray4,
     .options = s_reader_img_gray4_options,
     .option_count =
         (int)(sizeof(s_reader_img_gray4_options) / sizeof(s_reader_img_gray4_options[0]))},
    /* 系统：网络 */
    {.name = "WiFi 模式",
     .type = SETTINGS_ROW_CYCLE,
     .apply_wifi = true,
     .get_value = settings_get_wifi_mode,
     .set_value = NULL,
     .fmt_value = settings_fmt_wifi_mode,
     .options = s_wifi_mode_options,
     .option_count = (int)(sizeof(s_wifi_mode_options) / sizeof(s_wifi_mode_options[0]))},
    {.name = "WiFi 网络", .type = SETTINGS_ROW_ACTION, .on_action = settings_open_wifi_list},
    {.name = "WiFi 凭据", .type = SETTINGS_ROW_WEB, .fmt_value = settings_fmt_wifi_cred},
    {.name = "重新运行开机引导", .type = SETTINGS_ROW_ACTION, .on_action = settings_open_setup},
    /* 系统：字体 / 版本 / 开发者 */
    {.name = "当前字体", .type = SETTINGS_ROW_FONT, .fmt_value = settings_fmt_font},
    {.name = "软件版本", .type = SETTINGS_ROW_INFO, .fmt_value = settings_fmt_version},
    {.name = "SD 日志等级",
     .type = SETTINGS_ROW_CYCLE,
     .get_value = settings_get_sd_log_level,
     .set_value = settings_set_sd_log_level,
     .fmt_value = settings_fmt_sd_log_level,
     .options = s_sd_log_level_options,
     .option_count = (int)(sizeof(s_sd_log_level_options) / sizeof(s_sd_log_level_options[0]))},
    {.name = "测试页", .type = SETTINGS_ROW_ACTION, .on_action = settings_open_test},
    {.name = "屏保预览", .type = SETTINGS_ROW_ACTION, .on_action = settings_open_screensaver},
    /* 服务：天气 */
    {.name = "天气 API Key", .type = SETTINGS_ROW_WEB, .fmt_value = settings_fmt_weather_key},
    {.name = "天气位置", .type = SETTINGS_ROW_WEB, .fmt_value = settings_fmt_weather_loc},
    {.name = "天气 API Host", .type = SETTINGS_ROW_WEB, .fmt_value = settings_fmt_weather_host},
};

#define SETTINGS_ROW_CNT (int)(sizeof(s_rows) / sizeof(s_rows[0]))
typedef struct {
    const char *title;
    const char *summary;
    const lv_image_dsc_t *icon;
    int rows[4];
    int count;
    const char *hint;
} settings_category_t;
static const settings_category_t s_categories[] = {
    {"网络与连接", "Wi-Fi、连接方式与设备配置", &icon_wifi_16, {5, 6, 7, 8}, 4,
     "Wi-Fi 名称和密码可在 Web 管理页配置。"},
    {"显示", "屏幕刷新、字体与屏保", &icon_settings_64, {0, 1, 9, 13}, 4,
     "屏幕休眠只关闭显示驱动，不影响设备联网。"},
    {"按键", "长按动作与响应时间", &icon_debug_64, {2, 3}, 2, NULL},
    {"阅读器", "图像显示与阅读偏好", &icon_reader_64, {4}, 1,
     "字体大小可在阅读时通过底部工具栏调整。"},
    {"天气服务", "位置与服务配置", &icon_weather_64, {14, 15, 16}, 3,
     "请在 Web 管理页配置天气服务。"},
    {"系统", "版本、日志与设备测试", &icon_debug_64, {10, 11, 12}, 3, NULL},
};
#define SETTINGS_CATEGORY_CNT (int)(sizeof(s_categories) / sizeof(s_categories[0]))
static int s_category = -1;
static bool s_resume_category = false;
static lv_obj_t *s_content = NULL;
static lv_obj_t *s_category_rows[SETTINGS_CATEGORY_CNT];
static int s_detail_row_h;
static lv_timer_t *s_timer = NULL;                /*!< 周期刷新定时器 */
static lv_obj_t *s_modal = NULL;                  /*!< 模态覆盖层（NULL=未打开） */
static settings_row_t *s_modal_row = NULL;        /*!< 模态正在编辑的行 */
static int32_t s_modal_value = 0;                 /*!< 模态编辑中的值 */
static lv_obj_t *s_modal_value_label = NULL;      /*!< 模态中的值标签 */
static settings_row_t *s_confirm_row = NULL;      /*!< 确认模态目标行 */
static int32_t s_confirm_value = 0;               /*!< 确认模态目标值 */
static espaperplay_ui_status_bar_t *s_bar = NULL; /*!< 统一状态栏 */

/* 字体列表（模态构建时收集：出厂内置 + SD 卡）。 */
static char s_font_names[SETTINGS_FONT_MAX][ESPAPERPLAY_SYSTEM_FONT_NAME_MAX_LEN];
static int s_font_count = 0;

/* 手势跟踪 */
static bool s_touch_down = false;
static lv_point_t s_touch_start = {0, 0};
static lv_point_t s_touch_last = {0, 0};
static settings_row_t *s_touch_row = NULL; /*!< 按下起点命中的行（-1=无） */

/* 前向声明 */
static void settings_modal_close(void);
static void settings_row_trigger(settings_row_t *row);

/* ------------------------------------------------------------------ */
/* 工具函数                                                             */
/* ------------------------------------------------------------------ */

/** 刷新单行值标签（仅文本变化时更新，EPD 上避免无谓刷新）。 */
static void settings_row_refresh(settings_row_t *row) {
    if (row->value_label == NULL || row->fmt_value == NULL) {
        return;
    }
    char buf[64];
    row->fmt_value(row->get_value ? row->get_value() : 0, buf, sizeof(buf));
    if (strcmp(row->last_value, buf) != 0) {
        strlcpy(row->last_value, buf, sizeof(row->last_value));
        lv_label_set_text(row->value_label, buf);
    }
}

/** 刷新全部行（进入页面 / 周期刷新 / 修改后调用）。 */
static void settings_refresh_all(void) {
    for (int i = 0; i < SETTINGS_ROW_CNT; i++) {
        settings_row_refresh(&s_rows[i]);
    }
    /* 统一状态栏（时间/WiFi/睡眠图标）由统一调度定时器周期刷新；此处
     * 立即刷新一次，确保进入设置页时即时显示。 */
    espaperplay_ui_status_bar_refresh(s_bar);
}

/** 查找指定类型的行（模态修改后刷新对应行用）。 */
static settings_row_t *settings_row_find(settings_row_type_t type) {
    for (int i = 0; i < SETTINGS_ROW_CNT; i++) {
        if (s_rows[i].type == type) {
            return &s_rows[i];
        }
    }
    return NULL;
}

/* ------------------------------------------------------------------ */
/* 设置应用任务（NVS 写入必须在内部 RAM 栈任务中执行）                     */
/* ------------------------------------------------------------------ */

/**
 * LVGL 任务栈位于 PSRAM（见 lvgl_port.c：32KB 内部 RAM 分配失败，PSRAM 栈
 * 是官方支持做法）。而 NVS / flash 操作会禁用 flash 缓存，期间 CPU 无法
 * 访问 PSRAM——若在 LVGL 任务里直接写 NVS，会触发
 * esp_task_stack_is_sane_cache_disabled() 断言崩溃（实测保存屏幕睡眠超时
 * 时崩溃）。因此所有 NVS 写入 + 服务应用都投递到本任务（内部 RAM 栈）
 * 执行，完成后经 espaperplay_gui_lv_call 回 LVGL 线程刷新行显示。
 */

typedef enum {
    SETTINGS_OP_STEPPER = 0, /* 数值步进：应用新值 */
    SETTINGS_OP_CYCLE,       /* 循环切换：应用新值 */
    SETTINGS_OP_WIFI_MODE,   /* WiFi 模式：持久化 + 重新应用网络 */
    SETTINGS_OP_FONT,        /* 选择字体（font_name 有效） */
} settings_op_type_t;

typedef struct {
    settings_op_type_t type;                              /*!< 操作类型 */
    settings_row_t *row;                                  /*!< 目标行（静态数据，跨任务安全） */
    int32_t value;                                        /*!< STEPPER/CYCLE/WIFI_MODE 新值 */
    char font_name[ESPAPERPLAY_SYSTEM_FONT_NAME_MAX_LEN]; /*!< FONT：字体文件名 */
} settings_op_t;

static QueueHandle_t s_op_queue = NULL;   /*!< 设置应用操作队列 */
static TaskHandle_t s_worker_task = NULL; /*!< 设置应用任务（内部 RAM 栈） */

/** 回 LVGL 线程刷新指定行（EPD 更新必须在 LVGL 线程）。 */
static void settings_apply_refresh_lv(void *arg) {
    settings_row_t *row = arg;
    if (row != NULL) {
        settings_row_refresh(row);
    }
}

/** 设置应用任务：从队列取操作，执行 NVS 写入 + 服务应用，再回 LVGL 刷新。 */
static void settings_worker_task(void *arg) {
    (void)arg;
    settings_op_t op;
    for (;;) {
        if (xQueueReceive(s_op_queue, &op, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        settings_row_t *row = op.row;
        if (row == NULL) {
            continue;
        }
        esp_err_t err = ESP_OK;
        if (op.type == SETTINGS_OP_FONT) {
            err = espaperplay_system_set_selected_font(op.font_name);
        } else if (op.type == SETTINGS_OP_WIFI_MODE) {
            err = espaperplay_system_set_wifi_mode((espaperplay_wifi_mode_t)op.value);
            if (err == ESP_OK) {
                /* 重新应用网络（切换模式会重建接口，可能改变 IP）。 */
                err = espaperplay_wifi_start();
            }
        } else if (row->set_value != NULL) {
            err = row->set_value(op.value);
        }
        if (err == ESP_OK) {
            ESP_LOGI(TAG, "settings: %s -> %d", row->name, (int)op.value);
            /* 回 LVGL 线程刷新行显示（EPD 更新必须在 LVGL 线程）。 */
            espaperplay_gui_lv_call(settings_apply_refresh_lv, row, 100);
        } else {
            ESP_LOGW(TAG, "settings: set %s failed: %s", row->name, esp_err_to_name(err));
        }
    }
}

/** 确保设置应用任务已创建（首次使用时创建，常驻程序生命周期）。 */
static void settings_worker_ensure(void) {
    if (s_op_queue != NULL && s_worker_task != NULL) {
        return;
    }
    if (s_op_queue == NULL) {
        s_op_queue = xQueueCreate(8, sizeof(settings_op_t));
    }
    if (s_op_queue != NULL && s_worker_task == NULL) {
        /* 栈必须放内部 RAM（NVS/flash 操作禁用缓存期间可访问）；CONFIG_SPIRAM_USE_MALLOC
         * 下 xTaskCreate 默认栈在 PSRAM，须用 xTaskCreateWithCaps 显式指定。 */
        if (xTaskCreateWithCaps(settings_worker_task, "ui_set", 4096, NULL, 4, &s_worker_task,
                                MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT) != pdPASS) {
            ESP_LOGE(TAG, "settings: worker task create failed");
        }
    }
}

/** 投递一个设置应用操作（LVGL 线程内调用，非阻塞）。 */
static void settings_op_post(const settings_op_t *op) {
    settings_worker_ensure();
    if (s_op_queue == NULL) {
        ESP_LOGW(TAG, "settings: op queue unavailable");
        return;
    }
    if (xQueueSend(s_op_queue, op, 0) != pdTRUE) {
        ESP_LOGW(TAG, "settings: op queue full");
    }
}

/* ------------------------------------------------------------------ */
/* 行 / 分页构建                                                         */
/* ------------------------------------------------------------------ */

/** 行是否可交互（决定右侧是否画 ">" 箭头：可点 / 只读的唯一视觉区分）。 */
static bool settings_row_tappable(const settings_row_t *row) {
    return row->type == SETTINGS_ROW_STEPPER || row->type == SETTINGS_ROW_CYCLE ||
           row->type == SETTINGS_ROW_FONT || row->type == SETTINGS_ROW_ACTION;
}

/** 设置项采用上下两行：名称和当前值分别完整占用行宽。 */
static void settings_row_create(lv_obj_t *parent, settings_row_t *row, int x, int y, int w) {
    lv_obj_t *obj = lv_obj_create(parent);
    lv_obj_set_pos(obj, x, y);
    lv_obj_set_size(obj, w, s_detail_row_h);
    espaperplay_ui_card_style(obj);
    lv_obj_set_style_radius(obj, 0, 0);
    lv_obj_set_style_border_side(obj, LV_BORDER_SIDE_BOTTOM, 0);
    row->row_obj = obj;
    lv_obj_t *name = espaperplay_ui_label_create(obj, row->name, 20, LV_TEXT_ALIGN_LEFT);
    lv_obj_set_pos(name, 8, 12); lv_obj_set_width(name, w - 44);
    lv_obj_t *value = espaperplay_ui_label_create(obj, row->type == SETTINGS_ROW_ACTION ? "点击打开" : "", 16, LV_TEXT_ALIGN_LEFT);
    lv_obj_set_pos(value, 8, 44); lv_obj_set_size(value, w - 44, 24);
    lv_label_set_long_mode(value, LV_LABEL_LONG_DOT);
    row->value_label = value;
    row->last_value[0] = '\0';
    if (settings_row_tappable(row)) {
        espaperplay_ui_chevron_create(obj, w - 32, (s_detail_row_h - 20) / 2, false);
    }
}

static void settings_content_build(void) {
    for (int i = 0; i < SETTINGS_ROW_CNT; i++) {
        s_rows[i].row_obj = NULL;
        s_rows[i].value_label = NULL;
    }
    memset(s_category_rows, 0, sizeof(s_category_rows));
    if (s_content) lv_obj_delete(s_content);
    int32_t w, h; espaperplay_ui_screen_size(&w, &h);
    s_content = lv_obj_create(lv_screen_active());
    lv_obj_set_pos(s_content, 0, ESPAPERPLAY_UI_HEADER_H);
    lv_obj_set_size(s_content, w, h - ESPAPERPLAY_UI_HEADER_H);
    espaperplay_ui_card_style(s_content);
    lv_obj_set_style_border_width(s_content, 0, 0);
    lv_obj_set_style_radius(s_content, 0, 0);
    if (h < 700) {
        lv_obj_add_flag(s_content, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_scroll_dir(s_content, LV_DIR_VER);
    }
    espaperplay_ui_chevron_create(s_content, 26, 30, true);
    const settings_category_t *cat = s_category >= 0 ? &s_categories[s_category] : NULL;
    lv_obj_t *title = espaperplay_ui_label_create(s_content, cat ? cat->title : "设置", 32, LV_TEXT_ALIGN_LEFT);
    lv_obj_set_pos(title, 76, 18); lv_obj_set_width(title, w - 96);
    lv_obj_t *summary = espaperplay_ui_label_create(s_content,
        cat ? cat->summary : "让设备更合你的习惯", 16, LV_TEXT_ALIGN_LEFT);
    lv_obj_set_pos(summary, 24, 80); lv_obj_set_width(summary, w - 48);
    if (!cat) {
        int row_h = (h - ESPAPERPLAY_UI_HEADER_H - 132) / SETTINGS_CATEGORY_CNT;
        if (row_h < 88) row_h = 88;
        for (int i = 0; i < SETTINGS_CATEGORY_CNT; i++) {
            const settings_category_t *c = &s_categories[i];
            lv_obj_t *row = lv_obj_create(s_content);
            lv_obj_set_pos(row, 16, 120 + i * row_h); lv_obj_set_size(row, w - 32, row_h);
            espaperplay_ui_card_style(row); lv_obj_set_style_border_width(row, 0, 0);
            s_category_rows[i] = row;
            if (i == 2 || i == 5) {
                lv_obj_t *symbol = lv_obj_create(row);
                espaperplay_ui_card_style(symbol);
                lv_obj_set_size(symbol, i == 2 ? 24 : 32, 32);
                lv_obj_set_pos(symbol, i == 2 ? 16 : 12, (row_h - 32) / 2);
                lv_obj_set_style_radius(symbol, i == 2 ? 5 : LV_RADIUS_CIRCLE, 0);
                lv_obj_set_style_border_width(symbol, 2, 0);
                lv_obj_t *mark = espaperplay_ui_label_create(symbol, i == 2 ? "-" : "i", 20, LV_TEXT_ALIGN_CENTER);
                lv_obj_set_size(mark, i == 2 ? 20 : 28, 28);
                lv_obj_center(mark);
            } else {
                lv_obj_t *icon = lv_image_create(row);
                lv_image_set_src(icon, c->icon); lv_image_set_pivot(icon, 0, 0);
                lv_image_set_scale(icon, c->icon->header.w == 16 ? 512 : 128);
                lv_obj_set_pos(icon, 12, (row_h - 32) / 2);
            }
            lv_obj_t *name = espaperplay_ui_label_create(row, c->title, 24, LV_TEXT_ALIGN_LEFT);
            lv_obj_set_pos(name, 64, (row_h - 66) / 2); lv_obj_set_width(name, w - 136);
            lv_obj_t *sub = espaperplay_ui_label_create(row, c->summary, 16, LV_TEXT_ALIGN_LEFT);
            lv_obj_set_pos(sub, 64, (row_h - 66) / 2 + 38); lv_obj_set_width(sub, w - 136);
            espaperplay_ui_chevron_create(row, w - 64, (row_h - 20) / 2, false);
        }
    } else {
        s_detail_row_h = (h - ESPAPERPLAY_UI_HEADER_H - 220) / cat->count;
        if (s_detail_row_h > 96) s_detail_row_h = 96;
        if (s_detail_row_h < 72) s_detail_row_h = 72;
        for (int i = 0; i < cat->count; i++) {
            settings_row_create(s_content, &s_rows[cat->rows[i]], 16,
                                124 + i * s_detail_row_h, w - 32);
        }
        if (cat->hint) {
            lv_obj_t *hint = espaperplay_ui_label_create(s_content, cat->hint, 16, LV_TEXT_ALIGN_LEFT);
            lv_obj_set_pos(hint, 24, 146 + cat->count * s_detail_row_h);
            lv_obj_set_width(hint, w - 48);
        }
    }
    settings_refresh_all();
}

static void settings_back(void) {
    if (s_category >= 0) { s_category = -1; settings_content_build(); }
    else if (espaperplay_ui_page_depth() > 1) { s_resume_category = false; espaperplay_ui_page_pop_lv(); }
}

/* ------------------------------------------------------------------ */
/* 模态：数值步进编辑                                                    */
/* ------------------------------------------------------------------ */

/** 模态覆盖层点击（点击空白处关闭，不保存）。 */
static void settings_modal_overlay_cb(lv_event_t *e) {
    (void)e;
    settings_modal_close();
}

/** 模态 [-] 按钮：步进减小（钳制到下限）。 */
static void settings_modal_minus_cb(lv_event_t *e) {
    lv_event_stop_bubbling(e);
    s_modal_value -= s_modal_row->step;
    if (s_modal_value < s_modal_row->min) {
        s_modal_value = s_modal_row->min;
    }
    char buf[32];
    s_modal_row->fmt_value(s_modal_value, buf, sizeof(buf));
    lv_label_set_text(s_modal_value_label, buf);
}

/** 模态 [+] 按钮：步进增大（钳制到上限）。 */
static void settings_modal_plus_cb(lv_event_t *e) {
    lv_event_stop_bubbling(e);
    s_modal_value += s_modal_row->step;
    if (s_modal_value > s_modal_row->max) {
        s_modal_value = s_modal_row->max;
    }
    char buf[32];
    s_modal_row->fmt_value(s_modal_value, buf, sizeof(buf));
    lv_label_set_text(s_modal_value_label, buf);
}

/** 模态 完成 按钮：投递应用操作并关闭（NVS 写入由设置应用任务执行）。 */
static void settings_modal_done_cb(lv_event_t *e) {
    lv_event_stop_bubbling(e);
    settings_op_t op = {0};
    op.type = SETTINGS_OP_STEPPER;
    op.row = s_modal_row;
    op.value = s_modal_value;
    settings_op_post(&op);
    settings_modal_close();
}

/** 关闭模态（删除覆盖层）。 */
static void settings_modal_close(void) {
    if (s_modal != NULL) {
        lv_obj_del(s_modal);
        s_modal = NULL;
    }
    s_modal_row = NULL;
    s_modal_value_label = NULL;
    s_confirm_row = NULL;
    ESP_LOGI(TAG, "settings: modal closed");
}

/* ------------------------------------------------------------------ */
/* 模态：确认对话框（WiFi 模式切换等需要二级确认的操作）                   */
/* ------------------------------------------------------------------ */

/** 确认模态 取消 按钮：关闭，不执行。 */
static void settings_confirm_cancel_cb(lv_event_t *e) {
    lv_event_stop_bubbling(e);
    settings_modal_close();
}

/** 确认模态 确定 按钮：投递应用操作并关闭（NVS 写入由设置应用任务执行）。 */
static void settings_confirm_ok_cb(lv_event_t *e) {
    lv_event_stop_bubbling(e);
    settings_op_t op = {0};
    op.type = SETTINGS_OP_WIFI_MODE;
    op.row = s_confirm_row;
    op.value = s_confirm_value;
    settings_op_post(&op);
    settings_modal_close();
}

/** 打开确认对话框模态（标题 + 消息 + 取消/确定）。 */
static void settings_confirm_modal_open(settings_row_t *row, int32_t new_value,
                                        const char *message) {
    s_confirm_row = row;
    s_confirm_value = new_value;

    int32_t scr_w, scr_h;
    espaperplay_ui_screen_size(&scr_w, &scr_h);

    /* 全屏覆盖层（点空白关闭）+ 居中卡片（尺寸按屏高缩放）。 */
    s_modal = espaperplay_ui_modal_overlay_create(true, settings_modal_overlay_cb);
    lv_obj_t *card = espaperplay_ui_modal_card_create(s_modal, espaperplay_ui_scaled(340),
                                                      espaperplay_ui_scaled(250));

    /* 标题 */
    espaperplay_ui_modal_card_title(card, row->name, espaperplay_ui_scaled(14), 0);

    /* 消息（可换行居中） */
    lv_obj_t *msg = espaperplay_ui_label_create(card, message, 16, LV_TEXT_ALIGN_CENTER);
    lv_obj_set_width(msg, espaperplay_ui_scaled(300));
    lv_obj_set_pos(msg, espaperplay_ui_scaled(20), espaperplay_ui_scaled(60));
    lv_label_set_long_mode(msg, LV_LABEL_LONG_WRAP);

    /* 取消（白底黑边） */
    espaperplay_ui_modal_button(card, "取消", espaperplay_ui_scaled(30), espaperplay_ui_scaled(180),
                                espaperplay_ui_scaled(120), espaperplay_ui_scaled(44), false,
                                settings_confirm_cancel_cb, NULL);

    /* 确定（黑底白字） */
    espaperplay_ui_modal_button(card, "确定", espaperplay_ui_scaled(190), espaperplay_ui_scaled(180),
                                espaperplay_ui_scaled(120), espaperplay_ui_scaled(44), true,
                                settings_confirm_ok_cb, NULL);

    ESP_LOGI(TAG, "settings: confirm modal open (%s -> %d)", row->name, (int)new_value);
}

/** 打开数值步进编辑模态。 */
static void settings_stepper_modal_open(settings_row_t *row) {
    s_modal_row = row;
    s_modal_value = row->get_value();

    int32_t scr_w, scr_h;
    espaperplay_ui_screen_size(&scr_w, &scr_h);

    const int w = scr_w - 48;
    /* 数值与步进按钮放在同一控制组；打开时的释放事件不关闭背景。 */
    s_modal = espaperplay_ui_modal_overlay_create(false, NULL);
    lv_obj_t *card = espaperplay_ui_modal_card_create(s_modal, w, 300);
    lv_obj_t *title = espaperplay_ui_label_create(card, row->name, 24, LV_TEXT_ALIGN_LEFT);
    lv_obj_set_pos(title, 24, 20); lv_obj_set_width(title, w - 48);
    const char *help = row->get_value == settings_get_sleep_timeout
        ? "仅关闭显示驱动，设备保持联网。\n设为 0 可关闭显示驱动休眠。"
        : "调整数值后点击保存，取消不会应用修改。";
    lv_obj_t *hint = espaperplay_ui_label_create(card, help, 16, LV_TEXT_ALIGN_LEFT);
    lv_obj_set_pos(hint, 24, 66); lv_obj_set_width(hint, w - 48);
    lv_obj_t *control = lv_obj_create(card);
    lv_obj_set_pos(control, 24, 130); lv_obj_set_size(control, w - 48, 72);
    espaperplay_ui_card_style(control);
    lv_obj_t *minus = espaperplay_ui_modal_button(control, "-", 6, 9, 52, 52,
        false, settings_modal_minus_cb, NULL);
    lv_obj_t *plus = espaperplay_ui_modal_button(control, "+", w - 108, 9, 52, 52,
        false, settings_modal_plus_cb, NULL);
    lv_obj_set_style_border_width(minus, 0, 0);
    lv_obj_set_style_border_width(plus, 0, 0);
    s_modal_value_label = espaperplay_ui_label_create(control, "", 32, LV_TEXT_ALIGN_CENTER);
    lv_obj_set_size(s_modal_value_label, w - 180, 48);
    lv_obj_align(s_modal_value_label, LV_ALIGN_CENTER, 0, 0);
    const int bw = (w - 60) / 2;
    lv_obj_t *cancel = espaperplay_ui_modal_button(card, "取消", 24, 232, bw, 44,
        false, settings_confirm_cancel_cb, NULL);
    lv_obj_set_style_border_width(cancel, 0, 0);
    lv_obj_t *save = espaperplay_ui_modal_button(card, "保存", 36 + bw, 232, bw, 44,
        true, settings_modal_done_cb, NULL);
    lv_obj_set_style_radius(save, 22, 0);

    char buf[32];
    row->fmt_value(s_modal_value, buf, sizeof(buf));
    lv_label_set_text(s_modal_value_label, buf);

    ESP_LOGI(TAG, "settings: stepper modal open (%s)", row->name);
}

/* ------------------------------------------------------------------ */
/* 模态：字体选择                                                        */
/* ------------------------------------------------------------------ */

/** 收集字体列表：出厂内置 + SD 卡（/sdcard/system/fonts/，规则见 sd_fonts 组件）。 */
static void settings_font_collect(void) {
    s_font_count = 0;
    strlcpy(s_font_names[s_font_count++], ESPAPERPLAY_FONTS_DEFAULT_NAME, sizeof(s_font_names[0]));

    char (*sd_names)[ESPAPERPLAY_SD_FONTS_NAME_MAX] =
        heap_caps_malloc(SETTINGS_FONT_MAX * ESPAPERPLAY_SD_FONTS_NAME_MAX,
                         MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (sd_names == NULL) {
        return;
    }
    const int n = espaperplay_sd_fonts_list(sd_names, SETTINGS_FONT_MAX - 1);
    for (int i = 0; i < n && s_font_count < SETTINGS_FONT_MAX; i++) {
        strlcpy(s_font_names[s_font_count], sd_names[i], sizeof(s_font_names[0]));
        s_font_count++;
    }
    heap_caps_free(sd_names);
}

/** 字体行点击：投递选择操作并关闭模态（NVS 写入由设置应用任务执行，重启后生效）。 */
static void settings_font_item_cb(lv_event_t *e) {
    lv_event_stop_bubbling(e);
    const char *name = lv_event_get_user_data(e);
    if (name == NULL) {
        return;
    }
    settings_op_t op = {0};
    op.type = SETTINGS_OP_FONT;
    op.row = settings_row_find(SETTINGS_ROW_FONT);
    strlcpy(op.font_name, name, sizeof(op.font_name));
    settings_op_post(&op);
    settings_modal_close();
}

/** 打开字体选择模态。 */
static void settings_font_modal_open(void) {
    settings_font_collect();

    int32_t scr_w, scr_h;
    espaperplay_ui_screen_size(&scr_w, &scr_h);

    /* 全屏覆盖层（点空白关闭）+ 居中卡片（宽高按屏高缩放，宽不超过屏幕）。 */
    s_modal = espaperplay_ui_modal_overlay_create(true, settings_modal_overlay_cb);
    const int card_w = espaperplay_ui_scaled(400) > scr_w - 2 * SETTINGS_MARGIN
                           ? scr_w - 2 * SETTINGS_MARGIN
                           : espaperplay_ui_scaled(400);
    const int card_h = espaperplay_ui_scaled(480);
    lv_obj_t *card = espaperplay_ui_modal_card_create(s_modal, card_w, card_h);

    /* 标题 */
    lv_obj_t *title = espaperplay_ui_label_create(card, "选择字体", 20, LV_TEXT_ALIGN_CENTER);
    lv_obj_set_width(title, LV_PCT(100));
    lv_obj_set_pos(title, 0, espaperplay_ui_scaled(12));

    /* 提示（重启生效） */
    lv_obj_t *hint = espaperplay_ui_label_create(card, "选择后重启设备生效", 16, LV_TEXT_ALIGN_CENTER);
    lv_obj_set_width(hint, LV_PCT(100));
    lv_obj_set_pos(hint, 0, espaperplay_ui_scaled(44));

    /* 字体列表（可滚动） */
    lv_obj_t *list = lv_obj_create(card);
    lv_obj_set_size(list, card_w - 24, card_h - espaperplay_ui_scaled(100));
    lv_obj_set_pos(list, 12, espaperplay_ui_scaled(76));
    lv_obj_set_style_bg_color(list, lv_color_white(), 0);
    lv_obj_set_style_border_width(list, 0, 0);
    lv_obj_set_style_radius(list, 0, 0);
    lv_obj_set_style_pad_all(list, 0, 0);
    lv_obj_set_scroll_dir(list, LV_DIR_VER);
    lv_obj_add_flag(list, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(list, LV_SCROLLBAR_MODE_OFF);
    lv_obj_remove_flag(list, LV_OBJ_FLAG_SCROLL_ELASTIC);

    const char *current = espaperplay_system_get_config()->selected_font;
    if (current[0] == '\0') {
        current = ESPAPERPLAY_FONTS_DEFAULT_NAME;
    }

    for (int i = 0; i < s_font_count; i++) {
        const bool is_builtin = (strcmp(s_font_names[i], ESPAPERPLAY_FONTS_DEFAULT_NAME) == 0);
        const bool is_current = (strcmp(s_font_names[i], current) == 0);

        const int item_h = espaperplay_ui_scaled(44) < 32 ? 32 : espaperplay_ui_scaled(44);
        lv_obj_t *item = lv_obj_create(list);
        lv_obj_set_size(item, card_w - 24, item_h);
        lv_obj_set_pos(item, 0, i * (item_h + 2));
        lv_obj_set_style_bg_color(item, is_current ? lv_color_black() : lv_color_white(), 0);
        lv_obj_set_style_border_width(item, 0, 0);
        lv_obj_set_style_radius(item, 6, 0);
        lv_obj_set_style_pad_all(item, 0, 0);
        lv_obj_remove_flag(item, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(item, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(item, settings_font_item_cb, LV_EVENT_CLICKED, (void *)s_font_names[i]);

        lv_obj_t *label = lv_label_create(item);
        lv_label_set_text(label, s_font_names[i]);
        lv_obj_set_style_text_color(label, is_current ? lv_color_white() : lv_color_black(), 0);
        lv_obj_set_style_text_font(label, espaperplay_ui_font(16), 0);
        lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_LEFT, 0);
        lv_obj_set_width(label, card_w - 24 - 70);
        lv_obj_align(label, LV_ALIGN_LEFT_MID, 10, 0);
        lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);

        if (is_builtin) {
            lv_obj_t *tag = lv_label_create(item);
            lv_label_set_text(tag, "内置");
            lv_obj_set_style_text_color(tag, is_current ? lv_color_white() : lv_color_black(), 0);
            lv_obj_set_style_text_font(tag, espaperplay_ui_font(16), 0);
            lv_obj_align(tag, LV_ALIGN_RIGHT_MID, -10, 0);
        }
    }

    ESP_LOGI(TAG, "settings: font modal open (%d fonts)", s_font_count);
}

/* ------------------------------------------------------------------ */
/* 行触发 / 分页切换                                                     */
/* ------------------------------------------------------------------ */

/** 触发一个设置行操作（LVGL 线程内，由 on_touch 点击判定调用）。 */
static void settings_row_trigger(settings_row_t *row) {
    switch (row->type) {
    case SETTINGS_ROW_STEPPER:
        settings_stepper_modal_open(row);
        break;
    case SETTINGS_ROW_CYCLE: {
        int idx = row->get_value();
        idx = (idx + 1) % row->option_count;
        if (row->apply_wifi) {
            /* WiFi 模式切换影响网络连接（IP 可能变化）：先弹二级确认。 */
            char msg[96];
            snprintf(msg, sizeof(msg), "将切换到「%s」模式，网络会重新连接，IP 可能变化。",
                     row->options[idx]);
            settings_confirm_modal_open(row, idx, msg);
        } else {
            settings_op_t op = {0};
            op.type = SETTINGS_OP_CYCLE;
            op.row = row;
            op.value = idx;
            settings_op_post(&op);
        }
        break;
    }
    case SETTINGS_ROW_FONT:
        settings_font_modal_open();
        break;
    case SETTINGS_ROW_ACTION:
        if (row->on_action != NULL) {
            row->on_action();
        }
        break;
    default:
        break; /* WEB / INFO：仅展示，无操作 */
    }
}

static bool settings_hit(lv_obj_t *obj, const lv_point_t *p) {
    if (!obj) return false;
    lv_area_t area; lv_obj_get_coords(obj, &area);
    return espaperplay_ui_point_in(p, area.x1, area.y1, lv_area_get_width(&area), lv_area_get_height(&area));
}

static settings_row_t *settings_hit_row(const lv_point_t *p) {
    for (int i = 0; i < SETTINGS_ROW_CNT; i++)
        if (settings_row_tappable(&s_rows[i]) && settings_hit(s_rows[i].row_obj, p)) return &s_rows[i];
    return NULL;
}

/** 周期刷新（LVGL 线程内，lv_timer 驱动）：Web 端改配置后同步显示。 */
static void settings_timer_cb(lv_timer_t *timer) {
    (void)timer;
    settings_refresh_all();
}

/* ------------------------------------------------------------------ */
/* 页面构建 / 交互                                                      */
/* ------------------------------------------------------------------ */

/** 设置页构建（页面 enter：屏幕已由页面栈清空）。 */
static void settings_enter(void) {
    lv_obj_t *scr = lv_screen_active();
    lv_obj_set_style_bg_color(scr, lv_color_white(), 0);
    /* 防止整个屏幕被 LVGL 滚动（分页切换由手势接管） */
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    int32_t scr_w, scr_h;
    espaperplay_ui_screen_size(&scr_w, &scr_h);

    /* 按屏高计算缩放因子（基准 800；横屏 480 高 -> 0.6，小屏 600 -> 0.75）。 */
    espaperplay_ui_scale_init(SETTINGS_REF_H);
    s_bar = espaperplay_ui_status_bar_create(scr, ESPAPERPLAY_UI_HEADER_H, "", false);
    if (!s_resume_category) s_category = -1;
    s_resume_category = false;
    s_content = NULL;
    settings_content_build();

    s_modal = NULL;
    s_modal_row = NULL;
    s_modal_value_label = NULL;
    s_touch_down = false;
    s_touch_row = NULL;

    settings_refresh_all();

    s_timer = lv_timer_create(settings_timer_cb, SETTINGS_UI_PERIOD_MS, NULL);
    if (s_timer == NULL) {
        /* 定时器创建失败：只有进入时的一次刷新，无法周期更新（罕见，仅记录）。 */
        ESP_LOGE(TAG, "settings: periodic refresh timer create failed");
    }

    ESP_LOGI(TAG, "settings screen entered");
}

/** 设置页退出（页面 exit：删除定时器并关闭模态，避免离开页面后仍刷新屏幕）。 */
static void settings_exit(void) {
    if (s_timer != NULL) {
        lv_timer_delete(s_timer);
        s_timer = NULL;
    }
    settings_modal_close();
    s_content = NULL;
    /* 页面对象已删除：清空行对象/值标签指针，防止设置应用任务回刷时访问已释放对象。 */
    for (int i = 0; i < SETTINGS_ROW_CNT; i++) {
        s_rows[i].row_obj = NULL;
        s_rows[i].value_label = NULL;
    }
    ESP_LOGI(TAG, "settings screen exited");
}

/** 设置页触摸处理：模态打开时忽略手势（模态按钮自行处理）；
 * 否则边缘向内滑动返回主页、中间左右滑动切换分页、点击行触发操作
 * （滑动优先于点击，互不冲突）。 */
static void settings_on_touch(const espaperplay_input_event_t *event) {
    if (s_modal != NULL) {
        return; /* 模态打开：点击由覆盖层 / 按钮处理 */
    }

    lv_point_t p;
    espaperplay_ui_touch_map_to_lv(event->point.x, event->point.y, &p);

    if (event->touch_pressed) {
        if (!s_touch_down) {
            s_touch_down = true;
            s_touch_start = p;
            s_touch_row = settings_hit_row(&p);
        }
        s_touch_last = p;
    } else if (s_touch_down) {
        s_touch_down = false;
        const int dx = s_touch_last.x - s_touch_start.x;
        const int dy = s_touch_last.y - s_touch_start.y;
        const int adx = abs(dx);
        const int ady = abs(dy);

        /* 边缘向内滑动返回（横向为主，避免与分页切换冲突）。 */
        if (adx > UI_GESTURE_EDGE_SWIPE_PX && adx > ady * UI_GESTURE_SWIPE_MIN_RATIO) {
            int32_t scr_w = lv_display_get_horizontal_resolution(lv_display_get_default());
            if ((s_touch_start.x < UI_GESTURE_EDGE_PX && dx > 0) ||
                (s_touch_start.x > scr_w - UI_GESTURE_EDGE_PX && dx < 0)) {
                settings_back();
                s_touch_row = NULL;
                return;
            }
        }

        if (adx <= UI_GESTURE_CLICK_MAX_PX && ady <= UI_GESTURE_CLICK_MAX_PX) {
            if (s_touch_start.y < ESPAPERPLAY_UI_HEADER_H + 72 && s_touch_start.x < 72) {
                settings_back();
            } else if (s_category < 0) {
                for (int i = 0; i < SETTINGS_CATEGORY_CNT; i++) {
                    if (settings_hit(s_category_rows[i], &s_touch_start)) {
                        s_category = i;
                        settings_content_build();
                        break;
                    }
                }
            } else if (s_touch_row) {
                s_resume_category = s_touch_row->type == SETTINGS_ROW_ACTION;
                settings_row_trigger(s_touch_row);
            }
        }
        s_touch_row = NULL;
    }
}

/** 设置页按键处理（LVGL 线程内）：模态打开时单击先关闭模态，否则返回上一页。 */
static void settings_on_key(const espaperplay_input_event_t *event) {
    if (event->key_action != ESPAPERPLAY_INPUT_KEY_ACTION_SINGLE_CLICK) {
        return;
    }
    if (s_modal != NULL) {
        settings_modal_close();
        return;
    }
    settings_back();
}

/** 设置页页面实例（页面栈用）。 */
const espaperplay_ui_page_t espaperplay_ui_page_settings = {settings_enter, settings_exit,
                                                            settings_on_key, settings_on_touch};
