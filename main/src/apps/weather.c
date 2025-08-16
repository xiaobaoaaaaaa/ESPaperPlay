#include "weather.h"
#include "esp_log.h"
#include "cJSON.h"
#include "esp_http_client.h"
#include "esp_netif.h"
#include "lwip/dns.h"
#include "esp_crt_bundle.h"
#include <ctype.h>
#include "zlib.h"
static const char *TAG = "WEATHER";

// 定义安全释放宏
#define SAFE_FREE(ptr) do { if (ptr) { free(ptr); ptr = NULL; } } while(0)

// 内部使用的缓冲区
static char weather_buffer[4096];   
static size_t weather_len = 0;

// 用于存储 Content-Encoding 头部值
static char content_encoding_value[32] = {0}; 

// URL编码函数
static void url_encode(char *dest, const char *src, size_t max_len) {
    size_t i = 0, j = 0;
    while (src[i] != '\0' && j < max_len - 1) {
        unsigned char c = (unsigned char)src[i];
        if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
            dest[j++] = src[i++];
        } else {
            if (j + 3 >= max_len) break;  // 额外的安全检查
            snprintf(&dest[j], max_len - j, "%%%02X", c);
            j += 3;
            i++;
        }
    }
    dest[j] = '\0';
}

// HTTP事件处理器
static esp_err_t weather_http_handler(esp_http_client_event_t *evt) {
    switch(evt->event_id) {
        case HTTP_EVENT_ON_HEADER:
            // 存储Content-Encoding头
            if (strcasecmp(evt->header_key, "Content-Encoding") == 0) {
                strlcpy(content_encoding_value, evt->header_value, sizeof(content_encoding_value));
            }
            break;

        case HTTP_EVENT_ON_DATA:
            if (weather_len + evt->data_len >= sizeof(weather_buffer)) {
                ESP_LOGE(TAG, "Buffer overflow");
                return ESP_FAIL;
            }
            memcpy(weather_buffer + weather_len, evt->data, evt->data_len);
            weather_len += evt->data_len;
            break;
        default:
            break;
    }
    return ESP_OK;
}

//解压Gzip数据
#define MAX_UNCOMPRESSED_SIZE 8192
static char uncompressed_buffer[MAX_UNCOMPRESSED_SIZE];

static char* decompress_gzip_data(const char *compressed_data, int compressed_len) {
    z_stream strm = {0};
    int ret;

    ret = inflateInit2(&strm, 16 + MAX_WBITS);
    if (ret != Z_OK) {
        ESP_LOGE(TAG, "inflateInit2 failed: %d", ret);
        return NULL;
    }

    strm.avail_in = compressed_len;
    strm.next_in = (Bytef *)compressed_data;
    strm.avail_out = MAX_UNCOMPRESSED_SIZE;
    strm.next_out = (Bytef *)uncompressed_buffer;

    ret = inflate(&strm, Z_FINISH);
    inflateEnd(&strm);

    if (ret == Z_STREAM_END) {
        uncompressed_buffer[strm.total_out] = '\0';
        return strdup(uncompressed_buffer);
    }

    ESP_LOGE(TAG, "Decompression failed");
    return NULL;
}

// 执行HTTP请求
static char* weather_http_request(const char *url) {
    esp_http_client_config_t config = {
        .url = url,
        .method = HTTP_METHOD_GET,
        .event_handler = weather_http_handler,
        .timeout_ms = 60000,
        .buffer_size = 3072,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };

    esp_http_client_handle_t client = esp_http_client_init(&config);

    
    weather_len = 0;
    memset(weather_buffer, 0, sizeof(weather_buffer));
    memset(content_encoding_value, 0, sizeof(content_encoding_value));  // 清空编码信息

    esp_err_t err = esp_http_client_perform(client);    // 执行HTTP请求
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Request failed: %s", esp_err_to_name(err));
        esp_http_client_cleanup(client);
        return NULL;
    }

    if (weather_len >= sizeof(weather_buffer)) {
        weather_len = sizeof(weather_buffer) - 1;
    }
    weather_buffer[weather_len] = '\0';

    // 检查是否是 gzip 压缩数据
    if (content_encoding_value[0] && strcmp(content_encoding_value, "gzip") == 0) {
        ESP_LOGI(TAG, "Data is gzip compressed, decompressing...");
        char *uncompressed_data = decompress_gzip_data(weather_buffer, weather_len);
        esp_http_client_cleanup(client);
        return uncompressed_data;
    }

    esp_http_client_cleanup(client);
    return strdup(weather_buffer); // 只在这里进行一次内存分配
}

