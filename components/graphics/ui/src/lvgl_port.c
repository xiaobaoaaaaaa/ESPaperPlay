/*
 * SPDX-FileCopyrightText: 2026 ESPaperPlay Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#include <stdlib.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h" /* xTaskCreateWithCaps（PSRAM 任务栈） */
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"

#include "espaperplay_config.h"
#include "espaperplay_display.h"
#include "espaperplay_gui.h"
#include "espaperplay_gui_lv.h"
#include "espaperplay_ui_touch.h"

#include "lvgl.h"

static const char *TAG = "ESPaperPlay_LVGL";

/** LVGL 日志回调：默认不注册则 LVGL 内部警告/错误全部静默
 * （如 FreeType 的 FT_ERROR_MSG 错误详情），接入 ESP-IDF 日志便于排查。 */
static void espaperplay_lvgl_log_cb(lv_log_level_t level, const char *buf) {
    switch (level) {
    case LV_LOG_LEVEL_ERROR:
        ESP_LOGE(TAG, "LVGL: %s", buf);
        break;
    case LV_LOG_LEVEL_WARN:
        ESP_LOGW(TAG, "LVGL: %s", buf);
        break;
    case LV_LOG_LEVEL_INFO:
        ESP_LOGI(TAG, "LVGL: %s", buf);
        break;
    default:
        ESP_LOGD(TAG, "LVGL: %s", buf);
        break;
    }
}

/* ====================================================================
 * LVGL 移植层：把 LVGL 渲染桥接到渲染后端（espaperplay_gui）
 * ====================================================================
 *
 * 本文件是"LVGL 如何对接 ESPaperPlay 渲染后端"的唯一实现：
 *   - LVGL 初始化（lv_init + tick 源 + display 注册 + 绘制暂存）；
 *   - flush 回调：LVGL 的 RGB565 渲染结果逐块拷入主帧并提交脏区，
 *     周期末触发一次异步刷新（worker 执行真实面板刷新，本任务不阻塞）；
 *   - lv_timer_handler 渲染任务循环。
 * UI 页面（widget 树）不在此文件，见 src/screens/（espaperplay_ui.h）。
 *
 * 屏幕旋转按 LVGL 官方方案在 flush 回调内完成（《Rotation | LVGL Open》
 * 文档）：lv_display_set_rotation 只交换逻辑分辨率，实际旋转在 flush 内
 * 逐像素直接写入主帧（无额外旋转暂存），映射公式与 LVGL 内核的 indev
 * 坐标旋转（lv_display_rotate_point）互为逆变换，物理脏区由
 * lv_display_rotate_area() 换算。indev 坐标由 LVGL 内核自动旋转
 * （indev_pointer_proc 调用 lv_display_rotate_point），因此 read_cb 必须
 * 上报面板物理坐标，不得再手动换算。
 */

#define ESPAPERPLAY_LVGL_BUF_ROWS 60 /* LVGL 绘制暂存行数（1/8 屏），渲染按带分块 */

static lv_display_t *s_lv_disp = NULL; /*!< LVGL 显示句柄 */
static uint8_t *s_lv_buf = NULL;       /*!< LVGL 绘制暂存（RGB565，部分渲染模式） */

static espaperplay_gui_framebuffer_t s_lv_fb; /*!< 主帧描述（start 时获取，flush 回调使用） */

/* 跨线程 UI 操作投递：LVGL 非线程安全（渲染期间跨线程 invalidate 会触发
 * 内部断言死循环），UI 回调经队列排入 gui_lvgl 任务串行执行。 */
typedef struct {
    espaperplay_gui_lv_call_fn_t fn; /*!< 回调（LVGL 线程执行） */
    void *arg;                       /*!< 回调参数 */
    uint32_t seq;                    /*!< 完成序号（need_done 时有效，从 1 递增） */
    bool need_done;                  /*!< true=同步调用，执行完需发完成信号 */
} espaperplay_lv_call_item_t;

