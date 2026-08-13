/*
 * SPDX-FileCopyrightText: 2026 ESPaperPlay Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#include <stdlib.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "driver/gpio.h"
#include "driver/spi_master.h"

#include "esp_check.h"
#include "esp_log.h"
#include "esp_timer.h"

#include "espaperplay_config.h"
#include "espaperplay_epd.h"

static const char *TAG = "ESPaperPlay_EPD";

/* ====================================================================
 * UC8179 命令定义（《GDEY075T7-T01 规格书》第 7 章命令表）
 * ==================================================================== */

#define UC8179_CMD_PSR 0x00  /*!< Panel Setting（面板设置） */
#define UC8179_CMD_PWR 0x01  /*!< Power Setting（内部电源选择，本驱动用默认值） */
#define UC8179_CMD_POF 0x02  /*!< Power OFF（关断内部电源） */
#define UC8179_CMD_PON 0x04  /*!< Power ON（内部电源上电） */
#define UC8179_CMD_BTST 0x06 /*!< Booster Soft Start（升压软启动） */
#define UC8179_CMD_DSLP 0x07 /*!< Deep Sleep（深度睡眠，校验码 0xA5） */
#define UC8179_CMD_DTM1 0x10 /*!< Data Start Transmission 1（OLD/旧图像平面） */
#define UC8179_CMD_DRF 0x12  /*!< Display Refresh（启动刷新，等待 BUSY） */
#define UC8179_CMD_DTM2 0x13 /*!< Data Start Transmission 2（NEW/新图像平面） */
#define UC8179_CMD_CDI 0x50  /*!< VCOM and Data Interval Setting */
#define UC8179_CMD_PTL 0x90  /*!< Partial Window（局部窗口，8 参数 + PT_SCAN） */
#define UC8179_CMD_PTIN 0x91 /*!< Partial In（进入局部模式） */
#define UC8179_CMD_PTOUT 0x92 /*!< Partial Out（退出局部模式） */
#define UC8179_CMD_TRES 0x61 /*!< Resolution Setting（分辨率设置） */
#define UC8179_CMD_CASCADE 0xE0 /*!< Cascade Setting（厂商未公开命令） */
#define UC8179_CMD_FORCE_TEMP 0xE5 /*!< Force Temperature（厂商未公开命令） */

/* ====================================================================
 * 寄存器取值
 * 全屏/局部取值参考厂商 demo（Display_EPD_W21.c）；N2OCP 语义参考同面板
 * （GDEY075T7）上验证过的 idfxx_epaper_uc8179 驱动。
 * ==================================================================== */

/** PSR：REG=0（LUT 取自 OTP）、KW/R=1（黑白 KW 模式）、UD=1、SHL=1、
 *  SHD_N=1（升压开）、RST_N=1。 */
#define UC8179_PSR_VALUE 0x1F

/* CDI 共 2 个数据字节：
 *   字节 1：BDZ | - | BDV[1:0] | N2OCP | - | DDX[1:0]
 *   字节 2：- | - | - | SDEND | CDI[3:0]（VCOM 与数据间隔，0111=10 hsync 默认）
 *
 * N2OCP=1（bit3）：每次刷新完成后控制器自动把新图像平面（DTM2）拷贝到
 * 旧图像平面（DTM1），因此驱动无需任何"上一帧"软件维护，每次刷新都按
 * 真实的 {旧,新} 差分选择 LUT 波形（黑底画白、白底画黑均正确）。
 * DDX=01（默认）：数据位 1 = 白（0xFF）、0 = 黑（0x00），与 GUI_Paint.h
 * 的 WHITE=0xFF / BLACK=0x00 一致。注意厂商 demo 的 EPD_init_Fast 用
 * CDI=0x10（DDX=00，极性相反），其位图按相反极性制作，本驱动不沿用。 */
#define UC8179_CDI_FULL_B0 0x29 /*!< 全屏刷新：N2OCP=1，DDX=01（idfxx 取值） */
#define UC8179_CDI_PARTIAL_B0 0xA9 /*!< 局部刷新：边界保持 + N2OCP=1（demo/idfxx 一致） */
#define UC8179_CDI_SLEEP_B0 0xF7   /*!< 睡眠前设置（参考工程 EPD_sleep） */
#define UC8179_CDI_B1 0x07         /*!< 间隔 10 hsync（默认值） */

/** BTST：升压软启动参数（参考工程 EPD_init_Fast，快速刷新模式）。 */
#define UC8179_BTST_B0 0x27
#define UC8179_BTST_B1 0x27
#define UC8179_BTST_B2 0x18
#define UC8179_BTST_B3 0x17

/** 级联设置（未公开命令，取值来自参考工程）。 */
#define UC8179_CASCADE_VALUE 0x02

/** 强制温度（未公开命令，取值来自参考工程）：全屏 0x5A，局部 0x6E，
 *  4 灰阶 0x5F（选择出厂四灰阶波形，idfxx 在同面板验证）。 */
#define UC8179_FORCE_TEMP_FULL 0x5A
#define UC8179_FORCE_TEMP_PARTIAL 0x6E
#define UC8179_FORCE_TEMP_GRAY4 0x5F

/** 4 灰阶初始化参数（idfxx _init_gray，同面板验证）：
 *  PWR：VGH/VGL=20V，VDH/VDL=0x3F；TRES：800x480。 */
static const uint8_t UC8179_PWR_GRAY4[4] = {0x07, 0x07, 0x3F, 0x3F};
/** 面板分辨率 800x480（TRES 命令参数，灰阶/快刷初始化使用）。 */
static const uint8_t UC8179_TRES_VALUE[4] = {
    (uint8_t)(ESPAPERPLAY_DISPLAY_WIDTH >> 8),  (uint8_t)(ESPAPERPLAY_DISPLAY_WIDTH & 0xFF),
    (uint8_t)(ESPAPERPLAY_DISPLAY_HEIGHT >> 8), (uint8_t)(ESPAPERPLAY_DISPLAY_HEIGHT & 0xFF),
};

/** DSLP 校验码：命令仅在数据 == 0xA5 时执行。 */
#define UC8179_DSLP_CHECK 0xA5

/* ====================================================================
 * 快刷参数：出厂 OTP 快刷波形（强制温度 0x5A，与全屏相同）
 *
 * 实测结论：PLL 帧率对本面板无效——规格书 FRS 表与 uc8151 同族编码的
 * 全部候选值（0x06/0x0B/0x3C/0x3A/0x39/不写）刷新耗时完全相同（1826ms），
 * OTP 波形相位按时间而非帧数计时。快刷与全屏耗时相同，保留模式仅为
 * API 兼容；后续可尝试注册表 LUT 或局部波形做整屏刷新。
 * ==================================================================== */

/* ====================================================================
 * 内部状态
 * ==================================================================== */

/** 全屏 1bpp 帧缓冲字节数（800x480/8 = 48000）。 */
#define EPD_FRAME_BYTES (ESPAPERPLAY_DISPLAY_WIDTH * ESPAPERPLAY_DISPLAY_HEIGHT / 8)

/** 4 灰阶整帧字节数（2bpp：每像素 2bit，96000）。 */
#define EPD_GRAY4_FRAME_BYTES (EPD_FRAME_BYTES * 2)

