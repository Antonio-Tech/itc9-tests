/* SPI Master example: jpeg decoder.

   This example code is in the Public Domain (or CC0 licensed, at your option.)

   Unless required by applicable law or agreed to in writing, this
   software is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
   CONDITIONS OF ANY KIND, either express or implied.
*/

/*
The image used for the effect on the LCD in the SPI master example is stored in flash
as a jpeg file. This file contains the decode_image routine, which uses the tiny JPEG
decoder library to decode this JPEG into a format that can be sent to the display.

Keep in mind that the decoder library cannot handle progressive files (will give
``Image decoder: jd_prepare failed (8)`` as an error) so make sure to save in the correct
format if you want to use a different image file.
*/

#include "decode_image.h"
#include "esp_log.h"
#include "esp_jpeg_dec.h"
#include "esp_jpeg_common.h"
#include "esp_vfs_fat.h"
#include "string.h"
#include "s3_logger.h"

// Reference the binary-included jpeg file
extern const uint8_t image_jpg_start[] asm("_binary_image_jpg_start");
extern const uint8_t image_jpg_end[] asm("_binary_image_jpg_end");

const char *TAG = "ImageDec";

uint16_t *pixels = NULL;

static jpeg_error_t esp_jpeg_decoder_one_picture(unsigned char *input_buf, int len, unsigned char **output_buf);

uint16_t *get_pixel(int x, int y) { return (((uint16_t *)pixels) + y * 240 + x); }

esp_err_t local_jpeg_init(void) {
    //    return decode_image(&pixels, image_jpg_start, image_jpg_end - image_jpg_start);
    if (pixels != NULL) {
        // free(pixels);
        jpeg_free_align(pixels);
        pixels = NULL;
    }
    return esp_jpeg_decoder_one_picture((unsigned char *)image_jpg_start, image_jpg_end - image_jpg_start,
                                        (unsigned char **)&pixels);
}

esp_err_t jpeg_init(char *fbuf, uint32_t size) {
    // return decode_image(&pixels, (uint8_t *)fbuf, size);
    if (pixels != NULL) {
        jpeg_free_align(pixels);
        pixels = NULL;
    }
    return esp_jpeg_decoder_one_picture((unsigned char *)fbuf, size, (unsigned char **)&pixels);
}

jpeg_error_t esp_jpeg_decoder_one_picture(unsigned char *input_buf, int len, unsigned char **output_buf) {
    int outbuf_len = 0;
    unsigned char *out_buf = NULL;
    jpeg_error_t ret = JPEG_ERR_OK;
    jpeg_dec_io_t *jpeg_io = NULL;
    jpeg_dec_header_info_t *out_info = NULL;
    // Generate default configuration

    jpeg_dec_config_t config = DEFAULT_JPEG_DEC_CONFIG();
    config.output_type = JPEG_RAW_TYPE_RGB565_BE;
    // Empty handle to jpeg_decoder

    jpeg_dec_handle_t jpeg_dec = NULL;

    // Create jpeg_dec
    jpeg_dec = jpeg_dec_open(&config);
    if (jpeg_dec == NULL) {
        ret = JPEG_ERR_PAR;
        goto jpeg_dec_failed;
    }

    // Create io_callback handle
    jpeg_io = calloc(1, sizeof(jpeg_dec_io_t));
    if (jpeg_io == NULL) {
        ret = JPEG_ERR_MEM;
        goto jpeg_dec_failed;
    }

    // Create out_info handle
    out_info = calloc(1, sizeof(jpeg_dec_header_info_t));
    if (out_info == NULL) {
        ret = JPEG_ERR_MEM;
        goto jpeg_dec_failed;
    }

    // Set input buffer and buffer len to io_callback
    jpeg_io->inbuf = input_buf;
    jpeg_io->inbuf_len = len;

    // Parse jpeg picture header and get picture for user and decoder
    ret = jpeg_dec_parse_header(jpeg_dec, jpeg_io, out_info);
    if (ret != JPEG_ERR_OK) {
        goto jpeg_dec_failed;
    }

    // Calloc out_put data buffer and update inbuf ptr and inbuf_len
    if (config.output_type == JPEG_RAW_TYPE_RGB565_LE || config.output_type == JPEG_RAW_TYPE_RGB565_BE ||
        config.output_type == JPEG_RAW_TYPE_CbYCrY) {
        outbuf_len = out_info->width * out_info->height * 2;
    } else if (config.output_type == JPEG_RAW_TYPE_RGB888) {
        outbuf_len = out_info->width * out_info->height * 3;
    } else {
        ret = JPEG_ERR_PAR;
        goto jpeg_dec_failed;
    }
    out_buf = jpeg_malloc_align(outbuf_len, 16);
    if (out_buf == NULL) {
        ret = JPEG_ERR_MEM;
        goto jpeg_dec_failed;
    }
    jpeg_io->outbuf = out_buf;
    *output_buf = out_buf;
    int inbuf_consumed = jpeg_io->inbuf_len - jpeg_io->inbuf_remain;
    jpeg_io->inbuf = input_buf + inbuf_consumed;
    jpeg_io->inbuf_len = jpeg_io->inbuf_remain;

    // Start decode jpeg raw data
    ret = jpeg_dec_process(jpeg_dec, jpeg_io);
    if (ret != JPEG_ERR_OK) {
        goto jpeg_dec_failed;
    }

    // Decoder deinitialize
jpeg_dec_failed:
    // jpeg_free_align(out_buf);
    jpeg_dec_close(jpeg_dec);
    if (out_info) {
        free(out_info);
    }
    if (jpeg_io) {
        free(jpeg_io);
    }
    return ret;
}

esp_err_t load_file(const char *path, char **buf, uint32_t *size) {
    struct stat st;
    if (stat(path, &st) != 0 || st.st_size <= 0) {
        ESP_LOGE(TAG, "Failed to stat file or empty file: %s", path);
        return ESP_FAIL;
    }

    long file_size = st.st_size;
    ESP_LOGI(TAG, "File size: %ld", file_size);

    FILE *f = s3_fopen(path, "rb");
    if (!f) {
        ESP_LOGE(TAG, "Failed to open file for reading %s", path);
        return ESP_FAIL;
    }
    //*buf = (char *)malloc(file_size);

    *buf = jpeg_malloc_align(file_size, 16);
    if (!*buf) {
        ESP_LOGE(TAG, "Failed to allocate memory for file buffer");
        s3_fclose(f);
        return ESP_ERR_NO_MEM;
    }
    size_t read_bytes = s3_fread(*buf, 1, file_size, f);
    s3_fclose(f);
    if (read_bytes != file_size) {
        ESP_LOGE(TAG, "Failed to read the entire file");
        free(*buf);
        *buf = NULL;
        return ESP_FAIL;
    }
    *size = file_size;
    return ESP_OK;
}
