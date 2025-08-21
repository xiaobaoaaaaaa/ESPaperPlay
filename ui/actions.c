#include <stdio.h>
#include<time.h>

#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_mac.h"
#include "lvgl.h"

#include "actions.h"
#include "fonts.h"
#include "vars.h"
#include "ui.h"
#include "screens.h"

#include "yiyan.h"
#include "wifi_ctrl.h"
#include "config_manager.h"
#include "weather.h"
#include "tcpserver.h"

void action_user_change_screen(lv_event_t *e) 
{
    lv_event_code_t event = lv_event_get_code(e);
    lv_dir_t dir = lv_indev_get_gesture_dir(lv_event_get_indev(e));
    int gesture_dir = (int)(uintptr_t)lv_event_get_user_data(e) / 10;
    int screen_id = (int)(uintptr_t)lv_event_get_user_data(e) % 10;
    if(event == LV_EVENT_GESTURE)
    { 
        if(dir == gesture_dir)
        {
            ESP_LOGI("action_user_change_screen", "Gesture direction %d detected, changing to  screen：%d", gesture_dir, screen_id);
            int anim_type;
            switch (gesture_dir)
            {
            case LV_DIR_LEFT:
                anim_type = LV_SCR_LOAD_ANIM_MOVE_LEFT;
                break;

            case LV_DIR_RIGHT:
                anim_type = LV_SCR_LOAD_ANIM_MOVE_RIGHT;
                break;

            case LV_DIR_TOP:
                anim_type = LV_SCR_LOAD_ANIM_MOVE_TOP;
                break;

            case LV_DIR_BOTTOM:
                anim_type = LV_SCR_LOAD_ANIM_MOVE_BOTTOM;
                break;
            
            default:
                anim_type = LV_SCR_LOAD_ANIM_MOVE_LEFT;
                break;
            }
            eez_flow_set_screen(screen_id, anim_type, 200, 0);
        }
        else
        {
            ESP_LOGI("action_user_change_screen", "Unknown gesture direction %d, target direction: %d", dir, gesture_dir);
        }
    }
}

bool wifi_signal_strength_check = false;
void action_check_wifi_status(lv_event_t *e) 
{
    set_var_ui_wifi_on_off(wifi_on_off);
    if(!wifi_on_off) return;

    wifi_ap_record_t ap_info;
    esp_err_t ret = esp_wifi_sta_get_ap_info(&ap_info);

    if (ret == ESP_OK) 
    {
        int rssi = ap_info.rssi;
        if(rssi > -60 && !wifi_signal_strength_check)
        {
            set_var_wifi_rssi(rssi);
            wifi_signal_strength_check = true;
        }
        else if(rssi <= -60 && wifi_signal_strength_check)
        {
            set_var_wifi_rssi(rssi);
            wifi_signal_strength_check = false;
        }

        set_var_wifi_connected(true);
    } 
    else 
    {
        set_var_wifi_connected(false);
    }
}

void action_get_yiyan(lv_event_t *e) 
{
    get_yiyan();
}

void action_set_wifi_on_off(lv_event_t *e) 
{
    lv_obj_t *sw = lv_event_get_target(e);
    bool on_off = lv_obj_has_state(sw, LV_STATE_CHECKED);
    if(on_off)
    {
        if(wifi_on_off)
            return;
        else
        {
            ESP_LOGI("action_set_wifi_on_off", "Set WiFi on");
            set_wifi_on_off(true);
        }
    }
    else
    {
        if(!wifi_on_off) 
            return;
        else
        {
            ESP_LOGI("action_set_wifi_on_off", "Set WiFi off");
            set_wifi_on_off(false);
        }
    }
}

