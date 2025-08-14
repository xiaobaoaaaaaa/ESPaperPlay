#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"

#include "esp_log.h"
#include "esp_timer.h"

#include "epaper_driver.h"
#include "ssd1681_waveshare_1in54_lut.h"

#include "lv_demos.h"
#include "lvgl_init.h"

#include "power_save.h"
#include "touch.h"
#include "ui.h"

#define TAG "lvgl_init"

#define EXAMPLE_LVGL_TICK_PERIOD_MS    50
#define MAX_PARTIAL_REFRESH_COUNT 30

#ifndef MY_DISP_HOR_RES
    #define MY_DISP_HOR_RES    200
#endif

#ifndef MY_DISP_VER_RES
    #define MY_DISP_VER_RES    200
#endif

#define BYTE_PER_PIXEL (LV_COLOR_FORMAT_GET_SIZE(LV_COLOR_FORMAT_I1))

lv_display_t *disp = NULL;
lv_indev_t *indev_touchpad = NULL;
void *buf1 = NULL, *buf2 = NULL;
EventGroupHandle_t lvgl_flush_event_group = NULL;

int fast_refresh_count = 0;
static void disp_flush(lv_display_t * disp_drv, const lv_area_t * area, uint8_t * px_map)
{
    int height = area->y2 - area->y1 + 1;
    int width = area->x2 - area->x1 + 1;

    size_t size = height * width / 8;
    uint8_t *buf = px_map + 8; // px_map的前8字节是LVGL固定头部，不应被渲染

    uint8_t inverted[size]; // 屏幕驱动芯片的红色寄存器与黑白寄存器写入相反数据（黑白双色屏幕）
    for (unsigned i = 0; i < size; i++) {
        inverted[i] = ~buf[i];
    }

    esp_lcd_panel_disp_on_off(panel_handle, true);

    if (fast_refresh_count < MAX_PARTIAL_REFRESH_COUNT) {
        epaper_panel_set_refresh_mode(panel_handle, true);
        fast_refresh_count++;
    } else {
        epaper_panel_set_refresh_mode(panel_handle, false);
        fast_refresh_count = 0;
    }

    ESP_LOGI(TAG, "Flushing area: x1=%d, y1=%d, x2=%d, y2=%d", 
             area->x1, area->y1, area->x2, area->y2);  // Fixed format specifiers

    epaper_panel_set_bitmap_color(panel_handle, SSD1681_EPAPER_BITMAP_BLACK);
    esp_lcd_panel_draw_bitmap(panel_handle, area->x1, area->y1, area->x2 + 1, area->y2 + 1, buf);

    epaper_panel_set_bitmap_color(panel_handle, SSD1681_EPAPER_BITMAP_RED);
    esp_lcd_panel_draw_bitmap(panel_handle, area->x1, area->y1, area->x2 + 1, area->y2 + 1, inverted);

    epaper_panel_refresh_screen(panel_handle);
    esp_lcd_panel_disp_on_off(panel_handle, false);
}

void lv_port_disp_init(void)
{
    ESP_LOGI(TAG, "Initializing LVGL display");

    disp = lv_display_create(MY_DISP_HOR_RES, MY_DISP_VER_RES);
    if (disp == NULL) {
        ESP_LOGE(TAG, "Display creation failed");
        return;
    }

    lv_display_set_flush_cb(disp, disp_flush);
    size_t buf_size = MY_DISP_HOR_RES * MY_DISP_VER_RES / 8 + 8; // 屏幕大小除以8以保存全部像素，额外申请8字节用于lvgl添加的固定头部
    buf1 = heap_caps_malloc(buf_size, MALLOC_CAP_DMA);

    if (!buf1) {
        ESP_LOGE(TAG, "Display buffer allocation failed");
        return;
    }

    void *aligned_buf = lv_draw_buf_align(buf1, LV_COLOR_FORMAT_I1);

    lv_display_set_buffers(disp, buf1, NULL, buf_size, LV_DISPLAY_RENDER_MODE_PARTIAL);
}

static void increase_lvgl_tick(void *arg)
{
    lv_tick_inc(EXAMPLE_LVGL_TICK_PERIOD_MS);
}

void lvgl_timer_task(void *param)
{
    while (1) {
        EventBits_t bits = xEventGroupWaitBits(lvgl_flush_event_group, BIT0, 
                         pdFALSE, pdFALSE, pdMS_TO_TICKS(500));

        if (bits & BIT0) {
            lv_timer_handler();
            ui_tick();
        }

        vTaskDelay(pdMS_TO_TICKS(EXAMPLE_LVGL_TICK_PERIOD_MS));
    }
}

typedef struct {
    int32_t x;
    int32_t y;
    bool is_pressed;
    SemaphoreHandle_t mutex;
} touch_data_t;

static touch_data_t g_touch_data = {
    .x = 0,
    .y = 0,
    .is_pressed = false,
    .mutex = NULL
};

static void touch_read_task(void *param)
{
    while (1) {
        ctp_tp_t ctp;
        i2c_ctp_FTxxxx_read_all(I2C_NUM_0, &ctp);

        xSemaphoreTake(g_touch_data.mutex, portMAX_DELAY);
        
        if (ctp.tp_num > 0) {
            int32_t x = ((int16_t)(ctp.tp[0].x - 160) - 319) * -1;
            int32_t y = ctp.tp[0].y;
            
            if (x < 200 && y < 200) {
                g_touch_data.x = x;
                g_touch_data.y = y;
                g_touch_data.is_pressed = true;
                reset_inactivity_timer();
            } else {
                g_touch_data.is_pressed = false;
            }
        } else {
            g_touch_data.is_pressed = false;
        }
        
        xSemaphoreGive(g_touch_data.mutex);
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

static void touchpad_read(lv_indev_t * indev_drv, lv_indev_data_t * data)
{
    xSemaphoreTake(g_touch_data.mutex, portMAX_DELAY);
    
    data->state = g_touch_data.is_pressed ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
    data->point.x = g_touch_data.x;
    data->point.y = g_touch_data.y;
    
    xSemaphoreGive(g_touch_data.mutex);
}

void lv_port_indev_init(void)
{
    sd_touch_init();

    g_touch_data.mutex = xSemaphoreCreateMutex();
    if (g_touch_data.mutex == NULL) {
        ESP_LOGE(TAG, "Touch mutex creation failed");
        return;
    }

    xTaskCreatePinnedToCore(touch_read_task, "touch_task", 2048, NULL, 5, NULL, 0);

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

    ESP_LOGI(TAG, "Setting up LVGL tick timer");
    const esp_timer_create_args_t lvgl_tick_timer_args = {
        .callback = &increase_lvgl_tick,
        .name = "lvgl_tick"
    };
    esp_timer_handle_t lvgl_tick_timer = NULL;
    ESP_ERROR_CHECK(esp_timer_create(&lvgl_tick_timer_args, &lvgl_tick_timer));
    ESP_ERROR_CHECK(esp_timer_start_periodic(lvgl_tick_timer, EXAMPLE_LVGL_TICK_PERIOD_MS * 1000));

    ui_init();
    xTaskCreatePinnedToCore(lvgl_timer_task, "lvgl_task", 8192, NULL, 10, NULL, 1);
}