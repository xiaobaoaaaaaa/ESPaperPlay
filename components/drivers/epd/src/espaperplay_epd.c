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
#include "esp_heap_caps.h"
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
#define UC8179_CMD_DTM1 0x10 /*!< Data Start Transmission 1（OLD 数据） */
#define UC8179_CMD_DRF 0x12  /*!< Display Refresh（启动刷新，等待 BUSY） */
#define UC8179_CMD_DTM2 0x13 /*!< Data Start Transmission 2（NEW 数据） */
#define UC8179_CMD_CDI 0x50  /*!< VCOM and Data Interval Setting */
#define UC8179_CMD_PTL 0x90  /*!< Partial Window（局部窗口，8 参数 + PT_SCAN） */
#define UC8179_CMD_PTIN 0x91 /*!< Partial In（进入局部模式） */
#define UC8179_CMD_PTOUT 0x92 /*!< Partial Out（退出局部模式） */
#define UC8179_CMD_CASCADE 0xE0 /*!< Cascade Setting（厂商未公开命令） */
#define UC8179_CMD_FORCE_TEMP 0xE5 /*!< Force Temperature（厂商未公开命令） */

/* ====================================================================
 * 寄存器取值（全部取自参考工程 Display_EPD_W21.c，已上板验证）
 * ==================================================================== */

/** PSR：REG=0（LUT 取自 OTP）、KW/R=1（黑白 KW 模式）、UD=1、SHL=1、
 *  SHD_N=1（升压开）、RST_N=1。 */
#define UC8179_PSR_VALUE 0x1F

/* CDI 共 2 个数据字节：
 *   字节 1：BDZ | - | BDV[1:0] | N2OCP | - | DDX[1:0]（数据极性，01=默认）
 *   字节 2：- | - | - | SDEND | CDI[3:0]（VCOM 与数据间隔，0111=10 hsync 默认）
 *
 * 极性约定：本驱动统一使用 DDX=01（规格书默认），此时数据位 1 = 白、0 = 黑，
 * 与参考工程 GUI_Paint.h 的 WHITE=0xFF / BLACK=0x00 一致。注意参考工程的
 * EPD_init_Fast 用的是 CDI=0x10（DDX=00，极性相反），其配套位图按相反极性
 * 制作；本驱动不沿用，图像数据一律按「1=白」提供。 */
#define UC8179_CDI_FULL_B0 0x21 /*!< 全屏刷新（参考工程 EPD_init，DDX=01） */
#define UC8179_CDI_PARTIAL_B0 0xA9 /*!< 局部刷新（参考工程 EPD_partial_display） */
#define UC8179_CDI_SLEEP_B0 0xF7   /*!< 睡眠前设置（参考工程 EPD_sleep） */
#define UC8179_CDI_B1 0x07         /*!< 间隔 10 hsync（默认值） */

/** BTST：升压软启动参数（参考工程 EPD_init_Fast，快速刷新模式）。 */
#define UC8179_BTST_B0 0x27
#define UC8179_BTST_B1 0x27
#define UC8179_BTST_B2 0x18
#define UC8179_BTST_B3 0x17

/** 级联设置（未公开命令，取值来自参考工程）。 */
#define UC8179_CASCADE_VALUE 0x02

/** 强制温度（未公开命令，取值来自参考工程）：全屏 0x5A，局部 0x6E。 */
#define UC8179_FORCE_TEMP_FULL 0x5A
#define UC8179_FORCE_TEMP_PARTIAL 0x6E

/** DSLP 校验码：命令仅在数据 == 0xA5 时执行。 */
#define UC8179_DSLP_CHECK 0xA5

/* ====================================================================
 * 内部状态
 * ==================================================================== */

/** 全屏 1bpp 帧缓冲字节数（800x480/8 = 48000）。 */
#define EPD_FRAME_BYTES (ESPAPERPLAY_DISPLAY_WIDTH * ESPAPERPLAY_DISPLAY_HEIGHT / 8)

static spi_device_handle_t s_spi_dev = NULL; /*!< EPD SPI 设备句柄 */
static SemaphoreHandle_t s_lock = NULL;      /*!< 刷新互斥锁（自检任务与业务可并发调用） */
static bool s_initialized = false;           /*!< init() 已完成标志 */
static bool s_asleep = true;                 /*!< 面板是否处于深度睡眠（init 后默认睡眠） */
static uint8_t *s_shadow = NULL;             /*!< 上一帧影子缓冲（实际显示内容），NULL=分配失败 */

/* ====================================================================
 * 影子缓冲（上一帧跟踪）
 * ==================================================================== */

