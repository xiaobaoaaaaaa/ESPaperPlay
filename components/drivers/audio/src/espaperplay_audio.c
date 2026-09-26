/*
 * SPDX-FileCopyrightText: 2026 ESPaperPlay Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#include <string.h>
#include <stdatomic.h>
#include "esp_timer.h"

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "driver/gpio.h"
#include "driver/i2s_std.h"

#include "esp_check.h"
#include "esp_log.h"

#include "espaperplay_audio.h"
#include "espaperplay_config.h"
#include "espaperplay_diaglog.h"

static const char *TAG = "ESPaperPlay_AUDIO";

/** 上电默认音量（百分比；持久化值由上层 init 后回填）。 */
#define ESPAPERPLAY_AUDIO_DEFAULT_VOLUME 40

/* 全双工流状态：start() 建立后有效，stop() 清空。 */
static i2s_chan_handle_t s_tx_chan; /*!< I2S TX（主机：驱动 BCLK/WS，数据->功放） */
static i2s_chan_handle_t s_rx_chan; /*!< I2S RX（从机：共享 BCLK/WS，数据<-麦克风） */
static uint32_t s_sample_rate;
static atomic_bool s_active;
static bool s_initialized;
static bool s_amp_enabled;

static atomic_int s_volume_percent = ESPAPERPLAY_AUDIO_DEFAULT_VOLUME;

/* 读写 / 上下电共用一把互斥锁：防止并发的 start/stop 与写读交错。 */
static SemaphoreHandle_t s_lock;

/** 音频轨与功放的上电/下电时序（须持 s_lock 调用）。 */
static void audio_amp_set_enabled(bool enable) {
    if (s_amp_enabled == enable) {
        return;
    }
    /* SD_MODE# 高=解除关断；先开轨后解静音、先静音后关轨，避免爆音。 */
    gpio_set_level(ESPAPERPLAY_PIN_AMP_SD_MODE, enable ? 1 : 0);
    s_amp_enabled = enable;
}

static void audio_rail_set_enabled(bool enable) {
    gpio_set_level(ESPAPERPLAY_PIN_AUDIO_PWR_EN, enable ? 1 : 0);
}

esp_err_t espaperplay_audio_init(void) {
    if (s_lock == NULL) {
        s_lock = xSemaphoreCreateMutex();
        if (s_lock == NULL) {
            return ESP_ERR_NO_MEM;
        }
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (s_initialized) {
        xSemaphoreGive(s_lock);
        return ESP_OK;
    }

    /* GPIO 静态电平：音频轨关断（R31 下拉同态）、功放关断（R33 下拉同态）。
     * 上电即调用也安全：即使 start() 从未发生，引脚也处于无漏电默认态。 */
    gpio_config_t out_cfg = {
        .pin_bit_mask = BIT64(ESPAPERPLAY_PIN_AUDIO_PWR_EN) | BIT64(ESPAPERPLAY_PIN_AMP_SD_MODE),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    esp_err_t ret = gpio_config(&out_cfg);
    if (ret != ESP_OK) {
        xSemaphoreGive(s_lock);
        return ret;
    }
    audio_rail_set_enabled(false);
    s_amp_enabled = false;
    (void)gpio_set_level(ESPAPERPLAY_PIN_AMP_SD_MODE, 0);

    s_initialized = true;
    xSemaphoreGive(s_lock);
    ESP_LOGI(TAG, "audio driver ready (rail default off, volume=%d%%)", s_volume_percent);
    return ESP_OK;
}

esp_err_t espaperplay_audio_start(uint32_t sample_rate) {
    if (sample_rate == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_lock == NULL || !s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (s_active) {
        xSemaphoreGive(s_lock);
        return ESP_ERR_INVALID_STATE;
    }

    bool tx_enabled = false;
    bool rx_enabled = false;

    /* 1. 音频轨先上电：TLV62569 软启动 + MIC_3V3 经磁珠派生均需时间。 */
    audio_rail_set_enabled(true);
    vTaskDelay(pdMS_TO_TICKS(ESPAPERPLAY_AUDIO_RAIL_STABILIZE_MS));

    /* 2. 建立全双工 I2S 通道对（同一控制器：TX 主机出时钟，RX 从机收数据）。
     * 槽位固定 32-bit 立体声 => BCLK = 64 × fs，满足 ICS-43434（SCK 32-64fs）
     * 与 MAX98357A（支持 16-32bit 数据）两家的共同约束。 */
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    chan_cfg.auto_clear = true; /* TX FIFO 空时补零：流间隙不循环末样本 */
    esp_err_t ret = i2s_new_channel(&chan_cfg, &s_tx_chan, &s_rx_chan);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "i2s channel alloc failed: %s", esp_err_to_name(ret));
        goto fail;
    }

    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(sample_rate),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_32BIT,
                                                        I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = (gpio_num_t)ESPAPERPLAY_PIN_I2S_BCLK,
            .ws = (gpio_num_t)ESPAPERPLAY_PIN_I2S_WS,
            .dout = (gpio_num_t)ESPAPERPLAY_PIN_SPK_DIN,
            .din = (gpio_num_t)ESPAPERPLAY_PIN_MIC_DOUT,
            .invert_flags = { false, false, false },
        },
    };
    /* TX 先初始化（主机配置生效），RX 复用同一组时钟引脚自动降为从机。 */
    ret = i2s_channel_init_std_mode(s_tx_chan, &std_cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "i2s tx init failed: %s", esp_err_to_name(ret));
        goto fail;
    }
    std_cfg.gpio_cfg.dout = I2S_GPIO_UNUSED; /* RX 不驱动数据出 */
    ret = i2s_channel_init_std_mode(s_rx_chan, &std_cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "i2s rx init failed: %s", esp_err_to_name(ret));
        goto fail;
    }

    ret = i2s_channel_enable(s_tx_chan);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "i2s tx enable failed: %s", esp_err_to_name(ret));
        goto fail;
    }
    tx_enabled = true;
    ret = i2s_channel_enable(s_rx_chan);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "i2s rx enable failed: %s", esp_err_to_name(ret));
        goto fail;
    }

    rx_enabled = true;

    /* 3. 时钟已稳定后解除功放关断；麦克风等 ICS-43434 时钟唤醒
     * （32768 个 SCK 周期 ≈ 32ms @64×fs/16kHz）后再宣告流就绪，
     * DMA 中可能已有启动过渡数据，调用方仍需按采样率预热丢弃。 */
    audio_amp_set_enabled(true);
    vTaskDelay(pdMS_TO_TICKS(ESPAPERPLAY_AUDIO_MIC_WARMUP_MS));

    s_sample_rate = sample_rate;
    s_active = true;
    xSemaphoreGive(s_lock);
    espaperplay_diaglog_write("AUDIO", "stream start: %u Hz, volume=%d%%",
                              (unsigned)sample_rate, s_volume_percent);
    ESP_LOGI(TAG, "full-duplex stream up: %u Hz (BCLK=%u Hz), amp unmuted", (unsigned)sample_rate,
             (unsigned)(sample_rate * 64));
    return ESP_OK;

