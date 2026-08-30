/*
 * SPDX-FileCopyrightText: 2026 ESPaperPlay Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#include <dirent.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#include "esp_heap_caps.h"
#include "esp_log.h"

#include "espaperplay_config.h"
#include "espaperplay_fonts.h"
#include "espaperplay_gui_lv.h"
#include "espaperplay_input.h"
#include "espaperplay_storage.h"
#include "espaperplay_system.h"
#include "espaperplay_ui.h"
#include "espaperplay_ui_touch.h"

#include "lvgl.h"

static const char *TAG = "ESPaperPlay_UI";

/* ====================================================================
 * 文件管理页（SD 卡基本文件操作，UI 风格与设置页统一）
 * ====================================================================
 *
 * 布局（自上而下）：统一状态栏（"文件"）+ 相对路径条 + 分页列表卡片 +
 * 页面指示点 + 底部操作栏（上一级 / 新建文件夹）。墨水屏不适合滚动，
 * 目录条目按行高分页展示，左右滑动切换分页（与主界面 / 设置页一致）。
 *
 * 操作：
 *   - 点击目录进入；点击普通文件弹出详情（类型 / 大小 / 修改时间）；
 *     底部「上一级」返回上级（根目录下无操作）；
 *   - 底部「新建文件夹」：键盘输入名称 -> 二次确认 -> 创建；
 *   - 长按条目弹上下文菜单：重命名（键盘输入 -> 二次确认）/ 删除
 *     （二次确认，文件夹递归删除并明确警示不可恢复）；
 *   - 所有写操作（mkdir / rename / unlink / 递归 rmdir）投递到专用
 *     worker 任务执行（LVGL 任务栈在 PSRAM，flash 操作禁用缓存期间
 *     无法访问 PSRAM 栈，见设置页同款处理），完成后回 LVGL 线程重扫
 *     刷新；readdir / stat 只读操作留在 LVGL 线程。
 *
 * 手势与其他页面一致：屏幕左右边缘 24px 向内滑动（>70px、横向为主）
 * 返回主页；中间横向滑动（>90px、横纵比 1.2）切换分页；小位移
 * （<=15px）点击触发条目 / 按钮。长按（600ms、位移 <=15px）弹菜单：
 * 在 on_touch 触摸帧流中判定（不用 lv_timer——one-shot 定时器触发后
 * 指针悬空且时机受渲染调度影响），触发后锁存等待物理释放，避免松手
 * 误入目录或误点菜单。
 *
 * 实机坑（LVGL 9.5 indev 行为）：长按弹菜单发生在"按住期间"，指针
 * indev 每个读取周期都会重新搜索按压对象，菜单出现在手指下方后会被
 * 重新锁定；抬起/反弹帧在其上产生新鲜 CLICKED，冒泡误关菜单甚至误触
 * 菜单按钮。对策：模态内点击抑制窗口 = max(打开后 300ms，观察到物理
 * 释放后再延 150ms)——仅靠固定时间窗不够：按住超过 300ms 后才松手
 * （位置不变）时窗口已过期，释放仍会被判成覆盖层上的新鲜点击而误关
 * 菜单；故必须等到本次笔画完整释放。files_modal_click_ok() 在覆盖层
 * 与全部按钮回调入口统一调用。仅"按住期间打开"的模态需要抑制；由
 * 抬起点击打开的模态不受影响。
 *
 * 文本输入用 LVGL 键盘（lv_keyboard，设备无输入法仅 ASCII 可输；已有
 * 名称中的中文保留显示、可追加后缀改名）。输入模态不可点空白关闭
 * （防误触丢输入），必须显式取消 / 确定。
 */

#define FILES_EDGE_PX 24                 /* 边缘滑动触发宽度（物理手势，不缩放） */
#define FILES_EDGE_SWIPE_PX 70           /* 边缘向内滑动位移阈值（不缩放） */
#define FILES_SWIPE_PX 90                /* 分页切换位移阈值（不缩放） */
#define FILES_CLICK_MAX_PX 15            /* 点击允许的最大位移（防抖，不缩放） */
#define FILES_SWIPE_MIN_RATIO 1.2f       /* 横向位移 / 纵向位移 最小比例 */
#define FILES_MARGIN 16                  /* 卡片与屏幕边缘间距 */
#define FILES_LONG_PRESS_MS 600          /* 长按判定时长 */
#define FILES_MODAL_GUARD_MS 300         /* 按住期间弹出模态的点击抑制下限 */
#define FILES_MODAL_RELEASE_GRACE_MS 150 /* 观察到物理释放后的额外抑制宽限 */

/* ---- 尺寸缩放（与设置页同方案：垂直尺寸按屏高缩放并设下限） ---- */
#define FILES_REF_H 800       /* 基准逻辑高度（缩放参考） */
#define FILES_PATH_H 40       /* 路径条高度（基准） */
#define FILES_ROW_H 52        /* 列表行高度（基准） */
#define FILES_BOTTOM_H 68     /* 底部操作栏高度（基准） */
#define FILES_MIN_H 30        /* 路径条最小高度 */
#define FILES_MIN_ROW_H 40    /* 行最小高度 */
#define FILES_MIN_BOTTOM_H 56 /* 底部操作栏最小高度 */

/* ---- 容量上限 ---- */
#define FILES_PATH_MAX 256    /* 绝对路径缓冲（含 NUL） */
#define FILES_NAME_MAX 256    /* 条目名缓冲（FAT LFN 上限 255 UTF-8 字节 + NUL） */
#define FILES_DISP_MAX 64     /* 弹窗文案中的显示名截断长度 */
#define FILES_MAX_ENTRIES 128 /* 单目录条目数上限（超出截断并日志提示） */
#define FILES_ROWS_MAX 16     /* 单页行数上限（按最小行高估算的防御值） */
#define FILES_PAGE_MAX 32     /* 分页数上限（防御） */
#define FILES_RM_DEPTH_MAX 8  /* 递归删除深度上限 */

#define FILES_WORKER_STACK 6144 /* worker 任务栈（内部 RAM） */
#define FILES_QUEUE_LEN 4       /* 写操作队列长度 */

/** SD 卡挂载点（文件管理根目录）。 */
#define FILES_ROOT ESPAPERPLAY_STORAGE_MOUNT_POINT

/** 当前选用字体（SD 优先，缺则回退 Flash 子集；与各页面共用缓存键）。 */
#define FILES_FONT_NAME (espaperplay_system_get_config()->selected_font)

/* ------------------------------------------------------------------ */
/* 类型                                                                 */
/* ------------------------------------------------------------------ */

/** 目录条目（名字存全量，显示时另行截断）。 */
typedef struct {
    char name[FILES_NAME_MAX]; /*!< 条目名（不含路径） */
    bool is_dir;               /*!< 是否目录 */
} files_entry_t;

/** 键盘输入用途（新建 / 重命名共用同一输入模态）。 */
typedef enum {
    FILES_INPUT_MKDIR = 0, /*!< 新建文件夹：输入新名称 */
    FILES_INPUT_RENAME,    /*!< 重命名：预填旧名，输入新名称 */
} files_input_mode_t;

/** 待执行操作（二次确认通过后由 worker 执行）。 */
typedef enum {
    FILES_PENDING_NONE = 0,
    FILES_PENDING_MKDIR,  /*!< 创建文件夹（s_pending_new） */
    FILES_PENDING_RENAME, /*!< 重命名（s_pending_old -> s_pending_new） */
    FILES_PENDING_DELETE, /*!< 删除（s_pending_old，是否目录见 s_pending_is_dir） */
} files_pending_op_t;

/** 写操作类型（worker 执行）。 */
typedef enum {
    FILES_WOP_MKDIR = 0, /*!< 创建目录（path_a） */
    FILES_WOP_RENAME,    /*!< 重命名（path_a -> path_b） */
    FILES_WOP_DELETE,    /*!< 删除（path_a，文件 unlink / 目录递归 rmdir） */
} files_wop_t;

/** worker 操作项（路径为绝对路径副本，跨任务安全）。 */
typedef struct {
    files_wop_t type;            /*!< 操作类型 */
    char path_a[FILES_PATH_MAX]; /*!< mkdir 目标 / rename 旧路径 / delete 目标 */
    char path_b[FILES_PATH_MAX]; /*!< rename 新路径（其余不用） */
} files_work_t;

/** worker 执行结果（堆分配，经 gui_lv_call 回 LVGL 线程后释放）。 */
typedef struct {
    bool ok;          /*!< 是否成功 */
    files_wop_t type; /*!< 操作类型 */
    int err_no;       /*!< 失败时的 errno（仅提示/日志用） */
} files_result_t;

/** 上下文菜单按钮标识。 */
typedef enum {
    FILES_MENU_RENAME = 0,
    FILES_MENU_DELETE,
    FILES_MENU_CANCEL,
} files_menu_action_t;

/* ------------------------------------------------------------------ */
/* 页面状态                                                             */
/* ------------------------------------------------------------------ */

