/*
 * SPDX-FileCopyrightText: 2026 ESPaperPlay Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#include <inttypes.h>
#include <stdio.h>

#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"

#include "esp_err.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"

#include "espaperplay_auth.h"
#include "espaperplay_board.h"
#include "espaperplay_clock.h"
#include "espaperplay_config.h"
#include "espaperplay_diaglog.h"
#include "espaperplay_epd.h"
#include "espaperplay_fonts.h"
#include "espaperplay_gui.h"
#include "espaperplay_gui_lv.h"
#include "espaperplay_input.h"
#include "espaperplay_nettime.h"
#include "espaperplay_power.h"
#include "espaperplay_reader.h"
#include "espaperplay_session.h"
#include "espaperplay_storage.h"
#include "espaperplay_system.h"
#include "espaperplay_touch.h"
#include "espaperplay_ui.h"
#include "espaperplay_weather.h"
#include "espaperplay_webserver.h"
#include "espaperplay_wifi.h"

static const char *TAG = "ESPaperPlay_MAIN";

#define ESPAPERPLAY_SYSTEM_TASK_STACK_SIZE 4096
#define ESPAPERPLAY_SYSTEM_TASK_PRIORITY 5
#define ESPAPERPLAY_HEARTBEAT_PERIOD_MS 10000

/*!< 显示链路任务栈：复用 app_main 原有的显示初始化调用深度（含 LVGL/字体）。 */
#define ESPAPERPLAY_BOOT_DISPLAY_TASK_STACK_SIZE 8192
#define ESPAPERPLAY_BOOT_DISPLAY_TASK_PRIORITY 5

/*!< 事件位：显示链路（EPD/GUI/LVGL/字体）就绪，主界面可以安全入栈。 */
#define BOOT_DISPLAY_READY_BIT BIT0

static EventGroupHandle_t s_boot_events = NULL;

/**
 * @brief 系统监控任务。
 *
 * 应用任务层的占位实现。当前仅上报运行健康状态（运行时间 / 剩余堆）。
 * 应用级任务（GUI 循环、reader 循环等）将在后续阶段接入此处 ——
 * main.c 本身不包含任何业务逻辑。
 */
static void system_task(void *arg) {
    (void)arg;

    ESP_LOGI(TAG, "System task started");

    while (1) {
        ESP_LOGI(TAG, "uptime: %llu s, free heap: %" PRIu32 " bytes",
                 esp_timer_get_time() / 1000000ULL, (uint32_t)esp_get_free_heap_size());
        vTaskDelay(pdMS_TO_TICKS(ESPAPERPLAY_HEARTBEAT_PERIOD_MS));
    }
}

/** 屏幕日志快捷方式（显示链路未就绪时自动退化为纯串口日志）。 */
#define BOOT_LOGF(...)                                                                             \
    do {                                                                                           \
        if (s_boot_events != NULL &&                                                               \
            (xEventGroupGetBits(s_boot_events) & BOOT_DISPLAY_READY_BIT)) {                        \
            espaperplay_ui_boot_logf(__VA_ARGS__);                                                 \
        }                                                                                          \
    } while (0)

/**
 * @brief 显示链路初始化任务。
 *
 * 把"从上电到屏幕可用"的最短路径单独成线程尽早执行：EPD -> GUI 渲染后端
 * -> LVGL 移植层 -> 字体资产 -> 开机日志屏。完成后置位事件组，主线程的
 * 服务链路从此可以把进度写到屏幕上；主界面入栈则等两条链路汇合后进行。
 *
 * 失败不阻断启动（与 SD 卡容错哲学一致）：仅记录错误并照常置位，后续
 * 页面切换会按原有路径报错。
 */
static void boot_display_task(void *arg) {
    (void)arg;

    esp_err_t err = espaperplay_epd_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "EPD init failed: %s (display unavailable)", esp_err_to_name(err));
    }

    err = espaperplay_gui_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "GUI backend init failed: %s", esp_err_to_name(err));
        xEventGroupSetBits(s_boot_events, BOOT_DISPLAY_READY_BIT);
        vTaskDelete(NULL);
    }
    /* 应用"连续局刷后强制全刷"阈值（Web 可配置，NVS 持久化）。 */
    (void)espaperplay_gui_set_full_force_after(
        espaperplay_system_get_config()->gui_full_force_after);

    err = espaperplay_gui_lv_start();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "LVGL start failed: %s", esp_err_to_name(err));
        xEventGroupSetBits(s_boot_events, BOOT_DISPLAY_READY_BIT);
        vTaskDelete(NULL);
    }
    /* 初始化统一状态栏调度（1s 周期刷新 + 进睡前准备回调注册）。 */
    espaperplay_ui_status_bar_init();

    err = espaperplay_fonts_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "fonts init failed: %s (text rendering degraded)", esp_err_to_name(err));
    } else {
        err = espaperplay_ui_boot_show();
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "boot screen show failed: %s", esp_err_to_name(err));
        } else {
            ESP_LOGI(TAG, "boot screen up");
        }
    }

    /* 空闲自动睡眠超时（Web 可配置，NVS 持久化）。 */
    (void)espaperplay_epd_set_idle_sleep_timeout_ms(
        espaperplay_system_get_config()->epd_idle_sleep_timeout_ms);

    xEventGroupSetBits(s_boot_events, BOOT_DISPLAY_READY_BIT);
    vTaskDelete(NULL);
}

