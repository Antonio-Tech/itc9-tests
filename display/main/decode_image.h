/*
   This example code is in the Public Domain (or CC0 licensed, at your option.)

   Unless required by applicable law or agreed to in writing, this
   software is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
   CONDITIONS OF ANY KIND, either express or implied.
*/

#pragma once
#include <stdint.h>
#include "esp_err.h"

extern uint16_t *pixels;

esp_err_t local_jpeg_init(void);
esp_err_t jpeg_init(char *fbuf, uint32_t size);
uint16_t *get_pixel(int x, int y);
esp_err_t load_file(const char *path, char **buf, uint32_t *size);
