#include "wifi_ctrl.h"
#include "nvs_flash.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_smartconfig.h"
#include <string.h>
#include "vars.h"

#define NVS_NAMESPACE_WIFI "wifi_config"
#define NVS_KEY_SSID "ssid"
#define NVS_KEY_PASSWORD "password"
#define MAX_WIFI_RETRY 5

EventGroupHandle_t s_wifi_event_group;

static const int CONNECTED_BIT = BIT0;
static const int ESPTOUCH_DONE_BIT = BIT1;
static const char *TAG = "smartconfig";

static int s_wifi_retry_count = 0;
static bool s_wifi_init_phase = true; // 标记是否为初始化阶段
bool wifi_manually_stopped = false;

static void start_smartconfig(void);
static TaskHandle_t smartconfig_task_handle = NULL;

static void smartconfig_task(void * parm)
{
    EventBits_t uxBits;
    ESP_ERROR_CHECK( esp_smartconfig_set_type(SC_TYPE_ESPTOUCH) );
    smartconfig_start_config_t cfg = SMARTCONFIG_START_CONFIG_DEFAULT();
    ESP_ERROR_CHECK( esp_smartconfig_start(&cfg) );
    while (1) {
        uxBits = xEventGroupWaitBits(s_wifi_event_group, CONNECTED_BIT | ESPTOUCH_DONE_BIT, true, false, portMAX_DELAY);
        if(uxBits & CONNECTED_BIT) {
            ESP_LOGI(TAG, "WiFi Connected to ap");
        }
        if(uxBits & ESPTOUCH_DONE_BIT) {
            ESP_LOGI(TAG, "smartconfig over");
            esp_smartconfig_stop();
            smartconfig_task_handle = NULL; // 任务即将结束，清空句柄
            vTaskDelete(NULL);
        }
    }
}