static spi_device_handle_t s_spi_dev = NULL; /*!< EPD SPI 设备句柄 */
static SemaphoreHandle_t s_lock = NULL;      /*!< 刷新互斥锁（自检任务与业务可并发调用） */
static bool s_initialized = false;           /*!< init() 已完成标志 */
static bool s_asleep = true;                 /*!< 面板是否处于深度睡眠（init 后默认睡眠） */
static espaperplay_epd_mode_t s_last_mode = ESPAPERPLAY_EPD_MODE_MAX; /*!< 上次刷新模式（灰阶<->黑白切换时重置旧平面用） */
static espaperplay_epd_mode_t s_controller_mode = ESPAPERPLAY_EPD_MODE_MAX; /*!< 控制器当前波形模式（同模式连续刷新跳过重复初始化） */
#if ESPAPERPLAY_EPD_IDLE_SLEEP_TIMEOUT_MS > 0
static esp_timer_handle_t s_idle_timer = NULL; /*!< 空闲自动睡眠定时器（一次性） */
static volatile uint32_t s_idle_gen = 0;       /*!< 刷新代数：定时器回调核对，过期回调直接退出 */
static uint32_t s_idle_timeout_override_ms = 0; /*!< 空闲超时覆盖（仅 selftest 验证用；0=用宏默认） */
#endif /* ESPAPERPLAY_EPD_IDLE_SLEEP_TIMEOUT_MS > 0 */

/* ====================================================================
 * 底层：GPIO / SPI
 * ==================================================================== */

/**
 * @brief 配置 EPD 控制引脚（DC/RST 输出、BUSY 输入上拉）。
 */
static esp_err_t epd_gpio_init(void) {
    const uint64_t out_mask = (1ULL << ESPAPERPLAY_PIN_EPD_DC) | (1ULL << ESPAPERPLAY_PIN_EPD_RST);

    gpio_config_t io_out = {
        .pin_bit_mask = out_mask,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&io_out), TAG, "EPD DC/RST pin config failed");

    gpio_config_t io_busy = {
        .pin_bit_mask = 1ULL << ESPAPERPLAY_PIN_EPD_BUSY,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE, /* 未驱动时上拉视为"空闲"，与参考工程 IPU 一致 */
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&io_busy), TAG, "EPD BUSY pin config failed");

    gpio_set_level((gpio_num_t)ESPAPERPLAY_PIN_EPD_DC, 0);
    gpio_set_level((gpio_num_t)ESPAPERPLAY_PIN_EPD_RST, 1); /* 复位释放 */
    return ESP_OK;
}

#if ESPAPERPLAY_EPD_ENABLE_POWER_PIN
/**
 * @brief 使能 EPD 电源轨（ESPAPERPLAY_PIN_EPD_PWR 拉高）。
 */
static esp_err_t epd_enable_power(void) {
    gpio_config_t io = {
        .pin_bit_mask = 1ULL << ESPAPERPLAY_PIN_EPD_PWR,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&io), TAG, "EPD power pin config failed");
    gpio_set_level((gpio_num_t)ESPAPERPLAY_PIN_EPD_PWR, 1);
    ESP_LOGI(TAG, "EPD power rail enabled (GPIO%d)", ESPAPERPLAY_PIN_EPD_PWR);
    return ESP_OK;
}
#endif /* ESPAPERPLAY_EPD_ENABLE_POWER_PIN */

/**
 * @brief 写一个 UC8179 命令字节（DC=0）。
 */
static esp_err_t epd_write_cmd(uint8_t cmd) {
    spi_transaction_t t = {
        .flags = SPI_TRANS_USE_TXDATA,
        .tx_data = {cmd},
        .length = 8,
    };
    gpio_set_level((gpio_num_t)ESPAPERPLAY_PIN_EPD_DC, 0); /* 命令 */
    return spi_device_transmit(s_spi_dev, &t);
}

/**
 * @brief 写一组 UC8179 数据字节（DC=1，按 ESPAPERPLAY_EPD_SPI_MAX_TRANSFER
 *        分块，帧缓冲流式传输）。
 *
 * @note CS 在每个事务间自动拉高再拉低，符合规格书"每 8 位拉高 CSB 防误码"
 *       的建议；DC 在整组数据期间保持高电平。
 */
static esp_err_t epd_write_data(const uint8_t *data, size_t len) {
    esp_err_t ret;
    gpio_set_level((gpio_num_t)ESPAPERPLAY_PIN_EPD_DC, 1); /* 数据 */
    while (len > 0) {
        size_t chunk = len > ESPAPERPLAY_EPD_SPI_MAX_TRANSFER ? ESPAPERPLAY_EPD_SPI_MAX_TRANSFER : len;
        spi_transaction_t t = {
            .tx_buffer = data,
            .length = (uint32_t)chunk * 8,
        };
        ret = spi_device_transmit(s_spi_dev, &t);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "SPI transmit failed (%u bytes): %s", (unsigned)chunk,
                     esp_err_to_name(ret));
            return ret;
        }
        data += chunk;
        len -= chunk;
    }
    return ESP_OK;
}

/**
 * @brief 写 len 个相同字节（用于清屏/平面填充，避免分配大缓冲）。
 */
static esp_err_t epd_write_fill(uint8_t value, size_t len) {
    static uint8_t s_fill[ESPAPERPLAY_EPD_SPI_MAX_TRANSFER];
    esp_err_t ret;

    memset(s_fill, value, sizeof(s_fill));
    while (len > 0) {
        size_t chunk = len > sizeof(s_fill) ? sizeof(s_fill) : len;
        ret = epd_write_data(s_fill, chunk);
        if (ret != ESP_OK) {
            return ret;
        }
        len -= chunk;
    }
    return ESP_OK;
}

/**
 * @brief 等待 BUSY 释放（BUSY 低电平 = 忙，高电平 = 空闲）。
 *
 * @return ESP_OK 或 ESP_ERR_TIMEOUT。
 */
static esp_err_t epd_wait_busy(void) {
    const int64_t deadline = esp_timer_get_time() + (int64_t)ESPAPERPLAY_EPD_BUSY_TIMEOUT_MS * 1000;

    while (gpio_get_level((gpio_num_t)ESPAPERPLAY_PIN_EPD_BUSY) == 0) {
        if (esp_timer_get_time() >= deadline) {
            ESP_LOGE(TAG, "BUSY wait timeout (%u ms)", ESPAPERPLAY_EPD_BUSY_TIMEOUT_MS);
            return ESP_ERR_TIMEOUT;
        }
        vTaskDelay(pdMS_TO_TICKS(1));
    }
    return ESP_OK;
}

/**
 * @brief 硬件复位（参考工程 EPD_W21_Init：低电平至少 10ms 后释放）。
 *        图像 RAM 在复位后保留（idfxx 在同面板上验证），旧平面数据不丢失。
 */
static esp_err_t epd_hw_reset(void) {
    gpio_set_level((gpio_num_t)ESPAPERPLAY_PIN_EPD_RST, 0);
    vTaskDelay(pdMS_TO_TICKS(10));
    gpio_set_level((gpio_num_t)ESPAPERPLAY_PIN_EPD_RST, 1);
    vTaskDelay(pdMS_TO_TICKS(10));
    return ESP_OK;
}

/* ====================================================================
 * 控制器初始化（PSR -> [CDI] -> PON -> [BTST] -> 级联/温度）
 * 注：DTM 数据由各刷新流程写入；旧图像平面由 N2OCP 自动维护。
 * ==================================================================== */

/**
 * @brief 初始化 UC8179 控制器。
 *
 * 每次刷新前都会完整执行本流程（对应参考工程在每次显示操作前的
 * EPD_init_Fast / EPD_display_init），因此睡眠唤醒无需额外调用 init。
 *
 * @param fast  true：全屏刷新参数（CDI 0x29/0x07、BTST、强制温度 0x5A）；
 *              false：局部刷新参数（强制温度 0x6E，CDI 由局部流程另行设置）。
 */
