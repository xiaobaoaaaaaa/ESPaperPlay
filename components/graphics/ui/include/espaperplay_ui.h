/*
 * SPDX-FileCopyrightText: 2026 ESPaperPlay Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include "esp_err.h"

#include "lvgl.h"

#include "espaperplay_input.h"

/**
 * @file espaperplay_ui.h
 * @brief UI 页面层：构建 LVGL widget 树。
 *
 * 页面函数只创建/组织 LVGL 控件并注册事件回调，不接触像素、不调用渲染
 * 后端 API（LVGL 的无效区机制自动驱动 flush 回调链）。须在
 * espaperplay_gui_lv_start() 之后调用（页面依赖 LVGL 已初始化）。
 */

/**
 * @brief 按键输入自检（验收用，默认关闭）。
 *
 * 使能后 espaperplay_ui_key_input_start() 会额外创建自检任务：注入合成
 * 按键事件（与真实按键走同一条 input 队列 / 分发路径），验证
 * "input 队列 -> GUI 读取 -> 页面响应"整条链路：单击进入测试页
 * （页面栈深度 1 -> 2），长按松开返回（2 -> 1），往返后再进入（1 -> 2），
 * 结果以日志输出（PASS / FAIL）。
 */
#ifndef ESPAPERPLAY_UI_ENABLE_KEY_SELFTEST
#define ESPAPERPLAY_UI_ENABLE_KEY_SELFTEST 0
#endif

/**
 * @brief 展示主界面（最基础原型：标题 + NTP 系统时钟 + 状态行 + 占位提示）。
 *
 * 等价于 espaperplay_ui_page_push(&espaperplay_ui_page_home)。
 */
void espaperplay_ui_home_show(void);

/**
 * @brief 页面描述（进入/退出/按键/触摸钩子，均在 LVGL 线程内执行）。
 *
 * 按键与触摸事件均经输入分发任务转发（见 espaperplay_ui_key_input_start）：
 *   - enter：构建页面内容到当前屏幕（屏幕已由页面栈清空），并创建页面级
 *     资源（定时器等）；exit：释放页面级资源（删除定时器等），可为 NULL；
 *   - on_key：页面级按键处理（按键分发任务把按键事件转发给栈顶页面的该
 *     钩子，页面据此更新自身内容或发起导航），可为 NULL；
 *   - on_touch：页面级触摸处理（触摸事件经输入分发任务按 ~30ms 窗口批量
 *     投递后逐条转发给栈顶页面的该钩子，页面据此展示触摸轨迹/坐标；
 *     LVGL 控件点击不依赖本钩子，由触摸指针 indev 直接驱动），可为 NULL。
 */
typedef struct {
    void (*enter)(void); /*!< 进入页面：构建内容（LVGL 线程内） */
    void (*exit)(void);  /*!< 退出页面：清理资源（LVGL 线程内，可 NULL） */
    void (*on_key)(const espaperplay_input_event_t *event); /*!< 按键事件（LVGL 线程内，可 NULL） */
    void (*on_touch)(
        const espaperplay_input_event_t *event); /*!< 触摸事件（LVGL 线程内，可 NULL） */
} espaperplay_ui_page_t;

#define ESPAPERPLAY_UI_PAGE_MAX 8 /*!< 页面栈最大深度 */

/**
 * @brief 压入并进入一个页面（栈管理，跨线程安全）。
 *
 * 先调用当前页 exit 清理，再清空屏幕并调用新页 enter 构建。全部在
 * LVGL 线程内执行（内部经 espaperplay_gui_lv_call 投递，同步等待完成）。
 *
 * @param page 页面描述（enter 必须非 NULL；调用方须保持有效直到返回）。
 *
 * @return ESP_OK；参数非法返回 ESP_ERR_INVALID_ARG；栈满返回 ESP_ERR_NO_MEM。
 */
esp_err_t espaperplay_ui_page_push(const espaperplay_ui_page_t *page);

/**
 * @brief 退出当前页并重建上一页（根页面不可弹出，跨线程安全）。
 *
 * @return ESP_OK；栈中无页面可弹（已是根）返回 ESP_ERR_NOT_FOUND。
 */
esp_err_t espaperplay_ui_page_pop(void);

/**
 * @brief 压入并进入一个页面（LVGL 线程内直接切换，不做跨线程投递）。
 *
 * 供页面钩子 / 按键分发等已在 LVGL 线程内的代码使用：若再调用
 * espaperplay_ui_page_push() 会经 gui_lv_call 投递并阻塞等待自身，死锁。
 *
 * @param page 页面描述（enter 必须非 NULL）。
 *
 * @return ESP_OK；参数非法返回 ESP_ERR_INVALID_ARG；栈满返回 ESP_ERR_NO_MEM。
 */