/**
 * @brief 分配上一帧影子缓冲并初始化为全白。
 *
 * 局部/全屏刷新时 DTM1（OLD）必须反映面板上的*真实*旧内容，控制器才能按
 * {旧,新} 组合选择正确的 LUT 波形。若把 DTM1 固定为 0xFF（旧=全白），在
 * 黑底上局部画白将不会更新（{1,1}->LUTWW 保持白，物理黑像素不动）。
 *
 * 优先使用 PSRAM（8MB），失败回退内部 RAM；再失败则降级为"旧=全白"近似
 * （打印警告，局部刷新在非白背景上可能不更新）。
 *
 * 初始值 0xFF：假设上电时面板为白。首个操作若为清屏则不受影响（清屏强制
 * DTM1=0x00，全像素黑->白翻转）。
 */
static esp_err_t epd_shadow_alloc(void) {
    if (s_shadow != NULL) {
        return ESP_OK;
    }
    s_shadow = heap_caps_malloc(EPD_FRAME_BYTES, MALLOC_CAP_SPIRAM);
    if (s_shadow == NULL) {
        s_shadow = heap_caps_malloc(EPD_FRAME_BYTES, MALLOC_CAP_8BIT);
    }
    if (s_shadow == NULL) {
        ESP_LOGW(TAG,
                 "shadow framebuffer alloc failed (%u bytes); DTM1 falls back to "
                 "all-white (partial refresh on non-white background may not update)",
                 (unsigned)EPD_FRAME_BYTES);
        return ESP_ERR_NO_MEM;
    }
    memset(s_shadow, 0xFF, EPD_FRAME_BYTES); /* 假设上电时面板为全白 */
    ESP_LOGI(TAG, "shadow framebuffer ready: %u bytes", (unsigned)EPD_FRAME_BYTES);
    return ESP_OK;
}

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
 * @brief 写 len 个相同字节（用于清屏 0x00/0xFF 填充，避免分配大缓冲）。
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
 * 注：DTM1/DTM2 由各刷新流程写入。
 * ==================================================================== */

/**
 * @brief 初始化 UC8179 控制器。
 *
 * 每次刷新前都会完整执行本流程（对应参考工程在每次显示操作前的
 * EPD_init_Fast / EPD_display_init），因此睡眠唤醒无需额外调用 init。
 * DTM1/DTM2 由各刷新流程写入（见 epd_refresh_full / epd_refresh_partial）。
 *
 * @param fast  true：全屏刷新参数（CDI 0x21/0x07、BTST、强制温度 0x5A）；
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

/* ====================================================================
 * 刷新流程
 * ==================================================================== */

/**
 * @brief 全屏刷新：DTM1=上一帧（影子缓冲）-> DTM2=图像 -> DRF -> 等待 BUSY。
 */
static esp_err_t epd_refresh_full(const void *image_buf) {
    esp_err_t ret;

    ESP_RETURN_ON_ERROR(epd_init_controller(true), TAG, "controller init (full) failed");

    /* DTM1（OLD）：上一帧真实内容（影子缓冲），每个像素按 {旧,新} 组合选择
     * 正确的 LUT 波形，无残影、无漏刷。清屏时强制 0x00：全像素经历 黑->白
     * 翻转，保证面板被真正驱动为纯白（不依赖面板此前状态）。 */
    ret = epd_write_cmd(UC8179_CMD_DTM1);
    ESP_RETURN_ON_ERROR(ret, TAG, "DTM1 cmd failed");
    if (image_buf == NULL) {
        ret = epd_write_fill(0x00, EPD_FRAME_BYTES);
    } else if (s_shadow != NULL) {
        ret = epd_write_data(s_shadow, EPD_FRAME_BYTES);
    } else {
        ret = epd_write_fill(0xFF, EPD_FRAME_BYTES);
    }
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

    /* DRF：启动刷新；命令后至少等待 200us 再查询 BUSY（规格书/参考工程均要求）。 */
    ret = epd_write_cmd(UC8179_CMD_DRF);
    ESP_RETURN_ON_ERROR(ret, TAG, "DRF cmd failed");
    vTaskDelay(pdMS_TO_TICKS(1));
    ret = epd_wait_busy();
    if (ret != ESP_OK) {
        return ret;
    }

    /* 同步影子缓冲：新图像成为下一帧的"旧图像"。 */
    if (s_shadow != NULL) {
        if (image_buf == NULL) {
            memset(s_shadow, 0xFF, EPD_FRAME_BYTES);
        } else {
            memcpy(s_shadow, image_buf, EPD_FRAME_BYTES);
        }
    }
    return ESP_OK;
}