static files_entry_t *s_entries = NULL;         /*!< 目录条目数组（PSRAM，enter 分配） */
static int s_count = 0;                         /*!< 条目数 */
static char s_cwd[FILES_PATH_MAX] = FILES_ROOT; /*!< 当前目录（绝对路径） */

static espaperplay_ui_status_bar_t *s_bar = NULL; /*!< 统一状态栏 */
static lv_obj_t *s_path_label = NULL;             /*!< 路径条文本 */
static lv_obj_t *s_hint_label = NULL;             /*!< 空目录 / 未挂载提示 */
static lv_obj_t *s_btn_up = NULL;                 /*!< 底部按钮：上一级 */
static lv_obj_t *s_btn_mkdir = NULL;              /*!< 底部按钮：新建文件夹 */

static lv_obj_t *s_page_objs[FILES_PAGE_MAX]; /*!< 分页容器 */
static bool s_page_built[FILES_PAGE_MAX];     /*!< 分页是否已构建（惰性） */
static lv_obj_t *s_dots[FILES_PAGE_MAX];      /*!< 页面指示点 */
static int s_page = 0;                        /*!< 当前分页 */
static int s_page_count = 1;                  /*!< 分页总数 */
static int s_per_page = 8;                    /*!< 每页行数（enter 时计算） */

static lv_obj_t *s_row_objs[FILES_ROWS_MAX]; /*!< 当前已构建页的行对象 */
static int s_row_idx[FILES_ROWS_MAX];        /*!< 行 -> 条目下标 */
static int s_row_cnt = 0;                    /*!< 当前页行数 */

static float s_scale = 1.0f; /*!< 屏高缩放因子（enter 时计算） */
static int s_bar_h = 30;     /*!< 状态栏高度 */
static int s_path_y = 0;     /*!< 路径条顶部 y */
static int s_path_h = 40;    /*!< 路径条高度 */
static int s_list_y = 0;     /*!< 列表卡片顶部 y */
static int s_list_h = 0;     /*!< 列表卡片高度 */
static int s_card_w = 0;     /*!< 卡片宽度（enter 时算好：布局完成前读宽为 0） */
static int s_row_h = 52;     /*!< 行高（缩放后） */
static int s_bottom_y = 0;   /*!< 底部操作栏顶部 y */
static int s_btn_w = 0;      /*!< 底部按钮宽 */
static int s_btn_h = 0;      /*!< 底部按钮高 */

static bool s_active = false; /*!< 页面是否在前台（worker 回调访问 UI 前检查） */
static bool s_busy = false;   /*!< 写操作进行中（防并发投递） */

/* 模态状态 */
static lv_obj_t *s_modal = NULL;           /*!< 模态覆盖层（NULL=未打开） */
static uint32_t s_modal_guard_until = 0;   /*!< 模态内点击抑制截止 tick（0=不抑制） */
static bool s_modal_track_release = false; /*!< 按住期间打开的模态：等待观察其后的首次物理释放 */
static lv_obj_t *s_kb_ta = NULL;           /*!< 输入框（键盘模态） */
static lv_obj_t *s_kb_status = NULL;       /*!< 名称校验提示标签 */
static files_input_mode_t s_input_mode = FILES_INPUT_MKDIR;  /*!< 键盘模态用途 */
static files_pending_op_t s_pending_op = FILES_PENDING_NONE; /*!< 待执行操作 */
static char s_pending_old[FILES_NAME_MAX] = "";              /*!< 原名（重命名 / 删除目标） */
static char s_pending_new[FILES_NAME_MAX] = "";              /*!< 新名（新建 / 重命名目标） */
static bool s_pending_is_dir = false;                        /*!< 待删除目标是否目录 */

/* 手势跟踪 */
static bool s_touch_down = false;
static bool s_await_release = false; /*!< 长按触发后锁存：忽略一切按下帧直到物理释放 */
static lv_point_t s_touch_start = {0, 0};
static lv_point_t s_touch_last = {0, 0};
static uint32_t s_touch_down_tick = 0; /*!< 按下时刻（点击时长门槛用） */
static int s_touch_hit = -1;           /*!< 按下起点命中的条目下标（-1=无） */
static int s_touch_btn = -1;           /*!< 按下起点命中的底部按钮（-1=无 0=上一级 1=新建） */

/* worker */
static QueueHandle_t s_queue = NULL;
static TaskHandle_t s_worker_handle = NULL;

/* 前向声明 */
static void files_modal_close(void);
static void files_scan(void);
static void files_confirm_open(const char *title, const char *msg, bool alert_only);
static void files_keyboard_open(files_input_mode_t mode, const char *init_text);
static void files_info_open(int idx);

/* ------------------------------------------------------------------ */
/* 工具函数                                                             */
/* ------------------------------------------------------------------ */

/** FreeType 字体按需加载（16 / 20 / 24 档，与各页面共用缓存）。 */
static lv_font_t *files_font(int size_px) {
    const char *name = FILES_FONT_NAME[0] ? FILES_FONT_NAME : ESPAPERPLAY_FONTS_DEFAULT_NAME;
    return espaperplay_fonts_load(name, (uint32_t)size_px, ESPAPERPLAY_FONT_STYLE_NORMAL);
}

/** 通用标签：白底黑字 + FreeType 字体 + 禁用滚动。 */
static lv_obj_t *files_label_create(lv_obj_t *parent, const char *text, int font_px,
                                    lv_text_align_t align) {
    lv_obj_t *label = lv_label_create(parent);
    lv_label_set_text(label, text);
    lv_obj_set_style_text_color(label, lv_color_black(), 0);
    lv_obj_set_style_text_font(label, files_font(font_px), 0);
    lv_obj_set_style_text_align(label, align, 0);
    lv_obj_set_width(label, LV_PCT(100));
    lv_obj_remove_flag(label, LV_OBJ_FLAG_SCROLLABLE);
    return label;
}

/** 逻辑分辨率（旋转后）。 */
static void files_screen_size(int32_t *out_w, int32_t *out_h) {
    lv_display_t *disp = lv_display_get_default();
    *out_w = lv_display_get_horizontal_resolution(disp);
    *out_h = lv_display_get_vertical_resolution(disp);
}

/** 基准值按屏高缩放（取整）。 */
static int files_scaled(int v) { return (int)(v * s_scale); }

/** 弹窗按钮标准高度（缩放 + 下限）。 */
static int files_btn_h(void) {
    const int h = files_scaled(44);
    return h < 38 ? 38 : h;
}

/** 弹窗卡片宽度（与 files_modal_base 同一公式：布局完成前读宽为 0，
 * 调用方一律用本函数算术求得，勿读对象宽度）。 */
static int files_modal_card_w(void) {
    int32_t scr_w = 0;
    int32_t scr_h = 0;
    files_screen_size(&scr_w, &scr_h);
    const int avail_w = scr_w - 2 * FILES_MARGIN;
    return avail_w < files_scaled(360) ? avail_w : files_scaled(360);
}

/** 点在矩形内（逻辑坐标）。 */
static bool files_point_in(const lv_point_t *p, int x, int y, int w, int h) {
    return p->x >= x && p->x < x + w && p->y >= y && p->y < y + h;
}

/** 对象相对屏幕的坐标（累加父级偏移；LVGL 的 get_x/y 只返回相对父）。 */
static int files_obj_screen_x(const lv_obj_t *obj) {
    int x = 0;
    const lv_obj_t *p = obj;
    while (p != NULL && lv_obj_get_parent(p) != NULL) {
        x += lv_obj_get_x(p);
        p = lv_obj_get_parent(p);
    }
    return x;
}

static int files_obj_screen_y(const lv_obj_t *obj) {
    int y = 0;
    const lv_obj_t *p = obj;
    while (p != NULL && lv_obj_get_parent(p) != NULL) {
        y += lv_obj_get_y(p);
        p = lv_obj_get_parent(p);
    }
    return y;
}

/**
 * 显示名截断：UTF-8 边界安全截断到 @p n 字节内（含省略号），保证弹窗
 * 文案长度上界可证（-Werror=format-truncation 下 snprintf 拼 %s 必报错，
 * 故路径拼接一律 strlcpy/strlcat、文案拼接先截断名称再拼固定短语）。
 */
static void files_disp_name(const char *src, char *dst, size_t n) {
    size_t len = strlen(src);
    bool truncated = false;
    if (len > n - 4) { /* 预留 "…"（3 字节）+ NUL */
        len = n - 4;
        while (len > 0 && (((unsigned char)src[len] & 0xC0) == 0x80)) {
            len--; /* 回退到 UTF-8 字符边界 */
        }
        truncated = true;
    }
    memcpy(dst, src, len);
    dst[len] = '\0';
    if (truncated) {
        strlcat(dst, "…", n);
    }
}

