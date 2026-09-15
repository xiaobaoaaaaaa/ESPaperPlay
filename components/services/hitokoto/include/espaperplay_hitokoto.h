/*
 * SPDX-FileCopyrightText: 2026 ESPaperPlay Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @file espaperplay_hitokoto.h
 * @brief 一言（Hitokoto）服务。
 *
 * 基于 Hitokoto API（https://developer.hitokoto.cn/sentence/，接口
 * https://v1.hitokoto.cn/）获取一句话（句子 + 出处 + 作者 + 分类）。
 * 接口免鉴权、无需配置，服务启动后等待 STA 联网即拉取第一条，此后按
 * 周期（默认 1 小时）刷新到内存快照；失败按退避重试（尚无内容时 15 秒
 * 快速收敛，已有内容 60 秒退避）。
 *
 * 持久化：每次成功获取把快照写入 NVS，启动时先恢复——设备昼夜睡眠、
 * 网络窗口碎片化，避免"取到第一条之前就已入睡"导致展示长期回退问候语。
 *
 * 展示层经 espaperplay_hitokoto_get() 读取快照拷贝（带 dedup 字段 id，
 * 内容变化才需要重绘 EPD）；"换一句"交互经 espaperplay_hitokoto_request_refresh()
 * 唤醒后台任务立即拉取，随后轮询 get() 直到 id 变化。
 */

/** 句子文本缓冲上限（字节；接口 max_length 参数限制句长 32 字，UTF-8 最多 4 字节/字）。 */
#define ESPAPERPLAY_HITOKOTO_TEXT_MAX 128
/** 出处缓冲上限（字节，超长截断）。 */
#define ESPAPERPLAY_HITOKOTO_FROM_MAX 96
/** 作者缓冲上限（字节，超长截断）。 */
#define ESPAPERPLAY_HITOKOTO_WHO_MAX 64
/** 句子分类代码缓冲上限（单字母 + NUL）。 */
#define ESPAPERPLAY_HITOKOTO_TYPE_MAX 2

/** 刷新周期默认值（毫秒）：1 小时。 */
#define ESPAPERPLAY_HITOKOTO_REFRESH_INTERVAL_MS (60 * 60 * 1000)
/** 失败重试等待（毫秒）：避免持续失败时频繁请求。 */
#define ESPAPERPLAY_HITOKOTO_RETRY_INTERVAL_MS (60 * 1000)

/** 一条一言（快照拷贝用；字符串均以 '\0' 结尾，缺失字段为空串）。 */
typedef struct {
    bool valid;                                  /*!< 是否已成功获取过（false 时其余字段无意义） */
    int32_t id;                                  /*!< 句子 ID（同句去重 / 判断"换一句"生效） */
    char text[ESPAPERPLAY_HITOKOTO_TEXT_MAX];    /*!< 句子内容 */
    char from[ESPAPERPLAY_HITOKOTO_FROM_MAX];    /*!< 出处（《作品》/ 说话场合等） */
    char from_who[ESPAPERPLAY_HITOKOTO_WHO_MAX]; /*!< 作者（可为空） */
    char type[ESPAPERPLAY_HITOKOTO_TYPE_MAX];    /*!< 分类代码（a 动画 / d 文学 / i 诗词…） */
} espaperplay_hitokoto_t;

/**
 * @brief 启动一言后台刷新任务（幂等）。
 *
 * 任务等待 STA 联网（最多 60 秒）后拉取第一条，此后按周期刷新；
 * 断网时空转等待，恢复联网后自动续取。
 *
 * @return ESP_OK；任务创建失败返回 ESP_ERR_NO_MEM。
 */
esp_err_t espaperplay_hitokoto_start(void);

/**
 * @brief 获取当前一言快照（拷贝；无数据时 valid=false，其余字段清零）。
 *
 * @param out 输出快照（非空；结构小可直接栈上分配）。
 * @return ESP_OK；参数非法返回 ESP_ERR_INVALID_ARG。
 */
esp_err_t espaperplay_hitokoto_get(espaperplay_hitokoto_t *out);

/**
 * @brief 请求立即刷新（异步，"换一句"）。
 *
 * 唤醒后台任务尽快拉取新句子；任务未运行时为空操作。刷新结果经
 * espaperplay_hitokoto_get() 轮询获取（比较 id 是否变化）。
 */
void espaperplay_hitokoto_request_refresh(void);

/** @brief 当前快照是否已到刷新周期（含失败退避判断）。 */
bool espaperplay_hitokoto_is_refresh_due(void);

/** @brief 等待最近一次后台刷新完成，供睡眠联网窗口同步收尾。 */
bool espaperplay_hitokoto_wait_refresh_done(uint32_t timeout_ms);

/**
 * @brief 分类代码转中文名（"a"->"动画" 等，未知代码返回 NULL）。
 *
 * @param type 分类代码（单字母，如快照中的 type 字段）。
 * @return 中文名（只读静态串）；type 为 NULL/空/未知返回 NULL。
 */
const char *espaperplay_hitokoto_type_name(const char *type);

#ifdef __cplusplus
}
#endif