static QueueHandle_t s_lv_call_queue = NULL;    /*!< UI 操作投递队列 */
static SemaphoreHandle_t s_lv_call_done = NULL; /*!< 唤醒等待方（完成信号） */
/* 完成序号：同步调用方等待「自己的 seq」被执行完成（替代旧的全局单标志，
 * 避免异步投递（触摸批）与同步调用（按键）混用时完成信号互相污染）。 */
static volatile uint32_t s_lv_call_seq_counter = 0; /*!< 已分配的调用序号 */
static volatile uint32_t s_lv_call_done_seq = 0;    /*!< 最近执行完成的同步调用序号 */

/* 分配策略与渲染后端一致：PSRAM 优先，失败回退内部 RAM。 */
static uint8_t *espaperplay_lvgl_alloc(size_t bytes) {
    uint8_t *p = heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (p == NULL) {
        p = malloc(bytes);
    }
    return p;
}

/**
 * @brief LVGL flush 回调。
 *
 * LVGL 每周期把无效区渲染进暂存并逐块回调：把该块拷入 RGB565 主帧并提交
 * 脏区；周期最后一个区域（lv_display_flush_is_last）时触发一次合并刷新
 * （异步排队，worker 执行真实面板刷新——LVGL 任务不被屏幕刷新阻塞）。
 * 立即 flush_ready 放行，无需等待 e-paper 更新完成。
 *
 * 屏幕旋转（lv_display_set_rotation，test 页双击触发）：逐像素直接旋入
 * 主帧，无需额外旋转暂存。像素映射采用 LVGL 内核的 indev 坐标旋转约定
 * （lv_display_rotate_point 的逆变换：ROT_90 = 设备顺时针 90 度），与
 * 内核自动旋转触摸坐标的公式、物理脏区换算（lv_display_rotate_area）
 * 完全一致——渲染、控件命中、页面轨迹三处坐标统一。
 */
static void espaperplay_lvgl_flush_cb(lv_display_t *disp, const lv_area_t *area, uint8_t *px_map) {
    const uint16_t w = (uint16_t)(area->x2 - area->x1 + 1);
    const uint16_t h = (uint16_t)(area->y2 - area->y1 + 1);

    const lv_display_rotation_t rotation = lv_display_get_rotation(disp);
    if (rotation == LV_DISPLAY_ROTATION_0) {
        /* LVGL RGB565（LV_COLOR_16_SWAP=0，小端）与主帧格式一致，逐行拷贝。 */
        const uint16_t x = (uint16_t)area->x1;
        const uint16_t y = (uint16_t)area->y1;
        for (uint16_t row = 0; row < h; row++) {
            memcpy(s_lv_fb.buffer + (size_t)(y + row) * s_lv_fb.stride + (size_t)x * 2,
                   px_map + (size_t)row * w * 2, (size_t)w * 2);
        }
        espaperplay_gui_submit_area(x, y, w, h);
    } else {
        /* 逻辑脏区 -> 物理脏区（与 LVGL 内核 indev 旋转共用同一约定）。 */
        lv_area_t rotated_area = *area;
        lv_display_rotate_area(disp, &rotated_area);

        /* 逐像素旋转写入主帧：逻辑 (lx, ly) -> 物理 (px, py)，公式为
         * lv_display_rotate_point 的逆变换（hor/ver 为面板物理分辨率）。 */
        const int32_t hor = lv_display_get_original_horizontal_resolution(disp);
        const int32_t ver = lv_display_get_original_vertical_resolution(disp);
        const uint16_t *src = (const uint16_t *)px_map;
        uint16_t *dst = (uint16_t *)s_lv_fb.buffer;
        const uint32_t dst_stride_px = s_lv_fb.stride / 2;

        for (uint16_t row = 0; row < h; row++) {
            for (uint16_t col = 0; col < w; col++) {
                const int32_t lx = area->x1 + col;
                const int32_t ly = area->y1 + row;
                int32_t px, py;
                switch (rotation) {
                case LV_DISPLAY_ROTATION_90: /* 设备顺时针 90° */
                    px = ly;
                    py = ver - 1 - lx;
                    break;
                case LV_DISPLAY_ROTATION_180:
                    px = hor - 1 - lx;
                    py = ver - 1 - ly;
                    break;
                default: /* LV_DISPLAY_ROTATION_270（设备逆时针 90°） */
                    px = hor - 1 - ly;
                    py = lx;
                    break;
                }
                dst[(size_t)py * dst_stride_px + px] = src[(size_t)row * w + col];
            }
        }

        espaperplay_gui_submit_area((uint16_t)rotated_area.x1, (uint16_t)rotated_area.y1,
                                    (uint16_t)lv_area_get_width(&rotated_area),
                                    (uint16_t)lv_area_get_height(&rotated_area));
    }

    if (lv_display_flush_is_last(disp)) {
        espaperplay_gui_flush(); /* 周期末：合并脏区后异步排队 */
    }
    lv_display_flush_ready(disp);
}