esp_err_t espaperplay_ui_page_push_lv(const espaperplay_ui_page_t *page);

/**
 * @brief 退出当前页并重建上一页（LVGL 线程内直接切换，不做跨线程投递）。
 *
 * @return ESP_OK；栈中无页面可弹（已是根）返回 ESP_ERR_NOT_FOUND。
 */
esp_err_t espaperplay_ui_page_pop_lv(void);

/**
 * @brief 用新页替换栈顶页（LVGL 线程内直接切换，不做跨线程投递）。
 *
 * 栈深不变（空栈时等价压入）：旧页 exit -> 清屏 -> 新页 enter。供引导页
 * 完成后以主界面替换自身等场景使用。
 *
 * @param page 页面描述（enter 必须非 NULL）。
 *
 * @return ESP_OK；参数非法返回 ESP_ERR_INVALID_ARG。
 */
esp_err_t espaperplay_ui_page_replace_lv(const espaperplay_ui_page_t *page);

/**
 * @brief 把按键事件转发给栈顶页面的 on_key 钩子（须在 LVGL 线程内调用）。
 *
 * 由按键分发任务调用；栈为空或无钩子时为空操作。
 *
 * @param event 按键事件。
 */
void espaperplay_ui_page_handle_key_lv(const espaperplay_input_event_t *event);

/**
 * @brief 把触摸事件转发给栈顶页面的 on_touch 钩子（须在 LVGL 线程内调用）。
 *
 * 由输入分发任务调用（触摸事件按 ~30ms/32 点批量投递，批内逐条转发，
 * 保留全部中间坐标点）；栈为空或无钩子时为空操作。
 *
 * @param event 触摸事件。
 */
void espaperplay_ui_page_handle_touch_lv(const espaperplay_input_event_t *event);

/**
 * @brief 启动输入分发任务（input 队列 -> LVGL 线程 -> 页面钩子 / 触摸指针）。
 *
 * 任务阻塞在 espaperplay_input_get_event() 上（按键与触摸事件走同一条
 * 合并队列）：
 *   - 按键事件经 espaperplay_gui_lv_call 投递到 LVGL 线程，由
 *     espaperplay_ui_page_handle_key_lv() 转发给当前页面 on_key（导航 /
 *     刷新内容）；
 *   - 触摸事件直接更新 LVGL 指针 indev 状态（espaperplay_ui_touch_update，
 *     任意线程安全），并按 ~30ms 窗口批量投递、逐条转发给当前页面
 *     on_touch（轨迹绘制需要全部中间坐标点）。
 * 须在 espaperplay_input_init()、espaperplay_gui_lv_start() 之后调用。
 *
 * ESPAPERPLAY_UI_ENABLE_KEY_SELFTEST=1 时，任务启动后先执行按键链路自检
 * （注入合成事件并断言页面栈响应），结果以日志输出。
 *
 * @return ESP_OK；任务创建失败返回 ESP_ERR_NO_MEM。
 */
esp_err_t espaperplay_ui_key_input_start(void);

/**
 * @brief 当前页面栈深度（1 = 仅根页面）。
 */
uint8_t espaperplay_ui_page_depth(void);

/**
 * @brief 统一状态栏（顶栏）句柄（不透明）。
 *
 * 各页面（测试页除外）共用同一套顶栏逻辑：左侧时间、居中标题、右侧
 * WiFi 强度图标与睡眠/节能指示图标。由 espaperplay_ui_status_bar_create()
 * 创建，espaperplay_ui_status_bar_refresh() 刷新。睡眠图标位置随右侧图标
 * 数量动态调整（仅 WiFi 时靠右；叠加睡眠图标时睡眠图标左移）。
 */
typedef struct espaperplay_ui_status_bar_t espaperplay_ui_status_bar_t;

/**
 * @brief 创建统一状态栏（顶栏）。
 *
 * 白底 + 底部 2px 分隔线；左侧时间（HH:MM，16px），居中标题（20px，可空），
 * 右侧 WiFi 强度图标与睡眠/节能指示图标（默认隐藏）。睡眠图标位置随右侧
 * 图标数量动态调整。创建后自动登记为"当前页状态栏"，由统一调度定时器
 * 周期刷新（见 espaperplay_ui_status_bar_init）。
 *
 * @param scr         屏幕对象（页面 enter 时由页面栈清空后的活动屏幕）。
 * @param height_px  状态栏高度（像素）。
 * @param title      居中标题文本（NULL 或空串表示不显示标题，如主界面）。
 * @param live_clock 睡眠期间时钟是否仍实时更新（true=主界面，有分钟对齐
 *                   定时器唤醒；false=其他页面，无定时器唤醒，睡眠时时钟
 *                   冻结，故睡眠指示期间隐藏为 "--:--" 以免误导）。
 * @return 状态栏句柄（供刷新/设标题用），失败返回 NULL。
 */
