/*
 * SPDX-FileCopyrightText: 2026 ESPaperPlay Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "esp_err.h"
#include "esp_http_server.h"
#include "esp_log.h"

#include "cJSON.h"

#include "espaperplay_config.h"
#include "espaperplay_storage.h"

#include "webserver_internal.h"

static const char *TAG = "ESPaperPlay_WEB_FILES";

/* ====================================================================
 * 文件管理域：SD 卡文件浏览 / 上传 / 下载 / 新建 / 重命名 / 删除
 * ====================================================================
 *
 * 与设备端文件管理页（screen_files.c）对应的最小 Web 版本：
 *   - GET  /api/files?path=/sub/dir        枚举目录（目录优先，各带大小）
 *   - GET  /api/files/download             流式下载文件（附件，MIME 按扩展名）
 *   - POST /api/files/upload?overwrite=1   原始字节流式上传（同字体上传模式）
 *   - POST /api/files/mkdir                表单 path + name
 *   - POST /api/files/rename               表单 path + from + to
 *   - POST /api/files/delete               表单 path + name（目录递归删除）
 *
 * 安全：
 *   - 全部接口经 webserver_require_auth 鉴权（可浏览整个 SD 卡内容）；
 *   - 路径一律规范化（拒绝 ".." 目录穿越、控制字符、反斜杠），限定在
 *     SD 卡挂载点内；
 *   - 名称字段拒绝路径分隔符与 "." / ".."，重命名目标已存在时报错
 *     （POSIX rename 会静默覆盖，必须显式拦截）；上传默认拒绝覆盖已
 *     存在条目，需显式带 overwrite=1（前端先经用户确认）；
 *   - 递归删除深度受限（httpd 任务栈有限，比设备端更保守）。
 */

/** 表单请求体上限（path / name 均为短字段）。 */
#define FILES_FORM_BUF_SIZE 1024
/** 相对路径缓冲（规范化后的挂载点内路径，含 NUL）。 */
#define FILES_REL_MAX 256
/** 绝对路径缓冲（挂载点 + 相对路径 + 条目名，含 NUL）。 */
#define FILES_ABS_MAX 512
/** 单次枚举条目数上限（超出置 truncated 标志）。 */
#define FILES_LIST_MAX 500
/** 递归删除深度上限（httpd 任务栈 10KB，比设备端 8 层更保守）。 */
#define FILES_RM_DEPTH_MAX 6
/** 上传 / 下载分块缓冲（httpd 任务栈上分配，勿再加大）。 */
#define FILES_IO_CHUNK 4096
/** 查询串缓冲（path/name 经 URL 编码后长度会膨胀）。 */
#define FILES_QUERY_MAX 512

/* ------------------------------------------------------------------ */
/* 路径 / 名称工具                                                      */
/* ------------------------------------------------------------------ */

/**
 * 规范化相对路径（相对 SD 卡挂载点）。
 *
 * 空 / "/" 视为根（输出空串）；必须以 '/' 开头；逐段处理，忽略空段与
 * "."，拒绝 ".."（目录穿越）与含控制字符的段。输出不带结尾斜杠。
 */
static bool files_rel_normalize(const char *in, char *out, size_t out_size) {
    if (in == NULL || out == NULL || out_size == 0) {
        return false;
    }
    out[0] = '\0';
    if (in[0] == '\0' || (in[0] == '/' && in[1] == '\0')) {
        return true;
    }
    if (in[0] != '/') {
        return false;
    }

    const char *p = in;
    while (*p != '\0') {
        p++; /* 跳过分隔符（开头或连续斜杠） */
        const char *seg = p;
        while (*p != '\0' && *p != '/') {
            p++;
        }
        const size_t seg_len = (size_t)(p - seg);
        if (seg_len == 0) {
            continue;
        }
        if (seg_len == 1 && seg[0] == '.') {
            continue;
        }
        if (seg_len == 2 && seg[0] == '.' && seg[1] == '.') {
            return false; /* 目录穿越 */
        }
        for (size_t i = 0; i < seg_len; i++) {
            unsigned char c = (unsigned char)seg[i];
            if (c < 0x20 || c == 0x7F) {
                return false;
            }
        }
        if (strlen(out) + 1 + seg_len >= out_size) {
            return false; /* 超长 */
        }
        strlcat(out, "/", out_size);
        strncat(out, seg, seg_len); /* 段非 NUL 结尾，用定长追加 */
    }
    return true;
}

