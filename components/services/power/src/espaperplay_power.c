/*
 * SPDX-FileCopyrightText: 2026 ESPaperPlay Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#include <stdint.h>
#include <string.h>
#include <time.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "driver/gpio.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_log.h"
#include "esp_sleep.h"
#include "esp_timer.h"

#include "espaperplay_audio.h"
#include "espaperplay_board.h"
#include "espaperplay_clock.h"
#include "espaperplay_config.h"
#include "espaperplay_diaglog.h"
#include "espaperplay_epd.h"
#include "espaperplay_gui.h"
#include "espaperplay_input.h"
#include "espaperplay_power.h"
#include "espaperplay_ui.h"
#include "espaperplay_wifi.h"

static const char *TAG = "ESPaperPlay_POWER";

/* 设备自动浅睡眠默认超时（毫秒）。
 * 应不小于 EPD 面板空闲自动睡眠超时（默认 90s），使面板先进入自身深度
 * 睡眠、ESP32 再浅睡眠，避免面板在 ESP32 睡眠期间仍保持上电。管理任务
 * 在判定时会取 max(本值, EPD 空闲超时 + 5s) 作为实际阈值。 */
#define ESPAPERPLAY_POWER_AUTO_SLEEP_TIMEOUT_MS 30000

/* 自动睡眠管理任务栈与优先级。
 * 栈深度：该任务调用链含 WiFi 挂起、EPD 睡眠等待与 diaglog 写文件
 * （stdio/FATFS 链路耗栈），2048 已实测溢出，取 4096。 */
#define ESPAPERPLAY_POWER_AUTO_SLEEP_TASK_STACK_SIZE 4096
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

/* 睡眠联网服务注册表：固定容量避免运行期堆分配。到期时间使用 esp_timer
 * 单调时钟（浅睡眠后仍连续），并允许临近截止时间的服务合并进同一窗口。 */
#define ESPAPERPLAY_POWER_MAX_SLEEP_REFRESH_SERVICES 8
#define ESPAPERPLAY_POWER_NETWORK_COALESCE_MS 30000

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

typedef struct {
    espaperplay_sleep_refresh_service_t service;
    uint64_t next_due_ms;
} power_sleep_refresh_slot_t;

static power_sleep_refresh_slot_t s_sleep_refresh[ESPAPERPLAY_POWER_MAX_SLEEP_REFRESH_SERVICES];
static size_t s_sleep_refresh_count = 0;
static uint64_t s_last_network_wake_ms = 0;
static portMUX_TYPE s_sleep_refresh_mux = portMUX_INITIALIZER_UNLOCKED;

/* ---- 电池监测（v2.0 板：BAT_SENSE 分压 ADC + TP4056 状态回读） ---- */

/** BAT_SENSE 对应的 ADC 通道（GPIO1 = ADC1_CH0）。 */
#define ESPAPERPLAY_POWER_BAT_ADC_UNIT ADC_UNIT_1
#define ESPAPERPLAY_POWER_BAT_ADC_CHANNEL ADC_CHANNEL_0

/** 电池电压 ADC 采样次数（高阻分压 + 100nF 保持，均值去抖）。 */
#define ESPAPERPLAY_POWER_BAT_ADC_SAMPLES 16

/** 电池电压->电量百分比的查表锚点（毫伏，升序；锂电开路电压近似曲线）。 */
static const uint16_t power_bat_curve_mv[] = {
    3300, 3450, 3550, 3650, 3700, 3750, 3800, 3850, 3900, 4000, 4100, 4200,
};
/** 与上方电压锚点一一对应的电量百分比。 */
static const uint8_t power_bat_curve_pct[] = {
    0, 5, 10, 20, 30, 40, 50, 60, 70, 85, 95, 100,
};

static adc_oneshot_unit_handle_t s_bat_adc_unit;
static adc_cali_handle_t s_bat_adc_cali;
static bool s_battery_ready;

static uint32_t power_effective_refresh_interval(uint32_t requested_ms) {
    return requested_ms < ESPAPERPLAY_POWER_MIN_NETWORK_WAKE_INTERVAL_MS
               ? ESPAPERPLAY_POWER_MIN_NETWORK_WAKE_INTERVAL_MS
               : requested_ms;
}

static bool power_network_window_allowed(uint64_t now_ms) {
    portENTER_CRITICAL(&s_sleep_refresh_mux);
    const uint64_t last_wake_ms = s_last_network_wake_ms;
    portEXIT_CRITICAL(&s_sleep_refresh_mux);
    return last_wake_ms == 0 ||
           now_ms >= last_wake_ms + ESPAPERPLAY_POWER_MIN_NETWORK_WAKE_INTERVAL_MS;
}

