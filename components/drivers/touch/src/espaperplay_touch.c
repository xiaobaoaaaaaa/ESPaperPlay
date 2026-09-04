/*
 * SPDX-FileCopyrightText: 2026 ESPaperPlay Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#include <stdint.h>
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
#include "espaperplay_diaglog.h"
#include "espaperplay_touch.h"

static const char *TAG = "ESPaperPlay_TOUCH";

/* ====================================================================
 * GT911 寄存器定义（《GT911 Datasheet》Rev.10 + 厂商 STM32 demo gt9xx.c）
 * ==================================================================== */

#define GT911_REG_CONFIG_VERSION 0x8047 /*!< 配置版本（config 区第 0 字节） */
#define GT911_REG_X_OUTPUT_MAX 0x8048   /*!< X 输出最大值（小端，config 区第 1~2 字节） */
#define GT911_REG_Y_OUTPUT_MAX 0x804A   /*!< Y 输出最大值（小端，config 区第 3~4 字节） */
#define GT911_REG_TOUCH_NUMBER 0x804C   /*!< 支持触摸点数（config 区第 5 字节，低 4 位） */
#define GT911_REG_MODULE_SW1 0x804D /*!< 模块开关 1（config 区第 6 字节，低 2 位=INT 触发模式） */
#define GT911_REG_CMD 0x8040        /*!< 命令寄存器：写 1 = 软复位（芯片自动清零） */
#define GT911_REG_PRODUCT_ID 0x8140 /*!< 产品 ID："911" */
#define GT911_REG_STATUS 0x814E     /*!< 缓冲状态：b7=缓冲就绪，b3:0=触摸点数 */
#define GT911_REG_POINT_BASE 0x814F /*!< 触摸点数据区起点（每点 8 字节） */

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
#define GT911_RESET_ASSERT_MS 20      /*!< RST 低电平保持时间（≥12ms） */
#define GT911_INT_ADDR_HOLD_MS 60     /*!< RST 释放后 INT 保持地址电平时间（≥55ms） */
#define GT911_BOOT_SETTLE_MS 100      /*!< 复位结束后等待固件启动（毫秒） */
#define GT911_PROBE_RETRIES 10        /*!< 产品 ID 探测重试次数 */
#define GT911_PROBE_RETRY_DELAY_MS 50 /*!< 探测重试间隔（毫秒） */

/** INT 轮询模式（0x804D 低 2 位 == 3）下任务的读取周期（毫秒）。 */
#define GT911_POLL_MODE_PERIOD_MS 20

/* ====================================================================
 * 运行期诊断（失效现场分析，无需重启）
 *
 * 背景：触摸「高概率无响应」失效时，INT 引脚实测 ~2.53V（约等于 ESP32
 * 45k 内部上拉与 GT911 异常态弱下拉的分压），说明芯片侧进入异常状态。
 * 诊断目标是在失效现场回答四个互斥问题：
 *   1. 芯片应答哪个地址——GT911 任何一次复位（含内部复位）都会在复位
 *      释放瞬间按 INT 电平重锁存地址：INT 高 → 0x14，主机仍访问 0x5D
 *      则表现为触摸全死；
 *   2. 总线上是否还有设备应答——全无应答 = 芯片掉电 / 卡在复位态 /
 *      I2C 引擎挂死（区分：探测超时 vs NACK）；
 *   3. 缓冲状态是否卡在「有数据未消费」——INT 空闲却有待读帧，是主机
 *      中断事件丢失的直接证据；
 *   4. 寄存器快照是否还健康（产品 ID / 配置版本 / 刷新标志）。
 * ==================================================================== */

#define GT911_DIAG_PROBE_TIMEOUT_MS 50        /*!< 总线单地址探测超时（毫秒） */
#define GT911_DIAG_AUTO_MIN_INTERVAL_MS 10000 /*!< 自动诊断最小间隔（限流，毫秒） */
#define GT911_HEALTH_CHECK_PERIOD_MS 5000     /*!< 中断模式下无帧健康巡检周期（毫秒） */
#define GT911_HEALTH_CHECK_HEARTBEAT_CHECKS \
    ((3600U * 1000U) / GT911_HEALTH_CHECK_PERIOD_MS) /*!< SD 心跳间隔（换算为巡检次数，约 1 小时） */
#define GT911_DIAG_FAIL_STREAK_LIMIT 3        /*!< 连续 I2C 错误触发自动诊断的次数 */
#define GT911_HC_FAIL_RECOVER_LIMIT 2         /*!< 健康巡检连续失败触发自动恢复的次数 */
#define GT911_RECOVER_MIN_INTERVAL_MS 5000    /*!< 自动恢复最小间隔（限流，毫秒） */
#define GT911_RECOVER_MAX_INTERVAL_MS 60000   /*!< 连续失败指数退避上限（毫秒） */

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