/** 条目名合法性：非空、非 "."/".."、不含路径分隔符与控制字符。 */
static bool files_name_valid(const char *name) {
    if (name == NULL || name[0] == '\0') {
        return false;
    }
    if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0) {
        return false;
    }
    for (const char *p = name; *p != '\0'; p++) {
        unsigned char c = (unsigned char)*p;
        if (c == '/' || c == '\\' || c < 0x20 || c == 0x7F) {
            return false;
        }
    }
    return true;
}

/** 由规范化相对路径构造绝对路径（挂载点 + rel；rel 空串 = 挂载点本身）。 */
static bool files_abs_path(const char *rel, char *dst, size_t size) {
    if (strlcpy(dst, ESPAPERPLAY_STORAGE_MOUNT_POINT, size) >= size) {
        return false;
    }
    if (rel[0] != '\0' && strlcat(dst, rel, size) >= size) {
        return false;
    }
    return true;
}

/** 拼接目录 + "/" + 条目名（strlcpy/strlcat 恒 NUL 结尾；超长返回 false）。 */
static bool files_abs_join(char *dst, size_t size, const char *dir, const char *name) {
    if (strlcpy(dst, dir, size) >= size) {
        return false;
    }
    if (strlcat(dst, "/", size) >= size) {
        return false;
    }
    return strlcat(dst, name, size) < size;
}

/** 以 {"ok":true} 响应。 */
static void files_send_ok(httpd_req_t *req) {
    cJSON *root = cJSON_CreateObject();
    if (root == NULL) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "No memory");
        return;
    }
    cJSON_AddBoolToObject(root, "ok", true);
    webserver_send_json(req, "200 OK", root);
    cJSON_Delete(root);
}

/** 读取表单编码请求体（小表单）；失败时已响应错误并返回 NULL（调用方直接返回）。 */
static char *files_read_form_body(httpd_req_t *req) {
    const int total = req->content_len;
    if (total <= 0 || total >= FILES_FORM_BUF_SIZE) {
        webserver_send_json_err(req, "请求体过大或为空");
        return NULL;
    }
    char *body = malloc((size_t)total + 1);
    if (body == NULL) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "No memory");
        return NULL;
    }
    int received = 0;
    while (received < total) {
        int r = httpd_req_recv(req, body + received, (size_t)(total - received));
        if (r <= 0) {
            free(body);
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "读取请求体失败");
            return NULL;
        }
        received += r;
    }
    body[received] = '\0';
    return body;
}

/** 从表单解析目录 path 字段并转为绝对路径；失败已响应并返回 false。 */
static bool files_resolve_dir(httpd_req_t *req, const char *body, char *abs, size_t size) {
    char raw[FILES_REL_MAX] = "";
    char rel[FILES_REL_MAX] = "";
    if (!webserver_form_get_field(body, "path", raw, sizeof(raw)) ||
        !files_rel_normalize(raw, rel, sizeof(rel)) || !files_abs_path(rel, abs, size)) {
        webserver_send_json_err(req, "缺少或非法目录 path");
        return false;
    }
    return true;
}

/** 从表单解析名称字段并校验；失败已响应并返回 false。 */
static bool files_name_field(httpd_req_t *req, const char *body, const char *field, char *out,
                             size_t size) {
    if (!webserver_form_get_field(body, field, out, size) || !files_name_valid(out)) {
        webserver_send_json_err(req, "缺少或非法名称字段");
        return false;
    }
    return true;
}

/* ------------------------------------------------------------------ */
/* 递归删除                                                             */
/* ------------------------------------------------------------------ */

/** 按扩展名猜测 MIME 类型（未知类型用 application/octet-stream）。 */
static const char *files_mime_by_ext(const char *name) {
    static const struct {
        const char *ext;
        const char *mime;
    } map[] = {
        {".txt", "text/plain"},      {".md", "text/plain"},       {".log", "text/plain"},
        {".csv", "text/csv"},        {".html", "text/html"},      {".htm", "text/html"},
        {".css", "text/css"},        {".js", "text/javascript"},  {".json", "application/json"},
        {".xml", "application/xml"}, {".png", "image/png"},       {".jpg", "image/jpeg"},
        {".jpeg", "image/jpeg"},     {".gif", "image/gif"},       {".bmp", "image/bmp"},
        {".svg", "image/svg+xml"},   {".pdf", "application/pdf"}, {".zip", "application/zip"},
        {".ttf", "font/ttf"},        {".otf", "font/otf"},        {".ttc", "font/collection"},
        {".woff", "font/woff"},      {".woff2", "font/woff2"},    {".mp3", "audio/mpeg"},
        {".wav", "audio/wav"},       {".mp4", "video/mp4"},
    };
    const size_t len = strlen(name);
    for (size_t i = 0; i < sizeof(map) / sizeof(map[0]); i++) {
        const size_t ext_len = strlen(map[i].ext);
        if (len >= ext_len && strcasecmp(name + len - ext_len, map[i].ext) == 0) {
            return map[i].mime;
        }
    }
    return "application/octet-stream";
}

