/*
 * SPDX-FileCopyrightText: 2026 ESPaperPlay Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @file espaperplay_reader_blocks.h
 * @brief 文档统一块模型（TXT / EPUB 共用，阅读视图按块分页渲染）。
 *
 * 任意文档都被归一为「章节 → 块序列」：文本块指向章节文本 blob 的
 * (off, len) 区间（内部可含 '\n' 强制换行），图片块持有章节内图片 id。
 * 块 flags 携带样式（标题层级 / 粗体 / 斜体 / 引用），供渲染层选择字体。
 */

/** 块 flags：粗体（全文块以粗体渲染）。 */
#define ESPAPERPLAY_READER_BLK_BOLD (1u << 0)
/** 块 flags：斜体。 */
#define ESPAPERPLAY_READER_BLK_ITALIC (1u << 1)
/** 块 flags：引用段（缩进渲染）。 */
#define ESPAPERPLAY_READER_BLK_QUOTE (1u << 2)
/** 块 flags：居中对齐（来自 CSS / align 属性）。 */
#define ESPAPERPLAY_READER_BLK_CENTER (1u << 3)
/** 块 flags：右对齐。 */
#define ESPAPERPLAY_READER_BLK_RIGHT (1u << 7)
/** 块 flags：首行缩进已由解析器插入（无需渲染层处理）。 */
#define ESPAPERPLAY_READER_BLK_INDENTED (1u << 8)
/* 注意：bit4-6 保留给标题层级（ESPAPERPLAY_READER_BLK_HEAD_*），样式位不得占用。 */

/** 标题层级取值宏：0=正文，1..6 对应 h1..h6。 */
#define ESPAPERPLAY_READER_BLK_HEAD_SHIFT (4)
#define ESPAPERPLAY_READER_BLK_HEAD_MASK (0x7u << ESPAPERPLAY_READER_BLK_HEAD_SHIFT)
#define ESPAPERPLAY_READER_BLK_HEAD_LEVEL(flags) \
    ((int)(((flags) >> ESPAPERPLAY_READER_BLK_HEAD_SHIFT) & 0x7u))

/** 文档块。 */
typedef struct {
    uint32_t off;   /*!< 文本块：章节文本 blob 内的起始偏移 */
    uint32_t len;   /*!< 文本块：字节长度（不含 NUL；内部 '\n' 为强制换行） */
    uint16_t flags; /*!< ESPAPERPLAY_READER_BLK_* 样式位 */
    int32_t image;  /*!< >=0：图片块（章节内图片 id）；<0：文本块 */
} espaperplay_reader_block_t;

#ifdef __cplusplus
}
#endif
