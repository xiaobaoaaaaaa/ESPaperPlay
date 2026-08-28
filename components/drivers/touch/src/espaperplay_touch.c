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
#include "esp_timer.h"

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
#define GT911_REG_CMD 0x8040            /*!< 命令寄存器：写 1 = 软复位（芯片自动清零） */
#define GT911_REG_PRODUCT_ID 0x8140     /*!< 产品 ID："911" */
#define GT911_REG_STATUS 0x814E         /*!< 缓冲状态：b7=缓冲就绪，b3:0=触摸点数 */
#define GT911_REG_POINT_BASE 0x814F     /*!< 触摸点数据区起点（每点 8 字节） */

/** GT911 配置区（编程指南）：0x8047..0x80FE 共 184 字节数据。 */
#define GT911_CONFIG_DATA_LEN 184
#define GT911_REG_CONFIG_CHECKSUM 0x80FF /*!< 配置校验和（0x8047 + 184） */
#define GT911_REG_CONFIG_REFRESH 0x8100  /*!< 配置刷新标志（写 1 使新配置生效，芯片处理后清零） */
#define GT911_CONFIG_FRESH_WAIT_MS 100   /*!< 写刷新标志后等待芯片处理的时间 */

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

/* 复位时序（GT911 Datasheet 第 6.1 节 + GT911 调试经验值）：
 *   - RST 低电平保持 ≥12ms（规格书要求 ≥10ms）；
 *   - RST 释放后 INT 保持地址选择电平 ≥55ms 再转为输入——时序不足时
 *     GT911 会应答 I2C 但内部状态机不完成上电，触摸数据恒为 0，可能
 *     数分钟后才自行恢复（实测 4 分钟）。 */
#define GT911_POWER_ON_DELAY_MS 5    /*!< 电源轨上电后等待（毫秒） */
#define GT911_RESET_ASSERT_MS 20     /*!< RST 低电平保持时间（≥12ms） */
#define GT911_INT_ADDR_HOLD_MS 60    /*!< RST 释放后 INT 保持地址电平时间（≥55ms） */
#define GT911_BOOT_SETTLE_MS 100     /*!< 复位结束后等待固件启动（毫秒） */
#define GT911_PROBE_RETRIES 10       /*!< 产品 ID 探测重试次数 */
#define GT911_PROBE_RETRY_DELAY_MS 50 /*!< 探测重试间隔（毫秒） */

/** INT 轮询模式（0x804D 低 2 位 == 3）下任务的读取周期（毫秒）。 */
#define GT911_POLL_MODE_PERIOD_MS 20

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

/* 读取任务等待方式：0 = 无限等待 INT 中断；非 0 = INT 轮询模式下的读取周期（毫秒）。 */
static uint32_t s_task_wait_ms = 0;

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
    /* 最大写长度：186 字节配置表（184 数据 + 校验和 + 刷新标志）。 */
    uint8_t buf[2 + GT911_CONFIG_DATA_LEN + 2];
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
 * 时序（《GT911 Datasheet》第 6.1 节 + 厂商 demo gtp_reset_guitar +
 * GT911 调试经验值）：
 *   1. INT 置为输出并驱动到待锁存电平；
 *   2. RST 拉低（进入复位），保持 ≥12ms；
 *   3. 释放 RST（此时 GT911 采样 INT 锁存地址）；
 *   4. RST 释放后继续保持 INT 电平 ≥55ms（关键：过早释放 INT 会导致
 *      GT911 上电时序不完整、扫描固件迟迟不启动）；
 *   5. 把 INT 释放为带上拉的输入（地址锁存后 GT911 的 INT 变为输出，
 *      驱动后续中断），等待固件启动。
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
 *
 * 每次失败都打印原因（I2C 错误码 / 产品 ID 原始字节），便于排查接线、
 * 上电时序与地址锁存问题。
 */
