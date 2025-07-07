#include "driver/gpio.h"

#include "touch.h"
#include "gpioX.h"

void sd_touch_init()
{
    // 触摸IC的信息结构体，方便管理触摸发生的5个点信息
    //gpiox_set_ppOutput(5, 1); // 如电容触摸屏不能正常初始化，建议加上此句，将RESET拉高。如仍不可以，则触摸屏硬件可能损坏
    //vTaskDelay(200 / portTICK_PERIOD_MS);
    // 配置I2C0-主机模式，100K，指定 SCL-32，SDA-33
    i2c_master_init(I2C_NUM_0, 100000, GPIO_NUM_32, GPIO_NUM_33);
    // FTxxxx触控芯片初始化
    i2c_ctp_FTxxxx_init(I2C_NUM_0);

    //配置芯片中断引脚，启用上拉，用于触摸状态的检测
    gpio_config_t intr_cfg = {
        .mode = GPIO_MODE_INPUT,
        .pin_bit_mask = 1ULL << GPIO_NUM_4,
        .pull_up_en = GPIO_PULLUP_ENABLE,
    };
    gpio_config(&intr_cfg);

    ctp_tp_t ctp;
    i2c_ctp_FTxxxx_read_all(I2C_NUM_0, &ctp);
}