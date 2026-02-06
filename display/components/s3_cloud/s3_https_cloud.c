//
// Created by Shane_Hwang on 2025/6/16.
//
#include <inttypes.h>
#include <stdlib.h>
#include <string.h>
#include <sys/param.h>
#include "esp_event.h"
#include "esp_log.h"
#include "esp_tls.h"
#include "nvs_flash.h"
#if CONFIG_MBEDTLS_CERTIFICATE_BUNDLE
#include "esp_crt_bundle.h"
#endif

#include "cJSON.h"
#include "esp_http_client.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "s3_https_cloud.h"
#include "s3_nvs_item.h"
#include "storage.h"
#include "manual_ota.h"
#include "alarm_mgr.h"
#include "s3_logger.h"
#include <time.h>

static const char *TAG = "HTTP_CLIENT";
static char *dynamic_buffer = NULL; // Buffer to store response of http request from event handler
static int buffer_len = 0; // Stores number of bytes read

#define FILE_PATH "/sdcard/shane.bin"

/* todo : In this phase we default use staging server, until the production server is ready */
static const char *s3_domain_str[DOMAIN_ARRAY_SIZE] = {
	"s3-api.ipg-services.com", "s3-stg.ipg-services.com", "s3-dev.ipg-services.com"};

static const char *s3_Basic_Authorization[DOMAIN_ARRAY_SIZE] ={
	"Basic ZGt6UlVLNkdLSDhmd2JGeUMxeHBsdFhzenNmUWRKZGw6QUxib0laWGNLWFlYeDM1YVhDY0hpUldhcUZzckNFWHU=",
	"Basic Y09vdTkwQnl6ZUhKRVlQd08xbDVQeVh6Skhub2U1WjE6Q0VXaE9aMHhzWUhQa0xleXF6anZ6dUhZUXY2VnNEN1g=",
	"Basic WHBOR1ZnOEMzWjZpRDF0TGFmZWkybXlRRjVPN1JQaEw6d3ZTdTA5VzRyQUtpc0FJMVg5QjRmQ1E0R0l5aElOdDM="};

static const char *ota_domain_str[DOMAIN_ARRAY_SIZE] = {
    "s3-api.ipg-services.com", "s3-stg.ipg-services.com", "s3-dev.ipg-services.com"};

static const char *ota_Basic_Authorization[DOMAIN_ARRAY_SIZE] ={
    "Basic ZGt6UlVLNkdLSDhmd2JGeUMxeHBsdFhzenNmUWRKZGw6QUxib0laWGNLWFlYeDM1YVhDY0hpUldhcUZzckNFWHU=",
	"Basic Y09vdTkwQnl6ZUhKRVlQd08xbDVQeVh6Skhub2U1WjE6Q0VXaE9aMHhzWUhQa0xleXF6anZ6dUhZUXY2VnNEN1g=",
	"Basic WHBOR1ZnOEMzWjZpRDF0TGFmZWkybXlRRjVPN1JQaEw6d3ZTdTA5VzRyQUtpc0FJMVg5QjRmQ1E0R0l5aElOdDM="};

esp_err_t ensure_dir_exists(const char *dir_path) {
    struct stat st = {0};

    if (stat(dir_path, &st) == -1) {
        ESP_LOGI(TAG, "Directory not found. Creating: %s", dir_path);
        if (mkdir(dir_path, 0775) == -1) {
            ESP_LOGE(TAG, "Failed to create directory %s: %s", dir_path, strerror(errno));
            return ESP_FAIL;
        }
    }
    return ESP_OK;
}

void create_directories(const char *fullpath) {
    char tmp[256];
    snprintf(tmp, sizeof(tmp), "%s", fullpath);

    char *last_slash = strrchr(tmp, '/');
    if (!last_slash) return;

    *last_slash = '\0';

    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            mkdir(tmp, 0775);
            *p = '/';
        }
    }
    mkdir(tmp, 0775);
}

esp_err_t _http_event_handler(esp_http_client_event_t *evt) {
    switch (evt->event_id) {
        case HTTP_EVENT_ERROR:
            ESP_LOGD(TAG, "HTTP_EVENT_ERROR");
            break;
        case HTTP_EVENT_ON_CONNECTED:
            ESP_LOGD(TAG, "HTTP_EVENT_ON_CONNECTED");
            break;
        case HTTP_EVENT_HEADER_SENT:
            ESP_LOGD(TAG, "HTTP_EVENT_HEADER_SENT");
            break;
        case HTTP_EVENT_ON_HEADER:
            ESP_LOGD(TAG, "HTTP_EVENT_ON_HEADER, key=%s, value=%s", evt->header_key, evt->header_value);
            break;
        case HTTP_EVENT_ON_DATA:
            ESP_LOGD(TAG, "HTTP_EVENT_ON_DATA, len=%d", evt->data_len);
            // Clean the buffer in case of a new request
            // first init buffer
            if (dynamic_buffer == NULL) {
                int content_length = esp_http_client_get_content_length(evt->client);
                if (content_length <= 0) {
                    ESP_LOGE(TAG, "Invalid content length");
                    return ESP_FAIL;
                }

                dynamic_buffer = (char *) heap_caps_calloc(content_length + 1, sizeof(char),MALLOC_CAP_SPIRAM); // +1 for null terminator
                if (!dynamic_buffer) {
                    ESP_LOGE(TAG, "Failed to allocate memory");
                    return ESP_FAIL;
                }
                buffer_len = 0;
            }

            // Copy  paragraph information to buffer
            if (dynamic_buffer && (buffer_len + evt->data_len <= esp_http_client_get_content_length(evt->client))) {
                memcpy(dynamic_buffer + buffer_len, evt->data, evt->data_len);
                buffer_len += evt->data_len;
            } else {
                ESP_LOGE(TAG, "Buffer overflow or invalid state");
                return ESP_FAIL;
            }
            break;
        case HTTP_EVENT_ON_FINISH:
            ESP_LOGD(TAG, "HTTP_EVENT_ON_FINISH");
            break;
        case HTTP_EVENT_DISCONNECTED:
            ESP_LOGD(TAG, "HTTP_EVENT_DISCONNECTED");
            int mbedtls_err = 0;
            esp_err_t err = esp_tls_get_and_clear_last_error((esp_tls_error_handle_t) evt->data, &mbedtls_err, NULL);
            if (err != 0) {
                ESP_LOGD(TAG, "Last esp error code: 0x%x", err);
                ESP_LOGD(TAG, "Last mbedtls failure: 0x%x", mbedtls_err);
            }
            if (dynamic_buffer != NULL) {
                free(dynamic_buffer);
                dynamic_buffer = NULL;
            }
            buffer_len = 0;
            break;
        case HTTP_EVENT_REDIRECT:
            ESP_LOGE(TAG, "HTTP_EVENT_REDIRECT");
            // esp_http_client_set_header(evt->client, "From", "user@example.com");
            // esp_http_client_set_header(evt->client, "Accept", "text/html");
            // esp_http_client_set_redirection(evt->client);
            break;
    }
    return ESP_OK;
}