/**
 * 构造 Content-Disposition 头值（附件下载）。
 *
 * 文件名可能含非 ASCII（中文）：同时给出 ASCII 兜底 filename（非可打印
 * 字符替换为 '_'）与 RFC 5987 filename*（UTF-8 百分号编码），现代浏览器
 * 优先取后者。
 */
static void files_content_disposition(const char *name, char *out, size_t out_size) {
    /* ASCII 兜底名 */
    char ascii[FILES_REL_MAX];
    size_t ai = 0;
    for (const char *p = name; *p != '\0' && ai + 1 < sizeof(ascii); p++) {
        unsigned char c = (unsigned char)*p;
        ascii[ai++] = (c >= 0x21 && c <= 0x7E && c != '"' && c != '\\') ? (char)c : '_';
    }
    ascii[ai] = '\0';

    /* UTF-8 百分号编码 */
    char encoded[FILES_REL_MAX * 3];
    size_t ei = 0;
    for (const char *p = name; *p != '\0' && ei + 4 < sizeof(encoded); p++) {
        unsigned char c = (unsigned char)*p;
        if (c > 0x20 && c < 0x7F && c != '"' && c != '%' && c != '\\') {
            encoded[ei++] = (char)c;
        } else {
            snprintf(encoded + ei, sizeof(encoded) - ei, "%%%02X", c);
            ei += 3;
        }
    }
    encoded[ei] = '\0';

    /* 链式拼接（strlcat 恒 NUL 结尾；极端长名时安全截断，浏览器回退
     * 到 ASCII 兜底 filename，规避 -Werror=format-truncation）。 */
    strlcpy(out, "attachment; filename=\"", out_size);
    strlcat(out, ascii, out_size);
    strlcat(out, "\"; filename*=UTF-8''", out_size);
    strlcat(out, encoded, out_size);
}

/** 递归删除文件 / 目录（深度受限；尽力删完其余条目，最后统一报失败）。 */
static esp_err_t files_rm_rf(const char *path, int depth) {
    if (depth > FILES_RM_DEPTH_MAX) {
        return ESP_ERR_INVALID_STATE;
    }
    struct stat st;
    if (stat(path, &st) != 0) {
        return ESP_FAIL;
    }
    if (!S_ISDIR(st.st_mode)) {
        return (unlink(path) == 0) ? ESP_OK : ESP_FAIL;
    }
    DIR *d = opendir(path);
    if (d == NULL) {
        return ESP_FAIL;
    }
    esp_err_t ret = ESP_OK;
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        if (e->d_name[0] == '.' &&
            (e->d_name[1] == '\0' || (e->d_name[1] == '.' && e->d_name[2] == '\0'))) {
            continue;
        }
        char child[FILES_ABS_MAX];
        if (!files_abs_join(child, sizeof(child), path, e->d_name)) {
            ret = ESP_ERR_INVALID_SIZE;
            continue;
        }
        esp_err_t sub = files_rm_rf(child, depth + 1);
        if (sub != ESP_OK) {
            ret = sub;
        }
    }
    closedir(d);
    if (ret == ESP_OK && rmdir(path) != 0) {
        ret = ESP_FAIL;
    }
    return ret;
}

/* ------------------------------------------------------------------ */
/* 路由处理器                                                           */
/* ------------------------------------------------------------------ */

