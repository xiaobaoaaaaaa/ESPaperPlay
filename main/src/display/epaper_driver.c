/*
 * SPDX-FileCopyrightText: 2010-2022 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: CC0-1.0
 */

#include <inttypes.h>
#include <string.h>
#include "sdkconfig.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_lcd_panel_ssd1681.h"
#include "esp_lcd_panel_ops.h"
#include "driver/spi_common.h"
#include "driver/gpio.h"

#include "ssd1681_waveshare_1in54_lut.h"
#include "epaper_driver.h"
#include "lvgl.h"
#include "lv_demos.h"

// SPI Bus
#define EPD_PANEL_SPI_CLK           1000000
#define EPD_PANEL_SPI_CMD_BITS      8
#define EPD_PANEL_SPI_PARAM_BITS    8
#define EPD_PANEL_SPI_MODE          0
// e-Paper GPIO
#define EXAMPLE_PIN_NUM_EPD_DC      14
#define EXAMPLE_PIN_NUM_EPD_RST     12
#define EXAMPLE_PIN_NUM_EPD_CS      27
#define EXAMPLE_PIN_NUM_EPD_BUSY    13
// e-Paper SPI
#define EXAMPLE_PIN_NUM_MOSI        26
#define EXAMPLE_PIN_NUM_SCLK        25

#define EXAMPLE_LVGL_TICK_PERIOD_MS    2

#define TAG  "epaper_driver"

#ifndef MY_DISP_HOR_RES
    #define MY_DISP_HOR_RES    200
#endif

#ifndef MY_DISP_VER_RES
    #define MY_DISP_VER_RES    200
#endif

#define BYTE_PER_PIXEL (LV_COLOR_FORMAT_GET_SIZE(LV_COLOR_FORMAT_I1)) /*will be 2 for RGB565 */

lv_display_t *disp = NULL;
lv_color_t *buf1 = NULL, *buf2 = NULL;
static uint8_t *converted_buffer_black, *converted_buffer_red;
esp_lcd_panel_handle_t panel_handle = NULL;

static void disp_flush(lv_display_t * disp, const lv_area_t * area, uint8_t * px_map);

// e-Paper刷新完成回调（给LVGL通知刷新完成）
IRAM_ATTR bool epaper_flush_ready_callback(const esp_lcd_panel_handle_t handle, const void *edata, void *user_data)
{
    lv_display_flush_ready(disp);  
    BaseType_t xHigherPriorityTaskWoken = pdFALSE;
    if (xHigherPriorityTaskWoken == pdTRUE) {
        return true;
    }
    return false;
}


static void display_event_cb(lv_event_t * e)
{
    lv_area_t *area = lv_event_get_param(e);
    area->x1 = (area->x1 & ~0x7);
    area->x2 = (area->x2 | 0x7);
}

void lv_port_disp_init(void)
{
    ESP_LOGI(TAG, "Initializing LVGL display...");

    disp = lv_display_create(MY_DISP_HOR_RES, MY_DISP_VER_RES);
    if (disp == NULL) {
        ESP_LOGE(TAG, "lv_display_create failed!");
        return;
    }

    lv_display_set_flush_cb(disp, disp_flush);
    lv_display_add_event_cb(disp, display_event_cb, LV_EVENT_INVALIDATE_AREA, disp);
    size_t buf_size = MY_DISP_HOR_RES * MY_DISP_VER_RES * sizeof(lv_color_t) / 2;
    buf1 = heap_caps_malloc(buf_size, MALLOC_CAP_DMA);
    buf2 = heap_caps_malloc(buf_size, MALLOC_CAP_DMA);

    size_t conv_buf_size = MY_DISP_HOR_RES * MY_DISP_VER_RES / 8;
    converted_buffer_black = heap_caps_malloc(conv_buf_size, MALLOC_CAP_DMA);
    converted_buffer_red   = heap_caps_malloc(conv_buf_size, MALLOC_CAP_DMA);

    if (!buf1 || !buf2 || !converted_buffer_black || !converted_buffer_red) {
        ESP_LOGE(TAG, "Display buffer malloc failed! buf1=%p, buf2=%p, black=%p, red=%p", buf1, buf2, converted_buffer_black, converted_buffer_red);
        ESP_LOGE(TAG, "Free DMA heap: %u, largest free block: %u", heap_caps_get_free_size(MALLOC_CAP_DMA), heap_caps_get_largest_free_block(MALLOC_CAP_DMA));
        return;
    }

    lv_display_set_buffers(disp, buf1, buf2, buf_size, LV_DISPLAY_RENDER_MODE_PARTIAL);
}