/** 条目名合法性：非空、非 "."/".."、不含 '/' 与控制字符（UTF-8 直通）。 */
static bool files_name_valid(const char *s) {
    size_t n = strlen(s);
    if (n == 0 || n >= FILES_NAME_MAX) {
        return false;
    }
    if (strcmp(s, ".") == 0 || strcmp(s, "..") == 0) {
        return false;
    }
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        if (c == '/' || c < 0x20 || c == 0x7F) {
            return false;
        }
    }
    return true;
}

/** 路径拼接 a + "/" + b（strlcpy/strlcat 恒 NUL 结尾，规避格式化告警）。 */
static void files_path_join(char *dst, size_t n, const char *a, const char *b) {
    strlcpy(dst, a, n);
    strlcat(dst, "/", n);
    strlcat(dst, b, n);
}

/** 取当前目录的相对显示路径（根目录显示 "/"）。 */
static const char *files_rel_path(void) {
    const char *rel = s_cwd + strlen(FILES_ROOT);
    return (*rel == '\0') ? "/" : rel;
}

/** 条目排序：目录优先，其余按字节序（UTF-8 字节序稳定）。 */
static int files_entry_cmp(const void *a, const void *b) {
    const files_entry_t *x = a;
    const files_entry_t *y = b;
    if (x->is_dir != y->is_dir) {
        return x->is_dir ? -1 : 1;
    }
    return strcmp(x->name, y->name);
}

/* ------------------------------------------------------------------ */
/* 写操作 worker 任务（内部 RAM 栈，flash 缓存坑同设置页）                 */
/* ------------------------------------------------------------------ */

/**
 * LVGL 任务栈位于 PSRAM（见 lvgl_port.c），SD 卡写操作期间 FatFs /
 * SDMMC 会禁用 flash 缓存，此时 CPU 无法访问 PSRAM——在 LVGL 任务里直接
 * 执行写操作会触发栈合法性断言。因此写操作一律投递到本任务（内部 RAM
 * 栈）执行，完成后经 espaperplay_gui_lv_call 回 LVGL 线程重扫刷新。
 */

/** 递归删除目录 / 文件（深度受限，防御异常嵌套）。 */
static esp_err_t files_rm_rf(const char *path, int depth) {
    if (depth > FILES_RM_DEPTH_MAX) {
        ESP_LOGE(TAG, "files: rm depth limit exceeded (%s)", path);
        return ESP_ERR_INVALID_STATE;
    }
    struct stat st;
    if (stat(path, &st) != 0) {
        return ESP_FAIL;
    }
    if (!S_ISDIR(st.st_mode)) {
        return unlink(path) == 0 ? ESP_OK : ESP_FAIL;
    }
    DIR *d = opendir(path);
    if (d == NULL) {
        return ESP_FAIL;
    }
    esp_err_t ret = ESP_OK;
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0) {
            continue;
        }
        char child[FILES_PATH_MAX];
        files_path_join(child, sizeof(child), path, e->d_name);
        esp_err_t sub = files_rm_rf(child, depth + 1);
        if (sub != ESP_OK) {
            ret = sub; /* 尽力删完其余条目，最后统一报失败 */
        }
    }
    closedir(d);
    if (ret == ESP_OK && rmdir(path) != 0) {
        ret = ESP_FAIL;
    }
    return ret;
}

/** 回 LVGL 线程：应用写结果（重扫刷新 / 失败提示）。 */
static void files_op_done_lv(void *arg) {
    files_result_t *r = arg;
    s_busy = false;
    if (s_active) {
        files_scan(); /* 重扫当前目录并重建分页（含指示点） */
        if (!r->ok) {
            static const char *const op_names[] = {"创建文件夹", "重命名", "删除"};
            char msg[64];
            snprintf(msg, sizeof(msg), "%s失败（errno=%d）", op_names[r->type], r->err_no);
            files_confirm_open("操作失败", msg, true);
        }
    }
    heap_caps_free(r);
}

