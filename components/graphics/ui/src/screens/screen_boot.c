/*
 * SPDX-FileCopyrightText: 2026 ESPaperPlay Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/portmacro.h"

#include "esp_log.h"
#include "esp_timer.h"

#include "espaperplay_config.h"
#include "espaperplay_fonts.h"
#include "espaperplay_gui_lv.h"
#include "espaperplay_system.h"
#include "espaperplay_ui.h"

#include "lvgl.h"

/* ====================================================================
 * 开机日志屏
 * ====================================================================
 *
 * 双线程并行启动的显示端：显示链路（EPD/GUI/LVGL/字体）初始化完成后
 * 立即构建本屏，主线程的服务初始化步骤经 espaperplay_ui_boot_logf()
 * 把进度逐行追加到屏幕上，让"黑屏等待"变成可见的启动过程。
 *
 * 实现要点：
 *   - 日志行存于环形缓冲（任意线程写入，临界区保护）；LVGL 线程内
 *     重建标签文本时统一读取。屏幕晚于首批日志出现时，构建时一次性
 *     回放已有内容（不丢行）。
 *   - 本屏不经页面栈管理：直接绘制在活动屏幕上（栈尚未启用），后续
 *     espaperplay_ui_page_push(home) 的清屏自然将其移除。
 */

#define BOOT_LOG_MAX_LINES 20 /*!< 环形缓冲容量（超出丢弃最旧行） */
#define BOOT_LOG_LINE_LEN 96  /*!< 单行最大长度（含时间戳前缀与结尾 '\0'） */

#define BOOT_TITLE_FONT_PX 36 /*!< 标题字号 */
#define BOOT_SUB_FONT_PX 16   /*!< 版本/副标题字号 */
#define BOOT_LOG_FONT_PX 16   /*!< 日志字号 */
#define BOOT_MARGIN 24        /*!< 页边距 */

static const char *TAG = "ESPaperPlay_BOOT";

/** 环形日志缓冲（写入方持锁；读取方为 LVGL 线程，同样持锁）。 */
static char s_lines[BOOT_LOG_MAX_LINES][BOOT_LOG_LINE_LEN];
static int s_head = 0; /*!< 下一条写入位置 */
static int s_count = 0;
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;

/** 日志标签（仅 LVGL 线程访问；屏幕被页面清屏移除后置 NULL）。 */
static lv_obj_t *s_log_label = NULL;

/** 追加一行到环形缓冲（满则覆盖最旧）。 */
static void boot_ring_push(const char *line) {
    portENTER_CRITICAL(&s_lock);
    strlcpy(s_lines[s_head], line, BOOT_LOG_LINE_LEN);
    s_head = (s_head + 1) % BOOT_LOG_MAX_LINES;
    if (s_count < BOOT_LOG_MAX_LINES) {
        s_count++;
    }
    portEXIT_CRITICAL(&s_lock);
}

/** LVGL 线程内：把环形缓冲内容重建进日志标签。 */
static void boot_rebuild_label(void) {
    if (s_log_label == NULL) {
        return;
    }
    static char buf[BOOT_LOG_MAX_LINES * BOOT_LOG_LINE_LEN]; /* 仅 LVGL 线程访问 */
    size_t off = 0;
    buf[0] = '\0';
    portENTER_CRITICAL(&s_lock);
    for (int i = 0; i < s_count; i++) {
        const int idx = (s_head - s_count + i + BOOT_LOG_MAX_LINES) % BOOT_LOG_MAX_LINES;
        const int len = strlen(s_lines[idx]);
        if (off + (size_t)len + 1 >= sizeof(buf)) {
            break;
        }
        memcpy(buf + off, s_lines[idx], (size_t)len);
        off += (size_t)len;
        buf[off++] = '\n';
    }
    portEXIT_CRITICAL(&s_lock);
    if (off > 0) {
        buf[off - 1] = '\0'; /* 去掉末尾换行 */
    }
    lv_label_set_text(s_log_label, buf);
}

/** gui_lv_call 回调：追加一行后的标签重建。 */
static void boot_append_cb(void *arg) {
    (void)arg;
    boot_rebuild_label();
}