/* 运行期诊断状态。 */
static int64_t s_last_diag_us = INT64_MIN; /*!< 上次自动诊断时刻（限流用，微秒） */
static uint32_t s_i2c_fail_streak = 0;     /*!< 连续 I2C 读错误计数 */
static uint32_t s_hc_fail_streak = 0;      /*!< 健康巡检连续失败计数 */
static int64_t s_last_recover_us = INT64_MIN; /*!< 上次自动恢复时刻（限流用，微秒） */
static int64_t s_recover_interval_us =
    (int64_t)GT911_RECOVER_MIN_INTERVAL_MS * 1000LL; /*!< 当前恢复重试间隔（连续失败指数退避） */
static gpio_int_type_t s_int_trig =
    GPIO_INTR_LOW_LEVEL; /*!< 当前生效的 INT 触发方式（运行期恢复后需重放） */

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
 * @brief 从指定 I2C 设备读 GT911 寄存器（16 位大端寄存器地址）。
 *
 * 诊断时需要对备用地址建临时设备句柄读寄存器，故拆出带设备参数的底层。
 */
static esp_err_t gt911_read_reg_dev(i2c_master_dev_handle_t dev, uint16_t reg, uint8_t *data,
                                    size_t len) {
    const uint8_t reg_addr[2] = {(uint8_t)(reg >> 8), (uint8_t)(reg & 0xFF)};
    ESP_RETURN_ON_ERROR(
        i2c_master_transmit(dev, reg_addr, sizeof(reg_addr), ESPAPERPLAY_I2C_TIMEOUT_MS), TAG,
        "write register address 0x%04X failed", reg);
    return i2c_master_receive(dev, data, len, ESPAPERPLAY_I2C_TIMEOUT_MS);
}

/**
 * @brief 读 GT911 寄存器（16 位大端寄存器地址 + 连续数据）。
 *
 * 严格按《GT911 Datasheet》Rev.10 第 6 章读时序（c) Reading Data from
 * GT911）分两段事务执行：
 * S | Addr_W | Reg_H | Reg_L | E，随后 S | Addr_R | Data... | NACK | E。
 */