static esp_err_t epd_init_controller(bool fast) {
    static const uint8_t cdi_full[2] = {UC8179_CDI_FULL_B0, UC8179_CDI_B1};
    static const uint8_t btst[4] = {UC8179_BTST_B0, UC8179_BTST_B1, UC8179_BTST_B2, UC8179_BTST_B3};
    const uint8_t cascade = UC8179_CASCADE_VALUE;
    const uint8_t force_temp = fast ? UC8179_FORCE_TEMP_FULL : UC8179_FORCE_TEMP_PARTIAL;
    esp_err_t ret;

    /* 1. 硬件复位（同时用于从深度睡眠唤醒）。 */
    ret = epd_hw_reset();
    if (ret != ESP_OK) {
        return ret;
    }

    /* 2. 面板设置：KW 黑白模式、默认扫描方向、升压开。 */
    {
        const uint8_t psr = UC8179_PSR_VALUE;
        ret = epd_write_cmd(UC8179_CMD_PSR);
        ESP_RETURN_ON_ERROR(ret, TAG, "PSR cmd failed");
        ret = epd_write_data(&psr, 1);
        ESP_RETURN_ON_ERROR(ret, TAG, "PSR data failed");
    }

    /* 3. 全屏模式设置 VCOM/数据间隔（局部模式由局部流程设置 0xA9）。 */
    if (fast) {
        ret = epd_write_cmd(UC8179_CMD_CDI);
        ESP_RETURN_ON_ERROR(ret, TAG, "CDI cmd failed");
        ret = epd_write_data(cdi_full, sizeof(cdi_full));
        ESP_RETURN_ON_ERROR(ret, TAG, "CDI data failed");
    }

    /* 4. 内部电源上电，等待 BUSY 释放（参考工程延时 300ms 的等价物）。
     *    先留 5ms 余量：BUSY 在 PON 命令后才会拉低，立即轮询可能误读
     *    复位后尚未驱动的空闲电平而提前返回。 */
    ret = epd_write_cmd(UC8179_CMD_PON);
    ESP_RETURN_ON_ERROR(ret, TAG, "PON cmd failed");
    vTaskDelay(pdMS_TO_TICKS(5));
    ret = epd_wait_busy();
    if (ret != ESP_OK) {
        return ret;
    }

    /* 5. 升压软启动（仅全屏/快速模式，参考工程 EPD_init_Fast）。 */
    if (fast) {
        ret = epd_write_cmd(UC8179_CMD_BTST);
        ESP_RETURN_ON_ERROR(ret, TAG, "BTST cmd failed");
        ret = epd_write_data(btst, sizeof(btst));
        ESP_RETURN_ON_ERROR(ret, TAG, "BTST data failed");
    }

    /* 6. 级联设置 + 强制温度（未公开命令，取值来自参考工程，勿随意修改）。 */
    ret = epd_write_cmd(UC8179_CMD_CASCADE);
    ESP_RETURN_ON_ERROR(ret, TAG, "cascade cmd failed");
    ret = epd_write_data(&cascade, 1);
    ESP_RETURN_ON_ERROR(ret, TAG, "cascade data failed");
    ret = epd_write_cmd(UC8179_CMD_FORCE_TEMP);
    ESP_RETURN_ON_ERROR(ret, TAG, "force temp cmd failed");
    ret = epd_write_data(&force_temp, 1);
    ESP_RETURN_ON_ERROR(ret, TAG, "force temp data failed");

    ESP_LOGI(TAG, "controller initialized (%s mode)", fast ? "full" : "partial");
    return ESP_OK;
}

/**
 * @brief 初始化 UC8179 控制器为 4 灰阶模式。
 *
 * 顺序与参数取自 idfxx _init_gray()（在 GDEY075T7 上验证）：PWR 电压配置、
 * BTST 升压软启动、PON、PSR（KW 模式，LUT 取自 OTP）、TRES 分辨率、CDI
 * （N2OCP=1，保持灰阶旧平面同步）、级联设置、强制温度 0x5F 选出厂四灰阶
 * 波形。灰阶波形只能整屏驱动，不支持局部刷新。
 */
static esp_err_t epd_init_controller_gray4(void) {
    static const uint8_t btst[4] = {UC8179_BTST_B0, UC8179_BTST_B1, UC8179_BTST_B2, UC8179_BTST_B3};
    static const uint8_t cdi_full[2] = {UC8179_CDI_FULL_B0, UC8179_CDI_B1};
    const uint8_t psr = UC8179_PSR_VALUE;
    const uint8_t cascade = UC8179_CASCADE_VALUE;
    const uint8_t force_temp = UC8179_FORCE_TEMP_GRAY4;
    esp_err_t ret;

    /* 1. 硬件复位（从任何波形模式/睡眠切换到灰阶前都先复位，清掉粘滞寄存器）。 */
    ret = epd_hw_reset();
    if (ret != ESP_OK) {
        return ret;
    }

    /* 2. 电源配置 + 升压软启动（灰阶需要更高的驱动电压，勿用默认值）。 */
    ret = epd_write_cmd(UC8179_CMD_PWR);
    ESP_RETURN_ON_ERROR(ret, TAG, "PWR cmd failed");
    ret = epd_write_data(UC8179_PWR_GRAY4, sizeof(UC8179_PWR_GRAY4));
    ESP_RETURN_ON_ERROR(ret, TAG, "PWR data failed");
    ret = epd_write_cmd(UC8179_CMD_BTST);
    ESP_RETURN_ON_ERROR(ret, TAG, "BTST cmd failed");
    ret = epd_write_data(btst, sizeof(btst));
    ESP_RETURN_ON_ERROR(ret, TAG, "BTST data failed");

    /* 3. 内部电源上电。 */
    ret = epd_write_cmd(UC8179_CMD_PON);
    ESP_RETURN_ON_ERROR(ret, TAG, "PON cmd failed");
    vTaskDelay(pdMS_TO_TICKS(5));
    ret = epd_wait_busy();
    if (ret != ESP_OK) {
        return ret;
    }

    /* 4. 面板设置 + 分辨率 + VCOM/数据间隔。 */
    ret = epd_write_cmd(UC8179_CMD_PSR);
    ESP_RETURN_ON_ERROR(ret, TAG, "PSR cmd failed");
    ret = epd_write_data(&psr, 1);
    ESP_RETURN_ON_ERROR(ret, TAG, "PSR data failed");
    ret = epd_write_cmd(UC8179_CMD_TRES);
    ESP_RETURN_ON_ERROR(ret, TAG, "TRES cmd failed");
    ret = epd_write_data(UC8179_TRES_VALUE, sizeof(UC8179_TRES_VALUE));
    ESP_RETURN_ON_ERROR(ret, TAG, "TRES data failed");
    ret = epd_write_cmd(UC8179_CMD_CDI);
    ESP_RETURN_ON_ERROR(ret, TAG, "CDI cmd failed");
    ret = epd_write_data(cdi_full, sizeof(cdi_full));
    ESP_RETURN_ON_ERROR(ret, TAG, "CDI data failed");

    /* 5. 级联设置 + 强制温度 0x5F（选择出厂四灰阶波形，未公开命令）。 */
    ret = epd_write_cmd(UC8179_CMD_CASCADE);
    ESP_RETURN_ON_ERROR(ret, TAG, "cascade cmd failed");
    ret = epd_write_data(&cascade, 1);
    ESP_RETURN_ON_ERROR(ret, TAG, "cascade data failed");
    ret = epd_write_cmd(UC8179_CMD_FORCE_TEMP);
    ESP_RETURN_ON_ERROR(ret, TAG, "force temp cmd failed");
    ret = epd_write_data(&force_temp, 1);
    ESP_RETURN_ON_ERROR(ret, TAG, "force temp data failed");

    ESP_LOGI(TAG, "controller initialized (gray4 mode)");
    return ESP_OK;
}

/**
 * @brief 快刷模式复用全屏初始化（OTP 快刷波形，强制温度 0x5A）。
 *
 * 实测 PLL 对本面板无效（所有编码刷新耗时相同，见文件头注释），快刷
 * 与全屏波形一致；保留独立初始化入口仅为模式语义清晰，后续若改用
 * 注册表 LUT 可在此扩展。
 */
static esp_err_t epd_init_controller_fast(void) {
    return epd_init_controller(true);
}

