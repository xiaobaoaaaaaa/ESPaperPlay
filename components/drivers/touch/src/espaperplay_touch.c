/*
 * SPDX-FileCopyrightText: 2026 ESPaperPlay Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "driver/gpio.h"
#include "driver/i2c_master.h"

#include "esp_check.h"
#include "esp_log.h"

#include "espaperplay_config.h"
#include "espaperplay_touch.h"

static const char *TAG = "ESPaperPlay_TOUCH";

/* ====================================================================
 * GT911 寄存器定义（《GT911 Datasheet》Rev.10 + 厂商 STM32 demo gt9xx.c）
 * ==================================================================== */

#define GT911_REG_CONFIG_VERSION 0x8047 /*!< 配置版本（config 区第 0 字节） */
#define GT911_REG_X_OUTPUT_MAX 0x8048   /*!< X 输出最大值（小端，config 区第 1~2 字节） */
#define GT911_REG_Y_OUTPUT_MAX 0x804A   /*!< Y 输出最大值（小端，config 区第 3~4 字节） */
#define GT911_REG_TOUCH_NUMBER 0x804C   /*!< 支持触摸点数（config 区第 5 字节，低 4 位） */
#define GT911_REG_MODULE_SW1 0x804D     /*!< 模块开关 1（config 区第 6 字节，低 2 位=INT 触发模式） */
#define GT911_REG_PRODUCT_ID 0x8140     /*!< 产品 ID："911" */
#define GT911_REG_STATUS 0x814E         /*!< 缓冲状态：b7=缓冲就绪，b3:0=触摸点数 */
#define GT911_REG_POINT_BASE 0x814F     /*!< 触摸点数据区起点（每点 8 字节） */

/** 缓冲状态位定义。 */
#define GT911_STATUS_BUFFER_READY 0x80 /*!< 缓冲就绪（有新的触摸帧） */
#define GT911_STATUS_POINTS_MASK 0x0F  /*!< 触摸点数掩码 */

/**
 * 单个触摸点数据包长度（字节）：
 * [0]=跟踪 ID，[1..2]=X（小端），[3..4]=Y（小端），[5..6]=面积，[7]=保留。
 * 与厂商 demo gt910_isr() 中 point_data[1+8*i .. 7+8*i] 的布局一致。
 */
#define GT911_POINT_PACKET_LEN 8

/* ====================================================================
 * I2C 地址与复位时序（《GT911 Datasheet》Rev.10 第 6.1 节）
 *
 * GT911 在复位释放瞬间采样 INT 电平以锁存从机地址：
 *   - INT = 低电平 → 0xBA/0xBB（7 位 0x5D）；
 *   - INT = 高电平 → 0x28/0x29（7 位 0x14）。
 * 厂商 demo（gtp_reset_guitar）把 INT 拉低后释放复位并访问 0xBA/0xBB，
 * 本驱动默认沿用该时序。
 * ==================================================================== */

#define GT911_I2C_ADDR_INT_LOW 0x5D  /*!< INT 低电平复位锁存的 7 位地址 */
#define GT911_I2C_ADDR_INT_HIGH 0x14 /*!< INT 高电平复位锁存的 7 位地址 */

#define GT911_POWER_ON_DELAY_MS 5    /*!< 电源轨上电后等待（毫秒） */
#define GT911_RESET_ASSERT_MS 10     /*!< RST 与 INT 同时保持 ≥10ms 再释放复位 */
#define GT911_INT_ADDR_HOLD_MS 10    /*!< RST 释放后继续保持 INT 电平 ≥5ms 完成地址锁存 */
#define GT911_BOOT_SETTLE_MS 50      /*!< 复位结束后等待固件启动（毫秒） */
#define GT911_PROBE_RETRIES 5        /*!< 产品 ID 探测重试次数 */
#define GT911_PROBE_RETRY_DELAY_MS 20 /*!< 探测重试间隔（毫秒） */