static void event_handler(void* arg, esp_event_base_t event_base,
                                int32_t event_id, void* event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        set_var_wifi_connected(false);
        if (s_wifi_retry_count < MAX_WIFI_RETRY) {
            esp_wifi_connect();
            s_wifi_retry_count++;
            ESP_LOGW(TAG, "Retry to connect to the AP (%d/%d)", s_wifi_retry_count, MAX_WIFI_RETRY);
        } else {
            if (s_wifi_init_phase) { // 仅初始化阶段才启动 smartconfig
                ESP_LOGE(TAG, "Failed to connect after %d attempts during init, starting smartconfig", MAX_WIFI_RETRY);
                s_wifi_retry_count = 0;
                start_smartconfig();
            } else {
                ESP_LOGE(TAG, "Failed to connect after %d attempts, but not in init phase, NOT starting smartconfig", MAX_WIFI_RETRY);
                s_wifi_retry_count = 0;
            }
        }
        xEventGroupClearBits(s_wifi_event_group, CONNECTED_BIT);
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        s_wifi_retry_count = 0; // 连接成功，重置重试计数
        s_wifi_init_phase = false; // 只要连上一次，初始化阶段结束
        xEventGroupSetBits(s_wifi_event_group, CONNECTED_BIT);
        set_var_wifi_connected(true);
    } else if (event_base == SC_EVENT && event_id == SC_EVENT_SCAN_DONE) {
        ESP_LOGI(TAG, "Scan done");
    } else if (event_base == SC_EVENT && event_id == SC_EVENT_FOUND_CHANNEL) {
        ESP_LOGI(TAG, "Found channel");
    } else if (event_base == SC_EVENT && event_id == SC_EVENT_GOT_SSID_PSWD) {
        ESP_LOGI(TAG, "Got SSID and password");

        smartconfig_event_got_ssid_pswd_t *evt = (smartconfig_event_got_ssid_pswd_t *)event_data;
        wifi_config_t wifi_config;
        uint8_t ssid[33] = { 0 };
        uint8_t password[65] = { 0 };
        uint8_t rvd_data[33] = { 0 };

        bzero(&wifi_config, sizeof(wifi_config_t));
        memcpy(wifi_config.sta.ssid, evt->ssid, sizeof(wifi_config.sta.ssid));
        memcpy(wifi_config.sta.password, evt->password, sizeof(wifi_config.sta.password));

        // 保存到NVS
        nvs_handle_t nvs_handle_wifi;
        esp_err_t err = nvs_open(NVS_NAMESPACE_WIFI, NVS_READWRITE, &nvs_handle_wifi);
        if (err == ESP_OK) {
            nvs_set_str(nvs_handle_wifi, NVS_KEY_SSID, (const char*)wifi_config.sta.ssid);
            nvs_set_str(nvs_handle_wifi, NVS_KEY_PASSWORD, (const char*)wifi_config.sta.password);
            nvs_commit(nvs_handle_wifi);
            nvs_close(nvs_handle_wifi);
            ESP_LOGI(TAG, "WiFi config saved to NVS");
        } else {
            ESP_LOGE(TAG, "Failed to open NVS for writing: %s", esp_err_to_name(err));
        }

#ifdef CONFIG_SET_MAC_ADDRESS_OF_TARGET_AP
        wifi_config.sta.bssid_set = evt->bssid_set;
        if (wifi_config.sta.bssid_set == true) {
            ESP_LOGI(TAG, "Set MAC address of target AP: "MACSTR" ", MAC2STR(evt->bssid));
            memcpy(wifi_config.sta.bssid, evt->bssid, sizeof(wifi_config.sta.bssid));
        }
#endif

        memcpy(ssid, evt->ssid, sizeof(evt->ssid));
        memcpy(password, evt->password, sizeof(evt->password));
        ESP_LOGI(TAG, "SSID:%s", ssid);
        ESP_LOGI(TAG, "PASSWORD:%s", password);
        if (evt->type == SC_TYPE_ESPTOUCH_V2) {
            ESP_ERROR_CHECK( esp_smartconfig_get_rvd_data(rvd_data, sizeof(rvd_data)) );
            ESP_LOGI(TAG, "RVD_DATA:");
            for (int i=0; i<33; i++) {
                printf("%02x ", rvd_data[i]);
            }
            printf("\n");
        }

        ESP_ERROR_CHECK( esp_wifi_disconnect() );
        ESP_ERROR_CHECK( esp_wifi_set_config(WIFI_IF_STA, &wifi_config) );
        esp_wifi_connect();
    } else if (event_base == SC_EVENT && event_id == SC_EVENT_SEND_ACK_DONE) {
        xEventGroupSetBits(s_wifi_event_group, ESPTOUCH_DONE_BIT);
    }
}

int get_wifi_from_nvs()
{
    //从 NVS 中读取 WiFi 配置
    nvs_handle_t nvs_handle_wifi;
    esp_err_t err = nvs_open(NVS_NAMESPACE_WIFI, NVS_READONLY, &nvs_handle_wifi);
    if (err != ESP_OK) {
        ESP_LOGE("wifi_ctrl", "Failed to open NVS namespace '%s': %s", NVS_NAMESPACE_WIFI, esp_err_to_name(err));
        return 0;
    }
    char ssid[32];
    size_t ssid_len = sizeof(ssid);
    err = nvs_get_str(nvs_handle_wifi, NVS_KEY_SSID, ssid, &ssid_len);
    if (err != ESP_OK) {
        if (err == ESP_ERR_NVS_NOT_FOUND) {
            ESP_LOGW("wifi_ctrl", "SSID not found in NVS, please set it first.");
            nvs_close(nvs_handle_wifi);
            return 1; // SSID not found, return 1 to indicate this
        } else {
            ESP_LOGE("wifi_ctrl", "Failed to read SSID from NVS: %s", esp_err_to_name(err));
            nvs_close(nvs_handle_wifi);
            return 0;
        }
    }
    char password[64];
    size_t password_len = sizeof(password);
    err = nvs_get_str(nvs_handle_wifi, NVS_KEY_PASSWORD, password, &password_len);
    if (err != ESP_OK) {
        if (err == ESP_ERR_NVS_NOT_FOUND) {
            ESP_LOGW("wifi_ctrl", "Password not found in NVS, please set it first.");
            nvs_close(nvs_handle_wifi);
            return 1; // Password not found, return 1 to indicate this
        } else {
            ESP_LOGE("wifi_ctrl", "Failed to read password from NVS: %s", esp_err_to_name(err));
            nvs_close(nvs_handle_wifi);
            return 0;
        }
    }
    nvs_close(nvs_handle_wifi);
    return -1; // WiFi config found and read successfully
}

