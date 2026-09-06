/*
 * SPDX-FileCopyrightText: 2026 ESPaperPlay Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

#include "esp_err.h"
#include "esp_http_server.h"
#include "esp_log.h"

#include "cJSON.h"

#include "espaperplay_config.h"
#include "espaperplay_fonts.h"
#include "espaperplay_storage.h"
#include "espaperplay_system.h"

#include "webserver_internal.h"

static const char *TAG = "ESPaperPlay_WEB_FONT";

/** 上传请求体的流式读写缓冲（分块写入 SD，避免整文件驻留 RAM）。 */
#define FONT_UPLOAD_CHUNK 4096
/** 选择 / 枚举请求体上限（表单很小）。 */
#define FONT_FORM_BUF_SIZE 1024

/** 校验字体文件名：仅允许简单文件名（无路径分隔符 / 目录穿越），且以
 *  .ttf / .otf / .ttc 结尾（大小写不敏感）。 */
static bool font_name_valid(const char *name) {
    if (name == NULL || name[0] == '\0') {
        return false;
    }
    if (strchr(name, '/') != NULL || strchr(name, '\\') != NULL || strcmp(name, ".") == 0 ||
        strcmp(name, "..") == 0) {
        return false;
    }
    size_t len = strlen(name);
    if (len < 5) {
        return false;
    }
    const char *ext = name + len - 4;
    if (strcasecmp(ext, ".ttf") == 0 || strcasecmp(ext, ".otf") == 0 ||
        strcasecmp(ext, ".ttc") == 0) {
        return true;
    }
    return false;
}

/** 以 {"ok":true} 响应。 */
static void webserver_send_ok(httpd_req_t *req) {
    cJSON *root = cJSON_CreateObject();
    if (root == NULL) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "No memory");
        return;
    }
    cJSON_AddBoolToObject(root, "ok", true);
    webserver_send_json(req, "200 OK", root);
    cJSON_Delete(root);
}

/* ------------------------------------------------------------------ */
/* 字体枚举 / 选择 / 上传                                               */
/* ------------------------------------------------------------------ */

/** GET /api/fonts —— 返回 SD 卡字体列表、当前选用字体、出厂默认字体与下载提示。 */
esp_err_t webserver_handle_fonts_get(httpd_req_t *req) {
    if (webserver_require_auth(req) != ESP_OK) {
        return ESP_FAIL;
    }

    const espaperplay_system_config_t *cfg = espaperplay_system_get_config();

    cJSON *root = cJSON_CreateObject();
    if (root == NULL) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "No memory");
        return ESP_FAIL;
    }

    const bool mounted = espaperplay_storage_is_mounted();
    cJSON_AddBoolToObject(root, "sd_mounted", mounted);
    cJSON_AddStringToObject(root, "selected",
                            cfg->selected_font[0] != '\0' ? cfg->selected_font
                                                          : ESPAPERPLAY_FONTS_DEFAULT_NAME);
    /* 当前实际正在使用的字体（开机建屏时加载，可能尚未反映最新选择）。 */
    cJSON_AddStringToObject(root, "active", espaperplay_fonts_get_active_name());
    /* 出厂内置字体（位于 Flash 字体分区，不可删除）。 */
    cJSON_AddStringToObject(root, "flash_font", ESPAPERPLAY_FONTS_DEFAULT_NAME);

    cJSON *fonts = cJSON_AddArrayToObject(root, "fonts");
    if (mounted) {
        DIR *d = opendir(ESPAPERPLAY_FONTS_SD_DIR);
        if (d != NULL) {
            struct dirent *e = NULL;
            while ((e = readdir(d)) != NULL) {
                const char *nm = e->d_name;
                size_t len = strlen(nm);
                if (len < 5) {
                    continue;
                }
                const char *ext = nm + len - 4;
                if (strcasecmp(ext, ".ttf") == 0 || strcasecmp(ext, ".otf") == 0 ||
                    strcasecmp(ext, ".ttc") == 0) {
                    cJSON *item = cJSON_CreateObject();
                    if (item == NULL) {
                        closedir(d);
                        cJSON_Delete(root);
                        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "No memory");
                        return ESP_FAIL;
                    }
                    cJSON_AddStringToObject(item, "name", nm);
                    /* 文件大小（字节），用于前端展示。 */
                    char fpath[sizeof(ESPAPERPLAY_FONTS_SD_DIR) +
                               ESPAPERPLAY_SYSTEM_FONT_NAME_MAX_LEN] = {0};
                    snprintf(fpath, sizeof(fpath), "%s/%s", ESPAPERPLAY_FONTS_SD_DIR, nm);
                    struct stat st;
                    cJSON_AddNumberToObject(item, "size",
                                            stat(fpath, &st) == 0 ? (double)st.st_size : 0);
                    cJSON_AddBoolToObject(item, "deletable", true);
                    cJSON_AddBoolToObject(item, "active",
                                          strcmp(espaperplay_fonts_get_active_name(), nm) == 0);
                    cJSON_AddItemToArray(fonts, item);
                }
            }
            closedir(d);
        }
    }

    /* WebUI 下载提示：用户可从此处获取完整字体文件后上传到 SD 卡。 */
    cJSON_AddStringToObject(root, "download_hint",
                            "https://fonts.google.com/noto/specimen/Noto+Sans+SC");

    webserver_send_json(req, "200 OK", root);
    cJSON_Delete(root);
    return ESP_OK;
}