/** LVGL tick 源：esp_timer 单调毫秒时钟（1ms 分辨率，uint32 回绕由 LVGL 处理）。 */
static uint32_t espaperplay_lvgl_tick_ms(void) { return (uint32_t)(esp_timer_get_time() / 1000); }

/**
 * LVGL 渲染任务：周期调用 lv_timer_handler（渲染 + 触发 flush 回调链）。
 * 睡眠时长取返回值并钳制：0（定时器恰好到期）退化为 1 tick，
 * LV_NO_TIMER_READY 或异常大值兜底 100ms。注意 CONFIG_FREERTOS_HZ=100，
 * pdMS_TO_TICKS(5) 会截断为 0 tick（vTaskDelay(0) 仅让出一次），切勿用小于
 * 10ms 的固定延时，否则高优先级任务会空转饿死 IDLE 触发任务看门狗。
 */
static void espaperplay_lvgl_task(void *arg) {
    (void)arg;
    ESP_LOGI(TAG, "LVGL task started");
    for (;;) {
        /* 先执行投递的 UI 操作（在 LVGL 线程内、渲染周期之外，安全）。 */
        espaperplay_lv_call_item_t item;
        if (xQueueReceive(s_lv_call_queue, &item, 0) == pdTRUE) {
            if (item.fn != NULL) {
                item.fn(item.arg);
            }
            if (item.need_done) {
                s_lv_call_done_seq = item.seq; /* 先发布序号再给信号量 */
                xSemaphoreGive(s_lv_call_done);
            }
        }

        uint32_t next = lv_timer_handler();
        if (next == 0 || next > 100) {
            next = 100; /* 无定时器就绪（LV_NO_TIMER_READY）或异常值：兜底 */
        }
        vTaskDelay(pdMS_TO_TICKS(next));
    }
}

esp_err_t espaperplay_gui_lv_call(espaperplay_gui_lv_call_fn_t fn, void *arg, uint32_t timeout_ms) {
    if (s_lv_call_queue == NULL || s_lv_call_done == NULL) {
        return ESP_ERR_INVALID_STATE; /* 移植层未启动 */
    }
    const uint32_t seq = ++s_lv_call_seq_counter;
    const espaperplay_lv_call_item_t item = {fn, arg, seq, true};
    if (xQueueSend(s_lv_call_queue, &item, pdMS_TO_TICKS(timeout_ms)) != pdTRUE) {
        return ESP_ERR_TIMEOUT; /* 队列满 */
    }
    /* 等待「自己的 seq」被执行完成（10ms 唤醒粒度轮询）。 */
    uint32_t waited = 0;
    while (s_lv_call_done_seq != seq && waited < timeout_ms) {
        xSemaphoreTake(s_lv_call_done, pdMS_TO_TICKS(10));
        waited += 10;
    }
    if (s_lv_call_done_seq != seq) {
        return ESP_ERR_TIMEOUT;
    }
    return ESP_OK;
}

