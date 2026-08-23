/*
 * SPDX-FileCopyrightText: 2026 ESPaperPlay Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @file espaperplay_weather.h
 * @brief 和风天气（QWeather）服务。
 *
 * 基于官方 Web API v7（https://dev.qweather.com/docs/api/）实现天气数据
 * 获取，覆盖：实时天气（now）、天气预报（3 日 / 7 日 daily、24 小时
 * hourly）、分钟级降水（minutely）、气象灾害预警（warning）、天气指数
 * （indices）、空气质量（air）与天文（astronomy 日出日落 / 月相）。
 *
 * 使用方式：
 *   1. 通过 Web 管理页（或 espaperplay_system_set_weather_api_key()）配置
 *      API Key、可选位置（LocationID / 城市名 / "经度,纬度"）与可选自定义
 *      API Host（和风天气 2026 年起逐步停止公共地址，建议在控制台-设置
 *      获取自己的 API Host 并填入）；
 *   2. 调用 espaperplay_weather_start() 启动后台刷新任务（等待 STA 联网后
 *      按周期刷新全部数据到内存快照）；
 *   3. 调用 espaperplay_weather_get_snapshot() 读取快照（屏幕 / Web 展示用），
 *      或直接调用 espaperplay_weather_query_*() 查询单项数据。
 *
 * 位置解析：配置了位置时直接使用；未配置时自动定位——公网 IP（netip）
 * → 地理位置（geoip）→ 经纬度反查 GeoAPI 获取 LocationID。坐标按
 * QWeather 要求使用 "经度,纬度" 格式（最多 2 位小数）；若手动配置时按
 * 习惯写了 "纬度,经度"，会自动识别并交换。自动定位结果带 24 小时缓存。
 *
 * 缓存：各 API 独立 TTL 缓存（实时/分钟级/预警 10 分钟，逐小时 30 分钟，
 * 预报/空气 60 分钟，指数/天文 6 小时），TTL 内重复查询不发起网络请求；
 * 快照刷新同样受这些 TTL 约束，避免无谓消耗 API 配额。
 */

/** 天气 API Key 最大长度（含结尾 '\0'，与系统配置一致）。 */
#define ESPAPERPLAY_WEATHER_API_KEY_MAX_LEN 48
/** 天气位置参数（LocationID / 城市名 / "经度,纬度"）最大长度（含结尾 '\0'）。 */
#define ESPAPERPLAY_WEATHER_LOCATION_MAX_LEN 64
/** 天气数据请求超时（毫秒）。 */
#define ESPAPERPLAY_WEATHER_HTTP_TIMEOUT_MS 10000
/** 快照刷新任务默认唤醒周期（毫秒）：10 分钟。 */
#define ESPAPERPLAY_WEATHER_REFRESH_INTERVAL_MS (10 * 60 * 1000)
/** 自动定位结果缓存有效期（毫秒）：24 小时。 */
#define ESPAPERPLAY_WEATHER_AUTO_LOC_TTL_MS (24 * 60 * 60 * 1000)

/* 各 API 独立缓存 TTL（毫秒）。 */
#define ESPAPERPLAY_WEATHER_TTL_NOW_MS (10 * 60 * 1000)      /*!< 实时天气 */
#define ESPAPERPLAY_WEATHER_TTL_MINUTELY_MS (10 * 60 * 1000) /*!< 分钟级降水 */
#define ESPAPERPLAY_WEATHER_TTL_WARNING_MS (10 * 60 * 1000)  /*!< 气象灾害预警 */
#define ESPAPERPLAY_WEATHER_TTL_HOURLY_MS (30 * 60 * 1000)   /*!< 逐小时预报 */
#define ESPAPERPLAY_WEATHER_TTL_DAILY_MS (60 * 60 * 1000)    /*!< 逐日预报 */
#define ESPAPERPLAY_WEATHER_TTL_AIR_MS (60 * 60 * 1000)      /*!< 空气质量 */
#define ESPAPERPLAY_WEATHER_TTL_INDICES_MS (6 * 60 * 60 * 1000)   /*!< 天气指数 */
#define ESPAPERPLAY_WEATHER_TTL_ASTRONOMY_MS (6 * 60 * 60 * 1000) /*!< 天文 */

/* ------------------------------------------------------------------ */
/* 数据结构（字符串字段与 QWeather 响应字段一一对应，均以 '\0' 结尾）     */
/* ------------------------------------------------------------------ */