/**
 * @brief 应用程序入口。
 *
 * 启动分两条并行链路：
 *   1. 显示链路（boot_display_task）：EPD -> GUI -> LVGL -> 字体 ->
 *      开机日志屏，让屏幕最早可用地呈现启动过程；
 *   2. 主线程服务链路：存储 / 触摸 / 输入 / 电源 / WiFi / Web / NTP /
 *      天气，每步完成即把进度追加到开机屏。
 *
 * 两链路在事件组上汇合后再推主界面、启动按键分发。所有业务逻辑都位于
 * 各组件中，main.c 仅编排初始化顺序。
 */
void app_main(void) {
    ESP_LOGI(TAG, "%s v%s starting on %s", ESPAPERPLAY_PROJECT_NAME, ESPAPERPLAY_VERSION,
             CONFIG_IDF_TARGET);

    /* ---- 前置系统级初始化（其余模块的硬依赖，必须先行且极快）。 ---- */
    /* NVS 挂载 + 配置加载（WiFi / Web 等全部依赖配置）。 */
    ESP_ERROR_CHECK(espaperplay_system_init());
    /* 时钟：恢复持久化时区（NTP 同步与本地时间输出使用）。 */
    ESP_ERROR_CHECK(espaperplay_clock_init());
    /* 鉴权：加载密码记录（Web 校验使用）。 */
    ESP_ERROR_CHECK(espaperplay_auth_init());
    /* 会话管理：登录态与失败限速锁定（纯内存）。 */
    ESP_ERROR_CHECK(espaperplay_session_init());

    /* 板级 GPIO / SPI 总线（EPD 与触摸初始化的前置）。 */
    ESP_ERROR_CHECK(espaperplay_board_init());

    s_boot_events = xEventGroupCreate();
    if (s_boot_events == NULL) {
        ESP_LOGE(TAG, "boot event group create failed");
        return;
    }

    /* ---- 并行启动：显示链路尽早点亮屏幕，主线程继续拉起服务。 ---- */
    if (xTaskCreate(boot_display_task, "boot_display", ESPAPERPLAY_BOOT_DISPLAY_TASK_STACK_SIZE,
                    NULL, ESPAPERPLAY_BOOT_DISPLAY_TASK_PRIORITY, NULL) != pdPASS) {
        ESP_LOGE(TAG, "boot display task create failed (startup continues headless)");
        xEventGroupSetBits(s_boot_events, BOOT_DISPLAY_READY_BIT); /* 无显示继续启动 */
    }

    /* ---- 服务链路（每步完成即写屏幕进度）。 ---- */
    esp_err_t storage_err = espaperplay_storage_mount();
    if (storage_err != ESP_OK) {
        /* SD 卡挂载失败不阻断启动：无卡/未格式化时设备其余功能照常工作，
         * 仅文件类功能（阅读器）不可用。探测耗时较长（无卡时可达数秒），
         * 放在服务链路首位与显示链路并行，不再阻塞 EPD 点亮。 */
        ESP_LOGW(TAG, "SD card storage unavailable: %s (device continues without storage)",
                 esp_err_to_name(storage_err));
        BOOT_LOGF("SD 卡不可用（无卡或未格式化）");
    } else {
        BOOT_LOGF("SD 卡已挂载");
    }

    /* 诊断日志：SD 可用时把触摸/电源关键事件落到
     * /sdcard/system/diag.log，供长时间无人值守监测（失效后回看现场；
     * Web 文件管理器可直接下载）。SD 不可用时静默停用，不影响其余功能。 */
    (void)espaperplay_diaglog_init();
    espaperplay_diaglog_write("BOOT", "firmware v%s started (reset_reason=%d, storage=%s)",
                              ESPAPERPLAY_VERSION, (int)esp_reset_reason(),
                              (storage_err == ESP_OK) ? "mounted" : "unavailable");

    BOOT_LOGF("触摸控制器初始化…");
    ESP_ERROR_CHECK(espaperplay_touch_init());

    BOOT_LOGF("输入服务初始化…");
    ESP_ERROR_CHECK(espaperplay_input_init());

    BOOT_LOGF("电源管理初始化…");
    ESP_ERROR_CHECK(espaperplay_power_init());
    /* 电源管理：配置浅睡眠唤醒源（触摸 INT / BOOT 按键 / UART），
     * 并启动自动浅睡眠管理任务（无用户操作超时后进入 light sleep）。 */
    const espaperplay_wakeup_config_t wakeup_cfg = {
        .enable_timer = false, /*!< 默认不启用定时器唤醒（睡眠期间不服务 Web） */
        .wakeup_timeout_ms = 0,
        .enable_gpio = true,
        .gpio_num = -1,
        .gpio_level = false,
    };
    ESP_ERROR_CHECK(espaperplay_power_configure_wakeup(&wakeup_cfg));
    /* 设备自动浅睡眠超时（Web 可配置，NVS 持久化）。 */
    (void)espaperplay_power_set_auto_sleep_timeout_ms(
        espaperplay_system_get_config()->auto_sleep_timeout_ms);
    ESP_ERROR_CHECK(espaperplay_power_start_auto_sleep());

    BOOT_LOGF("WiFi 启动…");
    /* 网络服务：依据系统配置（AP / STA 模式及凭据）启动 WiFi（异步连接，
     * 不等待关联完成）。 */
    ESP_ERROR_CHECK(espaperplay_wifi_init());

    BOOT_LOGF("Web 管理服务启动…");
    /* Web 管理：查看系统状态、修改系统设置（监听所有网络接口）。 */
    ESP_ERROR_CHECK(espaperplay_webserver_start());

    BOOT_LOGF("网络时间服务启动…");
    /* 网络时间：STA 联网后自动执行「公网 IP → 地理位置 → 时区 → NTP 同步」。 */
    ESP_ERROR_CHECK(espaperplay_nettime_start());

    BOOT_LOGF("天气服务启动…");
    /* 天气：STA 联网后按周期拉取和风天气全部数据（实时 / 预报 / 分钟降水 /
     * 预警 / 指数 / 空气质量 / 天文）到内存快照。API Key 与位置经 Web
     * 管理页配置（NVS 持久化）。 */
    ESP_ERROR_CHECK(espaperplay_weather_start());

    /* ---- 汇合：等显示链路就绪后推主界面并启动按键分发。 ---- */
    const EventBits_t bits = xEventGroupWaitBits(s_boot_events, BOOT_DISPLAY_READY_BIT, pdFALSE,
                                                 pdTRUE, pdMS_TO_TICKS(15000));
    if (!(bits & BOOT_DISPLAY_READY_BIT)) {
        ESP_LOGE(TAG, "timeout waiting for display pipeline (continuing headless)");
    } else if (!espaperplay_system_is_setup_done()) {
        /* 出厂状态首次开机：推入引导页（替代主界面作为根页面），完成或
         * 跳过后由引导页自行切换到主界面。 */
        ESP_LOGI(TAG, "first boot: setup wizard pushed (%.2fs since boot)",
                 (double)(esp_timer_get_time() / 10000) / 100.0);
        if (espaperplay_ui_page_push(&espaperplay_ui_page_setup) != ESP_OK) {
            ESP_LOGE(TAG, "setup screen push failed");
        }
    } else if (espaperplay_ui_page_push(&espaperplay_ui_page_home) != ESP_OK) {
        ESP_LOGE(TAG, "home screen push failed");
    } else {
        ESP_LOGI(TAG, "home screen pushed (%.2fs since boot)",
                 (double)(esp_timer_get_time() / 10000) / 100.0);
    }

    /* 输入消费：input 双队列 -> LVGL 线程直读。启动失败不直接 abort，
     * 避免 LVGL 任务短暂忙碌导致的超时使整机无法启动（曾触发
     * ESP_ERROR_CHECK 0x107 abort）。 */
    esp_err_t input_err = espaperplay_ui_input_start();
    if (input_err != ESP_OK) {
        ESP_LOGE(TAG, "input start failed: %s (retry once in 500ms)", esp_err_to_name(input_err));
        vTaskDelay(pdMS_TO_TICKS(500));
        input_err = espaperplay_ui_input_start();
        ESP_ERROR_CHECK(input_err);
    }
    ESP_ERROR_CHECK(espaperplay_reader_init());

    /* 创建任务。 */
    xTaskCreate(system_task, "system_task", ESPAPERPLAY_SYSTEM_TASK_STACK_SIZE, NULL,
                ESPAPERPLAY_SYSTEM_TASK_PRIORITY, NULL);

    ESP_LOGI(TAG, "Startup sequence complete");
}