location_info_t* get_city_by_ip(const char *ip) {
    char url[128];
    snprintf(url, sizeof(url), "https://api.vore.top/api/IPdata?ip=%s", ip ? ip : "");

    char *response = weather_http_request(url);
    if (!response) return NULL;

    cJSON *root = cJSON_Parse(response);
    if (!root) {
        free(response);
        return NULL;
    }

    cJSON *code = cJSON_GetObjectItem(root, "code");
    if (!code || !cJSON_IsNumber(code) || code->valueint != 200) {
        cJSON_Delete(root);
        free(response);
        return NULL;
    }

    location_info_t *loc_info = calloc(1, sizeof(location_info_t));
    if (!loc_info) {
        cJSON_Delete(root);
        free(response);
        return NULL;
    }

    // 获取嵌套字段
    cJSON *ipdata = cJSON_GetObjectItem(root, "ipdata");
    cJSON *ipinfo = cJSON_GetObjectItem(root, "ipinfo");

    if (ipdata) {
        cJSON *province = cJSON_GetObjectItem(ipdata, "info1");
        cJSON *city = cJSON_GetObjectItem(ipdata, "info2");
        cJSON *isp = cJSON_GetObjectItem(ipdata, "isp");

        if (province && cJSON_IsString(province))
            loc_info->province = strdup(province->valuestring);
        if (city && cJSON_IsString(city))
            loc_info->city = strdup(city->valuestring);
        if (isp && cJSON_IsString(isp))
            loc_info->isp = strdup(isp->valuestring);
    }

    if (ipinfo) {
        cJSON *ipaddr = cJSON_GetObjectItem(ipinfo, "text");
        if (ipaddr && cJSON_IsString(ipaddr))
            loc_info->ip_address = strdup(ipaddr->valuestring);
    }

    cJSON_Delete(root);
    free(response);
    return loc_info;
}


// 解析高德天气
static weather_info_t* parse_gaode(const char *json) {
    cJSON *root = cJSON_Parse(json);
    if (!root) return NULL;
    
    weather_info_t *info = calloc(1, sizeof(weather_info_t));
    if (!info) {
        cJSON_Delete(root);
        return NULL;
    }
    
    cJSON *lives = cJSON_GetObjectItem(root, "lives");
    if (lives && cJSON_IsArray(lives)) {
        cJSON *item = cJSON_GetArrayItem(lives, 0);
        if (item) {
            info->weather = strdup(cJSON_GetObjectItem(item, "weather")->valuestring);
            info->temperature = atof(cJSON_GetObjectItem(item, "temperature_float")->valuestring);
            info->humidity = atof(cJSON_GetObjectItem(item, "humidity_float")->valuestring);
            info->wind_dir = strdup(cJSON_GetObjectItem(item, "winddirection")->valuestring);
            info->wind_scale = strdup(cJSON_GetObjectItem(item, "windpower")->valuestring);
            info->update_time = strdup(cJSON_GetObjectItem(item, "reporttime")->valuestring);
        }
    }
    
    cJSON_Delete(root);
    return info;
}

// 解析心知天气
static weather_info_t* parse_xinzhi(const char *json) {
    cJSON *root = cJSON_Parse(json);
    if (!root) return NULL;
    
    weather_info_t *info = calloc(1, sizeof(weather_info_t));
    if (!info) {
        cJSON_Delete(root);
        return NULL;
    }
    
    cJSON *results = cJSON_GetObjectItem(root, "results");
    if (results && cJSON_IsArray(results)) {
        cJSON *item = cJSON_GetArrayItem(results, 0);
        if (item) {
            cJSON *now = cJSON_GetObjectItem(item, "now");
            if (now) {
                // 提取天气信息
                info->weather = strdup(cJSON_GetObjectItem(now, "text")->valuestring);
                info->temperature = atof(cJSON_GetObjectItem(now, "temperature")->valuestring);

                // 提取最后更新时间
                cJSON *last_update = cJSON_GetObjectItem(item, "last_update");
                if (last_update) {
                    info->update_time = strdup(last_update->valuestring);
                }
            }
        }
    }
    
    cJSON_Delete(root);
    return info;
}

