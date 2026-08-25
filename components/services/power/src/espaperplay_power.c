/*
 * SPDX-FileCopyrightText: 2026 ESPaperPlay Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#include <stdint.h>
#include <time.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "driver/gpio.h"
#include "driver/uart.h"
#include "esp_log.h"
#include "esp_sleep.h"
#include "esp_timer.h"

#include "espaperplay_clock.h"
#include "espaperplay_config.h"
#include "espaperplay_epd.h"
#include "espaperplay_input.h"
#include "espaperplay_power.h"
#include "espaperplay_weather.h"
#include "espaperplay_wifi.h"

static const char *TAG = "ESPaperPlay_POWER";

/* 设备自动浅睡眠默认超时（毫秒）。
 * 应不小于 EPD 面板空闲自动睡眠超时（默认 90s），使面板先进入自身深度
 * 睡眠、ESP32 再浅睡眠，避免面板在 ESP32 睡眠期间仍保持上电。管理任务
 * 在判定时会取 max(本值, EPD 空闲超时 + 5s) 作为实际阈值。 */
#define ESPAPERPLAY_POWER_AUTO_SLEEP_TIMEOUT_MS 30000

/* 自动睡眠管理任务栈与优先级。 */
#define ESPAPERPLAY_POWER_AUTO_SLEEP_TASK_STACK_SIZE 2048
#define ESPAPERPLAY_POWER_AUTO_SLEEP_TASK_PRIORITY 3

/* 唤醒后重置活动时间戳的宽限期（毫秒）：给用户事件处理留出窗口，
 * 避免刚唤醒（尤其触摸 INT 仍保持有效电平）又立即重新睡眠。 */
#define ESPAPERPLAY_POWER_WAKE_GRACE_MS 2000

/* 定时器唤醒后的刷新窗口（毫秒）：唤醒用于周期刷新（如主界面更新时钟）
 * 时，留出足够时间让 LVGL 周期定时器触发 home_refresh 并完成 EPD 局部刷新
 * （EPD 异步 worker 任务持锁，睡眠前会等待在途刷新完成）。窗口结束后
 * 不重置用户活动计时，使管理任务立即重新睡眠。 */
#define ESPAPERPLAY_POWER_REFRESH_GRACE_MS 3000

/* NTP 对时/标定超时（毫秒）：用户唤醒与周期标定重连后等待 NTP 同步的最长时间。 */
#define ESPAPERPLAY_POWER_NTP_TIMEOUT_MS 8000

/* 睡眠期间天气刷新等待超时（毫秒）：天气到期时借定时器唤醒重连拉取，
 * 等待后台任务完成整体刷新的最长时间（重连后 TLS 拉取全部 API 约 5~10s）。 */
#define ESPAPERPLAY_POWER_WEATHER_WAIT_MS 20000

/* 进睡/唤醒后等待状态栏图标落屏的窗口（毫秒）：睡眠图标与 WiFi 图标走同一
 * 局部刷新路径，由状态栏 1s 定时器驱动；进睡前置位标志、唤醒后清除标志，
 * 各留出约 2s 让定时器把图标显隐真正绘制到屏上，再正式睡眠/重连。 */
#define ESPAPERPLAY_POWER_SLEEP_ICON_DELAY_MS 2000

/* 外部活动保持唤醒窗口（毫秒）：最近一次外部活动（如 Web 控制台心跳）
 * 在本窗口内则不进入自动浅睡眠。窗口需大于前端心跳间隔（15s）并覆盖
 * 浏览器后台标签页的定时器节流（隐藏页最坏 ~60s/次），取 70s。 */
#define ESPAPERPLAY_POWER_EXT_ACTIVITY_WINDOW_MS 70000

static uint32_t s_auto_sleep_timeout_ms = ESPAPERPLAY_POWER_AUTO_SLEEP_TIMEOUT_MS;
static uint32_t s_periodic_wakeup_ms = 0;             /*!< 周期定时器唤醒间隔（0=关闭） */
static bool s_periodic_wakeup_minute_aligned = false; /*!< 周期唤醒对齐到分钟边界 */
static bool s_auto_sleep_started = false;
static bool s_wakeup_configured = false;
static bool s_wake_was_timer = false;           /*!< 上次唤醒是否由定时器触发 */
static volatile uint64_t s_ext_activity_ms = 0; /*!< 上次外部活动时刻（0=尚无） */

void espaperplay_power_note_external_activity(void) {
    s_ext_activity_ms = (uint64_t)(esp_timer_get_time() / 1000);
}