/** gui_lv_call 回调：构建开机屏。 */
static void boot_build_cb(void *arg) {
    (void)arg;
    lv_obj_t *scr = lv_screen_active();
    lv_obj_clean(scr);
    lv_obj_set_style_bg_color(scr, lv_color_white(), 0);

    const lv_font_t *font_title =
        espaperplay_fonts_load(espaperplay_system_get_config()->selected_font,
                               BOOT_TITLE_FONT_PX, ESPAPERPLAY_FONT_STYLE_NORMAL);
    const lv_font_t *font_sub =
        espaperplay_fonts_load(espaperplay_system_get_config()->selected_font, BOOT_SUB_FONT_PX,
                               ESPAPERPLAY_FONT_STYLE_NORMAL);
    const lv_font_t *font_log = espaperplay_fonts_load(
        espaperplay_system_get_config()->selected_font, BOOT_LOG_FONT_PX,
        ESPAPERPLAY_FONT_STYLE_NORMAL);

    /* 纵向坐标按屏高百分比（基准 480x800 = 72/124/164/196px），矮屏/横屏自适应 */
    const int32_t scr_h = lv_display_get_vertical_resolution(lv_display_get_default());

    lv_obj_t *title = lv_label_create(scr);
    lv_label_set_text(title, ESPAPERPLAY_PROJECT_NAME);
    lv_obj_set_style_text_color(title, lv_color_black(), 0);
    if (font_title != NULL) {
        lv_obj_set_style_text_font(title, font_title, 0);
    }
    lv_obj_set_pos(title, BOOT_MARGIN, scr_h * 9 / 100);

    lv_obj_t *sub = lv_label_create(scr);
    lv_label_set_text(sub, "v" ESPAPERPLAY_VERSION " 正在启动…");
    lv_obj_set_style_text_color(sub, lv_color_black(), 0);
    if (font_sub != NULL) {
        lv_obj_set_style_text_font(sub, font_sub, 0);
    }
    lv_obj_set_pos(sub, BOOT_MARGIN, scr_h * 155 / 1000);

    lv_obj_t *line = lv_obj_create(scr);
    lv_obj_set_size(line, LV_PCT(100), 2);
    lv_obj_set_pos(line, 0, scr_h * 205 / 1000);
    lv_obj_set_style_bg_color(line, lv_color_black(), 0);
    lv_obj_set_style_border_width(line, 0, 0);
    lv_obj_set_style_radius(line, 0, 0);

    s_log_label = lv_label_create(scr);
    lv_obj_set_style_text_color(s_log_label, lv_color_black(), 0);
    if (font_log != NULL) {
        lv_obj_set_style_text_font(s_log_label, font_log, 0);
    }
    const int32_t scr_w = lv_display_get_horizontal_resolution(lv_display_get_default());
    lv_obj_set_width(s_log_label, scr_w - 2 * BOOT_MARGIN);
    lv_obj_set_pos(s_log_label, BOOT_MARGIN, scr_h * 245 / 1000);

    boot_rebuild_label();
}

esp_err_t espaperplay_ui_boot_show(void) {
    return espaperplay_gui_lv_call(boot_build_cb, NULL, 2000);
}

void espaperplay_ui_boot_logf(const char *fmt, ...) {
    char line[BOOT_LOG_LINE_LEN];

    /* 时间戳前缀（自上电起的秒数），便于定位启动耗时。 */
    const float sec = (float)(esp_timer_get_time() / 1000) / 1000.0f;
    int pre = snprintf(line, sizeof(line), "[%6.2fs] ", (double)sec);
    if (pre < 0 || (size_t)pre >= sizeof(line)) {
        return;
    }

    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line + pre, sizeof(line) - (size_t)pre, fmt, ap);
    va_end(ap);

    ESP_LOGI(TAG, "%s", line);
    boot_ring_push(line);

    /* 屏幕已构建则增量重绘；未就绪/超时时仅留在缓冲，构建时统一回放。 */
    (void)espaperplay_gui_lv_call(boot_append_cb, NULL, 200);
}