/**
 * @brief 从 2bpp 灰阶帧缓冲解出单个位平面，输出即控制器 RAM 取值（已取反）。
 *
 * 灰阶帧布局：每像素 2bit（MSB 在前），0=白 / 1=浅灰 / 2=深灰 / 3=黑，
 * 每字节 4 像素。bit0 平面写 DTM1（0x10），bit1 平面写 DTM2（0x13）；
 * 控制器灰阶极性为 RAM (1,1)=白，与 1bpp 模式一致，故输出取反
 * （idfxx 在同面板上的做法）。
 *
 * @param frame       2bpp 帧缓冲。
 * @param frame_bytes 帧字节数（须为偶数）。
 * @param out         输出缓冲（frame_bytes/2 字节）。
 * @param plane       0 = 灰阶值 bit0，1 = bit1。
 */
static void gray4_unpack_plane(const uint8_t *frame, size_t frame_bytes, uint8_t *out,
                               unsigned plane) {
    const size_t out_bytes = frame_bytes / 2;

    for (size_t j = 0; j < out_bytes; j++) {
        uint8_t o = 0;
        for (unsigned k = 0; k < 8; k++) {
            const size_t px = j * 8 + k; /* 像素序号 */
            const uint8_t v = (frame[px / 4] >> (6 - 2 * (px % 4))) & 0x03;
            if ((v >> plane) & 1) {
                o |= (uint8_t)(0x80 >> k);
            }
        }
        out[j] = (uint8_t)~o;
    }
}

/**
 * @brief 从灰阶模式切回黑白时，把旧图像平面（DTM1）重置为全白。
 *
 * 灰阶刷新后 DTM1 保存的是灰阶 bit0/bit1 平面（N2OCP 拷贝），黑白差分若
 * 直接使用会得到错误的 {旧,新} 组合；重置为 0xFF 即"旧=全白"近似——面板
 * 当前是灰阶图像，逐像素差分本就无意义，随后的黑白刷新为全屏重绘。
 */
static esp_err_t epd_prepare_bw_old_plane(void) {
    esp_err_t ret;

    if (s_last_mode != ESPAPERPLAY_EPD_MODE_GRAY4) {
        return ESP_OK;
    }
    ret = epd_write_cmd(UC8179_CMD_DTM1);
    ESP_RETURN_ON_ERROR(ret, TAG, "DTM1 cmd failed");
    return epd_write_fill(0xFF, EPD_FRAME_BYTES);
}

/* ====================================================================
 * 局部窗口辅助
 * ==================================================================== */

/**
 * @brief 进入局部模式并设置窗口（CDI 边界保持 -> PTIN -> PTL）。
 *
 * 窗口终点为含端点坐标（x_end = x+w-1，y_end = y+h-1），与 idfxx 驱动在
 * 同面板上的取值一致；HRST/HRED 低 3 位按规格书填写（000/111）。
 */
static esp_err_t epd_window_begin(uint16_t x, uint16_t y, uint16_t width, uint16_t height) {
    static const uint8_t cdi_partial[2] = {UC8179_CDI_PARTIAL_B0, UC8179_CDI_B1};
    const uint16_t x_end = x + width - 1;
    const uint16_t y_end = y + height - 1;
    const uint8_t ptl[9] = {
        (uint8_t)(x >> 8),      (uint8_t)(x & 0xFF),        /* HRST[9:0] */
        (uint8_t)(x_end >> 8),  (uint8_t)(x_end & 0xFF),    /* HRED[9:0] */
        (uint8_t)(y >> 8),      (uint8_t)(y & 0xFF),        /* VRST[9:0] */
        (uint8_t)(y_end >> 8),  (uint8_t)(y_end & 0xFF),    /* VRED[9:0] */
        0x01, /* PT_SCAN=1：全面板扫描（实测 0/1 对刷新耗时无影响，取默认值） */
    };
    esp_err_t ret;

    /* VCOM/数据间隔：局部刷新取值（demo/idfxx 一致），边界保持防闪烁。 */
    ret = epd_write_cmd(UC8179_CMD_CDI);
    ESP_RETURN_ON_ERROR(ret, TAG, "CDI cmd failed");
    ret = epd_write_data(cdi_partial, sizeof(cdi_partial));
    ESP_RETURN_ON_ERROR(ret, TAG, "CDI data failed");

    /* 进入局部模式并设置窗口。 */
    ret = epd_write_cmd(UC8179_CMD_PTIN);
    ESP_RETURN_ON_ERROR(ret, TAG, "PTIN cmd failed");
    ret = epd_write_cmd(UC8179_CMD_PTL);
    ESP_RETURN_ON_ERROR(ret, TAG, "PTL cmd failed");
    return epd_write_data(ptl, sizeof(ptl));
}

/**
 * @brief 退出局部模式。
 */
static esp_err_t epd_window_end(void) {
    return epd_write_cmd(UC8179_CMD_PTOUT);
}

/* ====================================================================
 * 刷新流程
 * ==================================================================== */

/**
 * @brief 启动刷新并等待完成（DRF 后等 10ms 再轮询 BUSY）。
 *
 * @note DRF 后 BUSY 需要一点时间才拉低，立即轮询会误读空闲电平导致提前
 *       返回；参考工程与 idfxx 均延时 10ms（"!!!The delay here is necessary,
 *       200uS at least!!!" 是保守下限）。
 */
static esp_err_t epd_refresh_and_wait(void) {
    esp_err_t ret = epd_write_cmd(UC8179_CMD_DRF);
    ESP_RETURN_ON_ERROR(ret, TAG, "DRF cmd failed");
    vTaskDelay(pdMS_TO_TICKS(10));
    return epd_wait_busy();
}

/**
 * @brief 全屏刷新：DTM2=图像 -> DRF -> 等待 BUSY。
 *
 * 旧图像平面（DTM1）由 N2OCP 在上次刷新后自动维护，因此本流程按真实的
 * {旧,新} 差分选择波形。清屏（image_buf=NULL）时只写 DTM2=0xFF：黑像素
 * 走 黑->白 深度擦除波形，白像素保持（参考 idfxx do_clear 的做法）。
 */
static esp_err_t epd_refresh_full(const void *image_buf) {
    esp_err_t ret;

    /* 同模式连续刷新跳过重复初始化（省掉每次的硬件复位 + PON 开销）；
     * 睡眠唤醒或模式切换时仍完整初始化。 */
    if (s_asleep || s_controller_mode != ESPAPERPLAY_EPD_MODE_FULL) {
        ESP_RETURN_ON_ERROR(epd_init_controller(true), TAG, "controller init (full) failed");
    }

    /* 从灰阶切回黑白时重置旧平面（"旧=全白"近似）。 */
    ret = epd_prepare_bw_old_plane();
    if (ret != ESP_OK) {
        return ret;
    }

    /* DTM2（NEW）：新图像（NULL = 清屏为全白，保留 DTM1 使黑像素深擦除）。 */
    ret = epd_write_cmd(UC8179_CMD_DTM2);
    ESP_RETURN_ON_ERROR(ret, TAG, "DTM2 cmd failed");
    if (image_buf == NULL) {
        ret = epd_write_fill(0xFF, EPD_FRAME_BYTES);
    } else {
        ret = epd_write_data(image_buf, EPD_FRAME_BYTES);
    }
    if (ret != ESP_OK) {
        return ret;
    }

    return epd_refresh_and_wait();
}

/**
 * @brief 局部刷新：CDI(0xA9) -> PTIN -> PTL(窗口) -> DTM2 -> PTOUT -> DRF。
 *
 * 只写窗口内的 DTM2（新数据），旧平面由 N2OCP 维护——黑底画白、白底画黑
 * 均按真实 {旧,新} 差分正确更新。顺序与 idfxx（同面板验证）一致：
 * PTOUT 在 DRF 之前发送。
 *
 * @note x 与 width 必须为 8 的倍数（1bpp 按字节寻址）。
 */