/* ====================================================================
 * 内部状态
 * ==================================================================== */

static i2c_master_bus_handle_t s_bus = NULL; /*!< I2C 主机总线句柄 */
static i2c_master_dev_handle_t s_dev = NULL; /*!< GT911 I2C 设备句柄 */
static uint8_t s_i2c_addr = 0;               /*!< 实际锁存的 7 位 I2C 地址 */

/* 坐标换算：芯片配置中的 X/Y 输出最大值（0 表示未回读成功，按恒等映射）。 */
static uint16_t s_x_max = ESPAPERPLAY_DISPLAY_WIDTH;
static uint16_t s_y_max = ESPAPERPLAY_DISPLAY_HEIGHT;
static bool s_swap_xy = false; /*!< 竖屏配置（X_max < Y_max）时坐标轴互换 */

/* 中断驱动：ISR 仅唤醒任务（I2C 不能在 ISR 中执行）。 */
static SemaphoreHandle_t s_int_sem = NULL; /*!< GT911 INT 中断信号量 */

/* INT 触发模式（config 0x804D 低 2 位）：决定中断沿与数据就绪有效电平。 */
static bool s_int_active_low = true; /*!< true=下降沿/低电平有效，false=上升沿/高电平有效 */

/* 最新一帧缓存（供轮询式 read() 使用）。 */
static portMUX_TYPE s_frame_lock = portMUX_INITIALIZER_UNLOCKED;
static espaperplay_touch_point_t s_frame[ESPAPERPLAY_TOUCH_MAX_POINTS];
static uint8_t s_frame_count = 0;
static bool s_frame_pending = false;

static volatile espaperplay_touch_event_cb_t s_event_cb = NULL; /*!< 帧回调 */

static bool s_initialized = false;

/* ====================================================================
 * I2C 访问原语
 * ==================================================================== */

/**
 * @brief 写 GT911 寄存器（16 位大端寄存器地址 + 连续数据）。
 *
 * 对应《GT911 Datasheet》Rev.10 第 6 章写时序（b) Writing Data to GT911）：
 * S | Addr_W | Reg_H | Reg_L | Data... | E。
 */
static esp_err_t gt911_write_reg(uint16_t reg, const uint8_t *data, size_t len) {
    uint8_t buf[2 + ESPAPERPLAY_TOUCH_MAX_POINTS * GT911_POINT_PACKET_LEN];
    if (len > sizeof(buf) - 2) {
        return ESP_ERR_INVALID_ARG;
    }
    buf[0] = (uint8_t)(reg >> 8);
    buf[1] = (uint8_t)(reg & 0xFF);
    if (len > 0) {
        memcpy(&buf[2], data, len);
    }
    return i2c_master_transmit(s_dev, buf, 2 + len, ESPAPERPLAY_I2C_TIMEOUT_MS);
}

/**
 * @brief 读 GT911 寄存器（16 位大端寄存器地址 + 连续数据）。
 *
 * 严格按《GT911 Datasheet》Rev.10 第 6 章读时序（c) Reading Data from
 * GT911）分两段事务执行：
 * S | Addr_W | Reg_H | Reg_L | E，随后 S | Addr_R | Data... | NACK | E。
 */
static esp_err_t gt911_read_reg(uint16_t reg, uint8_t *data, size_t len) {
    const uint8_t reg_addr[2] = {(uint8_t)(reg >> 8), (uint8_t)(reg & 0xFF)};
    ESP_RETURN_ON_ERROR(i2c_master_transmit(s_dev, reg_addr, sizeof(reg_addr),
                                            ESPAPERPLAY_I2C_TIMEOUT_MS),
                        TAG, "write register address 0x%04X failed", reg);
    return i2c_master_receive(s_dev, data, len, ESPAPERPLAY_I2C_TIMEOUT_MS);
}

/* ====================================================================
 * 硬件复位与探测
 * ==================================================================== */

