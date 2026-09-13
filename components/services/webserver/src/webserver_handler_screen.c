/*
 * SPDX-FileCopyrightText: 2026 ESPaperPlay Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "esp_http_server.h"
#include "esp_log.h"

#include "cJSON.h"

#include "espaperplay_auth.h"
#include "espaperplay_display.h"
#include "espaperplay_gui.h"
#include "espaperplay_input.h"
#include "espaperplay_session.h"
#include "webserver_internal.h"

/**
 * @file webserver_handler_screen.c
 * @brief 屏幕镜像（调试）路由：HTTP 快照 + WebSocket 推流与远程注入。
 *
 * - GET /api/screen/snapshot：当前画布完整帧（二进制，帧头 + 像素），
 *   供页面首绘 / WebSocket 不可用时的回退轮询；
 * - GET /api/screen/ws：WebSocket（wss）。握手前经 pre-handshake 回调鉴权
 *   （Bearer 头或 ?token= 查询参数——浏览器 WebSocket 无法自定义请求头），
 *   握手完成后 post-handshake 回调把连接注册进镜像引擎开始接收推流；
 *   客户端文本消息（JSON）注入输入事件：触摸（按下 / 移动 / 抬起）、
 *   BOOT 键动作、强制全刷、保活 ping。
 *
 * 注入路径与真实输入完全一致（espaperplay_input_post_event），并刷新
 * "最近用户活动"时间戳，远程触控与物理触控行为无差别。
 */

static const char *TAG = "ESPaperPlay_SCREEN";

/*!< 客户端文本消息（JSON）最大长度；超出直接断开（消费完整帧需读入载荷）。 */
#define SCREEN_WS_RX_MAX 256

/*!< 注入触摸事件的自增帧序号（设备侧独立于 GT911 真实帧计数）。 */
static uint16_t s_inject_touch_seq = 0;

/* ------------------------------------------------------------------ */
/* 鉴权                                                                 */
/* ------------------------------------------------------------------ */

/**
 * @brief 提取 WebSocket 握手请求的会话令牌。
 *
 * 优先 Authorization: Bearer 头（非浏览器客户端），回退 ?token= 查询参数
 * （浏览器 WebSocket API 无法设置自定义请求头）。
 */
static bool screen_ws_get_token(httpd_req_t *req, char *token, size_t token_size) {
    if (webserver_get_bearer_token(req, token, token_size)) {
        return true;
    }
    const size_t qlen = httpd_req_get_url_query_len(req);
    if (qlen == 0 || qlen >= 512) {
        return false;
    }
    char query[512];
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK) {
        return false;
    }
    return httpd_query_key_value(query, "token", token, token_size) == ESP_OK;
}

/**
 * @brief WebSocket 握手前回调：校验会话令牌，失败中止握手。
 *
 * 与控制台其他敏感接口的鉴权语义一致：出厂未设置密码（首次设置阶段）
 * 放行。回调上下文为 httpd 任务，返回非 ESP_OK 即断开连接。
 */
esp_err_t webserver_screen_ws_pre_handshake(httpd_req_t *req) {
    if (!espaperplay_auth_is_configured()) {
        return ESP_OK; /* 首次设置阶段：与控制台其余接口一致放行 */
    }

    char token[ESPAPERPLAY_SESSION_TOKEN_HEX_LEN];
    if (!screen_ws_get_token(req, token, sizeof(token)) ||
        espaperplay_session_verify(token, NULL) != ESP_OK) {
        ESP_LOGW(TAG, "ws handshake rejected: invalid token");
        return ESP_FAIL;
    }
    return ESP_OK;
}

/**
 * @brief WebSocket 握手完成回调：把连接注册进镜像引擎。
 *
 * 在握手完成后注册（此时会话才处于 WebSocket 状态）：镜像推送按
 * "连接是否为活跃 WS 会话"过滤，握手前注册会被误判剔除。注册即触发
 * 一次当前画面推送（客户端 last_seq 从 0 起）。
 */
esp_err_t webserver_screen_ws_post_handshake(httpd_req_t *req) {
    const esp_err_t err =
        webserver_screen_client_add(req->handle, httpd_req_to_sockfd(req));
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "ws client register failed: %s", esp_err_to_name(err));
        return ESP_FAIL;
    }
    return ESP_OK;
}

/* ------------------------------------------------------------------ */
/* 注入                                                                 */
/* ------------------------------------------------------------------ */