static esp_err_t epd_refresh_partial(const void *image_buf, uint16_t x, uint16_t y, uint16_t width,
                                     uint16_t height) {
    const size_t window_bytes = (size_t)width * height / 8;
    esp_err_t ret;

    /* 同模式连续刷新跳过重复初始化；睡眠唤醒或模式切换时完整初始化。 */
    if (s_asleep || s_controller_mode != ESPAPERPLAY_EPD_MODE_PARTIAL) {
        ESP_RETURN_ON_ERROR(epd_init_controller(false), TAG, "controller init (partial) failed");
    }

    /* 从灰阶切回黑白时重置旧平面（局部窗口同样按"旧=全白"近似）。 */
    ret = epd_prepare_bw_old_plane();
    if (ret != ESP_OK) {
        return ret;
    }

    ret = epd_window_begin(x, y, width, height);
    if (ret != ESP_OK) {
        return ret;
    }

    /* 窗口内 DTM2（NEW）：新数据（NULL = 窗口清白，保留窗口 DTM1 深擦除）。 */
    ret = epd_write_cmd(UC8179_CMD_DTM2);
    ESP_RETURN_ON_ERROR(ret, TAG, "DTM2 cmd failed");
    if (image_buf == NULL) {
        ret = epd_write_fill(0xFF, window_bytes);
    } else {
        ret = epd_write_data(image_buf, window_bytes);
    }
    if (ret != ESP_OK) {
        return ret;
    }

    /* 官方 demo 顺序：刷新在局部模式下进行（DRF 后等 BUSY），完成后再
     * PTOUT。实测（窗口 80x80~800x480 扫描）此顺序比"先 PTOUT 再 DRF"
     * （idfxx 顺序）快 0~13%（小窗口收益最大，全屏无差别），故为默认。 */
    ret = epd_refresh_and_wait();
    if (ret != ESP_OK) {
        return ret;
    }
    return epd_window_end();
}

/**
 * @brief 快刷全屏刷新：DTM2=图像 -> DRF -> 等待 BUSY（注册表 LUT 短波形）。
 *
 * 与全屏模式同为 1bpp 差分刷新（N2OCP 维护旧平面），但波形更短、刷新更
 * 快。清屏（image_buf=NULL）时 DTM2=0xFF，黑像素走深擦除。仅支持全屏。
 */
static esp_err_t epd_refresh_fast(const void *image_buf) {
    esp_err_t ret;

    /* 同模式连续刷新跳过重复初始化；睡眠唤醒或模式切换时完整初始化。 */
    if (s_asleep || s_controller_mode != ESPAPERPLAY_EPD_MODE_FAST) {
        ESP_RETURN_ON_ERROR(epd_init_controller_fast(), TAG, "controller init (fast) failed");
    }

    /* 从灰阶切回黑白/快刷时重置旧平面（"旧=全白"近似）。 */
    ret = epd_prepare_bw_old_plane();
    if (ret != ESP_OK) {
        return ret;
    }

    /* DTM2（NEW）：新图像（NULL = 清屏为全白）。 */
    ret = epd_write_cmd(UC8179_CMD_DTM2);
    ESP_RETURN_ON_ERROR(ret, TAG, "DTM2 cmd failed");
    if (image_buf == NULL) {
        ret = epd_write_fill(0xFF, EPD_FRAME_BYTES);
    } else {
        ret = epd_write_data(image_buf, EPD_FRAME_BYTES);
    }
    if (ret != ESP_OK) {
        return ret;
    }

    return epd_refresh_and_wait();
}

/**
 * @brief 4 灰阶全屏刷新：DTM1=~bit0 -> DTM2=~bit1 -> DRF -> 等待 BUSY。
 *
 * 仅支持全屏（灰阶波形无法局部驱动）。清屏（image_buf=NULL）时两个平面
 * 均写 0xFF（全白 RAM）。从灰阶切回黑白时由 epd_prepare_bw_old_plane()
 * 处理旧平面。
 */
static esp_err_t epd_refresh_gray4(const void *image_buf) {
    static uint8_t s_plane[ESPAPERPLAY_EPD_SPI_MAX_TRANSFER / 2]; /* 位平面暂存 */
    const size_t chunk_bytes = sizeof(s_plane) * 2;                /* 每块输入帧字节数 */
    esp_err_t ret;

    /* 同模式连续刷新跳过重复初始化；睡眠唤醒或模式切换时完整初始化。 */
    if (s_asleep || s_controller_mode != ESPAPERPLAY_EPD_MODE_GRAY4) {
        ESP_RETURN_ON_ERROR(epd_init_controller_gray4(), TAG, "controller init (gray4) failed");
    }

    for (unsigned plane = 0; plane < 2; plane++) {
        ret = epd_write_cmd(plane == 0 ? UC8179_CMD_DTM1 : UC8179_CMD_DTM2);
        ESP_RETURN_ON_ERROR(ret, TAG, "DTM%d cmd failed", plane + 1);
        if (image_buf == NULL) {
            ret = epd_write_fill(0xFF, EPD_FRAME_BYTES);
        } else {
            /* 注意：边界必须是 2bpp 整帧字节数（96000），不是单平面 48000。 */
            for (size_t off = 0; off < EPD_GRAY4_FRAME_BYTES && ret == ESP_OK; off += chunk_bytes) {
                size_t chunk = EPD_GRAY4_FRAME_BYTES - off;
                if (chunk > chunk_bytes) {
                    chunk = chunk_bytes;
                }
                gray4_unpack_plane((const uint8_t *)image_buf + off, chunk, s_plane, plane);
                ret = epd_write_data(s_plane, chunk / 2);
            }
        }
        if (ret != ESP_OK) {
            return ret;
        }
    }

    return epd_refresh_and_wait();
}

/* ====================================================================
 * 自检任务（仅 ESPAPERPLAY_EPD_ENABLE_SELFTEST=1 时启用）
 * ==================================================================== */

#if ESPAPERPLAY_EPD_ENABLE_SELFTEST
/**
 * @brief 生成自检图案：全白底 + 左半屏黑色（x ∈ [0, 400)）。
 *
 * @return 图案缓冲（调用方负责 free），分配失败返回 NULL。
 */
static uint8_t *epd_make_test_pattern(void) {
    uint8_t *buf = malloc(EPD_FRAME_BYTES);
    if (buf == NULL) {
        ESP_LOGE(TAG, "selftest pattern alloc failed (%u bytes)", (unsigned)EPD_FRAME_BYTES);
        return NULL;
    }
    memset(buf, 0xFF, EPD_FRAME_BYTES); /* 全白 */
    for (uint16_t y = 0; y < ESPAPERPLAY_DISPLAY_HEIGHT; y++) {
        memset(buf + (size_t)y * (ESPAPERPLAY_DISPLAY_WIDTH / 8), 0x00, 400 / 8);
    }
    return buf;
}

/**
 * @brief 自检任务（性能测试）：全屏清白 -> 全屏图案 -> 局部（两个方向）
 *        -> 4 灰阶色带 -> 局刷大小对照 -> 刷成全白 -> 空闲自动睡眠验证
 *        -> 睡眠。
 *
 * 对全刷（FULL）与局刷（PARTIAL）计时；局刷含 80x80 与 400x480 大小
 * 对照（实测结论：固定波形 ~350ms 主导耗时，区域大小仅贡献 0~150ms，
 * PTOUT 在 DRF 后为默认顺序）；4 灰阶步骤仅作功能验证。空闲自动睡眠
 * 保底验证：临时 5s 超时等待自动入睡，再刷新一次验证自动唤醒。测试
 * 结束后把面板刷成全白再进入睡眠。接入正式 UI 前应将
 * ESPAPERPLAY_EPD_ENABLE_SELFTEST 置 0。
 */