/** GET /api/files?path=/sub/dir —— 枚举目录（两趟扫描：先目录后文件）。 */
esp_err_t webserver_handle_files_get(httpd_req_t *req) {
    if (webserver_require_auth(req) != ESP_OK) {
        return ESP_FAIL;
    }

    char rel[FILES_REL_MAX] = "";
    char query[192] = {0};
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {
        char raw[FILES_REL_MAX] = "";
        if (webserver_form_get_field(query, "path", raw, sizeof(raw)) &&
            !files_rel_normalize(raw, rel, sizeof(rel))) {
            webserver_send_json_err(req, "非法路径");
            return ESP_FAIL;
        }
    }

    cJSON *root = cJSON_CreateObject();
    if (root == NULL) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "No memory");
        return ESP_FAIL;
    }

    const bool mounted = espaperplay_storage_is_mounted();
    cJSON_AddBoolToObject(root, "sd_mounted", mounted);
    cJSON_AddStringToObject(root, "path", rel[0] != '\0' ? rel : "/");
    cJSON_AddBoolToObject(root, "truncated", false);

    cJSON *entries = cJSON_AddArrayToObject(root, "entries");
    int count = 0;
    bool truncated = false;

    if (mounted) {
        char abs[FILES_ABS_MAX];
        if (!files_abs_path(rel, abs, sizeof(abs))) {
            cJSON_Delete(root);
            webserver_send_json_err(req, "路径过长");
            return ESP_FAIL;
        }
        /* 两趟扫描避免整目录排序缓存：第一趟目录、第二趟文件。 */
        for (int pass = 0; pass < 2 && !truncated; pass++) {
            DIR *d = opendir(abs);
            if (d == NULL) {
                break; /* 目录不存在 / 打不开：返回空列表 */
            }
            struct dirent *e;
            while ((e = readdir(d)) != NULL) {
                if (e->d_name[0] == '.' &&
                    (e->d_name[1] == '\0' || (e->d_name[1] == '.' && e->d_name[2] == '\0'))) {
                    continue;
                }
                char full[FILES_ABS_MAX];
                struct stat st;
                if (!files_abs_join(full, sizeof(full), abs, e->d_name) || stat(full, &st) != 0) {
                    continue;
                }
                const bool is_dir = S_ISDIR(st.st_mode);
                if ((pass == 0) != is_dir) {
                    continue;
                }
                if (count >= FILES_LIST_MAX) {
                    truncated = true;
                    break;
                }
                cJSON *item = cJSON_CreateObject();
                if (item == NULL) {
                    closedir(d);
                    cJSON_Delete(root);
                    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "No memory");
                    return ESP_FAIL;
                }
                cJSON_AddStringToObject(item, "name", e->d_name);
                cJSON_AddBoolToObject(item, "dir", is_dir);
                if (!is_dir) {
                    cJSON_AddNumberToObject(item, "size", (double)st.st_size);
                }
                cJSON_AddItemToArray(entries, item);
                count++;
            }
            closedir(d);
        }
    }
    if (truncated) {
        cJSON_ReplaceItemInObject(root, "truncated", cJSON_CreateBool(true));
    }

    webserver_send_json(req, "200 OK", root);
    cJSON_Delete(root);
    return ESP_OK;
}

/** POST /api/files/mkdir —— 在指定目录下新建文件夹（表单 path + name）。 */
esp_err_t webserver_handle_files_mkdir_post(httpd_req_t *req) {
    if (webserver_require_auth(req) != ESP_OK) {
        return ESP_FAIL;
    }
    char *body = files_read_form_body(req);
    if (body == NULL) {
        return ESP_FAIL;
    }
    char abs[FILES_ABS_MAX];
    char name[FILES_REL_MAX];
    bool ok = files_resolve_dir(req, body, abs, sizeof(abs)) &&
              files_name_field(req, body, "name", name, sizeof(name));
    free(body);
    if (!ok) {
        return ESP_FAIL;
    }
    if (!espaperplay_storage_is_mounted()) {
        webserver_send_json_err(req, "SD 卡未挂载");
        return ESP_FAIL;
    }

    char target[FILES_ABS_MAX];
    if (!files_abs_join(target, sizeof(target), abs, name)) {
        webserver_send_json_err(req, "路径过长");
        return ESP_FAIL;
    }
    struct stat st;
    if (stat(target, &st) == 0) {
        webserver_send_json_err(req, "同名条目已存在");
        return ESP_FAIL;
    }
    if (mkdir(target, 0775) != 0) {
        webserver_send_json_err(req, "创建失败（目录可能已满或只读）");
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "mkdir: %s", target);
    files_send_ok(req);
    return ESP_OK;
}

