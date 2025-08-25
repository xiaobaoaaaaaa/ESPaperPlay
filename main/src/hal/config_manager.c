
/**
 * @file config_manager.c
 * @brief 系统配置管理模块，负责配置的加载、保存、重置等操作。
 */

#include <string.h>

#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"

#include "config_manager.h"

#define CONFIG_NAMESPACE "sys_config"
#define CONFIG_KEY "config"


/**
 * @brief 全局系统配置结构体实例
 */
static system_config_t s_cfg;


/**
 * @brief 从 NVS 加载配置到指定结构体
 * @param config 指向 system_config_t 的指针，用于存放加载结果
 * @return ESP_OK 表示成功，否则为错误码
 */
esp_err_t config_load(system_config_t *config) 
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(CONFIG_NAMESPACE, NVS_READONLY, &handle);
    if (err != ESP_OK) return err;

    size_t required_size = sizeof(system_config_t);
    err = nvs_get_blob(handle, CONFIG_KEY, config, &required_size);
    ESP_LOGI("CONFIG", "nvs_get_blob returned: %s", esp_err_to_name(err));
    nvs_close(handle);
    return err;
}


/**
 * @brief 获取当前配置的只读指针
 * @return 指向全局配置的 const 指针
 */
const system_config_t* config_get(void) 
{
    return &s_cfg;
}


/**
 * @brief 获取当前配置的可变指针（可用于修改配置）
 * @return 指向全局配置的指针
 */
system_config_t* config_get_mutable(void) 
{
    return &s_cfg;
}


/**
 * @brief 保存当前配置到 NVS
 * @return ESP_OK 表示成功，否则为错误码
 */
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


/**
 * @brief 重置配置为默认值，并保存到 NVS
 * @return ESP_OK 表示成功，否则为错误码
 */
esp_err_t config_reset_defaults(void) 
{
    memset(&s_cfg, 0, sizeof(s_cfg)); // 清零所有配置项
    s_cfg.power_save_enabled = true;  // 默认启用省电
    s_cfg.power_save_min = 3;         // 默认省电最小值
    s_cfg.max_partial_refresh_count = 30; // 默认最大局部刷新次数
    return config_save();
}



/**
 * @brief 初始化配置管理器，尝试从 NVS 加载配置，失败则重置为默认值
 * @return ESP_OK 总是返回成功（配置已初始化）
 */
esp_err_t config_manager_init(void) 
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(CONFIG_NAMESPACE, NVS_READONLY, &handle);
    if (err == ESP_OK) {
        size_t size = sizeof(system_config_t);
        err = nvs_get_blob(handle, CONFIG_KEY, &s_cfg, &size);
        nvs_close(handle);
    }

    if (err != ESP_OK) {
        // 如果加载失败，则重置为默认配置
        config_reset_defaults();
    }

    return ESP_OK;
}