/** POST /api/fonts/select —— 设置当前选用字体（表单字段 name=文件名）。 */
esp_err_t webserver_handle_fonts_select_post(httpd_req_t *req) {
    if (webserver_require_auth(req) != ESP_OK) {
        return ESP_FAIL;
    }

    const int total = req->content_len;
    if (total <= 0 || total >= FONT_FORM_BUF_SIZE) {
        webserver_send_json_err(req, "请求体过大或为空");
        return ESP_FAIL;
    }
    char *body = malloc((size_t)total + 1);
    if (body == NULL) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "No memory");
        return ESP_FAIL;
    }
    int received = 0;
    while (received < total) {
        int r = httpd_req_recv(req, body + received, (size_t)(total - received));
        if (r <= 0) {
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "读取请求体失败");
            free(body);
            return ESP_FAIL;
        }
        received += r;
    }
    body[received] = '\0';

    char name[ESPAPERPLAY_SYSTEM_FONT_NAME_MAX_LEN] = {0};
    const bool has_name = webserver_form_get_field(body, "name", name, sizeof(name));
    free(body);

    if (!has_name || name[0] == '\0') {
        webserver_send_json_err(req, "缺少字体名 name");
        return ESP_FAIL;
    }
    if (!font_name_valid(name)) {
        webserver_send_json_err(req, "非法字体名（需为 .ttf/.otf/.ttc 简单文件名）");
        return ESP_FAIL;
    }

    esp_err_t err = espaperplay_system_set_selected_font(name);
    if (err != ESP_OK) {
        webserver_send_json_err(req, esp_err_to_name(err));
        return ESP_FAIL;
    }

    webserver_send_ok(req);
    return ESP_OK;
}

/** POST /api/fonts/upload?name=文件名 —— 将请求体（原始字体字节）流式写入 SD 卡。 */
esp_err_t webserver_handle_fonts_upload_post(httpd_req_t *req) {
    if (webserver_require_auth(req) != ESP_OK) {
        return ESP_FAIL;
    }

    /* 文件名取自查询参数（避免 multipart 解析开销）。 */
    char name[ESPAPERPLAY_SYSTEM_FONT_NAME_MAX_LEN] = {0};
    char query[128] = {0};
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK ||
        !webserver_form_get_field(query, "name", name, sizeof(name)) || name[0] == '\0') {
        webserver_send_json_err(req, "缺少查询参数 name");
        return ESP_FAIL;
    }
    if (!font_name_valid(name)) {
        webserver_send_json_err(req, "非法字体名（需为 .ttf/.otf/.ttc 简单文件名）");
        return ESP_FAIL;
    }
    if (!espaperplay_storage_is_mounted()) {
        webserver_send_json_err(req, "SD 卡未挂载，无法保存字体");
        return ESP_FAIL;
    }

    /* 逐级创建字体目录（FAT mkdir 不支持一次建多级）。 */
    mkdir(ESPAPERPLAY_STORAGE_MOUNT_POINT "/system", 0777);
    if (mkdir(ESPAPERPLAY_FONTS_SD_DIR, 0777) != 0 && errno != EEXIST) {
        webserver_send_json_err(req, "无法创建字体目录");
        return ESP_FAIL;
    }

    char path[sizeof(ESPAPERPLAY_FONTS_SD_DIR) + ESPAPERPLAY_SYSTEM_FONT_NAME_MAX_LEN] = {0};
    snprintf(path, sizeof(path), "%s/%s", ESPAPERPLAY_FONTS_SD_DIR, name);

    FILE *f = fopen(path, "wb");
    if (f == NULL) {
        webserver_send_json_err(req, "无法创建字体文件");
        return ESP_FAIL;
    }

    const int total = req->content_len;
    if (total <= 0) {
        fclose(f);
        remove(path);
        webserver_send_json_err(req, "空文件");
        return ESP_FAIL;
    }

    /* 分块读取请求体并写入 SD，避免大字体整文件驻留 RAM。 */
    char buf[FONT_UPLOAD_CHUNK];
    int received = 0;
    while (received < total) {
        int chunk = total - received;
        if (chunk > (int)sizeof(buf)) {
            chunk = (int)sizeof(buf);
        }
        int r = httpd_req_recv(req, buf, (size_t)chunk);
        if (r <= 0) {
            fclose(f);
            remove(path);
            webserver_send_json_err(req, "读取请求体失败");
            return ESP_FAIL;
        }
        received += r;
        if (fwrite(buf, 1, (size_t)r, f) != (size_t)r) {
            fclose(f);
            remove(path);
            webserver_send_json_err(req, "写入 SD 卡失败");
            return ESP_FAIL;
        }
    }
    fclose(f);

    ESP_LOGI(TAG, "font uploaded: %s (%d bytes)", name, received);
    cJSON *ok = cJSON_CreateObject();
    if (ok == NULL) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "No memory");
        return ESP_FAIL;
    }
    cJSON_AddBoolToObject(ok, "ok", true);
    cJSON_AddNumberToObject(ok, "size", received);
    webserver_send_json(req, "200 OK", ok);
    cJSON_Delete(ok);
    return ESP_OK;
}

