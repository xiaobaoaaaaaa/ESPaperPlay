#include <string.h>

#include "esp_event.h"
#include "esp_log.h"
#include "esp_smartconfig.h"
#include "esp_wifi.h"
#include "nvs_flash.h"

#include "config_manager.h"
#include "vars.h"
#include "wifi_ctrl.h"

#define NVS_NAMESPACE_WIFI "wifi_config"
#define NVS_KEY_SSID "ssid"
#define NVS_KEY_PASSWORD "password"
#define MAX_WIFI_RETRY 5

EventGroupHandle_t s_wifi_event_group;

static const int CONNECTED_BIT = BIT0;
static const int ESPTOUCH_DONE_BIT = BIT1;
static const char *TAG = "smartconfig";

static int s_wifi_retry_count = 0;
static bool s_wifi_init_phase = true;
bool wifi_on_off;

void start_smartconfig(void);
static TaskHandle_t smartconfig_task_handle = NULL;

static void smartconfig_task(void * parm)
{
    EventBits_t uxBits;
    ESP_ERROR_CHECK(esp_smartconfig_set_type(SC_TYPE_ESPTOUCH));
    smartconfig_start_config_t cfg = SMARTCONFIG_START_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_smartconfig_start(&cfg));
    while (1) 
    {
        uxBits = xEventGroupWaitBits(s_wifi_event_group, 
                   CONNECTED_BIT | ESPTOUCH_DONE_BIT, 
                   true, false, portMAX_DELAY);
        if (uxBits & CONNECTED_BIT) 
        {
            ESP_LOGI(TAG, "WiFi connected to AP");
        }
        if (uxBits & ESPTOUCH_DONE_BIT) 
        {
            ESP_LOGI(TAG, "Smartconfig completed");
            esp_smartconfig_stop();
            smartconfig_task_handle = NULL;
            wifi_on_off = true;
            set_var_ui_wifi_on_off(wifi_on_off);
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
            ESP_LOGW(TAG, "Retry connecting to AP (%d/%d)", 
                     s_wifi_retry_count, MAX_WIFI_RETRY);
        } else {
            if (s_wifi_init_phase) {
                ESP_LOGE(TAG, "Connection failed after %d attempts, starting smartconfig", 
                         MAX_WIFI_RETRY);
                s_wifi_retry_count = 0;
                start_smartconfig();
            } else {
                ESP_LOGE(TAG, "Connection failed after %d attempts (non-init phase)", 
                         MAX_WIFI_RETRY);
                s_wifi_retry_count = 0;
            }
        }
        xEventGroupClearBits(s_wifi_event_group, CONNECTED_BIT);
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        s_wifi_retry_count = 0;
        s_wifi_init_phase = false;
        xEventGroupSetBits(s_wifi_event_group, CONNECTED_BIT);
        set_var_wifi_connected(true);
    } else if (event_base == SC_EVENT && event_id == SC_EVENT_SCAN_DONE) {
        ESP_LOGI(TAG, "Smartconfig scan completed");
    } else if (event_base == SC_EVENT && event_id == SC_EVENT_FOUND_CHANNEL) {
        ESP_LOGI(TAG, "Smartconfig channel found");
    } else if (event_base == SC_EVENT && event_id == SC_EVENT_GOT_SSID_PSWD) {
        smartconfig_event_got_ssid_pswd_t *evt = (smartconfig_event_got_ssid_pswd_t *)event_data;

        wifi_config_t wifi_config = {0};
        strncpy((char*)wifi_config.sta.ssid, (const char*)evt->ssid, 
                sizeof(wifi_config.sta.ssid));
        strncpy((char*)wifi_config.sta.password, (const char*)evt->password, 
                sizeof(wifi_config.sta.password));

        system_config_t *cfg = config_get_mutable();
        strncpy(cfg->wifi_ssid, (const char*)evt->ssid, sizeof(cfg->wifi_ssid));
        strncpy(cfg->wifi_password, (const char*)evt->password, sizeof(cfg->wifi_password));
        config_save();
        ESP_LOGI(TAG, "WiFi config saved");

        ESP_ERROR_CHECK(esp_wifi_disconnect());
        ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
        esp_wifi_connect();
    } else if (event_base == SC_EVENT && event_id == SC_EVENT_SEND_ACK_DONE) {
        xEventGroupSetBits(s_wifi_event_group, ESPTOUCH_DONE_BIT);
    }
}

int get_wifi_from_config()
{
    const system_config_t *cfg = config_get();
    if (strlen(cfg->wifi_ssid) == 0 || strlen(cfg->wifi_password) == 0) {
        ESP_LOGW(TAG, "SSID or password not set in config");
        return 1;
    }
    return -1;
}

static void initialise_wifi_base(void)
{
    ESP_ERROR_CHECK(esp_netif_init());
    s_wifi_event_group = xEventGroupCreate();
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_t *sta_netif = esp_netif_create_default_wifi_sta();
    assert(sta_netif);

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(SC_EVENT, ESP_EVENT_ANY_ID, &event_handler, NULL));
}

static void connect_wifi_from_config(void)
{
    const system_config_t *cfg = config_get();

    wifi_config_t wifi_config = {0};
    strncpy((char*)wifi_config.sta.ssid, cfg->wifi_ssid, sizeof(wifi_config.sta.ssid));
    strncpy((char*)wifi_config.sta.password, cfg->wifi_password, sizeof(wifi_config.sta.password));

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_ERROR_CHECK(esp_wifi_connect());
}

void start_smartconfig(void)
{
    if (smartconfig_task_handle == NULL) {
        ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
        ESP_ERROR_CHECK(esp_wifi_start());
        xTaskCreate(smartconfig_task, "smartconfig_task", 4096, NULL, 3, &smartconfig_task_handle);
        ESP_LOGI(TAG, "Smartconfig task started");
    } else {
        ESP_LOGW(TAG, "Smartconfig task already running");
    }
}

void set_wifi_on_off(bool op)
{
    if (op && !wifi_on_off) 
    {
        esp_wifi_start();
        esp_wifi_connect();
        wifi_on_off = true;
    } 
    else if(!op && wifi_on_off)
    {
        esp_wifi_disconnect();
        esp_wifi_stop();
        wifi_on_off = false;
    }
}

bool wifi_init(void)
{
    s_wifi_init_phase = true;
    wifi_on_off = true;

    initialise_wifi_base();

    int result = get_wifi_from_config();
    if (result != -1) {
        ESP_LOGW(TAG, "WiFi config missing, starting smartconfig");
        start_smartconfig();
        return false;
    }

    ESP_LOGI(TAG, "WiFi config found, connecting...");
    connect_wifi_from_config();
    vTaskDelay(pdMS_TO_TICKS(1000));
    wifi_ap_record_t ap_info;
    esp_err_t ret = esp_wifi_sta_get_ap_info(&ap_info);
    return (ret == ESP_OK);
}