/** 返回最近一个联网服务截止时间距 now 的延时；0 表示无注册服务。 */
static uint32_t power_next_network_wakeup_delay_ms(uint64_t now_ms) {
    uint64_t earliest = UINT64_MAX;
    portENTER_CRITICAL(&s_sleep_refresh_mux);
    for (size_t i = 0; i < s_sleep_refresh_count; i++) {
        if (s_sleep_refresh[i].next_due_ms < earliest) {
            earliest = s_sleep_refresh[i].next_due_ms;
        }
    }
    const uint64_t last_wake_ms = s_last_network_wake_ms;
    portEXIT_CRITICAL(&s_sleep_refresh_mux);
    if (earliest == UINT64_MAX) {
        return 0;
    }

    /* 即使某服务给出了更细粒度，也不能突破全局 WiFi 窗口下限。 */
    const uint64_t allowed_ms =
        last_wake_ms == 0 ? 0 : last_wake_ms + ESPAPERPLAY_POWER_MIN_NETWORK_WAKE_INTERVAL_MS;
    if (earliest < allowed_ms) {
        earliest = allowed_ms;
    }
    if (earliest <= now_ms) {
        return 1; /* ESP-IDF 不接受 0us 定时器；尽快唤醒即可。 */
    }
    const uint64_t delay = earliest - now_ms;
    return delay > UINT32_MAX ? UINT32_MAX : (uint32_t)delay;
}

/**
 * 取出本联网窗口需要刷新的服务，并把各自截止时间推进到未来。
 * 先复制配置再在临界区外执行 is_due，避免用户回调阻塞调度锁。
 */
static size_t power_collect_due_refresh_services(uint64_t now_ms,
                                                 espaperplay_sleep_refresh_service_t *due,
                                                 size_t due_capacity) {
    espaperplay_sleep_refresh_service_t candidates[ESPAPERPLAY_POWER_MAX_SLEEP_REFRESH_SERVICES];
    size_t candidate_count = 0;
    const uint64_t cutoff_ms = now_ms + ESPAPERPLAY_POWER_NETWORK_COALESCE_MS;

    portENTER_CRITICAL(&s_sleep_refresh_mux);
    const bool network_window_allowed =
        s_last_network_wake_ms == 0 ||
        now_ms >= s_last_network_wake_ms + ESPAPERPLAY_POWER_MIN_NETWORK_WAKE_INTERVAL_MS;
    if (network_window_allowed) {
        for (size_t i = 0; i < s_sleep_refresh_count; i++) {
            power_sleep_refresh_slot_t *slot = &s_sleep_refresh[i];
            if (slot->next_due_ms > cutoff_ms) {
                continue;
            }
            if (candidate_count < ESPAPERPLAY_POWER_MAX_SLEEP_REFRESH_SERVICES) {
                candidates[candidate_count++] = slot->service;
            }
            const uint32_t interval =
                power_effective_refresh_interval(slot->service.refresh_interval_ms);
            do {
                slot->next_due_ms += interval;
            } while (slot->next_due_ms <= cutoff_ms);
        }
    }
    portEXIT_CRITICAL(&s_sleep_refresh_mux);

    size_t due_count = 0;
    for (size_t i = 0; i < candidate_count && due_count < due_capacity; i++) {
        if (candidates[i].is_refresh_due == NULL || candidates[i].is_refresh_due()) {
            due[due_count++] = candidates[i];
        }
    }
    return due_count;
}

/** WiFi 连接失败时把本批服务安排到下一个允许的联网窗口重试。 */
static void power_reschedule_refresh_services(const espaperplay_sleep_refresh_service_t *services,
                                              size_t count, uint64_t now_ms) {
    const uint64_t retry_ms = now_ms + ESPAPERPLAY_POWER_MIN_NETWORK_WAKE_INTERVAL_MS;
    portENTER_CRITICAL(&s_sleep_refresh_mux);
    for (size_t i = 0; i < s_sleep_refresh_count; i++) {
        for (size_t j = 0; j < count; j++) {
            if (strcmp(s_sleep_refresh[i].service.name, services[j].name) == 0 &&
                s_sleep_refresh[i].next_due_ms > retry_ms) {
                s_sleep_refresh[i].next_due_ms = retry_ms;
                break;
            }
        }
    }
    portEXIT_CRITICAL(&s_sleep_refresh_mux);
}