/* Doc : APIs_for_Device_v0.3.17.docx 2.2
 * 1. Get Sn from nvs (SPKLF20000001)
 * 2. Get secretKey from nvs (3fc048a5-d89c-4e1d-9483-21147b23f046)
 * 3. domain : https://s3-dev.ipg-services.com/client_service/api/v1/devices/s3/c
 * 4. check response_data :
 *  I (29014) HTTP_CLIENT: cei_complete_binding_of_device GET:{"traceId":"2a034922-8337-41c0-9af2-01741ac9577a","code":0,"message":""}
 *  I (76254) HTTP_CLIENT: cei_complete_binding_of_device GET:{"traceId":"b7ffe594-aeab-4d68-94f6-9755363191ae","code":10033,"message":"Invalid pairing task"}
 */
// manual_ota http://192.168.31.115:8078/cli_app.bin off

esp_err_t https_cloud_complete_binding_of_device(char **response_data) {

    char sn[S3_NVS_SN_LENGTH] = {0};
    if (read_serial_number(sn) == ESP_OK)
        ESP_LOGI(TAG, "get_sn:%s", sn);
    else
        ESP_LOGE(TAG, "get_sn:failed");


    char secret_key[S3_NVS_SECRET_KEY_LENGTH] = {0};
    if(read_secret_key(secret_key) == ESP_OK)
    {
        ESP_LOGI(TAG, "get_secret_key:%s", secret_key);
    }
    else
    {
        ESP_LOGE(TAG, "get_secret_key:failed");
    }


    char patch_data[128] = {0};
    sprintf(patch_data, "{\"secretKey\":\"%s\"}", secret_key);

	int domain = DOMAIN_PRODUCTION;
	if (s3_nvs_get(NVS_S3_SW_CLOUD_DOMAIN, &domain) != ESP_OK)
	{
		ESP_LOGE(TAG, "Failed to get DOMAIN from NVS, using default value");
		domain = DOMAIN_PRODUCTION;
	}

    char tmp_url[128] = {0};
    sprintf(tmp_url, "https://%s/client_service/api/v1/devices/s3/c/%s",s3_domain_str[domain], sn);
	ESP_LOGI(TAG, "url:%s", tmp_url);

    esp_http_client_config_t config = {
            .url = tmp_url,
            .method = HTTP_METHOD_PATCH,
            .event_handler = _http_event_handler,
            .crt_bundle_attach = esp_crt_bundle_attach,
            .disable_auto_redirect = true,
    };

    esp_http_client_handle_t client = esp_http_client_init(&config);
    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_set_header(client, "Authorization", s3_Basic_Authorization[domain]);
    esp_http_client_set_post_field(client, patch_data, strlen(patch_data));
    esp_err_t result = esp_http_client_perform(client);
    ESP_LOGI(TAG, "HTTPS Status = %d", esp_http_client_get_status_code(client));
    if (dynamic_buffer != NULL) {
        *response_data = strdup_spiram(dynamic_buffer);
        free(dynamic_buffer);
        dynamic_buffer = NULL;
    }
    buffer_len = 0;
    esp_http_client_cleanup(client);
    return result;
}

esp_err_t cei_complete_binding_of_device(int *biding_code) {
    char *response_data = NULL;
    int code = -1;
    if (https_cloud_complete_binding_of_device(&response_data) == ESP_OK && response_data != NULL) {
        ESP_LOGI(TAG, "cei_complete_binding_of_device response_data:%s", response_data);
        cJSON *root = cJSON_Parse(response_data);
        if (root == NULL) {
            ESP_LOGE(TAG, "Failed to parse JSON");
        } else {
            const cJSON *jCode = cJSON_GetObjectItem(root, "code");
            if (cJSON_IsNumber(jCode)) {
                code = jCode->valueint;
            }
        }
        cJSON_Delete(root);
        free(response_data);
        response_data = NULL;
    }

    *biding_code = code;
    ESP_LOGI(TAG, "cei_complete_binding_of_device code:%d", code);
    if (code == 0) {
        return ESP_OK;
    }
    ESP_LOGE(TAG, "cei_complete_binding_of_device Err code %d",code);
    return ESP_FAIL;
}