/**
 * @brief GT911 硬件复位并按 INT 电平锁存 I2C 地址。
 *
 * 时序（《GT911 Datasheet》第 6.1 节 + 厂商 demo gtp_reset_guitar）：
 *   1. INT 置为输出并驱动到待锁存电平；
 *   2. RST 拉低（进入复位）；
 *   3. 保持 RST/INT ≥10ms 后释放 RST（此时 GT911 采样 INT 锁存地址）；
 *   4. RST 释放后继续保持 INT 电平 ≥5ms，随后把 INT 释放为带上拉的输入
 *      （地址锁存后 GT911 的 INT 变为输出，驱动后续中断）；
 *   5. 等待固件启动。
 *
 * @param int_level_during_reset 复位释放瞬间 INT 的电平：0 → 锁存 0x5D，
 *                               1 → 锁存 0x14。
 */
static void gt911_hw_reset(int int_level_during_reset) {
    const gpio_config_t out_cfg = {
        .pin_bit_mask = BIT64(ESPAPERPLAY_PIN_TOUCH_INT) | BIT64(ESPAPERPLAY_PIN_TOUCH_RST),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = false,
        .pull_down_en = false,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&out_cfg);

    gpio_set_level(ESPAPERPLAY_PIN_TOUCH_INT, int_level_during_reset ? 1 : 0);
    gpio_set_level(ESPAPERPLAY_PIN_TOUCH_RST, 0);
    vTaskDelay(pdMS_TO_TICKS(GT911_RESET_ASSERT_MS));

    gpio_set_level(ESPAPERPLAY_PIN_TOUCH_RST, 1);
    vTaskDelay(pdMS_TO_TICKS(GT911_INT_ADDR_HOLD_MS));

    /* 释放 INT：输入 + 上拉，GT911 拉低 INT 表示有触摸数据。 */
    const gpio_config_t int_in_cfg = {
        .pin_bit_mask = BIT64(ESPAPERPLAY_PIN_TOUCH_INT),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = true,
        .pull_down_en = false,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&int_in_cfg);

    vTaskDelay(pdMS_TO_TICKS(GT911_BOOT_SETTLE_MS));
}

/**
 * @brief 探测 GT911：重试回读产品 ID，期望 "911"。
 */
static bool gt911_probe_product_id(void) {
    for (int i = 0; i < GT911_PROBE_RETRIES; i++) {
        uint8_t id[4] = {0};
        if (gt911_read_reg(GT911_REG_PRODUCT_ID, id, sizeof(id)) == ESP_OK &&
            memcmp(id, "911", 3) == 0) {
            return true;
        }
        vTaskDelay(pdMS_TO_TICKS(GT911_PROBE_RETRY_DELAY_MS));
    }
    return false;
}

/* ====================================================================
 * 坐标换算
 * ==================================================================== */

/**
 * @brief 把 GT911 原始坐标换算为显示像素坐标。
 *
 * 芯片配置中的 X/Y 输出最大值（0x8048~0x804B）可能与显示分辨率不同，
 * 按比例换算并裁剪到有效显示区；竖屏配置（X_max < Y_max）时互换坐标轴。
 */
static void gt911_map_coord(uint16_t raw_x, uint16_t raw_y, uint16_t *out_x, uint16_t *out_y) {
    uint32_t x = 0;
    uint32_t y = 0;

    if (s_swap_xy) {
        x = (uint32_t)raw_y * ESPAPERPLAY_DISPLAY_WIDTH / s_y_max;
        y = (uint32_t)raw_x * ESPAPERPLAY_DISPLAY_HEIGHT / s_x_max;
    } else {
        x = (uint32_t)raw_x * ESPAPERPLAY_DISPLAY_WIDTH / s_x_max;
        y = (uint32_t)raw_y * ESPAPERPLAY_DISPLAY_HEIGHT / s_y_max;
    }

    if (x >= ESPAPERPLAY_DISPLAY_WIDTH) {
        x = ESPAPERPLAY_DISPLAY_WIDTH - 1;
    }
    if (y >= ESPAPERPLAY_DISPLAY_HEIGHT) {
        y = ESPAPERPLAY_DISPLAY_HEIGHT - 1;
    }
    *out_x = (uint16_t)x;
    *out_y = (uint16_t)y;
}