// 获取和风天气的城市ID
static char* get_hefeng_location_id(const char *city_name, const char *api_host, const char *api_key) {
    char url[256];
    char encoded_city[64] = {0};
    url_encode(encoded_city, city_name, sizeof(encoded_city));
    
    snprintf(url, sizeof(url), "https://%s/geo/v2/city/lookup?key=%s&location=%s&number=1", 
            api_host, api_key, encoded_city);

    char *response = weather_http_request(url);
    if (!response) return NULL;

    cJSON *root = cJSON_Parse(response);
    if (!root) {
        free(response);
        return NULL;
    }
    char *location_id = NULL;
    cJSON *code = cJSON_GetObjectItem(root, "code");
    if (code && cJSON_IsString(code) && strcmp(code->valuestring, "200") == 0) {
        cJSON *location = cJSON_GetObjectItem(root, "location");
        if (location && cJSON_IsArray(location)) {
            cJSON *first_item = cJSON_GetArrayItem(location, 0);
            if (first_item) {
                cJSON *id = cJSON_GetObjectItem(first_item, "id");
                if (id && cJSON_IsString(id)) {
                    location_id = strdup(id->valuestring);
                }
            }
        }
    }
    cJSON_Delete(root);
    free(response);
    return location_id;
}

// 解析和风天气
static weather_info_t* parse_hefeng(const char *json) {
    cJSON *root = cJSON_Parse(json);
    if (!root) return NULL;
    
    weather_info_t *info = calloc(1, sizeof(weather_info_t));
    if (!info) {
        cJSON_Delete(root);
        return NULL;
    }
    
    cJSON *now = cJSON_GetObjectItem(root, "now");
    if (now) {
        // 基本天气信息
        info->weather = strdup(cJSON_GetObjectItem(now, "text")->valuestring);
        info->temperature = atof(cJSON_GetObjectItem(now, "temp")->valuestring);
        info->feels_like = atof(cJSON_GetObjectItem(now, "feelsLike")->valuestring);
        info->humidity = atof(cJSON_GetObjectItem(now, "humidity")->valuestring);

        // 天气图标信息
        info->icon = strdup(cJSON_GetObjectItem(now, "icon")->valuestring);
        
        // 风信息
        info->wind_dir = strdup(cJSON_GetObjectItem(now, "windDir")->valuestring);
        info->wind_scale = strdup(cJSON_GetObjectItem(now, "windScale")->valuestring);
        info->wind_speed = atof(cJSON_GetObjectItem(now, "windSpeed")->valuestring);
        
        // 其他气象数据
        info->precip = atof(cJSON_GetObjectItem(now, "precip")->valuestring);
        info->pressure = atof(cJSON_GetObjectItem(now, "pressure")->valuestring);
        info->visibility = atof(cJSON_GetObjectItem(now, "vis")->valuestring);
        info->cloud = atof(cJSON_GetObjectItem(now, "cloud")->valuestring);
        info->dew_point = atof(cJSON_GetObjectItem(now, "dew")->valuestring);
        
        // 更新时间
        info->update_time = strdup(cJSON_GetObjectItem(root, "updateTime")->valuestring);
    }
    
    cJSON_Delete(root);
    return info;
}