static bool gt911_probe_product_id(void) {
    for (int i = 0; i < GT911_PROBE_RETRIES; i++) {
        uint8_t id[4] = {0};
        const esp_err_t err = gt911_read_reg(GT911_REG_PRODUCT_ID, id, sizeof(id));
        if (err == ESP_OK) {
            if (memcmp(id, "911", 3) == 0) {
                ESP_LOGI(TAG, "probe OK on retry %d: product ID \"%c%c%c\"", i + 1, id[0], id[1],
                         id[2]);
                return true;
            }
            ESP_LOGW(TAG, "probe retry %d/%d: product ID mismatch, got %02X %02X %02X %02X "
                         "(expect \"911\")",
                     i + 1, GT911_PROBE_RETRIES, id[0], id[1], id[2], id[3]);
        } else {
            ESP_LOGW(TAG, "probe retry %d/%d: read 0x8140 failed (%s)", i + 1, GT911_PROBE_RETRIES,
                     esp_err_to_name(err));
        }
        vTaskDelay(pdMS_TO_TICKS(GT911_PROBE_RETRY_DELAY_MS));
    }
    return false;
}

/**
 * @brief 扫描 I2C 总线（7 位地址 0x03..0x77），列出所有应答的从机。
 *
 * 排查用：若 GT911 探测失败，通过扫描结果可以区分「总线上完全无设备」
 * （接线/电源问题）与「有设备但地址不符」（地址锁存/复位时序问题）。
 */
static void gt911_bus_scan(void) {
    int found = 0;
    for (uint16_t addr = 0x03; addr <= 0x77; addr++) {
        if (i2c_master_probe(s_bus, addr, 50) == ESP_OK) {
            ESP_LOGI(TAG, "bus scan: device ACK at 0x%02X", (unsigned int)addr);
            found++;
        }
    }
    if (found == 0) {
        ESP_LOGW(TAG, "bus scan: no device answered (check SDA/SCL wiring, pull-ups, power)");
    } else {
        ESP_LOGI(TAG, "bus scan done: %d device(s) found", found);
    }
}

/* ====================================================================
 * 面板配置（GT911 编程指南：复位后配置区为空/版本不符时主机必须写入）
 *
 * 本表取自厂商 demo（S-GDEY075T7-FP-GT911Touch20230713 的 gt9xx.h
 * CTP_CFG_GROUP1）前 184 字节，为该 7.5 寸 800x480 面板的横屏配置：
 *   [0] 版本 0x00；[1..2] X 输出最大值 0x0320=800；[3..4] Y 输出最大值
 *   0x01E0=480；[5] 触摸点数 5；[6] 模块开关 1=0x3D（INT 下降沿触发）。
 * 校验和与刷新标志（共 2 字节）在写入时由驱动计算。
 * ==================================================================== */

static const uint8_t GT911_CONFIG_TABLE[GT911_CONFIG_DATA_LEN] = {
    0x00, 0x20, 0x03, 0xE0, 0x01, 0x05, 0x3D, 0x00, 0x01, 0x3F, 0x23, 0x0F, 0x55, 0x37, 0x03, 0x05,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x17, 0x1A, 0x1D, 0x14, 0x89, 0x29, 0x0A, 0x61, 0x5F,
    0xB2, 0x04, 0x00, 0x00, 0x00, 0x00, 0x02, 0x1D, 0x00, 0x03, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x53, 0x85, 0x94, 0xC5, 0x02, 0x08, 0x00, 0x00, 0x04, 0x8B, 0x57, 0x00, 0x7F,
    0x5F, 0x00, 0x74, 0x69, 0x00, 0x6B, 0x73, 0x00, 0x62, 0x7F, 0x00, 0x62, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x1A, 0x18, 0x16, 0x14, 0x12, 0x10, 0x0E, 0x0C, 0x0A, 0x08, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x2A, 0x29,
    0x28, 0x24, 0x22, 0x20, 0x1F, 0x1E, 0x1D, 0x0E, 0x0C, 0x0A, 0x08, 0x06, 0x05, 0x04, 0x02, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
};

/**
 * @brief 写入面板配置表并触发刷新（GT911 编程指南 4.3 / 4.4 节）。
 *
 * 流程（注意：按编程指南不需要也不应该发送 0x8040 软复位，软复位会
 * 打断芯片对 Config_Fresh 标志的处理）：
 *   1. 写 184 字节配置到 0x8047 + 校验和到 0x80FF + 刷新标志 0x01 到 0x8100；
 *   2. 等待芯片消费刷新标志（消费后 0x8100 自动清零）；
 *   3. 若标志未被消费，重写 0x8100 = 1 再等（最多 3 次）。
 *
 * @return 成功返回 ESP_OK，否则返回错误码。
 */
