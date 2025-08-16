#ifndef __WEATHER_H__
#define __WEATHER_H__

#include "esp_http_client.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    WEATHER_GAODE,  // 高德天气
    WEATHER_XINZHI, // 心知天气
    WEATHER_HEFENG  // 和风天气
} weather_type_t;

// 位置信息字段
typedef struct {
    char *ip_address;      // IP地址
    char *province;        // 省份
    char *city;            // 城市
    char *isp;             // 运营商
} location_info_t;

// 天气信息结构体
typedef struct {
    char* weather;          // 天气现象
    char* icon;             // 天气图标代码
    float temperature;      // 温度
    float feels_like;       // 体感温度
    float humidity;         // 湿度
    char* wind_dir;         // 风向
    char* wind_scale;       // 风力等级
    float wind_speed;       // 风速(km/h)
    float precip;           // 降水量(mm)
    float pressure;         // 气压(hPa)
    float visibility;       // 能见度(km)
    float cloud;            // 云量(%)
    float dew_point;       // 露点温度
    char* update_time;     // 更新时间
    location_info_t* location_info; // 位置信息
} weather_info_t;

//每日预报结构体
typedef struct {
    char* fx_date;             // 预报日期，如 "2021-11-15"
    char* sunrise;             // 日出时间
    char* sunset;              // 日落时间
    char* moonrise;            // 月升时间
    char* moonset;             // 月落时间
    char* moon_phase;          // 月相名称，如 "盈凸月"
    char* moon_phase_icon;     // 月相图标代号

    char* temp_max;            // 最高气温（原始为字符串）
    char* temp_min;            // 最低气温

    char* icon_day;            // 白天图标代码
    char* text_day;            // 白天天气现象
    char* icon_night;          // 夜间图标代码
    char* text_night;          // 夜间天气现象

    char* wind360_day;         // 白天风向角度
    char* wind_dir_day;        // 白天风向
    char* wind_scale_day;      // 白天风力等级
    char* wind_speed_day;      // 白天风速

    char* wind360_night;       // 夜间风向角度
    char* wind_dir_night;      // 夜间风向
    char* wind_scale_night;    // 夜间风力等级
    char* wind_speed_night;    // 夜间风速

    char* humidity;            // 相对湿度(%)
    char* precip;              // 降水量(mm)
    char* pressure;            // 大气压强(hPa)
    char* vis;                 // 能见度(km)
    char* cloud;               // 云量(%)
    char* uv_index;            // 紫外线强度指数
} daily_weather_t;

typedef struct {
    char* code;                    // 状态码
    char* update_time;            // 更新时间
    char* fx_link;                // 链接
    daily_weather_t* daily; // 每日天气数组
    int daily_count;              // 实际包含的天数
} forecast_weather_t;

typedef struct {
    char *api_key;
    char *api_host;     // 使用和风天气时需要
    char *city;         // 可选，如果为NULL则自动获取位置
    weather_type_t type;
} weather_config_t;

/**
 * @brief 获取天气信息
 * 
 * @param config    配置信息
 * @return weather_info_t*
 */
weather_info_t* weather_get(weather_config_t *config);

/**
 * @brief 获取每日预报信息
 * 
 * @param config    配置信息
 * @return forecast_weather_t*
 */
forecast_weather_t* weather_forecast(weather_config_t *config, int days);

/**
 * @brief 获取城市信息
 * 
 * @param ip     IP地址如果为NULL则自动获取
 * @return location_info_t* 
 */
location_info_t* get_city_by_ip(const char *ip);

/**
 * @brief 打印天气信息
 * 
 * @param info  天气信息 weather_info_t*
 */
void weather_print_info(const weather_info_t *info);

/**
 * @brief 打印每日天气信息
 * 
 * @param forecast  每日天气信息 forecast_weather_t*
 */
void forecast_weather_print_info(const forecast_weather_t *forecast);

/**
 * @brief 释放位置信息内存
 * 
 * @param location_info  位置信息 location_info_t*
 */
void location_info_free(location_info_t *location_info);

/**
 * @brief 释放天气信息内存
 * 
 * @param info  天气信息 weather_info_t*
 */
void weather_info_free(weather_info_t *info);

/**
 * @brief 释放每日天气信息内存
 * 
 * @param forecast  每日天气信息 forecast_weather_t*
 */
void forecast_weather_free(forecast_weather_t *forecast);

#ifdef __cplusplus
}
#endif

#endif  // __WEATHER_H__