/*
 * Doc : APIs_for_Device_v0.3.17.docx 2.5
 * {"traceId":"bdf5b491-df02-48df-b016-0c4c97acc823","code":0,"message":""}
 */
esp_err_t https_upload_device_info(char *input_data, char **response_data) {

    char sn[S3_NVS_SN_LENGTH] = {0};
    if (read_serial_number(sn) == ESP_OK)
        ESP_LOGI(TAG, "get_sn:%s", sn);
    else
        ESP_LOGE(TAG, "get_sn:failed");
    char secret_key[S3_NVS_SECRET_KEY_LENGTH] = {0};
    if (read_secret_key(secret_key) == ESP_OK) {
        ESP_LOGI(TAG, "get_secret_key:%s", secret_key);
    } else {
        ESP_LOGE(TAG, "get_secret_key:failed");
    }

    int domain = DOMAIN_PRODUCTION;
    if (s3_nvs_get(NVS_S3_SW_CLOUD_DOMAIN, &domain) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to get DOMAIN from NVS, using default value");
        domain = DOMAIN_PRODUCTION;
    }

    char tmp_url[128] = {0};
    sprintf(tmp_url, "https://%s/client_service/api/v1/devices/s3/s/%s", s3_domain_str[domain], sn);
    ESP_LOGI(TAG, "url:%s", tmp_url);
    esp_http_client_config_t config = {
            .url = tmp_url,
            .method = HTTP_METHOD_PATCH,
            .event_handler = _http_event_handler,
            .crt_bundle_attach = esp_crt_bundle_attach,
            .disable_auto_redirect = true,
    };

    esp_http_client_handle_t client = esp_http_client_init(&config);
    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_set_header(client, "x-player-secret", secret_key);
    esp_http_client_set_post_field(client, input_data, strlen(input_data));
    esp_err_t result = esp_http_client_perform(client);
    ESP_LOGI(TAG, "HTTPS Status = %d", esp_http_client_get_status_code(client));
    if (dynamic_buffer != NULL) {
        *response_data = strdup_spiram(dynamic_buffer);
        free(dynamic_buffer);
        dynamic_buffer = NULL;
    }
    buffer_len = 0;
    esp_http_client_cleanup(client);
    return result;
}

esp_err_t cei_upload_device_info(char *input_data) {
    char *response_data = NULL;
    int code = -1;
    if (https_upload_device_info(input_data, &response_data) == ESP_OK && response_data != NULL) {
        ESP_LOGI(TAG, "cei_upload_device_info response_data:%s", response_data);
        cJSON *root = cJSON_Parse(response_data);
        if (root == NULL) {
            ESP_LOGE(TAG, "Failed to parse JSON");
        } else {
            const cJSON *jCode = cJSON_GetObjectItem(root, "code");
            if (cJSON_IsNumber(jCode)) {
                code = jCode->valueint;
            }
        }
        cJSON_Delete(root);
        free(response_data);
        response_data = NULL;
    }
    ESP_LOGI(TAG, "cei_complete_binding_of_device code:%d", code);
    if (code == 0) {
        return ESP_OK;
    }
    ESP_LOGE(TAG, "cei_upload_device_info Err code %d",code);
    if (code == CEI_INVALID_SECRET_KEY)
        return CEI_INVALID_SECRET_KEY;

    return ESP_FAIL;
}