esp_err_t espaperplay_power_register_sleep_refresh_service(
    const espaperplay_sleep_refresh_service_t *service) {
    if (service == NULL || service->name == NULL || service->name[0] == '\0' ||
        service->refresh_interval_ms == 0 || service->request_refresh == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    const uint64_t now_ms = (uint64_t)(esp_timer_get_time() / 1000);
    const uint32_t interval_ms = power_effective_refresh_interval(service->refresh_interval_ms);
    esp_err_t result = ESP_OK;
    bool added = false;

    portENTER_CRITICAL(&s_sleep_refresh_mux);
    for (size_t i = 0; i < s_sleep_refresh_count; i++) {
        if (strcmp(s_sleep_refresh[i].service.name, service->name) == 0) {
            const espaperplay_sleep_refresh_service_t *old = &s_sleep_refresh[i].service;
            if (old->refresh_interval_ms != service->refresh_interval_ms ||
                old->refresh_timeout_ms != service->refresh_timeout_ms ||
                old->is_refresh_due != service->is_refresh_due ||
                old->request_refresh != service->request_refresh ||
                old->wait_refresh_done != service->wait_refresh_done) {
                result = ESP_ERR_INVALID_STATE;
            }
            portEXIT_CRITICAL(&s_sleep_refresh_mux);
            return result;
        }
    }
    if (s_sleep_refresh_count >= ESPAPERPLAY_POWER_MAX_SLEEP_REFRESH_SERVICES) {
        result = ESP_ERR_NO_MEM;
    } else {
        power_sleep_refresh_slot_t *slot = &s_sleep_refresh[s_sleep_refresh_count++];
        slot->service = *service;
        slot->next_due_ms = now_ms + interval_ms;
        added = true;
    }
    portEXIT_CRITICAL(&s_sleep_refresh_mux);

    if (added) {
        ESP_LOGI(TAG, "sleep refresh registered: %s, interval=%u ms%s", service->name,
                 (unsigned)interval_ms,
                 interval_ms != service->refresh_interval_ms ? " (clamped)" : "");
    }
    return result;
}

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

/** 电压（毫伏）查表插值为电量百分比。 */
static uint8_t power_battery_voltage_to_percent(uint32_t voltage_mv) {
    const size_t points = sizeof(power_bat_curve_mv) / sizeof(power_bat_curve_mv[0]);
    if (voltage_mv <= power_bat_curve_mv[0]) {
        return 0;
    }
    for (size_t i = 1; i < points; i++) {
        if (voltage_mv <= power_bat_curve_mv[i]) {
            const uint32_t lo_mv = power_bat_curve_mv[i - 1];
            const uint32_t hi_mv = power_bat_curve_mv[i];
            const uint32_t lo_pct = power_bat_curve_pct[i - 1];
            const uint32_t hi_pct = power_bat_curve_pct[i];
            return (uint8_t)(lo_pct + (hi_pct - lo_pct) * (voltage_mv - lo_mv) / (hi_mv - lo_mv));
        }
    }
    return 100;
}

/**
 * 读电池端电压（毫伏）：多次采样取均值后按分压比反算。
 * ADC 未就绪时返回 0（调用方以 0 电压降级呈现）。
 */
static uint32_t power_battery_read_voltage_mv(void) {
    if (!s_battery_ready) {
        return 0;
    }

    int raw = 0;
    uint32_t raw_sum = 0;
    uint32_t valid = 0;
    for (int i = 0; i < ESPAPERPLAY_POWER_BAT_ADC_SAMPLES; i++) {
        if (adc_oneshot_read(s_bat_adc_unit, ESPAPERPLAY_POWER_BAT_ADC_CHANNEL, &raw) ==
            ESP_OK) {
            raw_sum += (uint32_t)raw;
            valid++;
        }
    }
    if (valid == 0) {
        return 0;
    }

    int voltage_mv = 0;
    if (s_bat_adc_cali == NULL ||
        adc_cali_raw_to_voltage(s_bat_adc_cali, (int)(raw_sum / valid), &voltage_mv) != ESP_OK) {
        /* 校准不可用时的粗略回退：12-bit 满量程按标称 3.3V 折算。 */
        voltage_mv = (int)((raw_sum / valid) * 3300U / 4095U);
    }

    /* 分压反算：V_bat = V_pin × (R_upper + R_lower) / R_lower。分压比
     * 由 config 宏给出（10M/3.3M ≈ 0.248），乘法先做避免精度损失。 */
    return (uint32_t)((uint32_t)voltage_mv *
                      (ESPAPERPLAY_BAT_DIVIDER_R_UPPER_KOHM + ESPAPERPLAY_BAT_DIVIDER_R_LOWER_KOHM) /
                      ESPAPERPLAY_BAT_DIVIDER_R_LOWER_KOHM);
}

esp_err_t espaperplay_power_get_battery_info(espaperplay_battery_info_t *info) {
    if (info == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_battery_ready) return ESP_ERR_INVALID_STATE;

    /* 充电状态（低有效、开漏 + 外部上拉）：无 USB 时 TP4056 无 VCC，
     * 两脚均被上拉为高 => 放电态；任一拉低即 USB 在位。 */
    const bool charging = gpio_get_level(ESPAPERPLAY_PIN_CHRG_STAT) == 0;
    const bool full = gpio_get_level(ESPAPERPLAY_PIN_STANDBY_STAT) == 0;

    info->voltage_mv = power_battery_read_voltage_mv();
    info->percent = power_battery_voltage_to_percent(info->voltage_mv);
    info->state = charging ? ESPAPERPLAY_BATTERY_CHARGING
                           : (full ? ESPAPERPLAY_BATTERY_FULL
                                   : ESPAPERPLAY_BATTERY_DISCHARGING);
    info->usb_present = charging || full;
    return ESP_OK;
}

esp_err_t espaperplay_power_init(void) {
    if (s_battery_ready) return ESP_OK;
    /* 充电状态回读：输入、无内部上下拉（板上 100K 上拉至常供 3V3）。 */
    gpio_config_t stat_cfg = {
        .pin_bit_mask = BIT64(ESPAPERPLAY_PIN_CHRG_STAT) | BIT64(ESPAPERPLAY_PIN_STANDBY_STAT),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    esp_err_t ret = gpio_config(&stat_cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "charge status GPIO config failed: %s", esp_err_to_name(ret));
        return ret;
    }

    /* 电池电压 ADC：BAT_SENSE（ADC1_CH0），分压后满量程约 1.05V，
     * DB_12 衰减足够且留有裕量。校准曲线创建失败时读数走粗略回退。 */
    const adc_oneshot_unit_init_cfg_t unit_cfg = {
        .unit_id = ESPAPERPLAY_POWER_BAT_ADC_UNIT,
    };
    ret = adc_oneshot_new_unit(&unit_cfg, &s_bat_adc_unit);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "battery ADC unit init failed: %s", esp_err_to_name(ret));
        return ret;
    }
    const adc_oneshot_chan_cfg_t chan_cfg = {
        .atten = ADC_ATTEN_DB_12,
        .bitwidth = ADC_BITWIDTH_DEFAULT,
    };
    ret = adc_oneshot_config_channel(s_bat_adc_unit, ESPAPERPLAY_POWER_BAT_ADC_CHANNEL, &chan_cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "battery ADC channel config failed: %s", esp_err_to_name(ret));
        adc_oneshot_del_unit(s_bat_adc_unit);
        s_bat_adc_unit = NULL;
        return ret;
    }
    const adc_cali_curve_fitting_config_t cali_cfg = {
        .unit_id = ESPAPERPLAY_POWER_BAT_ADC_UNIT,
        .chan = ESPAPERPLAY_POWER_BAT_ADC_CHANNEL,
        .atten = ADC_ATTEN_DB_12,
        .bitwidth = ADC_BITWIDTH_DEFAULT,
    };
    (void)adc_cali_create_scheme_curve_fitting(&cali_cfg, &s_bat_adc_cali);
    s_battery_ready = true;

    espaperplay_battery_info_t bat;
    if (espaperplay_power_get_battery_info(&bat) == ESP_OK) {
        ESP_LOGI(TAG, "Power management init: battery %u mV (%u%%), %s%s", (unsigned)bat.voltage_mv,
                 (unsigned)bat.percent, bat.usb_present ? "usb present, " : "",
                 bat.state == ESPAPERPLAY_BATTERY_CHARGING
                     ? "charging"
                     : (bat.state == ESPAPERPLAY_BATTERY_FULL ? "full" : "discharging"));
    }
    return ESP_OK;
}