//解析和风天气每日天气预报
forecast_weather_t* parse_forecast_weather(const char *json, int n_days) {
    if (!json || n_days <= 0) return NULL;

    cJSON *root = cJSON_Parse(json);
    if (!root) return NULL;

    forecast_weather_t *forecast = calloc(1, sizeof(forecast_weather_t));
    if (!forecast) {
        cJSON_Delete(root);
        return NULL;
    }

    forecast->code = strdup(cJSON_GetObjectItem(root, "code")->valuestring);
    forecast->update_time = strdup(cJSON_GetObjectItem(root, "updateTime")->valuestring);
    forecast->fx_link = strdup(cJSON_GetObjectItem(root, "fxLink")->valuestring);

    cJSON *daily_array = cJSON_GetObjectItem(root, "daily");
    int total_days = cJSON_GetArraySize(daily_array);
    int actual_days = n_days < total_days ? n_days : total_days;

    forecast->daily = calloc(actual_days, sizeof(daily_weather_t));
    forecast->daily_count = actual_days;

    for (int i = 0; i < actual_days; ++i) {
        cJSON *item = cJSON_GetArrayItem(daily_array, i);
        daily_weather_t *d = &forecast->daily[i];

        d->fx_date = strdup(cJSON_GetObjectItem(item, "fxDate")->valuestring);
        d->sunrise = strdup(cJSON_GetObjectItem(item, "sunrise")->valuestring);
        d->sunset = strdup(cJSON_GetObjectItem(item, "sunset")->valuestring);
        d->moonrise = strdup(cJSON_GetObjectItem(item, "moonrise")->valuestring);
        d->moonset = strdup(cJSON_GetObjectItem(item, "moonset")->valuestring);
        d->moon_phase = strdup(cJSON_GetObjectItem(item, "moonPhase")->valuestring);
        d->moon_phase_icon = strdup(cJSON_GetObjectItem(item, "moonPhaseIcon")->valuestring);

        d->temp_max = strdup(cJSON_GetObjectItem(item, "tempMax")->valuestring);
        d->temp_min = strdup(cJSON_GetObjectItem(item, "tempMin")->valuestring);

        d->icon_day = strdup(cJSON_GetObjectItem(item, "iconDay")->valuestring);
        d->text_day = strdup(cJSON_GetObjectItem(item, "textDay")->valuestring);
        d->icon_night = strdup(cJSON_GetObjectItem(item, "iconNight")->valuestring);
        d->text_night = strdup(cJSON_GetObjectItem(item, "textNight")->valuestring);

        d->wind360_day = strdup(cJSON_GetObjectItem(item, "wind360Day")->valuestring);
        d->wind_dir_day = strdup(cJSON_GetObjectItem(item, "windDirDay")->valuestring);
        d->wind_scale_day = strdup(cJSON_GetObjectItem(item, "windScaleDay")->valuestring);
        d->wind_speed_day = strdup(cJSON_GetObjectItem(item, "windSpeedDay")->valuestring);

        d->wind360_night = strdup(cJSON_GetObjectItem(item, "wind360Night")->valuestring);
        d->wind_dir_night = strdup(cJSON_GetObjectItem(item, "windDirNight")->valuestring);
        d->wind_scale_night = strdup(cJSON_GetObjectItem(item, "windScaleNight")->valuestring);
        d->wind_speed_night = strdup(cJSON_GetObjectItem(item, "windSpeedNight")->valuestring);

        d->humidity = strdup(cJSON_GetObjectItem(item, "humidity")->valuestring);
        d->precip = strdup(cJSON_GetObjectItem(item, "precip")->valuestring);
        d->pressure = strdup(cJSON_GetObjectItem(item, "pressure")->valuestring);
        d->vis = strdup(cJSON_GetObjectItem(item, "vis")->valuestring);
        d->cloud = strdup(cJSON_GetObjectItem(item, "cloud")->valuestring);
        d->uv_index = strdup(cJSON_GetObjectItem(item, "uvIndex")->valuestring);
    }

    cJSON_Delete(root);
    return forecast;
}

