#include <string.h>

#include "storage.h"

#include "esp_littlefs.h"
#include "nvs_flash.h"
#include "esp_log.h"
#include "cJSON.h"
#include "s3_nvs_item.h"

#define MOUNT_POINT "/S3_LFS"
#define FILE_NAME_SIZE (64)
#define FILE_NAME_SIZE (64)

// NVS RELATED =======================================================================
// Static flag to track if NVS has been initialized (prevent multiple init/erase)
static bool nvs_initialized = false;

esp_err_t init_nvs(void)
{
    // If already initialized, skip to avoid repeated erase operations
    if (nvs_initialized) {
        ESP_LOGD("STORAGE", "NVS already initialized, skipping");
        return ESP_OK;
    }

    esp_err_t ret = nvs_flash_init();

    // NOTE: Only erase NVS in truly critical situations
    // IMPORTANT: ESP_ERR_NVS_NEW_VERSION_FOUND after OTA should be handled carefully
    // to preserve user data as much as possible
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES)
    {
        // Last resort: NVS is completely full and cannot function
        ESP_LOGW("STORAGE", "⚠️ NVS has no free pages - erasing to recover (all settings will be lost)");
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
        nvs_initialized = (ret == ESP_OK);
    }
    else if (ret == ESP_ERR_NVS_NEW_VERSION_FOUND)
    {
        // After OTA, NVS version may change
        // TODO: Implement proper data migration instead of erasing
        // For now, we must erase to make NVS functional, but this will reset all settings
        ESP_LOGW("STORAGE", "⚠️ NVS version mismatch (after OTA) - erasing NVS (settings will be reset)");
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
        nvs_initialized = (ret == ESP_OK);
        ESP_LOGI("STORAGE", "NVS re-initialized after version change");
    }
    else if (ret == ESP_OK)
    {
        nvs_initialized = true;
        ESP_LOGD("STORAGE", "NVS initialized successfully");
    }

    return ret;
}

// LITTLE FS RELATED ==================================================================
esp_err_t write_serial_number(const char *sn_value)
{
    return s3_nvs_set(NVS_S3_DEVICE_SN, (void*)sn_value);
}

esp_err_t read_serial_number(char *sn_buffer)
{
    return s3_nvs_get(NVS_S3_DEVICE_SN, (void *)sn_buffer);
}

esp_err_t write_wifi_credentials(const char *wifi_ssid, const char *wifi_password)
{
    esp_err_t err = s3_nvs_set(NVS_S3_WIFI_ssid, (void*)wifi_ssid);
    if(err != ESP_OK)
        return err;

    err = s3_nvs_set(NVS_S3_WIFI_password, (void*)wifi_password);
    if(err != ESP_OK)
        return err;

    return ESP_OK;
}

esp_err_t write_ssid(const char *wifi_ssid)
{
    esp_err_t err = s3_nvs_set(NVS_S3_WIFI_ssid, (void*)wifi_ssid);
    if(err != ESP_OK)
        return err;

    return ESP_OK;
}

esp_err_t write_pass(const char *wifi_password)
{
    esp_err_t err = s3_nvs_set(NVS_S3_WIFI_password, (void*)wifi_password);
    if(err != ESP_OK)
        return err;

    return ESP_OK;
}

esp_err_t read_wifi_credentials(char *wifi_ssid, char *wifi_password)
{
    esp_err_t err = s3_nvs_get(NVS_S3_WIFI_ssid, (void *)wifi_ssid);
    if(err != ESP_OK)
        return err;

    err = s3_nvs_get(NVS_S3_WIFI_password, (void *)wifi_password);
    if(err != ESP_OK)
        return err;

    return ESP_OK;
}

esp_err_t write_secret_key(const char *secret_key_str)
{
    return s3_nvs_set(NVS_S3_CLOUD_secret_key, (void*)secret_key_str);
}

esp_err_t read_secret_key(char *secret_key_str)
{
    return s3_nvs_get(NVS_S3_CLOUD_secret_key, (void *)secret_key_str);
}

esp_err_t write_timezone(const char *timezone_str)
{
    return s3_nvs_set(NVS_S3_timezone, (void*)timezone_str);
}

esp_err_t read_timezone(char *timezone_str)
{
    return s3_nvs_get(NVS_S3_timezone, (void *)timezone_str);
}

esp_err_t write_oob_status(int *oob)
{
	return s3_nvs_set(NVS_S3_DEVICE_OOB, (void*)oob);
}

esp_err_t read_oob_status(int *oob)
{
	return s3_nvs_get(NVS_S3_DEVICE_OOB, (void *)oob);
}