/**
 * @brief 局部刷新：CDI(0xA9) -> PTIN -> PTL(窗口) -> DTM2 -> DRF -> PTOUT。
 *
 * @note x 与 width 必须为 8 的倍数（1bpp 按字节寻址）；窗口终点取含端点
 *       （x+width-1）。参考工程对终点做了多余的 "-1"（off-by-one），本驱动
 *       不沿用；若实测出现 1 像素偏移，只需改回终点减 1。
 */
static esp_err_t epd_refresh_partial(const void *image_buf, uint16_t x, uint16_t y, uint16_t width,
                                     uint16_t height) {
    static const uint8_t cdi_partial[2] = {UC8179_CDI_PARTIAL_B0, UC8179_CDI_B1};
    const uint16_t x_end = x + width - 1;
    const uint16_t y_end = y + height - 1;
    const uint8_t ptl[9] = {
        (uint8_t)(x >> 8),      (uint8_t)(x & 0xFF),        /* HRST[9:0] */
        (uint8_t)(x_end >> 8),  (uint8_t)(x_end & 0xFF),    /* HRED[9:0] */
        (uint8_t)(y >> 8),      (uint8_t)(y & 0xFF),        /* VRST[9:0] */
        (uint8_t)(y_end >> 8),  (uint8_t)(y_end & 0xFF),    /* VRED[9:0] */
        0x01,                                               /* PT_SCAN=1：全面板扫描（默认） */
    };
    const size_t window_bytes = (size_t)width * height / 8;
    esp_err_t ret;

    ESP_RETURN_ON_ERROR(epd_init_controller(false), TAG, "controller init (partial) failed");

    /* VCOM/数据间隔：局部刷新取值（参考工程 EPD_partial_display）。 */
    ret = epd_write_cmd(UC8179_CMD_CDI);
    ESP_RETURN_ON_ERROR(ret, TAG, "CDI cmd failed");
    ret = epd_write_data(cdi_partial, sizeof(cdi_partial));
    ESP_RETURN_ON_ERROR(ret, TAG, "CDI data failed");

    /* 进入局部模式并设置窗口。 */
    ret = epd_write_cmd(UC8179_CMD_PTIN);
    ESP_RETURN_ON_ERROR(ret, TAG, "PTIN cmd failed");
    ret = epd_write_cmd(UC8179_CMD_PTL);
    ESP_RETURN_ON_ERROR(ret, TAG, "PTL cmd failed");
    ret = epd_write_data(ptl, sizeof(ptl));
    ESP_RETURN_ON_ERROR(ret, TAG, "PTL data failed");

    /* 窗口内 DTM1（OLD）：上一帧窗口内容（影子缓冲），黑底画白/白底画黑
     * 均正确更新。清屏窗口时强制 0x00（黑->白翻转），保证窗口真正变白。 */
    ret = epd_write_cmd(UC8179_CMD_DTM1);
    ESP_RETURN_ON_ERROR(ret, TAG, "DTM1 cmd failed");
    if (image_buf == NULL) {
        ret = epd_write_fill(0x00, window_bytes);
    } else if (s_shadow != NULL) {
        const uint8_t *old = s_shadow + (size_t)y * (ESPAPERPLAY_DISPLAY_WIDTH / 8) + x / 8;
        ret = epd_write_data(old, window_bytes);
    } else {
        ret = epd_write_fill(0xFF, window_bytes);
    }
    if (ret != ESP_OK) {
        return ret;
    }

    /* 窗口内 DTM2（NEW）：新数据（NULL = 窗口清白）。 */
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

    /* 刷新，等待完成，退出局部模式。 */
    ret = epd_write_cmd(UC8179_CMD_DRF);
    ESP_RETURN_ON_ERROR(ret, TAG, "DRF cmd failed");
    vTaskDelay(pdMS_TO_TICKS(1));
    ret = epd_wait_busy();
    if (ret != ESP_OK) {
        return ret;
    }
    ret = epd_write_cmd(UC8179_CMD_PTOUT);
    ESP_RETURN_ON_ERROR(ret, TAG, "PTOUT cmd failed");

    /* 同步影子缓冲窗口区域。 */
    if (s_shadow != NULL) {
        uint8_t *dst = s_shadow + (size_t)y * (ESPAPERPLAY_DISPLAY_WIDTH / 8) + x / 8;
        if (image_buf == NULL) {
            memset(dst, 0xFF, window_bytes);
        } else {
            memcpy(dst, image_buf, window_bytes);
        }
    }
    return ESP_OK;
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
 * @brief 自检任务：全屏清白 -> 全屏测试图案 -> 局部刷新（黑底翻白块）。
 *
 * 用于上电验收：依次验证全屏刷新、局部窗口刷新与睡眠流程，全部完成后
 * 转为空闲轮询。接入正式 UI 前应将 ESPAPERPLAY_EPD_ENABLE_SELFTEST 置 0。
 */
static void epd_selftest_task(void *arg) {
    (void)arg;
    const uint16_t box_x = 96;  /* 8 对齐 */
    const uint16_t box_y = 190;
    const uint16_t box_w = 80;
    const uint16_t box_h = 80;
    uint8_t *pattern = NULL;
    uint8_t *box = NULL;
    esp_err_t ret;

    ESP_LOGI(TAG, "EPD selftest started");
    vTaskDelay(pdMS_TO_TICKS(2000)); /* 等待系统启动完成 */

    /* 1. 全屏清白。 */
    ret = espaperplay_epd_refresh(NULL, 0, 0, ESPAPERPLAY_DISPLAY_WIDTH, ESPAPERPLAY_DISPLAY_HEIGHT,
                                  ESPAPERPLAY_EPD_MODE_FULL);
    ESP_LOGI(TAG, "selftest: full clear -> %s", esp_err_to_name(ret));
    if (ret != ESP_OK) {
        goto out;
    }
    vTaskDelay(pdMS_TO_TICKS(3000));

    /* 2. 全屏测试图案（左半屏黑）。 */
    pattern = epd_make_test_pattern();
    if (pattern == NULL) {
        goto out;
    }
    ret = espaperplay_epd_refresh(pattern, 0, 0, ESPAPERPLAY_DISPLAY_WIDTH,
                                  ESPAPERPLAY_DISPLAY_HEIGHT, ESPAPERPLAY_EPD_MODE_FULL);
    ESP_LOGI(TAG, "selftest: full pattern -> %s", esp_err_to_name(ret));
    if (ret != ESP_OK) {
        goto out;
    }
    vTaskDelay(pdMS_TO_TICKS(3000));

    /* 3. 局部刷新：左半屏黑色区域内翻转一个 80x80 白色方块。 */
    box = malloc((size_t)box_w * box_h / 8);
    if (box == NULL) {
        ESP_LOGE(TAG, "selftest box alloc failed");
        goto out;
    }
    memset(box, 0xFF, (size_t)box_w * box_h / 8);
    ret = espaperplay_epd_refresh(box, box_x, box_y, box_w, box_h, ESPAPERPLAY_EPD_MODE_PARTIAL);
    ESP_LOGI(TAG, "selftest: partial %ux%u@(%u,%u) -> %s", box_w, box_h, box_x, box_y,
             esp_err_to_name(ret));
    if (ret != ESP_OK) {
        goto out;
    }
    vTaskDelay(pdMS_TO_TICKS(3000));

out:
    free(pattern);
    free(box);
    /* 自检结束，进入低功耗。 */
    ret = espaperplay_epd_sleep();
    ESP_LOGI(TAG, "selftest done, EPD sleep -> %s", esp_err_to_name(ret));

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(10000));
    }
}
#endif /* ESPAPERPLAY_EPD_ENABLE_SELFTEST */

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

    /* 影子缓冲（跟踪实际显示内容，供 DTM1 使用）；分配失败自动降级。 */
    epd_shadow_alloc();

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

    /* 硬件复位后进入深度睡眠（低功耗待机）；首次刷新时自动重新初始化。 */
    ret = epd_hw_reset();
    if (ret != ESP_OK) {
        xSemaphoreGive(s_lock);
        return ret;
    }
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
    } else {
        /* 局部窗口参数校验：x/width 8 对齐，窗口在屏内。 */
        if ((x & 7) != 0 || (width & 7) != 0 || width == 0 || height == 0 ||
            (uint32_t)x + width > ESPAPERPLAY_DISPLAY_WIDTH ||
            (uint32_t)y + height > ESPAPERPLAY_DISPLAY_HEIGHT) {
            ESP_LOGE(TAG, "invalid partial window: x=%u y=%u w=%u h=%u", x, y, width, height);
            xSemaphoreGive(s_lock);
            return ESP_ERR_INVALID_ARG;
        }
        ret = epd_refresh_partial(image_buf, x, y, width, height);
    }

    s_asleep = false; /* 刷新完成后面板保持上电，由调用方决定何时睡眠 */
    xSemaphoreGive(s_lock);
    return ret;
}

esp_err_t espaperplay_epd_sleep(void) {
    static const uint8_t cdi_sleep[1] = {UC8179_CDI_SLEEP_B0};
    const uint8_t check = UC8179_DSLP_CHECK;
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

    /* 睡眠时序（参考工程 EPD_sleep）：CDI -> POF（等待 BUSY）-> DSLP。 */
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