weather_info_t* weather_get(weather_config_t *config) {

    location_info_t *location_info = NULL;
    // 如果配置中指定了位置，直接使用
    if (config->city && strlen(config->city) > 0) {
        location_info = calloc(1, sizeof(location_info_t));
        location_info->city = strdup(config->city);
    } 
    // 否则自动获取位置
    else {
        // 通过IP获取城市信息
        location_info = get_city_by_ip(NULL);
        
        if (!location_info) {
            ESP_LOGE(TAG, "Failed to get city by IP, Using default location");
            location_info = calloc(1, sizeof(location_info_t));
            location_info->province = strdup("陕西省");
            location_info->city = strdup("西安市");
            location_info->isp = strdup("电信");
        }
        ESP_LOGI(TAG, "Location: %s %s", location_info->province, location_info->city);
    }

    char url[256];
    weather_info_t *weather_info = NULL;
    char encoded_city[64] = {0};
    char *response = NULL;
    switch(config->type) {
        case WEATHER_GAODE:
            if(!config->api_key){
                ESP_LOGE(TAG, "GAODE API key is not configured");
                return NULL;
            }
            url_encode(encoded_city, location_info->city, sizeof(encoded_city));
            snprintf(url, sizeof(url), "https://restapi.amap.com/v3/weather/weatherInfo?city=%s&key=%s", 
                    encoded_city, config->api_key);
            response = weather_http_request(url);
            if (response) {
                weather_info = parse_gaode(response);
                free(response); // 解析完成后释放响应内存
            }
            break;
        
                
        case WEATHER_XINZHI: 
            if(!config->api_key){
                ESP_LOGE(TAG, "XINZHI API key is not configured");
                return NULL;
            }
            url_encode(encoded_city, location_info->city, sizeof(encoded_city));
            snprintf(url, sizeof(url), "https://api.seniverse.com/v3/weather/now.json?key=%s&location=%s&language=zh-Hans&unit=c", 
                    config->api_key, encoded_city);
            response = weather_http_request(url);
            if (response) {
                weather_info = parse_xinzhi(response);
                free(response);
            }
            break;
        
                
        case WEATHER_HEFENG:
            if (!config->api_host || !config->api_key) {
                ESP_LOGE(TAG, "HEFENG API host or key is not configured");
                return NULL;
            }
            char *location_id = get_hefeng_location_id(location_info->city, config->api_host, config->api_key);
            if (!location_id) {
                ESP_LOGE(TAG, "Failed to get location ID for city: %s", location_info->city);
                location_info_free(location_info);
                return NULL;
            }
            
            snprintf(url, sizeof(url), "https://%s/v7/weather/now?location=%s&key=%s", 
                    config->api_host, location_id, config->api_key);
            response = weather_http_request(url);
            if (response) {
                weather_info = parse_hefeng(response);
                free(response);
            }
            
            free(location_id);
            break;
            
        default:
            break;
    }
    if(weather_info!=NULL) {
        weather_info->location_info = location_info; // 保存位置信息
    }
    return weather_info;
}

forecast_weather_t* weather_forecast(weather_config_t *config, int days)
{
    
    location_info_t *location_info = NULL;
    // 如果配置中指定了位置，直接使用
    if (config->city && strlen(config->city) > 0) {
        location_info = calloc(1, sizeof(location_info_t));
        location_info->city = strdup(config->city);
    } 
    // 否则自动获取位置
    else {
        // 通过IP获取城市信息
        location_info = get_city_by_ip(NULL);
        
        if (!location_info) {
            ESP_LOGE(TAG, "Failed to get city by IP, Using default location");
            location_info = calloc(1, sizeof(location_info_t));
            location_info->province = strdup("陕西省");
            location_info->city = strdup("西安市");
            location_info->isp = strdup("电信");
        }
        ESP_LOGI(TAG, "Location: %s %s", location_info->province, location_info->city);
    }

    char url[256];
    forecast_weather_t *forecast_weather = NULL;
    char encoded_city[64] = {0};
    char *response = NULL;
    switch(config->type) {
        case WEATHER_HEFENG:
            if (!config->api_host || !config->api_key) {
                ESP_LOGE(TAG, "HEFENG API host or key is not configured");
                return NULL;
            }
            char *location_id = get_hefeng_location_id(location_info->city, config->api_host, config->api_key);
            if (!location_id) {
                ESP_LOGE(TAG, "Failed to get location ID for city: %s", location_info->city);
                location_info_free(location_info);
                return NULL;
            }
            
            snprintf(url, sizeof(url), "https://%s/v7/weather/%dd?location=%s&key=%s", 
                    config->api_host, days, location_id, config->api_key);
            response = weather_http_request(url);
            if (response) {
                forecast_weather = parse_forecast_weather(response, days);
                free(response);
            }
            
            free(location_id);
            break;
            
        default:
            break;
    }

    return forecast_weather;
}