/** 实时天气（weather/now）。 */
typedef struct {
    char obs_time[32];  /*!< 数据观测时间 */
    char temp[8];       /*!< 温度（℃） */
    char feels_like[8]; /*!< 体感温度（℃） */
    char icon[8];       /*!< 天气状况图标代码 */
    char text[32];      /*!< 天气状况文字（如 "晴"） */
    char wind_dir[16];  /*!< 风向（如 "东南风"） */
    char wind_scale[8]; /*!< 风力等级（如 "2"） */
    char wind_speed[16]; /*!< 风速（km/h） */
    char humidity[8];   /*!< 相对湿度（%） */
    char precip[8];     /*!< 过去 1 小时降水量（mm） */
    char pressure[16];  /*!< 大气压强（hPa） */
    char vis[16];       /*!< 能见度（km） */
    char cloud[8];      /*!< 云量（%） */
    char dew[8];        /*!< 露点温度（℃） */
} espaperplay_weather_now_t;

/** 逐日天气预报条目（weather/3d、weather/7d）。 */
typedef struct {
    char fx_date[16];       /*!< 预报日期 */
    char sunrise[8];        /*!< 日出时间 */
    char sunset[8];         /*!< 日落时间 */
    char moonrise[8];       /*!< 月升时间 */
    char moonset[8];        /*!< 月落时间 */
    char moon_phase[16];    /*!< 月相（如 "盈凸月"） */
    char temp_max[8];       /*!< 最高温（℃） */
    char temp_min[8];       /*!< 最低温（℃） */
    char icon_day[8];       /*!< 白天天气图标代码 */
    char text_day[32];      /*!< 白天天气状况文字 */
    char icon_night[8];     /*!< 夜间天气图标代码 */
    char text_night[32];    /*!< 夜间天气状况文字 */
    char wind_dir_day[16];  /*!< 白天风向 */
    char wind_scale_day[8]; /*!< 白天风力等级 */
    char wind_speed_day[16]; /*!< 白天风速（km/h） */
    char wind_dir_night[16]; /*!< 夜间风向 */
    char wind_scale_night[8]; /*!< 夜间风力等级 */
    char wind_speed_night[16]; /*!< 夜间风速（km/h） */
    char humidity[8];       /*!< 相对湿度（%） */
    char precip[8];         /*!< 降水量（mm） */
    char pressure[16];      /*!< 大气压强（hPa） */
    char vis[16];           /*!< 能见度（km） */
    char cloud[8];          /*!< 云量（%） */
    char uv_index[8];       /*!< 紫外线强度指数 */
} espaperplay_weather_daily_t;

/** 逐小时天气预报条目（weather/24h）。 */
typedef struct {
    char fx_time[16];       /*!< 预报时间 */
    char temp[8];           /*!< 温度（℃） */
    char icon[8];           /*!< 天气状况图标代码 */
    char text[32];          /*!< 天气状况文字 */
    char wind_dir[16];      /*!< 风向 */
    char wind_scale[8];     /*!< 风力等级 */
    char wind_speed[16];    /*!< 风速（km/h） */
    char humidity[8];       /*!< 相对湿度（%） */
    char precip[8];         /*!< 降水量（mm） */
    char pressure[16];      /*!< 大气压强（hPa） */
    char cloud[8];          /*!< 云量（%） */
    char dew[8];            /*!< 露点温度（℃） */
} espaperplay_weather_hourly_t;

/** 分钟级降水（minutely/5m，未来 2 小时逐分钟降水强度，每 5 分钟更新）。 */
typedef struct {
    char summary[128];       /*!< 分钟降水描述（如 "未来2小时无降水"） */
    char fx_link[256];       /*!< 分钟级降水自适应链接 */
    uint16_t count;          /*!< 有效分钟数（<= 120） */
    char precip[120][8];     /*!< 逐分钟降水强度（mm/h） */
} espaperplay_weather_minutely_t;

/** 气象灾害预警条目（warning/now）。 */
typedef struct {
    char id[16];             /*!< 预警 ID */
    char sender[64];         /*!< 预警发布单位 */
    char pub_time[32];       /*!< 预警发布时间 */
    char title[128];         /*!< 预警标题 */
    char start_time[32];     /*!< 预警开始时间 */
    char end_time[32];       /*!< 预警结束时间 */
    char type[8];            /*!< 预警类型代码 */
    char type_name[32];      /*!< 预警类型名称（如 "台风"） */
    char level[8];           /*!< 预警等级（如 "蓝色"） */
    char severity[16];       /*!< 严重程度 */
    char severity_color[8];  /*!< 预警颜色（如 "Blue"） */
    char text[512];          /*!< 预警详细描述 */
    char related[128];       /*!< 相关灾害性天气现象 */
} espaperplay_weather_warning_t;