/** POST /api/fonts/delete —— 删除 SD 卡上已上传的字体文件（表单字段 name=文件名）。 */
esp_err_t webserver_handle_fonts_delete_post(httpd_req_t *req) {
    if (webserver_require_auth(req) != ESP_OK) {
        return ESP_FAIL;
    }

    /* 读取表单编码的请求体（与 select 接口一致，name 经请求体传递）。 */
    int total = req->content_len;
    if (total <= 0 || total >= FONT_FORM_BUF_SIZE) {
        webserver_send_json_err(req, "请求体过大或为空");
        return ESP_FAIL;
    }
    char *body = malloc((size_t)total + 1);
    if (body == NULL) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "No memory");
        return ESP_FAIL;
    }
    int received = 0;
    while (received < total) {
        int r = httpd_req_recv(req, body + received, (size_t)(total - received));
        if (r <= 0) {
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "读取请求体失败");
            free(body);
            return ESP_FAIL;
        }
        received += r;
    }
    body[received] = '\0';

    char name[ESPAPERPLAY_SYSTEM_FONT_NAME_MAX_LEN] = {0};
    const bool has_name = webserver_form_get_field(body, "name", name, sizeof(name));
    free(body);
    if (!has_name || name[0] == '\0') {
        webserver_send_json_err(req, "缺少字体名 name");
        return ESP_FAIL;
    }
    if (!font_name_valid(name)) {
        webserver_send_json_err(req, "非法字体名（需为 .ttf/.otf/.ttc 简单文件名）");
        return ESP_FAIL;
    }
    if (!espaperplay_storage_is_mounted()) {
        webserver_send_json_err(req, "SD 卡未挂载，无法删除字体");
        return ESP_FAIL;
    }

    char path[sizeof(ESPAPERPLAY_FONTS_SD_DIR) + ESPAPERPLAY_SYSTEM_FONT_NAME_MAX_LEN] = {0};
    snprintf(path, sizeof(path), "%s/%s", ESPAPERPLAY_FONTS_SD_DIR, name);

    if (remove(path) != 0) {
        webserver_send_json_err(req, "删除失败（文件可能不存在）");
        return ESP_FAIL;
    }

    /* 出厂内置字体位于 Flash 字体分区，不可经 SD 接口删除（前端亦不提供按钮）。 */
    if (strcmp(name, ESPAPERPLAY_FONTS_DEFAULT_NAME) == 0) {
        webserver_send_json_err(req, "出厂内置字体不可删除");
        return ESP_FAIL;
    }

    /* 若删除的正是当前实际正在使用的字体，清空选择以回退出厂内置字体，
     * 并提示前端重启使回退生效（字体在开机建屏时加载，运行期不热切换）。 */
    if (strcmp(espaperplay_fonts_get_active_name(), name) == 0) {
        ESP_LOGI(TAG, "deleted active font %s, resetting selection to default", name);
        espaperplay_system_set_selected_font("");
        cJSON *ok = cJSON_CreateObject();
        if (ok == NULL) {
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "No memory");
            return ESP_FAIL;
        }
        cJSON_AddBoolToObject(ok, "ok", true);
        cJSON_AddBoolToObject(ok, "reboot", true);
        webserver_send_json(req, "200 OK", ok);
        cJSON_Delete(ok);
        return ESP_OK;
    }

    ESP_LOGI(TAG, "font deleted: %s", name);
    webserver_send_ok(req);
    return ESP_OK;
}