static void epd_selftest_task(void *arg) {
    (void)arg;
    int64_t t0; /* 刷新计时起点（esp_timer_get_time，us） */
    const uint16_t box_x = 96;  /* 8 对齐 */
    const uint16_t box_y = 190;
    const uint16_t box_w = 80;
    const uint16_t box_h = 80;
    const uint16_t bx2_x = 496; /* 8 对齐（496/8=62） */
    const uint16_t bx2_y = 80;
    const uint16_t bx2_w = 64;
    const uint16_t bx2_h = 64;
    uint8_t *pattern = NULL;
    uint8_t *box = NULL;
    uint8_t *box2 = NULL;
    uint8_t *gray = NULL;
    esp_err_t ret;

    ESP_LOGI(TAG, "EPD selftest started (perf: FULL / PARTIAL / FAST)");
    vTaskDelay(pdMS_TO_TICKS(2000)); /* 等待系统启动完成 */

    /* 1. 全屏清白（FULL 模式，清屏样本）。 */
    t0 = esp_timer_get_time();
    ret = espaperplay_epd_refresh(NULL, 0, 0, ESPAPERPLAY_DISPLAY_WIDTH, ESPAPERPLAY_DISPLAY_HEIGHT,
                                  ESPAPERPLAY_EPD_MODE_FULL);
    ESP_LOGI(TAG, "selftest: FULL clear -> %s (%lld ms)", esp_err_to_name(ret),
             (esp_timer_get_time() - t0) / 1000);
    if (ret != ESP_OK) {
        goto out;
    }
    vTaskDelay(pdMS_TO_TICKS(3000));

    /* 2. 全屏测试图案（FULL 模式，左半屏黑）。 */
    pattern = epd_make_test_pattern();
    if (pattern == NULL) {
        goto out;
    }
    t0 = esp_timer_get_time();
    ret = espaperplay_epd_refresh(pattern, 0, 0, ESPAPERPLAY_DISPLAY_WIDTH,
                                  ESPAPERPLAY_DISPLAY_HEIGHT, ESPAPERPLAY_EPD_MODE_FULL);
    ESP_LOGI(TAG, "selftest: FULL pattern -> %s (%lld ms)", esp_err_to_name(ret),
             (esp_timer_get_time() - t0) / 1000);
    if (ret != ESP_OK) {
        goto out;
    }
    vTaskDelay(pdMS_TO_TICKS(3000));

    /* 3. 局部刷新（黑->白）：左半屏黑色区域内翻白 80x80 方块。 */
    box = malloc((size_t)box_w * box_h / 8);
    if (box == NULL) {
        ESP_LOGE(TAG, "selftest box alloc failed");
        goto out;
    }
    memset(box, 0xFF, (size_t)box_w * box_h / 8);
    t0 = esp_timer_get_time();
    ret = espaperplay_epd_refresh(box, box_x, box_y, box_w, box_h, ESPAPERPLAY_EPD_MODE_PARTIAL);
    ESP_LOGI(TAG, "selftest: PARTIAL white %ux%u@(%u,%u) -> %s (%lld ms)", box_w, box_h, box_x,
             box_y, esp_err_to_name(ret), (esp_timer_get_time() - t0) / 1000);
    if (ret != ESP_OK) {
        goto out;
    }
    vTaskDelay(pdMS_TO_TICKS(3000));

    /* 4. 局部刷新（白->黑）：右半屏白色区域画黑 64x64 方块（反向差分）。 */
    box2 = malloc((size_t)bx2_w * bx2_h / 8);
    if (box2 == NULL) {
        ESP_LOGE(TAG, "selftest box2 alloc failed");
        goto out;
    }
    memset(box2, 0x00, (size_t)bx2_w * bx2_h / 8);
    t0 = esp_timer_get_time();
    ret = espaperplay_epd_refresh(box2, bx2_x, bx2_y, bx2_w, bx2_h, ESPAPERPLAY_EPD_MODE_PARTIAL);
    ESP_LOGI(TAG, "selftest: PARTIAL black %ux%u@(%u,%u) -> %s (%lld ms)", bx2_w, bx2_h, bx2_x,
             bx2_y, esp_err_to_name(ret), (esp_timer_get_time() - t0) / 1000);
    if (ret != ESP_OK) {
        goto out;
    }
    vTaskDelay(pdMS_TO_TICKS(3000));

    /* 5. 4 灰阶：四条纵向色带（白/浅灰/深灰/黑，各 200px 宽）。 */
    gray = malloc(EPD_FRAME_BYTES * 2); /* 2bpp 整帧 */
    if (gray == NULL) {
        ESP_LOGE(TAG, "selftest gray frame alloc failed (%u bytes)", (unsigned)(EPD_FRAME_BYTES * 2));
        goto out;
    }
    memset(gray, 0x00, EPD_FRAME_BYTES * 2); /* 默认全白（灰阶值 0） */
    for (uint16_t yy = 0; yy < ESPAPERPLAY_DISPLAY_HEIGHT; yy++) {
        uint8_t *row = gray + (size_t)yy * (ESPAPERPLAY_DISPLAY_WIDTH / 4);
        memset(row + 200 / 4, 0x55, 200 / 4);   /* 浅灰带（灰阶值 1） */
        memset(row + 400 / 4, 0xAA, 200 / 4);   /* 深灰带（灰阶值 2） */
        memset(row + 600 / 4, 0xFF, 200 / 4);   /* 黑带（灰阶值 3） */
    }
    ret = espaperplay_epd_refresh(gray, 0, 0, ESPAPERPLAY_DISPLAY_WIDTH,
                                  ESPAPERPLAY_DISPLAY_HEIGHT, ESPAPERPLAY_EPD_MODE_GRAY4);
    ESP_LOGI(TAG, "selftest: gray4 bands -> %s", esp_err_to_name(ret));
    if (ret != ESP_OK) {
        goto out;
    }
    vTaskDelay(pdMS_TO_TICKS(3000));

    /* 6. 局刷大小对照：小窗口（80x80）与大窗口（400x480）各两次黑白交替，
     * 展示区域大小对刷新耗时的影响（已实测：固定波形 ~350ms 主导，大小
     * 仅贡献 0~150ms；PTOUT 已固化在 DRF 后）。 */
    {
        static const struct {
            uint16_t w, h;
        } win_sz[] = {{80, 80}, {400, 480}};
        uint8_t *winbuf = malloc(EPD_FRAME_BYTES); /* 最大窗口缓冲 */
        int dir = 0;
        if (winbuf == NULL) {
            ESP_LOGE(TAG, "selftest window buf alloc failed (%u bytes)", (unsigned)EPD_FRAME_BYTES);
            goto out;
        }
        for (size_t s = 0; s < sizeof(win_sz) / sizeof(win_sz[0]); s++) {
            const uint16_t wx = (ESPAPERPLAY_DISPLAY_WIDTH - win_sz[s].w) / 2;
            const uint16_t wy = (ESPAPERPLAY_DISPLAY_HEIGHT - win_sz[s].h) / 2;
            for (int k = 0; k < 2; k++) {
                memset(winbuf, dir ? 0x00 : 0xFF, (size_t)win_sz[s].w * win_sz[s].h / 8);
                t0 = esp_timer_get_time();
                ret = espaperplay_epd_refresh(winbuf, wx, wy, win_sz[s].w, win_sz[s].h,
                                              ESPAPERPLAY_EPD_MODE_PARTIAL);
                ESP_LOGI(TAG, "selftest: PARTIAL %ux%u %s -> %s (%lld ms)", win_sz[s].w,
                         win_sz[s].h, dir ? "black" : "white", esp_err_to_name(ret),
                         (esp_timer_get_time() - t0) / 1000);
                if (ret != ESP_OK) {
                    free(winbuf);
                    goto out;
                }
                dir = !dir; /* 黑白交替 */
                vTaskDelay(pdMS_TO_TICKS(1000));
            }
        }
        free(winbuf);
    }
    s_controller_mode = ESPAPERPLAY_EPD_MODE_MAX; /* 让最终清白走一次完整初始化 */

out:
    free(pattern);
    free(box);
    free(box2);
    free(gray);

    /* 性能测试完成：刷成全白（FULL 清屏，黑像素深擦除）。 */
    t0 = esp_timer_get_time();
    ret = espaperplay_epd_refresh(NULL, 0, 0, ESPAPERPLAY_DISPLAY_WIDTH,
                                  ESPAPERPLAY_DISPLAY_HEIGHT, ESPAPERPLAY_EPD_MODE_FULL);
    ESP_LOGI(TAG, "selftest: final white clear -> %s (%lld ms)", esp_err_to_name(ret),
             (esp_timer_get_time() - t0) / 1000);
    if (ret != ESP_OK) {
        goto sleep_out;
    }

#if ESPAPERPLAY_EPD_IDLE_SLEEP_TIMEOUT_MS > 0
    /* 7. 空闲自动睡眠保底验证：临时把超时改为 5s，等待驱动自动深度睡眠
     *    （应出现 "idle timeout: auto deep sleep" 日志），随后再刷新一次
     *    验证自动唤醒（唤醒需重新初始化，耗时应比同模式连续刷新略长）。 */
    s_idle_timeout_override_ms = 5000;
    ESP_LOGI(TAG, "selftest: waiting for idle auto-sleep (timeout 5s)");
    vTaskDelay(pdMS_TO_TICKS(8000));
    s_idle_timeout_override_ms = 0; /* 恢复正常默认超时 */
    t0 = esp_timer_get_time();
    ret = espaperplay_epd_refresh(NULL, 0, 0, ESPAPERPLAY_DISPLAY_WIDTH,
                                  ESPAPERPLAY_DISPLAY_HEIGHT, ESPAPERPLAY_EPD_MODE_FULL);
    ESP_LOGI(TAG, "selftest: wake refresh after idle sleep -> %s (%lld ms)", esp_err_to_name(ret),
             (esp_timer_get_time() - t0) / 1000);
#endif /* ESPAPERPLAY_EPD_IDLE_SLEEP_TIMEOUT_MS > 0 */

sleep_out:
    /* 自检结束，进入低功耗。 */
    ret = espaperplay_epd_sleep();
    ESP_LOGI(TAG, "selftest done, EPD sleep -> %s", esp_err_to_name(ret));

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(10000));
    }
}
#endif /* ESPAPERPLAY_EPD_ENABLE_SELFTEST */

