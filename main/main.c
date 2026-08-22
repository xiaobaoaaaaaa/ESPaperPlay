/*
 * SPDX-FileCopyrightText: 2026 ESPaperPlay Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#include <inttypes.h>
#include <stdio.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"

#include "espaperplay_auth.h"
#include "espaperplay_board.h"
#include "espaperplay_clock.h"
#include "espaperplay_config.h"
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

/**
 * @brief 应用程序入口。
 *
 * 仅负责：
 *   1. 初始化系统，
 *   2. 初始化各模块，
 *   3. 创建 FreeRTOS 任务。
 *
 * 所有业务逻辑都位于各组件中。
 */
void app_main(void) {
    ESP_LOGI(TAG, "%s v%s starting on %s", ESPAPERPLAY_PROJECT_NAME, ESPAPERPLAY_VERSION,
             CONFIG_IDF_TARGET);

    /* 系统级初始化：先从 NVS 加载系统配置（不依赖硬件），再初始化 board / 总线。 */
    ESP_ERROR_CHECK(espaperplay_system_init());
    /* 系统时钟：从 NVS 恢复持久化的时区（不依赖硬件），供 NTP 同步与本地时间输出使用。 */
    ESP_ERROR_CHECK(espaperplay_clock_init());
    /* 设备鉴权：加载密码记录（不依赖硬件），供 Web 等模块校验使用。 */
    ESP_ERROR_CHECK(espaperplay_auth_init());
    /* 会话管理：签发/校验登录态与失败限速锁定（纯内存，不依赖硬件）。 */
    ESP_ERROR_CHECK(espaperplay_session_init());

    ESP_ERROR_CHECK(espaperplay_board_init());

    /* 网络服务：依据系统配置（AP / STA 模式及凭据）启动 WiFi。 */
    ESP_ERROR_CHECK(espaperplay_wifi_init());

    /* Web 管理服务：查看系统状态、修改系统设置（监听所有网络接口）。 */
    ESP_ERROR_CHECK(espaperplay_webserver_start());

    /* 网络时间：STA 联网后自动执行「公网 IP → 地理位置 → 时区 → NTP 同步」。 */
    ESP_ERROR_CHECK(espaperplay_nettime_start());

    /* 天气：STA 联网后按周期拉取和风天气全部数据（实时 / 预报 / 分钟降水 /
     * 预警 / 指数 / 空气质量 / 天文）到内存快照，供屏幕与 Web 展示使用。
     * API Key 与位置通过 Web 管理页配置（NVS 持久化）。 */
    ESP_ERROR_CHECK(espaperplay_weather_start());

    /* 外设模块。 */
    /* SD 卡挂载失败不阻断启动：无卡/未格式化时设备其余功能（网络时钟、
     * 天气、Web 管理、UI）照常工作，仅文件类功能（阅读器）不可用。 */
    esp_err_t storage_err = espaperplay_storage_mount();
    if (storage_err != ESP_OK) {
        ESP_LOGW(TAG, "SD card storage unavailable: %s (device continues without storage)",
                 esp_err_to_name(storage_err));
    }
    ESP_ERROR_CHECK(espaperplay_epd_init());
    /* 应用屏幕空闲自动睡眠超时（Web 可配置，NVS 持久化）。 */
    espaperplay_epd_set_idle_sleep_timeout_ms(
        espaperplay_system_get_config()->epd_idle_sleep_timeout_ms);
    ESP_ERROR_CHECK(espaperplay_touch_init());

    /* 应用级模块。 */
    ESP_ERROR_CHECK(espaperplay_input_init());
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
    ESP_ERROR_CHECK(espaperplay_power_start_auto_sleep());
    ESP_ERROR_CHECK(espaperplay_gui_init());
    /* 应用"连续局刷后强制全刷"阈值（Web 可配置，NVS 持久化）。 */
    espaperplay_gui_set_full_force_after(espaperplay_system_get_config()->gui_full_force_after);
    ESP_ERROR_CHECK(espaperplay_gui_lv_start()); /* LVGL 移植层：初始化 + 渲染任务 */
    /* 初始化统一状态栏调度（1s 周期刷新 + 进睡前准备回调注册）。 */
    espaperplay_ui_status_bar_init();
    /* 字体资产：映射 fonts 分区并注册 LVGL 盘符（需在 LVGL 初始化之后）。 */
    ESP_ERROR_CHECK(espaperplay_fonts_init());
    ESP_ERROR_CHECK(espaperplay_ui_page_push(&espaperplay_ui_page_home)); /* 主界面入栈 */
    ESP_ERROR_CHECK(espaperplay_ui_key_input_start()); /* 按键分发：input 队列 -> GUI 页面 */
    ESP_ERROR_CHECK(espaperplay_reader_init());

    /* 创建任务。 */
    xTaskCreate(system_task, "system_task", ESPAPERPLAY_SYSTEM_TASK_STACK_SIZE, NULL,
                ESPAPERPLAY_SYSTEM_TASK_PRIORITY, NULL);

    ESP_LOGI(TAG, "Startup sequence complete");
}