/** 外部活动是否仍在保持唤醒窗口内。 */
static bool power_ext_activity_fresh(void) {
    if (s_ext_activity_ms == 0) {
        return false;
    }
    const uint64_t now_ms = (uint64_t)(esp_timer_get_time() / 1000);
    return now_ms < s_ext_activity_ms + ESPAPERPLAY_POWER_EXT_ACTIVITY_WINDOW_MS;
}

esp_err_t espaperplay_power_init(void) {
    ESP_LOGI(TAG, "Power management init");
    return ESP_OK;
}

esp_err_t espaperplay_power_domain_set(espaperplay_power_domain_t domain, bool enable) {
    (void)domain;
    (void)enable;

    ESP_LOGW(TAG, "power_domain_set not implemented yet");
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t espaperplay_power_configure_wakeup(const espaperplay_wakeup_config_t *config) {
    esp_err_t ret = ESP_OK;

    if (config == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    /* 唤醒源固定为设备输入引脚 + 调试串口：
     *   - 触摸 INT（GPIO3，低电平有效）：触摸唤醒；
     *   - BOOT 按键（GPIO0，低电平有效）：按键唤醒；
     *   - UART0：开发期用监视器发送字符唤醒。
     * 睡眠期间 GPIO 电平保持，唤醒后外设状态完整恢复。
     *
     * 注意：两个输入引脚已由各自驱动（touch_init / input_init）配置为
     * 输入（含上拉），此处仅使能唤醒，不再调用 gpio_set_direction，
     * 以免重置触摸 INT 的中断配置（ISR 已在 touch_init 中安装）。 */

    /* 触摸 INT 引脚：低电平唤醒（空闲为高，由外部上拉保证）。 */
    ret = gpio_wakeup_enable((gpio_num_t)ESPAPERPLAY_PIN_TOUCH_INT, GPIO_INTR_LOW_LEVEL);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "touch INT (GPIO%d) wakeup enable failed: %s", ESPAPERPLAY_PIN_TOUCH_INT,
                 esp_err_to_name(ret));
        return ret;
    }

    /* BOOT 按键引脚：低电平唤醒（空闲为高，内部上拉）。 */
    ret = gpio_wakeup_enable((gpio_num_t)ESPAPERPLAY_PIN_KEY_BOOT, GPIO_INTR_LOW_LEVEL);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "BOOT key (GPIO%d) wakeup enable failed: %s", ESPAPERPLAY_PIN_KEY_BOOT,
                 esp_err_to_name(ret));
        return ret;
    }

    /* 统一使能 GPIO 唤醒（须在全部 gpio_wakeup_enable 之后调用一次）。 */
    ret = esp_sleep_enable_gpio_wakeup();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "gpio wakeup enable failed: %s", esp_err_to_name(ret));
        return ret;
    }

    /* 调试串口唤醒（开发期便于用监视器唤醒；失败不致命）。 */
    ret = esp_sleep_enable_uart_wakeup(UART_NUM_0);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "UART wakeup enable failed: %s (debug wakeup disabled)",
                 esp_err_to_name(ret));
    }

    /* 可选：定时器周期唤醒（用于睡眠期间周期刷新，如主界面更新时钟）。
     * 统一由 s_periodic_wakeup_ms 管理，在每次进入浅睡眠前重新装载
     * （esp_sleep_enable_timer_wakeup 为一次性，唤醒后需重设）。 */
    if (config->enable_timer && config->wakeup_timeout_ms > 0) {
        s_periodic_wakeup_ms = config->wakeup_timeout_ms;
        ESP_LOGI(TAG, "timer wakeup configured: %u ms (periodic)",
                 (unsigned)config->wakeup_timeout_ms);
    }

    s_wakeup_configured = true;
    ESP_LOGI(TAG, "wakeup configured: touch INT(GPIO%d) + BOOT(GPIO%d) + UART0",
             ESPAPERPLAY_PIN_TOUCH_INT, ESPAPERPLAY_PIN_KEY_BOOT);
    return ESP_OK;
}