/** worker 任务：取队列操作执行，结果回投 LVGL 线程。 */
static void files_worker_task_fn(void *arg) {
    (void)arg;
    files_work_t w;
    for (;;) {
        if (xQueueReceive(s_queue, &w, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        errno = 0;
        esp_err_t err;
        if (w.type == FILES_WOP_MKDIR) {
            err = mkdir(w.path_a, 0775) == 0 ? ESP_OK : ESP_FAIL;
        } else if (w.type == FILES_WOP_RENAME) {
            err = rename(w.path_a, w.path_b) == 0 ? ESP_OK : ESP_FAIL;
        } else {
            err = files_rm_rf(w.path_a, 0);
        }
        const int err_no = errno;
        if (err == ESP_OK) {
            ESP_LOGI(TAG, "files: wop %d ok (%s)", (int)w.type, w.path_a);
        } else {
            ESP_LOGW(TAG, "files: wop %d failed (%s): errno=%d", (int)w.type, w.path_a, err_no);
        }

        files_result_t *r = heap_caps_malloc(sizeof(*r), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        if (r == NULL) {
            /* 结果分配失败：仅记日志（操作实际已执行，下次进入页面会刷新）。 */
            s_busy = false;
            continue;
        }
        r->ok = (err == ESP_OK);
        r->type = w.type;
        r->err_no = err_no;
        if (espaperplay_gui_lv_call(files_op_done_lv, r, 500) != ESP_OK) {
            heap_caps_free(r); /* 投递失败自行释放，避免泄漏 */
            s_busy = false;
        }
    }
}

/** 确保 worker 任务与队列已创建（首次使用时创建，常驻程序生命周期）。 */
static void files_worker_ensure(void) {
    if (s_queue != NULL && s_worker_handle != NULL) {
        return;
    }
    if (s_queue == NULL) {
        s_queue = xQueueCreate(FILES_QUEUE_LEN, sizeof(files_work_t));
    }
    if (s_queue != NULL && s_worker_handle == NULL) {
        /* 栈必须放内部 RAM（flash 操作禁用缓存期间可访问）；CONFIG_SPIRAM_USE_MALLOC
         * 下 xTaskCreate 默认栈在 PSRAM，须用 xTaskCreateWithCaps 显式指定。 */
        if (xTaskCreateWithCaps(files_worker_task_fn, "ui_files", FILES_WORKER_STACK, NULL, 4,
                                &s_worker_handle,
                                MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT) != pdPASS) {
            ESP_LOGE(TAG, "files: worker task create failed");
        }
    }
}

/** 投递一个写操作（LVGL 线程内调用，非阻塞；进行中则忽略新操作）。 */
static void files_work_post(files_pending_op_t op) {
    files_worker_ensure();
    if (s_queue == NULL || s_worker_handle == NULL) {
        return;
    }
    if (s_busy) {
        ESP_LOGW(TAG, "files: busy, drop op %d", (int)op);
        return;
    }
    files_work_t w = {0};
    switch (op) {
    case FILES_PENDING_MKDIR:
        w.type = FILES_WOP_MKDIR;
        files_path_join(w.path_a, sizeof(w.path_a), s_cwd, s_pending_new);
        break;
    case FILES_PENDING_RENAME:
        w.type = FILES_WOP_RENAME;
        files_path_join(w.path_a, sizeof(w.path_a), s_cwd, s_pending_old);
        files_path_join(w.path_b, sizeof(w.path_b), s_cwd, s_pending_new);
        break;
    case FILES_PENDING_DELETE:
        w.type = FILES_WOP_DELETE;
        files_path_join(w.path_a, sizeof(w.path_a), s_cwd, s_pending_old);
        break;
    default:
        return;
    }
    s_busy = true;
    if (xQueueSend(s_queue, &w, 0) != pdTRUE) {
        s_busy = false;
        ESP_LOGW(TAG, "files: op queue full");
    }
}

/* ------------------------------------------------------------------ */
/* 目录扫描 / 分页构建                                                   */
/* ------------------------------------------------------------------ */

/** 删除全部分页容器与指示点（重扫前清理）。 */
static void files_pages_destroy(void) {
    for (int i = 0; i < FILES_PAGE_MAX; i++) {
        if (s_page_built[i] && s_page_objs[i] != NULL) {
            lv_obj_del(s_page_objs[i]);
        }
        s_page_objs[i] = NULL;
        s_page_built[i] = false;
        if (s_dots[i] != NULL) {
            lv_obj_del(s_dots[i]);
            s_dots[i] = NULL;
        }
    }
    for (int i = 0; i < FILES_ROWS_MAX; i++) {
        s_row_objs[i] = NULL;
        s_row_idx[i] = -1;
    }
    s_row_cnt = 0;
}

/** 构建一个分页的全部行控件（首次显示该页时调用）。 */
static void files_page_build(int idx) {
    if (idx < 0 || idx >= s_page_count || idx >= FILES_PAGE_MAX || s_page_built[idx]) {
        return;
    }
    lv_obj_t *page = lv_obj_create(lv_screen_active());
    s_page_objs[idx] = page;
    lv_obj_set_size(page, s_card_w, s_list_h);
    lv_obj_set_pos(page, FILES_MARGIN, s_list_y);
    lv_obj_set_style_bg_color(page, lv_color_white(), 0);
    lv_obj_set_style_border_color(page, lv_color_black(), 0);
    lv_obj_set_style_border_width(page, 2, 0);
    lv_obj_set_style_radius(page, 12, 0);
    lv_obj_set_style_pad_all(page, 0, 0);
    lv_obj_remove_flag(page, LV_OBJ_FLAG_SCROLLABLE);

    const int pad = 10;
    const int start = idx * s_per_page;
    int n = s_count - start;
    if (n > s_per_page) {
        n = s_per_page;
    }
    for (int i = 0; i < n; i++) {
        const files_entry_t *ent = &s_entries[start + i];

        lv_obj_t *row = lv_obj_create(page);
        lv_obj_set_size(row, s_card_w - 2 * pad, s_row_h - 4);
        lv_obj_set_pos(row, pad, pad + i * s_row_h);
        lv_obj_set_style_bg_color(row, lv_color_white(), 0);
        lv_obj_set_style_border_width(row, 0, 0);
        lv_obj_set_style_radius(row, 0, 0);
        lv_obj_set_style_pad_all(row, 0, 0);
        lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);

        /* 名称（目录以 "/" 结尾标记；超长省略号截断） */
        char text[FILES_DISP_MAX + 2];
        files_disp_name(ent->name, text, sizeof(text) - 2);
        if (ent->is_dir) {
            strlcat(text, "/", sizeof(text));
        }
        lv_obj_t *label = files_label_create(row, text, 16, LV_TEXT_ALIGN_LEFT);
        lv_obj_set_width(label, s_card_w - 2 * pad - 20);
        lv_obj_align(label, LV_ALIGN_LEFT_MID, 4, 0);
        lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);

        /* 行间分隔线（最后一行不加） */
        if (i < n - 1) {
            lv_obj_t *sep = lv_obj_create(page);
            lv_obj_set_size(sep, s_card_w - 2 * pad, 1);
            lv_obj_set_pos(sep, pad, pad + (i + 1) * s_row_h - 3);
            lv_obj_set_style_bg_color(sep, lv_color_black(), 0);
            lv_obj_set_style_border_width(sep, 0, 0);
            lv_obj_set_style_radius(sep, 0, 0);
            lv_obj_remove_flag(sep, LV_OBJ_FLAG_SCROLLABLE);
        }

        s_row_objs[i] = row;
        s_row_idx[i] = start + i;
    }
    s_row_cnt = n;
    /* 新建的分页默认盖在当前页之上：非当前页立即隐藏。 */
    if (idx != s_page) {
        lv_obj_add_flag(page, LV_OBJ_FLAG_HIDDEN);
    }
    s_page_built[idx] = true;
}

/** 切换分页：容器显隐 + 指示点刷新（LVGL 线程内）。 */
static void files_show_page(int idx) {
    if (idx < 0 || idx >= s_page_count || idx == s_page) {
        return;
    }
    files_page_build(idx); /* 惰性构建：首次显示才创建控件 */
    s_page = idx;
    for (int i = 0; i < s_page_count; i++) {
        if (!s_page_built[i]) {
            continue;
        }
        if (i == idx) {
            lv_obj_remove_flag(s_page_objs[i], LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(s_page_objs[i], LV_OBJ_FLAG_HIDDEN);
        }
    }
    for (int i = 0; i < s_page_count; i++) {
        lv_obj_set_style_bg_color(s_dots[i], i == idx ? lv_color_black() : lv_color_white(), 0);
    }
    ESP_LOGI(TAG, "files: page %d/%d", idx + 1, s_page_count);
}

/** 扫描当前目录并重建分页（LVGL 线程内；进入页面 / 导航 / 写完成后调用）。 */
static void files_scan(void) {
    files_pages_destroy();

    /* 条目数组分配失败（enter 已提示）：仅保持提示，不再扫描。 */
    if (s_entries == NULL) {
        lv_label_set_text(s_hint_label, "内存不足");
        lv_obj_remove_flag(s_hint_label, LV_OBJ_FLAG_HIDDEN);
        lv_label_set_text(s_path_label, files_rel_path());
        espaperplay_ui_status_bar_refresh(s_bar);
        return;
    }

    /* 清空条目并扫描（readdir/stat 只读，可在 LVGL 线程执行）。 */
    s_count = 0;
    const bool mounted = espaperplay_storage_is_mounted();
    DIR *d = NULL;
    if (mounted) {
        d = opendir(s_cwd);
        while (d == NULL && strcmp(s_cwd, FILES_ROOT) != 0) {
            /* 目录打不开（可能已被外部删除）：逐级回退到根。 */
            ESP_LOGW(TAG, "files: opendir(%s) failed, fallback up", s_cwd);
            char *slash = strrchr(s_cwd, '/');
            if (slash == NULL || slash == s_cwd) {
                strlcpy(s_cwd, FILES_ROOT, sizeof(s_cwd));
            } else {
                *slash = '\0';
            }
            d = opendir(s_cwd);
        }
    }
    if (d != NULL) {
        struct dirent *e;
        while ((e = readdir(d)) != NULL) {
            if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0) {
                continue;
            }
            if (s_count >= FILES_MAX_ENTRIES) {
                ESP_LOGW(TAG, "files: too many entries (%s), truncating", s_cwd);
                break;
            }
            files_entry_t *ent = &s_entries[s_count];
            bool is_dir = (e->d_type == DT_DIR);
            if (e->d_type == DT_UNKNOWN) {
                /* d_type 不可靠时 stat 兜底。 */
                char full[FILES_PATH_MAX];
                files_path_join(full, sizeof(full), s_cwd, e->d_name);
                struct stat st;
                is_dir = (stat(full, &st) == 0 && S_ISDIR(st.st_mode));
            }
            strlcpy(ent->name, e->d_name, sizeof(ent->name));
            ent->is_dir = is_dir;
            s_count++;
        }
        closedir(d);
        qsort(s_entries, s_count, sizeof(files_entry_t), files_entry_cmp);
    }

    /* 提示与路径条 */
    if (!mounted) {
        lv_label_set_text(s_hint_label, "SD 卡未挂载");
        lv_obj_remove_flag(s_hint_label, LV_OBJ_FLAG_HIDDEN);
    } else if (s_count == 0) {
        lv_label_set_text(s_hint_label, "空目录");
        lv_obj_remove_flag(s_hint_label, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(s_hint_label, LV_OBJ_FLAG_HIDDEN);
    }
    lv_label_set_text(s_path_label, files_rel_path());
    /* 提示标签创建于分页卡片之前，重扫重建卡片后会盖住它：置顶保证可见。 */
    lv_obj_move_foreground(s_hint_label);

    /* 分页参数（每页行数按列表高度整除，至少 1 页） */
    s_per_page = (s_list_h - 20) / s_row_h;
    if (s_per_page < 1) {
        s_per_page = 1;
    }
    if (s_per_page > FILES_ROWS_MAX) {
        s_per_page = FILES_ROWS_MAX;
    }
    s_page_count = (s_count + s_per_page - 1) / s_per_page;
    if (s_page_count < 1) {
        s_page_count = 1;
    }
    if (s_page >= s_page_count) {
        s_page = s_page_count - 1;
    }

    /* 指示点（数量随分页变化，每次重扫重建） */
    int32_t scr_w = 0;
    int32_t scr_h = 0;
    files_screen_size(&scr_w, &scr_h);
    for (int i = 0; i < s_page_count && i < FILES_PAGE_MAX; i++) {
        s_dots[i] = lv_obj_create(lv_screen_active());
        lv_obj_set_size(s_dots[i], 10, 10);
        lv_obj_set_pos(s_dots[i], scr_w / 2 + (i - (s_page_count - 1) / 2) * 24 - 5,
                       s_bottom_y - 18);
        lv_obj_set_style_radius(s_dots[i], LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_border_width(s_dots[i], 1, 0);
        lv_obj_set_style_border_color(s_dots[i], lv_color_black(), 0);
        lv_obj_remove_flag(s_dots[i], LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_style_bg_color(s_dots[i], i == s_page ? lv_color_black() : lv_color_white(), 0);
    }

    files_page_build(s_page);
    espaperplay_ui_status_bar_refresh(s_bar);
    ESP_LOGI(TAG, "files: scan %s -> %d entries, %d page(s)", s_cwd, s_count, s_page_count);
}

/* ------------------------------------------------------------------ */
/* 导航                                                                 */
/* ------------------------------------------------------------------ */

/** 进入子目录。 */
static void files_enter_dir(const char *name) {
    char target[FILES_PATH_MAX];
    files_path_join(target, sizeof(target), s_cwd, name);
    strlcpy(s_cwd, target, sizeof(s_cwd));
    s_page = 0;
    files_scan();
}

/** 返回上级目录（根目录下无操作）。 */
static void files_go_up(void) {
    if (strcmp(s_cwd, FILES_ROOT) == 0) {
        return;
    }
    char *slash = strrchr(s_cwd, '/');
    if (slash != NULL && slash != s_cwd) {
        *slash = '\0';
    }
    s_page = 0;
    files_scan();
}

/** 激活条目（点击）：目录进入；普通文件弹出详情（大小 / 修改时间）。 */
static void files_entry_activate(int idx) {
    if (idx < 0 || idx >= s_count) {
        return;
    }
    if (s_entries[idx].is_dir) {
        ESP_LOGI(TAG, "files: enter %s", s_entries[idx].name);
        files_enter_dir(s_entries[idx].name);
    } else {
        ESP_LOGI(TAG, "files: file tapped (%s) -> info", s_entries[idx].name);
        files_info_open(idx);
    }
}

/* ------------------------------------------------------------------ */
/* 模态通用                                                             */
/* ------------------------------------------------------------------ */

/**
 * 模态内点击是否有效。
 *
 * 按住期间打开的模态（长按菜单）：indev 会把手指下方的覆盖层重新锁定为
 * 按压对象，最终抬起产生的新鲜 CLICKED 不应关闭菜单——故抑制截止取
 * 「打开后 300ms」与「观察到物理释放后 +150ms 宽限」的较大者；宽限覆盖
 * indev 读周期滞后与机械反弹帧。由抬起点击打开的模态无抑制。
 */
static bool files_modal_click_ok(void) {
    return s_modal != NULL && lv_tick_get() >= s_modal_guard_until;
}

/** 模态覆盖层点击（点击空白处关闭；键盘输入模态不挂本回调）。 */
static void files_modal_overlay_cb(lv_event_t *e) {
    (void)e;
    if (!files_modal_click_ok()) {
        return;
    }
    files_modal_close();
}

/** 关闭模态（删除覆盖层）。 */
static void files_modal_close(void) {
    if (s_modal != NULL) {
        lv_obj_del(s_modal);
        s_modal = NULL;
    }
    s_kb_ta = NULL;
    s_kb_status = NULL;
    s_modal_guard_until = 0;
    s_modal_track_release = false;
    ESP_LOGI(TAG, "files: modal closed");
}

/**
 * 创建模态骨架：全屏覆盖层 + 居中卡片 + 标题 + 可选消息文本。
 *
 * @param title       标题（20px 居中，超长省略号截断）
 * @param msg         消息（16px 居中可换行，可为 NULL）
 * @param card_h      卡片高度
 * @param click_close 点击空白处是否关闭（输入模态传 false 防丢输入）
 * @param mid_press   是否在按住期间打开（true 时启用 300ms 点击抑制窗口）
 * @return 卡片对象。
 */
static lv_obj_t *files_modal_base(const char *title, const char *msg, int card_h, bool click_close,
                                  bool mid_press) {
    int32_t scr_w = 0;
    int32_t scr_h = 0;
    files_screen_size(&scr_w, &scr_h);

    /* 全屏覆盖层：拦截触摸。浅灰底在 BW 阈值模式下显示为白色（L>=128 判白），
     * 模态靠卡片黑边框区分；GRAY4 模式下显示浅灰。 */
    s_modal = lv_obj_create(lv_screen_active());
    lv_obj_set_size(s_modal, scr_w, scr_h);
    lv_obj_set_pos(s_modal, 0, 0);
    lv_obj_set_style_bg_color(s_modal, lv_color_hex(0xE8E8E8), 0);
    lv_obj_set_style_border_width(s_modal, 0, 0);
    lv_obj_set_style_radius(s_modal, 0, 0);
    lv_obj_set_style_pad_all(s_modal, 0, 0);
    lv_obj_remove_flag(s_modal, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_modal, LV_OBJ_FLAG_CLICKABLE);
    if (click_close) {
        lv_obj_add_event_cb(s_modal, files_modal_overlay_cb, LV_EVENT_CLICKED, NULL);
    }

    const int card_w = files_modal_card_w();
    lv_obj_t *card = lv_obj_create(s_modal);
    lv_obj_set_size(card, card_w, card_h);
    lv_obj_center(card);
    lv_obj_set_style_bg_color(card, lv_color_white(), 0);
    lv_obj_set_style_border_color(card, lv_color_black(), 0);
    lv_obj_set_style_border_width(card, 2, 0);
    lv_obj_set_style_radius(card, 12, 0);
    lv_obj_set_style_pad_all(card, 0, 0);
    lv_obj_remove_flag(card, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *title_label = files_label_create(card, title, 20, LV_TEXT_ALIGN_CENTER);
    lv_obj_set_width(title_label, LV_PCT(100));
    lv_label_set_long_mode(title_label, LV_LABEL_LONG_DOT);
    lv_obj_set_pos(title_label, 0, files_scaled(14));

    if (msg != NULL) {
        lv_obj_t *msg_label = files_label_create(card, msg, 16, LV_TEXT_ALIGN_CENTER);
        lv_obj_set_width(msg_label, card_w - files_scaled(40));
        lv_obj_set_pos(msg_label, files_scaled(20), files_scaled(52));
        lv_label_set_long_mode(msg_label, LV_LABEL_LONG_WRAP);
    }

    /* 按住期间打开的模态：开启点击抑制（下限 300ms，并跟踪首次物理释放
     * 以延长窗口，见 files_modal_click_ok 说明）。 */
    s_modal_guard_until = mid_press ? (lv_tick_get() + FILES_MODAL_GUARD_MS) : 0;
    s_modal_track_release = mid_press;
    return card;
}

/** 在卡片内创建一个按钮（primary: true=黑底白字主按钮，false=白底黑边次按钮）。 */
static lv_obj_t *files_card_button(lv_obj_t *card, const char *text, int x, int y, int w, int h,
                                   bool primary, lv_event_cb_t cb, void *user_data) {
    lv_obj_t *btn = lv_button_create(card);
    lv_obj_set_size(btn, w, h);
    lv_obj_set_pos(btn, x, y);
    if (primary) {
        lv_obj_set_style_bg_color(btn, lv_color_black(), 0);
        lv_obj_set_style_border_width(btn, 0, 0);
    } else {
        lv_obj_set_style_bg_color(btn, lv_color_white(), 0);
        lv_obj_set_style_border_color(btn, lv_color_black(), 0);
        lv_obj_set_style_border_width(btn, 2, 0);
    }
    lv_obj_set_style_radius(btn, 8, 0);
    lv_obj_t *label = lv_label_create(btn);
    lv_label_set_text(label, text);
    lv_obj_set_style_text_color(label, primary ? lv_color_white() : lv_color_black(), 0);
    lv_obj_set_style_text_font(label, files_font(20), 0);
    lv_obj_center(label);
    if (cb != NULL) {
        lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, user_data);
    }
    return btn;
}

/* ------------------------------------------------------------------ */
/* 模态：确认对话框（敏感操作二次确认）/ 提示框                            */
/* ------------------------------------------------------------------ */

/** 确认模态 取消 按钮：关闭，不执行。 */
static void files_confirm_cancel_cb(lv_event_t *e) {
    lv_event_stop_bubbling(e);
    if (!files_modal_click_ok()) {
        return;
    }
    files_modal_close();
}

/** 确认模态 确定 按钮：投递待执行操作到 worker 并关闭。 */
static void files_confirm_ok_cb(lv_event_t *e) {
    lv_event_stop_bubbling(e);
    if (!files_modal_click_ok()) {
        return;
    }
    const files_pending_op_t op = s_pending_op;
    s_pending_op = FILES_PENDING_NONE;
    files_work_post(op);
    files_modal_close();
}

/**
 * 打开确认对话框（标题 + 消息 + 取消/确定；@p alert_only=true 时仅一个
 * "确定" 按钮，用于失败提示）。由抬起后的点击链路打开，无需点击抑制。
 */
static void files_confirm_open(const char *title, const char *msg, bool alert_only) {
    const int card_h = files_scaled(250) < 210 ? 210 : files_scaled(250);
    lv_obj_t *card = files_modal_base(title, msg, card_h, true, false);

    const int card_w = files_modal_card_w(); /* 布局前读宽为 0，须算术求得 */
    const int bh = files_btn_h();
    const int by = card_h - bh - files_scaled(14);
    if (alert_only) {
        files_card_button(card, "确定", files_scaled(24), by, card_w - files_scaled(48), bh, true,
                          files_confirm_ok_cb, NULL);
    } else {
        const int bw = (card_w - files_scaled(60)) / 2;
        files_card_button(card, "取消", files_scaled(24), by, bw, bh, false,
                          files_confirm_cancel_cb, NULL);
        files_card_button(card, "确定", files_scaled(36) + bw, by, bw, bh, true,
                          files_confirm_ok_cb, NULL);
    }
    ESP_LOGI(TAG, "files: confirm modal open (%s)", title);
}

/* ------------------------------------------------------------------ */
/* 模态：文件详情（单击普通文件）                                         */
/* ------------------------------------------------------------------ */

/** 文件大小格式化（B / KB / MB，一位小数；不用 %f 以免依赖浮点 printf）。 */
static void files_fmt_size(uint64_t size, char *buf, size_t n) {
    if (size >= 1024u * 1024u) {
        /* 小数部分 = 余数*10/除数 四舍五入，全程整数运算。 */
        snprintf(buf, n, "%lu.%lu MB", (unsigned long)(size >> 20),
                 (unsigned long)(((size & 0xFFFFFu) * 10u + (1u << 19)) >> 20));
    } else if (size >= 1024u) {
        snprintf(buf, n, "%lu.%lu KB", (unsigned long)(size >> 10),
                 (unsigned long)(((size & 0x3FFu) * 10u + (1u << 9)) >> 10));
    } else {
        snprintf(buf, n, "%lu B", (unsigned long)size);
    }
}

/**
 * 打开文件详情模态（单击普通文件：类型 / 大小 / 修改时间）。
 * stat 只读，可在 LVGL 线程执行；失败时对应项显示「未知」。
 * 由抬起后的点击链路打开，无需点击抑制窗口。
 */
static void files_info_open(int idx) {
    if (idx < 0 || idx >= s_count || s_entries[idx].is_dir) {
        return;
    }
    const files_entry_t *ent = &s_entries[idx];
    char disp[FILES_DISP_MAX];
    files_disp_name(ent->name, disp, sizeof(disp));

    char full[FILES_PATH_MAX];
    files_path_join(full, sizeof(full), s_cwd, ent->name);
    struct stat st;
    char size_line[48];
    char time_line[48];
    if (stat(full, &st) == 0) {
        char size_text[24];
        files_fmt_size((uint64_t)st.st_size, size_text, sizeof(size_text));
        snprintf(size_line, sizeof(size_line), "大小：%s", size_text);
        struct tm tm_buf;
        localtime_r(&st.st_mtime, &tm_buf);
        char time_text[24];
        strftime(time_text, sizeof(time_text), "%Y-%m-%d %H:%M", &tm_buf);
        snprintf(time_line, sizeof(time_line), "修改：%s", time_text);
    } else {
        strlcpy(size_line, "大小：未知", sizeof(size_line));
        strlcpy(time_line, "修改：未知", sizeof(time_line));
    }

    char msg[128];
    snprintf(msg, sizeof(msg), "类型：文件\n%s\n%s", size_line, time_line);
    files_confirm_open(disp, msg, true); /* 标题=文件名，单「确定」按钮 */
}

/* ------------------------------------------------------------------ */
/* 模态：上下文菜单（长按条目）                                           */
/* ------------------------------------------------------------------ */

/** 菜单按钮回调：按标识分发（重命名 / 删除 / 取消）。 */
static void files_menu_cb(lv_event_t *e) {
    lv_event_stop_bubbling(e);
    if (!files_modal_click_ok()) {
        return;
    }
    const intptr_t action = (intptr_t)lv_event_get_user_data(e);
    if (action == FILES_MENU_CANCEL) {
        files_modal_close();
        return;
    }
    if (action == FILES_MENU_RENAME) {
        files_modal_close();
        files_keyboard_open(FILES_INPUT_RENAME, s_pending_old);
        return;
    }
    /* FILES_MENU_DELETE：删除属敏感操作，转二次确认。 */
    char old[FILES_DISP_MAX];
    files_disp_name(s_pending_old, old, sizeof(old));
    /* 固定短语为 UTF-8 中文（每字 3 字节），缓冲按「显示名 + 充裕短语」取值。 */
    char msg[FILES_DISP_MAX + 96];
    if (s_pending_is_dir) {
        snprintf(msg, sizeof(msg), "将删除文件夹「%s」及其全部内容，此操作不可恢复。", old);
    } else {
        snprintf(msg, sizeof(msg), "将删除文件「%s」，此操作不可恢复。", old);
    }
    files_modal_close();
    s_pending_op = FILES_PENDING_DELETE;
    files_confirm_open("删除确认", msg, false);
}

/** 打开条目上下文菜单（长按触发，发生在按住期间 -> 启用点击抑制窗口）。 */
static void files_menu_open(int idx) {
    if (idx < 0 || idx >= s_count) {
        return;
    }
    strlcpy(s_pending_old, s_entries[idx].name, sizeof(s_pending_old));
    s_pending_is_dir = s_entries[idx].is_dir;

    char disp[FILES_DISP_MAX];
    files_disp_name(s_pending_old, disp, sizeof(disp));

    const int bh = files_btn_h();
    const int gap = 10;
    const int card_h = files_scaled(56) + 3 * bh + 2 * gap + files_scaled(16);
    lv_obj_t *card = files_modal_base(disp, NULL, card_h, true, true);

    const int card_w = files_modal_card_w(); /* 布局前读宽为 0，须算术求得 */
    const int bw = card_w - files_scaled(48);
    int by = files_scaled(56);
    files_card_button(card, "重命名", files_scaled(24), by, bw, bh, false, files_menu_cb,
                      (void *)(intptr_t)FILES_MENU_RENAME);
    by += bh + gap;
    files_card_button(card, "删除", files_scaled(24), by, bw, bh, false, files_menu_cb,
                      (void *)(intptr_t)FILES_MENU_DELETE);
    by += bh + gap;
    files_card_button(card, "取消", files_scaled(24), by, bw, bh, true, files_menu_cb,
                      (void *)(intptr_t)FILES_MENU_CANCEL);
    ESP_LOGI(TAG, "files: context menu open (%s)", s_pending_old);
}

/* ------------------------------------------------------------------ */
/* 模态：文本输入（LVGL 键盘，新建文件夹 / 重命名共用）                    */
/* ------------------------------------------------------------------ */

/** 键盘模态 取消 按钮：关闭，丢弃输入。 */
static void files_kb_cancel_cb(lv_event_t *e) {
    lv_event_stop_bubbling(e);
    if (!files_modal_click_ok()) {
        return;
    }
    files_modal_close();
}

/** 键盘模态 确定 按钮：校验名称 -> 转二次确认。 */
static void files_kb_ok_cb(lv_event_t *e) {
    lv_event_stop_bubbling(e);
    if (!files_modal_click_ok() || s_kb_ta == NULL) {
        return;
    }
    const char *text = lv_textarea_get_text(s_kb_ta);
    if (!files_name_valid(text)) {
        if (s_kb_status != NULL) {
            lv_label_set_text(s_kb_status, "名称无效：不能为空，且不能含 / 或控制字符");
        }
        ESP_LOGW(TAG, "files: invalid name rejected");
        return; /* 不关闭，让用户修正输入 */
    }

    strlcpy(s_pending_new, text, sizeof(s_pending_new));
    char old_disp[FILES_DISP_MAX];
    char new_disp[FILES_DISP_MAX];
    files_disp_name(s_pending_old, old_disp, sizeof(old_disp));
    files_disp_name(s_pending_new, new_disp, sizeof(new_disp));

    char msg[2 * FILES_DISP_MAX + 48];
    if (s_input_mode == FILES_INPUT_RENAME) {
        if (strcmp(s_pending_new, s_pending_old) == 0) {
            files_modal_close(); /* 名字未变：无操作 */
            return;
        }
        snprintf(msg, sizeof(msg), "将把「%s」重命名为「%s」。", old_disp, new_disp);
        s_pending_op = FILES_PENDING_RENAME;
    } else {
        snprintf(msg, sizeof(msg), "将在当前目录创建文件夹「%s」。", new_disp);
        s_pending_op = FILES_PENDING_MKDIR;
    }
    files_modal_close();
    files_confirm_open(s_input_mode == FILES_INPUT_RENAME ? "重命名确认" : "新建确认", msg, false);
}

/**
 * 打开文本输入模态（底部面板：标题 + 输入框 + 取消/确定 + 键盘）。
 * 输入模态不可点空白关闭（防误触丢输入）；由抬起点击打开，无需抑制窗口。
 *
 * @param mode      用途（新建 / 重命名）
 * @param init_text 初始文本（重命名预填旧名，新建传空串）
 */
static void files_keyboard_open(files_input_mode_t mode, const char *init_text) {
    s_input_mode = mode;

    int32_t scr_w = 0;
    int32_t scr_h = 0;
    files_screen_size(&scr_w, &scr_h);

    /* 全屏覆盖层（不点空白关闭） */
    s_modal = lv_obj_create(lv_screen_active());
    lv_obj_set_size(s_modal, scr_w, scr_h);
    lv_obj_set_pos(s_modal, 0, 0);
    lv_obj_set_style_bg_color(s_modal, lv_color_hex(0xE8E8E8), 0);
    lv_obj_set_style_border_width(s_modal, 0, 0);
    lv_obj_set_style_radius(s_modal, 0, 0);
    lv_obj_set_style_pad_all(s_modal, 0, 0);
    lv_obj_remove_flag(s_modal, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_modal, LV_OBJ_FLAG_CLICKABLE);
    s_modal_guard_until = 0;
    s_modal_track_release = false;

    /* 底部面板尺寸（内容驱动，横竖屏自适应） */
    const int panel_w = scr_w - 2 * FILES_MARGIN;
    const int pad = 10;
    const int title_h = 30;
    const int ta_h = files_scaled(52) < 40 ? 40 : files_scaled(52);
    const int status_h = 22;
    const int bh = files_btn_h();
    const int kb_h = files_scaled(240) < 170 ? 170 : files_scaled(240);
    /* 面板仅含标题/输入框/提示/按钮（不含键盘，键盘单独挂全屏 modal 贴底，避免被面板裁剪）。 */
    const int panel_h = pad + title_h + 6 + ta_h + 4 + status_h + 6 + bh + pad;
    const int kb_y = scr_h - kb_h - 6;      /* 键盘贴底 */
    const int panel_y = kb_y - panel_h - 6; /* 面板位于键盘上方 */

    lv_obj_t *panel = lv_obj_create(s_modal);
    lv_obj_set_size(panel, panel_w, panel_h);
    lv_obj_set_pos(panel, FILES_MARGIN, panel_y);
    lv_obj_set_style_bg_color(panel, lv_color_white(), 0);
    lv_obj_set_style_border_color(panel, lv_color_black(), 0);
    lv_obj_set_style_border_width(panel, 2, 0);
    lv_obj_set_style_radius(panel, 12, 0);
    lv_obj_set_style_pad_all(panel, 0, 0);
    lv_obj_remove_flag(panel, LV_OBJ_FLAG_SCROLLABLE);

    /* 标题 */
    lv_obj_t *title = files_label_create(
        panel, mode == FILES_INPUT_RENAME ? "重命名" : "新建文件夹", 20, LV_TEXT_ALIGN_CENTER);
    lv_obj_set_width(title, LV_PCT(100));
    lv_obj_set_pos(title, 0, pad);

    /* 输入框（一行；FreeType 字体保证已有中文名可见） */
    lv_obj_t *ta = lv_textarea_create(panel);
    s_kb_ta = ta;
    lv_obj_set_size(ta, panel_w - 2 * pad, ta_h);
    lv_obj_set_pos(ta, pad, pad + title_h + 6);
    lv_textarea_set_one_line(ta, true);
    lv_textarea_set_max_length(ta, FILES_NAME_MAX - 1);
    lv_textarea_set_text(ta, init_text != NULL ? init_text : "");
    lv_obj_set_style_text_color(ta, lv_color_black(), 0);
    lv_obj_set_style_text_font(ta, files_font(20), 0);
    lv_obj_set_style_border_color(ta, lv_color_black(), 0);
    lv_obj_set_style_border_width(ta, 2, 0);
    lv_obj_set_style_radius(ta, 6, 0);
    lv_obj_set_style_pad_left(ta, 8, 0);
    /* 墨水屏：光标闪烁会触发连续局部刷新，禁用（anim_duration=0 即不闪烁）。
     * 默认主题在 LV_PART_CURSOR|LV_STATE_FOCUSED 上设了 400ms，故默认态与聚焦态都要覆盖。 */
    lv_obj_set_style_anim_duration(ta, 0, LV_PART_CURSOR);
    lv_obj_set_style_anim_duration(ta, 0, LV_PART_CURSOR | LV_STATE_FOCUSED);

    /* 校验提示（默认空） */
    lv_obj_t *status = files_label_create(panel, "", 16, LV_TEXT_ALIGN_LEFT);
    s_kb_status = status;
    lv_obj_set_width(status, LV_PCT(100));
    lv_label_set_long_mode(status, LV_LABEL_LONG_DOT);
    lv_obj_set_pos(status, pad + 2, pad + title_h + 6 + ta_h + 4);

    /* 取消 / 确定 */
    const int bw = (panel_w - 2 * pad - 12) / 2;
    const int btn_y = pad + title_h + 6 + ta_h + 4 + status_h + 6;
    files_card_button(panel, "取消", pad, btn_y, bw, bh, false, files_kb_cancel_cb, NULL);
    files_card_button(panel, "确定", pad + bw + 12, btn_y, bw, bh, true, files_kb_ok_cb, NULL);

    /* 键盘直接挂在全屏 modal 上并贴底，避免被面板裁剪（面板仅含标题/输入框/提示/按钮）。 */
    lv_obj_t *kb = lv_keyboard_create(s_modal);
    lv_obj_set_size(kb, panel_w, kb_h);
    lv_obj_align(kb, LV_ALIGN_TOP_LEFT, FILES_MARGIN, kb_y);
    lv_keyboard_set_mode(kb, LV_KEYBOARD_MODE_TEXT_LOWER);
    lv_keyboard_set_textarea(kb, ta);

    ESP_LOGI(TAG, "files: keyboard modal open (mode %d)", (int)mode);
}

/* ------------------------------------------------------------------ */
/* 动作入口                                                             */
/* ------------------------------------------------------------------ */

/** 底部「新建文件夹」：打开键盘输入（空名称）。 */
static void files_action_mkdir(void) { files_keyboard_open(FILES_INPUT_MKDIR, ""); }

/** 命中检测：返回按下点命中的条目下标（当前页内，-1=无）。 */
static int files_hit_entry(const lv_point_t *p) {
    for (int i = 0; i < s_row_cnt; i++) {
        const lv_obj_t *row = s_row_objs[i];
        if (row == NULL) {
            continue;
        }
        const int x = files_obj_screen_x(row);
        const int y = files_obj_screen_y(row);
        if (files_point_in(p, x, y, lv_obj_get_width(row), lv_obj_get_height(row))) {
            return s_row_idx[i];
        }
    }
    return -1;
}

/** 命中检测：底部按钮（-1=无 0=上一级 1=新建文件夹）。 */
static int files_hit_button(const lv_point_t *p) {
    const lv_obj_t *btns[2] = {s_btn_up, s_btn_mkdir};
    for (int i = 0; i < 2; i++) {
        const lv_obj_t *btn = btns[i];
        if (btn == NULL) {
            continue;
        }
        const int x = files_obj_screen_x(btn);
        const int y = files_obj_screen_y(btn);
        if (files_point_in(p, x, y, lv_obj_get_width(btn), lv_obj_get_height(btn))) {
            return i;
        }
    }
    return -1;
}

/* ------------------------------------------------------------------ */
/* 页面构建 / 交互                                                      */
/* ------------------------------------------------------------------ */

/** 文件管理页构建（页面 enter：屏幕已由页面栈清空）。 */
static void files_enter(void) {
    lv_obj_t *scr = lv_screen_active();
    lv_obj_set_style_bg_color(scr, lv_color_white(), 0);
    /* 防止整个屏幕被 LVGL 滚动（分页切换由手势接管） */
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    int32_t scr_w = 0;
    int32_t scr_h = 0;
    files_screen_size(&scr_w, &scr_h);

    /* 几何布局（按屏高缩放 + 下限；构建期布局未完成读宽为 0，全部算术求得） */
    s_scale = (float)scr_h / (float)FILES_REF_H;
    s_bar_h = files_scaled(30) < 24 ? 24 : files_scaled(30);
    s_path_h = files_scaled(FILES_PATH_H) < FILES_MIN_H ? FILES_MIN_H : files_scaled(FILES_PATH_H);
    s_path_y = s_bar_h + 6;
    const int bottom_h = files_scaled(FILES_BOTTOM_H) < FILES_MIN_BOTTOM_H
                             ? FILES_MIN_BOTTOM_H
                             : files_scaled(FILES_BOTTOM_H);
    s_bottom_y = scr_h - bottom_h - 4;
    s_list_y = s_path_y + s_path_h + 6;
    s_list_h = s_bottom_y - s_list_y - 26; /* 底部留出指示点空间 */
    if (s_list_h < s_row_h) {
        s_list_h = s_row_h;
    }
    s_card_w = scr_w - 2 * FILES_MARGIN;
    s_row_h =
        files_scaled(FILES_ROW_H) < FILES_MIN_ROW_H ? FILES_MIN_ROW_H : files_scaled(FILES_ROW_H);
    s_btn_w = (s_card_w - 12) / 2;
    s_btn_h = bottom_h - 12;

    /* 统一状态栏：左侧时间、居中"文件"、右侧 WiFi/睡眠图标 */
    s_bar = espaperplay_ui_status_bar_create(scr, s_bar_h, "文件", false);

    /* 路径条（相对路径，超长省略号截断） */
    lv_obj_t *path_bar = lv_obj_create(scr);
    lv_obj_set_size(path_bar, s_card_w, s_path_h);
    lv_obj_set_pos(path_bar, FILES_MARGIN, s_path_y);
    lv_obj_set_style_bg_color(path_bar, lv_color_white(), 0);
    lv_obj_set_style_border_color(path_bar, lv_color_black(), 0);
    lv_obj_set_style_border_width(path_bar, 1, 0);
    lv_obj_set_style_radius(path_bar, 8, 0);
    lv_obj_set_style_pad_all(path_bar, 0, 0);
    lv_obj_remove_flag(path_bar, LV_OBJ_FLAG_SCROLLABLE);
    s_path_label = files_label_create(path_bar, "/", 16, LV_TEXT_ALIGN_LEFT);
    lv_obj_set_width(s_path_label, s_card_w - 16);
    lv_obj_align(s_path_label, LV_ALIGN_LEFT_MID, 8, 0);
    lv_label_set_long_mode(s_path_label, LV_LABEL_LONG_DOT);

    /* 空目录 / 未挂载提示（居中于列表区，默认隐藏） */
    s_hint_label = files_label_create(scr, "", 16, LV_TEXT_ALIGN_CENTER);
    lv_obj_set_width(s_hint_label, s_card_w);
    lv_obj_set_pos(s_hint_label, FILES_MARGIN, s_list_y + s_list_h / 2 - 12);
    lv_obj_add_flag(s_hint_label, LV_OBJ_FLAG_HIDDEN);

    /* 底部操作栏：上一级（次按钮）+ 新建文件夹（主按钮）；点击由页面
     * on_touch 命中检测处理（不挂 LVGL 回调，避免与手势判定双触发）。 */
    s_btn_up = files_card_button(scr, "上一级", FILES_MARGIN, s_bottom_y + 6, s_btn_w, s_btn_h,
                                 false, NULL, NULL);
    s_btn_mkdir = files_card_button(scr, "新建文件夹", FILES_MARGIN + s_btn_w + 12, s_bottom_y + 6,
                                    s_btn_w, s_btn_h, true, NULL, NULL);

    /* 条目数组（PSRAM；容量 = 上限 × 定长名字缓冲） */
    if (s_entries == NULL) {
        s_entries = heap_caps_calloc(FILES_MAX_ENTRIES, sizeof(files_entry_t),
                                     MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    }
    if (s_entries == NULL) {
        ESP_LOGE(TAG, "files: entries alloc failed");
        lv_label_set_text(s_hint_label, "内存不足");
        lv_obj_remove_flag(s_hint_label, LV_OBJ_FLAG_HIDDEN);
    }

    /* 状态复位并从根目录开始扫描 */
    strlcpy(s_cwd, FILES_ROOT, sizeof(s_cwd));
    s_page = 0;
    s_busy = false;
    s_modal = NULL;
    s_pending_op = FILES_PENDING_NONE;
    s_touch_down = false;
    s_await_release = false;
    s_modal_track_release = false;
    s_touch_hit = -1;
    s_touch_btn = -1;
    s_active = true;
    files_scan();

    ESP_LOGI(TAG, "files screen entered");
}

/** 文件管理页退出（页面 exit：释放页面级资源，清空对象指针防悬垂访问）。 */
static void files_exit(void) {
    s_active = false;
    files_modal_close();
    if (s_entries != NULL) {
        heap_caps_free(s_entries);
        s_entries = NULL;
    }
    files_pages_destroy();
    s_bar = NULL;
    s_path_label = NULL;
    s_hint_label = NULL;
    s_btn_up = NULL;
    s_btn_mkdir = NULL;
    s_kb_ta = NULL;
    s_kb_status = NULL;
    s_pending_op = FILES_PENDING_NONE;
    ESP_LOGI(TAG, "files screen exited");
}

/** 文件管理页触摸处理：长按锁存优先于模态守卫（结束长按的释放发生在模态
 * 打开期间）；模态打开时忽略页面手势（模态控件由指针 indev 自行处理）；
 * 否则边缘向内滑动返回主页、中间横向滑动切换分页、小位移点击触发条目 /
 * 按钮（滑动优先于点击，互不冲突）。 */
static void files_on_touch(const espaperplay_input_event_t *event) {
    lv_point_t p;
    espaperplay_ui_touch_map_to_lv(event->point.x, event->point.y, &p);

    /* 按住期间打开的模态：观察其后的首次物理释放并延长抑制窗口（须在最
     * 前处理——结束长按的释放会被下面的锁存分支吞掉）。宽限需大于 indev
     * 读周期（~30ms），确保迟到的 phantom CLICKED 仍落在窗口内。 */
    if (!event->touch_pressed && s_modal_track_release) {
        s_modal_track_release = false;
        const uint32_t until = lv_tick_get() + FILES_MODAL_RELEASE_GRACE_MS;
        if ((int32_t)(until - s_modal_guard_until) > 0) {
            s_modal_guard_until = until;
        }
    }

    /* 长按触发后的锁存：忽略一切按下帧，直到物理释放被消费（防止松手
     * 误入目录 / 反弹帧误触菜单；须在模态守卫之前检查）。 */
    if (s_await_release) {
        if (!event->touch_pressed) {
            s_await_release = false;
        }
        return;
    }

    if (s_modal != NULL) {
        return; /* 模态打开：点击由覆盖层 / 按钮处理 */
    }

    if (event->touch_pressed) {
        if (!s_touch_down) {
            s_touch_down = true;
            s_touch_start = p;
            s_touch_down_tick = lv_tick_get();
            s_touch_hit = files_hit_entry(&p);
            s_touch_btn = files_hit_button(&p);
        }
        s_touch_last = p;

        /* 长按判定（仅条目）：按住达阈值且位移极小，立即弹菜单（不等抬起；
         * 触发后锁存等释放——不用 lv_timer，见文件头说明）。 */
        if (s_touch_hit >= 0 && lv_tick_elaps(s_touch_down_tick) >= FILES_LONG_PRESS_MS &&
            abs(p.x - s_touch_start.x) <= FILES_CLICK_MAX_PX &&
            abs(p.y - s_touch_start.y) <= FILES_CLICK_MAX_PX) {
            const int idx = s_touch_hit;
            s_touch_down = false;
            s_touch_hit = -1;
            s_touch_btn = -1;
            s_await_release = true;
            ESP_LOGI(TAG, "files: long press -> menu (entry %d)", idx);
            files_menu_open(idx);
        }
        return;
    }

    if (!s_touch_down) {
        return;
    }
    s_touch_down = false;
    const int dx = s_touch_last.x - s_touch_start.x;
    const int dy = s_touch_last.y - s_touch_start.y;
    const int adx = abs(dx);
    const int ady = abs(dy);
    const int hit = s_touch_hit;
    const int btn = s_touch_btn;
    s_touch_hit = -1;
    s_touch_btn = -1;

    /* 边缘向内滑动返回主页（横向为主，避免与分页切换冲突）。 */
    if (adx > FILES_EDGE_SWIPE_PX && adx > ady * FILES_SWIPE_MIN_RATIO) {
        const int32_t scr_w = lv_display_get_horizontal_resolution(lv_display_get_default());
        if ((s_touch_start.x < FILES_EDGE_PX && dx > 0) ||
            (s_touch_start.x > scr_w - FILES_EDGE_PX && dx < 0)) {
            if (espaperplay_ui_page_depth() > 1) {
                ESP_LOGI(TAG, "files: edge swipe -> pop back");
                espaperplay_ui_page_pop_lv();
            }
            return;
        }
    }

    /* 中间横向滑动：切换分页（优先于点击）。 */
    if (adx > FILES_SWIPE_PX && adx > ady * FILES_SWIPE_MIN_RATIO) {
        files_show_page(s_page + (dx < 0 ? 1 : -1));
        return;
    }

    /* 小位移 + 时长在长按阈值以下：点击触发（达到长按阈值的笔画不再按
     * 点击处理，防止长按松手误触发）。 */
    if (adx <= FILES_CLICK_MAX_PX && ady <= FILES_CLICK_MAX_PX &&
        lv_tick_elaps(s_touch_down_tick) < FILES_LONG_PRESS_MS) {
        if (hit >= 0) {
            files_entry_activate(hit);
        } else if (btn == 0) {
            files_go_up();
        } else if (btn == 1) {
            files_action_mkdir();
        }
    }
}

/** 文件管理页按键处理（LVGL 线程内）：模态打开时单击先关闭模态，否则返回
 * 上一页。 */
static void files_on_key(const espaperplay_input_event_t *event) {
    if (event->key_action != ESPAPERPLAY_INPUT_KEY_ACTION_SINGLE_CLICK) {
        return;
    }
    if (s_modal != NULL) {
        files_modal_close();
        return;
    }
    if (espaperplay_ui_page_depth() > 1) {
        ESP_LOGI(TAG, "files: single click -> pop back");
        espaperplay_ui_page_pop_lv();
    }
}

/** 文件管理页页面实例（页面栈用）。 */
const espaperplay_ui_page_t espaperplay_ui_page_files = {files_enter, files_exit, files_on_key,
                                                         files_on_touch};
