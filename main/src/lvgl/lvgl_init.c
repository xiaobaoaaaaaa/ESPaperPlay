#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "lvgl_init.h"
#include "epaper_driver.h"
#include "esp_timer.h"
#include "ssd1681_waveshare_1in54_lut.h"
#include "esp_log.h"
#include <string.h>
#include "lv_demos.h"
#include "touch.h"
#include "ui.h"
#include "power_save.h"

#define TAG "lvgl_init"

#define EXAMPLE_LVGL_TICK_PERIOD_MS    50
#define MAX_PARTIAL_REFRESH_COUNT 30

#ifndef MY_DISP_HOR_RES
    #define MY_DISP_HOR_RES    200
#endif

#ifndef MY_DISP_VER_RES
    #define MY_DISP_VER_RES    200
#endif

#define BYTE_PER_PIXEL (LV_COLOR_FORMAT_GET_SIZE(LV_COLOR_FORMAT_I1)) /*will be 2 for RGB565 */

lv_display_t *disp = NULL;
lv_indev_t *indev_touchpad = NULL;
lv_color_t *buf1 = NULL, *buf2 = NULL;
EventGroupHandle_t lvgl_flush_event_group = NULL;

int fast_refresh_count = 0;
static void disp_flush(lv_display_t * disp_drv, const lv_area_t * area, uint8_t * px_map)
{

    int height = area->y2 - area->y1 + 1;
    int width = area->x2 - area->x1 + 1;

    // px_map 总字节数
    size_t size = height * width / 8;

    memmove(px_map, px_map + 8, size);

    uint8_t inverted[size];
    for (unsigned i = 0; i < size; i++) {
        inverted[i] = ~px_map[i];
    }

    esp_lcd_panel_disp_on_off(panel_handle, true);

    if(fast_refresh_count < MAX_PARTIAL_REFRESH_COUNT)
    {
        epaper_panel_set_refresh_mode(panel_handle, true);
        fast_refresh_count++;
    }
    else
    {
        epaper_panel_set_refresh_mode(panel_handle, false);
        fast_refresh_count = 0;
    }

    ESP_LOGI(TAG, "Flushing area: x1=%ld, y1=%ld, x2=%ld, y2=%ld", area->x1, area->y1, area->x2, area->y2);

    epaper_panel_set_bitmap_color(panel_handle, SSD1681_EPAPER_BITMAP_BLACK);
    esp_lcd_panel_draw_bitmap(panel_handle, area->x1, area->y1, area->x2 + 1, area->y2 + 1, px_map);

    epaper_panel_set_bitmap_color(panel_handle, SSD1681_EPAPER_BITMAP_RED);
    esp_lcd_panel_draw_bitmap(panel_handle, area->x1, area->y1, area->x2 + 1, area->y2 + 1, inverted);

    epaper_panel_refresh_screen(panel_handle);
    esp_lcd_panel_disp_on_off(panel_handle, false);
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
    //lv_display_add_event_cb(disp, display_event_cb, LV_EVENT_INVALIDATE_AREA, disp);
    size_t buf_size = MY_DISP_HOR_RES * MY_DISP_VER_RES * sizeof(lv_color_t) / 8;
    buf1 = heap_caps_malloc(buf_size, MALLOC_CAP_DMA);
    buf2 = heap_caps_malloc(buf_size, MALLOC_CAP_DMA);

    if (!buf1 || !buf2) {
        ESP_LOGE(TAG, "Display buffer malloc failed! buf1=%p, buf2=%p", buf1, buf2);
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
    while (1) 
    {
        xEventGroupWaitBits(lvgl_flush_event_group, BIT0, pdFALSE, pdFALSE, portMAX_DELAY);
        lv_timer_handler();
        ui_tick(); 
        vTaskDelay(pdMS_TO_TICKS(50));
    }
}

static void touchpad_read(lv_indev_t * indev_drv, lv_indev_data_t * data)
{
    static int32_t last_x = 0;
    static int32_t last_y = 0;

    ctp_tp_t ctp;
    i2c_ctp_FTxxxx_read_all(I2C_NUM_0, &ctp);

    /*Save the pressed coordinates and the state*/
    if( ctp.tp_num > 0) {
        ctp.tp[0].x = ((int16_t)(ctp.tp[0].x - 160) - 319) * -1; // 去除`FT6236U`触摸屏的x坐标固有偏移，再对屏幕倒立的x轴进行补偿
        last_x = ctp.tp[0].x;
        last_y = ctp.tp[0].y;
        if(last_x < 200 && last_y < 200) 
        {
            data->state = LV_INDEV_STATE_PRESSED;
            reset_inactivity_timer();
        }
        else 
        {
            data->state = LV_INDEV_STATE_RELEASED;
        }
    }
    else {
        data->state = LV_INDEV_STATE_RELEASED;
    }

    /*Set the last pressed coordinates*/
    data->point.x = last_x;
    data->point.y = last_y;
}

void lv_port_indev_init(void)
{
    sd_touch_init();

    /*Register a touchpad input device*/
    indev_touchpad = lv_indev_create();
    lv_indev_set_type(indev_touchpad, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(indev_touchpad, touchpad_read);
}

void lvgl_init_epaper_display(void)
{
    lvgl_flush_event_group = xEventGroupCreate();
    xEventGroupSetBits(lvgl_flush_event_group, BIT0);

    epaper_init();

    lv_init();

    lv_port_disp_init();

    lv_port_indev_init();

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

    ui_init();

    xTaskCreatePinnedToCore(lvgl_timer_task, "lvgl_task", 8192, NULL, 5, NULL, 1);
}