void action_get_wifi_ap_info(lv_event_t *e) 
{
    if(!wifi_on_off) return;

    wifi_ap_record_t ap_info;
    esp_err_t ret = esp_wifi_sta_get_ap_info(&ap_info);

    if (ret == ESP_OK) 
    {
        set_var_wifi_rssi(ap_info.rssi);

        char ssid[43];
        sprintf(ssid, "%s", ap_info.ssid);
        set_var_wifi_ssid(ssid);

        char ip[16]; // IPv4最大长度"255.255.255.255"
        esp_netif_ip_info_t ip_info;
        esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
        if (esp_netif_get_ip_info(netif, &ip_info) == ESP_OK) 
        {
            sprintf(ip, IPSTR, IP2STR(&ip_info.ip));
            set_var_wifi_ip(ip);
        }

        uint8_t mac[6];
        char mac_char[18]; // MAC地址长度17字符+1
        esp_err_t ret = esp_read_mac(mac, ESP_MAC_WIFI_STA);
        if (ret != ESP_OK) {
            ESP_LOGE("wifi settings page", "Failed to get MAC address");
        } else {
            sprintf(mac_char, "%02X:%02X:%02X:%02X:%02X:%02X", 
                mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
            set_var_wifi_mac(mac_char);
        }

        set_var_wifi_primary(ap_info.primary);

        static const char* auth_mode_strs[] = {
        [WIFI_AUTH_OPEN] = "OPEN",
        [WIFI_AUTH_WEP] = "WEP",
        [WIFI_AUTH_WPA_PSK] = "WPA_PSK",
        [WIFI_AUTH_WPA2_PSK] = "WPA2_PSK",
        [WIFI_AUTH_WPA_WPA2_PSK] = "WPA_WPA2_PSK",
        [WIFI_AUTH_ENTERPRISE] = "ENTERPRISE",
        [WIFI_AUTH_WPA3_PSK] = "WPA3_PSK",
        [WIFI_AUTH_WPA2_WPA3_PSK] = "WPA2_WPA3_PSK",
        [WIFI_AUTH_WAPI_PSK] = "WAPI_PSK",
        [WIFI_AUTH_OWE] = "OWE",
        [WIFI_AUTH_WPA3_ENT_192] = "WPA3_ENT_192",
        [WIFI_AUTH_WPA3_EXT_PSK] = "WPA3_EXT_PSK (Deprecated)",
        [WIFI_AUTH_WPA3_EXT_PSK_MIXED_MODE] = "WPA3_EXT_PSK_MIXED (Deprecated)",
        [WIFI_AUTH_DPP] = "DPP",
        [WIFI_AUTH_WPA3_ENTERPRISE] = "WPA3_ENTERPRISE",
        [WIFI_AUTH_WPA2_WPA3_ENTERPRISE] = "WPA2_WPA3_ENTERPRISE",
        // 添加默认项防止越界
        [WIFI_AUTH_MAX] = "UNKNOWN"
        };

        set_var_wifi_authmode(auth_mode_strs[ap_info.authmode]);

        static const char* bandwidth[] = {
        [WIFI_BW20] = "20MHz",
        [WIFI_BW40] = "40MHz",
        [WIFI_BW80] = "80MHz",
        [WIFI_BW160] = "160HMz",
        [WIFI_BW80_BW80] = "80+80MHz"
        };

        set_var_wifi_bandwidth(bandwidth[ap_info.authmode]);
    } 
}

void action_wifi_reconnect(lv_event_t *e) 
{
    set_wifi_on_off(false);
    start_smartconfig();
}

void action_power_save_on_off(lv_event_t *e) 
{
    system_config_t *cfg = config_get_mutable();
    lv_obj_t *sw = lv_event_get_target(e);
    cfg->power_save_enabled = lv_obj_has_state(sw, LV_STATE_CHECKED);
    config_save();
    ESP_LOGI("action_power_save_on_off", "Power save mode turned %s", cfg->power_save_enabled ? "on" : "off");
}

void action_check_power_save_mode(lv_event_t *e) 
{
    const system_config_t *cfg = config_get();
    set_var_is_power_save_enabled(cfg->power_save_enabled);
    set_var_power_save_min(cfg->power_save_min);
}

void action_set_power_save_min(lv_event_t *e) 
{
    lv_obj_t *dropdown = lv_event_get_target(e);
    system_config_t *cfg = config_get_mutable();
    switch (lv_dropdown_get_selected(dropdown))
    {
        case 0:
            cfg->power_save_min = 3;
            break;

        case 1:
            cfg->power_save_min = 5;
            break;

        case 2:
            cfg->power_save_min = 10;
            break;

        case 3:
            cfg->power_save_min = 30;
            break;
        
        default:
            cfg->power_save_min = 3;
            break;
    }
    ESP_LOGI("action_set_power_save_min", "Set power save min: %d", cfg->power_save_min);
    config_save();
}

lv_obj_t *chart = NULL;
static lv_chart_series_t *ser_max = NULL;
static lv_chart_series_t *ser_min = NULL;

static forecast_weather_t* g_forecast = NULL;
static SemaphoreHandle_t weather_mutex = NULL;

static void update_weather_ui(void *param) {
    forecast_weather_t *forecast = (forecast_weather_t *)param;
    if (!forecast || !chart || !lv_obj_is_valid(chart)) return;

    int temp_max = -999;
    int temp_min = 999;
    for (int i = 0; i < 7; i++) 
    {
        if(atoi(forecast->daily[i].temp_max) > temp_max)
        {
            temp_max = atoi(forecast->daily[i].temp_max);
        }
        if(atoi(forecast->daily[i].temp_min) < temp_min)
        {
            temp_min = atoi(forecast->daily[i].temp_min);
        }
        ser_max->y_points[i] = atoi(forecast->daily[i].temp_max);
        ser_min->y_points[i] = atoi(forecast->daily[i].temp_min);
    }

    char temp_str[128] = {0};
    char weather_str[128] = {0};
    int temp_len = 0;
    int weather_len = 0;

    for (int i = 0; i < 7; i++) {
        temp_len += snprintf(temp_str + temp_len, sizeof(temp_str) - temp_len, "%s%s",
                        forecast->daily[i].temp_max,
                        (i < 6) ? "," : "");
        weather_len += snprintf(weather_str + weather_len, sizeof(weather_str) - weather_len, "%s%s" ,
                        forecast->daily[i].text_day,
                        (i < 6) ? "," : "");
    }
    set_var_temp_max(temp_str);
    set_var_weather_day(weather_str);

    memcpy(temp_str, "", sizeof(temp_str));
    memcpy(weather_str, "", sizeof(weather_str));
    weather_len = 0;
    temp_len = 0;
    for (int i = 0; i < 7; i++)
    {
        temp_len += snprintf(temp_str + temp_len, sizeof(temp_str) - temp_len, "%s%s",
                        forecast->daily[i].temp_min,
                        (i < 6) ? "," : "");
        weather_len += snprintf(weather_str + weather_len, sizeof(weather_str) - weather_len, "%s%s" ,
                        forecast->daily[i].text_night,
                        (i < 6) ? "," : "");
    }
    set_var_temp_min(temp_str);
    set_var_weather_night(weather_str);

    ESP_LOGI("task_get_weather", "Max temperature: %d, Min temperature: %d", temp_max, temp_min);
    
    lv_chart_set_range(chart, LV_CHART_AXIS_PRIMARY_Y, temp_min, temp_max);
    lv_chart_set_range(chart, LV_CHART_AXIS_SECONDARY_Y, temp_min, temp_max);
    ESP_LOGI("task_get_weather", "Weather UI updated");
}

static void task_get_weather(void *param) {
    system_config_t *cfg = config_get_mutable();
    weather_config_t config = {
        .api_key = cfg->weather_api_key,
        .api_host = cfg->weather_api_host,
        .city = cfg->weather_city,
        .type = WEATHER_HEFENG
    };

    weather_info_t *info = weather_get(&config);
    if (info) {
        if (info->weather) set_var_weather(info->weather);
        set_var_weather_temp(info->temperature);
        set_var_weather_feels_like(info->feels_like);
        set_var_weather_humidity(info->humidity);
        if (info->wind_dir) set_var_weather_wind_dir(info->wind_dir);
        if (info->wind_scale) set_var_weather_wind_scale(info->wind_scale);
        set_var_weather_wind_speed(info->wind_speed);
        set_var_weather_precip(info->precip);
        set_var_weather_pressure(info->pressure);
        set_var_weather_visibility(info->visibility);
        set_var_weather_cloud(info->cloud);
        set_var_weather_dew_point(info->dew_point);
        if(atoi(info->icon) > 100)
        {
            ESP_LOGI("task_get_weather", "Weather icon: %s", info->icon);
            set_var_weather_icon(info->icon);
        }
        else
        {
            ESP_LOGW("task_get_weather", "Weather icon is empty");
        }
        if (info->update_time) set_var_weather_update_time(info->update_time);

        if (info->location_info && info->location_info->city)
            set_var_weather_city(info->location_info->city);

        weather_info_free(info);
    }
    set_var_weather_updated(true);

    forecast_weather_t *forecast = weather_forecast(&config, 7);

    // 互斥锁保护共享数据
    if (weather_mutex == NULL) {
        weather_mutex = xSemaphoreCreateMutex();
    }
    xSemaphoreTake(weather_mutex, portMAX_DELAY);
    if (g_forecast) {
        forecast_weather_free(g_forecast);
        g_forecast = NULL;
    }
    g_forecast = forecast;
    xSemaphoreGive(weather_mutex);

    if (forecast && forecast->daily_count) {
        // UI更新异步调用
        lv_async_call(update_weather_ui, forecast);
    }
    ESP_LOGI("task_get_weather", "Weather data updated successfully");
    vTaskDelete(NULL);
}

void action_get_weather(lv_event_t *e) 
{
    if(chart == NULL)
    {
        chart = objects.chart_weather_forecast_temp;
        if(chart && lv_obj_is_valid(chart))
        {
            ESP_LOGI("action_get_weather", "Chart object initialized");
            lv_chart_set_type(chart, LV_CHART_TYPE_LINE);
            lv_chart_set_update_mode(chart, LV_CHART_UPDATE_MODE_CIRCULAR);
            lv_chart_set_point_count(chart, 7);
            lv_chart_set_range(chart, LV_CHART_AXIS_PRIMARY_Y, -10, 40);
            lv_chart_set_range(chart, LV_CHART_AXIS_SECONDARY_Y, -10, 40);
            lv_chart_set_div_line_count(chart, 0, 7);
            ser_max = lv_chart_add_series(chart, lv_color_hex(0xff000000), LV_CHART_AXIS_PRIMARY_Y);
            ser_min = lv_chart_add_series(chart, lv_color_hex(0xff000000), LV_CHART_AXIS_PRIMARY_Y);
        }
    }
    // 创建异步任务
    xTaskCreate(task_get_weather, "task_get_weather", 5100, NULL, 7, NULL);
}

void action_get_weather_settings(lv_event_t *e) 
{
    const system_config_t *cfg = config_get();
    ESP_LOGI("action_get_weather_settings", "Weather city: %s, API key: %s, API host: %s",
             cfg->weather_city, cfg->weather_api_key, cfg->weather_api_host);

    set_var_weather_city_in_nvs(cfg->weather_city);
    set_var_weather_api_key(cfg->weather_api_key);
    set_var_weather_api_host(cfg->weather_api_host);
}

TaskHandle_t  save_tcp_msg_task_hander = NULL;
bool save_tcp_msg_task_running = false;
void task_save_tcp_msg(void *param)
{
    char msg[128];
    while(save_tcp_msg_task_running)
    {
        if(xQueueReceive(tcp_msg_queue, msg, pdMS_TO_TICKS(1000)) == pdTRUE)
        {
            set_var_tcp_msg(msg);
            break;
        }
        else
        {
            continue;
        }
    }

    save_tcp_msg_task_hander = NULL;
    save_tcp_msg_task_running = false;
    vTaskDelete(NULL);
}

void action_get_tcp_msg(lv_event_t *e) 
{
    tcpserver_create();
    if(!save_tcp_msg_task_hander)
    {
        save_tcp_msg_task_running = true;
        xTaskCreate(task_save_tcp_msg, "task_save_tcp_msg", 2048, NULL, 5, NULL);
    }
    else
    {
        ESP_LOGW("action_get_tcp_msg", "Tcp msg get task is running");
    }

    ESP_LOGI("action_get_tcp_msg", "TCP message task created");
}

void action_save_weather_config(lv_event_t *e) 
{
    system_config_t *cfg = config_get_mutable();
    strcpy(cfg->weather_city, get_var_weather_city_in_nvs());
    strcpy(cfg->weather_api_key, get_var_weather_api_key());
    strcpy(cfg->weather_api_host, get_var_weather_api_host());
    ESP_LOGI("action_save_weather_config", "Weather config saved: City: %s, API Key: %s, API Host: %s",
             cfg->weather_city, cfg->weather_api_key, cfg->weather_api_host);
    config_save();
}

void action_close_tcp_server(lv_event_t *e) 
{
    save_tcp_msg_task_running = false;
    tcp_server_stop();
}

void action_update_calendar(lv_event_t *e) 
{
    int index = 0;
    lv_obj_t *child = lv_obj_get_child(objects.calendar, index);
    while(child)
    {
        if(lv_obj_check_type(child, &lv_calendar_header_arrow_class))
        {
            lv_obj_del(child);
            break;
        }
        index++;
        child = lv_obj_get_child(objects.calendar, index);
    }
    lv_calendar_header_dropdown_create(objects.calendar);
}

void action_timer_get_setted(lv_event_t *e) 
{
    
}