esp_err_t espaperplay_gui_lv_start(void) {
    if (s_lv_disp != NULL) {
        ESP_LOGW(TAG, "LVGL layer already started");
        return ESP_OK;
    }
    /* 依赖：须在 espaperplay_gui_init() 之后调用（flush 回调使用主帧）。 */
    esp_err_t err = espaperplay_gui_get_framebuffer(&s_lv_fb);
    if (err != ESP_OK) {
        return err; /* 渲染后端未初始化 */
    }

    lv_init();
    /* LVGL 日志接入 ESP-IDF 日志（否则 LVGL/FreeType 内部错误不可见）。 */
    lv_log_register_print_cb(espaperplay_lvgl_log_cb);
    /* LVGL 不自动获取系统时钟：必须注册 tick 源（esp_timer），否则 lv_tick_get()
     * 恒为 0，定时器全部"立即到期"且行为异常（曾导致空转触发任务看门狗）。 */
    lv_tick_set_cb(espaperplay_lvgl_tick_ms);

    s_lv_call_queue = xQueueCreate(4, sizeof(espaperplay_lv_call_item_t));
    s_lv_call_done = xSemaphoreCreateBinary();
    if (s_lv_call_queue == NULL || s_lv_call_done == NULL) {
        ESP_LOGE(TAG, "LVGL call queue create failed");
        return ESP_ERR_NO_MEM;
    }

    const uint16_t disp_w = espaperplay_display_width();
    const uint16_t disp_h = espaperplay_display_height();
    s_lv_disp = lv_display_create(disp_w, disp_h);
    if (s_lv_disp == NULL) {
        ESP_LOGE(TAG, "lv_display_create failed");
        return ESP_ERR_NO_MEM;
    }
    /* 开机默认竖屏：设备顺时针旋转 90°（逻辑分辨率 480x800）。
     * 旋转的像素/脏区/触摸换算全部由 flush 回调与 LVGL 内核处理
     * （见本文件头部注释），此处只需设置初始方向。 */
    lv_display_set_rotation(s_lv_disp, LV_DISPLAY_ROTATION_90);
    s_lv_buf = espaperplay_lvgl_alloc((size_t)disp_w * ESPAPERPLAY_LVGL_BUF_ROWS * 2);
    if (s_lv_buf == NULL) {
        ESP_LOGE(TAG, "LVGL draw buffer alloc failed (%u bytes)",
                 (unsigned)(disp_w * ESPAPERPLAY_LVGL_BUF_ROWS * 2));
        return ESP_ERR_NO_MEM;
    }
    lv_display_set_buffers(s_lv_disp, s_lv_buf, NULL,
                           (uint32_t)disp_w * ESPAPERPLAY_LVGL_BUF_ROWS * 2,
                           LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_flush_cb(s_lv_disp, espaperplay_lvgl_flush_cb);
    /* 背景由屏幕对象样式控制（UI 页面负责设置；主帧初始也为全白）。 */

    /* 触摸指针 indev：LVGL 控件点击/按压由该输入设备驱动（GT911 -> input
     * 队列 -> 分发任务 -> indev 状态，见 lvgl_touch.c）。 */
    ESP_RETURN_ON_ERROR(espaperplay_ui_touch_init(), TAG, "touch pointer indev init failed");

    /* 渲染任务栈：LVGL 单线程渲染模式（LV_OS_NONE）下，FreeType 字形栅格化
     * （smooth 渲染器）在本任务栈上执行，8KB 会栈溢出（曾实测 gui_lvgl 溢出），
     * 需 >=32KB（与 LVGL 对 draw 线程的官方要求一致）。
     * 栈放 PSRAM：32KB 内部 RAM 分配会失败（WiFi/Web 已占大量内部堆），
     * PSRAM 栈是官方支持做法（esp_lvgl_adapter 的 LVGL_THREAD_STACK_IN_PSRAM）。 */
    static TaskHandle_t s_lvgl_task = NULL;
    if (xTaskCreateWithCaps(espaperplay_lvgl_task, "gui_lvgl", 32768, NULL, 5, &s_lvgl_task,
                            MALLOC_CAP_SPIRAM) != pdPASS) {
        ESP_LOGE(TAG, "LVGL task create failed");
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "LVGL %d.%d.%d started (%ux%u, draw buf %uB, partial mode)", LVGL_VERSION_MAJOR,
             LVGL_VERSION_MINOR, LVGL_VERSION_PATCH, disp_w, disp_h,
             (unsigned)(disp_w * ESPAPERPLAY_LVGL_BUF_ROWS * 2));
    return ESP_OK;
}