static esp_err_t gt911_write_config(void) {
    uint8_t cfg[GT911_CONFIG_DATA_LEN + 2];
    uint8_t sum = 0;

    memcpy(cfg, GT911_CONFIG_TABLE, GT911_CONFIG_DATA_LEN);
    for (int i = 0; i < GT911_CONFIG_DATA_LEN; i++) {
        sum += cfg[i];
    }
    cfg[GT911_CONFIG_DATA_LEN] = (uint8_t)(0 - sum); /* 校验和 = (~sum) + 1 */
    cfg[GT911_CONFIG_DATA_LEN + 1] = 0x01;           /* 刷新标志 */

    ESP_RETURN_ON_ERROR(
        gt911_write_reg(GT911_REG_CONFIG_VERSION, cfg, sizeof(cfg)), TAG,
        "write config table (%u bytes) failed", (unsigned int)sizeof(cfg));
    ESP_LOGI(TAG, "  config written: %u bytes @0x8047, checksum 0x%02X, refresh 0x01",
             (unsigned int)sizeof(cfg), cfg[GT911_CONFIG_DATA_LEN]);

    /* 等待芯片消费刷新标志；未消费则重写标志重试（配置与校验和已在
     * 芯片 RAM 中，无需重写整表）。0x8100 是否清零仅作日志参考，不
     * 作为失败依据——部分芯片不主动清零。 */
    const uint8_t fresh_cmd = 0x01;
    for (int attempt = 0; attempt < 3; attempt++) {
        vTaskDelay(pdMS_TO_TICKS(GT911_CONFIG_FRESH_WAIT_MS));
        uint8_t fresh = 0xFF;
        if (gt911_read_reg(GT911_REG_CONFIG_REFRESH, &fresh, 1) == ESP_OK && fresh == 0x00) {
            ESP_LOGI(TAG, "  config refresh consumed by GT911 (0x8100 cleared, attempt %d)",
                     attempt + 1);
            return ESP_OK;
        }
        ESP_LOGW(TAG, "  config refresh flag still set (0x8100=0x%02X), retry %d/3", fresh,
                 attempt + 1);
        ESP_RETURN_ON_ERROR(gt911_write_reg(GT911_REG_CONFIG_REFRESH, &fresh_cmd, 1), TAG,
                            "rewrite config refresh flag failed");
    }

    ESP_LOGW(TAG, "  GT911 did not clear config refresh flag (may still work later)");
    return ESP_OK;
}

/**
 * @brief 回读并解析 GT911 配置区（0x8047..0x804D）。
 *
 * @param[out] version   配置版本。
 * @param[out] x_max     X 输出最大值。
 * @param[out] y_max     Y 输出最大值。
 * @param[out] touch_num 支持触摸点数（低 4 位）。
 * @param[out] int_mode  INT 触发模式（0x804D 低 2 位）。
 *
 * @return 配置有效（分辨率与触摸点数均非 0）返回 true，否则返回 false。
 */