/** 以 JSON 文本回复客户端（仅错误 / pong；成功注入保持静默）。 */
static void screen_ws_send_json(httpd_req_t *req, const cJSON *root) {
    char *str = cJSON_PrintUnformatted(root);
    if (str == NULL) {
        return;
    }
    httpd_ws_frame_t pkt = {
        .final = true,
        .fragmented = false,
        .type = HTTPD_WS_TYPE_TEXT,
        .payload = (uint8_t *)str,
        .len = strlen(str),
    };
    if (httpd_ws_send_frame(req, &pkt) != ESP_OK) {
        ESP_LOGD(TAG, "ws send reply failed");
    }
    cJSON_free(str);
}

/** 以 {"t":"err",...} 回复。 */
static void screen_ws_send_err(httpd_req_t *req, const char *msg) {
    cJSON *root = cJSON_CreateObject();
    if (root == NULL) {
        return;
    }
    cJSON_AddStringToObject(root, "t", "err");
    cJSON_AddStringToObject(root, "msg", msg);
    screen_ws_send_json(req, root);
    cJSON_Delete(root);
}

/** 注入一个触摸事件（与 GT911 真实事件同路径投递），并标记用户活动。 */
static void screen_inject_touch(uint16_t x, uint16_t y, bool pressed) {
    espaperplay_input_event_t event = {
        .type = ESPAPERPLAY_INPUT_EVENT_TOUCH,
        .point = {.x = x, .y = y, .id = 0, .reserved = 0},
        .touch_pressed = pressed ? 1 : 0,
        .touch_points = pressed ? 1u : 0u,
        .touch_seq = ++s_inject_touch_seq,
    };
    espaperplay_input_mark_activity();
    (void)espaperplay_input_post_event(&event);
}

/** 注入一个 BOOT 键事件。 */
static void screen_inject_key(espaperplay_input_key_action_t action) {
    const espaperplay_input_event_t event = {
        .type = ESPAPERPLAY_INPUT_EVENT_KEY,
        .key_id = ESPAPERPLAY_INPUT_KEY_ID_BOOT,
        .key_action = action,
    };
    espaperplay_input_mark_activity();
    (void)espaperplay_input_post_event(&event);
}

/** 键动作字符串 -> 归一化动作枚举。 */
static bool screen_parse_key_action(const char *str, espaperplay_input_key_action_t *out) {
    static const struct {
        const char *name;
        espaperplay_input_key_action_t action;
    } map[] = {
        {"press_down", ESPAPERPLAY_INPUT_KEY_ACTION_PRESS_DOWN},
        {"press_up", ESPAPERPLAY_INPUT_KEY_ACTION_PRESS_UP},
        {"click", ESPAPERPLAY_INPUT_KEY_ACTION_SINGLE_CLICK},
        {"double_click", ESPAPERPLAY_INPUT_KEY_ACTION_DOUBLE_CLICK},
        {"long_press_start", ESPAPERPLAY_INPUT_KEY_ACTION_LONG_PRESS_START},
        {"long_press_hold", ESPAPERPLAY_INPUT_KEY_ACTION_LONG_PRESS_HOLD},
        {"long_press_up", ESPAPERPLAY_INPUT_KEY_ACTION_LONG_PRESS_UP},
    };
    if (str == NULL) {
        return false;
    }
    for (size_t i = 0; i < sizeof(map) / sizeof(map[0]); i++) {
        if (strcmp(str, map[i].name) == 0) {
            *out = map[i].action;
            return true;
        }
    }
    return false;
}