/**
 * @brief 执行深度睡眠时序（调用方须已持有 s_lock）。
 *
 * 睡眠时序（参考工程 EPD_sleep）：CDI -> POF（等待 BUSY）-> DSLP。
 */
static esp_err_t epd_sleep_locked(void) {
    static const uint8_t cdi_sleep[1] = {UC8179_CDI_SLEEP_B0};
    const uint8_t check = UC8179_DSLP_CHECK;
    esp_err_t ret;

    ret = epd_write_cmd(UC8179_CMD_CDI);
    if (ret == ESP_OK) {
        ret = epd_write_data(cdi_sleep, sizeof(cdi_sleep));
    }
    if (ret == ESP_OK) {
        ret = epd_write_cmd(UC8179_CMD_POF);
    }
    if (ret == ESP_OK) {
        ret = epd_wait_busy();
    }
    vTaskDelay(pdMS_TO_TICKS(100)); /* POF 后至少 200us，此处取余量 */
    if (ret == ESP_OK) {
        ret = epd_write_cmd(UC8179_CMD_DSLP);
    }
    if (ret == ESP_OK) {
        ret = epd_write_data(&check, 1);
    }

    if (ret == ESP_OK) {
        s_asleep = true;
        ESP_LOGI(TAG, "EPD deep sleep");
    }
    return ret;
}

#if ESPAPERPLAY_EPD_IDLE_SLEEP_TIMEOUT_MS > 0
/**
 * @brief 空闲自动睡眠定时器回调。
 *
 * 最后一次刷新后超过 ESPAPERPLAY_EPD_IDLE_SLEEP_TIMEOUT_MS 无新刷新时，
 * 驱动自动让面板进入深度睡眠（保底：防止上层忘记睡眠导致面板长期带电）。
 * 刷新会自动唤醒（重新初始化），因此本保底不影响正确性，仅在下一次刷新
 * 时增加约 300ms 的唤醒初始化开销。
 *
 * 防误睡：记录触发时的刷新代数；若回调执行前已有新刷新（代数变化），
 * 直接退出——新刷新已重新武装定时器。
 */
static void epd_idle_sleep_cb(void *arg) {
    const uint32_t gen = s_idle_gen;
    (void)arg;

    /* 有界等待：若刷新正持锁（最长 ~2s），本轮放弃——刷新路径会递增代数
     * 并重新武装定时器，过期回调自然失效。 */
    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(50)) != pdTRUE) {
        return;
    }
    if (gen == s_idle_gen && s_initialized && !s_asleep) {
        ESP_LOGI(TAG, "idle timeout (%u ms): auto deep sleep",
                 (unsigned)(s_idle_timeout_override_ms ? s_idle_timeout_override_ms
                                                       : ESPAPERPLAY_EPD_IDLE_SLEEP_TIMEOUT_MS));
        epd_sleep_locked();
    }
    xSemaphoreGive(s_lock);
}
#endif /* ESPAPERPLAY_EPD_IDLE_SLEEP_TIMEOUT_MS > 0 */

/* ====================================================================
 * 公共 API
 * ==================================================================== */

esp_err_t espaperplay_epd_init(void) {
    esp_err_t ret;

    if (s_initialized) {
        ESP_LOGW(TAG, "already initialized");
        return ESP_OK;
    }
    if (s_lock == NULL) {
        s_lock = xSemaphoreCreateMutex();
        if (s_lock == NULL) {
            return ESP_ERR_NO_MEM;
        }
    }
    if (xSemaphoreTake(s_lock, portMAX_DELAY) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

#if ESPAPERPLAY_EPD_ENABLE_POWER_PIN
    ret = epd_enable_power();
    if (ret != ESP_OK) {
        xSemaphoreGive(s_lock);
        return ret;
    }
#endif /* ESPAPERPLAY_EPD_ENABLE_POWER_PIN */

    ret = epd_gpio_init();
    if (ret != ESP_OK) {
        xSemaphoreGive(s_lock);
        return ret;
    }

    if (s_spi_dev == NULL) {
        spi_device_interface_config_t devcfg = {
            .mode = 0, /* CPOL=0, CPHA=0：参考工程位时序（CLK 空闲低，上升沿锁存） */
            .clock_speed_hz = ESPAPERPLAY_EPD_SPI_CLK_HZ,
            .spics_io_num = ESPAPERPLAY_PIN_EPD_CS,
            .queue_size = 1,
            .flags = SPI_DEVICE_HALFDUPLEX, /* EPD 只写不读 */
        };
        ret = spi_bus_add_device((spi_host_device_t)ESPAPERPLAY_SPI_HOST_ID, &devcfg, &s_spi_dev);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "SPI device add failed: %s", esp_err_to_name(ret));
            xSemaphoreGive(s_lock);
            return ret;
        }
    }

#if ESPAPERPLAY_EPD_IDLE_SLEEP_TIMEOUT_MS > 0
    /* 空闲自动睡眠定时器（一次性，由每次刷新重新武装）。创建失败仅告警：
     * 保底失效不影响正常功能（上层仍可显式调用 espaperplay_epd_sleep()）。 */
    if (s_idle_timer == NULL) {
        const esp_timer_create_args_t idle_args = {
            .callback = epd_idle_sleep_cb,
            .arg = NULL,
            .name = "epd_idle_sleep",
        };
        ret = esp_timer_create(&idle_args, &s_idle_timer);
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "idle sleep timer create failed: %s (auto-sleep disabled)",
                     esp_err_to_name(ret));
        }
    }