esp_err_t espaperplay_power_configure_wakeup(const espaperplay_wakeup_config_t *config) {
    esp_err_t ret = ESP_OK;

    if (config == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    /* 唤醒源固定为设备输入引脚：
     *   - 触摸 INT（GPIO3，低电平有效）：触摸唤醒；
     *   - BOOT 按键（GPIO0，低电平有效）：按键唤醒；
     *   - VOL+ / VOL- 按键（GPIO2 / GPIO5，低电平有效）：任意按键唤醒
     *     （v2.0 板 UART0 引脚已让位给音量键 / 功放控制，日志走 USB
     *     Serial/JTAG，串口唤醒随之取消）。
     * 睡眠期间 GPIO 电平保持，唤醒后外设状态完整恢复。
     *
     * 注意：输入引脚已由各自驱动（touch_init / input_init）配置为
     * 输入（含上拉），此处仅使能唤醒，不再调用 gpio_set_direction，
     * 以免重置触摸 INT 的中断配置（ISR 已在 touch_init 中安装）。 */

    /* 触摸 INT 引脚：低电平唤醒（空闲为高，由外部上拉保证）。 */
    ret = gpio_wakeup_enable((gpio_num_t)ESPAPERPLAY_PIN_TOUCH_INT, GPIO_INTR_LOW_LEVEL);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "touch INT (GPIO%d) wakeup enable failed: %s", ESPAPERPLAY_PIN_TOUCH_INT,
                 esp_err_to_name(ret));
        return ret;
    }

    /* 按键引脚：低电平唤醒（BOOT 内部上拉；VOL± 外部 10K 上拉）。 */
    const int wakeup_key_gpios[] = {
        ESPAPERPLAY_PIN_KEY_BOOT,
        ESPAPERPLAY_PIN_KEY_VOL_UP,
        ESPAPERPLAY_PIN_KEY_VOL_DOWN,
    };
    for (size_t i = 0; i < sizeof(wakeup_key_gpios) / sizeof(wakeup_key_gpios[0]); i++) {
        ret = gpio_wakeup_enable((gpio_num_t)wakeup_key_gpios[i], GPIO_INTR_LOW_LEVEL);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "key (GPIO%d) wakeup enable failed: %s", wakeup_key_gpios[i],
                     esp_err_to_name(ret));
            return ret;
        }
    }

    /* 统一使能 GPIO 唤醒（须在全部 gpio_wakeup_enable 之后调用一次）。 */
    ret = esp_sleep_enable_gpio_wakeup();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "gpio wakeup enable failed: %s", esp_err_to_name(ret));
        return ret;
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
    ESP_LOGI(TAG, "wakeup configured: touch INT(GPIO%d) + BOOT(GPIO%d) + VOL+(GPIO%d) + VOL-(GPIO%d)",
             ESPAPERPLAY_PIN_TOUCH_INT, ESPAPERPLAY_PIN_KEY_BOOT, ESPAPERPLAY_PIN_KEY_VOL_UP,
             ESPAPERPLAY_PIN_KEY_VOL_DOWN);
    return ESP_OK;
}

