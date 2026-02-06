//
// Created by Shane_Hwang on 2025/6/16.
//
#ifndef S3_HTTPS_CLOUD_H
#define S3_HTTPS_CLOUD_H

#define CLOUD_DOWNLOAD_PATH "/sdcard/tmp/"
#define CLOUD_ACCOUNT_FILENAME "account_file.json"
#define CLOUD_FW_CONTENTS_JSON "fw-contents.json"

#define DEV_DOMAIN "https://s3-dev.ipg-services.com"
#define STG_DOMAIN "https://s3-stg.ipg-services.com"
#define PRO_DOMAIN "https://s3.ipg-services.com"

#include "time.h"
#define CEI_INVALID_SECRET_KEY 10032

esp_err_t ensure_dir_exists(const char *dir_path);

void create_directories(const char *fullpath);

esp_err_t cei_complete_binding_of_device(int *biding_code);

esp_err_t cei_upload_device_info(char *input_data);

esp_err_t cei_download_file(char *url, char *fullPath);

esp_err_t OTA_Update(char *url);

esp_err_t https_download_content_file(void *pvParameters);

esp_err_t https_download_account_file(void *pvParameters);

esp_err_t parser_ota_info(char **version, char **ota_url);

esp_err_t parser_ota_resource_info(char **version, char **ota_url);

esp_err_t s3_cloud_upload_tracking_info(const char *tracking_data);



#endif // S3_HTTPS_CLOUD_H
