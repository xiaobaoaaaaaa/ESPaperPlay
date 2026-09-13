/*
 * SPDX-FileCopyrightText: 2026 ESPaperPlay Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "espaperplay_display.h"
#include "espaperplay_gui.h"
#include "espaperplay_power.h"
#include "webserver_internal.h"

/**
 * @file webserver_screen_mirror.c
 * @brief 屏幕镜像引擎：电子纸帧画布维护 + WebSocket 客户端推送。
 *
 * 数据通路：
 *   GUI worker（每次刷新成功）--帧钩子--> 本引擎画布（1bpp / 2bpp 各一张，
 *   与面板实际显示内容一致）--推送任务--> 各 WebSocket 客户端（wss）。
 *
 * 设计要点：
 *   - 帧钩子在 GUI worker 上下文回调：只做画布拷贝 + 事件置位，网络发送
 *     全部在独立推送任务执行，绝不阻塞刷新管线；
 *   - 每条推送都是"完整帧"（16 字节头 + 全画面位图），无增量协议——客户端
 *     无状态、丢帧自愈（e-ink 刷新本身秒级，整帧 48/96KB 在局域网毫秒级）；
 *   - 每客户端记录 last_seq，只向落后于当前 seq 的客户端发送，客户端加入
 *     （last_seq=0）即自动收到当前画面；推送间隙多次刷新自然合并为最新帧；
 *   - 推送缓冲与画布分离（推送任务持锁拷贝画布后解锁再发送），TLS 发送的
 *     耗时不占用画布锁；
 *   - HTTPS 服务器因 IP 变化重启前，由 webserver 调
 *     webserver_screen_server_stopping() 清空客户端表，避免向已失效句柄
 *     投递发送。
 */

static const char *TAG = "ESPaperPlay_SCREEN_MIRROR";

/*!< 最大同时连接的镜像客户端数。 */
#define MIRROR_MAX_CLIENTS 4

/*!< 二进制帧头长度（魔数 4 + 版本 1 + 格式 1 + seq 4 + 宽 2 + 高 2 + 保留 2）。 */
#define MIRROR_HEADER_SIZE 16

/*!< 推送任务栈（事件等待 + 画布拷贝 + WS 发送排队，无 TLS 重活）。 */
#define MIRROR_TASK_STACK 4096
#define MIRROR_TASK_PRIO 4

/*!< 事件位：有待推送的新帧 / 新客户端加入。 */
#define MIRROR_EV_NEW_FRAME BIT0

/*!< 推送任务等待超时（毫秒）：无新帧时周期醒来，为在线客户端续保持唤醒窗口。 */
#define MIRROR_KEEPALIVE_POLL_MS 30000

/*!< 像素格式：1bpp 黑白（1=白 / 0=黑）。 */
#define MIRROR_FORMAT_BW 0u
/*!< 像素格式：2bpp 四灰阶（0=白 / 1=浅灰 / 2=深灰 / 3=黑，MSB 在前）。 */
#define MIRROR_FORMAT_GRAY4 1u

/** 一个镜像客户端（一条 WebSocket 连接）。 */
typedef struct {
    bool in_use;
    httpd_handle_t hd;   /*!< 所属 HTTP 服务器（IP 变化重启后句柄更换） */
    int fd;              /*!< 连接套接字 */
    uint32_t last_seq;   /*!< 已成功推送到的刷新序号（0 = 尚未推送） */
} mirror_client_t;

static bool s_inited = false;
static SemaphoreHandle_t s_lock = NULL;         /*!< 画布 / 客户端表互斥 */
static EventGroupHandle_t s_events = NULL;      /*!< 推送事件 */
static TaskHandle_t s_push_task = NULL;         /*!< 推送任务 */

static uint8_t *s_canvas_bw = NULL;  /*!< 1bpp 黑白画布（w*h/8，PSRAM 惰性分配） */
static uint8_t *s_canvas_g4 = NULL;  /*!< 2bpp 灰阶画布（w*h/4，PSRAM 惰性分配） */
static size_t s_bw_bytes = 0;        /*!< 1bpp 画布字节数 */
static size_t s_g4_bytes = 0;        /*!< 2bpp 画布字节数 */
static uint16_t s_disp_w = 0;        /*!< 画布宽度（= 显示宽度） */
static uint16_t s_disp_h = 0;        /*!< 画布高度（= 显示高度） */
static bool s_cur_gray4 = false;     /*!< 画布当前权威格式（面板处于灰阶内容时为 true） */
static uint32_t s_seq = 0;           /*!< 已应用到画布的刷新序号（= GUI 刷新序号） */