#endif /* ESPAPERPLAY_EPD_IDLE_SLEEP_TIMEOUT_MS > 0 */

    /* 上电并建立基线：硬件复位 -> PSR -> PON -> 把两个图像平面清为全白。
     * 之后旧平面由 N2OCP 自动维护；本基线保证首次差分刷新有确定起点
     * （假设上电时面板为白，与 idfxx 一致）。 */
    ret = epd_hw_reset();
    if (ret != ESP_OK) {
        xSemaphoreGive(s_lock);
        return ret;
    }
    {
        const uint8_t psr = UC8179_PSR_VALUE;
        ret = epd_write_cmd(UC8179_CMD_PSR);
        if (ret == ESP_OK) {
            ret = epd_write_data(&psr, 1);
        }
    }
    if (ret == ESP_OK) {
        ret = epd_write_cmd(UC8179_CMD_PON);
    }
    if (ret == ESP_OK) {
        vTaskDelay(pdMS_TO_TICKS(5));
        ret = epd_wait_busy();
    }
    if (ret == ESP_OK) {
        /* 全屏窗口写入两个平面（0xFF = 全白）。 */
        ret = epd_window_begin(0, 0, ESPAPERPLAY_DISPLAY_WIDTH, ESPAPERPLAY_DISPLAY_HEIGHT);
    }
    if (ret == ESP_OK) {
        ret = epd_write_cmd(UC8179_CMD_DTM1);
    }
    if (ret == ESP_OK) {
        ret = epd_write_fill(0xFF, EPD_FRAME_BYTES);
    }
    if (ret == ESP_OK) {
        ret = epd_write_cmd(UC8179_CMD_DTM2);
    }
    if (ret == ESP_OK) {
        ret = epd_write_fill(0xFF, EPD_FRAME_BYTES);
    }
    if (ret == ESP_OK) {
        ret = epd_window_end();
    }
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "initial plane clear failed: %s", esp_err_to_name(ret));
        ret = ESP_OK; /* 不阻塞启动：刷新时会重新初始化 */
    }

    /* 硬件复位后进入深度睡眠（低功耗待机）；首次刷新时自动重新初始化。 */
    {
        static const uint8_t cdi_sleep[1] = {UC8179_CDI_SLEEP_B0};
        const uint8_t check = UC8179_DSLP_CHECK;
        ret = epd_write_cmd(UC8179_CMD_CDI);
        if (ret == ESP_OK) {
            ret = epd_write_data(cdi_sleep, sizeof(cdi_sleep));
        }
        if (ret == ESP_OK) {
            ret = epd_write_cmd(UC8179_CMD_POF);
        }
        if (ret == ESP_OK) {
            ret = epd_wait_busy(); /* 参考工程 EPD_sleep：POF 后等待 BUSY 释放 */
        }
        vTaskDelay(pdMS_TO_TICKS(100)); /* POF 后至少 200us，此处取余量 */
        if (ret == ESP_OK) {
            ret = epd_write_cmd(UC8179_CMD_DSLP);
        }
        if (ret == ESP_OK) {
            ret = epd_write_data(&check, 1);
        }
    }
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "initial deep sleep failed: %s (will retry on refresh)",
                 esp_err_to_name(ret));
        ret = ESP_OK; /* 不阻塞启动：刷新时会重新初始化 */
    }

    s_initialized = true;
    s_asleep = true;
    xSemaphoreGive(s_lock);

    ESP_LOGI(TAG, "EPD %ux%u initialized (SPI%d, %u Hz)", ESPAPERPLAY_DISPLAY_WIDTH,
             ESPAPERPLAY_DISPLAY_HEIGHT, (int)ESPAPERPLAY_SPI_HOST_ID, ESPAPERPLAY_EPD_SPI_CLK_HZ);

#if ESPAPERPLAY_EPD_ENABLE_SELFTEST
    xTaskCreate(epd_selftest_task, "epd_selftest", 4096, NULL, 5, NULL);
#endif /* ESPAPERPLAY_EPD_ENABLE_SELFTEST */

    return ESP_OK;
}

esp_err_t espaperplay_epd_refresh(const void *image_buf, uint16_t x, uint16_t y, uint16_t width,
                                  uint16_t height, espaperplay_epd_mode_t mode) {
    esp_err_t ret;

    if (!s_initialized) {
        ESP_LOGE(TAG, "not initialized (call espaperplay_epd_init first)");
        return ESP_ERR_INVALID_STATE;
    }
    if (mode >= ESPAPERPLAY_EPD_MODE_MAX) {
        return ESP_ERR_INVALID_ARG;
    }

    if (xSemaphoreTake(s_lock, portMAX_DELAY) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    if (mode == ESPAPERPLAY_EPD_MODE_FULL) {
        ret = epd_refresh_full(image_buf);
    } else if (mode == ESPAPERPLAY_EPD_MODE_PARTIAL) {
        /* 局部窗口参数校验：x/width 8 对齐，窗口在屏内。 */
        if ((x & 7) != 0 || (width & 7) != 0 || width == 0 || height == 0 ||
            (uint32_t)x + width > ESPAPERPLAY_DISPLAY_WIDTH ||
            (uint32_t)y + height > ESPAPERPLAY_DISPLAY_HEIGHT) {
            ESP_LOGE(TAG, "invalid partial window: x=%u y=%u w=%u h=%u", x, y, width, height);
            xSemaphoreGive(s_lock);
            return ESP_ERR_INVALID_ARG;
        }
        ret = epd_refresh_partial(image_buf, x, y, width, height);
    } else if (mode == ESPAPERPLAY_EPD_MODE_FAST) {
        /* 快刷：1bpp 整帧，注册表 LUT 短波形，仅全屏。 */
        ret = epd_refresh_fast(image_buf);
    } else { /* ESPAPERPLAY_EPD_MODE_GRAY4：2bpp 整帧，仅全屏 */
        ret = epd_refresh_gray4(image_buf);
    }

    if (ret == ESP_OK) {
        s_last_mode = mode;        /* 供灰阶<->黑白切换时重置旧平面 */
        s_controller_mode = mode;  /* 同模式连续刷新跳过重复初始化 */
        s_asleep = false;          /* 刷新完成后面板保持上电 */
    } else {
        s_controller_mode = ESPAPERPLAY_EPD_MODE_MAX; /* 状态未知，下次强制重新初始化 */
    }

#if ESPAPERPLAY_EPD_IDLE_SLEEP_TIMEOUT_MS > 0
    /* 武装空闲自动睡眠：最后一次刷新后若超时无新刷新，驱动自动深度睡眠
     * （保底机制）。在锁内递增代数并重启定时器，防止过期回调误睡眠。 */
    if (s_idle_timer != NULL) {
        s_idle_gen++;
        esp_timer_start_once(s_idle_timer,
                             (uint64_t)(s_idle_timeout_override_ms
                                            ? s_idle_timeout_override_ms
                                            : ESPAPERPLAY_EPD_IDLE_SLEEP_TIMEOUT_MS) *
                                 1000);
    }
#endif /* ESPAPERPLAY_EPD_IDLE_SLEEP_TIMEOUT_MS > 0 */

    xSemaphoreGive(s_lock);
    return ret;
}

esp_err_t espaperplay_epd_sleep(void) {
    esp_err_t ret;

    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    if (s_asleep) {
        return ESP_OK; /* 已处于深度睡眠 */
    }
    if (xSemaphoreTake(s_lock, portMAX_DELAY) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    ret = epd_sleep_locked();
    xSemaphoreGive(s_lock);
    return ret;
}

esp_err_t espaperplay_epd_power_off(void) {
    esp_err_t ret = ESP_OK;

    if (s_initialized && !s_asleep) {
        ret = espaperplay_epd_sleep();
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "sleep before power-off failed: %s", esp_err_to_name(ret));
        }
    }

#if ESPAPERPLAY_EPD_ENABLE_POWER_PIN
    gpio_set_level((gpio_num_t)ESPAPERPLAY_PIN_EPD_PWR, 0);
    ESP_LOGI(TAG, "EPD power rail disabled (GPIO%d)", ESPAPERPLAY_PIN_EPD_PWR);
#endif /* ESPAPERPLAY_EPD_ENABLE_POWER_PIN */

    s_initialized = false;
    s_asleep = true;
    return ret;
}
