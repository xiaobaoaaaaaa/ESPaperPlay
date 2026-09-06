/*
 * SPDX-FileCopyrightText: 2026 ESPaperPlay Contributors
 *
 * SPDX-License-Identifier: MIT
 */

/*
 * ESPaperPlay - SD 卡二进制缓存通用读写
 *
 * 此前 reader 的章节/封面/分页缓存各自实现同一套范式：
 * 「magic+版本头 → 读校验，损坏即删；写临时文件 → rename 原子替换，失败删临时」。
 * 本模块把打开/目录确保/临时文件/原子替换/损坏清理收口为单一实现，
 * 各缓存的业务差异（头部字段、载荷布局、负缓存、容量回报）留在回调里。
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

/** 递增/变更 VERSION 使旧缓存失效；fingerprint 含 mtime/size，书变更自动换名失效。 */
#define ESPCACHE_TMP_SUFFIX ".tmp"

/**
 * @brief 文件指纹 token：FNV-1a(路径) ^ st_mtime ^ st_size。
 *
 * stat 失败时退化为纯路径哈希（调用方通常已确认文件存在）。
 * 同一文件在任何模块算出的 token 一致，作缓存文件名 / 键均安全。
 */
uint32_t espcache_file_token(const char *abs_path);

/** 读回调结果。 */
typedef enum {
    ESPCACHE_MISS = 0, /*!< 文件打不开（正常未命中，不动文件） */
    ESPCACHE_OK,       /*!< 命中且校验通过（回调已取走数据） */
    ESPCACHE_CORRUPT,  /*!< 校验失败（模块已删除文件，调用方重建） */
    ESPCACHE_KEEP,     /*!< 内容有效但本次不可用（如表容量不足；保留文件） */
} espcache_result_t;

/**
 * @brief 读回调：读头/校验/读载荷都在这里做。
 *
 * 文件已由模块打开并定位到头部之后；返回非 OK 时模块负责 fclose
 * （CORRUPT 时再 remove）。
 */
typedef espcache_result_t (*espcache_read_fn)(FILE *f, void *ud);

/**
 * @brief 读缓存：打开 path 执行 @p fn，按结果清理。
 * 失败路径（CORRUPT）自动删除损坏文件，调用方直接重建即可。
 */
espcache_result_t espcache_read(const char *path, espcache_read_fn fn, void *ud);

/**
 * @brief 写回调：写头部与载荷；返回 false 触发回滚（临时文件被删除）。
 * 文件已由模块以 "wb" 打开。
 */
typedef bool (*espcache_write_fn)(FILE *f, void *ud);

/**
 * @brief 原子写缓存：确保父目录 → 写 path.tmp → 成功 rename 原子替换 /
 * 失败删除临时文件。绝不产生半写缓存。
 * @return true=已落盘（rename 完成）。
 */
bool espcache_write_atomic(const char *path, espcache_write_fn fn, void *ud);

#ifdef __cplusplus
}
#endif