/** POST /api/files/rename —— 重命名（表单 path + from + to；目标已存在报错）。 */
esp_err_t webserver_handle_files_rename_post(httpd_req_t *req) {
    if (webserver_require_auth(req) != ESP_OK) {
        return ESP_FAIL;
    }
    char *body = files_read_form_body(req);
    if (body == NULL) {
        return ESP_FAIL;
    }
    char abs[FILES_ABS_MAX];
    char from[FILES_REL_MAX];
    char to[FILES_REL_MAX];
    bool ok = files_resolve_dir(req, body, abs, sizeof(abs)) &&
              files_name_field(req, body, "from", from, sizeof(from)) &&
              files_name_field(req, body, "to", to, sizeof(to));
    free(body);
    if (!ok) {
        return ESP_FAIL;
    }
    if (!espaperplay_storage_is_mounted()) {
        webserver_send_json_err(req, "SD 卡未挂载");
        return ESP_FAIL;
    }

    char src[FILES_ABS_MAX];
    char dst[FILES_ABS_MAX];
    if (!files_abs_join(src, sizeof(src), abs, from) ||
        !files_abs_join(dst, sizeof(dst), abs, to)) {
        webserver_send_json_err(req, "路径过长");
        return ESP_FAIL;
    }
    struct stat st;
    if (stat(src, &st) != 0) {
        webserver_send_json_err(req, "源条目不存在");
        return ESP_FAIL;
    }
    if (stat(dst, &st) == 0) {
        webserver_send_json_err(req, "目标名称已存在"); /* POSIX rename 会静默覆盖，须拦截 */
        return ESP_FAIL;
    }
    if (rename(src, dst) != 0) {
        webserver_send_json_err(req, "重命名失败");
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "rename: %s -> %s", src, to);
    files_send_ok(req);
    return ESP_OK;
}

/** POST /api/files/delete —— 删除文件 / 递归删除目录（表单 path + name）。 */
esp_err_t webserver_handle_files_delete_post(httpd_req_t *req) {
    if (webserver_require_auth(req) != ESP_OK) {
        return ESP_FAIL;
    }
    char *body = files_read_form_body(req);
    if (body == NULL) {
        return ESP_FAIL;
    }
    char abs[FILES_ABS_MAX];
    char name[FILES_REL_MAX];
    bool ok = files_resolve_dir(req, body, abs, sizeof(abs)) &&
              files_name_field(req, body, "name", name, sizeof(name));
    free(body);
    if (!ok) {
        return ESP_FAIL;
    }
    if (!espaperplay_storage_is_mounted()) {
        webserver_send_json_err(req, "SD 卡未挂载");
        return ESP_FAIL;
    }

    char target[FILES_ABS_MAX];
    if (!files_abs_join(target, sizeof(target), abs, name)) {
        webserver_send_json_err(req, "路径过长");
        return ESP_FAIL;
    }
    struct stat st;
    if (stat(target, &st) != 0) {
        webserver_send_json_err(req, "条目不存在");
        return ESP_FAIL;
    }
    esp_err_t err = files_rm_rf(target, 0);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "delete failed (%s): %s", target, esp_err_to_name(err));
        webserver_send_json_err(req, S_ISDIR(st.st_mode) ? "删除失败（目录过深或部分内容无法删除）"
                                                         : "删除失败");
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "deleted: %s", target);
    files_send_ok(req);
    return ESP_OK;
}

/* ------------------------------------------------------------------ */
/* 上传 / 下载                                                          */
/* ------------------------------------------------------------------ */

/**
 * 从查询串解析 path + name 并构造目标绝对路径（上传 / 下载共用）。
 * 失败已响应错误并返回 false。
 */
static bool files_resolve_query_target(httpd_req_t *req, char *target, size_t target_size,
                                       bool *out_overwrite) {
    char query[FILES_QUERY_MAX] = {0};
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK) {
        webserver_send_json_err(req, "缺少查询参数");
        return false;
    }
    char raw_path[FILES_REL_MAX] = "";
    char rel[FILES_REL_MAX] = "";
    char name[FILES_REL_MAX] = "";
    if (!webserver_form_get_field(query, "path", raw_path, sizeof(raw_path)) ||
        !files_rel_normalize(raw_path, rel, sizeof(rel))) {
        webserver_send_json_err(req, "缺少或非法目录 path");
        return false;
    }
    if (!webserver_form_get_field(query, "name", name, sizeof(name)) || !files_name_valid(name)) {
        webserver_send_json_err(req, "缺少或非法名称 name");
        return false;
    }
    if (out_overwrite != NULL) {
        char flag[8] = "";
        *out_overwrite =
            (webserver_form_get_field(query, "overwrite", flag, sizeof(flag)) && flag[0] == '1');
    }

    char abs[FILES_ABS_MAX];
    if (!files_abs_path(rel, abs, sizeof(abs)) || !files_abs_join(target, target_size, abs, name)) {
        webserver_send_json_err(req, "路径过长");
        return false;
    }
    return true;
}