static esp_err_t gt911_read_reg(uint16_t reg, uint8_t *data, size_t len) {
    return gt911_read_reg_dev(s_dev, reg, data, len);
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
    /* RST 用 INPUT_OUTPUT：输出驱动之外使能输入缓冲，诊断时软件可回读
     * pad 实际电平（纯 OUTPUT 模式下 gpio_get_level 读不到外部电平）。 */
    const gpio_config_t out_cfg = {
        .pin_bit_mask = BIT64(ESPAPERPLAY_PIN_TOUCH_INT) | BIT64(ESPAPERPLAY_PIN_TOUCH_RST),
        .mode = GPIO_MODE_INPUT_OUTPUT,
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
            ESP_LOGW(TAG,
                     "probe retry %d/%d: product ID mismatch, got %02X %02X %02X %02X "
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

    ESP_RETURN_ON_ERROR(gt911_write_reg(GT911_REG_CONFIG_VERSION, cfg, sizeof(cfg)), TAG,
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
    ESP_LOGI(TAG,
             "  raw config 0x8047..0x804D: %02X %02X %02X %02X %02X %02X %02X "
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
        ESP_RETURN_ON_ERROR(gt911_read_reg(GT911_REG_POINT_BASE, raw, num * GT911_POINT_PACKET_LEN),
                            TAG, "read %u touch point(s) failed", num);

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

    if (cb != NULL) {
        cb(points, num);
    }
}

/* ====================================================================
 * 运行期诊断
 * ==================================================================== */

/**
 * @brief 输出一份完整诊断现场（GPIO 电平 / 双地址探测 / 寄存器快照 / 结论）。
 *
 * 可在任意任务上下文调用；所有 I2C 访问都带超时，芯片挂死时也会在数百
 * 毫秒内返回。结论按「地址翻转 / 无设备应答 / 缓冲卡帧 / 健康」输出，
 * 全部落在串口日志（TAG = 本文件 TAG，行首 [diag/原因]）。
 *
 * @param reason 触发原因标签，如 "manual" / "alt-addr-ack"。
 */
static void gt911_diag_dump(const char *reason) {
    if (s_bus == NULL) {
        return;
    }

    const int int_level = gpio_get_level(ESPAPERPLAY_PIN_TOUCH_INT);
    const int rst_level = gpio_get_level(ESPAPERPLAY_PIN_TOUCH_RST);

    /* 1. 双地址探测：NOT_FOUND=该地址无人应答；TIMEOUT=总线被卡死
     *    （典型如芯片挂死拉住 SDA）。 */
    const esp_err_t probe_5d =
        i2c_master_probe(s_bus, GT911_I2C_ADDR_INT_LOW, GT911_DIAG_PROBE_TIMEOUT_MS);
    const esp_err_t probe_14 =
        i2c_master_probe(s_bus, GT911_I2C_ADDR_INT_HIGH, GT911_DIAG_PROBE_TIMEOUT_MS);
    ESP_LOGW(TAG, "[diag/%s] GPIO: INT=%d RST=%d | probe: 0x%02X=%s, 0x%02X=%s", reason, int_level,
             rst_level, GT911_I2C_ADDR_INT_LOW, esp_err_to_name(probe_5d),
             GT911_I2C_ADDR_INT_HIGH, esp_err_to_name(probe_14));
    espaperplay_diaglog_write("TOUCH", "diag/%s: INT=%d RST=%d probe 0x5D=%s 0x14=%s", reason,
                              int_level, rst_level, esp_err_to_name(probe_5d),
                              esp_err_to_name(probe_14));

    if (probe_5d != ESP_OK && probe_14 != ESP_OK) {
        ESP_LOGE(TAG, "[diag] verdict: NO DEVICE ACKs — GT911 unpowered / stuck in reset / "
                      "I2C engine crashed (TIMEOUT=bus stuck, NOT_FOUND=no answer at all)");
        espaperplay_diaglog_write("TOUCH", "diag/%s VERDICT: no device ACKs (0x5D=%s, 0x14=%s)",
                                  reason, esp_err_to_name(probe_5d), esp_err_to_name(probe_14));
        return;
    }

    /* 2. 地址翻转检测：期望地址无人应答而备用地址应答——芯片发生过一次
     *    复位（外部 RST 脉冲或内部复位），且复位释放瞬间采样到 INT=高，
     *    地址被重锁存为 0x14。 */
    const bool expect_low = (s_i2c_addr == GT911_I2C_ADDR_INT_LOW);
    const esp_err_t probe_expect = expect_low ? probe_5d : probe_14;
    const esp_err_t probe_alt = expect_low ? probe_14 : probe_5d;
    const uint8_t alt_addr =
        expect_low ? GT911_I2C_ADDR_INT_HIGH : GT911_I2C_ADDR_INT_LOW;
    if (probe_expect != ESP_OK && probe_alt == ESP_OK) {
        ESP_LOGE(TAG, "[diag] verdict: ADDRESS FLIP — GT911 answers 0x%02X but driver uses "
                      "0x%02X; chip was reset while INT sampled HIGH, touch dead until "
                      "re-reset with INT low",
                 alt_addr, s_i2c_addr);
        espaperplay_diaglog_write("TOUCH",
                                  "diag/%s VERDICT: ADDRESS FLIP, GT911 at 0x%02X (driver "
                                  "uses 0x%02X) — chip reset with INT sampled HIGH",
                                  reason, alt_addr, s_i2c_addr);
    }

    /* 3. 当前地址下的寄存器快照。 */
    uint8_t id[4] = {0};
    const esp_err_t id_ret = gt911_read_reg(GT911_REG_PRODUCT_ID, id, sizeof(id));
    uint8_t status = 0;
    const esp_err_t status_ret = gt911_read_reg(GT911_REG_STATUS, &status, 1);
    uint8_t cfg[7] = {0};
    const esp_err_t cfg_ret = gt911_read_reg(GT911_REG_CONFIG_VERSION, cfg, sizeof(cfg));
    uint8_t fresh = 0;
    const esp_err_t fresh_ret = gt911_read_reg(GT911_REG_CONFIG_REFRESH, &fresh, 1);
    ESP_LOGW(TAG,
             "[diag] regs@0x%02X: id=\"%c%c%c\" (%s), status=0x%02X (%u pts%s), "
             "cfg=%02X %02X %02X %02X %02X %02X %02X (%ux%u, INT mode %u, %s), fresh=0x%02X (%s)",
             s_i2c_addr, id[0], id[1], id[2], esp_err_to_name(id_ret),
             status_ret == ESP_OK ? status : 0xFF, status & GT911_STATUS_POINTS_MASK,
             (status & GT911_STATUS_BUFFER_READY) ? ", ready" : "", cfg[0], cfg[1], cfg[2], cfg[3],
             cfg[4], cfg[5], cfg[6], (unsigned)((cfg[2] << 8) | cfg[1]),
             (unsigned)((cfg[4] << 8) | cfg[3]), cfg[6] & 0x03,
             esp_err_to_name(cfg_ret), fresh_ret == ESP_OK ? fresh : 0xFF,
             esp_err_to_name(fresh_ret));
    espaperplay_diaglog_write("TOUCH",
                              "diag/%s regs@0x%02X: id=\"%c%c%c\" (%s) status=0x%02X "
                              "cfg=%02X %02X %02X %02X %02X %02X %02X fresh=0x%02X (%s)",
                              reason, s_i2c_addr, id[0], id[1], id[2], esp_err_to_name(id_ret),
                              status_ret == ESP_OK ? status : 0xFF, cfg[0], cfg[1], cfg[2], cfg[3],
                              cfg[4], cfg[5], cfg[6], fresh_ret == ESP_OK ? fresh : 0xFF,
                              esp_err_to_name(fresh_ret));

    /* 4. 备用地址上若真有 GT911，读其产品 ID 坐实地址翻转。 */
    if (probe_alt == ESP_OK && probe_expect != ESP_OK) {
        const i2c_device_config_t alt_cfg = {
            .dev_addr_length = I2C_ADDR_BIT_LEN_7,
            .device_address = alt_addr,
            .scl_speed_hz = ESPAPERPLAY_TOUCH_I2C_CLK_HZ,
        };
        i2c_master_dev_handle_t alt_dev = NULL;
        if (i2c_master_bus_add_device(s_bus, &alt_cfg, &alt_dev) == ESP_OK) {
            uint8_t alt_id[4] = {0};
            const esp_err_t alt_ret =
                gt911_read_reg_dev(alt_dev, GT911_REG_PRODUCT_ID, alt_id, sizeof(alt_id));
            if (alt_ret == ESP_OK) {
                ESP_LOGE(TAG, "[diag] alt addr 0x%02X product ID \"%c%c%c\"%s", alt_addr, alt_id[0],
                         alt_id[1], alt_id[2],
                         (memcmp(alt_id, "911", 3) == 0) ? " — address flip confirmed"
                                                         : " (not a GT911?)");
                espaperplay_diaglog_write("TOUCH",
                                          "diag/%s alt addr 0x%02X product ID \"%c%c%c\"%s",
                                          reason, alt_addr, alt_id[0], alt_id[1], alt_id[2],
                                          (memcmp(alt_id, "911", 3) == 0)
                                              ? " (address flip confirmed)"
                                              : " (not a GT911?)");
            } else {
                ESP_LOGW(TAG, "[diag] alt addr 0x%02X ACKs but regs unreadable (%s)", alt_addr,
                         esp_err_to_name(alt_ret));
            }
            i2c_master_bus_rm_device(alt_dev);
        }
    }

    /* 5. 结论：缓冲卡帧 / 寄存器不可读 / 健康。 */
    if (status_ret != ESP_OK) {
        ESP_LOGE(TAG, "[diag] verdict: registers unreadable at expected address 0x%02X (%s) — "
                      "chip state machine abnormal", s_i2c_addr, esp_err_to_name(status_ret));
        espaperplay_diaglog_write("TOUCH",
                                  "diag/%s VERDICT: regs unreadable at 0x%02X (%s) — chip "
                                  "state machine abnormal",
                                  reason, s_i2c_addr, esp_err_to_name(status_ret));
    } else if (status & GT911_STATUS_BUFFER_READY) {
        ESP_LOGE(TAG, "[diag] verdict: buffer stuck (0x814E=0x%02X) while INT=%d — frame "
                      "pending but never consumed: host interrupt event lost", status,
                 int_level);
        espaperplay_diaglog_write("TOUCH",
                                  "diag/%s VERDICT: buffer stuck 0x%02X while INT=%d — host "
                                  "interrupt event lost",
                                  reason, status, int_level);
    } else if (id_ret == ESP_OK && memcmp(id, "911", 3) == 0) {
        ESP_LOGI(TAG, "[diag] verdict: healthy at 0x%02X (id \"911\", buffer idle, INT=%d)",
                 s_i2c_addr, int_level);
        espaperplay_diaglog_write("TOUCH", "diag/%s VERDICT: healthy at 0x%02X (INT=%d)", reason,
                                  s_i2c_addr, int_level);
    }
}

/**
 * @brief 自动诊断（限流）：最小间隔内的重复触发直接忽略，避免刷屏。
 */
static void gt911_diag_dump_auto(const char *reason) {
    const int64_t now_us = esp_timer_get_time();
    if (s_last_diag_us != INT64_MIN &&
        now_us - s_last_diag_us < (int64_t)GT911_DIAG_AUTO_MIN_INTERVAL_MS * 1000LL) {
        return;
    }
    s_last_diag_us = now_us;
    gt911_diag_dump(reason);
}

static bool gt911_recover(void); /*!< 前置声明：健康巡检在检测到失效形态时调用 */

/**
 * @brief 中断模式下的周期健康巡检（读取任务在信号量上超时唤醒时调用）。
 *
 * 正常时静默，开销为每周期 2~3 次短 I2C 事务。巡检项：
 *   1. 备用地址探测——GT911 因内部复位把地址重锁存到备用地址（触摸全死
 *      的已知形态）时，直接落诊断现场；
 *   2. 缓冲状态——有帧未消费而 INT 空闲 = 中断事件丢失：读走投递恢复
 *      链路，并留下证据日志；
 *   3. I2C 读错误连续计数——达到阈值落诊断现场（由调用方累计）。
 */
static void gt911_health_check(void) {
    if (s_bus == NULL || s_dev == NULL) {
        return;
    }

    /* 1) 备用地址探测：有应答即地址翻转（已确认的失效形态），落诊断
     *    现场后立即自动恢复。 */
    const uint8_t alt = (s_i2c_addr == GT911_I2C_ADDR_INT_LOW) ? GT911_I2C_ADDR_INT_HIGH
                                                               : GT911_I2C_ADDR_INT_LOW;
    if (i2c_master_probe(s_bus, alt, GT911_DIAG_PROBE_TIMEOUT_MS) == ESP_OK) {
        ESP_LOGW(TAG, "[diag] health check: device ACKs at alternate address 0x%02X", alt);
        espaperplay_diaglog_write("TOUCH", "health: device ACKs at alt addr 0x%02X", alt);
        gt911_diag_dump_auto("alt-addr-ack");
        (void)gt911_recover();
        return;
    }

    /* 2) 缓冲状态巡检。 */
    uint8_t status = 0;
    const esp_err_t err = gt911_read_reg(GT911_REG_STATUS, &status, 1);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "[diag] health check: status read failed (%s)", esp_err_to_name(err));
        espaperplay_diaglog_write("TOUCH", "health: status read failed (%s)",
                                  esp_err_to_name(err));
        /* 连续失败：芯片可能整体挂死，尝试复位级恢复。 */
        if (++s_hc_fail_streak >= GT911_HC_FAIL_RECOVER_LIMIT) {
            s_hc_fail_streak = 0;
            gt911_diag_dump_auto("hc-fail-streak");
            (void)gt911_recover();
        }
        return;
    }
    s_hc_fail_streak = 0;
    if ((status & GT911_STATUS_BUFFER_READY) == 0) {
        /* 一切正常：无帧无错误。每小时向 SD 写一条心跳（自证存活 + 留
         * 状态时间线），串口保持静默不刷屏。 */
        static uint32_t s_checks_since_beat = 0;
        if (++s_checks_since_beat >= GT911_HEALTH_CHECK_HEARTBEAT_CHECKS) {
            s_checks_since_beat = 0;
            ESP_LOGI(TAG, "[diag] health check alive (addr 0x%02X, status 0x%02X, INT=%d)",
                     s_i2c_addr, status, gpio_get_level(ESPAPERPLAY_PIN_TOUCH_INT));
            espaperplay_diaglog_write("TOUCH", "health: alive (addr 0x%02X, status 0x%02X, "
                                               "INT=%d)",
                                      s_i2c_addr, status,
                                      gpio_get_level(ESPAPERPLAY_PIN_TOUCH_INT));
        }
        return;
    }

    /* 3) 有帧未消费而中断没来：读走投递并留下证据日志。 */
    uint8_t num = 0;
    espaperplay_touch_point_t points[ESPAPERPLAY_TOUCH_MAX_POINTS];
    const esp_err_t frame_ret = gt911_read_frame(points, &num);
    if (frame_ret != ESP_OK) {
        ESP_LOGW(TAG, "[diag] health check: buffer ready (0x814E=0x%02X) but frame read "
                      "failed (%s)", status, esp_err_to_name(frame_ret));
        espaperplay_diaglog_write("TOUCH", "health: buffer ready 0x%02X but frame read "
                                           "failed (%s)",
                                  status, esp_err_to_name(frame_ret));
        return;
    }
    ESP_LOGW(TAG, "[diag] health check: pending frame (%u pt) with INT idle (level=%d) — "
                  "INT event lost; consumed and delivered", num,
             gpio_get_level(ESPAPERPLAY_PIN_TOUCH_INT));
    espaperplay_diaglog_write("TOUCH",
                              "health: pending frame (%u pt) with INT idle (level=%d) — INT "
                              "event lost, consumed and delivered",
                              num, gpio_get_level(ESPAPERPLAY_PIN_TOUCH_INT));
    gt911_deliver_frame(points, num);
}

/**
 * @brief 运行期自动恢复：重新执行「INT 电平锁存复位」时序，把 I2C 地址
 *        重锁存回驱动使用的地址，并按需重写面板配置。
 *
 * 已由失效现场确认的形态：GT911 发生（内部）复位时 INT 采样为高，地址
 * 被重锁存为 0x14，而驱动仍访问 0x5D，触摸全死直到重新复位。本函数把
 * 芯片恢复到与开机初始化等价的状态。调用方须在读取任务上下文（独占
 * I2C 总线）或确认无并发总线访问。
 *
 * @return 恢复成功（产品 ID 重新应答）返回 true。
 */
static bool gt911_recover(void) {
    /* 限流 + 指数退避：正常恢复 5s 内即可重试；芯片持续无应答时逐次
     * 翻倍至 60s 封顶，避免高频空转刷爆串口与 SD 日志。 */
    const int64_t now_us = esp_timer_get_time();
    if (s_last_recover_us != INT64_MIN && now_us - s_last_recover_us < s_recover_interval_us) {
        return false;
    }
    s_last_recover_us = now_us;

    const int latch_level = (s_i2c_addr == GT911_I2C_ADDR_INT_HIGH) ? 1 : 0;
    ESP_LOGW(TAG, "[recover] re-running reset sequence (INT %s -> relatch 0x%02X)",
             latch_level ? "HIGH" : "LOW", s_i2c_addr);
    espaperplay_diaglog_write("TOUCH", "recover: re-reset with INT %s (relatch 0x%02X)",
                              latch_level ? "HIGH" : "LOW", s_i2c_addr);

    /* 先关 GPIO3 中断再动时序：复位期间 INT 被本驱动拉到锁存电平、芯片
     * 端又处于阈值附近的半浮动态，低电平触发的中断会连续重触发（中断
     * 风暴），叠加 esp_pm 的 ISR 钩子延时会撑爆中断看门狗（已实测崩溃）。
     * 开机路径无此问题，因为 ISR 在复位时序之后才安装。 */
    gpio_intr_disable(ESPAPERPLAY_PIN_TOUCH_INT);

    gt911_hw_reset(latch_level);
    const bool answered = gt911_probe_product_id();

    if (answered) {
        /* 硬复位后配置区可能被清空：回读无效则重写厂商面板配置。 */
        uint8_t version = 0;
        uint16_t x_max = 0;
        uint16_t y_max = 0;
        uint8_t touch_num = 0;
        uint8_t int_mode = 1;
        if (!gt911_read_config(&version, &x_max, &y_max, &touch_num, &int_mode)) {
            ESP_LOGW(TAG, "[recover] config area empty after reset -> rewriting vendor config");
            espaperplay_diaglog_write("TOUCH", "recover: config empty, rewriting vendor config");
            (void)gt911_write_config();
        }

        /* 复位/重配置后芯片在一两个扫描周期内可能报出幻影帧（残留电极
         * 状态）并拉低 INT：静置等待后读走并丢弃。若走正常投递路径，会
         * 向 UI 注入幽灵触摸并刷新活动时间戳，阻碍设备重新进入睡眠
         * （实测缺陷）。此窗口内 INT 中断仍处于关闭状态，无中断风暴。 */
        vTaskDelay(pdMS_TO_TICKS(150));
        for (int i = 0; i < 5; i++) {
            uint8_t status = 0;
            if (gt911_read_reg(GT911_REG_STATUS, &status, 1) != ESP_OK ||
                (status & GT911_STATUS_BUFFER_READY) == 0) {
                break;
            }
            uint8_t num = 0;
            espaperplay_touch_point_t points[ESPAPERPLAY_TOUCH_MAX_POINTS];
            if (gt911_read_frame(points, &num) == ESP_OK) {
                ESP_LOGW(TAG, "[recover] discarded phantom frame after reset (%u pt)", num);
                espaperplay_diaglog_write("TOUCH", "recover: discarded phantom frame (%u pt)",
                                          num);
            }
            vTaskDelay(pdMS_TO_TICKS(20));
        }
    }

    /* 恢复运行期触发方式（hw_reset 会把 INT 中断配置清为 DISABLE），再
     * 重新开中断，并清掉禁用期间积压的信号量。 */
    gpio_set_intr_type(ESPAPERPLAY_PIN_TOUCH_INT, s_int_trig);
    gpio_intr_enable(ESPAPERPLAY_PIN_TOUCH_INT);
    while (xSemaphoreTake(s_int_sem, 0) == pdTRUE) {
    }

    if (!answered) {
        s_recover_interval_us *= 2;
        if (s_recover_interval_us > (int64_t)GT911_RECOVER_MAX_INTERVAL_MS * 1000LL) {
            s_recover_interval_us = (int64_t)GT911_RECOVER_MAX_INTERVAL_MS * 1000LL;
        }
        ESP_LOGE(TAG, "[recover] GT911 still not answering after re-reset (next retry in "
                      "%lld ms)",
                 (long long)(s_recover_interval_us / 1000));
        espaperplay_diaglog_write("TOUCH", "recover FAILED: no answer after re-reset (retry in "
                                           "%lld ms)",
                                  (long long)(s_recover_interval_us / 1000));
        return false;
    }

    s_recover_interval_us = (int64_t)GT911_RECOVER_MIN_INTERVAL_MS * 1000LL;
    ESP_LOGI(TAG, "[recover] GT911 re-latched at 0x%02X, touch restored", s_i2c_addr);
    espaperplay_diaglog_write("TOUCH", "recover OK: GT911 re-latched 0x%02X", s_i2c_addr);
    return true;
}

/**
 * @brief 触摸读取任务。
 *
 * 中断模式：阻塞在信号量上等待 INT 中断，被唤醒后消费触摸帧，缓存最新
 * 一帧并回调（若已注册）。中断沿可能被遗漏，因此消费循环用 INT 有效电平
 * 兜底——数据未消费时 INT 保持有效电平，连续读取直到缓冲无新数据为止。
 * 信号量上另有周期性超时唤醒：超时（即一段时间无任何中断）则执行健康
 * 巡检 gt911_health_check()，覆盖「中断事件丢失 / 芯片地址翻转 / I2C
 * 连续出错」三类失效形态的现场捕捉。
 *
 * 轮询模式（芯片配置 0x804D 低 2 位 == 3，INT 不产生事件）：按
 * GT911_POLL_MODE_PERIOD_MS 周期直接读状态寄存器，每次唤醒消费一帧。
 */
static void gt911_task(void *arg) {
    (void)arg;
    static bool s_spurious_wake_logged = false;

    ESP_LOGI(TAG, "touch reader task started (%s)", s_task_wait_ms == 0 ? "interrupt" : "poll");

    for (;;) {
        /* 中断模式下不无限等待：周期超时唤醒做健康巡检（空闲时开销为
         * 每周期 2~3 次短 I2C 事务）。 */
        const TickType_t wait_ticks =
            (s_task_wait_ms == 0) ? pdMS_TO_TICKS(GT911_HEALTH_CHECK_PERIOD_MS)
                                  : pdMS_TO_TICKS(s_task_wait_ms);
        const bool wake_by_int = (xSemaphoreTake(s_int_sem, wait_ticks) == pdTRUE);

        /* 轮询模式（芯片 INT mode 3）：每个周期尝试读一帧，不依赖 INT 电平。 */
        if (s_task_wait_ms != 0) {
            uint8_t num = 0;
            espaperplay_touch_point_t points[ESPAPERPLAY_TOUCH_MAX_POINTS];
            if (gt911_read_frame(points, &num) == ESP_OK) {
                gt911_deliver_frame(points, num);
            }
            continue;
        }

        if (!wake_by_int) {
            gt911_health_check();
            continue;
        }

        /* 中断模式：数据未消费时 INT 保持有效电平，用电平兜底被遗漏的中断沿。 */
        while (gpio_get_level(ESPAPERPLAY_PIN_TOUCH_INT) == (s_int_active_low ? 0 : 1)) {
            uint8_t num = 0;
            espaperplay_touch_point_t points[ESPAPERPLAY_TOUCH_MAX_POINTS];
            const esp_err_t err = gt911_read_frame(points, &num);
            if (err == ESP_ERR_NOT_FOUND) {
                if (!s_spurious_wake_logged) {
                    ESP_LOGW(TAG,
                             "spurious INT wake: pin level %d but no data in buffer "
                             "(trigger %s)",
                             gpio_get_level(ESPAPERPLAY_PIN_TOUCH_INT),
                             s_int_active_low ? "falling/low" : "rising/high");
                    s_spurious_wake_logged = true;
                }
                break; /* 无新帧：缓冲已被消费，等待下一次中断 */
            }
            if (err != ESP_OK) {
                /* I2C 访问失败：累计连续错误，达到阈值落诊断现场并尝试
                 * 复位级恢复（芯片挂死/地址翻转都会在此被纠正）。 */
                s_i2c_fail_streak++;
                ESP_LOGW(TAG, "touch frame read failed (%s), streak %u/%u",
                         esp_err_to_name(err), (unsigned)s_i2c_fail_streak,
                         (unsigned)GT911_DIAG_FAIL_STREAK_LIMIT);
                if (s_i2c_fail_streak >= GT911_DIAG_FAIL_STREAK_LIMIT) {
                    s_i2c_fail_streak = 0;
                    s_hc_fail_streak = 0;
                    gt911_diag_dump_auto("i2c-error-streak");
                    (void)gt911_recover();
                }
                break;
            }
            s_i2c_fail_streak = 0;
            gt911_deliver_frame(points, num);
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

    ESP_LOGI(TAG, "[init] start: SDA=%d SCL=%d INT=%d RST=%d, addr candidate 0x%02X",
             ESPAPERPLAY_PIN_TOUCH_SDA, ESPAPERPLAY_PIN_TOUCH_SCL, ESPAPERPLAY_PIN_TOUCH_INT,
             ESPAPERPLAY_PIN_TOUCH_RST, ESPAPERPLAY_GT911_I2C_ADDR);

    /* 1. I2C 主机（GT911 专用，见 board 组件注释）。 */
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
    ESP_LOGI(TAG, "[init] step 1/4 OK: I2C master port %d @ %u Hz (internal pull-ups on)",
             ESPAPERPLAY_I2C_PORT_ID, (unsigned int)ESPAPERPLAY_TOUCH_I2C_CLK_HZ);

    /* 2. 复位 + 地址锁存 + 产品 ID 探测（0x5D 失败则回退 0x14）。 */
    const uint8_t addr_candidates[2] = {
        ESPAPERPLAY_GT911_I2C_ADDR,
        (ESPAPERPLAY_GT911_I2C_ADDR == GT911_I2C_ADDR_INT_LOW) ? GT911_I2C_ADDR_INT_HIGH
                                                               : GT911_I2C_ADDR_INT_LOW,
    };

    bool found = false;
    for (int attempt = 0; attempt < 2 && !found; attempt++) {
        /* INT 低电平复位 → 0x5D；INT 高电平复位 → 0x14（规格书第 6.1 节）。 */
        const int int_level = addr_candidates[attempt] == GT911_I2C_ADDR_INT_HIGH ? 1 : 0;
        ESP_LOGI(TAG, "[init] step 2/4 attempt %d/2: reset with INT held %s -> try addr 0x%02X",
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
                 "Check RST / INT wiring and pull-ups",
                 ESPAPERPLAY_I2C_PORT_ID, ESPAPERPLAY_PIN_TOUCH_SDA, ESPAPERPLAY_PIN_TOUCH_SCL);
        ret = ESP_ERR_NOT_FOUND;
        goto fail;
    }

    ESP_LOGI(TAG, "[init] step 2/4 OK: GT911 found at 0x%02X", s_i2c_addr);

    /* 3. 回读配置：版本、X/Y 输出最大值、触摸点数与 INT 触发模式，
     *    用于坐标换算与中断配置。
     *    若配置区为空/无效（复位后常见，芯片无法扫描报点），按 GT911
     *    编程指南写入厂商面板配置表并软复位，然后回读验证。 */
    uint8_t cfg_version = 0;
    uint16_t x_max = 0;
    uint16_t y_max = 0;
    uint8_t touch_num = 0;
    uint8_t int_mode = 1; /* 默认下降沿（与厂商 demo 一致） */

    if (!gt911_read_config(&cfg_version, &x_max, &y_max, &touch_num, &int_mode)) {
        ESP_LOGW(TAG, "config area empty/invalid -> writing vendor panel config "
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
             "[init] step 3/4 OK: GT911 at 0x%02X, config v0x%02X, "
             "resolution %ux%u, max %u touches, INT mode %u%s",
             s_i2c_addr, cfg_version, s_x_max, s_y_max, touch_num, int_mode,
             s_swap_xy ? ", swap XY" : "");

    /* 4. INT 中断（触发沿随芯片配置）+ 内部读取任务。
     *    主机侧统一用低电平触发（面板配置为下降沿有效/数据未消费保持
     *    低）：电平触发跨浅睡眠边界可靠（边沿中断在睡眠边界会丢失，
     *    见 IDF #9932/#11686），且与电源服务的低电平唤醒同型——
     *    gpio_wakeup_enable() 本就会把触发方式改写为电平，这里显式
     *    声明以消除对初始化顺序的隐式依赖，并供运行期恢复后重放。 */
    gpio_int_type_t int_trig = GPIO_INTR_LOW_LEVEL;
    switch (int_mode) {
    case 0: /* 上升沿触发 */
        int_trig = GPIO_INTR_POSEDGE;
        s_int_active_low = false;
        break;
    case 2: /* 低电平触发 */
    case 1: /* 下降沿触发（默认）：主机侧按低电平消费，覆盖丢失的沿 */
    default:
        int_trig = GPIO_INTR_LOW_LEVEL;
        s_int_active_low = true;
        break;
    case 3: /* 轮询模式：INT 无事件输出，任务按固定周期读状态寄存器 */
        ESP_LOGW(TAG, "GT911 INT polling mode: task polls every %u ms", GT911_POLL_MODE_PERIOD_MS);
        int_trig = GPIO_INTR_LOW_LEVEL;
        s_int_active_low = true;
        s_task_wait_ms = GT911_POLL_MODE_PERIOD_MS;
        break;
    }
    s_int_trig = int_trig;

    ESP_LOGI(TAG, "[init] step 4/4: INT mode %u -> GPIO trigger %d, active %s", int_mode,
             (int)int_trig, s_int_active_low ? "low" : "high");
    ESP_LOGI(TAG, "  INT level before ISR install: %d", gpio_get_level(ESPAPERPLAY_PIN_TOUCH_INT));

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

    if (xTaskCreate(gt911_task, "gt911_touch", 4096, NULL, 5, NULL) != pdPASS) {
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
    ESP_LOGI(TAG, "[init] done: GT911 at 0x%02X, INT GPIO%d trigger %d, reader task %s", s_i2c_addr,
             ESPAPERPLAY_PIN_TOUCH_INT, (int)int_trig,
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
    /* 完整诊断现场：GPIO 电平 / 0x5D、0x14 双地址探测 / 寄存器快照 / 结论，
     * 全部落串口日志（行首 [diag/manual]）。触摸「无响应」失效时请在重启
     * 前调用，现场数据是区分根因（地址翻转 / 芯片挂死 / 中断丢失）的唯一
     * 依据。 */
    gt911_diag_dump("manual");
    return ESP_OK;
}