/* ====================================================================
 * 触摸帧读取（任务上下文）
 * ==================================================================== */

/**
 * @brief 读取并消费一帧触摸数据。
 *
 * 流程与厂商 demo gt910_isr() 一致：
 *   1. 读缓冲状态 0x814E；
 *   2. 缓冲未就绪 → 返回 ESP_ERR_NOT_FOUND（不消费）；
 *   3. 按点数读触摸点数据（0x814F 起，每点 8 字节）并换算坐标；
 *   4. 写 0x814E = 0 清空缓冲，通知 GT911 可写入下一帧并释放 INT。
 *
 * @param[out] points 解析出的触摸点（容量 ESPAPERPLAY_TOUCH_MAX_POINTS）。
 * @param[out] count  触摸点数量；0 表示释放帧（全部手指抬起）。
 *
 * @return ESP_OK 已消费一帧；ESP_ERR_NOT_FOUND 无新数据。
 */
static esp_err_t gt911_read_frame(espaperplay_touch_point_t *points, uint8_t *count) {
    uint8_t status = 0;
    ESP_RETURN_ON_ERROR(gt911_read_reg(GT911_REG_STATUS, &status, 1), TAG,
                        "read buffer status failed");
    if ((status & GT911_STATUS_BUFFER_READY) == 0) {
        *count = 0;
        return ESP_ERR_NOT_FOUND;
    }

    uint8_t num = status & GT911_STATUS_POINTS_MASK;
    if (num > ESPAPERPLAY_TOUCH_MAX_POINTS) {
        num = ESPAPERPLAY_TOUCH_MAX_POINTS;
    }

    if (num > 0) {
        uint8_t raw[ESPAPERPLAY_TOUCH_MAX_POINTS * GT911_POINT_PACKET_LEN];
        ESP_RETURN_ON_ERROR(
            gt911_read_reg(GT911_REG_POINT_BASE, raw, num * GT911_POINT_PACKET_LEN), TAG,
            "read %u touch point(s) failed", num);

        for (uint8_t i = 0; i < num; i++) {
            const uint8_t *p = &raw[i * GT911_POINT_PACKET_LEN];
            points[i].id = p[0];
            points[i].reserved = 0;
            gt911_map_coord((uint16_t)((p[2] << 8) | p[1]), (uint16_t)((p[4] << 8) | p[3]),
                            &points[i].x, &points[i].y);
        }
    }

    /* 消费本帧：清空缓冲状态，GT911 随后释放 INT 并允许写入下一帧。 */
    const uint8_t zero = 0;
    ESP_RETURN_ON_ERROR(gt911_write_reg(GT911_REG_STATUS, &zero, 1), TAG,
                        "clear buffer status failed");

    *count = num;
    return ESP_OK;
}

/* ====================================================================
 * INT 中断与读取任务
 * ==================================================================== */

/**
 * @brief GT911 INT 引脚中断：仅唤醒读取任务。
 *
 * 触发沿由芯片配置 0x804D 低 2 位决定（0=上升沿、1=下降沿、2=低电平，
 * 见 NXP 官方 fsl_gt911 与本面板厂商配置 0x3D→下降沿）；本面板默认
 * 下降沿：有触摸数据时 INT 产生下降沿脉冲，数据未被消费前保持低电平。
 */
static void IRAM_ATTR gt911_int_isr(void *arg) {
    (void)arg;
    BaseType_t high_task_woken = pdFALSE;
    xSemaphoreGiveFromISR(s_int_sem, &high_task_woken);
    portYIELD_FROM_ISR(high_task_woken);
}