/** 天气指数条目（indices/1d）。 */
typedef struct {
    char date[16];           /*!< 预报日期 */
    char type[8];            /*!< 指数类型代码 */
    char name[32];           /*!< 指数名称（如 "穿衣"） */
    char level[16];          /*!< 指数级别（如 "较冷"） */
    char category[32];       /*!< 指数类别（如 "舒适度指数"） */
    char text[256];          /*!< 指数详情与建议 */
} espaperplay_weather_indices_t;

/** 空气质量（air/now）。 */
typedef struct {
    char pub_time[32];       /*!< 数据发布时间 */
    char aqi[8];             /*!< 空气质量指数 */
    char level[8];           /*!< 空气质量等级（1-6） */
    char category[32];       /*!< 空气质量类别（如 "优"） */
    char primary[32];        /*!< 首要污染物 */
    char pm10[8];            /*!< PM10（μg/m³） */
    char pm2p5[8];           /*!< PM2.5（μg/m³） */
    char no2[8];             /*!< 二氧化氮（μg/m³） */
    char so2[8];             /*!< 二氧化硫（μg/m³） */
    char co[8];              /*!< 一氧化碳（mg/m³） */
    char o3[8];              /*!< 臭氧（μg/m³） */
} espaperplay_weather_air_t;

/** 天文（astronomy/sun + astronomy/moon）。
 *  注：sunrise/sunset/moonrise/moonset 为完整时间戳（如 "2026-08-16T05:12+08:00"），
 *  展示时可按需截取 "HH:MM"。 */
typedef struct {
    char date[16];           /*!< 日期 */
    char sunrise[24];        /*!< 日出时间（时间戳） */
    char sunset[24];         /*!< 日落时间（时间戳） */
    char moonrise[24];       /*!< 月升时间（时间戳） */
    char moonset[24];        /*!< 月落时间（时间戳） */
    char moon_phase[16];     /*!< 月相名称（如 "盈凸月"） */
    char moon_phase_icon[8]; /*!< 月相图标代码 */
} espaperplay_weather_astronomy_t;

/** 城市查询条目（GeoAPI city/lookup）。 */
typedef struct {
    char name[64];           /*!< 地区 / 城市名称 */
    char id[16];             /*!< 地区 / 城市 LocationID */
    char lat[16];            /*!< 纬度 */
    char lon[16];            /*!< 经度 */
    char adm1[64];           /*!< 该地区上级行政区划（省 / 直辖市 / 国家） */
    char adm2[64];           /*!< 该地区上级行政区划（市 / 州） */
    char country[32];        /*!< 国家 */
    char tz[64];             /*!< 时区（如 "Asia/Shanghai"） */
    char utc_offset[8];      /*!< 与 UTC 的偏移（如 "+08:00"） */
    char type[16];           /*!< 类型（如 "city"） */
} espaperplay_weather_location_t;

/** 天气数据快照（一次整体刷新得到全部数据，供展示层使用）。 */
typedef struct {
    bool valid;              /*!< 快照有效（至少成功获取过实时天气） */
    bool auto_location;      /*!< 位置是否由自动定位（公网 IP）解析 */
    char location_id[16];    /*!< 实际使用的 LocationID */
    char location_name[128]; /*!< 位置显示名称（如 "北京"） */
    char update_time[32];    /*!< 最近一次整体刷新时间 */
    espaperplay_weather_now_t now;                   /*!< 实时天气 */
    espaperplay_weather_daily_t daily[7];            /*!< 逐日预报（3 日 / 7 日） */
    int daily_count;                                 /*!< 有效预报天数 */
    espaperplay_weather_hourly_t hourly[24];         /*!< 逐小时预报 */
    int hourly_count;                                /*!< 有效小时数 */
    espaperplay_weather_minutely_t minutely;         /*!< 分钟级降水 */
    espaperplay_weather_warning_t warnings[8];       /*!< 气象灾害预警 */
    int warning_count;                               /*!< 有效预警条数 */
    espaperplay_weather_indices_t indices[16];       /*!< 天气指数 */
    int indices_count;                               /*!< 有效指数条数 */
    espaperplay_weather_air_t air;                   /*!< 空气质量 */
    espaperplay_weather_astronomy_t astronomy;       /*!< 天文 */
} espaperplay_weather_snapshot_t;