esp_err_t espaperplay_power_enter_light_sleep(void) {
    if (!s_wakeup_configured) {
        ESP_LOGE(TAG, "wakeup not configured, call espaperplay_power_configure_wakeup() first");
        return ESP_ERR_INVALID_STATE;
    }

    /* 音频子系统下电（未建流为无操作）：浅睡眠会停 I2S 时钟/DMA，
     * 在途流必然中断；主动收尾同时确保 AUDIO_3V3 轨不带着功放过夜。 */
    if (espaperplay_audio_is_active()) {
        ESP_LOGW(TAG, "audio stream still active, tearing down before sleep");
        (void)espaperplay_audio_stop();
    }

    /* 冻结 GUI 刷新管线：从现在到 esp_light_sleep_start 之间不允许再有任何
     * EPD 刷新进入执行（epd_sleep 只保证等待"已在执行"的刷新完成，冻结前
     * 实测仍有新帧在其返回后排入并在睡眠入口执行，SPI DMA 被浅睡眠打断）。
     * worker 会丢弃已就绪帧，flush 丢弃新帧；唤醒后统一解冻。 */
    espaperplay_gui_set_frozen(true);

    /* 先确保 EPD 面板已进入自身深度睡眠：该函数会等待在途刷新完成
     * （持锁）后睡眠，避免浅睡眠期间有 SPI 传输被中断；面板已睡眠时
     * 为无操作。 */
    esp_err_t epd_ret = espaperplay_epd_sleep();
    if (epd_ret != ESP_OK) {
        ESP_LOGW(TAG, "EPD sleep before light sleep failed: %s (skip light sleep)",
                 esp_err_to_name(epd_ret));
        espaperplay_gui_set_frozen(false);
        return epd_ret;
    }

    /* 主动断开 STA 并抑制自动重连：手动浅睡眠期间 WiFi modem 断电，无法
     * 保活关联，唤醒后会触发 BEACON_TIMEOUT 被动断开 + 重连（约 2.5s 活跃
     * 爆发）。睡眠前显式断开使时机可控、日志干净；唤醒后由管理任务按
     * 唤醒源决定是否重连。AP 模式（热点）不受影响。 */
    espaperplay_wifi_suspend_for_sleep();

    /* 周期定时器唤醒（一次性，每次睡眠前重设）：用于睡眠期间周期刷新
     * （如主界面更新时钟）。0 表示不启用。
     * 若启用"分钟对齐"模式，则计算到下一分钟边界的剩余时间作为唤醒间隔，
     * 使唤醒恰好落在分钟切换点附近，主界面时钟得以在分钟更新时立即刷新
     * （而非固定 60s 相位，导致显示滞后真实分钟达 ~60s）。 */
    uint32_t timer_ms = 0;
    if (s_periodic_wakeup_ms > 0 || s_periodic_wakeup_minute_aligned) {
        timer_ms = s_periodic_wakeup_ms;
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
    }
    const uint64_t now_ms = (uint64_t)(esp_timer_get_time() / 1000);
    const uint32_t network_timer_ms = power_next_network_wakeup_delay_ms(now_ms);
    if (network_timer_ms > 0 && (timer_ms == 0 || network_timer_ms < timer_ms)) {
        timer_ms = network_timer_ms;
    }
    if (timer_ms > 0) {
        esp_err_t tw = esp_sleep_enable_timer_wakeup((uint64_t)timer_ms * 1000ULL);
        if (tw != ESP_OK) {
            ESP_LOGW(TAG, "timer wakeup enable failed: %s", esp_err_to_name(tw));
        }
    }
    ESP_LOGI(TAG, "entering light sleep (wakeup: touch/key%s, next=%u ms)",
             timer_ms > 0 ? "/timer" : "", (unsigned)timer_ms);

    /* 睡眠前置守卫：触摸 INT 仍处于有效电平（低）时进入睡眠会被低电平
     * 唤醒立即弹回（触摸任务可能正在恢复时序中驱动 INT，或芯片复位后
     * 有待读帧），形成「进入即唤醒」空转。等待其释放（至多约 1s），仍
     * 为低则本轮放弃睡眠（ESP_ERR_INVALID_STATE），管理任务下一轮重试。 */
    if (gpio_get_level(ESPAPERPLAY_PIN_TOUCH_INT) == 0) {
        for (int i = 0; i < 10 && gpio_get_level(ESPAPERPLAY_PIN_TOUCH_INT) == 0; i++) {
            vTaskDelay(pdMS_TO_TICKS(100));
        }
        if (gpio_get_level(ESPAPERPLAY_PIN_TOUCH_INT) == 0) {
            static int64_t s_last_skip_log_ms = 0;
            const int64_t now_ms = esp_timer_get_time() / 1000;
            ESP_LOGW(TAG, "touch INT still active, skip this light sleep cycle");
            if (now_ms - s_last_skip_log_ms >= 60000) { /* 持续阻塞时限流落盘 */
                s_last_skip_log_ms = now_ms;
                espaperplay_diaglog_write("PWR", "sleep skipped: touch INT still active");
            }
            espaperplay_gui_set_frozen(false);
            return ESP_ERR_INVALID_STATE;
        }
    }

    /* 用 esp_timer 前后差值度量本次睡眠的 RC 测得时长（微秒），供时钟漂移
     * 模型仅对睡眠部分补偿（运行期由 XTAL 精确推进，不计入）。 */
    espaperplay_diaglog_write("PWR", "enter light sleep%s", timer_ms > 0 ? " (timer armed)" : "");
    int64_t sleep_start = esp_timer_get_time();
    esp_err_t err = esp_light_sleep_start();
    int64_t sleep_end = esp_timer_get_time();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "light sleep failed: %s", esp_err_to_name(err));
        espaperplay_diaglog_write("PWR", "light sleep FAILED: %s", esp_err_to_name(err));
        espaperplay_gui_set_frozen(false);
        return err;
    }
    int64_t sleep_us = sleep_end - sleep_start;
    if (sleep_us > 0) {
        espaperplay_clock_account_sleep((uint64_t)sleep_us);
    }

    /* 唤醒：记录唤醒源，重连决策交由自动睡眠管理任务（需结合刷新窗口内
     * 是否有用户操作综合判断）。GPIO（触摸/按键）唤醒优先于定时器判定：
     * 两者同帧置位时必须按用户唤醒处理（清睡眠图标、重连网络），否则
     * 图标滞留、设备状态错乱。 */
    const uint32_t causes = esp_sleep_get_wakeup_causes();
    const bool woke_by_gpio = (causes & BIT(ESP_SLEEP_WAKEUP_GPIO)) != 0;
    s_wake_was_timer = !woke_by_gpio && (causes & BIT(ESP_SLEEP_WAKEUP_TIMER)) != 0;
    ESP_LOGI(TAG, "woke from light sleep (causes=0x%x%s%s, slept %lld us)", (unsigned)causes,
             s_wake_was_timer ? ", timer" : "", woke_by_gpio ? ", gpio" : "", (long long)sleep_us);
    espaperplay_diaglog_write("PWR", "woke: causes=0x%x%s%s, slept %lld us", (unsigned)causes,
                              s_wake_was_timer ? " (timer)" : "", woke_by_gpio ? " (gpio)" : "",
                              (long long)sleep_us);
    /* 唤醒：解冻刷新管线，后续的图标清除 / 界面恢复正常走刷新路径。 */
    espaperplay_gui_set_frozen(false);
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
 * 重连是异步的（约 2.5s）；后续的 NTP / 网络服务刷新需要连接已建立。
 * 超时返回后各操作仍按自身超时执行（网络不可用时快速失败，不影响流程）。
 */