const uint8_t bayer[4][4] = {
  {  0,  8,  2, 10},
  { 12,  4, 14,  6},
  {  3, 11,  1,  9},
  { 15,  7, 13,  5}
};
static uint8_t fast_refresh_lut[] = SSD1681_WAVESHARE_1IN54_V2_LUT_FAST_REFRESH;
static void disp_flush(lv_display_t * disp_drv, const lv_area_t * area, uint8_t * px_map)
{
    int width = area->x2 - area->x1 + 1;
    int height = area->y2 - area->y1 + 1;
    int len = width * height;
    
    memset(converted_buffer_black, 0, len / 8);
    memset(converted_buffer_red, 0, len / 8);  // 这里保留如果你后面有双色需求

    for (int y = 0; y < height; y++) {
        for (int x = 0; x < width; x++) {
            int i = y * width + x;
            uint8_t pixel = px_map[i];
            // 根据阈值矩阵做有序抖动
            uint8_t threshold = bayer[y % 4][x % 4] * 16; // 0~240
            bool bw = pixel > threshold;
            // 设置converted_buffer_black对应位
            int byte_index = i / 8;
            int bit_index = 7 - (i % 8);
            if (!bw) {
            converted_buffer_black[byte_index] |= (1 << bit_index);
            }
        }
    }


    // 发送图像到电子纸
    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(panel_handle, true));
    ESP_ERROR_CHECK(epaper_panel_set_custom_lut(panel_handle, fast_refresh_lut, 159));
    epaper_panel_set_bitmap_color(panel_handle, SSD1681_EPAPER_BITMAP_BLACK);
    esp_lcd_panel_draw_bitmap(panel_handle, area->x1, area->y1, area->x2, area->y2, converted_buffer_black);

    epaper_panel_refresh_screen(panel_handle);
    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(panel_handle, false));
}


static void increase_lvgl_tick(void *arg)
{
    /* Tell LVGL how many milliseconds has elapsed */
    lv_tick_inc(EXAMPLE_LVGL_TICK_PERIOD_MS);
}

