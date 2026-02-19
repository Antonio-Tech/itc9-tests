/*
 * ESPRESSIF MIT License
 *
 * Copyright (c) 2021 <ESPRESSIF SYSTEMS (SHANGHAI) CO., LTD>
 *
 * Permission is hereby granted for use on all ESPRESSIF SYSTEMS products, in which case,
 * it is free of charge, to any person obtaining a copy of this software and associated
 * documentation files (the "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the Software is furnished
 * to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all copies or
 * substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS
 * FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR
 * COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER
 * IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 * CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 *
 */

#include "board.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lv_port.h"
#include "lvgl.h"
#include "tt21100.h"
#include "esp_jpeg_dec.h"
#include "esp_jpeg_common.h"
#include "esp_heap_caps.h"
#include "lv_decoders.h"
#include "s3_definitions.h"
#include <stdio.h>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>

#ifndef LCD_H_RES
#define LCD_H_RES 240
#endif

#ifndef LCD_V_RES
#define LCD_V_RES 240
#endif

typedef enum {
    TP_VENDOR_NONE = -1,
    TP_VENDOR_TT = 0,
    TP_VENDOR_FT,
    TP_VENDOR_MAX,
} tp_vendor_t;

static lv_disp_drv_t disp_drv;
static const char *TAG = "lv_port";
static esp_lcd_panel_handle_t panel_handle;

static void *p_user_data = NULL;
static bool (*p_on_trans_done_cb)(void *) = NULL;
static SemaphoreHandle_t lcd_flush_done_sem = NULL;
bool lcd_trans_done_cb(esp_lcd_panel_io_handle_t, esp_lcd_panel_io_event_data_t *, void *);
static tp_vendor_t tp_vendor = TP_VENDOR_TT;

static void lv_tick_inc_cb(void *data)
{
    uint32_t tick_inc_period_ms = *((uint32_t *) data);
    lv_tick_inc(tick_inc_period_ms);
}

static bool lv_port_flush_ready(void *arg)
{
    /* Inform the graphics library that you are ready with the flushing */
    lv_disp_flush_ready(&disp_drv);
    /* portYIELD_FROM_ISR (true) or not (false). */
    return false;
}

static esp_err_t touch_ic_read(uint8_t *tp_num, uint16_t *x, uint16_t *y, uint8_t *btn_val)
{
    esp_err_t ret_val = ESP_OK;
    uint16_t btn_signal = 0;

    switch (tp_vendor) {
//        case TP_VENDOR_TT:
//            ret_val |= tt21100_tp_read();
//            ret_val |= tt21100_get_touch_point(tp_num, x, y);
//            ret_val |= tt21100_get_btn_val(btn_val, &btn_signal);
//            break;
        case TP_VENDOR_FT:
            break;
        default:
            return ESP_ERR_NOT_FOUND;
            break;
    }

#if TOUCH_PANEL_SWAP_XY
    uint16_t swap = *x;
    *x = *y;
    *y = swap;
#endif

#if TOUCH_PANEL_INVERSE_X
    *x = LCD_H_RES - ( *x + 1);
#endif

#if TOUCH_PANEL_INVERSE_Y
    *y = LCD_V_RES - (*y + 1);
#endif

    ESP_LOGV(TAG, "[%3u, %3u]", *x, *y);
    return ret_val;
}

static void button_read(lv_indev_drv_t *indev_drv, lv_indev_data_t *data)
{
    static uint8_t prev_btn_id = 0;
    uint8_t tp_num = 0, btn_val = 0;
    uint16_t x = 0, y = 0;
    /* Read touch point(s) via touch IC */
    if (ESP_OK != touch_ic_read(&tp_num, &x, &y, &btn_val)) {
        return;
    }

    /*Get the pressed button's ID*/
    if (btn_val) {
        data->btn_id = btn_val;
        data->state = LV_INDEV_STATE_PRESSED;
    } else {
        data->btn_id = 0;
        data->state = LV_INDEV_STATE_RELEASED;
    }

    if (prev_btn_id != data->btn_id) {
        lv_event_send(lv_scr_act(), LV_EVENT_HIT_TEST, (void *) (int)btn_val);
    }

    prev_btn_id = btn_val;
}