static bool gt911_read_config(uint8_t *version, uint16_t *x_max, uint16_t *y_max,
                              uint8_t *touch_num, uint8_t *int_mode) {
    uint8_t cfg[7] = {0};
    if (gt911_read_reg(GT911_REG_CONFIG_VERSION, cfg, sizeof(cfg)) != ESP_OK) {
        ESP_LOGW(TAG, "  read config 0x8047..0x804D failed");
        return false;
    }

    *version = cfg[0];
    *x_max = (uint16_t)((cfg[2] << 8) | cfg[1]);
    *y_max = (uint16_t)((cfg[4] << 8) | cfg[3]);
    *touch_num = cfg[5] & 0x0F;
    *int_mode = cfg[6] & 0x03;
    ESP_LOGI(TAG, "  raw config 0x8047..0x804D: %02X %02X %02X %02X %02X %02X %02X "
                  "(v0x%02X, %ux%u, %u touches, INT mode %u)",
             cfg[0], cfg[1], cfg[2], cfg[3], cfg[4], cfg[5], cfg[6], *version, *x_max, *y_max,
             *touch_num, *int_mode);

    return (*x_max != 0 && *y_max != 0 && *touch_num != 0);
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
 * @brief 投递一帧：更新缓存并回调（若已注册）。
 */
static void gt911_deliver_frame(const espaperplay_touch_point_t *points, uint8_t num) {
    static bool s_first_frame_logged = false;
    static uint32_t s_frame_seq = 0;
    const uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000);

    s_frame_seq++;
    espaperplay_touch_event_cb_t cb = NULL;
    portENTER_CRITICAL(&s_frame_lock);
    if (num > 0) {
        memcpy(s_frame, points, num * sizeof(points[0]));
    }
    s_frame_count = num;
    s_frame_pending = true;
    cb = s_event_cb;
    portEXIT_CRITICAL(&s_frame_lock);

    if (!s_first_frame_logged) {
        ESP_LOGI(TAG, "first touch frame received: %u point(s) (reader path OK)", num);
        s_first_frame_logged = true;
    }
    if (num == 0) {
        ESP_LOGD(TAG, "GT911 deliver: seq=%u release frame @%u ms cb=%p", (unsigned)s_frame_seq,
                 (unsigned)now_ms, (void *)cb);
    } else {
        ESP_LOGD(TAG, "GT911 deliver: seq=%u %u point(s) @%u ms p0=(%u,%u) id=%u cb=%p",
                 (unsigned)s_frame_seq, num, (unsigned)now_ms, points[0].x, points[0].y,
                 points[0].id, (void *)cb);
        for (uint8_t i = 1; i < num; i++) {
            ESP_LOGD(TAG, "GT911 deliver: seq=%u point[%u]=(%u,%u) id=%u", (unsigned)s_frame_seq,
                     i, points[i].x, points[i].y, points[i].id);
        }
    }

    if (cb != NULL) {
        const uint32_t cb_start_ms = (uint32_t)(esp_timer_get_time() / 1000);
        cb(points, num);
        const uint32_t cb_cost_ms = (uint32_t)(esp_timer_get_time() / 1000) - cb_start_ms;
        ESP_LOGD(TAG, "GT911 deliver: seq=%u cb done cost %u ms", (unsigned)s_frame_seq,
                 (unsigned)cb_cost_ms);
        if (cb_cost_ms > 20) {
            ESP_LOGW(TAG, "GT911 deliver: seq=%u cb cost %u ms — input 队列可能堆积",
                     (unsigned)s_frame_seq, (unsigned)cb_cost_ms);
        }
    } else {
        ESP_LOGW(TAG, "GT911 deliver: seq=%u no cb registered — 帧未投递到 input 队列",
                 (unsigned)s_frame_seq);
    }
}

/**
 * @brief 触摸读取任务。
 *
 * 中断模式：空闲时纯阻塞在信号量上（零开销），被 INT 中断唤醒后消费
 * 触摸帧，缓存最新一帧并回调（若已注册）。中断沿可能被遗漏，因此消费
 * 循环用 INT 有效电平兜底——数据未消费时 INT 保持有效电平，连续读取
 * 直到缓冲无新数据为止。
 *
 * 轮询模式（芯片配置 0x804D 低 2 位 == 3，INT 不产生事件）：按
 * GT911_POLL_MODE_PERIOD_MS 周期直接读状态寄存器，每次唤醒消费一帧。
 */