/** 天气服务运行状态。 */
typedef struct {
    bool configured;         /*!< API Key 已配置 */
    bool valid;              /*!< 快照有效 */
    bool auto_location;      /*!< 是否自动定位 */
    bool task_running;       /*!< 后台刷新任务是否运行 */
    int last_api_code;       /*!< 最近一次 QWeather 业务码（200=成功，0=无记录） */
    char last_error[160];    /*!< 最近一次错误描述（空串=无错误） */
    char location_id[16];    /*!< 实际使用的 LocationID */
    char location_name[128]; /*!< 位置显示名称 */
    char update_time[32];    /*!< 最近一次整体刷新时间 */
} espaperplay_weather_status_t;

/* ------------------------------------------------------------------ */
/* 服务生命周期                                                         */
/* ------------------------------------------------------------------ */

/**
 * @brief 启动天气后台刷新任务（幂等）。
 *
 * 任务等待 STA 联网（最多 60 秒），随后按周期（默认 10 分钟，可经
 * espaperplay_weather_set_refresh_interval_ms() 调整）执行整体刷新；
 * 每个 API 受自身 TTL 约束，未过期不重复请求。断网时任务空转等待。
 *
 * @return ESP_OK；任务创建失败返回 ESP_ERR_NO_MEM。
 */
esp_err_t espaperplay_weather_start(void);

/**
 * @brief 设置后台刷新任务的唤醒周期。
 *
 * 周期只决定任务唤醒频率，实际请求频率仍受各 API TTL 约束
 * （实时 / 分钟级 / 预警 10 分钟，预报 30-60 分钟，指数 / 天文 6 小时）。
 *
 * @param interval_ms 唤醒周期（毫秒，0=使用默认值）。
 */
void espaperplay_weather_set_refresh_interval_ms(uint32_t interval_ms);

/**
 * @brief 请求立即刷新（异步）。
 *
 * 唤醒后台任务尽快执行一次整体刷新（受各 API TTL 约束，未过期数据不
 * 重复请求）。任务未运行时为空操作。
 */
void espaperplay_weather_request_refresh(void);

/**
 * @brief 通知天气服务配置已变更（同步清空缓存并请求立即刷新）。
 *
 * Web 配置页修改 API Key / 位置后应调用本函数，使新配置尽快生效。
 */
void espaperplay_weather_config_changed(void);

/**
 * @brief 同步执行一次整体刷新（阻塞直到完成或超时）。
 *
 * 解析 / 复用位置（配置或自动定位），按各 API TTL 刷新全部数据到内存
 * 快照。后台任务与外部调用（如 Web 手动刷新）共用本函数。
 *
 * @return 实时天气获取成功返回 ESP_OK；API Key 未配置返回 ESP_ERR_INVALID_STATE；
 *         网络 / 业务码错误返回相应错误码。
 */
esp_err_t espaperplay_weather_refresh(void);

/**
 * @brief 判断天气数据是否已过期且值得发起联网刷新。
 *
 * 数据距上次成功刷新超过刷新周期，且距上次真实尝试（含失败）也已超过
 * 一周期（失败退避：持续失败时不会每次唤醒都触发重连）。API Key 未配置
 * 时恒为 false。供电源管理在定时器唤醒时判定是否借本次唤醒重连拉取；
 * 刷新周期同 espaperplay_weather_set_refresh_interval_ms()。
 */
bool espaperplay_weather_is_refresh_due(void);

/**
 * @brief 等待在途的整体刷新结束（最多 timeout_ms）。
 *
 * 后台任务未运行或无在途刷新时立即返回 true；超时返回 false。
 * 与 espaperplay_weather_request_refresh() 配合使用可同步等待一次刷新完成。
 *
 * @return true=刷新已结束（成功与否需另行查询）；false=等待超时。
 */
bool espaperplay_weather_wait_refresh_done(uint32_t timeout_ms);

/**
 * @brief 获取天气数据快照（当前缓存数据的拷贝）。
 *
 * @param out 输出快照（非空；结构较大，建议调用方在堆上分配）。
 * @return ESP_OK；参数非法返回 ESP_ERR_INVALID_ARG。
 */
esp_err_t espaperplay_weather_get_snapshot(espaperplay_weather_snapshot_t *out);

/**
 * @brief 获取天气服务运行状态。
 *
 * @param out 输出状态（非空）。
 * @return ESP_OK；参数非法返回 ESP_ERR_INVALID_ARG。
 */