static IRAM_ATTR void touchpad_read(lv_indev_drv_t *indev_drv, lv_indev_data_t *data)
{
    uint8_t tp_num = 0, btn_val = 0;
    uint16_t x = 0, y = 0;
    /* Read touch point(s) via touch IC */
    if (ESP_OK != touch_ic_read(&tp_num, &x, &y, &btn_val)) {
        return;
    }

    ESP_LOGV(TAG, "Touch (%u) : [%3u, %3u]", tp_num, x, y);

    /* FT series touch IC might return 0xff before first touch. */
    if ((0 == tp_num) || (5 < tp_num)) {
        data->state = LV_INDEV_STATE_REL;
    } else {
        data->point.x = x;
        data->point.y = y;
        data->state = LV_INDEV_STATE_PR;
    }
}

static void disp_flush(lv_disp_drv_t *disp_drv, const lv_area_t *area, lv_color_t *color_p)
{
    (void) disp_drv;
    /* Wait for previous tansmition done */
    if (pdPASS != xSemaphoreTake(lcd_flush_done_sem, portMAX_DELAY)) {
        return;
    }
    ESP_LOGD(TAG, "x:%d,y:%d", area->x2 + 1 - area->x1, area->y2 + 1 - area->y1);
    /*The most simple case (but also the slowest) to put all pixels to the screen one-by-one*/
    ESP_ERROR_CHECK(esp_lcd_panel_draw_bitmap(panel_handle, area->x1, area->y1, area->x2 + 1, area->y2 + 1, (uint8_t *) color_p));
}

#define USING_STATIC_DISP_BUF 0
static void lv_port_disp_init(void)
{
    static lv_disp_draw_buf_t draw_buf_dsc;

    /* Option 2 : Using static space for display buffer */
 #if  USING_STATIC_DISP_BUF
    size_t disp_buf_height = LCD_V_RES / 2;
    static lv_color_t p_disp_buf[LCD_H_RES * (LCD_V_RES / 2)];
#else
    // Double buffering with 20 lines to eliminate black flash during transitions
    // Each buffer: 20 lines (240x20x2 = 9.6KB), Total: 19.2KB (~7% DMA)
    // Reduces screen flushes from 24 to 12 (2x faster rendering)
    size_t disp_buf_height = 20;
    static lv_color_t * p_disp_buf1 = NULL;
    static lv_color_t * p_disp_buf2 = NULL;

    // Allocate first buffer (DMA capable for SPI transfer)
    p_disp_buf1 = heap_caps_malloc(LCD_H_RES * disp_buf_height * sizeof(lv_color_t), MALLOC_CAP_DMA);
    if (!p_disp_buf1) {
        ESP_LOGE(TAG, "Failed to allocate LVGL buffer 1 (DMA)");
        p_disp_buf1 = heap_caps_malloc(LCD_H_RES * disp_buf_height * sizeof(lv_color_t), MALLOC_CAP_8BIT);
        if (!p_disp_buf1) {
            ESP_LOGE(TAG, "Failed to allocate LVGL buffer 1!");
            return;
        }
    }

    // Allocate second buffer for double buffering (eliminates black flash)
    p_disp_buf2 = heap_caps_malloc(LCD_H_RES * disp_buf_height * sizeof(lv_color_t), MALLOC_CAP_DMA);
    if (!p_disp_buf2) {
        ESP_LOGW(TAG, "Failed to allocate buffer 2 (DMA), trying standard RAM");
        p_disp_buf2 = heap_caps_malloc(LCD_H_RES * disp_buf_height * sizeof(lv_color_t), MALLOC_CAP_8BIT);
        if (!p_disp_buf2) {
            ESP_LOGW(TAG, "Failed to allocate buffer 2, using single buffer (may see black flash)");
            p_disp_buf2 = NULL;
        }
    }

    ESP_LOGI(TAG, "LVGL: %sbuffering, %u lines/buf (%.1f KB each)",
             p_disp_buf2 ? "Double" : "Single",
             (unsigned int)disp_buf_height,
             (LCD_H_RES * disp_buf_height * sizeof(lv_color_t)) / 1024.0);
#endif

    /* Initialize display buffer with double buffering to prevent black flash */
    lv_disp_draw_buf_init(&draw_buf_dsc, p_disp_buf1, p_disp_buf2, LCD_H_RES * disp_buf_height);

    /* Register the display in LVGL */
    lv_disp_drv_init(&disp_drv);

    /*Set the resolution of the display*/
    disp_drv.hor_res = LCD_H_RES;
    disp_drv.ver_res = LCD_V_RES;

    /* Used to copy the buffer's content to the display */
    disp_drv.flush_cb = disp_flush;

    /*Set a display buffer*/
    disp_drv.draw_buf = &draw_buf_dsc;

    /* Use lcd_trans_done_cb to inform the graphics library that flush already done */
    p_on_trans_done_cb = lv_port_flush_ready;
    p_user_data = NULL;

    /*Finally register the driver*/
    lv_disp_drv_register(&disp_drv);
}