void weather_print_info(const weather_info_t *info) {
    if (!info) return;
    
    // 天气符号映射
    const char* weather_icon = "☁️";
    if (info->weather) {
        if (strstr(info->weather, "晴")) weather_icon = "☀️";
        else if (strstr(info->weather, "雨")) weather_icon = "🌧️";
        else if (strstr(info->weather, "雪")) weather_icon = "❄️";
        else if (strstr(info->weather, "雷")) weather_icon = "⛈️";
        else if (strstr(info->weather, "雾")) weather_icon = "🌫️";
        else if (strstr(info->weather, "风")) weather_icon = "🌬️";
    }

    printf("\n  🌈 WEATHER REPORT %s\n", weather_icon);
    printf("%s\n"," _ _ _ _____ _____ _____ _____ _____ _____ ");
    printf("%s\n","| | | |   __|  _  |_   _|  |  |   __| __  |");
    printf("%s\n","| | | |   __|     | | | |     |   __|    -|");
    printf("%s\n","|_____|_____|__|__| |_| |__|__|_____|__|__|\n");

    // 位置信息
    if (info->location_info) {
        char city[64] = {0};
        if (info->location_info->province && info->location_info->city) {
            snprintf(city, sizeof(city), "%s·%s", 
                    info->location_info->province, 
                    info->location_info->city);
        } else if (info->location_info->province) {
            snprintf(city, sizeof(city), "%s", 
                    info->location_info->province);
        } else if (info->location_info->city) {
            snprintf(city, sizeof(city), "%s", 
                    info->location_info->city);
        }
        printf("📍 位置: %s\n", city);
    }
    
    // 预格式化带单位的数据
    char temp_str[16] = "", feels_str[16] = "", humidity_str[16] = "";
    char precip_str[16] = "", pressure_str[16] = "", vis_str[16] = "";
    char cloud_str[16] = "", dew_str[16] = "", wspeed_str[16] = "";
    
    if (info->temperature != 0.0f) snprintf(temp_str, sizeof(temp_str), "%.1f℃", info->temperature);
    if (info->feels_like != 0.0f) snprintf(feels_str, sizeof(feels_str), "%.1f℃", info->feels_like);
    if (info->humidity != 0.0f) snprintf(humidity_str, sizeof(humidity_str), "%.1f%%", info->humidity);
    if (info->wind_speed != 0.0f) snprintf(wspeed_str, sizeof(wspeed_str), "%.1fkm/h", info->wind_speed);
    if (info->precip != 0.0f) snprintf(precip_str, sizeof(precip_str), "%.1fmm", info->precip);
    if (info->pressure != 0.0f) snprintf(pressure_str, sizeof(pressure_str), "%.1fhPa", info->pressure);
    if (info->visibility != 0.0f) snprintf(vis_str, sizeof(vis_str), "%.1fkm", info->visibility);
    if (info->cloud != 0.0f) snprintf(cloud_str, sizeof(cloud_str), "%.1f%%", info->cloud);
    if (info->dew_point != 0.0f) snprintf(dew_str, sizeof(dew_str), "%.1f℃", info->dew_point);

    printf("┌───────────────────────────────────────┐\n");
    if (info->weather) printf("│ %s  天气: %-29s │\n", weather_icon, info->weather);
    if (info->temperature != 0.0f) printf("│ 🌡️  温度: %-30s │\n", temp_str);
    if (info->feels_like != 0.0f) printf("│ 🤒 体感: %-30s │\n", feels_str);
    if (info->humidity != 0.0f) printf("│ 💧 湿度: %-28s │\n", humidity_str);
    if (info->wind_dir) printf("│ 🍃 风向: %-31s │\n", info->wind_dir);
    if (info->wind_scale) printf("│ 💨 风力: %-28s │\n", info->wind_scale);
    if (info->wind_speed != 0.0f) printf("│ 🌬️  风速: %-28s │\n", wspeed_str);
    if (info->precip != 0.0f) printf("│ 🌧️  降水: %-28s │\n", precip_str);
    if (info->pressure != 0.0f) printf("│ ⏲️  气压: %-28s │\n", pressure_str);
    if (info->visibility != 0.0f) printf("│ 👁️  能见度: %-26s │\n", vis_str);
    if (info->cloud != 0.0f) printf("│ ☁️  云量: %-28s │\n", cloud_str);
    if (info->dew_point != 0.0f) printf("│ 💦 露点: %-30s │\n", dew_str);
    printf("└───────────────────────────────────────┘\n");
    
    if (info->update_time) printf("\n🕒 更新时间: %s\n", info->update_time);
    printf("\n");
}

