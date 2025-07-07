#include "lvgl_init.h"
#include "epaper_driver.h"
#include "esp_timer.h"
#include "esp_lcd_panel_ssd1681.h"
#include "ssd1681_waveshare_1in54_lut.h"
#include "esp_log.h"
#include <string.h>
#include "lv_demos.h"

#define TAG "lvgl_init"

#define EXAMPLE_LVGL_TICK_PERIOD_MS    2

#ifndef MY_DISP_HOR_RES
    #define MY_DISP_HOR_RES    200
#endif

#ifndef MY_DISP_VER_RES
    #define MY_DISP_VER_RES    203
#endif

#define BYTE_PER_PIXEL (LV_COLOR_FORMAT_GET_SIZE(LV_COLOR_FORMAT_I1)) /*will be 2 for RGB565 */

lv_display_t *disp = NULL;
lv_color_t *buf1 = NULL, *buf2 = NULL;
static uint8_t *converted_buffer_black, *converted_buffer_red;


static uint8_t fast_refresh_lut[] = SSD1681_WAVESHARE_1IN54_V2_LUT_FAST_REFRESH_KEEP;
static void disp_flush(lv_display_t * disp_drv, const lv_area_t * area, uint8_t * px_map)
{
    ESP_LOGI(TAG, "Flush area: (%ld, %ld) - (%ld, %ld)", area->x1, area->y1, area->x2, area->y2);
    int width = area->x2 - area->x1 + 1;
    int height = area->y2 - area->y1 + 1;
    int len = width * height;
    int total_bytes = (len + 7) / 8;

    memset(converted_buffer_black, 0, total_bytes);

    for (int y = 0; y < height; y++) {
        for (int x = 0; x < width; x++) {
            int i = y * width + x; // 像素在 px_map 中的线性索引
            int byte_index = i / 8;
            int bit_index = 7 - (i % 8);
            bool pixel_on = (px_map[byte_index] >> bit_index) & 0x1;

            if (pixel_on) {
                int dst_byte_index = i / 8;
                int dst_bit_index = 7 - (i % 8);

                // 将字节向前循环移动 8 位
                int shifted_index = (dst_byte_index + total_bytes - 8) % total_bytes;

                converted_buffer_black[shifted_index] |= (1 << dst_bit_index);
            }
        }
    }

    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(panel_handle, true));
    ESP_ERROR_CHECK(epaper_panel_set_custom_lut(panel_handle, fast_refresh_lut, 159));

    epaper_panel_set_bitmap_color(panel_handle, SSD1681_EPAPER_BITMAP_BLACK);
    esp_lcd_panel_draw_bitmap(panel_handle, area->x1, area->y1, area->x2, area->y2, converted_buffer_black);

    epaper_panel_set_bitmap_color(panel_handle, SSD1681_EPAPER_BITMAP_RED);
    esp_lcd_panel_draw_bitmap(panel_handle, area->x1, area->y1, area->x2, area->y2, converted_buffer_black);

    epaper_panel_refresh_screen(panel_handle);

    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(panel_handle, false));
}

static void display_event_cb(lv_event_t * e)
{
    lv_area_t *area = lv_event_get_param(e);
     // 调整x1到当前块的起始（8的倍数）
    area->x1 = (area->x1 / 8) * 8;
    // 调整x2到下一个块的末尾（8的倍数减1）
    area->x2 = ((area->x2 / 8) + 1) * 8 - 1;
    
    // 确保不超出屏幕右边界
    if(area->x2 >= 200) {
        area->x2 = 200 - 1;
    }
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
    size_t buf_size = MY_DISP_HOR_RES * MY_DISP_VER_RES * sizeof(lv_color_t) / 8;
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

static void increase_lvgl_tick(void *arg)
{
    /* Tell LVGL how many milliseconds has elapsed */
    lv_tick_inc(EXAMPLE_LVGL_TICK_PERIOD_MS);
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