static void lv_port_indev_init(void)
{
    static lv_indev_drv_t indev_drv_tp;
    static lv_indev_drv_t indev_drv_btn;

    /* Register a touchpad input device */
//    lv_indev_drv_init(&indev_drv_tp);
//    indev_drv_tp.type = LV_INDEV_TYPE_POINTER;
//    indev_drv_tp.read_cb = touchpad_read;
//    lv_indev_drv_register(&indev_drv_tp);

    lv_indev_drv_init(&indev_drv_btn);
    indev_drv_btn.type = LV_INDEV_TYPE_BUTTON;
    indev_drv_btn.read_cb = button_read;
    lv_indev_drv_register(&indev_drv_btn);
}

static esp_err_t lv_port_tick_init(void)
{
    static const uint32_t tick_inc_period_ms = 5;
    const esp_timer_create_args_t periodic_timer_args = {
        .callback = lv_tick_inc_cb,
        .name = "",     /* name is optional, but may help identify the timer when debugging */
        .arg = (void *) &tick_inc_period_ms,
        .dispatch_method = ESP_TIMER_TASK,
        .skip_unhandled_events = true,
    };

    esp_timer_handle_t periodic_timer;
    ESP_ERROR_CHECK(esp_timer_create(&periodic_timer_args, &periodic_timer));

    /* The timer has been created but is not running yet. Start the timer now */
    ESP_ERROR_CHECK(esp_timer_start_periodic(periodic_timer, tick_inc_period_ms * 1000));
    return ESP_OK;
}

bool lcd_trans_done_cb(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_io_event_data_t *user_data, void *event_data)
{
    (void) panel_io;
    (void) user_data;
    (void) event_data;
    /* Used for `bsp_lcd_flush_wait` */
    if (likely(NULL != lcd_flush_done_sem)) {
        xSemaphoreGive(lcd_flush_done_sem);
    }
    if (p_on_trans_done_cb) {
        return p_on_trans_done_cb(p_user_data);
    }

    return false;
}

// Fills the entire screen with black pixels using the panel API directly
void lv_port_black_screen(void)
{
    // If using RGB565, lv_color_t is 16 bits, otherwise adjust as needed
    size_t line_bytes = LCD_H_RES * sizeof(lv_color_t);
    lv_color_t *black_line = heap_caps_malloc(line_bytes, MALLOC_CAP_DMA);
    if (black_line) {
        memset(black_line, 0, line_bytes); // Black (all zeros)
        for (int y = 0; y < LCD_V_RES; y++) {
            esp_err_t err = esp_lcd_panel_draw_bitmap(panel_handle, 0, y, LCD_H_RES, y + 1, (uint8_t *)black_line);
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "Black screen fill failed at y=%d: %s", y, esp_err_to_name(err));
                break;
            }
        }
        heap_caps_free(black_line);
    } else {
        ESP_LOGE(TAG, "Cannot allocate black_line buffer for black screen fill");
    }
}

