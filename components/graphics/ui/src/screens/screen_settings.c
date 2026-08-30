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

static const char *TAG = "ESPaperPlay_UI";

/* ====================================================================
 * 设置页（系统设置管理，UI 风格与主界面 / 天气页统一）
 * ====================================================================
 *
 * 墨水屏不适合滚动：设置项按内容量分页（每页可含多个大类），大类名称
 * 以横线分隔符形式呈现（居中文字 + 两侧横线），子类之间用黑底白字条
 * 明显分界（墨水屏仅黑白双色，阈值判定下浅灰=白不可见）；左右滑动切换
 * 分页（与主界面 / 天气页一致），点击整行触发操作（滑动优先于点击，
 * 互不冲突）。
 *
 * 布局：顶部标题栏（"设置"居中）+ 分页卡片 + 底部页面指示点：
 *   页 1：设备（显示 / 按键）+ 系统（网络 / 字体 / 开发者）；
 *   页 2：服务（天气）。
 *
 * 交互：
 *   - 数值步进行：点击打开编辑模态（大号数值 + [-] [+] 步进 + 完成），
 *     修改立即持久化到 NVS 并同步应用到运行中的服务（EPD 睡眠超时 /
 *     GUI 全刷阈值 / 按键长按时间），无需重启；
 *   - 循环行：点击切到下一选项并立即持久化（BOOT 长按动作 UI 分发实时
 *     读取，立即生效）；WiFi 模式切换先弹二级确认（网络会重新连接，
 *     IP 可能变化），确认后自动重新应用网络；
 *   - Web 提示行：仅展示当前状态，提示到 Web 管理页配置（设备无键盘，
 *     SSID / 密码 / API Key 等长文本不适合在墨水屏上输入）；
 *   - 字体行：点击打开字体列表（出厂内置 + SD 卡），选择后需重启生效；
 *   - 软件版本行：仅展示当前固件版本；
 *   - 屏幕左/右边缘向内滑动返回主页（安卓边缘手势）；中间左右滑动切换
 *     分页；点击行触发操作（滑动优先，不误触）；
 *   - 物理按键单击返回；模态打开时单击先关闭模态。
 *
 * 行点击在页面 on_touch 中判定（与主界面一致）：按下时记录起点并做整行
 * 命中检测，释放时横向位移达阈值 -> 切页，小位移且起点在行内 -> 触发
 * 该行操作。不依赖 LVGL CLICKED 事件（滚动/滑动会误触），因此整行都可
 * 点击，且滑动切页优先于点击。
 *
 * 风格统一（主界面设计基准）：FreeType 中文（16 / 20 / 24 档）、白底
 * 黑字、圆角卡片（与屏幕边缘保持间距）。
 */

#define SETTINGS_EDGE_PX 24           /* 边缘滑动触发宽度（物理手势，不缩放） */
#define SETTINGS_EDGE_SWIPE_PX 70     /* 边缘向内滑动位移阈值（不缩放） */
#define SETTINGS_SWIPE_PX 90          /* 分页切换位移阈值（不缩放） */
#define SETTINGS_CLICK_MAX_PX 15      /* 点击允许的最大位移（防抖，不缩放） */
#define SETTINGS_SWIPE_MIN_RATIO 1.2f /* 横向位移 / 纵向位移 最小比例 */
#define SETTINGS_MARGIN 16            /* 卡片与屏幕边缘间距 */
#define SETTINGS_UI_PERIOD_MS 5000    /* 周期刷新（Web 端改配置后同步显示） */

/* ---- 尺寸缩放（方案 1：行高/间距按屏高缩放，支持横屏与小屏） ----
 * 基准逻辑高度 800px（本项目竖屏 480x800）。所有垂直尺寸按
 * scale = scr_h / 800 缩放并设最小下限（保证 16px 字号可读）；
 * 宽度方向本就按 scr_w 自适应。字号保持固定档位（16/20/24/32），
 * 避免 FreeType 缓存（6 项）被缩放字号挤爆。 */
#define SETTINGS_REF_H 800           /* 基准逻辑高度（缩放参考） */
#define SETTINGS_BAR_H 30            /* 标题栏高度（基准） */
#define SETTINGS_SECTION_TITLE_H 32  /* 大类标题区高度（基准） */
#define SETTINGS_SUBGROUP_TITLE_H 28 /* 子类标题区高度（基准） */
#define SETTINGS_HINT_H 30           /* 页底提示区高度（基准） */
#define SETTINGS_ROW_H 52            /* 设置行高度（基准） */
#define SETTINGS_MIN_H 24            /* 标题/提示区最小高度（16px 字行高约 19px） */
#define SETTINGS_MIN_ROW_H 40        /* 行最小高度 */

static float s_scale = 1.0f; /*!< 屏高缩放因子（settings_enter 时按 scr_h 计算） */

/** 基准值按屏高缩放（取整）。 */
static int settings_scaled(int v) { return (int)(v * s_scale); }

/** 标题栏高度（缩放 + 下限）。 */
static int settings_bar_h(void) {
    int h = settings_scaled(SETTINGS_BAR_H);
    return h < SETTINGS_MIN_H ? SETTINGS_MIN_H : h;
}

/** 大类标题区高度（缩放 + 下限）。 */
static int settings_section_title_h(void) {
    int h = settings_scaled(SETTINGS_SECTION_TITLE_H);
    return h < SETTINGS_MIN_H ? SETTINGS_MIN_H : h;
}

/** 子类标题区高度（缩放 + 下限）。 */
static int settings_subgroup_title_h(void) {
    int h = settings_scaled(SETTINGS_SUBGROUP_TITLE_H);
    return h < SETTINGS_MIN_H ? SETTINGS_MIN_H : h;
}