static mirror_client_t s_clients[MIRROR_MAX_CLIENTS];

static uint8_t *s_send_buf = NULL; /*!< 推送任务组装缓冲（帧头 + 整帧像素） */
static uint8_t *s_snap_buf = NULL; /*!< HTTP 快照组装缓冲（与推送缓冲分离，避免争用） */

/**
 * @brief 惰性分配画布与缓冲（PSRAM 优先，失败回退内部 RAM）。
 *
 * 幂等；须持有 s_lock 或在初始化竞态无虞的上下文调用（首次调用点：帧钩子 /
 * 客户端注册 / 快照请求，均先拿 s_lock）。
 */
static esp_err_t mirror_ensure_alloc(void) {
    if (s_canvas_bw != NULL) {
        return ESP_OK;
    }

    s_disp_w = espaperplay_display_width();
    s_disp_h = espaperplay_display_height();
    if (s_disp_w == 0 || s_disp_h == 0) {
        return ESP_ERR_INVALID_STATE;
    }
    s_bw_bytes = (size_t)s_disp_w * s_disp_h / 8;
    s_g4_bytes = (size_t)s_disp_w * s_disp_h / 4;

    s_canvas_bw = heap_caps_malloc(s_bw_bytes, MALLOC_CAP_SPIRAM);
    if (s_canvas_bw == NULL) {
        s_canvas_bw = heap_caps_malloc(s_bw_bytes, MALLOC_CAP_8BIT);
    }
    s_canvas_g4 = heap_caps_malloc(s_g4_bytes, MALLOC_CAP_SPIRAM);
    if (s_canvas_g4 == NULL) {
        s_canvas_g4 = heap_caps_malloc(s_g4_bytes, MALLOC_CAP_8BIT);
    }
    s_send_buf = heap_caps_malloc(MIRROR_HEADER_SIZE + s_g4_bytes, MALLOC_CAP_SPIRAM);
    if (s_send_buf == NULL) {
        s_send_buf = heap_caps_malloc(MIRROR_HEADER_SIZE + s_g4_bytes, MALLOC_CAP_8BIT);
    }
    s_snap_buf = heap_caps_malloc(MIRROR_HEADER_SIZE + s_g4_bytes, MALLOC_CAP_SPIRAM);
    if (s_snap_buf == NULL) {
        s_snap_buf = heap_caps_malloc(MIRROR_HEADER_SIZE + s_g4_bytes, MALLOC_CAP_8BIT);
    }
    if (s_canvas_bw == NULL || s_canvas_g4 == NULL || s_send_buf == NULL || s_snap_buf == NULL) {
        ESP_LOGE(TAG, "mirror buffer alloc failed (bw=%u g4=%u)", (unsigned)s_bw_bytes,
                 (unsigned)s_g4_bytes);
        return ESP_ERR_NO_MEM;
    }

    /* 画布初始为全白（BW 1=白；灰阶 0=白），与面板上电基线一致。 */
    memset(s_canvas_bw, 0xFF, s_bw_bytes);
    memset(s_canvas_g4, 0x00, s_g4_bytes);
    ESP_LOGI(TAG, "mirror buffers ready: %ux%u, bw=%u B, gray4=%u B", s_disp_w, s_disp_h,
             (unsigned)s_bw_bytes, (unsigned)s_g4_bytes);
    return ESP_OK;
}

/**
 * @brief 把当前画布组装为一条完整帧消息（帧头 + 像素）。
 *
 * @param[out] buf    输出缓冲（至少 MIRROR_HEADER_SIZE + g4 字节数）。
 * @param      buf_size 输出缓冲大小。
 * @param[out] out_len  组装后的消息长度。
 * @param[out] out_seq  本消息对应的刷新序号。
 *
 * @return ESP_OK；画布未就绪返回 ESP_ERR_INVALID_STATE；缓冲不足返回
 *         ESP_ERR_NO_MEM。
 *
 * @note 持 s_lock 执行画布拷贝（PSRAM 96KB 量级，毫秒级）。
 */