static esp_err_t bootscreen_load_file(const char *path, char **buf, uint32_t *size) {
    struct stat st;
    if (stat(path, &st) != 0 || st.st_size <= 0) {
        ESP_LOGE(TAG, "Failed to stat file or empty file: %s", path);
        return ESP_FAIL;
    }

    long file_size = st.st_size;

    FILE *f = fopen(path, "rb");
    if (!f) {
        ESP_LOGE(TAG, "Failed to open file for reading %s", path);
        return ESP_FAIL;
    }
    
    *buf = malloc(file_size);
    if (!*buf) {
        ESP_LOGE(TAG, "Failed to allocate memory for file buffer");
        fclose(f);
        return ESP_ERR_NO_MEM;
    }
    size_t read_bytes = fread(*buf, 1, file_size, f);
    fclose(f);
    if (read_bytes != file_size) {
        ESP_LOGE(TAG, "Failed to read the entire file");
        free(*buf);
        *buf = NULL;
        return ESP_FAIL;
    }
    *size = file_size;
    return ESP_OK;
}

static esp_err_t bootscreen_decode_jpeg(char *input_buf, uint32_t len, uint16_t **output_buf, uint16_t *width, uint16_t *height) {
    jpeg_dec_config_t config = DEFAULT_JPEG_DEC_CONFIG();
    config.output_type = JPEG_RAW_TYPE_RGB565_BE;

    jpeg_dec_handle_t jpeg_dec = jpeg_dec_open(&config);
    if (!jpeg_dec) {
        ESP_LOGE(TAG, "Failed to open JPEG decoder");
        return ESP_FAIL;
    }

    jpeg_dec_io_t *jpeg_io = calloc(1, sizeof(jpeg_dec_io_t));
    jpeg_dec_header_info_t *out_info = calloc(1, sizeof(jpeg_dec_header_info_t));
    if (!jpeg_io || !out_info) {
        ESP_LOGE(TAG, "Failed to allocate JPEG decoder structures");
        if (jpeg_io) free(jpeg_io);
        if (out_info) free(out_info);
        jpeg_dec_close(jpeg_dec);
        return ESP_ERR_NO_MEM;
    }

    jpeg_io->inbuf = (unsigned char*)input_buf;
    jpeg_io->inbuf_len = len;

    esp_err_t ret = jpeg_dec_parse_header(jpeg_dec, jpeg_io, out_info);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to parse JPEG header");
        goto cleanup;
    }

    // Save dimensions for caller
    *width = out_info->width;
    *height = out_info->height;

    int outbuf_len = out_info->width * out_info->height * 2; // RGB565 = 2 bytes per pixel

    // Use heap_caps_aligned_alloc to match the allocator used in lv_decoders.c
    // This ensures the buffer can be freed with heap_caps_free()
    *output_buf = (uint16_t*)heap_caps_aligned_alloc(16, outbuf_len, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!*output_buf) {
        *output_buf = (uint16_t*)heap_caps_aligned_alloc(16, outbuf_len, MALLOC_CAP_8BIT);
    }
    if (!*output_buf) {
        ESP_LOGE(TAG, "Failed to allocate output buffer");
        ret = ESP_ERR_NO_MEM;
        goto cleanup;
    }

    jpeg_io->outbuf = (unsigned char*)*output_buf;
    int inbuf_consumed = jpeg_io->inbuf_len - jpeg_io->inbuf_remain;
    jpeg_io->inbuf = (unsigned char*)input_buf + inbuf_consumed;
    jpeg_io->inbuf_len = jpeg_io->inbuf_remain;

    ret = jpeg_dec_process(jpeg_dec, jpeg_io);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to decode JPEG");
        heap_caps_free(*output_buf);
        *output_buf = NULL;
    }

cleanup:
    if (out_info) free(out_info);
    if (jpeg_io) free(jpeg_io);
    jpeg_dec_close(jpeg_dec);
    return ret;
}

