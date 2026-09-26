/*
 * SPDX-FileCopyrightText: 2026 ESPaperPlay Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @file espaperplay_audio.h
 * @brief 板载音频子系统驱动（麦克风 + 扬声器 + 音频电源轨）。
 *
 * 对应原理图 AUDIO 页：
 *   - 麦克风 U4 ICS-43434（I2S 数字 MEMS，LR 接地=左声道）；
 *   - 功放  U5 MAX98357A（I2S 输入，内部混单声道，GAIN_SLOT 接地=12dB）；
 *   - 电源  U6 TLV62569 降压产生 AUDIO_3V3（EN=AUDIO_PWR_EN/GPIO4，
 *           复位默认下拉=关），MIC_3V3 经磁珠自 AUDIO_3V3 派生；
 *   - 功放 SD_MODE# 经 2K 电阻接 GPIO6，板上 100K 下拉（默认关断防爆音）。
 *
 * BCLK / WS 两线被麦克风与功放共享，ESP32-S3 作唯一 I2S 主机：I2S 控制器
 * 以全双工方式分配 TX（主机，驱动 BCLK/WS + DOUT->功放）与 RX（从机，
 * DIN<-麦克风）两个通道，共用同一组时钟引脚。
 *
 * 生命周期：init() 仅落 GPIO 静态电平（音频轨关、功放关断）；start() 开轨
 * 并建立 I2S 流；stop() 先静音再断轨。所有读写 API 仅在 start..stop 窗口
 * 内有效。sample 交付约定：扬声器写入 / 麦克风读取均为 **int16 单声道**
 * PCM，驱动内部完成立体声槽位扩展 / 提取与音量缩放。
 */

/** 扬声器音量百分比的合法范围。 */
#define ESPAPERPLAY_AUDIO_VOLUME_MIN 0
#define ESPAPERPLAY_AUDIO_VOLUME_MAX 100

/**
 * @brief 初始化音频子系统（不建流、不上电）。
 *
 * 配置 AUDIO_PWR_EN / AMP_SD_MODE 为输出并保持关闭电平（轨关断、功放
 * 静音），并记录初始音量（默认 40%）。幂等：重复调用不改变正在运行的流或音量。
 *
 * @return 成功返回 ESP_OK，否则返回错误码。
 */
esp_err_t espaperplay_audio_init(void);

/**
 * @brief 上电音频轨并建立全双工 I2S 流。
 *
 * 时序：开 AUDIO_3V3 轨 -> 等降压稳定 -> 建立并使能 I2S TX+RX -> 解除
 * 功放关断（SD_MODE# 拉高）-> 等麦克风时钟唤醒（ICS-43434 需 32768 个
 * SCK 周期，16kHz 下约 32ms）后返回；调用方应按具体采样率丢弃启动过渡数据。
 *
 * @param[in] sample_rate 采样率（Hz，典型 16000；I2S 槽位固定 32-bit
 *                        立体声，BCLK = 64 × 采样率）。
 *
 * @return 成功返回 ESP_OK；已建流返回 ESP_ERR_INVALID_STATE（先调
 *         espaperplay_audio_stop()）；参数非法 / 资源不足返回相应错误码。
 */
esp_err_t espaperplay_audio_start(uint32_t sample_rate);

/**
 * @brief 停止 I2S 流并下电音频轨。
 *
 * 时序：拉低 SD_MODE# 静音 -> 等输出收敛 -> 停止并释放 I2S 通道 ->
 * 关断 AUDIO_3V3。未建流时为无操作（仍返回 ESP_OK），供睡眠 / 关机
 * 路径无条件调用。
 *
 * @return 成功返回 ESP_OK，否则返回错误码。
 */
esp_err_t espaperplay_audio_stop(void);

/** 音频流是否处于活跃状态（start..stop 窗口内）。电源管理据此推迟睡眠。 */
bool espaperplay_audio_is_active(void);

/**
 * @brief 向扬声器写入一段 int16 单声道 PCM（阻塞直至全部写入或超时）。
 *
 * 内部按当前音量缩放并复制到左右两个 32-bit 槽位（MAX98357A 混单声道
 * 输出）。数据先进入 DMA 缓冲；持续写入速率受采样率节流。
 *
 * @param[in] samples PCM 数据（单声道、int16）。
 * @param[in] count   采样数（非字节数）。
 * @param[in] timeout_ms 最长阻塞时间（毫秒；0 表示不限时）。
 *
 * @return 成功返回 ESP_OK；未建流返回 ESP_ERR_INVALID_STATE；超时或
 *         参数非法返回相应错误码。
 */
esp_err_t espaperplay_audio_speaker_write(const int16_t *samples, size_t count,
                                          uint32_t timeout_ms);

/**
 * @brief 从麦克风读取一段 int16 单声道 PCM（阻塞直至读满或超时）。
 *
 * 麦克风数据仅出现在左声道（LR 接地）；本函数读取立体声帧后提取左
 * 声道并把 24-bit（32-bit 槽内）样本右移为 int16。
 *
 * @param[out] samples  接收缓冲（单声道、int16）。
 * @param[in]  count    期望采样数（非字节数）。
 * @param[in]  timeout_ms 最长阻塞时间（毫秒；0 表示不限时）。
 *
 * @return 成功返回 ESP_OK；未建流返回 ESP_ERR_INVALID_STATE；超时或
 *         参数非法返回相应错误码。
 */
esp_err_t espaperplay_audio_mic_read(int16_t *samples, size_t count, uint32_t timeout_ms);

/**
 * @brief 设置扬声器音量（软件增益，0-100%）。
 *
 * MAX98357A 的硬件增益由 GAIN_SLOT 电阻固定（板上接地=12dB），音量
 * 仅在扬声器写入路径上做软件缩放。立即对后续写入生效，不中断在途流。
 *
 * @param[in] percent 音量百分比（0=静音）。
 *
 * @return ESP_OK；越界返回 ESP_ERR_INVALID_ARG。
 */
esp_err_t espaperplay_audio_set_volume_percent(int percent);

/** 获取当前扬声器音量百分比（未 init 时返回默认值）。 */
int espaperplay_audio_get_volume_percent(void);

#ifdef __cplusplus
}
#endif