/**
 * @brief 触摸读取任务。
 *
 * 被 INT 中断唤醒后消费触摸帧：缓存最新一帧并回调（若已注册）。中断沿
 * 可能被遗漏，因此消费循环用 INT 有效电平兜底——数据未消费时 INT 保持
 * 有效电平，连续读取直到缓冲无新数据为止。
 */
static void gt911_task(void *arg) {
    (void)arg;
    ESP_LOGI(TAG, "touch reader task started");

    for (;;) {
        if (xSemaphoreTake(s_int_sem, portMAX_DELAY) != pdTRUE) {
            continue;
        }

        /* 数据未消费时 INT 保持有效电平（下降沿/低电平模式为低），
         * 用电平兜底被遗漏的中断沿。 */
        while (gpio_get_level(ESPAPERPLAY_PIN_TOUCH_INT) == (s_int_active_low ? 0 : 1)) {
            uint8_t num = 0;
            espaperplay_touch_point_t points[ESPAPERPLAY_TOUCH_MAX_POINTS];
            if (gt911_read_frame(points, &num) != ESP_OK) {
                break; /* 无新帧：缓冲已被消费，等待下一次中断 */
            }

            espaperplay_touch_event_cb_t cb = NULL;
            portENTER_CRITICAL(&s_frame_lock);
            if (num > 0) {
                memcpy(s_frame, points, num * sizeof(points[0]));
            }
            s_frame_count = num;
            s_frame_pending = true;
            cb = s_event_cb;
            portEXIT_CRITICAL(&s_frame_lock);

            if (cb != NULL) {
                cb(points, num);
            }
        }
    }
}

/* ====================================================================
 * 公共 API
 * ==================================================================== */