static void gt911_task(void *arg) {
    (void)arg;
    static bool s_spurious_wake_logged = false;
    static uint32_t s_wake_count = 0;
    static uint32_t s_frame_count = 0;

    ESP_LOGI(TAG, "touch reader task started (%s)", s_task_wait_ms == 0 ? "interrupt" : "poll");

    for (;;) {
        const TickType_t wait_ticks =
            (s_task_wait_ms == 0) ? portMAX_DELAY : pdMS_TO_TICKS(s_task_wait_ms);
        const uint32_t wait_start_ms = (uint32_t)(esp_timer_get_time() / 1000);
        const bool wake_by_int = (xSemaphoreTake(s_int_sem, wait_ticks) == pdTRUE);
        const uint32_t wait_cost_ms = (uint32_t)(esp_timer_get_time() / 1000) - wait_start_ms;

        /* 轮询模式（芯片 INT mode 3）：每个周期尝试读一帧，不依赖 INT 电平。 */
        if (s_task_wait_ms != 0) {
            uint8_t num = 0;
            espaperplay_touch_point_t points[ESPAPERPLAY_TOUCH_MAX_POINTS];
            const esp_err_t read_ret = gt911_read_frame(points, &num);
            if (read_ret == ESP_OK) {
                s_frame_count++;
                ESP_LOGD(TAG, "GT911 poll: frame #%u %u point(s) wait_cost=%u ms",
                         (unsigned)s_frame_count, num, (unsigned)wait_cost_ms);
                gt911_deliver_frame(points, num);
            } else if (read_ret != ESP_ERR_NOT_FOUND) {
                ESP_LOGW(TAG, "GT911 poll: read_frame failed: %s wait_cost=%u ms",
                         esp_err_to_name(read_ret), (unsigned)wait_cost_ms);
            } else {
                ESP_LOGD(TAG, "GT911 poll: no data wait_cost=%u ms", (unsigned)wait_cost_ms);
            }
            continue;
        }

        if (!wake_by_int) {
            continue;
        }

        s_wake_count++;
        const int int_level = gpio_get_level(ESPAPERPLAY_PIN_TOUCH_INT);
        ESP_LOGD(TAG, "GT911 wake #%u: INT=%d active_low=%d wait_cost=%u ms", (unsigned)s_wake_count,
                 int_level, (int)s_int_active_low, (unsigned)wait_cost_ms);

        /* 中断模式：数据未消费时 INT 保持有效电平，用电平兜底被遗漏的中断沿。 */
        uint32_t frames_in_wake = 0;
        while (gpio_get_level(ESPAPERPLAY_PIN_TOUCH_INT) == (s_int_active_low ? 0 : 1)) {
            uint8_t num = 0;
            espaperplay_touch_point_t points[ESPAPERPLAY_TOUCH_MAX_POINTS];
            const uint32_t read_start_ms = (uint32_t)(esp_timer_get_time() / 1000);
            const esp_err_t read_ret = gt911_read_frame(points, &num);
            const uint32_t read_cost_ms = (uint32_t)(esp_timer_get_time() / 1000) - read_start_ms;
            if (read_ret != ESP_OK) {
                if (read_ret != ESP_ERR_NOT_FOUND) {
                    ESP_LOGW(TAG, "GT911 read_frame failed: %s cost=%u ms INT=%d",
                             esp_err_to_name(read_ret), (unsigned)read_cost_ms,
                             gpio_get_level(ESPAPERPLAY_PIN_TOUCH_INT));
                } else if (!s_spurious_wake_logged) {
                    ESP_LOGW(TAG,
                             "spurious INT wake: pin level %d but no data in buffer "
                             "(trigger %s)",
                             gpio_get_level(ESPAPERPLAY_PIN_TOUCH_INT),
                             s_int_active_low ? "falling/low" : "rising/high");
                    s_spurious_wake_logged = true;
                } else {
                    ESP_LOGD(TAG, "GT911 spurious wake #%u: no data INT=%d cost=%u ms",
                             (unsigned)s_wake_count, gpio_get_level(ESPAPERPLAY_PIN_TOUCH_INT),
                             (unsigned)read_cost_ms);
                }
                break; /* 无新帧：缓冲已被消费，等待下一次中断 */
            }
            s_frame_count++;
            frames_in_wake++;
            ESP_LOGD(TAG, "GT911 wake #%u frame %u: %u point(s) read_cost=%u ms total_frames=%u",
                     (unsigned)s_wake_count, (unsigned)frames_in_wake, num, (unsigned)read_cost_ms,
                     (unsigned)s_frame_count);
            gt911_deliver_frame(points, num);
        }
        if (frames_in_wake == 0) {
            ESP_LOGD(TAG, "GT911 wake #%u: 0 frames consumed INT=%d", (unsigned)s_wake_count,
                     gpio_get_level(ESPAPERPLAY_PIN_TOUCH_INT));
        } else {
            ESP_LOGD(TAG, "GT911 wake #%u done: %u frame(s) consumed", (unsigned)s_wake_count,
                     (unsigned)frames_in_wake);
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

    ESP_LOGI(TAG, "[init] start: SDA=%d SCL=%d INT=%d RST=%d PWR=%d, addr candidate 0x%02X",
             ESPAPERPLAY_PIN_TOUCH_SDA, ESPAPERPLAY_PIN_TOUCH_SCL, ESPAPERPLAY_PIN_TOUCH_INT,
             ESPAPERPLAY_PIN_TOUCH_RST, ESPAPERPLAY_PIN_TOUCH_PWR, ESPAPERPLAY_GT911_I2C_ADDR);

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
        ESP_LOGI(TAG, "[init] step 1/5 OK: touch power GPIO%d driven high",
                 ESPAPERPLAY_PIN_TOUCH_PWR);
    } else {
        ESP_LOGI(TAG, "[init] step 1/5: no touch power GPIO (always on assumed)");
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
    ESP_LOGI(TAG, "[init] step 2/5 OK: I2C master port %d @ %u Hz (internal pull-ups on)",
             ESPAPERPLAY_I2C_PORT_ID, (unsigned int)ESPAPERPLAY_TOUCH_I2C_CLK_HZ);

    /* 3. 复位 + 地址锁存 + 产品 ID 探测（0x5D 失败则回退 0x14）。 */
    const uint8_t addr_candidates[2] = {
        ESPAPERPLAY_GT911_I2C_ADDR,
        (ESPAPERPLAY_GT911_I2C_ADDR == GT911_I2C_ADDR_INT_LOW) ? GT911_I2C_ADDR_INT_HIGH
                                                              : GT911_I2C_ADDR_INT_LOW,
    };

    bool found = false;
    for (int attempt = 0; attempt < 2 && !found; attempt++) {
        /* INT 低电平复位 → 0x5D；INT 高电平复位 → 0x14（规格书第 6.1 节）。 */
        const int int_level = addr_candidates[attempt] == GT911_I2C_ADDR_INT_HIGH ? 1 : 0;
        ESP_LOGI(TAG, "[init] step 3/5 attempt %d/2: reset with INT held %s -> try addr 0x%02X",
                 attempt + 1, int_level ? "HIGH" : "LOW", addr_candidates[attempt]);
        gt911_hw_reset(int_level);
        ESP_LOGI(TAG, "  after reset: INT level = %d (expect 1 when idle)",
                 gpio_get_level(ESPAPERPLAY_PIN_TOUCH_INT));

        /* 排查用：列出当前总线上所有应答的从机。 */
        gt911_bus_scan();

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

    ESP_LOGI(TAG, "[init] step 3/5 OK: GT911 found at 0x%02X", s_i2c_addr);

    /* 4. 回读配置：版本、X/Y 输出最大值、触摸点数与 INT 触发模式，
     *    用于坐标换算与中断配置。
     *    若配置区为空/无效（复位后常见，芯片无法扫描报点），按 GT911
     *    编程指南写入厂商面板配置表并软复位，然后回读验证。 */
    uint8_t cfg_version = 0;
    uint16_t x_max = 0;
    uint16_t y_max = 0;
    uint8_t touch_num = 0;
    uint8_t int_mode = 1; /* 默认下降沿（与厂商 demo 一致） */

    if (!gt911_read_config(&cfg_version, &x_max, &y_max, &touch_num, &int_mode)) {
        ESP_LOGW(TAG,
                 "config area empty/invalid -> writing vendor panel config "
                 "(800x480, 5 touches, INT falling)");
        if (gt911_write_config() != ESP_OK) {
            ESP_LOGE(TAG, "write vendor panel config failed");
        } else if (!gt911_read_config(&cfg_version, &x_max, &y_max, &touch_num, &int_mode)) {
            ESP_LOGE(TAG, "config still empty after update - touch may not work");
        }
    }

    if (x_max != 0 && y_max != 0) {
        s_x_max = x_max;
        s_y_max = y_max;
    }
    if (x_max == 0 || y_max == 0 || touch_num == 0) {
        /* 配置仍不可用：按厂商行为兜底——下降沿 INT + 恒等坐标。 */
        ESP_LOGW(TAG, "config still unavailable, forcing INT mode 1 (falling edge)");
        int_mode = 1;
    }
    /* 竖屏配置（如 X=480/Y=800）上报的坐标轴与横屏显示互换。 */
    s_swap_xy = s_x_max < s_y_max;
    ESP_LOGI(TAG,
             "[init] step 4/5 OK: GT911 at 0x%02X, config v0x%02X, "
             "resolution %ux%u, max %u touches, INT mode %u%s",
             s_i2c_addr, cfg_version, s_x_max, s_y_max, touch_num, int_mode,
             s_swap_xy ? ", swap XY" : "");

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
    case 3: /* 轮询模式：INT 无事件输出，任务按固定周期读状态寄存器 */
        ESP_LOGW(TAG, "GT911 INT polling mode: task polls every %u ms", GT911_POLL_MODE_PERIOD_MS);
        int_trig = GPIO_INTR_NEGEDGE;
        s_int_active_low = true;
        s_task_wait_ms = GT911_POLL_MODE_PERIOD_MS;
        break;
    case 1: /* 下降沿触发（默认） */
    default:
        int_trig = GPIO_INTR_NEGEDGE;
        s_int_active_low = true;
        break;
    }

    ESP_LOGI(TAG, "[init] step 5/5: INT mode %u -> GPIO trigger %d, active %s", int_mode,
             (int)int_trig, s_int_active_low ? "low" : "high");
    ESP_LOGI(TAG, "  INT level before ISR install: %d",
             gpio_get_level(ESPAPERPLAY_PIN_TOUCH_INT));

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

    /* 排查用：回读当前缓冲状态（空闲应为 0x00；若为 0x80|n 说明芯片有
     * 未消费的触摸数据——可能是幽灵触摸或复位后残留）。只读不清。 */
    uint8_t status = 0;
    const esp_err_t status_ret = gt911_read_reg(GT911_REG_STATUS, &status, 1);
    if (status_ret == ESP_OK) {
        ESP_LOGI(TAG, "  buffer status 0x814E after init: 0x%02X%s", status,
                 (status & GT911_STATUS_BUFFER_READY) ? " (data pending!)" : " (idle)");
        if (status & GT911_STATUS_BUFFER_READY) {
            /* 有启动残留数据：踢一次任务消费（此时 INT 沿可能已错过）。 */
            xSemaphoreGive(s_int_sem);
        }
    } else {
        ESP_LOGW(TAG, "  read buffer status 0x814E failed: %s", esp_err_to_name(status_ret));
    }

    s_initialized = true;
    ESP_LOGI(TAG, "[init] done: GT911 at 0x%02X, INT GPIO%d trigger %d, reader task %s",
             s_i2c_addr, ESPAPERPLAY_PIN_TOUCH_INT, (int)int_trig,
             s_task_wait_ms == 0 ? "interrupt-driven" : "polling");
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

esp_err_t espaperplay_touch_diag(void) {
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    const int int_level = gpio_get_level(ESPAPERPLAY_PIN_TOUCH_INT);

    uint8_t status = 0xFF;
    const esp_err_t status_ret = gt911_read_reg(GT911_REG_STATUS, &status, 1);

    uint8_t fresh = 0xFF;
    const esp_err_t fresh_ret = gt911_read_reg(GT911_REG_CONFIG_REFRESH, &fresh, 1);

    ESP_LOGI(TAG,
             "[diag] INT=%d status=0x%02X (%u points%s) config_fresh=0x%02X%s%s",
             int_level, status_ret == ESP_OK ? status : 0xFF, status & GT911_STATUS_POINTS_MASK,
             (status & GT911_STATUS_BUFFER_READY) ? ", buffer ready" : "",
             fresh_ret == ESP_OK ? fresh : 0xFF,
             status_ret != ESP_OK ? " (status read failed)" : "",
             fresh_ret != ESP_OK ? " (fresh read failed)" : "");
    return ESP_OK;
}