esp_err_t espaperplay_power_enter_light_sleep(void) {
    if (!s_wakeup_configured) {
        ESP_LOGE(TAG, "wakeup not configured, call espaperplay_power_configure_wakeup() first");
        return ESP_ERR_INVALID_STATE;
    }

    /* 先确保 EPD 面板已进入自身深度睡眠：该函数会等待在途刷新完成
     * （持锁）后睡眠，避免浅睡眠期间有 SPI 传输被中断；面板已睡眠时
     * 为无操作。 */
    esp_err_t epd_ret = espaperplay_epd_sleep();
    if (epd_ret != ESP_OK) {
        ESP_LOGW(TAG, "EPD sleep before light sleep failed: %s (skip light sleep)",
                 esp_err_to_name(epd_ret));
        return epd_ret;
    }

    /* 主动断开 STA 并抑制自动重连：手动浅睡眠期间 WiFi modem 断电，无法
     * 保活关联，唤醒后会触发 BEACON_TIMEOUT 被动断开 + 重连（约 2.5s 活跃
     * 爆发）。睡眠前显式断开使时机可控、日志干净；唤醒后由管理任务按
     * 唤醒源决定是否重连。AP 模式（热点）不受影响。 */
    espaperplay_wifi_suspend_for_sleep();

    ESP_LOGI(TAG, "entering light sleep (wakeup: touch/key/uart%s)",
             s_periodic_wakeup_ms > 0 ? "/timer" : "");

    /* 周期定时器唤醒（一次性，每次睡眠前重设）：用于睡眠期间周期刷新
     * （如主界面更新时钟）。0 表示不启用。
     * 若启用"分钟对齐"模式，则计算到下一分钟边界的剩余时间作为唤醒间隔，
     * 使唤醒恰好落在分钟切换点附近，主界面时钟得以在分钟更新时立即刷新
     * （而非固定 60s 相位，导致显示滞后真实分钟达 ~60s）。 */
    if (s_periodic_wakeup_ms > 0 || s_periodic_wakeup_minute_aligned) {
        uint32_t timer_ms = s_periodic_wakeup_ms;
        if (s_periodic_wakeup_minute_aligned) {
            time_t now = time(NULL);
            if (now != (time_t)-1) {
                /* 到下一分钟边界的毫秒；提前 500ms 唤醒，确保 LVGL 1s
                 * 周期定时器在边界后立刻触发 home_refresh 刷新时钟。 */
                int sec = (int)(now % 60);
                int ms_to_boundary = ((60 - sec) % 60) * 1000; /* 0..59000 */
                int wake_ms = ms_to_boundary - 500;            /* -500..58500 */
                if (wake_ms < 500) {
                    wake_ms += 60000; /* 边界临近或刚过：顺延至下一分钟 */
                }
                if (wake_ms > 60000) {
                    wake_ms = 60000;
                }
                timer_ms = (uint32_t)wake_ms;
            } else {
                /* 系统时间未就绪（如 NTP 未同步）：退化为固定 60s。 */
                timer_ms = s_periodic_wakeup_ms > 0 ? s_periodic_wakeup_ms : 60000;
            }
        }
        if (timer_ms > 0) {
            esp_err_t tw = esp_sleep_enable_timer_wakeup((uint64_t)timer_ms * 1000ULL);
            if (tw != ESP_OK) {
                ESP_LOGW(TAG, "timer wakeup enable failed: %s", esp_err_to_name(tw));
            }
        }
    }

    /* 用 esp_timer 前后差值度量本次睡眠的 RC 测得时长（微秒），供时钟漂移
     * 模型仅对睡眠部分补偿（运行期由 XTAL 精确推进，不计入）。 */
    int64_t sleep_start = esp_timer_get_time();
    esp_err_t err = esp_light_sleep_start();
    int64_t sleep_end = esp_timer_get_time();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "light sleep failed: %s", esp_err_to_name(err));
        return err;
    }
    int64_t sleep_us = sleep_end - sleep_start;
    if (sleep_us > 0) {
        espaperplay_clock_account_sleep((uint64_t)sleep_us);
    }

    /* 唤醒：记录唤醒源，重连决策交由自动睡眠管理任务（需结合刷新窗口内
     * 是否有用户操作综合判断）。此处仅标记定时器唤醒标志。 */
    const uint32_t causes = esp_sleep_get_wakeup_causes();
    s_wake_was_timer = (causes & BIT(ESP_SLEEP_WAKEUP_TIMER)) != 0;
    ESP_LOGI(TAG, "woke from light sleep (causes=0x%x%s, slept %lld us)", (unsigned)causes,
             s_wake_was_timer ? ", timer" : "", (long long)sleep_us);
    return ESP_OK;
}

esp_err_t espaperplay_power_set_auto_sleep_timeout_ms(uint32_t timeout_ms) {
    s_auto_sleep_timeout_ms = timeout_ms;
    ESP_LOGI(TAG, "auto sleep timeout set to %u ms (%s)", (unsigned)timeout_ms,
             timeout_ms ? "enabled" : "disabled");
    return ESP_OK;
}

esp_err_t espaperplay_power_set_periodic_wakeup_ms(uint32_t timeout_ms) {
    s_periodic_wakeup_ms = timeout_ms;
    ESP_LOGI(TAG, "periodic wakeup set to %u ms (%s)", (unsigned)timeout_ms,
             timeout_ms ? "enabled" : "disabled");
    return ESP_OK;
}