esp_err_t espaperplay_touch_init(void) {
    esp_err_t ret = ESP_OK; /* ESP_GOTO_ON_ERROR 宏要求 */
    if (s_initialized || s_bus != NULL) {
        ESP_LOGW(TAG, "touch already initialized");
        return ESP_ERR_INVALID_STATE;
    }

    /* 1. 触摸电源轨上电。 */
    if (ESPAPERPLAY_PIN_TOUCH_PWR >= 0) {
        const gpio_config_t pwr_cfg = {
            .pin_bit_mask = BIT64(ESPAPERPLAY_PIN_TOUCH_PWR),
            .mode = GPIO_MODE_OUTPUT,
            .pull_up_en = false,
            .pull_down_en = false,
            .intr_type = GPIO_INTR_DISABLE,
        };
        ESP_RETURN_ON_ERROR(gpio_config(&pwr_cfg), TAG, "touch power GPIO%d config failed",
                            ESPAPERPLAY_PIN_TOUCH_PWR);
        ESP_RETURN_ON_ERROR(gpio_set_level(ESPAPERPLAY_PIN_TOUCH_PWR, 1), TAG,
                            "touch power enable failed");
        vTaskDelay(pdMS_TO_TICKS(GT911_POWER_ON_DELAY_MS));
    }

    /* 2. I2C 主机（GT911 专用，见 board 组件注释）。 */
    const i2c_master_bus_config_t bus_cfg = {
        .i2c_port = ESPAPERPLAY_I2C_PORT_ID,
        .sda_io_num = ESPAPERPLAY_PIN_TOUCH_SDA,
        .scl_io_num = ESPAPERPLAY_PIN_TOUCH_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .intr_priority = 0,
        .trans_queue_depth = 0,
        .flags.enable_internal_pullup = true,
    };
    ESP_RETURN_ON_ERROR(i2c_new_master_bus(&bus_cfg, &s_bus), TAG,
                        "I2C master bus (port %d) init failed", ESPAPERPLAY_I2C_PORT_ID);

    /* 3. 复位 + 地址锁存 + 产品 ID 探测（0x5D 失败则回退 0x14）。 */
    const uint8_t addr_candidates[2] = {
        ESPAPERPLAY_GT911_I2C_ADDR,
        (ESPAPERPLAY_GT911_I2C_ADDR == GT911_I2C_ADDR_INT_LOW) ? GT911_I2C_ADDR_INT_HIGH
                                                              : GT911_I2C_ADDR_INT_LOW,
    };

    bool found = false;
    for (int attempt = 0; attempt < 2 && !found; attempt++) {
        /* INT 低电平复位 → 0x5D；INT 高电平复位 → 0x14（规格书第 6.1 节）。 */
        gt911_hw_reset(addr_candidates[attempt] == GT911_I2C_ADDR_INT_HIGH ? 1 : 0);

        const i2c_device_config_t dev_cfg = {
            .dev_addr_length = I2C_ADDR_BIT_LEN_7,
            .device_address = addr_candidates[attempt],
            .scl_speed_hz = ESPAPERPLAY_TOUCH_I2C_CLK_HZ,
        };
        ESP_GOTO_ON_ERROR(i2c_master_bus_add_device(s_bus, &dev_cfg, &s_dev), fail, TAG,
                          "add GT911 device at 0x%02X failed", addr_candidates[attempt]);

        if (gt911_probe_product_id()) {
            s_i2c_addr = addr_candidates[attempt];
            found = true;
        } else {
            ESP_LOGW(TAG, "no GT911 at address 0x%02X, trying alternate address",
                     addr_candidates[attempt]);
            i2c_master_bus_rm_device(s_dev);
            s_dev = NULL;
        }
    }

    if (!found) {
        ESP_LOGE(TAG,
                 "GT911 not found on I2C port %d (SDA=%d SCL=%d). "
                 "Check touch power / RST / INT wiring and pull-ups",
                 ESPAPERPLAY_I2C_PORT_ID, ESPAPERPLAY_PIN_TOUCH_SDA,
                 ESPAPERPLAY_PIN_TOUCH_SCL);
        ret = ESP_ERR_NOT_FOUND;
        goto fail;
    }

    /* 4. 回读配置：版本、X/Y 输出最大值、触摸点数与 INT 触发模式，
     *    用于坐标换算与中断配置。 */
    uint8_t cfg[7] = {0};
    uint8_t int_mode = 1; /* 默认下降沿（与厂商 demo 一致） */
    if (gt911_read_reg(GT911_REG_CONFIG_VERSION, cfg, sizeof(cfg)) == ESP_OK) {
        const uint16_t x_max = (uint16_t)((cfg[2] << 8) | cfg[1]);
        const uint16_t y_max = (uint16_t)((cfg[4] << 8) | cfg[3]);
        const uint8_t touch_num = cfg[5] & 0x0F;
        int_mode = cfg[6] & 0x03; /* 0x804D 低 2 位：INT 触发模式 */
        if (x_max != 0 && y_max != 0) {
            s_x_max = x_max;
            s_y_max = y_max;
        }
        /* 竖屏配置（如 X=480/Y=800）上报的坐标轴与横屏显示互换。 */
        s_swap_xy = s_x_max < s_y_max;
        ESP_LOGI(TAG,
                 "GT911 ready at 0x%02X: product ID \"911\", config v0x%02X, "
                 "resolution %ux%u, max %u touches, INT mode %u%s",
                 s_i2c_addr, cfg[0], s_x_max, s_y_max, touch_num, int_mode,
                 s_swap_xy ? ", swap XY" : "");
    } else {
        ESP_LOGW(TAG, "read config failed, assume raw coordinates match display %ux%u",
                 ESPAPERPLAY_DISPLAY_WIDTH, ESPAPERPLAY_DISPLAY_HEIGHT);
    }

    /* 5. INT 中断（触发沿随芯片配置）+ 内部读取任务。 */
    gpio_int_type_t int_trig = GPIO_INTR_NEGEDGE;
    switch (int_mode) {
    case 0: /* 上升沿触发 */
        int_trig = GPIO_INTR_POSEDGE;
        s_int_active_low = false;
        break;
    case 2: /* 低电平触发 */
        int_trig = GPIO_INTR_LOW_LEVEL;
        s_int_active_low = true;
        break;
    case 3: /* 轮询模式：INT 无事件输出，退化为下降沿 + 电平兜底 */
        ESP_LOGW(TAG, "GT911 INT polling mode (0x804D=0x%02X), assuming falling edge", cfg[6]);
        int_trig = GPIO_INTR_NEGEDGE;
        s_int_active_low = true;
        break;
    case 1: /* 下降沿触发（默认） */
    default:
        int_trig = GPIO_INTR_NEGEDGE;
        s_int_active_low = true;
        break;
    }

    s_int_sem = xSemaphoreCreateBinary();
    if (s_int_sem == NULL) {
        ESP_LOGE(TAG, "create INT semaphore failed");
        goto fail;
    }
    /* 0 表示 ISR 服务尚未安装时使用默认分配标志；若其他组件已安装，
     * 返回 ESP_ERR_INVALID_STATE，此时直接复用已有服务。 */
    const esp_err_t isr_service_ret = gpio_install_isr_service(0);
    if (isr_service_ret != ESP_OK && isr_service_ret != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "install GPIO ISR service failed: %s", esp_err_to_name(isr_service_ret));
        goto fail;
    }
    ESP_GOTO_ON_ERROR(gpio_isr_handler_add(ESPAPERPLAY_PIN_TOUCH_INT, gt911_int_isr, NULL), fail,
                      TAG, "add INT GPIO%d ISR failed", ESPAPERPLAY_PIN_TOUCH_INT);
    ESP_GOTO_ON_ERROR(gpio_set_intr_type(ESPAPERPLAY_PIN_TOUCH_INT, int_trig), fail, TAG,
                      "set INT GPIO%d trigger mode %d failed", ESPAPERPLAY_PIN_TOUCH_INT,
                      (int)int_trig);

    if (xTaskCreate(gt911_task, "gt911_touch", 3072, NULL, 5, NULL) != pdPASS) {
        ESP_LOGE(TAG, "create touch reader task failed");
        goto fail;
    }

    s_initialized = true;
    ESP_LOGI(TAG, "touch driver initialized (INT on GPIO%d, trigger mode %d)",
             ESPAPERPLAY_PIN_TOUCH_INT, (int)int_trig);
    return ESP_OK;

