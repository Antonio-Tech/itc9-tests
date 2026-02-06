#ifndef STORAGE_H
#define STORAGE_H

#include "esp_err.h"

#define INFO_BUFFER_SIZE (128)
// #define SERIAL_NUMBER_SIZE (64)
// #define TIMEZONE_STR_SIZE (10)
// #define SECRET_KEY_STR_SIZE (64)

// #define WIFI_SSID_SIZE (50)
// #define WIFI_PASSWORD_SIZE (64)

esp_err_t write_serial_number(const char *sn_value);
esp_err_t read_serial_number(char *sn_buffer);

esp_err_t write_wifi_credentials(const char *wifi_ssid, const char *wifi_password);
esp_err_t write_ssid(const char *wifi_ssid);
esp_err_t write_pass(const char *wifi_password);
esp_err_t read_wifi_credentials(char *wifi_ssid, char *wifi_password);

esp_err_t write_secret_key(const char *secret_key_str);
esp_err_t read_secret_key(char *secret_key_str);

esp_err_t write_timezone(const char *timezone_str);
esp_err_t read_timezone(char *timezone_str);

esp_err_t write_oob_status(int *oob);
esp_err_t read_oob_status(int *oob);

esp_err_t init_littleFS(void);
esp_err_t init_nvs(void);

#endif