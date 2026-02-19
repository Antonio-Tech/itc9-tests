//
// Created by Shane_Hwang on 2025/5/6.
//

#ifndef MANUAL_OTA_H
#define MANUAL_OTA_H

#include "esp_peripherals.h"

esp_err_t set_sn(char *sn);

esp_err_t get_sn(char *sn);

esp_err_t fw_version(esp_periph_handle_t periph, int argc, char *argv[]);

esp_err_t manual_ota(esp_periph_handle_t periph, int argc, char *argv[]);

esp_err_t ota_main(char *uri, int checkVer);

#endif //MANUAL_OTA_H