esp_err_t espaperplay_weather_get_status(espaperplay_weather_status_t *out);

/**
 * @brief 清空全部缓存（各 API 数据缓存与自动定位缓存）。
 *
 * 调用后下一次查询 / 刷新强制发起实时请求。
 */
void espaperplay_weather_cache_clear(void);

/* ------------------------------------------------------------------ */
/* 单项查询 API（location 传 NULL 时使用配置位置 / 自动定位）             */
/* ------------------------------------------------------------------ */

/**
 * @brief 城市查询 / 经纬度反查（GeoAPI city/lookup）。
 *
 * @param query     查询内容：城市名（如 "北京"）、LocationID 或 "经度,纬度"。
 * @param out       输出结果数组（最多 4 条）。
 * @param out_count 输出有效条数。
 * @return ESP_OK；参数非法返回 ESP_ERR_INVALID_ARG；API Key 未配置返回
 *         ESP_ERR_INVALID_STATE。
 */
esp_err_t espaperplay_weather_location_lookup(const char *query,
                                              espaperplay_weather_location_t *out,
                                              int *out_count);

/**
 * @brief 实时天气（v7/weather/now）。
 *
 * @param location 位置（LocationID / 城市名 / "经度,纬度"；NULL=配置位置）。
 * @param out      输出实时天气（非空）。
 */
esp_err_t espaperplay_weather_query_now(const char *location, espaperplay_weather_now_t *out);

/**
 * @brief 逐日天气预报（v7/weather/3d 或 /7d）。
 *
 * @param location 位置（NULL=配置位置）。
 * @param days     预报天数：3 或 7。
 * @param out      输出预报数组（至少 7 个元素）。
 * @param out_count 输出有效天数。
 */
esp_err_t espaperplay_weather_query_daily(const char *location, int days,
                                          espaperplay_weather_daily_t *out, int *out_count);

/**
 * @brief 逐小时天气预报（v7/weather/24h，未来 24 小时）。
 *
 * @param location 位置（NULL=配置位置）。
 * @param out      输出预报数组（至少 24 个元素）。
 * @param out_count 输出有效小时数。
 */
esp_err_t espaperplay_weather_query_hourly(const char *location,
                                           espaperplay_weather_hourly_t *out, int *out_count);

/**
 * @brief 分钟级降水（v7/minutely/5m，未来 2 小时）。
 *
 * 仅中国大陆地区支持；海外位置返回 ESP_ERR_NOT_SUPPORTED。
 *
 * @param location 位置（NULL=配置位置）。
 * @param out      输出分钟级降水（非空）。
 */
esp_err_t espaperplay_weather_query_minutely(const char *location,
                                             espaperplay_weather_minutely_t *out);

/**
 * @brief 气象灾害预警（v7/warning/now）。
 *
 * @param location  位置（NULL=配置位置）。
 * @param out       输出预警数组（至少 8 个元素）。
 * @param out_count 输出有效条数（0 = 当前无预警）。
 */
esp_err_t espaperplay_weather_query_warning(const char *location,
                                            espaperplay_weather_warning_t *out, int *out_count);

/**
 * @brief 天气指数（v7/indices/1d）。
 *
 * @param location 位置（NULL=配置位置）。
 * @param type     指数类型，如 "0"（全部）、"1"（运动）、"2"（洗车）、
 *                 "3"（穿衣）、"5"（紫外线）；NULL 等价于 "0" 全部。
 * @param out      输出指数数组（至少 16 个元素）。
 * @param out_count 输出有效条数。
 */
esp_err_t espaperplay_weather_query_indices(const char *location, const char *type,
                                            espaperplay_weather_indices_t *out, int *out_count);

/**
 * @brief 空气质量（v7/air/now）。
 *
 * 仅中国大陆地区支持；海外位置返回 ESP_ERR_NOT_SUPPORTED。
 *
 * @param location 位置（NULL=配置位置）。
 * @param out      输出空气质量（非空）。
 */
esp_err_t espaperplay_weather_query_air(const char *location, espaperplay_weather_air_t *out);

/**
 * @brief 天文（v7/astronomy/sun + v7/astronomy/moon：日出日落、月升月落、月相）。
 *
 * @param location 位置（NULL=配置位置）。
 * @param out      输出天文数据（非空）。
 */
esp_err_t espaperplay_weather_query_astronomy(const char *location,
                                              espaperplay_weather_astronomy_t *out);

#ifdef __cplusplus
}
#endif