static bool power_wait_wifi_connected(void) {
    for (int i = 0; i < 15; i++) {
        espaperplay_wifi_status_t st;
        if (espaperplay_wifi_get_status(&st) == ESP_OK && st.connected) {
            return true;
        }
        vTaskDelay(pdMS_TO_TICKS(200));
    }
    return false;
}

/**
 * @brief 自动浅睡眠管理任务。
 *
 * 每 1s 检查最近一次用户活动时刻：若空闲时长超过阈值（取
 * max(配置超时, EPD 空闲超时 + 5s)），进入浅睡眠。唤醒后由
 * espaperplay_power_enter_light_sleep() 内部重置活动时间戳，循环继续。
 *
 * 入睡前若栈顶为主界面，先压入睡眠屏保页并等落屏（大字时钟等信息页
 * 在睡眠期间由电子纸双稳态保持）；用户唤醒（含定时器唤醒升级）时弹出
 * 屏保、重建主界面，定时器唤醒则保留屏保借刷新窗口更新时钟后重新入睡。
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
            /* 音频流活跃（播放 / 录音进行中）同样保持唤醒：浅睡眠会打断
             * I2S 流；会话结束（调用方 stop）后本判定自然放行入睡。 */
            if (espaperplay_audio_is_active()) {
                continue;
            }
            ESP_LOGI(TAG, "idle %llu ms >= threshold %u ms -> light sleep", idle_ms,
                     (unsigned)threshold);

            /* 睡眠屏保：栈顶为主界面时，先压入屏保页并同步等待渲染落屏，
             * 使浅睡眠期间屏幕显示大字时钟等信息页（电子纸双稳态保持）。
             * 屏保整屏替换主界面，无需再等状态栏睡眠图标局刷；非主界面
             * （或屏保失败）走原路径（睡眠图标 + 2s 落屏窗口）。 */
            const bool screensaver = espaperplay_ui_screensaver_show();
            if (screensaver && espaperplay_input_get_last_activity_ms() != last) {
                /* 屏保落屏窗口内有用户触摸：屏保页 on_touch 已自行弹出并
                 * 重建主界面，本轮放弃入睡（下轮循环因活动新鲜而保持
                 * 唤醒）——避免「用户在场却入睡」与屏保/主界面切换竞态。 */
                continue;
            }

            espaperplay_input_set_sleep_indicator(true);
            /* 先断开 WiFi（睡眠期间 modem 断电，无法保活），再留出约 2s 窗口
             * 让状态栏定时器把睡眠图标绘制到屏上（与 WiFi 图标同一局部刷新
             * 路径），随后才正式进入浅睡眠（睡眠期间屏幕冻结）。 */
            espaperplay_wifi_suspend_for_sleep();
            if (!screensaver) {
                vTaskDelay(pdMS_TO_TICKS(ESPAPERPLAY_POWER_SLEEP_ICON_DELAY_MS));
            }
            const esp_err_t sleep_ret = espaperplay_power_enter_light_sleep();
            if (sleep_ret == ESP_ERR_INVALID_STATE) {
                /* 睡前守卫拦截（触摸 INT 未释放）：本次未真正入睡。必须
                 * 原地重试而非落入下方唤醒分支——那里依据的是上一次唤醒
                 * 的陈旧状态，定时器分支不清睡眠图标，会让图标永久滞留
                 * 且设备看起来再也不睡眠（实测缺陷）。 */
                continue;
            }

            if (s_wake_was_timer) {
                /* 定时器唤醒（周期刷新时钟）：默认不重连、保持断开，
                 * 留刷新窗口后由下一轮循环立即重新睡眠。时钟标定与所有
                 * 已注册网络服务共用一个联网窗口——任一到期才重连一次，
                 * 窗口内统一完成，尽量减少 WiFi 重连次数以省电。若窗口内
                 * 发生用户操作（触摸/按键），则升级为用户唤醒：重连并
                 * 保持唤醒，避免"刚唤醒刷新完又立刻睡、忽略用户操作"。 */
                const uint64_t activity_at_wake = espaperplay_input_get_last_activity_ms();
                espaperplay_sleep_refresh_service_t
                    due_services[ESPAPERPLAY_POWER_MAX_SLEEP_REFRESH_SERVICES];
                const uint64_t wake_now_ms = (uint64_t)(esp_timer_get_time() / 1000);
                const bool cal_due = espaperplay_clock_is_calibration_due() &&
                                     power_network_window_allowed(wake_now_ms);
                const size_t due_count = power_collect_due_refresh_services(
                    wake_now_ms, due_services, ESPAPERPLAY_POWER_MAX_SLEEP_REFRESH_SERVICES);
                bool connected = false;

                if (cal_due || due_count > 0) {
                    ESP_LOGI(TAG, "timer wake: reconnecting (clock_cal=%d, services=%u)", cal_due,
                             (unsigned)due_count);
                    espaperplay_wifi_resume_after_wake(true);
                    connected = power_wait_wifi_connected();
                    portENTER_CRITICAL(&s_sleep_refresh_mux);
                    s_last_network_wake_ms = wake_now_ms;
                    portEXIT_CRITICAL(&s_sleep_refresh_mux);
                    if (!connected) {
                        ESP_LOGW(TAG, "timer wake: WiFi connection timeout; retry next window");
                        power_reschedule_refresh_services(due_services, due_count, wake_now_ms);
                    }
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
                if (connected) {
                    /* 同一 WiFi 窗口先并发触发全部到期服务，再逐个等待；TLS
                     * 建连可相互重叠，且不会为每个服务重复唤醒 WiFi。 */
                    for (size_t i = 0; i < due_count; i++) {
                        ESP_LOGI(TAG, "sleep refresh dispatch: %s", due_services[i].name);
                        due_services[i].request_refresh();
                    }
                    for (size_t i = 0; i < due_count; i++) {
                        if (due_services[i].wait_refresh_done != NULL &&
                            !due_services[i].wait_refresh_done(
                                due_services[i].refresh_timeout_ms)) {
                            ESP_LOGW(TAG, "sleep refresh timeout: %s (%u ms)", due_services[i].name,
                                     (unsigned)due_services[i].refresh_timeout_ms);
                        }
                    }
                }
                vTaskDelay(pdMS_TO_TICKS(ESPAPERPLAY_POWER_REFRESH_GRACE_MS));
                /* 刷新窗口内若有用户操作或 Web 心跳到达（借重连窗口），均升级
                 * 为保持唤醒：维持连接并重置活动计时，避免刚刷新完又睡。 */
                const bool user_active =
                    espaperplay_input_get_last_activity_ms() > activity_at_wake;
                if (user_active || power_ext_activity_fresh()) {
                    /* 升级为用户唤醒：保持连接。 */
                    if (!connected) {
                        espaperplay_wifi_resume_after_wake(true);
                    }
                    /* 真实用户操作意味着设备即将回到交互状态：睡眠图标必须
                     * 同步清除，屏保也要退出、重建主界面，否则设备保持唤醒
                     * 而图标/屏保滞留在屏上（实测缺陷；纯 Web 心跳升级保持
                     * 图标与屏保，屏幕无需多一次刷新）。 */
                    if (user_active) {
                        espaperplay_input_set_sleep_indicator(false);
                        espaperplay_ui_screensaver_dismiss();
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
                 * 定时器唤醒不清除，图标在睡眠期间持续显示。
                 * 屏保恢复：睡眠期间栈顶为屏保页时弹出、重建主界面（非屏保
                 * 入睡时为无操作；与屏保页 on_touch/on_key 自退出幂等互兜底）。 */
                espaperplay_input_set_sleep_indicator(false);
                espaperplay_ui_screensaver_dismiss();
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
    /* 先验证唤醒配置；失败时尚未改变外设供电或网络状态。 */
    esp_err_t ret = esp_sleep_enable_ext0_wakeup(ESPAPERPLAY_PIN_KEY_BOOT, 0);
    if (ret != ESP_OK) return ret;
    ESP_LOGW(TAG, "entering deep sleep (power-off): peripherals rail down");
    (void)espaperplay_audio_stop();

    /* 与浅睡眠一样冻结刷新管线，防止 EPD 睡眠后又排入新帧。 */
    espaperplay_gui_set_frozen(true);
    ret = espaperplay_epd_sleep();
    if (ret != ESP_OK) {
        espaperplay_gui_set_frozen(false);
        (void)esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_EXT0);
        return ret;
    }
    espaperplay_wifi_suspend_for_sleep();
    espaperplay_diaglog_write("PWR", "deep sleep (power-off): rail off, wake=BOOT GPIO%d",
                              ESPAPERPLAY_PIN_KEY_BOOT);
    /* 关机仅允许 BOOT 唤醒，不沿用浅睡眠的分钟定时器及触摸 GPIO 唤醒。 */
    (void)esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_TIMER);
    (void)esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_GPIO);
    espaperplay_board_peripheral_rail_set(false);
    esp_deep_sleep_start();
    return ESP_OK;
}