fail:
    if (rx_enabled) (void)i2s_channel_disable(s_rx_chan);
    if (tx_enabled) (void)i2s_channel_disable(s_tx_chan);
    if (s_tx_chan != NULL) {
        (void)i2s_del_channel(s_tx_chan);
        s_tx_chan = NULL;
    }
    if (s_rx_chan != NULL) {
        (void)i2s_del_channel(s_rx_chan);
        s_rx_chan = NULL;
    }
    audio_amp_set_enabled(false);
    audio_rail_set_enabled(false);
    xSemaphoreGive(s_lock);
    return ret;
}

esp_err_t espaperplay_audio_stop(void) {
    if (s_lock == NULL) {
        return ESP_OK; /* 从未 init：无操作 */
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (!s_active) {
        xSemaphoreGive(s_lock);
        return ESP_OK;
    }

    /* 先静音再停流断轨：SD_MODE# 拉低使功放输出立即 Hi-Z。 */
    audio_amp_set_enabled(false);
    vTaskDelay(pdMS_TO_TICKS(ESPAPERPLAY_AUDIO_AMP_MUTE_HOLD_MS));

    (void)i2s_channel_disable(s_rx_chan);
    (void)i2s_channel_disable(s_tx_chan);
    (void)i2s_del_channel(s_rx_chan);
    (void)i2s_del_channel(s_tx_chan);
    s_rx_chan = NULL;
    s_tx_chan = NULL;

    audio_rail_set_enabled(false);
    s_active = false;
    xSemaphoreGive(s_lock);
    espaperplay_diaglog_write("AUDIO", "stream stop, rail off");
    ESP_LOGI(TAG, "stream stopped, audio rail off");
    return ESP_OK;
}

bool espaperplay_audio_is_active(void) { return s_active; }

esp_err_t espaperplay_audio_speaker_write(const int16_t *samples, size_t count,
                                          uint32_t timeout_ms) {
    if (samples == NULL || count == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_lock == NULL || !s_active) {
        return ESP_ERR_INVALID_STATE;
    }

    /* int16 单声道 -> 双 32-bit 槽位（同值复制），就地按音量缩放。
     * 调用方栈深度不可控（LVGL / 服务任务都可能调用），缓冲取静态分块
     * 上限，按 256 采样流式推进，同时避免长时间独占 I2S DMA 描述符。 */
    enum { CHUNK = 256 };
    static int32_t s_frame[CHUNK * 2];

    const int64_t start_us = esp_timer_get_time();
    const TickType_t wait_ticks = timeout_ms == 0 ? portMAX_DELAY :
        (TickType_t)(((uint64_t)timeout_ms * configTICK_RATE_HZ + 999) / 1000);
    if (xSemaphoreTake(s_lock, wait_ticks) != pdTRUE) return ESP_ERR_TIMEOUT;
    if (!s_active) {
        xSemaphoreGive(s_lock);
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t ret = ESP_OK;

    for (size_t off = 0; off < count && ret == ESP_OK; off += CHUNK) {
        /* 每个分块传入剩余毫秒预算；I2S 的等待粒度由底层 DMA 队列决定。 */
        uint32_t remaining_ms = UINT32_MAX;
        if (timeout_ms != 0) {
            const uint64_t elapsed_ms = (esp_timer_get_time() - start_us) / 1000;
            if (elapsed_ms >= timeout_ms) { ret = ESP_ERR_TIMEOUT; break; }
            remaining_ms = timeout_ms - (uint32_t)elapsed_ms;
        }

        const size_t n = (count - off < CHUNK) ? (count - off) : CHUNK;
        for (size_t i = 0; i < n; i++) {
            /* 音量缩放后的样本左对齐到 32-bit 槽（Philips MSB-first）。
             * int16 × percent / 100 结果仍在 int16 范围，乘以 65536 对负数也有定义且不溢出。 */
            const int32_t scaled = ((int32_t)samples[off + i] * s_volume_percent) / 100;
            s_frame[i * 2] = scaled * 65536;     /* 左槽 */
            s_frame[i * 2 + 1] = scaled * 65536; /* 右槽（MAX98357A 混单声道） */
        }
        size_t bytes_written = 0;
        ret = i2s_channel_write(s_tx_chan, s_frame, n * 2 * sizeof(int32_t), &bytes_written,
                                remaining_ms);
        /* 整块写入成功才推进；部分写入视为超时中断（调用方重试整段或放弃，
         * 语义简单可预测）。 */
        if (ret == ESP_OK && bytes_written < n * 2 * sizeof(int32_t)) {
            ret = ESP_ERR_TIMEOUT;
        }
    }

    xSemaphoreGive(s_lock);
    return ret;
}

esp_err_t espaperplay_audio_mic_read(int16_t *samples, size_t count, uint32_t timeout_ms) {
    if (samples == NULL || count == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_lock == NULL || !s_active) {
        return ESP_ERR_INVALID_STATE;
    }

    enum { CHUNK = 256 };
    static int32_t s_frame[CHUNK * 2];

    const int64_t start_us = esp_timer_get_time();
    const TickType_t wait_ticks = timeout_ms == 0 ? portMAX_DELAY :
        (TickType_t)(((uint64_t)timeout_ms * configTICK_RATE_HZ + 999) / 1000);
    if (xSemaphoreTake(s_lock, wait_ticks) != pdTRUE) return ESP_ERR_TIMEOUT;
    if (!s_active) {
        xSemaphoreGive(s_lock);
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t ret = ESP_OK;

    for (size_t off = 0; off < count && ret == ESP_OK; off += CHUNK) {
        /* 同 speaker_write：分块只消耗剩余超时预算。 */
        uint32_t remaining_ms = UINT32_MAX;
        if (timeout_ms != 0) {
            const uint64_t elapsed_ms = (esp_timer_get_time() - start_us) / 1000;
            if (elapsed_ms >= timeout_ms) { ret = ESP_ERR_TIMEOUT; break; }
            remaining_ms = timeout_ms - (uint32_t)elapsed_ms;
        }

        const size_t n = (count - off < CHUNK) ? (count - off) : CHUNK;
        size_t bytes_read = 0;
        ret = i2s_channel_read(s_rx_chan, s_frame, n * 2 * sizeof(int32_t), &bytes_read,
                               remaining_ms);
        if (ret != ESP_OK) {
            break;
        }
        const size_t frames = bytes_read / (2 * sizeof(int32_t));
        for (size_t i = 0; i < frames && i < n; i++) {
            /* 左声道（LR 接地）携麦克风数据；24-bit 样本 MSB 对齐在
             * 32-bit 槽内，右移 16 位得到 int16 幅度。 */
            samples[off + i] = (int16_t)(s_frame[i * 2] >> 16);
        }
        if (frames < n) {
            /* 部分数据不满足读满契约，向调用方报告超时。 */
            ret = ESP_ERR_TIMEOUT;
            break;
        }
    }

    xSemaphoreGive(s_lock);
    return ret;
}

esp_err_t espaperplay_audio_set_volume_percent(int percent) {
    if (percent < ESPAPERPLAY_AUDIO_VOLUME_MIN || percent > ESPAPERPLAY_AUDIO_VOLUME_MAX) {
        return ESP_ERR_INVALID_ARG;
    }
    s_volume_percent = percent;
    return ESP_OK;
}

int espaperplay_audio_get_volume_percent(void) { return s_volume_percent; }