espaperplay_ui_status_bar_t *espaperplay_ui_status_bar_create(lv_obj_t *scr, int height_px,
                                                              const char *title, bool live_clock);

/**
 * @brief 刷新统一状态栏（时间 / WiFi / 睡眠图标）。
 *
 * 内容未变化时不触发 LVGL 重绘，故不会造成无谓 EPD 局刷。睡眠图标按当前
 * 睡眠指示标志显隐，位置随右侧图标数量动态调整。由统一调度定时器周期调用，
 * 确保睡眠指示在标志变化后 ~1s 内更新（不受各页面自身刷新间隔影响）。
 *
 * @param bar 状态栏句柄（可 NULL）。
 */
void espaperplay_ui_status_bar_refresh(espaperplay_ui_status_bar_t *bar);

/**
 * @brief 设置/更新状态栏居中标题（内容变化时才重绘）。
 *
 * 供动态标题页（如天气页显示位置名）在内容就绪时调用；统一调度定时器也会
 * 周期性重绘（内容不变则不重绘）。
 *
 * @param bar   状态栏句柄（可 NULL）。
 * @param title 标题文本（NULL 视为空串）。
 */
void espaperplay_ui_status_bar_set_title(espaperplay_ui_status_bar_t *bar, const char *title);

/**
 * @brief 初始化统一状态栏调度（UI 初始化时调用一次）。
 *
 * 创建周期刷新定时器（1s）统一刷新当前页状态栏，并注册进睡前准备回调
 * （供电源管理在进睡前触发图标绘制与 EPD 刷新等待）。须在 LVGL 启动后调用。
 */
void espaperplay_ui_status_bar_init(void);

/** 主界面页面实例（screen_home.c）。 */
extern const espaperplay_ui_page_t espaperplay_ui_page_home;

/** 测试页页面实例（screen_test.c）：局刷压力测试 + 按键/触摸事件显示 +
 * 可点击返回按钮，双击旋转屏幕，长按返回。 */
extern const espaperplay_ui_page_t espaperplay_ui_page_test;

/** 天气页页面实例（screen_weather.c）：展示和风天气快照（实时 / 3 日预报 / 空气 / 天文 / 降水 /
 * 预警），单击返回。 */
extern const espaperplay_ui_page_t espaperplay_ui_page_weather;

/** 阅读器页页面实例（screen_reader.c）：占位页（FreeType 中文标题 + 即将推出提示），单击返回。 */
extern const espaperplay_ui_page_t espaperplay_ui_page_reader;

/** 设置页页面实例（screen_settings.c）：系统设置管理（数值步进 / 循环切换 /
 * 字体选择 / 测试页入口），难以输入的配置项提示到 Web 管理页，边缘滑动或单击返回。 */
extern const espaperplay_ui_page_t espaperplay_ui_page_settings;

/** 文件管理页页面实例（screen_files.c）：SD 卡基本文件操作（浏览 / 新建文件夹 /
 * 重命名 / 删除），敏感操作二次确认，长按条目弹菜单，边缘滑动或单击返回。 */
extern const espaperplay_ui_page_t espaperplay_ui_page_files;

/** 首次开机引导页页面实例（screen_setup.c）：分步交互式初始化配置
 * （联网配置二维码 / 本机输入 WiFi / 跳过），完成后标记 setup_done。 */
extern const espaperplay_ui_page_t espaperplay_ui_page_setup;

/**
 * @brief 显示开机日志屏（不经页面栈，直接绘制在活动屏幕上）。
 *
 * 供并行启动流程在 LVGL 就绪后立即调用：后续各服务初始化步骤经
 * espaperplay_ui_boot_logf() 把进度逐行显示到屏幕上。主界面入栈时随清屏
 * 自然移除。须在 espaperplay_gui_lv_start() 与 espaperplay_fonts_init()
 * 之后调用。
 *
 * @return ESP_OK 成功；LVGL 移植层未启动或投递超时返回相应错误码。
 */
esp_err_t espaperplay_ui_boot_show(void);

/**
 * @brief 向开机日志屏追加一行日志（跨线程安全）。
 *
 * 屏幕尚未构建时仅写入环形缓冲（构建时统一回放，不丢行）；已构建则触发
 * 一次局部重绘。启动完成、屏幕被主界面替换后调用为空操作（缓冲继续
 * 覆盖写入，无副作用）。
 *
 * @param fmt printf 风格格式串（建议带换行结尾的短句，单行截断于 96 字节）。
 */
void espaperplay_ui_boot_logf(const char *fmt, ...);

#ifdef __cplusplus
}
#endif