/*
* Doc : APIs_for_Device_v0.3.17.docx 2.3
* E (422194) HTTP_CLIENT: HTTP GET request :
* {"traceId":"74cd497b-9c9d-49b1-9b62-af982a25c02f","code":0,"message":"","result":{"md5":"8f32e22cd40b84512cff546c12dfe881","fileName":"systemContent.json","downloadUrl":"https://s3devcontent.blob.core.windows.net/system-media-content/system-content/systemContent.json?sv=2025-05-05&st=2025-06-17T01%3A53%3A36Z&se=2025-06-18T01%3A53%3A36Z&sr=b&sp=r&sig=xlfD7OKQXQy6WaKXmscRGtAonqUhOyZFR%2ByeGZ0pCCo%3D","token":"sv=2025-05-05&st=2025-06-17T01%3A53%3A36Z&se=2025-06-18T01%3A53%3A36Z&sr=d&sp=r&sig=mdv%2BHsWiUk90EglDfAjwihYAxZ4VIAp4EP3TEoviYQg%3D&sdd=1"}}
*/
esp_err_t cei_sync_all_available_contents(char **response_data) {

    char secret_key[S3_NVS_SECRET_KEY_LENGTH] = {0};
    if(read_secret_key(secret_key) == ESP_OK)
    {
        ESP_LOGI(TAG, "get_secret_key:%s", secret_key);
    }
    else
    {
        ESP_LOGE(TAG, "get_secret_key:failed");
    }

	int domain = DOMAIN_PRODUCTION;
	if (s3_nvs_get(NVS_S3_SW_CLOUD_DOMAIN, &domain) != ESP_OK)
	{
		ESP_LOGE(TAG, "Failed to get DOMAIN from NVS, using default value");
		domain = DOMAIN_PRODUCTION;
	}
	char tmp_url[128] = {0};
	sprintf(tmp_url, "https://%s/client_service/api/v1/contents/s3/c", s3_domain_str[domain]);
	ESP_LOGI(TAG, "url:%s", tmp_url);
    esp_http_client_config_t config = {
            .url = tmp_url,
            .method = HTTP_METHOD_GET,
            .event_handler = _http_event_handler,
            .crt_bundle_attach = esp_crt_bundle_attach,
            .disable_auto_redirect = true,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    esp_http_client_set_header(client, "Content-Type", "application/json");
	esp_http_client_set_header(client, "Authorization", s3_Basic_Authorization[domain]);
    esp_http_client_set_header(client, "x-player-secret", secret_key);
    esp_err_t result = esp_http_client_perform(client);
    ESP_LOGI(TAG, "HTTPS Status = %d", esp_http_client_get_status_code(client));
    if (dynamic_buffer != NULL) {
        *response_data = strdup_spiram(dynamic_buffer);
        free(dynamic_buffer);
        dynamic_buffer = NULL;
    }
    buffer_len = 0;
    esp_http_client_cleanup(client);
    return result;
}

#if 0
/*
cei_sync_all_available_contents
GET:{"traceId":"285c5cb3-ce00-444b-ac1c-8fa11e99558d","code":0,"message":"","result":{"babyId":"","nfcLang":"en-us","babyPacks":[],"alarms":[],"NFCs":[]}}
*/
esp_err_t cei_sync_content_list_by_account(char **response_data) {

    char secret_key[S3_NVS_SECRET_KEY_LENGTH] = {0};
    if(read_secret_key(secret_key) == ESP_OK)
    {
        ESP_LOGI(TAG, "get_secret_key:%s", secret_key);
    }
    else
    {
        ESP_LOGE(TAG, "get_secret_key:failed");
    }

	int domain = DOMAIN_PRODUCTION;
	if (s3_nvs_get(NVS_S3_SW_CLOUD_DOMAIN, &domain) != ESP_OK)
	{
		ESP_LOGE(TAG, "Failed to get DOMAIN from NVS, using default value");
		domain = DOMAIN_PRODUCTION;
	}
	char tmp_url[128] = {0};
	sprintf(tmp_url, "https://%s/client_service/api/v1/devices/s3/pc", s3_domain_str[domain]);
	ESP_LOGI(TAG, "url:%s", tmp_url);
    esp_http_client_config_t config = {
            .url = tmp_url,
            .method = HTTP_METHOD_GET,
            .event_handler = _http_event_handler,
            .crt_bundle_attach = esp_crt_bundle_attach,
            .disable_auto_redirect = true,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_set_header(client, "x-player-secret", secret_key);
    esp_err_t result = esp_http_client_perform(client);
    ESP_LOGI(TAG, "HTTPS Status = %d", esp_http_client_get_status_code(client));
    if (dynamic_buffer != NULL) {
        *response_data = strdup_spiram(dynamic_buffer);
        free(dynamic_buffer);
        dynamic_buffer = NULL;
    }
    buffer_len = 0;
    esp_http_client_cleanup(client);
    return result;
}

esp_err_t cei_download_file(char *url, char *fullPath) {
    // Use safe download: download to temporary file first
    char tempPath[512];
    snprintf(tempPath, sizeof(tempPath), "%s.tmp", fullPath);
    
    create_directories(fullPath);
    create_directories(tempPath);
    
    FILE *fp = s3_fopen(tempPath, "wb");
    if (fp == NULL) {
        ESP_LOGE(TAG, "Failed to open temporary file for writing: %s", tempPath);
        return ESP_FAIL;
    }

    // Diagnostics: log current RSSI and bandwidth before starting download
    wifi_ap_record_t ap_info;
    if (esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK) {
        wifi_bandwidth_t bw;
        if (esp_wifi_get_bandwidth(WIFI_IF_STA, &bw) == ESP_OK) {
            ESP_LOGI(TAG, "Download diagnostics - RSSI: %d dBm, Bandwidth: %s", ap_info.rssi, bw == WIFI_BW_HT40 ? "HT40" : "HT20");
        } else {
            ESP_LOGI(TAG, "Download diagnostics - RSSI: %d dBm", ap_info.rssi);
        }
    }

    // OPTIMIZED: Use large buffers for MP3 files and better network settings
    esp_http_client_config_t config = {
            .url = url,
            .buffer_size = 256*1024,     // 256KB buffer for maximum speed (25MB MP3 files need this!)
            .timeout_ms = 60000,         // 60 second timeout for large MP3 files
             .crt_bundle_attach = esp_crt_bundle_attach, // Disabled for speed
            .keep_alive_enable = true,   // Enable keep-alive for better performance
            .skip_cert_common_name_check = true,  // Azure blob storage certificate flexibility
            .buffer_size_tx = 64*1024,   // Large TX buffer for maximum throughput
            .disable_auto_redirect = false, // Allow redirects to fastest server
            .max_redirection_count = 5,  // Allow multiple redirects to CDN
    };

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == NULL) {
        ESP_LOGE(TAG, "Failed to init HTTP client");
        s3_fclose(fp);
        return ESP_FAIL;
    }

    // Set headers for maximum speed
    esp_http_client_set_header(client, "Connection", "keep-alive");
    esp_http_client_set_header(client, "User-Agent", "ESP32-MP3-Turbo/1.0");
    esp_http_client_set_header(client, "Accept", "*/*");
    esp_http_client_set_header(client, "Accept-Encoding", "identity");

    esp_err_t err = esp_http_client_open(client, 0);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open HTTP connection: %s", esp_err_to_name(err));
        s3_fclose(fp);
        esp_http_client_cleanup(client);
        return err;
    }

    int content_length = esp_http_client_fetch_headers(client);
    ESP_LOGI(TAG, "cei_download_file Content length: %d bytes (%.2f MB)", content_length, content_length/(1024.0f*1024.0f));

    // OPTIMIZED: Try 256KB buffer first, fallback to smaller if memory issues
    const int BUFFER_SIZE = 256*1024; // 256KB buffer for maximum speed
    char *buffer = heap_caps_malloc(BUFFER_SIZE, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    int buffer_size = BUFFER_SIZE;
    if (!buffer) {
        ESP_LOGW(TAG, "Failed to allocate 256KB buffer, trying 128KB fallback");
        buffer = heap_caps_malloc(128*1024, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        buffer_size = 128*1024;
        if (!buffer) {
            ESP_LOGW(TAG, "Failed to allocate 128KB buffer, trying 64KB fallback");
            buffer = heap_caps_malloc(64*1024, MALLOC_CAP_8BIT);
            buffer_size = 64*1024;
            if (!buffer) {
                ESP_LOGE(TAG, "Failed to allocate any reasonable buffer for download");
                s3_fclose(fp);
                esp_http_client_cleanup(client);
                return ESP_FAIL;
            }
        }
    }
    
    ESP_LOGI(TAG, "Using %dKB buffer for optimized MP3 download", buffer_size/1024);
    
    int64_t start_us = esp_timer_get_time();
    int64_t last_log_us = start_us;
    int total_read_len = 0;
    int read_len_this_second = 0;
    while (1) {
        int read_len = esp_http_client_read(client, buffer, buffer_size);
        if (read_len <= 0)
            break;
        s3_fwrite(buffer, 1, read_len, fp);

        total_read_len += read_len;
        read_len_this_second += read_len;
        int64_t now_us = esp_timer_get_time();
        if (now_us - last_log_us >= 1000000) {
            float kb_per_sec = read_len_this_second / 1024.0f;
            float mb_downloaded = total_read_len / (1024.0f * 1024.0f);
            float percent = content_length > 0 ? (total_read_len * 100.0f / content_length) : -1;
            ESP_LOGI(TAG, "Link: %.1f KB/s | Downloaded: %.1f MB (%.1f%%) | Method: CEI Client", kb_per_sec, mb_downloaded, percent);
            last_log_us = now_us;
            read_len_this_second = 0;
        }
        vTaskDelay(10 / portTICK_PERIOD_MS);
    }

    s3_fclose(fp);
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    free(buffer);

    ESP_LOGI(TAG, "File downloaded: %d bytes", total_read_len);
    
    // Check if download was successful and complete
    if (total_read_len == 0) {
        ESP_LOGW(TAG, "Download completely failed (0 bytes), removing temp file: %s", tempPath);
        s3_remove(tempPath);
        return ESP_FAIL;
    }
    
    // Download completed successfully - temp file ready for caller to validate and move
    ESP_LOGI(TAG, "Download completed: %d bytes written to temp file: %s", total_read_len, tempPath);
    return ESP_OK;
}
#endif

esp_err_t https_download_content_file(void *pvParameters) {
    char *response_data = NULL;
    char *_json_url = NULL;
    char *_json_fileName = NULL;
    esp_err_t ret = ESP_FAIL;
    int code = -1;

    if (cei_sync_all_available_contents(&response_data) == ESP_OK && response_data != NULL) {
        ESP_LOGI(TAG, "cei_sync_all_available_contents response_data:%s", response_data);

        cJSON *root = cJSON_Parse(response_data);
        if (root == NULL) {
            ESP_LOGE(TAG, "Failed to parse JSON");
            free(response_data);
            return ESP_FAIL;
        }

        const cJSON *jCode = cJSON_GetObjectItem(root, "code");
        if (cJSON_IsNumber(jCode)) {
            code = jCode->valueint;
            if (code != 0) {
                ESP_LOGE(TAG, "https_download_content_file Err code %d", code);
                goto CLEAN_JSON;
            }
        }

        const cJSON *result = cJSON_GetObjectItem(root, "result");
        if (!cJSON_IsObject(result)) {
            ESP_LOGE(TAG, "No 'result' object in JSON");
            goto CLEAN_JSON;
        }

        const cJSON *downloadUrl = cJSON_GetObjectItem(result, "downloadUrl");
        if (cJSON_IsString(downloadUrl)) {
            _json_url = strdup_spiram(downloadUrl->valuestring);
            ESP_LOGI(TAG, "downloadUrl: %s", _json_url);
        } else {
            ESP_LOGE(TAG, "parser fail downloadUrl");
            goto CLEAN_JSON;
        }

        const cJSON *fileName = cJSON_GetObjectItem(result, "fileName");
        if (cJSON_IsString(fileName)) {
            _json_fileName = strdup_spiram(fileName->valuestring);
            ESP_LOGI(TAG, "fileName: %s", _json_fileName);
            ret = ESP_OK;
        } else {
            ESP_LOGE(TAG, "parser fail fileName");
            goto CLEAN_JSON;
        }

    CLEAN_JSON:
        cJSON_Delete(root);
        free(response_data);
        response_data = NULL;
    }

    char path[128];
    if (ret == ESP_OK && _json_url != NULL && _json_fileName != NULL) {
        sprintf(path, CLOUD_DOWNLOAD_PATH"%s", _json_fileName);
        ret = cei_download_file(_json_url, path);
    }

    if (_json_url)
        free(_json_url);
    if (_json_fileName)
        free(_json_fileName);

    ESP_LOGI(TAG, "Finish https_download_content_file");
    return ret;
}

static char* read_file_to_buffer(const char *path) {
    struct stat st;
    if (stat(path, &st) != 0) {
        ESP_LOGE(TAG, "Failed to stat file: %s", path);
        return NULL;
    }

    long fsize = st.st_size;

    FILE *fp = s3_fopen(path, "r");
    if (fp == NULL) {
        ESP_LOGE(TAG, "Failed to open file for reading: %s", path);
        return NULL;
    }

    char *buffer = heap_caps_malloc(fsize + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (buffer == NULL) {
        ESP_LOGE(TAG, "Failed to allocate buffer for file");
        s3_fclose(fp);
        return NULL;
    }
    if (s3_fread(buffer, 1, fsize, fp) != fsize) {
        ESP_LOGE(TAG, "Failed to read complete file: %s", path);
        free(buffer);
        s3_fclose(fp);
        return NULL;
    }
    buffer[fsize] = '\0';
    s3_fclose(fp);
    return buffer;
}

esp_err_t https_download_account_file(void *pvParameters) {
    esp_err_t ret = ESP_FAIL;
    char secret_key[S3_NVS_SECRET_KEY_LENGTH] = {0};
    if(read_secret_key(secret_key) != ESP_OK) {
        ESP_LOGE(TAG, "get_secret_key:failed");
        return ESP_FAIL;
    }

    int domain = DOMAIN_PRODUCTION;
    if (s3_nvs_get(NVS_S3_SW_CLOUD_DOMAIN, &domain) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to get DOMAIN from NVS, using default value");
        domain = DOMAIN_PRODUCTION;
    }

    char tmp_url[128] = {0};
    sprintf(tmp_url, "https://%s/client_service/api/v1/devices/s3/pc", s3_domain_str[domain]);
    ESP_LOGI(TAG, "url:%s", tmp_url);

    if (ensure_dir_exists(CLOUD_DOWNLOAD_PATH) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create path");
        return ESP_FAIL;
    }

    char tempPath[128];
    sprintf(tempPath, "%s%s.tmp", CLOUD_DOWNLOAD_PATH, CLOUD_ACCOUNT_FILENAME);

    FILE *f = s3_fopen(tempPath, "w");
    if (f == NULL) {
        ESP_LOGE(TAG, "Failed to open file %s", tempPath);
        return ESP_FAIL;
    }

    esp_http_client_config_t config = {
            .url = tmp_url,
            .method = HTTP_METHOD_GET,
            .crt_bundle_attach = esp_crt_bundle_attach,
            .disable_auto_redirect = true,
            .timeout_ms = 20000, // 20s
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_set_header(client, "x-player-secret", secret_key);

    if (esp_http_client_open(client, 0) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open HTTP connection");
        goto cleanup;
    }

    int content_length = esp_http_client_fetch_headers(client);
    int status_code = esp_http_client_get_status_code(client);

    if (status_code != 200) {
        ESP_LOGE(TAG, "HTTP request failed with status code %d", status_code);
        goto cleanup;
    }

    char buffer[1024];
    int total_read_len = 0;
    int read_len;
    while ((read_len = esp_http_client_read(client, buffer, sizeof(buffer))) > 0) {
        s3_fwrite(buffer, 1, read_len, f);
        total_read_len += read_len;
    }

    if (content_length > 0 && total_read_len != content_length) {
        ESP_LOGE(TAG, "Download incomplete. Expected %d, got %d", content_length, total_read_len);
        goto cleanup;
    }

    s3_fclose(f);
    f = NULL;
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    client = NULL;

    // Now read back and parse
    char *response_data = read_file_to_buffer(tempPath);
    if (response_data == NULL) {
        s3_remove(tempPath);
        return ESP_FAIL;
    }

    register_alarms((const char*)response_data);

    cJSON *root = cJSON_Parse(response_data);
    int code = -1;
    if (root) {
        const cJSON *jCode = cJSON_GetObjectItem(root, "code");
        if (cJSON_IsNumber(jCode)) {
            code = jCode->valueint;
        }
        cJSON_Delete(root);
    }
    free(response_data);

    if (code != 0) {
        ESP_LOGE(TAG, "https_download_account_file Err code %d", code);
        s3_remove(tempPath);
        return ESP_FAIL;
    }

    // Success, move temp file with backup strategy (for differential updates)
    char finalPath[128];
    char backupPath[128];
    sprintf(finalPath, "%s%s", CLOUD_DOWNLOAD_PATH, CLOUD_ACCOUNT_FILENAME);
    sprintf(backupPath, "%s%s.bak", CLOUD_DOWNLOAD_PATH, CLOUD_ACCOUNT_FILENAME);

    // Step 1: Remove old backup (if exists), then rename original to backup (if exists)
    s3_remove(backupPath);  // Remove old backup first to avoid rename failure
    bool had_original = (access(finalPath, F_OK) == 0);
    if (had_original) {
        if (s3_rename(finalPath, backupPath) != 0) {
            ESP_LOGE(TAG, "Failed to backup account_file.json");
            s3_remove(tempPath);
            return ESP_FAIL;
        }
        ESP_LOGI(TAG, "Backed up old account_file.json for differential comparison");
    } else {
        ESP_LOGI(TAG, "No previous account_file.json found (first download)");
    }

    // Step 2: Move temp to final place
    if (s3_rename(tempPath, finalPath) == 0) {
        // Step 3: KEEP backup for differential comparison (DO NOT remove)
        ESP_LOGI(TAG, "Successfully updated account_file.json, backup preserved for differential update");
    } else {
        // Step 4: Failed - restore original from backup
        if (had_original) s3_rename(backupPath, finalPath);
        ESP_LOGE(TAG, "Failed to rename temp file - original restored");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Finish https_download_account_file");
    return ESP_OK;

cleanup:
    if (f) s3_fclose(f);
    if (client) esp_http_client_cleanup(client);
    s3_remove(tempPath);
    return ESP_FAIL;
}

esp_err_t OTA_Update(char *url)
{
	esp_err_t ret = ESP_FAIL;
	ret = ota_main(url, 1);
	return ret;
}

esp_err_t api_ota_info(char **response_data) {

    int domain = DOMAIN_PRODUCTION;
    if (s3_nvs_get(NVS_S3_SW_CLOUD_DOMAIN, &domain) != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed to get DOMAIN from NVS, using default value");
        domain = DOMAIN_PRODUCTION;
    }

    char tmp_url[128] = {0};
    sprintf(tmp_url, "https://%s/api/v1/otas/?model=ITC9", ota_domain_str[domain]);
    ESP_LOGI(TAG, "url:%s", tmp_url);
    esp_http_client_config_t config = {
        .url = tmp_url,
        .method = HTTP_METHOD_GET,
        .event_handler = _http_event_handler,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .disable_auto_redirect = true,
};
    esp_http_client_handle_t client = esp_http_client_init(&config);
    esp_http_client_set_header(client, "Authorization", ota_Basic_Authorization[domain]);
    esp_err_t result = esp_http_client_perform(client);
    ESP_LOGI(TAG, "HTTPS Status = %d", esp_http_client_get_status_code(client));
    if (dynamic_buffer != NULL) {
        *response_data = strdup_spiram(dynamic_buffer);
        free(dynamic_buffer);
        dynamic_buffer = NULL;
    }
    buffer_len = 0;
    esp_http_client_cleanup(client);
    return result;
}

esp_err_t parser_ota_info(char **version, char **ota_url) {
    esp_err_t ret = ESP_FAIL;
    char *response_data = NULL;
    int code = -1;
    cJSON *root = NULL;
    if (api_ota_info(&response_data) == ESP_OK && response_data != NULL) {
        ESP_LOGI(TAG, "parser_ota_info response_data:%s", response_data);

        root = cJSON_Parse(response_data);
        if (root == NULL) {
            ESP_LOGE(TAG, "Failed to parse JSON");
            free(response_data);
            return ESP_FAIL;
        }

        const cJSON *jCode = cJSON_GetObjectItem(root, "code");
        if (cJSON_IsNumber(jCode)) {
            code = jCode->valueint;
            if (code != 0) {
                ESP_LOGE(TAG, "https_download_account_file Err code %d", code);
                goto EXIT;
            }
        }

        const cJSON *result = cJSON_GetObjectItem(root, "result");
        if (!cJSON_IsObject(result)) {
            ESP_LOGE(TAG, "No 'result' object in JSON");
            goto EXIT;
        }

        const cJSON *firmwareVersion = cJSON_GetObjectItem(result, "firmwareVersion");
        if (cJSON_IsString(firmwareVersion)) {
            *version = strdup_spiram(firmwareVersion->valuestring);
            ESP_LOGI(TAG, "version: %s", *version);
        } else {
            ESP_LOGE(TAG, "parser fail version");
            goto EXIT;;
        }

        const cJSON *url = cJSON_GetObjectItem(result, "url");
        if (cJSON_IsString(url)) {
            *ota_url = strdup_spiram(url->valuestring);
            ESP_LOGI(TAG, "ota_url: %s", *ota_url);
        } else {
            ESP_LOGE(TAG, "parser fail ota_url");
            goto EXIT;
        }
        ret = ESP_OK;
    }
    EXIT:
        cJSON_Delete(root);
    if (response_data) {
        free(response_data);
        response_data = NULL;
    }

    ESP_LOGI(TAG, "Finish parser_ota_info");
    return ret;
}

//
esp_err_t api_ota_resource_info(char **response_data) {

    int domain = DOMAIN_PRODUCTION;
    if (s3_nvs_get(NVS_S3_SW_CLOUD_DOMAIN, &domain) != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed to get DOMAIN from NVS, using default value");
        domain = DOMAIN_PRODUCTION;
    }

    char tmp_url[128] = {0};
    sprintf(tmp_url, "https://%s/api/v1/otas/?model=ITC9_RESOURCE", ota_domain_str[domain]);
    ESP_LOGI(TAG, "url:%s", tmp_url);
    esp_http_client_config_t config = {
        .url = tmp_url,
        .method = HTTP_METHOD_GET,
        .event_handler = _http_event_handler,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .disable_auto_redirect = true,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    esp_http_client_set_header(client, "Authorization", ota_Basic_Authorization[domain]);
    esp_err_t result = esp_http_client_perform(client);
    ESP_LOGI(TAG, "HTTPS Status = %d", esp_http_client_get_status_code(client));
    if (dynamic_buffer != NULL) {
        *response_data = strdup_spiram(dynamic_buffer);
        free(dynamic_buffer);
        dynamic_buffer = NULL;
    }
    buffer_len = 0;
    esp_http_client_cleanup(client);
    return result;
}

esp_err_t parser_ota_resource_info(char **version, char **ota_url) {
    esp_err_t ret = ESP_FAIL;
    char *response_data = NULL;
    int code = -1;
    cJSON *root = NULL;
    if (api_ota_resource_info(&response_data) == ESP_OK && response_data != NULL) {
        ESP_LOGI(TAG, "parser_ota_resource_info response_data:%s", response_data);

        root = cJSON_Parse(response_data);
        if (root == NULL) {
            ESP_LOGE(TAG, "Failed to parse JSON");
            free(response_data);
            return ESP_FAIL;
        }

        const cJSON *jCode = cJSON_GetObjectItem(root, "code");
        if (cJSON_IsNumber(jCode)) {
            code = jCode->valueint;
            if (code != 0) {
                ESP_LOGE(TAG, "https_download_account_file Err code %d", code);
                goto EXIT;
            }
        }

        const cJSON *result = cJSON_GetObjectItem(root, "result");
        if (!cJSON_IsObject(result)) {
            ESP_LOGE(TAG, "No 'result' object in JSON");
            goto EXIT;
        }

        const cJSON *firmwareVersion = cJSON_GetObjectItem(result, "firmwareVersion");
        if (cJSON_IsString(firmwareVersion)) {
            *version = strdup_spiram(firmwareVersion->valuestring);
            ESP_LOGI(TAG, "version: %s", *version);
        } else {
            ESP_LOGE(TAG, "parser fail version");
            goto EXIT;;
        }

        const cJSON *url = cJSON_GetObjectItem(result, "url");
        if (cJSON_IsString(url)) {
            *ota_url = strdup_spiram(url->valuestring);
            ESP_LOGI(TAG, "ota_url: %s", *ota_url);
        } else {
            ESP_LOGE(TAG, "parser fail ota_url");
            goto EXIT;
        }
        ret = ESP_OK;
    }
    EXIT:
        cJSON_Delete(root);
    if (response_data) {
        free(response_data);
        response_data = NULL;
    }

    ESP_LOGI(TAG, "Finish parser_ota_resource_info");
    return ret;
}

esp_err_t s3_cloud_upload_tracking_info(const char *tracking_data) {
    char *post_url = NULL;
    char *auth_token = NULL;
    esp_err_t ret = ESP_FAIL;

    // Step 1: Get the upload URL and token
    int domain = DOMAIN_PRODUCTION;
    if (s3_nvs_get(NVS_S3_SW_CLOUD_DOMAIN, &domain) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to get DOMAIN from NVS, using default value");
        domain = DOMAIN_PRODUCTION;
    }

    char tmp_url[256] = {0};
    sprintf(tmp_url, "https://%s/client_service/api/v1/contents/tracking_info", s3_domain_str[domain]);

    esp_http_client_config_t config_get = {
        .url = tmp_url,
        .method = HTTP_METHOD_GET,
        .event_handler = _http_event_handler,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .disable_auto_redirect = true,
    };

    esp_http_client_handle_t client_get = esp_http_client_init(&config_get);
    esp_http_client_set_header(client_get, "Authorization", s3_Basic_Authorization[domain]);
    
    if (esp_http_client_perform(client_get) == ESP_OK) {
        ESP_LOGI(TAG, "s3_cloud_upload_tracking_info: GET request successful, status = %d", esp_http_client_get_status_code(client_get));
        if (dynamic_buffer != NULL) {
            cJSON *root = cJSON_Parse(dynamic_buffer);
            if (root) {
                cJSON *result = cJSON_GetObjectItem(root, "result");
                if (result) {
                    cJSON *url_item = cJSON_GetObjectItem(result, "url");
                    cJSON *token_item = cJSON_GetObjectItem(result, "token");
                    if (cJSON_IsString(url_item) && cJSON_IsString(token_item)) {
                        post_url = strdup_spiram(url_item->valuestring);
                        auth_token = strdup_spiram(token_item->valuestring);
                        ret = ESP_OK;
                    }
                }
                cJSON_Delete(root);
            }
        }
    } else {
        ESP_LOGE(TAG, "s3_cloud_upload_tracking_info: GET request failed");
    }

    esp_http_client_cleanup(client_get);
    if (dynamic_buffer) {
        free(dynamic_buffer);
        dynamic_buffer = NULL;
        buffer_len = 0;
    }

    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to get upload URL or token.");
        return ret;
    }

    // Step 2: Post the tracking data
    ESP_LOGI(TAG, "Uploading tracking info to: %s", post_url);
    esp_http_client_config_t config_post = {
        .url = post_url,
        .method = HTTP_METHOD_POST,
        .event_handler = _http_event_handler,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .disable_auto_redirect = true,
    };

    esp_http_client_handle_t client_post = esp_http_client_init(&config_post);
    esp_http_client_set_header(client_post, "Content-Type", "application/json");
    esp_http_client_set_header(client_post, "Authorization", auth_token);
    esp_http_client_set_post_field(client_post, tracking_data, strlen(tracking_data));

    if (esp_http_client_perform(client_post) == ESP_OK) {
        ESP_LOGI(TAG, "s3_cloud_upload_tracking_info: POST request successful, status = %d", esp_http_client_get_status_code(client_post));
        ret = ESP_OK;
    } else {
        ESP_LOGE(TAG, "s3_cloud_upload_tracking_info: POST request failed, status = %d", esp_http_client_get_status_code(client_post));
        ret = ESP_FAIL;
    }

    esp_http_client_cleanup(client_post);
    if(post_url)
    	free(post_url);
    if (auth_token)
    	free(auth_token);
    if (dynamic_buffer) {
        free(dynamic_buffer);
        dynamic_buffer = NULL;
        buffer_len = 0;
    }

    return ret;
}
