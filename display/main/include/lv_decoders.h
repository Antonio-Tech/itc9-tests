/*
   This example code is in the Public Domain (or CC0 licensed, at your option.)

   Unless required by applicable law or agreed to in writing, this
   software is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
   CONDITIONS OF ANY KIND, either express or implied.
*/

#ifndef LV_DECODERS_H
#define LV_DECODERS_H

#include "esp_err.h"
#include "lvgl.h"
#include "s3_definitions.h"

typedef struct {
    lv_img_dsc_t dsc;
    uint8_t *data_buf;
} image_resource_t;

// Legacy functions (deprecated - use content-specific functions instead)
esp_err_t lvgl_load_image_from_sdcard(const char *path);
esp_err_t lvgl_load_icon_from_sdcard(const char *path);
esp_err_t lvgl_load_gif_from_sdcard(const char *path);
esp_err_t lvgl_load_png_from_sdcard(const char *path);
// lv_obj_t *lvgl_load_lottie_from_sdcard(const char *path, uint16_t w, uint16_t h);
const lv_img_dsc_t *lvgl_get_img(void);
const lv_img_dsc_t *lvgl_get_icon(void);
const lv_img_dsc_t *lvgl_get_gif(void);
const lv_img_dsc_t *lvgl_get_png(void);

// New content-specific functions
esp_err_t lvgl_load_content_jpg(content_type_t content_type, const char *path);
esp_err_t lvgl_load_content_png(content_type_t content_type, const char *path);
esp_err_t lvgl_load_content_gif(content_type_t content_type, const char *path);
esp_err_t lvgl_set_content_buffer(content_type_t content_type, const char *path, uint8_t *decoded_buf, uint16_t width, uint16_t height);
const lv_img_dsc_t *lvgl_get_content_dsc(content_type_t content_type);

bool lvgl_validate_gif_dsc(const lv_img_dsc_t *gif_dsc);
void lvgl_free_previous_buffer(void);

// JPEG Cache functions
void jpeg_cache_init(void);
void jpeg_cache_invalidate(const char *path);

// PNG Cache functions
void png_cache_init(void);
void png_cache_invalidate(const char *path);

#endif // LV_DECODERS_H