/** 处理一条客户端文本消息（已解析为 cJSON）。 */
static void screen_ws_dispatch(httpd_req_t *req, const cJSON *root) {
    const cJSON *t = cJSON_GetObjectItemCaseSensitive(root, "t");
    if (!cJSON_IsString(t) || t->valuestring == NULL) {
        screen_ws_send_err(req, "missing field 't'");
        return;
    }

    if (strcmp(t->valuestring, "ping") == 0) {
        cJSON *pong = cJSON_CreateObject();
        if (pong != NULL) {
            cJSON_AddStringToObject(pong, "t", "pong");
            screen_ws_send_json(req, pong);
            cJSON_Delete(pong);
        }
        return;
    }

    if (strcmp(t->valuestring, "touch") == 0) {
        const cJSON *down = cJSON_GetObjectItemCaseSensitive(root, "down");
        if (!cJSON_IsBool(down)) {
            screen_ws_send_err(req, "touch frame requires 'down'");
            return;
        }
        const bool pressed = cJSON_IsTrue(down);
        if (!pressed) {
            /* 抬起帧：无坐标，投递全手指释放事件。 */
            screen_inject_touch(0, 0, false);
            return;
        }
        const cJSON *jx = cJSON_GetObjectItemCaseSensitive(root, "x");
        const cJSON *jy = cJSON_GetObjectItemCaseSensitive(root, "y");
        if (!cJSON_IsNumber(jx) || !cJSON_IsNumber(jy)) {
            screen_ws_send_err(req, "touch frame requires x/y");
            return;
        }
        /* 坐标裁剪到有效显示区（与触摸驱动归一化行为一致）。 */
        const uint16_t x =
            (uint16_t)(jx->valuedouble < 0 ? 0
                       : (jx->valuedouble >= espaperplay_display_width()
                              ? espaperplay_display_width() - 1
                              : jx->valuedouble));
        const uint16_t y =
            (uint16_t)(jy->valuedouble < 0 ? 0
                       : (jy->valuedouble >= espaperplay_display_height()
                              ? espaperplay_display_height() - 1
                              : jy->valuedouble));
        screen_inject_touch(x, y, true);
        return;
    }

    if (strcmp(t->valuestring, "key") == 0) {
        const cJSON *action = cJSON_GetObjectItemCaseSensitive(root, "action");
        espaperplay_input_key_action_t key_action = ESPAPERPLAY_INPUT_KEY_ACTION_MAX;
        if (!cJSON_IsString(action) ||
            !screen_parse_key_action(action->valuestring, &key_action)) {
            screen_ws_send_err(req, "unknown key action");
            return;
        }
        screen_inject_key(key_action);
        return;
    }

    if (strcmp(t->valuestring, "refresh") == 0) {
        const esp_err_t err = espaperplay_gui_full_refresh();
        if (err != ESP_OK) {
            screen_ws_send_err(req, esp_err_to_name(err));
        }
        return;
    }

    screen_ws_send_err(req, "unknown message type");
}

/* ------------------------------------------------------------------ */
/* 路由处理器                                                            */
/* ------------------------------------------------------------------ */

esp_err_t webserver_handle_screen_snapshot_get(httpd_req_t *req) {
    if (webserver_require_auth(req) != ESP_OK) {
        return ESP_FAIL;
    }

    const uint8_t *buf = NULL;
    size_t len = 0;
    const esp_err_t err = webserver_screen_snapshot(&buf, &len);
    if (err != ESP_OK) {
        webserver_send_json_err_status(req, "503 Service Unavailable", "镜像画布未就绪");
        return ESP_FAIL;
    }

    httpd_resp_set_type(req, "application/octet-stream");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, (const char *)buf, (ssize_t)len);
}

esp_err_t webserver_handle_screen_ws(httpd_req_t *req) {
    /* IDF v6.1：握手由框架完成（pre/post 回调承载鉴权与注册），本处理器
     * 仅在客户端数据帧到达时被调用。 */
    httpd_ws_frame_t pkt = {0};
    if (httpd_ws_recv_frame(req, &pkt, 0) != ESP_OK) {
        return ESP_FAIL;
    }
    /* 空帧 / 控制类空帧（PONG 等）：消费即可。 */
    if (pkt.len == 0) {
        return ESP_OK;
    }
    if (pkt.type != HTTPD_WS_TYPE_TEXT) {
        /* 带载荷的非文本帧：不读入就无法保持 TCP 流对齐，断开处理。 */
        ESP_LOGW(TAG, "ws non-text frame (%u B), closing", (unsigned)pkt.len);
        return ESP_FAIL;
    }
    if (pkt.len > SCREEN_WS_RX_MAX) {
        /* 载荷过大：不读入就无法保持 TCP 流对齐，直接断开。 */
        ESP_LOGW(TAG, "ws frame too large (%u B), closing", (unsigned)pkt.len);
        return ESP_FAIL;
    }

    uint8_t payload[SCREEN_WS_RX_MAX];
    pkt.payload = payload;
    if (httpd_ws_recv_frame(req, &pkt, sizeof(payload)) != ESP_OK) {
        return ESP_FAIL;
    }
    payload[pkt.len < sizeof(payload) ? pkt.len : sizeof(payload) - 1] = '\0';

    cJSON *root = cJSON_ParseWithLength((const char *)payload, pkt.len);
    if (root == NULL) {
        screen_ws_send_err(req, "invalid json");
        return ESP_OK;
    }
    screen_ws_dispatch(req, root);
    cJSON_Delete(root);
    return ESP_OK;
}