void lv_load_bootscreen(void)
{
    char *file_buffer = NULL;
    uint32_t file_size = 0;
    uint16_t *image_pixels = NULL;
    uint16_t img_width = 0, img_height = 0;
    esp_err_t ret;

    const char *boot_jpg_path = "/sdcard/animation_jpg/power/power_on.jpg";

    ret = bootscreen_load_file(boot_jpg_path, &file_buffer, &file_size);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to load boot image file: %s", esp_err_to_name(ret));
        return;
    }

    ret = bootscreen_decode_jpeg(file_buffer, file_size, &image_pixels, &img_width, &img_height);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to decode boot image: %s", esp_err_to_name(ret));
        free(file_buffer);
        return;
    }

    if (image_pixels != NULL) {
        const int block_height = 40;
        int blocks = (LCD_V_RES + block_height - 1) / block_height;

        for (int block = 0; block < blocks; block++) {
            int y_start = block * block_height;
            int y_end = (y_start + block_height > LCD_V_RES) ? LCD_V_RES : (y_start + block_height);

            uint8_t *block_data = (uint8_t *)image_pixels + (y_start * LCD_H_RES * sizeof(uint16_t));

            ret = esp_lcd_panel_draw_bitmap(panel_handle, 0, y_start, LCD_H_RES, y_end, block_data);
            if (ret != ESP_OK) {
                ESP_LOGE(TAG, "Failed to display boot image block %d: %s", block, esp_err_to_name(ret));
                break;
            }
        }

        if (ret == ESP_OK) {
            ESP_LOGI(TAG, "Boot screen displayed successfully in %d blocks", blocks);

            // Register the buffer with LVGL decoder system for reuse
            // This avoids re-reading and re-decoding the same JPG in lv_boot_animation()
            ret = lvgl_set_content_buffer(CONTENT_TYPE_POPUP, boot_jpg_path,
                                         (uint8_t *)image_pixels, img_width, img_height);
            if (ret == ESP_OK) {
                ESP_LOGI(TAG, "Boot screen buffer registered for reuse (saved ~60-120ms)");
            } else {
                ESP_LOGW(TAG, "Failed to register boot buffer, will be re-loaded later");
                heap_caps_free(image_pixels);
            }
        } else {
            // Display failed, free the buffer
            heap_caps_free(image_pixels);
        }
    }

    free(file_buffer);
}

void lv_load_offscreen(void)
{
    char *file_buffer = NULL;
    uint32_t file_size = 0;
    uint16_t *image_pixels = NULL;
    uint16_t img_width = 0, img_height = 0;
    esp_err_t ret;

    ret = bootscreen_load_file("/sdcard/animation_jpg/power/power_off.jpg", &file_buffer, &file_size);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to load power-off image file: %s", esp_err_to_name(ret));
        return;
    }

    ret = bootscreen_decode_jpeg(file_buffer, file_size, &image_pixels, &img_width, &img_height);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to decode power-off image: %s", esp_err_to_name(ret));
        free(file_buffer);
        return;
    }

    if (image_pixels != NULL) {
        const int block_height = 40;
        int blocks = (LCD_V_RES + block_height - 1) / block_height;

        for (int block = 0; block < blocks; block++) {
            int y_start = block * block_height;
            int y_end = (y_start + block_height > LCD_V_RES) ? LCD_V_RES : (y_start + block_height);

            uint8_t *block_data = (uint8_t *)image_pixels + (y_start * LCD_H_RES * sizeof(uint16_t));

            ret = esp_lcd_panel_draw_bitmap(panel_handle, 0, y_start, LCD_H_RES, y_end, block_data);
            if (ret != ESP_OK) {
                ESP_LOGE(TAG, "Failed to display power-off image block %d: %s", block, esp_err_to_name(ret));
                break;
            }
        }

        if (ret == ESP_OK) {
            ESP_LOGI(TAG, "Power-off screen displayed successfully in %d blocks", blocks);
        }

        heap_caps_free(image_pixels);
    }

    free(file_buffer);
}


esp_err_t lv_port_init(void *lcd_panel_handle)
{
    panel_handle  = lcd_panel_handle;
    lcd_flush_done_sem = xSemaphoreCreateBinary();

    xSemaphoreGive(lcd_flush_done_sem);

//    tt21100_tp_init();

    /* Initialize LVGL library */
    lv_init();

    /* Register display for LVGL */
    lv_port_disp_init();
    /* Register input device for LVGL*/
    lv_port_indev_init();

    /* Initialize LVGL's tick source */
    lv_port_tick_init();
    /* Nothing error */

    /* Makes the first screen already black */
    //lv_port_black_screen();
    lv_load_bootscreen();

    ESP_LOGI(TAG, "LVGL port initialized successfully");
    return ESP_OK;
}