/** GET /api/files/download?path=/sub&name=file —— 分块流式下载（附件）。 */
esp_err_t webserver_handle_files_download_get(httpd_req_t *req) {
    if (webserver_require_auth(req) != ESP_OK) {
        return ESP_FAIL;
    }
    char target[FILES_ABS_MAX];
    if (!files_resolve_query_target(req, target, sizeof(target), NULL)) {
        return ESP_FAIL;
    }
    if (!espaperplay_storage_is_mounted()) {
        webserver_send_json_err(req, "SD 卡未挂载");
        return ESP_FAIL;
    }
    struct stat st;
    const char *dl_name = strrchr(target, '/') + 1; /* 目标由本函数拼接，必有 '/' */
    if (stat(target, &st) != 0 || S_ISDIR(st.st_mode)) {
        webserver_send_json_err(req, "文件不存在");
        return ESP_FAIL;
    }
    FILE *f = fopen(target, "rb");
    if (f == NULL) {
        webserver_send_json_err(req, "无法打开文件");
        return ESP_FAIL;
    }

    /* 响应头：MIME 按扩展名 + 附件下载（filename* 支持非 ASCII 文件名）。
     * 头值缓冲须存活到响应结束，放本函数栈上即可。 */
    httpd_resp_set_type(req, files_mime_by_ext(dl_name));
    char disposition[FILES_ABS_MAX + 64];
    files_content_disposition(dl_name, disposition, sizeof(disposition));
    httpd_resp_set_hdr(req, "Content-Disposition", disposition);

    /* 分块读取发送（首块自动携带响应头，末块以 NULL 长度收尾）。 */
    char buf[FILES_IO_CHUNK];
    size_t total = 0;
    size_t n;
    esp_err_t err = ESP_OK;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) {
        if (httpd_resp_send_chunk(req, buf, n) != ESP_OK) {
            err = ESP_FAIL; /* 客户端中断（取消下载）属常态，静默退出 */
            break;
        }
        total += n;
    }
    fclose(f);
    if (err == ESP_OK && httpd_resp_send_chunk(req, NULL, 0) != ESP_OK) {
        err = ESP_FAIL;
    }
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "downloaded: %s (%u bytes)", target, (unsigned)total);
    } else {
        ESP_LOGW(TAG, "download aborted: %s (%u bytes sent)", target, (unsigned)total);
    }
    return err;
}

/** POST /api/files/upload?path=/sub&name=file[&overwrite=1] —— 原始字节流式上传。 */
esp_err_t webserver_handle_files_upload_post(httpd_req_t *req) {
    if (webserver_require_auth(req) != ESP_OK) {
        return ESP_FAIL;
    }
    char target[FILES_ABS_MAX];
    bool overwrite = false;
    if (!files_resolve_query_target(req, target, sizeof(target), &overwrite)) {
        return ESP_FAIL;
    }
    if (!espaperplay_storage_is_mounted()) {
        webserver_send_json_err(req, "SD 卡未挂载，无法保存文件");
        return ESP_FAIL;
    }

    struct stat st;
    if (!overwrite && stat(target, &st) == 0) {
        webserver_send_json_err(req, "目标已存在"); /* 覆盖需显式 overwrite=1（前端先确认） */
        return ESP_FAIL;
    }
    const int total = req->content_len;
    if (total <= 0) {
        webserver_send_json_err(req, "空文件");
        return ESP_FAIL;
    }

    FILE *f = fopen(target, "wb");
    if (f == NULL) {
        webserver_send_json_err(req, "无法创建文件（目录可能只读）");
        return ESP_FAIL;
    }

    /* 分块读取请求体写入 SD（同字体上传：大文件不驻留 RAM）。 */
    char buf[FILES_IO_CHUNK];
    int received = 0;
    while (received < total) {
        int chunk = total - received;
        if (chunk > (int)sizeof(buf)) {
            chunk = (int)sizeof(buf);
        }
        int r = httpd_req_recv(req, buf, (size_t)chunk);
        if (r <= 0) {
            fclose(f);
            remove(target); /* 半途而废的残留一并清理 */
            webserver_send_json_err(req, "读取请求体失败");
            return ESP_FAIL;
        }
        received += r;
        if (fwrite(buf, 1, (size_t)r, f) != (size_t)r) {
            fclose(f);
            remove(target);
            webserver_send_json_err(req, "写入 SD 卡失败");
            return ESP_FAIL;
        }
    }
    fclose(f);

    ESP_LOGI(TAG, "uploaded: %s (%d bytes)", target, received);
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