/** 页底提示区高度（缩放 + 下限）。 */
static int settings_hint_h(void) {
    int h = settings_scaled(SETTINGS_HINT_H);
    return h < SETTINGS_MIN_H ? SETTINGS_MIN_H : h;
}

/** 设置行高度（缩放 + 下限）。 */
static int settings_row_h(void) {
    int h = settings_scaled(SETTINGS_ROW_H);
    return h < SETTINGS_MIN_ROW_H ? SETTINGS_MIN_ROW_H : h;
}

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

static void settings_open_test(void) {
    ESP_LOGI(TAG, "settings: open test page");
    espaperplay_ui_page_push_lv(&espaperplay_ui_page_test);
}

static void settings_open_setup(void) {
    ESP_LOGI(TAG, "settings: open setup wizard");
    espaperplay_ui_page_push_lv(&espaperplay_ui_page_setup);
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
    {.name = "屏幕空闲自动睡眠",
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
    /* 系统：网络 */
    {.name = "WiFi 模式",
     .type = SETTINGS_ROW_CYCLE,
     .apply_wifi = true,
     .get_value = settings_get_wifi_mode,
     .set_value = NULL,
     .fmt_value = settings_fmt_wifi_mode,
     .options = s_wifi_mode_options,
     .option_count = (int)(sizeof(s_wifi_mode_options) / sizeof(s_wifi_mode_options[0]))},
    {.name = "WiFi 凭据", .type = SETTINGS_ROW_WEB, .fmt_value = settings_fmt_wifi_cred},
    {.name = "重新运行开机引导", .type = SETTINGS_ROW_ACTION, .on_action = settings_open_setup},
    /* 系统：字体 / 版本 / 开发者 */
    {.name = "当前字体", .type = SETTINGS_ROW_FONT, .fmt_value = settings_fmt_font},
    {.name = "软件版本", .type = SETTINGS_ROW_INFO, .fmt_value = settings_fmt_version},
    {.name = "测试页", .type = SETTINGS_ROW_ACTION, .on_action = settings_open_test},
    /* 服务：天气 */
    {.name = "天气 API Key", .type = SETTINGS_ROW_WEB, .fmt_value = settings_fmt_weather_key},
    {.name = "天气位置", .type = SETTINGS_ROW_WEB, .fmt_value = settings_fmt_weather_loc},
    {.name = "天气 API Host", .type = SETTINGS_ROW_WEB, .fmt_value = settings_fmt_weather_host},
};

/* ------------------------------------------------------------------ */
/* 大类定义（动态分页：按可用高度自动决定每页放几个大类）                   */
/* ------------------------------------------------------------------ */

typedef struct {
    const char *title; /*!< 子类标题（细分类，如 显示 / 按键） */
    int row_start;     /*!< 组内首行在 s_rows 中的下标 */
    int row_count;     /*!< 组内行数 */
} settings_subgroup_t;

typedef struct {
    const char *title;                    /*!< 大类名称（横线分隔符呈现） */
    const settings_subgroup_t *subgroups; /*!< 子类列表 */
    int subgroup_count;                   /*!< 子类数 */
    const char *hint;                     /*!< 页底提示（Web 配置项提示，可为 NULL） */
} settings_section_t;

static const settings_subgroup_t s_dev_subgroups[] = {
    {"显示", 0, 2},
    {"按键", 2, 2},
};

static const settings_subgroup_t s_sys_subgroups[] = {
    {"网络", 4, 3},
    {"字体", 6, 1},
    {"开发者", 7, 2},
};

static const settings_subgroup_t s_svc_subgroups[] = {
    {"天气", 9, 3},
};

/* 全部大类（顺序即显示顺序）。 */
static const settings_section_t s_sections[] = {
    {"设备", s_dev_subgroups, 2, NULL},
    {"系统", s_sys_subgroups, 3, NULL},
    {"服务", s_svc_subgroups, 1, "请在 Web 管理页配置"},
};

#define SETTINGS_ROW_CNT (int)(sizeof(s_rows) / sizeof(s_rows[0]))
#define SETTINGS_SECTION_CNT (int)(sizeof(s_sections) / sizeof(s_sections[0]))
#define SETTINGS_PAGE_MAX 6 /* 分页上限（动态分组，最多 6 页） */

/* 运行时分页分组：每页包含若干大类（settings_enter 按可用高度构建）。 */
static const settings_section_t *s_page_sections[SETTINGS_PAGE_MAX][SETTINGS_SECTION_CNT];
static int s_page_section_cnt[SETTINGS_PAGE_MAX];
static int s_page_count = 0;                 /*!< 实际分页数 */
static bool s_page_built[SETTINGS_PAGE_MAX]; /*!< 分页控件是否已构建（隐藏页惰性构建） */
static int s_area_y = 0;                     /*!< 分页容器顶部 y（enter 时计算，惰性构建用） */
static int s_area_h = 0;                     /*!< 分页容器高度 */

/* ------------------------------------------------------------------ */
/* 页面状态                                                             */
/* ------------------------------------------------------------------ */

static lv_obj_t *s_page_objs[SETTINGS_PAGE_MAX];  /*!< 分页容器 */
static lv_obj_t *s_dots[SETTINGS_PAGE_MAX];       /*!< 页面指示点 */
static int s_page = 0;                            /*!< 当前分页索引 */
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

/** FreeType 字体按需加载（与主界面共用缓存字号档）。 */
static lv_font_t *settings_font(int size_px) {
    return espaperplay_fonts_load(SETTINGS_FONT_NAME, (uint32_t)size_px,
                                  ESPAPERPLAY_FONT_STYLE_NORMAL);
}

/** 通用标签：白底黑字 + FreeType 字体 + 禁用 LVGL 滚动（防误滑页面）。 */
static lv_obj_t *settings_label_create(lv_obj_t *parent, const char *text, int font_px,
                                       lv_text_align_t align) {
    lv_obj_t *label = lv_label_create(parent);
    lv_label_set_text(label, text);
    lv_obj_set_style_text_color(label, lv_color_black(), 0);
    lv_font_t *font = settings_font(font_px);
    if (font != NULL) {
        lv_obj_set_style_text_font(label, font, 0);
    }
    lv_obj_set_style_text_align(label, align, 0);
    lv_obj_set_width(label, LV_PCT(100));
    lv_obj_remove_flag(label, LV_OBJ_FLAG_SCROLLABLE);
    return label;
}

/** 逻辑分辨率（旋转后）。 */
static void settings_screen_size(int32_t *out_w, int32_t *out_h) {
    lv_display_t *disp = lv_display_get_default();
    *out_w = lv_display_get_horizontal_resolution(disp);
    *out_h = lv_display_get_vertical_resolution(disp);
}

/** 点在矩形内（逻辑坐标）。 */
static bool settings_point_in(const lv_point_t *p, int x, int y, int w, int h) {
    return p->x >= x && p->x < x + w && p->y >= y && p->y < y + h;
}

/** 对象相对屏幕的坐标（累加父级偏移；LVGL 的 get_x/y 只返回相对父）。 */
static int settings_obj_screen_x(const lv_obj_t *obj) {
    int x = 0;
    const lv_obj_t *p = obj;
    while (p != NULL && lv_obj_get_parent(p) != NULL) {
        x += lv_obj_get_x(p);
        p = lv_obj_get_parent(p);
    }
    return x;
}

static int settings_obj_screen_y(const lv_obj_t *obj) {
    int y = 0;
    const lv_obj_t *p = obj;
    while (p != NULL && lv_obj_get_parent(p) != NULL) {
        y += lv_obj_get_y(p);
        p = lv_obj_get_parent(p);
    }
    return y;
}

/** 刷新单行值标签（仅文本变化时更新，EPD 上避免无谓刷新）。 */
static void settings_row_refresh(settings_row_t *row) {
    if (row->value_label == NULL || row->fmt_value == NULL) {
        return;
    }
    char buf[64];
    row->fmt_value(row->get_value ? row->get_value() : 0, buf, sizeof(buf));
    if (strcmp(lv_label_get_text(row->value_label), buf) != 0) {
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

/** 构建一个设置行：名称（左）+ 值（右）。整行命中检测由页面 on_touch 完成。 */
static void settings_row_create(lv_obj_t *parent, settings_row_t *row, int x, int y, int w) {
    lv_obj_t *row_obj = lv_obj_create(parent);
    lv_obj_set_size(row_obj, w, settings_row_h() - 4);
    lv_obj_set_pos(row_obj, x, y);
    lv_obj_set_style_bg_color(row_obj, lv_color_white(), 0);
    lv_obj_set_style_border_width(row_obj, 0, 0);
    lv_obj_set_style_radius(row_obj, 0, 0);
    lv_obj_set_style_pad_all(row_obj, 0, 0);
    lv_obj_remove_flag(row_obj, LV_OBJ_FLAG_SCROLLABLE);
    row->row_obj = row_obj;

    /* 名称（左，垂直居中与右侧值对齐） */
    lv_obj_t *name = settings_label_create(row_obj, row->name, 16, LV_TEXT_ALIGN_LEFT);
    lv_obj_set_width(name, w - 170);
    lv_obj_align(name, LV_ALIGN_LEFT_MID, 4, 0);

    /* 值（右，垂直居中，超长省略号截断） */
    lv_obj_t *value = settings_label_create(row_obj, "", 16, LV_TEXT_ALIGN_RIGHT);
    lv_obj_set_width(value, 160);
    lv_obj_align(value, LV_ALIGN_RIGHT_MID, -4, 0);
    lv_label_set_long_mode(value, LV_LABEL_LONG_DOT);
    row->value_label = value;
}

/** 大类标题：居中文字 + 两侧横线（横线分隔符形式，flex 布局）。 */
static void settings_section_title_create(lv_obj_t *parent, const char *text, int x, int y, int w) {
    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_set_size(row, w, settings_section_title_h());
    lv_obj_set_pos(row, x, y);
    lv_obj_set_style_bg_color(row, lv_color_white(), 0);
    lv_obj_set_style_border_width(row, 0, 0);
    lv_obj_set_style_radius(row, 0, 0);
    lv_obj_set_style_pad_all(row, 0, 0);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    /* 左横线（flex 伸展占满剩余空间） */
    lv_obj_t *l = lv_obj_create(row);
    lv_obj_set_height(l, 1);
    lv_obj_set_style_bg_color(l, lv_color_black(), 0);
    lv_obj_set_style_border_width(l, 0, 0);
    lv_obj_set_style_radius(l, 0, 0);
    lv_obj_set_flex_grow(l, 1);

    /* 大类名称 */
    lv_obj_t *label = lv_label_create(row);
    lv_label_set_text(label, text);
    lv_obj_set_style_text_color(label, lv_color_black(), 0);
    lv_obj_set_style_text_font(label, settings_font(16), 0);
    lv_obj_set_style_pad_left(label, 10, 0);
    lv_obj_set_style_pad_right(label, 10, 0);

    /* 右横线（flex 伸展占满剩余空间） */
    lv_obj_t *r = lv_obj_create(row);
    lv_obj_set_height(r, 1);
    lv_obj_set_style_bg_color(r, lv_color_black(), 0);
    lv_obj_set_style_border_width(r, 0, 0);
    lv_obj_set_style_radius(r, 0, 0);
    lv_obj_set_flex_grow(r, 1);
}

/** 子类标题：黑底白字条（与相邻子类明显分界）。
 * 墨水屏仅黑白双色：BW 阈值模式（L >= 128 判白）下浅灰会显示为白色，
 * 与背景无法区分，故用黑底白字保证两种刷新模式下都清晰可见。 */
static void settings_subgroup_title_create(lv_obj_t *parent, const char *text, int x, int y,
                                           int w) {
    lv_obj_t *bar = lv_obj_create(parent);
    lv_obj_set_size(bar, w, settings_subgroup_title_h());
    lv_obj_set_pos(bar, x, y);
    lv_obj_set_style_bg_color(bar, lv_color_black(), 0);
    lv_obj_set_style_border_width(bar, 0, 0);
    lv_obj_set_style_radius(bar, 0, 0);
    lv_obj_set_style_pad_all(bar, 0, 0);
    lv_obj_remove_flag(bar, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *label = lv_label_create(bar);
    lv_label_set_text(label, text);
    lv_obj_set_style_text_color(label, lv_color_white(), 0);
    lv_obj_set_style_text_font(label, settings_font(16), 0);
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_LEFT, 0);
    lv_obj_set_width(label, LV_PCT(100));
    lv_obj_set_pos(label, 8, 2);
    lv_obj_remove_flag(label, LV_OBJ_FLAG_SCROLLABLE);
}

/** 单个大类的总高度（标题 + 子类标题 + 行 + 页底提示）。 */
static int settings_section_h(const settings_section_t *sec) {
    int h = settings_section_title_h();
    for (int g = 0; g < sec->subgroup_count; g++) {
        h += settings_subgroup_title_h();
        h += sec->subgroups[g].row_count * settings_row_h();
    }
    if (sec->hint != NULL) {
        h += settings_hint_h();
    }
    return h;
}

/** 构建一个分页卡片：多个大类（横线分隔符）+ 子类（黑底白字条）+ 行 + 页底提示。 */
static void settings_page_create(lv_obj_t *parent, const settings_section_t *const *sections,
                                 int section_count, int y) {
    int32_t scr_w = lv_display_get_horizontal_resolution(lv_display_get_default());
    const int card_w = scr_w - 2 * SETTINGS_MARGIN;
    /* 卡片高 = 各大类（标题 + 子类标题 + 行 + 提示）之和（全部按屏高缩放） */
    int card_h = 0;
    for (int s = 0; s < section_count; s++) {
        card_h += settings_section_h(sections[s]);
    }

    lv_obj_t *card = lv_obj_create(parent);
    lv_obj_set_size(card, card_w, card_h);
    lv_obj_set_pos(card, SETTINGS_MARGIN, y);
    lv_obj_set_style_bg_color(card, lv_color_white(), 0);
    lv_obj_set_style_border_color(card, lv_color_black(), 0);
    lv_obj_set_style_border_width(card, 2, 0);
    lv_obj_set_style_radius(card, 12, 0);
    lv_obj_set_style_pad_all(card, 0, 0);
    lv_obj_remove_flag(card, LV_OBJ_FLAG_SCROLLABLE);

    /* 大类（横线分隔符）-> 子类（黑底白字条）-> 行（行间 1px 分隔线） */
    int ry = 0;
    for (int s = 0; s < section_count; s++) {
        const settings_section_t *sec = sections[s];
        settings_section_title_create(card, sec->title, 12, ry, card_w - 24);
        ry += settings_section_title_h();
        for (int g = 0; g < sec->subgroup_count; g++) {
            const settings_subgroup_t *sg = &sec->subgroups[g];
            settings_subgroup_title_create(card, sg->title, 12, ry, card_w - 24);
            ry += settings_subgroup_title_h();
            for (int i = 0; i < sg->row_count; i++) {
                settings_row_t *row = &s_rows[sg->row_start + i];
                settings_row_create(card, row, 12, ry, card_w - 24);
                if (i < sg->row_count - 1) {
                    lv_obj_t *sep = lv_obj_create(card);
                    lv_obj_set_size(sep, card_w - 24, 1);
                    lv_obj_set_pos(sep, 12, ry + settings_row_h() - 3);
                    lv_obj_set_style_bg_color(sep, lv_color_black(), 0);
                    lv_obj_set_style_border_width(sep, 0, 0);
                    lv_obj_set_style_radius(sep, 0, 0);
                    lv_obj_remove_flag(sep, LV_OBJ_FLAG_SCROLLABLE);
                }
                ry += settings_row_h();
            }
        }
        /* 页底提示（Web 配置项提示） */
        if (sec->hint != NULL) {
            lv_obj_t *hint = settings_label_create(card, sec->hint, 16, LV_TEXT_ALIGN_CENTER);
            lv_obj_set_width(hint, LV_PCT(100));
            lv_obj_set_pos(hint, 0, ry + 6);
            ry += settings_hint_h();
        }
    }
}

/** 按可用高度把全部大类动态分组到各页（空间够则同页多类，不够则拆页）。 */
static void settings_build_pages(int avail_h) {
    s_page_count = 0;
    int cur_h = 0;
    for (int s = 0; s < SETTINGS_SECTION_CNT; s++) {
        const int sec_h = settings_section_h(&s_sections[s]);
        /* 当前页已有内容且放不下时开新页；单个大类超高时也独占一页。 */
        if (cur_h > 0 && cur_h + sec_h > avail_h) {
            s_page_count++;
            cur_h = 0;
        }
        if (s_page_count >= SETTINGS_PAGE_MAX) {
            break; /* 分页上限（防御，正常不会触发） */
        }
        s_page_sections[s_page_count][s_page_section_cnt[s_page_count]++] = &s_sections[s];
        cur_h += sec_h;
    }
    s_page_count++;
    ESP_LOGI(TAG, "settings: %d sections -> %d pages (avail %d px)", SETTINGS_SECTION_CNT,
             s_page_count, avail_h);
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
    settings_screen_size(&scr_w, &scr_h);

    /* 全屏覆盖层：拦截触摸（点击空白关闭）。浅灰底在 BW 阈值模式下显示为
     * 白色（L>=128 判白），模态靠卡片黑边框区分；GRAY4 模式下显示浅灰。 */
    s_modal = lv_obj_create(lv_screen_active());
    lv_obj_set_size(s_modal, scr_w, scr_h);
    lv_obj_set_pos(s_modal, 0, 0);
    lv_obj_set_style_bg_color(s_modal, lv_color_hex(0xE8E8E8), 0);
    lv_obj_set_style_border_width(s_modal, 0, 0);
    lv_obj_set_style_radius(s_modal, 0, 0);
    lv_obj_set_style_pad_all(s_modal, 0, 0);
    lv_obj_remove_flag(s_modal, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_modal, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_modal, settings_modal_overlay_cb, LV_EVENT_CLICKED, NULL);

    /* 卡片（尺寸按屏高缩放） */
    lv_obj_t *card = lv_obj_create(s_modal);
    lv_obj_set_size(card, settings_scaled(340), settings_scaled(250));
    lv_obj_center(card);
    lv_obj_set_style_bg_color(card, lv_color_white(), 0);
    lv_obj_set_style_border_color(card, lv_color_black(), 0);
    lv_obj_set_style_border_width(card, 2, 0);
    lv_obj_set_style_radius(card, 12, 0);
    lv_obj_set_style_pad_all(card, 0, 0);
    lv_obj_remove_flag(card, LV_OBJ_FLAG_SCROLLABLE);

    /* 标题 */
    lv_obj_t *title = settings_label_create(card, row->name, 20, LV_TEXT_ALIGN_CENTER);
    lv_obj_set_width(title, LV_PCT(100));
    lv_obj_set_pos(title, 0, settings_scaled(14));

    /* 消息（可换行居中） */
    lv_obj_t *msg = settings_label_create(card, message, 16, LV_TEXT_ALIGN_CENTER);
    lv_obj_set_width(msg, settings_scaled(300));
    lv_obj_set_pos(msg, settings_scaled(20), settings_scaled(60));
    lv_label_set_long_mode(msg, LV_LABEL_LONG_WRAP);

    /* 取消（白底黑边） */
    lv_obj_t *cancel = lv_button_create(card);
    lv_obj_set_size(cancel, settings_scaled(120), settings_scaled(44));
    lv_obj_set_pos(cancel, settings_scaled(30), settings_scaled(180));
    lv_obj_set_style_bg_color(cancel, lv_color_white(), 0);
    lv_obj_set_style_border_color(cancel, lv_color_black(), 0);
    lv_obj_set_style_border_width(cancel, 2, 0);
    lv_obj_set_style_radius(cancel, 8, 0);
    lv_obj_t *cancel_label = lv_label_create(cancel);
    lv_label_set_text(cancel_label, "取消");
    lv_obj_set_style_text_color(cancel_label, lv_color_black(), 0);
    lv_obj_set_style_text_font(cancel_label, settings_font(20), 0);
    lv_obj_center(cancel_label);
    lv_obj_add_event_cb(cancel, settings_confirm_cancel_cb, LV_EVENT_CLICKED, NULL);

    /* 确定（黑底白字） */
    lv_obj_t *ok = lv_button_create(card);
    lv_obj_set_size(ok, settings_scaled(120), settings_scaled(44));
    lv_obj_set_pos(ok, settings_scaled(190), settings_scaled(180));
    lv_obj_set_style_bg_color(ok, lv_color_black(), 0);
    lv_obj_set_style_border_width(ok, 0, 0);
    lv_obj_set_style_radius(ok, 8, 0);
    lv_obj_t *ok_label = lv_label_create(ok);
    lv_label_set_text(ok_label, "确定");
    lv_obj_set_style_text_color(ok_label, lv_color_white(), 0);
    lv_obj_set_style_text_font(ok_label, settings_font(20), 0);
    lv_obj_center(ok_label);
    lv_obj_add_event_cb(ok, settings_confirm_ok_cb, LV_EVENT_CLICKED, NULL);

    ESP_LOGI(TAG, "settings: confirm modal open (%s -> %d)", row->name, (int)new_value);
}

/** 打开数值步进编辑模态。 */
static void settings_stepper_modal_open(settings_row_t *row) {
    s_modal_row = row;
    s_modal_value = row->get_value();

    int32_t scr_w, scr_h;
    settings_screen_size(&scr_w, &scr_h);

    /* 全屏覆盖层：拦截触摸（点击空白关闭）。浅灰底在 BW 阈值模式下显示为
     * 白色（L>=128 判白），模态靠卡片黑边框区分；GRAY4 模式下显示浅灰。 */
    s_modal = lv_obj_create(lv_screen_active());
    lv_obj_set_size(s_modal, scr_w, scr_h);
    lv_obj_set_pos(s_modal, 0, 0);
    lv_obj_set_style_bg_color(s_modal, lv_color_hex(0xE8E8E8), 0);
    lv_obj_set_style_border_width(s_modal, 0, 0);
    lv_obj_set_style_radius(s_modal, 0, 0);
    lv_obj_set_style_pad_all(s_modal, 0, 0);
    lv_obj_remove_flag(s_modal, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_modal, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_modal, settings_modal_overlay_cb, LV_EVENT_CLICKED, NULL);

    /* 卡片（尺寸按屏高缩放） */
    lv_obj_t *card = lv_obj_create(s_modal);
    lv_obj_set_size(card, settings_scaled(340), settings_scaled(280));
    lv_obj_center(card);
    lv_obj_set_style_bg_color(card, lv_color_white(), 0);
    lv_obj_set_style_border_color(card, lv_color_black(), 0);
    lv_obj_set_style_border_width(card, 2, 0);
    lv_obj_set_style_radius(card, 12, 0);
    lv_obj_set_style_pad_all(card, 0, 0);
    lv_obj_remove_flag(card, LV_OBJ_FLAG_SCROLLABLE);

    /* 标题 */
    lv_obj_t *title = settings_label_create(card, row->name, 20, LV_TEXT_ALIGN_CENTER);
    lv_obj_set_width(title, LV_PCT(100));
    lv_obj_set_pos(title, 0, settings_scaled(14));

    /* 大号数值 */
    s_modal_value_label = settings_label_create(card, "", 32, LV_TEXT_ALIGN_CENTER);
    lv_obj_set_width(s_modal_value_label, LV_PCT(100));
    lv_obj_set_pos(s_modal_value_label, 0, settings_scaled(66));

    /* [-] [+] 步进按钮 */
    lv_obj_t *minus = lv_button_create(card);
    lv_obj_set_size(minus, settings_scaled(72), settings_scaled(52));
    lv_obj_set_pos(minus, settings_scaled(30), settings_scaled(150));
    lv_obj_set_style_bg_color(minus, lv_color_white(), 0);
    lv_obj_set_style_border_color(minus, lv_color_black(), 0);
    lv_obj_set_style_border_width(minus, 2, 0);
    lv_obj_set_style_radius(minus, 8, 0);
    lv_obj_t *minus_label = lv_label_create(minus);
    lv_label_set_text(minus_label, "-");
    lv_obj_set_style_text_color(minus_label, lv_color_black(), 0);
    lv_obj_set_style_text_font(minus_label, settings_font(24), 0);
    lv_obj_center(minus_label);
    lv_obj_add_event_cb(minus, settings_modal_minus_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *plus = lv_button_create(card);
    lv_obj_set_size(plus, settings_scaled(72), settings_scaled(52));
    lv_obj_set_pos(plus, settings_scaled(238), settings_scaled(150));
    lv_obj_set_style_bg_color(plus, lv_color_white(), 0);
    lv_obj_set_style_border_color(plus, lv_color_black(), 0);
    lv_obj_set_style_border_width(plus, 2, 0);
    lv_obj_set_style_radius(plus, 8, 0);
    lv_obj_t *plus_label = lv_label_create(plus);
    lv_label_set_text(plus_label, "+");
    lv_obj_set_style_text_color(plus_label, lv_color_black(), 0);
    lv_obj_set_style_text_font(plus_label, settings_font(24), 0);
    lv_obj_center(plus_label);
    lv_obj_add_event_cb(plus, settings_modal_plus_cb, LV_EVENT_CLICKED, NULL);

    /* 完成按钮 */
    lv_obj_t *done = lv_button_create(card);
    lv_obj_set_size(done, settings_scaled(140), settings_scaled(44));
    lv_obj_align(done, LV_ALIGN_BOTTOM_MID, 0, -settings_scaled(12));
    lv_obj_set_style_bg_color(done, lv_color_black(), 0);
    lv_obj_set_style_border_width(done, 0, 0);
    lv_obj_set_style_radius(done, 8, 0);
    lv_obj_t *done_label = lv_label_create(done);
    lv_label_set_text(done_label, "完成");
    lv_obj_set_style_text_color(done_label, lv_color_white(), 0);
    lv_obj_set_style_text_font(done_label, settings_font(20), 0);
    lv_obj_center(done_label);
    lv_obj_add_event_cb(done, settings_modal_done_cb, LV_EVENT_CLICKED, NULL);

    char buf[32];
    row->fmt_value(s_modal_value, buf, sizeof(buf));
    lv_label_set_text(s_modal_value_label, buf);

    ESP_LOGI(TAG, "settings: stepper modal open (%s)", row->name);
}

/* ------------------------------------------------------------------ */
/* 模态：字体选择                                                        */
/* ------------------------------------------------------------------ */

/** 校验字体文件名：以 .ttf / .otf / .ttc 结尾（大小写不敏感）。 */
static bool settings_font_ext_ok(const char *name) {
    size_t len = strlen(name);
    if (len < 5) {
        return false;
    }
    const char *ext = name + len - 4;
    return strcasecmp(ext, ".ttf") == 0 || strcasecmp(ext, ".otf") == 0 ||
           strcasecmp(ext, ".ttc") == 0;
}

/** 收集字体列表：出厂内置 + SD 卡（/sdcard/system/fonts/）。 */
static void settings_font_collect(void) {
    s_font_count = 0;
    strlcpy(s_font_names[s_font_count++], ESPAPERPLAY_FONTS_DEFAULT_NAME, sizeof(s_font_names[0]));

    if (espaperplay_storage_is_mounted()) {
        DIR *d = opendir(ESPAPERPLAY_FONTS_SD_DIR);
        if (d != NULL) {
            struct dirent *e = NULL;
            while ((e = readdir(d)) != NULL && s_font_count < SETTINGS_FONT_MAX) {
                if (settings_font_ext_ok(e->d_name)) {
                    strlcpy(s_font_names[s_font_count], e->d_name, sizeof(s_font_names[0]));
                    s_font_count++;
                }
            }
            closedir(d);
        }
    }
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
    settings_screen_size(&scr_w, &scr_h);

    /* 全屏覆盖层：拦截触摸（点击空白关闭）。浅灰底在 BW 阈值模式下显示为
     * 白色（L>=128 判白），模态靠卡片黑边框区分；GRAY4 模式下显示浅灰。 */
    s_modal = lv_obj_create(lv_screen_active());
    lv_obj_set_size(s_modal, scr_w, scr_h);
    lv_obj_set_pos(s_modal, 0, 0);
    lv_obj_set_style_bg_color(s_modal, lv_color_hex(0xE8E8E8), 0);
    lv_obj_set_style_border_width(s_modal, 0, 0);
    lv_obj_set_style_radius(s_modal, 0, 0);
    lv_obj_set_style_pad_all(s_modal, 0, 0);
    lv_obj_remove_flag(s_modal, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_modal, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_modal, settings_modal_overlay_cb, LV_EVENT_CLICKED, NULL);

    /* 卡片（宽高按屏高缩放，宽不超过屏幕） */
    const int card_w = settings_scaled(400) > scr_w - 2 * SETTINGS_MARGIN
                           ? scr_w - 2 * SETTINGS_MARGIN
                           : settings_scaled(400);
    const int card_h = settings_scaled(480);
    lv_obj_t *card = lv_obj_create(s_modal);
    lv_obj_set_size(card, card_w, card_h);
    lv_obj_center(card);
    lv_obj_set_style_bg_color(card, lv_color_white(), 0);
    lv_obj_set_style_border_color(card, lv_color_black(), 0);
    lv_obj_set_style_border_width(card, 2, 0);
    lv_obj_set_style_radius(card, 12, 0);
    lv_obj_set_style_pad_all(card, 0, 0);
    lv_obj_remove_flag(card, LV_OBJ_FLAG_SCROLLABLE);

    /* 标题 */
    lv_obj_t *title = settings_label_create(card, "选择字体", 20, LV_TEXT_ALIGN_CENTER);
    lv_obj_set_width(title, LV_PCT(100));
    lv_obj_set_pos(title, 0, settings_scaled(12));

    /* 提示（重启生效） */
    lv_obj_t *hint = settings_label_create(card, "选择后重启设备生效", 16, LV_TEXT_ALIGN_CENTER);
    lv_obj_set_width(hint, LV_PCT(100));
    lv_obj_set_pos(hint, 0, settings_scaled(44));

    /* 字体列表（可滚动） */
    lv_obj_t *list = lv_obj_create(card);
    lv_obj_set_size(list, card_w - 24, card_h - settings_scaled(100));
    lv_obj_set_pos(list, 12, settings_scaled(76));
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

        const int item_h = settings_scaled(44) < 32 ? 32 : settings_scaled(44);
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
        lv_obj_set_style_text_font(label, settings_font(16), 0);
        lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_LEFT, 0);
        lv_obj_set_width(label, card_w - 24 - 70);
        lv_obj_align(label, LV_ALIGN_LEFT_MID, 10, 0);
        lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);

        if (is_builtin) {
            lv_obj_t *tag = lv_label_create(item);
            lv_label_set_text(tag, "内置");
            lv_obj_set_style_text_color(tag, is_current ? lv_color_white() : lv_color_black(), 0);
            lv_obj_set_style_text_font(tag, settings_font(16), 0);
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

/** 命中检测：返回按下点命中的行（当前分页内，仅可交互行）。 */
static settings_row_t *settings_hit_row(const lv_point_t *p) {
    for (int s = 0; s < s_page_section_cnt[s_page]; s++) {
        const settings_section_t *sec = s_page_sections[s_page][s];
        for (int g = 0; g < sec->subgroup_count; g++) {
            const settings_subgroup_t *sg = &sec->subgroups[g];
            for (int i = 0; i < sg->row_count; i++) {
                settings_row_t *row = &s_rows[sg->row_start + i];
                if (row->row_obj == NULL) {
                    continue;
                }
                /* 仅可交互行响应点击。 */
                if (row->type != SETTINGS_ROW_STEPPER && row->type != SETTINGS_ROW_CYCLE &&
                    row->type != SETTINGS_ROW_FONT && row->type != SETTINGS_ROW_ACTION) {
                    continue;
                }
                const int x = settings_obj_screen_x(row->row_obj);
                const int y = settings_obj_screen_y(row->row_obj);
                if (settings_point_in(p, x, y, lv_obj_get_width(row->row_obj),
                                      lv_obj_get_height(row->row_obj))) {
                    return row;
                }
            }
        }
    }
    return NULL;
}

/** 分页容器顶部 y / 高度（enter 时计算，惰性构建复用）。 */
static int settings_area_y(void) { return s_area_y; }
static int settings_area_h(void) { return s_area_h; }

/** 构建一个分页的全部控件（卡片 + 大类 + 行）。首次显示该页时调用。 */
static void settings_page_build(int idx) {
    if (idx < 0 || idx >= s_page_count || s_page_built[idx]) {
        return;
    }
    s_page_objs[idx] = lv_obj_create(lv_screen_active());
    lv_obj_set_size(s_page_objs[idx],
                    lv_display_get_horizontal_resolution(lv_display_get_default()),
                    settings_area_h());
    lv_obj_set_pos(s_page_objs[idx], 0, settings_area_y());
    lv_obj_set_style_bg_color(s_page_objs[idx], lv_color_white(), 0);
    lv_obj_set_style_border_width(s_page_objs[idx], 0, 0);
    lv_obj_set_style_radius(s_page_objs[idx], 0, 0);
    lv_obj_set_style_pad_all(s_page_objs[idx], 0, 0);
    lv_obj_remove_flag(s_page_objs[idx], LV_OBJ_FLAG_SCROLLABLE);
    settings_page_create(s_page_objs[idx], s_page_sections[idx], s_page_section_cnt[idx], 8);
    /* 新建的分页默认盖在当前页之上：非当前页立即隐藏。 */
    if (idx != s_page) {
        lv_obj_add_flag(s_page_objs[idx], LV_OBJ_FLAG_HIDDEN);
    }
    s_page_built[idx] = true;
}

/** 切换分页：容器显隐 + 指示点刷新（LVGL 线程内）。 */
static void settings_show_page(int idx) {
    if (idx < 0 || idx >= s_page_count || idx == s_page) {
        return;
    }
    settings_page_build(idx); /* 惰性构建：首次显示才创建控件 */
    s_page = idx;
    for (int i = 0; i < s_page_count; i++) {
        /* 未构建的分页没有控件（指针无效），跳过显隐操作。 */
        if (!s_page_built[i]) {
            lv_obj_set_style_bg_color(s_dots[i], i == idx ? lv_color_black() : lv_color_white(), 0);
            continue;
        }
        if (i == idx) {
            lv_obj_remove_flag(s_page_objs[i], LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(s_page_objs[i], LV_OBJ_FLAG_HIDDEN);
        }
        lv_obj_set_style_bg_color(s_dots[i], i == idx ? lv_color_black() : lv_color_white(), 0);
    }
    ESP_LOGI(TAG, "settings: page %d/%d", idx + 1, s_page_count);
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
    settings_screen_size(&scr_w, &scr_h);

    /* 按屏高计算缩放因子（基准 800；横屏 480 高 -> 0.6，小屏 600 -> 0.75）。 */
    s_scale = (float)scr_h / (float)SETTINGS_REF_H;
    const int bar_h = settings_bar_h();

    /* 统一状态栏：左侧时间、居中"设置"、右侧 WiFi/睡眠图标 */
    s_bar = espaperplay_ui_status_bar_create(scr, bar_h, "设置", false);

    /* 动态分页：按可用高度把大类分组到各页（空间够则同页多类）。 */
    for (int i = 0; i < SETTINGS_PAGE_MAX; i++) {
        s_page_section_cnt[i] = 0;
        s_page_built[i] = false;
    }
    s_area_y = bar_h;
    s_area_h = scr_h - s_area_y - 24;
    settings_build_pages(s_area_h - 8); /* 卡片顶部留 8px 边距 */

    /* 惰性构建：仅立即构建第 0 页，其余分页首次显示时才创建控件
     * （设置页控件量大，全量构建明显拖慢进入速度）。 */
    s_page = 0;
    settings_page_build(0);

    /* 指示点 */
    for (int i = 0; i < s_page_count; i++) {
        s_dots[i] = lv_obj_create(scr);
        lv_obj_set_size(s_dots[i], 10, 10);
        lv_obj_set_pos(s_dots[i], scr_w / 2 + (i - (s_page_count - 1) / 2) * 24 - 5, scr_h - 18);
        lv_obj_set_style_radius(s_dots[i], LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_border_width(s_dots[i], 1, 0);
        lv_obj_set_style_border_color(s_dots[i], lv_color_black(), 0);
        lv_obj_remove_flag(s_dots[i], LV_OBJ_FLAG_SCROLLABLE);
    }
    /* 未构建的分页没有控件，仅指示点需要着色（当前页黑、其余白）。 */
    for (int i = 0; i < s_page_count; i++) {
        lv_obj_set_style_bg_color(s_dots[i], i == 0 ? lv_color_black() : lv_color_white(), 0);
    }

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
        if (adx > SETTINGS_EDGE_SWIPE_PX && adx > ady * SETTINGS_SWIPE_MIN_RATIO) {
            int32_t scr_w = lv_display_get_horizontal_resolution(lv_display_get_default());
            if ((s_touch_start.x < SETTINGS_EDGE_PX && dx > 0) ||
                (s_touch_start.x > scr_w - SETTINGS_EDGE_PX && dx < 0)) {
                if (espaperplay_ui_page_depth() > 1) {
                    ESP_LOGI(TAG, "settings: edge swipe -> pop back");
                    espaperplay_ui_page_pop_lv();
                }
                s_touch_row = NULL;
                return;
            }
        }

        /* 中间横向滑动：切换分页（优先于点击）。 */
        if (adx > SETTINGS_SWIPE_PX && adx > ady * SETTINGS_SWIPE_MIN_RATIO) {
            settings_show_page(s_page + (dx < 0 ? 1 : -1));
            s_touch_row = NULL;
            return;
        }

        /* 小位移 + 起点在行内：点击触发该行操作。 */
        if (s_touch_row != NULL && adx <= SETTINGS_CLICK_MAX_PX && ady <= SETTINGS_CLICK_MAX_PX) {
            settings_row_trigger(s_touch_row);
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
    if (espaperplay_ui_page_depth() > 1) {
        ESP_LOGI(TAG, "settings: single click -> pop back");
        espaperplay_ui_page_pop_lv();
    }
}

/** 设置页页面实例（页面栈用）。 */
const espaperplay_ui_page_t espaperplay_ui_page_settings = {settings_enter, settings_exit,
                                                            settings_on_key, settings_on_touch};
