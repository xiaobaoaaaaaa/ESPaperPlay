#include <string.h>

#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"

#include "config_manager.h"

#define CONFIG_NAMESPACE "sys_config"
#define CONFIG_KEY "config"

static system_config_t s_cfg;

esp_err_t config_load(system_config_t *config) 
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(CONFIG_NAMESPACE, NVS_READONLY, &handle);
    if (err != ESP_OK) return err;

    size_t required_size = sizeof(system_config_t);
    err = nvs_get_blob(handle, CONFIG_KEY, config, &required_size);
    ESP_LOGI("CONFIG", "nvs_get_blob returned: %s", esp_err_to_name(err)); // Fixed spelling
    nvs_close(handle);
    return err;
}

const system_config_t* config_get(void) {
    return &s_cfg;
}

system_config_t* config_get_mutable(void) {
    return &s_cfg;
}

esp_err_t config_save(void) 
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(CONFIG_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) return err;

    err = nvs_set_blob(handle, CONFIG_KEY, &s_cfg, sizeof(s_cfg));
    if (err == ESP_OK) err = nvs_commit(handle);
    nvs_close(handle);
    return err;
}

esp_err_t config_reset_defaults(void) {
    memset(&s_cfg, 0, sizeof(s_cfg));
    s_cfg.power_save_enabled = true;
    s_cfg.power_save_min = 3;
    return config_save();
}

esp_err_t config_manager_init(void) {
    nvs_handle_t handle;
    esp_err_t err = nvs_open(CONFIG_NAMESPACE, NVS_READONLY, &handle);
    if (err == ESP_OK) {
        size_t size = sizeof(system_config_t);
        err = nvs_get_blob(handle, CONFIG_KEY, &s_cfg, &size);
        nvs_close(handle);
    }

    if (err != ESP_OK) {
        config_reset_defaults();
    }

    return ESP_OK;
}