fail:
    gpio_isr_handler_remove(ESPAPERPLAY_PIN_TOUCH_INT);
    if (s_int_sem != NULL) {
        vSemaphoreDelete(s_int_sem);
        s_int_sem = NULL;
    }
    if (s_dev != NULL) {
        i2c_master_bus_rm_device(s_dev);
        s_dev = NULL;
    }
    if (s_bus != NULL) {
        i2c_del_master_bus(s_bus);
        s_bus = NULL;
    }
    return (ret != ESP_OK) ? ret : ESP_FAIL;
}

esp_err_t espaperplay_touch_register_event_cb(espaperplay_touch_event_cb_t cb) {
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    portENTER_CRITICAL(&s_frame_lock);
    s_event_cb = cb;
    portEXIT_CRITICAL(&s_frame_lock);
    return ESP_OK;
}

esp_err_t espaperplay_touch_read(espaperplay_touch_point_t *points, uint8_t max_points,
                                 uint8_t *count) {
    if (points == NULL || count == NULL || max_points == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    portENTER_CRITICAL(&s_frame_lock);
    if (s_frame_pending) {
        const uint8_t num = (s_frame_count < max_points) ? s_frame_count : max_points;
        memcpy(points, s_frame, num * sizeof(points[0]));
        *count = num;
        s_frame_pending = false;
    } else {
        *count = 0;
    }
    portEXIT_CRITICAL(&s_frame_lock);
    return ESP_OK;
}