void epaper_init(void)
{
    esp_err_t ret;
    // --- Init SPI Bus
    ESP_LOGI(TAG, "Initializing SPI Bus...");
    spi_bus_config_t buscfg = {
        .sclk_io_num = EXAMPLE_PIN_NUM_SCLK,
        .mosi_io_num = EXAMPLE_PIN_NUM_MOSI,
        .miso_io_num = -1,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = SOC_SPI_MAXIMUM_BUFFER_SIZE
    };
    ESP_ERROR_CHECK(spi_bus_initialize(SPI2_HOST, &buscfg, SPI_DMA_CH_AUTO));
    // --- Init ESP_LCD IO
    ESP_LOGI(TAG, "Initializing panel IO...");
    esp_lcd_panel_io_handle_t io_handle = NULL;
    esp_lcd_panel_io_spi_config_t io_config = {
        .dc_gpio_num = EXAMPLE_PIN_NUM_EPD_DC,
        .cs_gpio_num = EXAMPLE_PIN_NUM_EPD_CS,
        .pclk_hz = EPD_PANEL_SPI_CLK,
        .lcd_cmd_bits = EPD_PANEL_SPI_CMD_BITS,
        .lcd_param_bits = EPD_PANEL_SPI_PARAM_BITS,
        .spi_mode = EPD_PANEL_SPI_MODE,
        .trans_queue_depth = 10,
        .on_color_trans_done = NULL
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t) SPI2_HOST, &io_config, &io_handle));
    // --- Create esp_lcd panel
    ESP_LOGI(TAG, "Creating SSD1681 panel...");
    esp_lcd_ssd1681_config_t epaper_ssd1681_config = {
        .busy_gpio_num = EXAMPLE_PIN_NUM_EPD_BUSY,
        // NOTE: Enable this to reduce one buffer copy if you do not use swap-xy, mirror y or invert color
        // since those operations are not supported by ssd1681 and are implemented by software
        // Better use DMA-capable memory region, to avoid additional data copy
        .non_copy_mode = true,
    };
    esp_lcd_panel_dev_config_t panel_config = {
        .reset_gpio_num = EXAMPLE_PIN_NUM_EPD_RST,
        .flags.reset_active_high = false,
        .vendor_config = &epaper_ssd1681_config
    };
    // NOTE: Please call gpio_install_isr_service() manually before esp_lcd_new_panel_ssd1681()
    // because gpio_isr_handler_add() is called in esp_lcd_new_panel_ssd1681()
    gpio_install_isr_service(0);
    ret = esp_lcd_new_panel_ssd1681(io_handle, &panel_config, &panel_handle);
    ESP_ERROR_CHECK(ret);
    // --- Reset the display
    ESP_LOGI(TAG, "Resetting e-Paper display...");
    ESP_ERROR_CHECK(esp_lcd_panel_reset(panel_handle));
    vTaskDelay(100 / portTICK_PERIOD_MS);
    // --- Initialize LCD panel
    ESP_LOGI(TAG, "Initializing e-Paper display...");
    ESP_ERROR_CHECK(esp_lcd_panel_init(panel_handle));
    vTaskDelay(100 / portTICK_PERIOD_MS);
    // Turn on the screen
    ESP_LOGI(TAG, "Turning e-Paper display on...");
    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(panel_handle, true));
    // Set custom lut
    // NOTE: Setting custom LUT is not necessary. Panel built-in LUT is used calling after esp_lcd_panel_disp_on_off()
    // NOTE: Uncomment code below to see difference between full refresh & fast refresh
    // NOTE: epaper_panel_set_custom_lut() must be called AFTER calling esp_lcd_panel_disp_on_off()

    vTaskDelay(100 / portTICK_PERIOD_MS);

    // --- Configurate the screen
    // NOTE: the configurations below are all FALSE by default
    ESP_ERROR_CHECK(esp_lcd_panel_mirror(panel_handle, false, false));
    ESP_ERROR_CHECK(esp_lcd_panel_swap_xy(panel_handle, false));
    esp_lcd_panel_invert_color(panel_handle, false);
    // NOTE: Calling esp_lcd_panel_disp_on_off(panel_handle, true) will reset the LUT to the panel built-in one,
    // custom LUT will not take effect any more after calling esp_lcd_panel_disp_on_off(panel_handle, true)

    epaper_panel_callbacks_t cbs = {
        .on_epaper_refresh_done = epaper_flush_ready_callback,
    };
    epaper_panel_register_event_callbacks(panel_handle, &cbs, disp);
}

void lvgl_timer_task(void *param)
{
    while (1) {
        lv_timer_handler();
        vTaskDelay(pdMS_TO_TICKS(200));
    }
}

void lvgl_init_epaper_display(void)
{
    epaper_init();

    lv_init();

    lv_port_disp_init();

    // init lvgl tick
    ESP_LOGI(TAG, "Installing LVGL tick timer");
    // Tick interface for LVGL (using esp_timer to generate 2ms periodic event)
    const esp_timer_create_args_t lvgl_tick_timer_args = {
        .callback = &increase_lvgl_tick,
        .name = "lvgl_tick"
    };
    esp_timer_handle_t lvgl_tick_timer = NULL;
    ESP_ERROR_CHECK(esp_timer_create(&lvgl_tick_timer_args, &lvgl_tick_timer));
    ESP_ERROR_CHECK(esp_timer_start_periodic(lvgl_tick_timer, EXAMPLE_LVGL_TICK_PERIOD_MS * 1000));

    ESP_LOGI(TAG, "Display LVGL Meter Widget");
    lv_demo_music();

    xTaskCreatePinnedToCore(lvgl_timer_task, "lvgl_task", 8192, NULL, 5, NULL, 1);
}