static void initialise_wifi_base(void)
{
    ESP_ERROR_CHECK(esp_netif_init());
    s_wifi_event_group = xEventGroupCreate();
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_t *sta_netif = esp_netif_create_default_wifi_sta();
    assert(sta_netif);

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK( esp_wifi_init(&cfg) );

    ESP_ERROR_CHECK( esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &event_handler, NULL) );
    ESP_ERROR_CHECK( esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &event_handler, NULL) );
    ESP_ERROR_CHECK( esp_event_handler_register(SC_EVENT, ESP_EVENT_ANY_ID, &event_handler, NULL) );
}

static void connect_wifi_from_nvs(void)
{
    // 从 NVS 读取配置并连接
    nvs_handle_t nvs_handle_wifi;
    esp_err_t err = nvs_open(NVS_NAMESPACE_WIFI, NVS_READONLY, &nvs_handle_wifi);
    if (err != ESP_OK) {
        ESP_LOGE("wifi_ctrl", "Failed to open NVS namespace for connect: %s", esp_err_to_name(err));
        return;
    }
    wifi_config_t wifi_config = {0};
    size_t ssid_len = sizeof(wifi_config.sta.ssid);
    size_t pwd_len = sizeof(wifi_config.sta.password);
    nvs_get_str(nvs_handle_wifi, NVS_KEY_SSID, (char*)wifi_config.sta.ssid, &ssid_len);
    nvs_get_str(nvs_handle_wifi, NVS_KEY_PASSWORD, (char*)wifi_config.sta.password, &pwd_len);
    nvs_close(nvs_handle_wifi);

    ESP_ERROR_CHECK( esp_wifi_set_mode(WIFI_MODE_STA) );
    ESP_ERROR_CHECK( esp_wifi_set_config(WIFI_IF_STA, &wifi_config) );
    ESP_ERROR_CHECK( esp_wifi_start() );
    ESP_ERROR_CHECK( esp_wifi_connect() );
}

static void start_smartconfig(void)
{
    if (smartconfig_task_handle == NULL) { // 只允许一个任务
        ESP_ERROR_CHECK( esp_wifi_set_mode(WIFI_MODE_STA) );
        ESP_ERROR_CHECK( esp_wifi_start() );
        xTaskCreate(smartconfig_task, "smartconfig_task", 4096, NULL, 3, &smartconfig_task_handle);
        ESP_LOGI(TAG, "smartconfig task started");
    } else {
        ESP_LOGW(TAG, "smartconfig task already running");
    }
}

bool wifi_init(void)
{
    s_wifi_init_phase = true; // 初始化阶段开始
    initialise_wifi_base();
    // 尝试从 NVS 中获取 WiFi 配置
    int result = get_wifi_from_nvs();
    if (result == 0) {
        ESP_LOGE("wifi_ctrl", "Failed to retrieve WiFi configuration from NVS, start smartconfig.");
        start_smartconfig();
        return false;
    } else if (result == 1) {
        ESP_LOGW("wifi_ctrl", "WiFi configuration not found in NVS, start smartconfig.");
        start_smartconfig();
        return false;
    }
    ESP_LOGI("wifi_ctrl", "WiFi configuration retrieved successfully from NVS, connecting...");
    connect_wifi_from_nvs();
    return true;
}