esp_err_t espaperplay_power_set_periodic_wakeup_minute_aligned(bool enable) {
    s_periodic_wakeup_minute_aligned = enable;
    ESP_LOGI(TAG, "periodic wakeup minute-aligned %s", enable ? "enabled" : "disabled");
    return ESP_OK;
}

/**
 * @brief 等待 STA 连接就绪（最多约 3s）。
 *
 * 重连是异步的（约 2.5s）；后续的 NTP / 天气操作需要连接已建立才能成功。
 * 超时返回后各操作仍按自身超时执行（网络不可用时快速失败，不影响流程）。
 */
static void power_wait_wifi_connected(void) {
    for (int i = 0; i < 15; i++) {
        espaperplay_wifi_status_t st;
        if (espaperplay_wifi_get_status(&st) == ESP_OK && st.connected) {
            return;
        }
        vTaskDelay(pdMS_TO_TICKS(200));
    }
}

/**
 * @brief 自动浅睡眠管理任务。
 *
 * 每 1s 检查最近一次用户活动时刻：若空闲时长超过阈值（取
 * max(配置超时, EPD 空闲超时 + 5s)），进入浅睡眠。唤醒后由
 * espaperplay_power_enter_light_sleep() 内部重置活动时间戳，循环继续。
 */
static void power_auto_sleep_task(void *arg) {
    (void)arg;
    ESP_LOGI(TAG, "auto sleep manager started (base timeout=%u ms)",
             (unsigned)s_auto_sleep_timeout_ms);

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(1000));

        if (s_auto_sleep_timeout_ms == 0) {
            continue; /* 自动睡眠关闭 */
        }

        /* 实际阈值：不小于 EPD 面板空闲超时 + 5s，确保面板先睡。 */
        const uint32_t epd_idle = espaperplay_epd_get_idle_sleep_timeout_ms();
        uint32_t threshold = s_auto_sleep_timeout_ms;
        if (epd_idle > 0 && epd_idle + 5000 > threshold) {
            threshold = epd_idle + 5000;
        }

        const uint64_t now_ms = (uint64_t)(esp_timer_get_time() / 1000);
        const uint64_t last = espaperplay_input_get_last_activity_ms();
        const uint64_t idle_ms = (now_ms > last) ? (now_ms - last) : 0;

        if (idle_ms >= threshold) {
            /* 外部活动（Web 客户端心跳）保持唤醒：窗口内跳过本轮睡眠判定
             * （WiFi 保持连接，心跳可持续到达）。 */
            if (power_ext_activity_fresh()) {
                continue;
            }
            ESP_LOGI(TAG, "idle %llu ms >= threshold %u ms -> light sleep", idle_ms,
                     (unsigned)threshold);
            espaperplay_input_set_sleep_indicator(true);
            /* 先断开 WiFi（睡眠期间 modem 断电，无法保活），再留出约 2s 窗口
             * 让状态栏定时器把睡眠图标绘制到屏上（与 WiFi 图标同一局部刷新
             * 路径），随后才正式进入浅睡眠（睡眠期间屏幕冻结）。 */
            espaperplay_wifi_suspend_for_sleep();
            vTaskDelay(pdMS_TO_TICKS(ESPAPERPLAY_POWER_SLEEP_ICON_DELAY_MS));
            espaperplay_power_enter_light_sleep();

            if (s_wake_was_timer) {
                /* 定时器唤醒（周期刷新时钟）：默认不重连、保持断开，
                 * 留刷新窗口后由下一轮循环立即重新睡眠。时钟标定与天气
                 * 刷新共用同一个联网窗口——任一到期才重连一次，窗口内
                 * 依次完成，尽量减少 WiFi 重连次数以省电。若刷新窗口内
                 * 发生用户操作（触摸/按键），则升级为用户唤醒：重连并
                 * 保持唤醒，避免"刚唤醒刷新完又立刻睡、忽略用户操作"。 */
                const uint64_t activity_at_wake = espaperplay_input_get_last_activity_ms();
                const bool cal_due = espaperplay_clock_is_calibration_due();
                const bool weather_due = espaperplay_weather_is_refresh_due();
                bool connected = false;

                if (cal_due || weather_due) {
                    ESP_LOGI(TAG, "timer wake: reconnecting (%s%s%s)",
                             cal_due ? "clock-calibration" : "",
                             cal_due && weather_due ? " + " : "",
                             weather_due ? "weather-refresh" : "");
                    espaperplay_wifi_resume_after_wake(true);
                    power_wait_wifi_connected();
                    connected = true;
                }
                if (cal_due && connected) {
                    /* NTP 标定（测量/精修漂移率），使本次刷新即显示校正后时间。 */
                    esp_err_t cal = espaperplay_clock_calibrate(ESPAPERPLAY_POWER_NTP_TIMEOUT_MS);
                    if (cal != ESP_OK) {
                        ESP_LOGW(TAG, "clock calibration failed: %s (retry later)",
                                 esp_err_to_name(cal));
                    } else {
                        ESP_LOGI(TAG, "clock calibration ok (drift=%ld ppm)",
                                 (long)espaperplay_clock_get_drift_ppm());
                    }
                }
                if (weather_due && connected) {
                    /* 天气到期：触发后台任务拉取并等待完成（各 API 受 TTL
                     * 约束，未过期项不重复请求）。 */
                    espaperplay_weather_request_refresh();
                    if (!espaperplay_weather_wait_refresh_done(ESPAPERPLAY_POWER_WEATHER_WAIT_MS)) {
                        ESP_LOGW(TAG, "weather refresh wait timeout (%d ms)",
                                 ESPAPERPLAY_POWER_WEATHER_WAIT_MS);
                    }
                }
                vTaskDelay(pdMS_TO_TICKS(ESPAPERPLAY_POWER_REFRESH_GRACE_MS));
                /* 刷新窗口内若有用户操作或 Web 心跳到达（借重连窗口），均升级
                 * 为保持唤醒：维持连接并重置活动计时，避免刚刷新完又睡。 */
                if (espaperplay_input_get_last_activity_ms() > activity_at_wake ||
                    power_ext_activity_fresh()) {
                    /* 升级为用户唤醒：保持连接。 */
                    if (!connected) {
                        espaperplay_wifi_resume_after_wake(true);
                    }
                    espaperplay_input_mark_activity();
                } else {
                    /* 无用户操作：断开以省电（避免下一轮睡眠再触发 BEACON
                     * 被动断连）；未重连过则保持断开，下一轮立即重新睡眠。 */
                    if (connected) {
                        espaperplay_wifi_suspend_for_sleep();
                    }
                }
            } else {
                /* 用户/串口唤醒：立即清除睡眠指示（须在重连/NTP 等阻塞调用
                 * 之前，否则状态栏 1s 定时器要等数秒才能把图标局刷隐藏）。
                 * 定时器唤醒不清除，图标在睡眠期间持续显示。 */
                espaperplay_input_set_sleep_indicator(false);
                /* 重连 WiFi 并强制 NTP 对时（立即校正时钟，消除标定残差），
                 * 再标记活动避免立即重新睡眠。重连约 2.5s；期间若再有用户
                 * 操作，输入路径会持续刷新活动时间戳，设备保持唤醒，重连与
                 * 对时独立进行、互不冲突。 */
                espaperplay_wifi_resume_after_wake(true);
                esp_err_t rs = espaperplay_clock_resync_now(ESPAPERPLAY_POWER_NTP_TIMEOUT_MS);
                if (rs != ESP_OK) {
                    ESP_LOGW(TAG, "user-wake NTP resync failed: %s", esp_err_to_name(rs));
                }
                vTaskDelay(pdMS_TO_TICKS(ESPAPERPLAY_POWER_WAKE_GRACE_MS));
                espaperplay_input_mark_activity();
            }
        }
    }
}

esp_err_t espaperplay_power_start_auto_sleep(void) {
    if (s_auto_sleep_started) {
        ESP_LOGW(TAG, "auto sleep already started");
        return ESP_OK;
    }
    if (!s_wakeup_configured) {
        ESP_LOGE(TAG, "wakeup not configured, call espaperplay_power_configure_wakeup() first");
        return ESP_ERR_INVALID_STATE;
    }

    BaseType_t rc = xTaskCreate(power_auto_sleep_task, "power_auto_sleep",
                                ESPAPERPLAY_POWER_AUTO_SLEEP_TASK_STACK_SIZE, NULL,
                                ESPAPERPLAY_POWER_AUTO_SLEEP_TASK_PRIORITY, NULL);
    if (rc != pdPASS) {
        ESP_LOGE(TAG, "failed to create auto sleep task");
        return ESP_ERR_NO_MEM;
    }

    s_auto_sleep_started = true;
    ESP_LOGI(TAG, "auto sleep manager launched");
    return ESP_OK;
}

esp_err_t espaperplay_power_enter_sleep(void) {
    ESP_LOGW(TAG, "power_enter_sleep (deep sleep) not implemented yet");
    return ESP_ERR_NOT_SUPPORTED;
}
