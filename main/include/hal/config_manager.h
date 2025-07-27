#ifndef _CONFIG_MANAGER_H
#define _CONFIG_MANAGER_H
#include "sys_config.h"
#include "esp_err.h"

esp_err_t config_manager_init(void); // 初始化并加载配置
const system_config_t* config_get(void); // 获取只读配置
system_config_t* config_get_mutable(void); // 获取可修改配置
esp_err_t config_save(void); // 保存当前配置
esp_err_t config_reset_defaults(void);

#endif