static esp_err_t mirror_build_message(uint8_t *buf, size_t buf_size, size_t *out_len,
                                      uint32_t *out_seq) {
    if (s_canvas_bw == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    const size_t need = MIRROR_HEADER_SIZE + (s_cur_gray4 ? s_g4_bytes : s_bw_bytes);
    if (buf_size < need) {
        return ESP_ERR_NO_MEM;
    }

    memcpy(buf + MIRROR_HEADER_SIZE, s_cur_gray4 ? s_canvas_g4 : s_canvas_bw,
           s_cur_gray4 ? s_g4_bytes : s_bw_bytes);

    buf[0] = 'E';
    buf[1] = 'P';
    buf[2] = 'M';
    buf[3] = '1';
    buf[4] = 1; /* version */
    buf[5] = s_cur_gray4 ? MIRROR_FORMAT_GRAY4 : MIRROR_FORMAT_BW;
    const uint32_t seq = s_seq;
    buf[6] = (uint8_t)(seq & 0xFF);
    buf[7] = (uint8_t)((seq >> 8) & 0xFF);
    buf[8] = (uint8_t)((seq >> 16) & 0xFF);
    buf[9] = (uint8_t)((seq >> 24) & 0xFF);
    buf[10] = (uint8_t)(s_disp_w & 0xFF);
    buf[11] = (uint8_t)((s_disp_w >> 8) & 0xFF);
    buf[12] = (uint8_t)(s_disp_h & 0xFF);
    buf[13] = (uint8_t)((s_disp_h >> 8) & 0xFF);
    buf[14] = 0;
    buf[15] = 0;

    *out_len = need;
    *out_seq = seq;
    return ESP_OK;
}

/**
 * @brief 向单个客户端发送一条 WS 二进制帧（阻塞式，内部排队到 httpd 任务）。
 *
 * @return true = 已发送；false = 会话已失效 / 发送失败（调用方应移除该客户端）。
 */
static bool mirror_client_send(httpd_handle_t hd, int fd, const uint8_t *buf, size_t len) {
    if (httpd_ws_get_fd_info(hd, fd) != HTTPD_WS_CLIENT_WEBSOCKET) {
        return false;
    }
    httpd_ws_frame_t pkt = {
        .final = true,
        .fragmented = false,
        .type = HTTPD_WS_TYPE_BINARY,
        .payload = (uint8_t *)buf,
        .len = len,
    };
    return httpd_ws_send_data(hd, fd, &pkt) == ESP_OK;
}

/**
 * @brief 推送任务：把最新画布推给所有落后的客户端。
 *
 * 唤醒后组装一次最新帧（多次刷新自然合并），依次发给 last_seq 落后的
 * 客户端；发送在锁外执行，失败即移除对应客户端。
 *
 * 等待带 30s 超时：超时且有客户端在线时续一次保持唤醒窗口（调
 * espaperplay_power_note_external_activity，与 Web 控制台心跳同一机制），
 * 使"调试会话在线"即抑制自动浅睡眠——否则设备睡眠会断开 WS，推流与
 * 远程注入全部失效。最后一个客户端离开后超时唤醒只做一次空检查，
 * 恢复正常休眠节奏。
 */
static void mirror_push_task(void *arg) {
    (void)arg;

    for (;;) {
        const EventBits_t bits = xEventGroupWaitBits(s_events, MIRROR_EV_NEW_FRAME, pdTRUE,
                                                     pdFALSE,
                                                     pdMS_TO_TICKS(MIRROR_KEEPALIVE_POLL_MS));
        if (!(bits & MIRROR_EV_NEW_FRAME)) {
            xSemaphoreTake(s_lock, portMAX_DELAY);
            bool have_client = false;
            for (size_t i = 0; i < MIRROR_MAX_CLIENTS; i++) {
                if (s_clients[i].in_use) {
                    have_client = true;
                    break;
                }
            }
            xSemaphoreGive(s_lock);
            if (have_client) {
                espaperplay_power_note_external_activity();
            }
            continue;
        }

        size_t len = 0;
        uint32_t msg_seq = 0;
        if (xSemaphoreTake(s_lock, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        const esp_err_t berr = mirror_build_message(s_send_buf,
                                                    MIRROR_HEADER_SIZE + s_g4_bytes, &len,
                                                    &msg_seq);
        xSemaphoreGive(s_lock);
        if (berr != ESP_OK) {
            continue;
        }

        for (size_t i = 0; i < MIRROR_MAX_CLIENTS; i++) {
            xSemaphoreTake(s_lock, portMAX_DELAY);
            if (!s_clients[i].in_use || s_clients[i].last_seq >= msg_seq) {
                xSemaphoreGive(s_lock);
                continue;
            }
            const httpd_handle_t hd = s_clients[i].hd;
            const int fd = s_clients[i].fd;
            xSemaphoreGive(s_lock);

            if (!mirror_client_send(hd, fd, s_send_buf, len)) {
                /* 会话已失效（关闭 / 重启 / 非 WS）：移除，推送循环自愈。 */
                xSemaphoreTake(s_lock, portMAX_DELAY);
                if (s_clients[i].in_use && s_clients[i].fd == fd && s_clients[i].hd == hd) {
                    s_clients[i].in_use = false;
                    ESP_LOGI(TAG, "mirror client #%u removed (send failed)", (unsigned)i);
                }
                xSemaphoreGive(s_lock);
                continue;
            }

            xSemaphoreTake(s_lock, portMAX_DELAY);
            if (s_clients[i].in_use && s_clients[i].fd == fd && s_clients[i].hd == hd) {
                s_clients[i].last_seq = msg_seq;
            }
            xSemaphoreGive(s_lock);
        }
    }
}

/**
 * @brief GUI 帧钩子（GUI worker 上下文）：把本次刷新应用进画布。
 *
 * 快速路径：画布拷贝 + 事件置位，绝不阻塞。BW 局部窗口按行拼入画布
 * （窗口数据每行 w/8 字节压缩，与面板差分刷新一致）；BW 全屏 / 灰阶整帧
 * 直接整块拷贝；清屏按格式填全白。灰阶是"面板当前内容"的权威格式——
 * 切换黑白 / 灰阶时记录当前权威画布，推送与快照均按权威画布取数据。
 */
static void mirror_frame_hook(const espaperplay_gui_frame_evt_t *evt, void *user_ctx) {
    (void)user_ctx;

    if (xSemaphoreTake(s_lock, portMAX_DELAY) != pdTRUE) {
        return;
    }
    if (mirror_ensure_alloc() != ESP_OK) {
        xSemaphoreGive(s_lock);
        return;
    }

    if (evt->color == ESPAPERPLAY_GUI_COLOR_GRAY4) {
        if (evt->is_clear) {
            memset(s_canvas_g4, 0x00, s_g4_bytes); /* 2bpp：0=白 */
        } else if (evt->data != NULL) {
            memcpy(s_canvas_g4, evt->data, s_g4_bytes);
        }
        s_cur_gray4 = true;
    } else {
        if (evt->is_clear) {
            memset(s_canvas_bw, 0xFF, s_bw_bytes); /* 1bpp：1=白 */
        } else if (evt->data != NULL) {
            if (evt->full_screen) {
                memcpy(s_canvas_bw, evt->data, s_bw_bytes);
            } else {
                /* 局部窗口：行压缩数据（每行 w/8 字节）拼入整帧画布。
                 * x 已 8 对齐，字节边界直接按行 memcpy。 */
                const size_t row = (size_t)evt->w / 8;
                const size_t dst_row = (size_t)s_disp_w / 8;
                for (uint16_t r = 0; r < evt->h; r++) {
                    memcpy(s_canvas_bw + (size_t)(evt->y + r) * dst_row + evt->x / 8,
                           evt->data + (size_t)r * row, row);
                }
            }
        }
        s_cur_gray4 = false;
    }
    s_seq = evt->seq;

    bool have_client = false;
    for (size_t i = 0; i < MIRROR_MAX_CLIENTS; i++) {
        if (s_clients[i].in_use) {
            have_client = true;
            break;
        }
    }
    xSemaphoreGive(s_lock);

    if (have_client) {
        xEventGroupSetBits(s_events, MIRROR_EV_NEW_FRAME);
    }
}

/* ------------------------------------------------------------------ */
/* 内部接口（webserver 主模块 / 处理器调用）                              */
/* ------------------------------------------------------------------ */

void webserver_screen_mirror_init(void) {
    if (s_inited) {
        return;
    }
    s_lock = xSemaphoreCreateMutex();
    s_events = xEventGroupCreate();
    if (s_lock == NULL || s_events == NULL) {
        ESP_LOGE(TAG, "mirror init failed (no mem for sync primitives)");
        return;
    }
    memset(s_clients, 0, sizeof(s_clients));

    if (xTaskCreate(mirror_push_task, "scr_mirror", MIRROR_TASK_STACK, NULL, MIRROR_TASK_PRIO,
                    &s_push_task) != pdPASS) {
        ESP_LOGE(TAG, "mirror push task create failed");
        return;
    }

    /* GUI 未就绪（无头启动）时仅告警：镜像无帧源，注入通道不受影响。 */
    const esp_err_t herr = espaperplay_gui_set_frame_hook(mirror_frame_hook, NULL);
    if (herr != ESP_OK) {
        ESP_LOGW(TAG, "register gui frame hook failed (%s), mirror has no frame source",
                 esp_err_to_name(herr));
    }

    s_inited = true;
    ESP_LOGI(TAG, "screen mirror engine started");
}

void webserver_screen_server_stopping(void) {
    if (s_lock == NULL) {
        return;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool had = false;
    for (size_t i = 0; i < MIRROR_MAX_CLIENTS; i++) {
        if (s_clients[i].in_use) {
            s_clients[i].in_use = false;
            had = true;
        }
    }
    xSemaphoreGive(s_lock);
    if (had) {
        ESP_LOGI(TAG, "mirror clients cleared (https server restarting)");
    }
}

esp_err_t webserver_screen_client_add(httpd_handle_t hd, int fd) {
    if (s_lock == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t ret = ESP_ERR_NO_MEM;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    /* 先清僵尸槽（会话已关闭的残留 fd），再找空闲槽。 */
    for (size_t i = 0; i < MIRROR_MAX_CLIENTS; i++) {
        if (s_clients[i].in_use &&
            httpd_ws_get_fd_info(s_clients[i].hd, s_clients[i].fd) !=
                HTTPD_WS_CLIENT_WEBSOCKET) {
            s_clients[i].in_use = false;
        }
    }
    for (size_t i = 0; i < MIRROR_MAX_CLIENTS; i++) {
        if (!s_clients[i].in_use) {
            s_clients[i].in_use = true;
            s_clients[i].hd = hd;
            s_clients[i].fd = fd;
            s_clients[i].last_seq = 0; /* 触发加入即推送当前画面 */
            ret = ESP_OK;
            ESP_LOGI(TAG, "mirror client #%u joined (fd=%d)", (unsigned)i, fd);
            break;
        }
    }
    xSemaphoreGive(s_lock);

    if (ret == ESP_OK) {
        xEventGroupSetBits(s_events, MIRROR_EV_NEW_FRAME);
    }
    return ret;
}

esp_err_t webserver_screen_snapshot(const uint8_t **out_buf, size_t *out_len) {
    if (s_lock == NULL || out_buf == NULL || out_len == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);
    /* 快照前确保画布存在（无刷新历史的极端场景：全白画布）。 */
    esp_err_t ret = mirror_ensure_alloc();
    size_t len = 0;
    uint32_t seq = 0;
    if (ret == ESP_OK) {
        ret = mirror_build_message(s_snap_buf, MIRROR_HEADER_SIZE + s_g4_bytes, &len, &seq);
    }
    xSemaphoreGive(s_lock);

    if (ret == ESP_OK) {
        /* s_snap_buf 为引擎私有静态缓冲，推送任务不写它，返回后仍然有效。 */
        *out_buf = s_snap_buf;
        *out_len = len;
    }
    return ret;
}

void webserver_screen_get_info(uint32_t *out_seq, bool *out_gray4, uint16_t *out_w,
                               uint16_t *out_h) {
    if (s_lock == NULL) {
        return;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (out_seq != NULL) {
        *out_seq = s_seq;
    }
    if (out_gray4 != NULL) {
        *out_gray4 = s_cur_gray4;
    }
    if (out_w != NULL) {
        *out_w = s_disp_w;
    }
    if (out_h != NULL) {
        *out_h = s_disp_h;
    }
    xSemaphoreGive(s_lock);
}