void forecast_weather_print_info(const forecast_weather_t *forecast) {
    if (!forecast) return;

    printf("\n📅 %d-Day Forecast Report\n", forecast->daily_count);
    printf("📈 数据更新于: %s\n", forecast->update_time);
    printf("🔗 查看详情: %s\n", forecast->fx_link);
    printf("%s\n", "╭────────────────────────────────────────────╮");

    for (int i = 0; i < forecast->daily_count; ++i) {
        const daily_weather_t *d = &forecast->daily[i];

        // 根据天气文本选择 emoji
        const char* icon = "🌥️";
        if (d->text_day) {
            if (strstr(d->text_day, "晴")) icon = "☀️";
            else if (strstr(d->text_day, "雨")) icon = "🌧️";
            else if (strstr(d->text_day, "雪")) icon = "❄️";
            else if (strstr(d->text_day, "雷")) icon = "⛈️";
            else if (strstr(d->text_day, "雾")) icon = "🌫️";
            else if (strstr(d->text_day, "风")) icon = "🌬️";
        }

        printf("│\n");
        printf("│ %s [%s] %s/%s℃\n", icon, d->fx_date, d->temp_min ?: "--", d->temp_max ?: "--");
        printf("│ 🌞 白天: %s (风: %s %s)\n", d->text_day ?: "-", d->wind_dir_day ?: "-", d->wind_scale_day ?: "-");
        printf("│ 🌙 夜晚: %s (风: %s %s)\n", d->text_night ?: "-", d->wind_dir_night ?: "-", d->wind_scale_night ?: "-");
        printf("│ 🌡️ 湿度: %s%% | 气压: %shPa | UV: %s\n",
               d->humidity ?: "--",
               d->pressure ?: "--",
               d->uv_index ?: "--");
        printf("│ 🌤️ 日出: %s | 日落: %s\n", d->sunrise ?: "--", d->sunset ?: "--");
        printf("│ 🌙 月升: %s | 月落: %s | %s\n", d->moonrise ?: "--", d->moonset ?: "--", d->moon_phase ?: "--");
    }

    printf("╰────────────────────────────────────────────╯\n\n");
}


// 在文件顶部添加静态缓冲区定义
#define MAX_URL_LEN 256
#define MAX_RESPONSE_LEN 4096
#define MAX_LOCATION_STR 64

static char url_buffer[MAX_URL_LEN];
static char encoded_city_buffer[MAX_LOCATION_STR];


void location_info_free(location_info_t *location_info) {
    if (!location_info) return;
    
    SAFE_FREE(location_info->ip_address);
    SAFE_FREE(location_info->province);
    SAFE_FREE(location_info->city);
    SAFE_FREE(location_info->isp);
    SAFE_FREE(location_info);
}

void weather_info_free(weather_info_t *info) {
    if (!info) return;
    
    // 释放天气信息
    SAFE_FREE(info->weather);
    SAFE_FREE(info->icon);
    SAFE_FREE(info->wind_dir);
    SAFE_FREE(info->wind_scale);
    SAFE_FREE(info->update_time);
    
    // 释放位置信息
    if (info->location_info) {
        location_info_free(info->location_info);
    }
    SAFE_FREE(info);
}

void forecast_weather_free(forecast_weather_t *forecast) {
    if (!forecast) return;

    free(forecast->code);
    free(forecast->update_time);
    free(forecast->fx_link);

    for (int i = 0; i < forecast->daily_count; ++i) {
        daily_weather_t *d = &forecast->daily[i];

        free(d->fx_date);
        free(d->sunrise);
        free(d->sunset);
        free(d->moonrise);
        free(d->moonset);
        free(d->moon_phase);
        free(d->moon_phase_icon);

        free(d->temp_max);
        free(d->temp_min);
        free(d->icon_day);
        free(d->text_day);
        free(d->icon_night);
        free(d->text_night);
        free(d->wind360_day);
        free(d->wind_dir_day);
        free(d->wind_scale_day);
        free(d->wind_speed_day);
        free(d->wind360_night);
        free(d->wind_dir_night);
        free(d->wind_scale_night);
        free(d->wind_speed_night);
        free(d->humidity);
        free(d->precip);
        free(d->pressure);
        free(d->vis);
        free(d->cloud);
        free(d->uv_index);
    }

    free(forecast->daily);
    free(forecast);
}
