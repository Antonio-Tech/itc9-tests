#include <string.h>

#include "WiFi.h"
#include "storage.h"
#include "sntp_syncer.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_wifi_default.h"
#include "esp_http_server.h"
#include "esp_event.h"
#include "esp_wifi.h"
#include "esp_wifi_types_generic.h"
#include "esp_mac.h"
#include "esp_log.h"
#include "esp_coexist.h"
#include "esp_heap_caps.h"
#include "cJSON.h"

#include "lwip/err.h"
#include "lwip/sys.h"

#include "s3_https_cloud.h"
#include "manual_ota.h"
#include "s3_sync_account_contents.h"
#include "esp_ota_ops.h"
#include "app_timeout.h"
#include "alarm_mgr.h"
#include "s3_album_mgr.h"
#include "lv_screen_mgr.h"
#include "audio_player.h"
#include <dirent.h>
#include "s3_tracking.h"
#include "s3_definitions.h"
#include "s3_bluetooth.h"  // For BLE/BT coexistence coordination
#include "s3_logger.h"


#define MIN(a,b) (((a)<(b))?(a):(b))

#define POST_RECEIVED_BIT BIT0
#define WIFI_PAITRING_TIMEOUT (1 * 60 * 1000)

// CLOUD STATUS CODES
#define BOUND_BY_OTHERS_CODE (10034)

typedef enum
{
    WIFI_ERROR_TIMEOUT = 0,
    WIFI_ERROR_DISCONNECT,
    WIFI_ERROR_DATA_SYNC_FAIL,
    WIFI_ERROR_NO_FIRMWARE,

    WIFI_ERROR_UNKNOWN_ERROR,
}wifi_exception_screen_e;

// HTTP RELATED ------------------------------------------
#define HTTP_CONTENT_SIZE (200)
#define HTTP_PORT (33467)

static EventGroupHandle_t wifi_event_group = NULL;
static esp_netif_t *esp_netif_sta_handle = NULL;
static int connection_tries = 0;

extern bool gWiFi_SYNC_USER_INTERRUPT;
extern bool s3_show_default_syncUp;
extern bool s3_wifi_downloading;

TaskHandle_t wifi_connecting_task_handle = NULL; // Made non-static for BLE status query

// Track if we disconnected BT Classic to free DMA for WiFi, so we can reconnect later
static bool s_bt_was_disconnected_for_wifi = false;

// Add missing global variables for WiFi Access Point functionality
static EventGroupHandle_t s_web_event_group = NULL;
static httpd_handle_t s3_http_server_handler = NULL;
static bool s_post_received_flag = false;
static bool ap_is_on = false;
static TaskHandle_t wifi_pairing_task_handle = NULL;

// Global flag to skip OTA updates on next sync
static bool skip_ota_flag = false;

// Global flag to track OTA progress (exposed for BLE status query)
bool gOTA_in_progress = false;

// DMA usage monitoring (no hard limits - collect real data)
// Previous observation: system was stable at 98-99% DMA usage

static const char *TAG = "WIFI";

void notify_post_received_event(void);
esp_err_t deinit_wifi_access_point(void);
httpd_handle_t start_webserver(void);
esp_err_t stop_webserver(void);
void stop_wifi_pairing_task(void *arg);
static void get_dma_usage(size_t *used_kb, int *percent);

// Using proper header from lv_screen_mgr.h
// extern void set_current_screen(s3_screens_t current_screen, s3_screens_t next_screen);

// WIFI STATION RELATED ============================================================================
void wifi_station_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id,void *event_data)
{
    if(event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START)
    {
        ESP_LOGI(TAG, "Connecting to AP...");
        esp_wifi_connect();
    }
    else if(event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED)
    {
        if(connection_tries < MAX_CONNECT_TRIES)
        {
            ESP_LOGI(TAG, "reconnecting to AP...");
            esp_wifi_connect();
            connection_tries++;
        }
        else
        {
            xEventGroupSetBits(wifi_event_group, WIFI_FAILURE_ON_CONNECT);
        }
    }
    else if(event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_CONNECTED)
    {
        ESP_LOGI(TAG, "Device conected");
        // Log current RSSI and bandwidth once connected
        wifi_ap_record_t ap_info;
        if (esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK) {
            wifi_bandwidth_t bw;
            if (esp_wifi_get_bandwidth(WIFI_IF_STA, &bw) == ESP_OK) {
                ESP_LOGI(TAG, "RSSI: %d dBm, Bandwidth: %s", ap_info.rssi, bw == WIFI_BW_HT40 ? "HT40" : "HT20");
            } else {
                ESP_LOGI(TAG, "RSSI: %d dBm", ap_info.rssi);
            }
        }
    }
    else if(event_base == WIFI_EVENT)
    {
        ESP_LOGW(TAG, "[BUG_FIX] Unhandled WiFi event_id: %ld", event_id);
    }
}

void ip_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data)
{
    if(event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP)
    {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *) event_data;
        ESP_LOGI(TAG, "STA IP: " IPSTR, IP2STR(&event->ip_info.ip));
        connection_tries = 0;
        xEventGroupSetBits(wifi_event_group, WIFI_SUCCESS_ON_CONNECT);
    }
}

/* ========================= MEMORY OPTIMIZATION FOR WIFI ========================= */
/**
 * @brief Suspend audio tasks and free maximum DMA memory for WiFi initialization
 * 
 * Aggressively free DMA memory from audio pipelines, LVGL buffers, and other
 * non-critical components to maximize available DMA for WiFi (~83KB requirement).
 */
static void suspend_audio_tasks_for_wifi(void) {
    ESP_LOGI(TAG, "Suspending audio tasks to free DMA RAM for WiFi initialization");
    
    // Check if audio system is initialized before trying to use it
    if (!is_powered_on) {
        ESP_LOGI(TAG, "Audio system not initialized - no audio tasks to suspend");
        return;
    }
    
    // Stop any ongoing audio playback to release resources
    if (is_audio_playing()) {
        ESP_LOGI(TAG, "Stopping audio playback before system shutdown");
        play_stop();
        vTaskDelay(pdMS_TO_TICKS(100)); // Allow stop to complete
    }
    
    // Free audio pipeline DMA memory without full system shutdown
    ESP_LOGI(TAG, "Cleaning up audio pipelines to free DMA memory for WiFi");
    cleanup_persistent_i2s_element();  // Free I2S DMA buffers
    vTaskDelay(pdMS_TO_TICKS(100)); // Allow DMA cleanup to complete
    
    ESP_LOGI(TAG, "Audio pipeline DMA memory freed for WiFi");
}

/**
 * @brief Resume audio tasks after WiFi initialization completes
 */
static void resume_audio_tasks_after_wifi(void) {
    ESP_LOGI(TAG, "Resuming audio tasks after WiFi initialization");
    
    // Reinitialize audio pipelines that were cleaned up for WiFi DMA
    ESP_LOGI(TAG, "Reinitializing audio pipelines after WiFi initialization");
    init_persistent_i2s_element();  // Recreate I2S DMA buffers
    vTaskDelay(pdMS_TO_TICKS(50)); // Allow I2S element to initialize
    
    // Audio pipelines are now ready - no need to resume playback automatically
    // User will trigger playback when needed
    ESP_LOGI(TAG, "Audio pipelines restored and ready for use");
}

esp_err_t connect_wifi(const char *wifi_ssid, const char *wifi_password, bool use_four_tries) {
    ESP_LOGI(TAG, "ENTERED FUNCTION: %s", __func__);

    esp_err_t status = WIFI_FAILURE_ON_CONNECT;

    esp_event_handler_instance_t wifi_handler_event_instance;
    esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_station_event_handler, NULL, &wifi_handler_event_instance);

    esp_event_handler_instance_t got_ip_event_instance;
    esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &ip_event_handler, NULL, &got_ip_event_instance);

    wifi_config_t wifi_configs = {
            .sta =
                    {
                            .ssid = "",
                            .password = "",
                            .threshold.authmode = WIFI_AUTH_WPA_WPA2_PSK,
                            .pmf_cfg = {.capable = true, .required = false},
                    },
    };
    strncpy((char *) wifi_configs.sta.ssid, wifi_ssid, sizeof(wifi_configs.sta.ssid));
    strncpy((char *) wifi_configs.sta.password, wifi_password, sizeof(wifi_configs.sta.password));

    esp_wifi_set_mode(WIFI_MODE_STA);
    // Prefer HT40 for higher throughput when AP supports it
    esp_wifi_set_bandwidth(WIFI_IF_STA, WIFI_BW_HT40);
    esp_wifi_set_config(WIFI_IF_STA, &wifi_configs);
    esp_wifi_start();

    ESP_LOGI(TAG, "Wifi driver initialized on station mode!");
    int retry_count = 0;
    int max_retries = use_four_tries ? MAX_CONNECT_TRIES : 1;

    while (retry_count < max_retries) {
        xEventGroupClearBits(wifi_event_group, WIFI_SUCCESS_ON_CONNECT | WIFI_FAILURE_ON_CONNECT);
        ESP_LOGI(TAG, "Attempting Wi-Fi connection (Try %d/%d)...", retry_count + 1, max_retries);
        // set_pixsee_status(S3ER_SETUP_CONNECT_FAIL + retry_count); // Use displaced error codes for retries

        esp_wifi_disconnect();
        esp_wifi_connect();
        // Wait 10 seconds for connection attempt (reduced from 30s to minimize timeout on unavailable networks)
        EventBits_t wifi_flag_bits =
                xEventGroupWaitBits(wifi_event_group, WIFI_SUCCESS_ON_CONNECT | WIFI_FAILURE_ON_CONNECT, pdFALSE, pdFALSE, pdMS_TO_TICKS(10000));

        if (wifi_flag_bits & WIFI_SUCCESS_ON_CONNECT) {
            ESP_LOGI(TAG, "Connected to AP!\r\n\t\tSSID: %s\r\n\t\tPASSWORD: %s", wifi_configs.sta.ssid, wifi_configs.sta.password);
            status = WIFI_SUCCESS_ON_CONNECT;
            set_pixsee_status(S3ER_SETUP_CONNECT_SUCCESS);

            // Set BLE priority for coexistence to allow BLE connections during WiFi operations
            ESP_LOGI(TAG, "Setting BLE coexistence priority");
            esp_coex_preference_set(ESP_COEX_PREFER_BT);

            break;
        } else if (wifi_flag_bits & WIFI_FAILURE_ON_CONNECT) {
            ESP_LOGI(TAG, "Fail to connect to AP");
            status = WIFI_FAILURE_ON_CONNECT;
        } else {
            ESP_LOGI(TAG, "Event unknown");
            status = WIFI_FAILURE_ON_CONNECT;
        }

        retry_count++;
        connection_tries = 0;
    }

    if (status != WIFI_SUCCESS_ON_CONNECT) {
        set_pixsee_status(S3ER_SETUP_CONNECT_FAIL); // Send error to ble
        vTaskDelay(pdMS_TO_TICKS(2 * 1000));  // Allow BLE to process error
    } else {
        set_pixsee_status(S3ER_SETUP_CONNECT_SUCCESS);  // Send success to ble
    }

    // FIX: Remove redundant second wait that causes infinite hang after successful connection
    // The connection status is already determined by the retry loop above
    ESP_LOGI(TAG, "connect_wifi returning with status: %d (1=SUCCESS, 2=FAILURE)", status);

    // Stop WiFi connection attempts before unregistering handlers to prevent background reconnect loops
    if (status != WIFI_SUCCESS_ON_CONNECT) {
        ESP_LOGI(TAG, "Stopping WiFi connection attempts");
        esp_wifi_disconnect();
        esp_wifi_stop();
        vTaskDelay(pdMS_TO_TICKS(100)); // Allow WiFi to stop cleanly
    }

    esp_event_handler_instance_unregister(IP_EVENT, IP_EVENT_STA_GOT_IP, got_ip_event_instance);
    esp_event_handler_instance_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_handler_event_instance);
    if (wifi_event_group != NULL) {
        vEventGroupDelete(wifi_event_group);
        wifi_event_group = NULL;
    }

    connection_tries = 0;
    return status;
}

esp_err_t init_wifi_station(bool sync_mode)
{
    ESP_LOGD(TAG, "ENTERED FUNCTION: %s (sync_mode=%s)", __func__, sync_mode ? "SYNC" : "NORMAL");

    // If already connected, skip reinitialization
    if (is_wifi_connected()) {
        ESP_LOGI(TAG, "WiFi already connected - skipping initialization");
        return ESP_OK;
    }

    // DIAGNOSTIC: Check if WiFi is still initialized from previous attempt
    esp_err_t wifi_status = esp_wifi_stop();  // Try to stop - will fail if not initialized
    if (wifi_status == ESP_OK) {
        ESP_LOGW(TAG, "[DIAG] WiFi was still running from previous attempt - cleaning up");
        esp_wifi_deinit();  // Force cleanup
        vTaskDelay(pdMS_TO_TICKS(200));
    } else if (wifi_status != ESP_ERR_WIFI_NOT_INIT) {
        ESP_LOGW(TAG, "[DIAG] Unexpected WiFi state: %s - attempting cleanup", esp_err_to_name(wifi_status));
    }

    // Clean up any existing WiFi state
    if (!sync_mode) {
        deinit_wifi_station();
        vTaskDelay(pdMS_TO_TICKS(100));
    } else {
        ESP_LOGI(TAG, "[DIAG] sync_mode=true - skipping deinit_wifi_station()");
    }

    // Free audio memory before WiFi initialization to prevent ESP_ERR_NO_MEM
    suspend_audio_tasks_for_wifi();

    // Initialize NVS if needed
    init_nvs();

    // If BLE is connected, prefer BT in coexistence so BLE stays stable during WiFi
    if (s3_ble_manager_is_connected()) {
        ESP_LOGI(TAG, "BLE connected -> setting coexistence to prefer BT for WiFi operations");
        esp_coex_preference_set(ESP_COEX_PREFER_BT);
    }

    // If BT Classic is connected, disconnect it to free DMA for WiFi (will restore later)
    if (s3_bt_classic_is_connected()) {
        ESP_LOGW(TAG, "BT Classic connected -> disconnecting temporarily to free DMA for WiFi");
        s_bt_was_disconnected_for_wifi = true;
        bt_manager_disconnect();
        
        // Wait for BT deinitialization to complete (up to 6 seconds)
        int wait_count = 0;
        const int max_wait_ms = 6000;
        const int check_interval_ms = 100;
        
        ESP_LOGI(TAG, "Waiting for BT deinitialization to complete...");
        while (bt_manager_get_status() != BT_STATUS_OFF && wait_count < max_wait_ms) {
            vTaskDelay(pdMS_TO_TICKS(check_interval_ms));
            wait_count += check_interval_ms;
        }
        
        if (bt_manager_get_status() == BT_STATUS_OFF) {
            ESP_LOGI(TAG, "BT deinitialization completed after %d ms", wait_count);
        } else {
            ESP_LOGW(TAG, "BT deinitialization timeout after %d ms - proceeding anyway", wait_count);
        }
    } else {
        s_bt_was_disconnected_for_wifi = false;
    }
    
    // Measure DMA usage AFTER all cleanup (audio + BT Classic)
    size_t dma_before_kb;
    int dma_before_percent;
    get_dma_usage(&dma_before_kb, &dma_before_percent);
    ESP_LOGI(TAG, "DMA after ALL cleanup, before WiFi init: %d KB (%d%%)", (int)dma_before_kb, dma_before_percent);

    // Initialize network interface
    esp_err_t ret = esp_netif_init();
    if(ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {  // ESP_ERR_INVALID_STATE = already initialized
        ESP_LOGE(TAG, "Fail to initialize network infrastructure: %s", esp_err_to_name(ret));
        return ret;
    }

    // Use default WiFi configuration from sdkconfig.defaults
    // This provides proper buffer sizes for WiFi + SDMMC coexistence
    wifi_init_config_t wifi_conf = WIFI_INIT_CONFIG_DEFAULT();

    // FUTURE STRATEGY: Dynamic WiFi buffer allocation based on system usage
    // - System idle: Use large buffers for fast downloads (sdkconfig values)
    // - User active: Switch to minimal buffers, slower but allows user interaction
    // - User idle again: Reallocate large buffers
    // TODO: Implement dynamic reallocation strategy when needed

    // Minimal WiFi buffer configuration (COMMENTED OUT - causes SDMMC DMA conflicts)
    // wifi_conf.static_rx_buf_num = 2;    // Minimum functional value (was 1 - insufficient)
    // wifi_conf.dynamic_rx_buf_num = 4;   // Minimum practical
    // wifi_conf.tx_buf_type = 1;          // Static TX buffers (type 0 was failing)
    // wifi_conf.static_tx_buf_num = 2;    // Minimum static TX (0 was invalid)
    // wifi_conf.dynamic_tx_buf_num = 1;   // Minimum required (0 is invalid)
    // wifi_conf.rx_mgmt_buf_num = 2;      // Minimum for management frames
    // wifi_conf.cache_tx_buf_num = 16;    // Use minimum allowed value

    ESP_LOGI(TAG, "Using WiFi buffer configuration from sdkconfig.defaults");

    ret = esp_wifi_init(&wifi_conf);
    if(ret != ESP_OK)
    {
        ESP_LOGE(TAG, "Fail to initialize wifi: %s", esp_err_to_name(ret));
        
        // DIAGNOSTIC: Log detailed state to identify retry without cleanup
        size_t dma_fail_kb;
        int dma_fail_percent;
        get_dma_usage(&dma_fail_kb, &dma_fail_percent);
        ESP_LOGE(TAG, "[DIAG] WiFi init failed - sync_mode=%s, DMA=%dKB (%d%%), BT_connected=%d, BLE_connected=%d",
                 sync_mode ? "SYNC" : "NORMAL", 
                 (int)dma_fail_kb, (int)dma_fail_percent,
                 s3_bt_classic_is_connected(), s3_ble_manager_is_connected());
        
        // DIAGNOSTIC: Check if this is a retry without cleanup
        if (sync_mode && wifi_status == ESP_OK) {
            ESP_LOGE(TAG, "[DIAG] POSSIBLE ROOT CAUSE: WiFi retry with sync_mode=true after previous failure (incomplete cleanup)");
        }
        
        return ret;
    }

    if (wifi_event_group == NULL)
        wifi_event_group = xEventGroupCreate();
    ESP_LOGI(TAG, "Wifi initialized!");

    if (esp_netif_sta_handle == NULL)
        esp_netif_sta_handle = esp_netif_create_default_wifi_sta();
    
    // Measure actual WiFi DMA usage
    size_t dma_after_kb;
    int dma_after_percent;
    get_dma_usage(&dma_after_kb, &dma_after_percent);
    int wifi_dma_actual = (int)(dma_after_kb - dma_before_kb);
    ESP_LOGI(TAG, "DMA after WiFi init: %d KB (%d%%) - WiFi used: %d KB", 
             (int)dma_after_kb, dma_after_percent, wifi_dma_actual);

    return ESP_OK;
}

esp_err_t deinit_wifi_station(void)
{
	// Disconnect first to allow clean shutdown
	esp_err_t ret = esp_wifi_disconnect();
	if (ret != ESP_OK && ret != ESP_ERR_WIFI_NOT_CONNECT) {
		ESP_LOGW(TAG, "WiFi disconnect warning: %s", esp_err_to_name(ret));
	}

	// Allow time for proper disconnection
	vTaskDelay(pdMS_TO_TICKS(100));

	// Stop WiFi
	ret = esp_wifi_stop();
	if (ret != ESP_OK) {
		ESP_LOGW(TAG, "WiFi stop warning: %s", esp_err_to_name(ret));
	}

	// Reset coexistence to give BLE full priority after WiFi stops
	ESP_LOGI(TAG, "[BUG_FIX] Resetting coexistence to prefer BT after WiFi stop");
	esp_coex_preference_set(ESP_COEX_PREFER_BT);

	// Deinitialize WiFi driver
	ret = esp_wifi_deinit();
	if (ret != ESP_OK) {
		ESP_LOGW(TAG, "WiFi deinit warning: %s", esp_err_to_name(ret));
	}

    // Properly delete the event group if it exists
    if (wifi_event_group) {
        vEventGroupDelete(wifi_event_group);
        wifi_event_group = NULL;
    }

    if (esp_netif_sta_handle) {
        esp_netif_destroy(esp_netif_sta_handle);
        esp_netif_sta_handle = NULL;
    }

	// Additional cleanup delay to ensure hardware settles
	// This helps prevent interference with NFC operations
	vTaskDelay(pdMS_TO_TICKS(200));
	
	ESP_LOGI(TAG, "WiFi station fully deinitialized");
	return ESP_OK;
}

esp_err_t disconnect_wifi_with_cleanup(void)
{
	// Lightweight WiFi disconnect without driver deinitialization
	// This avoids power management issues that cause screen blinks

	ESP_LOGI(TAG, "Performing lightweight WiFi disconnect for memory cleanup");

	// Disconnect first to allow clean shutdown
	esp_err_t ret = esp_wifi_disconnect();
	if (ret != ESP_OK && ret != ESP_ERR_WIFI_NOT_CONNECT) {
		ESP_LOGW(TAG, "WiFi disconnect warning: %s", esp_err_to_name(ret));
	}

	// Allow time for proper disconnection
	vTaskDelay(pdMS_TO_TICKS(100));

	// Stop WiFi to free DMA memory but keep driver initialized
	ret = esp_wifi_stop();
	if (ret != ESP_OK && ret != ESP_ERR_WIFI_NOT_INIT) {
		ESP_LOGW(TAG, "WiFi stop warning: %s", esp_err_to_name(ret));
	}

	// Reset coexistence to give BLE full priority after WiFi stops
	ESP_LOGI(TAG, "[BUG_FIX] Resetting coexistence to prefer BT after WiFi stop");
	esp_coex_preference_set(ESP_COEX_PREFER_BT);

	// Note: We do NOT call esp_wifi_deinit() here to avoid display power issues
	// The WiFi driver remains initialized and ready for quick restart

	// Shorter delay since we're not doing full deinitialization
	vTaskDelay(pdMS_TO_TICKS(100));

	ESP_LOGI(TAG, "WiFi disconnected and stopped (driver preserved)");
	return ESP_OK;
}

bool is_wifi_connected(void)
{
	wifi_ap_record_t ap_info;
	esp_err_t ret = esp_wifi_sta_get_ap_info(&ap_info);
	return (ret == ESP_OK);
}

void sntp_sync_task(void *param) {
    char tz[TIMEZONE_STR_SIZE] = {0};  // char tz[TIMEZONE_STR_LEN] = {0};
    if (read_timezone(tz) == ESP_OK) {
        init_sntp(tz);
        wait_for_time_sync();
        deinit_sntp();
    }
    vTaskDelete(NULL);
}

bool version_gt(const char *a, const char *b) {
    if (!a || !b) {
        ESP_LOGW(TAG, "[BUG_FIX] version_gt: NULL version string (a=%p, b=%p)", a, b);
        return false;
    }
    int a_parts[3] = {0}, b_parts[3] = {0};
    sscanf(a, "%d.%d.%d", &a_parts[0], &a_parts[1], &a_parts[2]);
    sscanf(b, "%d.%d.%d", &b_parts[0], &b_parts[1], &b_parts[2]);

    for (int i = 0; i < 3; i++) {
        if (a_parts[i] > b_parts[i]) return true;
        if (a_parts[i] < b_parts[i]) return false;
    }
    return false;
}

// Helper structure to hold file information
typedef struct {
    char name[64];
    time_t mtime;
} file_entry_t;

// Comparison function for qsort
static int compare_files(const void *a, const void *b) {
    file_entry_t *file_a = (file_entry_t *)a;
    file_entry_t *file_b = (file_entry_t *)b;
    return difftime(file_a->mtime, file_b->mtime);
}

static esp_err_t exec_upload_tracking_info(void)
{
    const char *dir_path = "/sdcard/tmp";
    DIR *dir = opendir(dir_path);
    if (!dir) {
        ESP_LOGE(TAG, "Failed to open directory: %s", dir_path);
        return ESP_FAIL;
    }

    file_entry_t *files = NULL;
    int file_count = 0;
    struct dirent *entry = NULL;

    while ((entry = readdir(dir)) != NULL) {
        if (strncmp(entry->d_name, "tracking_", 9) == 0 && strstr(entry->d_name, ".bin") != NULL) {
            files = heap_caps_realloc(files, (file_count + 1) * sizeof(file_entry_t), MALLOC_CAP_SPIRAM);
            if (!files) {
                ESP_LOGE(TAG, "Failed to allocate memory for file list");
                closedir(dir);
                return ESP_ERR_NO_MEM;
            }

            char full_path[256];
            size_t max_name_len = sizeof(full_path) - strlen(dir_path) - 2;
            snprintf(full_path, sizeof(full_path), "%s/%.*s", dir_path, (int)max_name_len, entry->d_name);

            struct stat st;
            if (stat(full_path, &st) == 0) {
                strncpy(files[file_count].name, entry->d_name, sizeof(files[file_count].name) - 1);
                files[file_count].name[sizeof(files[file_count].name) - 1] = '\0';
                files[file_count].mtime = st.st_mtime;
                file_count++;
            }
        }
    }
    closedir(dir);

    if (file_count == 0) {
        ESP_LOGI(TAG, "No tracking files found in %s", dir_path);
        return ESP_OK;
    }

    qsort(files, file_count, sizeof(file_entry_t), compare_files);

    ESP_LOGI(TAG, "Found %d tracking files. Processing in chronological order...", file_count);

    for (int i = 0; i < file_count; i++) {
        char filepath[256];
        snprintf(filepath, sizeof(filepath), "%s/%s", dir_path, files[i].name);
        ESP_LOGI(TAG, "Processing file: %s", filepath);

        TrackingRecord *loaded_records = NULL;
        int record_count = 0;

        if (s3_tracking_load_records_from_file(filepath, &loaded_records, &record_count) == 0) {
            if (record_count > 0) {
                char *json_data = make_json_tracking_messages(loaded_records, record_count);
                ESP_LOGI(TAG, "%s", json_data);
                if (json_data) {
                    if (s3_cloud_upload_tracking_info(json_data) == ESP_OK) {
                        ESP_LOGI(TAG, "Successfully uploaded tracking info from %s", filepath);
                        remove(filepath);
                    } else {
                        ESP_LOGE(TAG, "Failed to upload tracking info from %s", filepath);
                    }
                    free(json_data);
                }
                s3_tracking_free_loaded_records(loaded_records, record_count);
            } else {
                ESP_LOGI(TAG, "No records in %s, deleting empty file.", filepath);
                remove(filepath);
            }
        } else {
            ESP_LOGE(TAG, "Failed to load records from %s", filepath);
        }
    }

    free(files);
    return ESP_OK;
}

// LEGACY: This function is kept for backward compatibility but should use unified_sync_task for new implementations
void wifi_connect_task(void *pvParameters) {
    gWiFi_SYNC_USER_INTERRUPT = false;
    s3_show_default_syncUp = true;
    ESP_LOGI(TAG, "wifi_connect_task");
    //	E (20165) lcd_panel.io.spi: panel_io_spi_tx_color(390): spi transmit (queue) color failed
    //  E (20165) lcd_panel.st7789: panel_st7789_draw_bitmap(225): io tx color failed
    vTaskDelay(pdMS_TO_TICKS(500)); // Avoid updating the UI and enabling Wi-Fi at the same time
    char ssid[WIFI_SSID_SIZE] = {0};
    char pass[WIFI_PASSWORD_SIZE] = {0};
    char tz[TIMEZONE_STR_SIZE] = {0};
    char secret[SECRET_KEY_STR_SIZE] = {0};
    const char *msg = NULL;
    bool success = false;
    int oob_status = 0;
    esp_err_t ret = ESP_FAIL;
    int biding_code = -1;

    read_oob_status(&oob_status);
    int i = 0;

    app_timeout_stop();
    stop_alarm_timer();
    if (read_wifi_credentials(ssid, pass) != ESP_OK) {
        msg = "Fail to access credentials file";
        goto finish;
    }

    ESP_LOGI(TAG, "[1.0] connect_wifi");
    init_wifi_station(true);  // This is a sync operation - use emergency cleanup
    if (connect_wifi(ssid, pass, WIFI_CMD) != WIFI_SUCCESS_ON_CONNECT) {
        msg = "Fail to connect to Wi-Fi";
        goto finish;
    }

    set_current_screen(DATA_SYNC_SCREEN, NULL_SCREEN);
    ESP_LOGI(TAG, "[2.0] sntp ");
    ESP_LOGI(TAG, "Available heap: %u, SPIRAM: %u", heap_caps_get_free_size(MALLOC_CAP_8BIT), heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    if (read_timezone(tz) == ESP_OK) {
        init_sntp(tz);
        esp_err_t sntp_result = wait_for_time_sync();
        deinit_sntp();
        if (sntp_result != ESP_OK) {
            goto finish;
        }
    }

    ESP_LOGI(TAG, "[3.0] oob ");
    ESP_LOGI(TAG, "Available heap: %u, SPIRAM: %u", heap_caps_get_free_size(MALLOC_CAP_8BIT), heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    if (oob_status == 0) {
        ESP_LOGI(TAG, "[3.1] OOB==0 -> binding ");
        if (read_secret_key(secret) == ESP_OK) {
            if (cei_complete_binding_of_device(&biding_code) == ESP_OK) {
                msg = "Success on cloud binding";
                oob_status = 1;
                write_oob_status(&oob_status);
                ESP_LOGW(TAG, "binding success oob = 1");
            } else {
                msg = "Fail on cloud binding";
                ESP_LOGE(TAG, "binding fail");
                goto finish;
            }
        }
    } else {
        ESP_LOGI(TAG, "3.2 OOB==1 -> skip binding ");
    }

    // update ui to sync-up
    ESP_LOGI(TAG, "[4.0] ui DATA_SYNC_SCREEN");
    set_current_screen(DATA_SYNC_SCREEN, NULL_SCREEN);

    // dl resource non mp3
    char *resource_version = NULL;
    char *resource_url = NULL;
    parser_ota_resource_info(&resource_version, &resource_url);
    char tmp[16];
    read_resource_version_or_default(tmp, sizeof(tmp));
    ESP_LOGI(TAG, "[4.1] check resource version remote:%s,local:%s",
             resource_version ? resource_version : "NULL", tmp);
    bool doResource = version_gt(resource_version, tmp);
    if (doResource) {
        for (i = 0; i < 2; i++) {
            if (sync_resource_without_mp3(resource_url,i) == ESP_OK) {
                write_resource_version_to_file(resource_version);
                ESP_LOGW(TAG, "[4.1] sync success write_resource_version_to_file %s",
                         resource_version ? resource_version : "NULL");
                break;
            }
            ESP_LOGI(TAG, "ret ry %d", i);
        }
    }
    if (resource_version)
        free(resource_version);
    if (resource_url)
        free(resource_url);
    set_current_screen(DATA_SYNC_SCREEN, NULL_SCREEN);

    ESP_LOGI(TAG, "[5.0] fw version api");
    const esp_partition_t *running = esp_ota_get_running_partition();
    esp_app_desc_t app_desc;
    esp_ota_get_partition_description(running, &app_desc);
    char *version = NULL;
    char *ota_url = NULL;
    parser_ota_info(&version, &ota_url);
    bool doOTA = version_gt(version, app_desc.version + 1);
    ESP_LOGI(TAG, "[5.1] doOTA=%d, rVersion=%s,lVersion=%s", doOTA,
             version ? version : "NULL", app_desc.version + 1);
    if (doOTA) {
        ESP_LOGI(TAG, "[5.2] battery check %d > 2, charger=%d ? ", s3_battery_level, s3_charger_status);
        if (s3_battery_level > 2 || s3_charger_status == BATTERY_CHARGE) {
            set_current_screen(OTA_SCREEN, NULL_SCREEN);
            for (int i = 0; i < 4; i++) {
                ret = OTA_Update(ota_url);
                if (ret == ESP_OK)
                    break;
                vTaskDelay(pdMS_TO_TICKS(100));
                ESP_LOGE(TAG, " ota retry =%d", i);
            }
        } else {
            set_current_screen(WIFI_PLUG_IN_SCREEN, NULL_SCREEN);
            while (!gWiFi_SYNC_USER_INTERRUPT && s3_charger_status != BATTERY_CHARGE) {
                vTaskDelay(pdMS_TO_TICKS(500));
            }
            if (s3_charger_status != BATTERY_CHARGE) {
                ESP_LOGI(TAG, "[5.1] s3_charger_status != BATTERY_CHARGE ");
                goto FINISH_WITHOUT_UI;
            } else {
                ESP_LOGI(TAG, "[5.1] BATTERY_CHARGE ");
                set_current_screen(OTA_SCREEN, NULL_SCREEN);
                for (int i = 0; i < 4; i++) {
                    ret = OTA_Update(ota_url);
                    if (ret == ESP_OK)
                        break;
                    vTaskDelay(pdMS_TO_TICKS(100));
                    ESP_LOGE(TAG, " ota retry =%d", i);
                }
            }
        }
    }
    if (version)
        free(version);
    if (ota_url)
        free(ota_url);
    if (gWiFi_SYNC_USER_INTERRUPT == true)
        goto finish;

    s3_show_default_syncUp = false;
    set_current_screen(DATA_SYNC_SCREEN, NULL_SCREEN);
    ESP_LOGI(TAG, "[7.1] DATA_SYNC_SCREEN ");
    i = 0;
    while (!gWiFi_SYNC_USER_INTERRUPT) {
        ret = https_download_account_file(NULL);
        if (ret == ESP_OK || i > 2)
            break;
        i++;
        ESP_LOGE(TAG, " https_download_account_file retry =%d", i);
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    if (ret != ESP_OK || gWiFi_SYNC_USER_INTERRUPT)
        goto finish;

    ESP_LOGI(TAG, "[7.2] cei_upload_device_info");
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    char patch_data[256] = {0};
    sprintf(patch_data, "{\"battery\":%d,\"wifi\":\"%s\",\"fwVersion\":\"%s\",\"mac\":\"%02x:%02x:%02x:%02x:%02x:%02x\"}", s3_battery_level, ssid,
            app_desc.version, mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    ESP_LOGI(TAG, "patch_data:%s", patch_data);
    for (int i = 0; i < 3; i++) {
        ret = cei_upload_device_info(patch_data);
        if (ret == ESP_OK)
            break;
        vTaskDelay(pdMS_TO_TICKS(100));
        ESP_LOGE(TAG, " ota retry =%d", i);
    }

    ESP_LOGI(TAG, "[7.2] s3_cloud_upload_tracking_info");
    exec_upload_tracking_info();

    ESP_LOGI(TAG, "[8.1] parser_and_contents_sync");
    ret = parser_account_contents(PARSE_AND_DOWNLOAD);
    if (ret == ESP_OK) {
        s3_albums_dynamic_build();
        success = true;
    }

finish:
    if (msg) {
        ESP_LOGI(TAG, "[8.0] wifi_connect_task end: %s", msg);
    } else {
        ESP_LOGI(TAG, "[8.0] wifi_connect_task end");
    }
    if (!gWiFi_SYNC_USER_INTERRUPT) {
        if (success) {
            set_current_screen(WIFI_SYNCED_SCREEN, WIFI_DISCONNECT_SCREEN);
        } else {
            if (oob_status == 0) {
                set_current_screen(WIFI_UNKNOWN_SCREEN, HOME_SCREEN);
            } else {
                set_current_screen(WIFI_DISCONNECT_SCREEN, NULL_SCREEN);
            }
        }
    }
    app_timeout_init();

FINISH_WITHOUT_UI:
    ESP_LOGI(TAG, "[8.1] WIFI_DEINIT");
    get_alarm_setting(TIMER_SOURCE_ESP_TIMER);
    wifi_connecting_task_handle = NULL;
    deinit_wifi_station();
    gWiFi_SYNC_USER_INTERRUPT = true;
    s3_show_default_syncUp = false;
    vTaskDelete(NULL);
}

bool conn_task_running(void)
{
    if (wifi_connecting_task_handle != NULL)
        return true;
    else
        return false;
}

void start_wifi_connecting(void)
{
    if (wifi_connecting_task_handle == NULL) {
        // Create unified sync parameter for full WiFi sync
        unified_sync_param_t *param = malloc(sizeof(unified_sync_param_t));
        if (param == NULL) {
            ESP_LOGE(TAG, "Failed to allocate memory for WiFi sync params");
            return;
        }

        param->sync_mode = SYNC_MODE_FULL;
        param->callback = NULL; // No callback needed for full sync
        
        xTaskCreatePinnedToCore(unified_sync_task, "unified_sync_task", (12 * 1024), param, 0, &wifi_connecting_task_handle, 1);
    }else {
        ESP_LOGW(TAG, "unified_sync_task is already running.");
    }
}

void start_ble_wifi_sync(void)
{
    if (wifi_connecting_task_handle == NULL) {
        // Create unified sync parameter for BLE-triggered WiFi sync
        unified_sync_param_t *param = malloc(sizeof(unified_sync_param_t));
        if (param == NULL) {
            ESP_LOGE(TAG, "Failed to allocate memory for BLE WiFi sync params");
            return;
        }

        param->sync_mode = SYNC_MODE_BLE;
        param->callback = NULL; // No callback needed for BLE sync

        ESP_LOGI(TAG, "Starting BLE-triggered WiFi sync (will return to HOME_SCREEN after completion)");
        xTaskCreatePinnedToCore(unified_sync_task, "unified_sync_task", (12 * 1024), param, 0, &wifi_connecting_task_handle, 1);
    }else {
        ESP_LOGW(TAG, "unified_sync_task is already running.");
    }
}

/**
 * @brief Get current DMA usage in KB and percentage
 */
static void get_dma_usage(size_t *used_kb, int *percent) {
    size_t total_dmaram = heap_caps_get_total_size(MALLOC_CAP_DMA);
    size_t free_dmaram = heap_caps_get_free_size(MALLOC_CAP_DMA);
    size_t used_dmaram = total_dmaram - free_dmaram;
    
    *used_kb = used_dmaram / 1024;
    *percent = (total_dmaram > 0) ? (used_dmaram * 100 / total_dmaram) : 0;
}

// WIFI ACCESS POINT RELATED =======================================================================
static void wifi_ap_event_handler(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data)
{
    if (event_id == WIFI_EVENT_AP_STACONNECTED)
    {
        ESP_LOGI(TAG, "WIFI_EVENT_AP_STACONNECTED");
        s3_http_server_handler = start_webserver();
        if(s3_http_server_handler != NULL)
        {
            ESP_LOGI(TAG, "Webserver started");
        }
    }
    else if (event_id == WIFI_EVENT_AP_STADISCONNECTED)
    {
        wifi_event_ap_stadisconnected_t* event = (wifi_event_ap_stadisconnected_t*) event_data;
        ESP_LOGD(TAG, "station "MACSTR" leave, AID=%d, reason=%d", MAC2STR(event->mac), event->aid, event->reason);
        ESP_LOGI(TAG, "WIFI_EVENT_AP_STADISCONNECTED");
        if(s3_http_server_handler != NULL)
        {
            stop_webserver();
        }
    }
}

esp_err_t init_wifi_access_point(void)
{
    // If station mode is active, deinitialize it first
    // if (gWiFi_SYNC_USER_INTERRUPT) {
    //     ESP_LOGI(TAG, "Station mode active, deinitializing before starting AP mode");
    //     deinit_wifi_station();
    // }

    init_nvs();
    esp_netif_init();
    s_web_event_group = xEventGroupCreate();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    esp_wifi_init(&cfg);
    esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_ap_event_handler, NULL, NULL);

    char sn[32] = {0};
    char sn_final_numbers[5] = {0};
    char ssid_name[SERIAL_NUMBER_SIZE] = {0};
    read_serial_number(sn);
    strncpy(sn_final_numbers, &sn[strlen(sn) - 4], 4);
    sn_final_numbers[4] = '\0';
    snprintf(ssid_name, sizeof(ssid_name), "Pixsee_%s", sn_final_numbers);

    wifi_config_t wifi_config = {
        .ap = {
            .ssid_len = strlen(ssid_name),
            .channel = CONFIG_S3_AP_WIFI_CHANNEL,
            .password = CONFIG_S3_AP_WIFI_PASSWORD,
            .max_connection = CONFIG_ESP_MAX_STA_CONN,
#if 0
            .authmode = WIFI_AUTH_WPA3_PSK,
            .sae_pwe_h2e = WPA3_SAE_PWE_BOTH,
#else /* CONFIG_ESP_WIFI_SOFTAP_SAE_SUPPORT */
            .authmode = WIFI_AUTH_WPA2_PSK,
#endif
            .pmf_cfg = {
                .required = false,
                .capable = true,
            },
#ifdef CONFIG_ESP_WIFI_BSS_MAX_IDLE_SUPPORT
            .bss_max_idle_cfg = {
                .period = WIFI_AP_DEFAULT_MAX_IDLE_PERIOD,
                .protected_keep_alive = 1,
            },
#endif
        },
    };
    strncpy((char*)wifi_config.ap.ssid, ssid_name, strlen(ssid_name));
    if (strlen(CONFIG_S3_AP_WIFI_PASSWORD) == 0) {
        wifi_config.ap.authmode = WIFI_AUTH_OPEN;
    }

    esp_wifi_set_mode(WIFI_MODE_AP);
    esp_wifi_set_config(WIFI_IF_AP, &wifi_config);
    esp_wifi_start();

    ESP_LOGI(TAG, "Access point started");
    ESP_LOGI(TAG, "SSID: [%s]", wifi_config.ap.ssid);
    ESP_LOGW(TAG, "Password: [%s]", wifi_config.ap.password);
    ap_is_on = true;
    return ESP_OK;
}

bool wait_to_rcv_wifi_data(void)
{
    ESP_LOGI(TAG, "Wainting POST with wifi credentials...");

    const int check_interval_ms = 100;
    int waited_ms = 0;

    while (!s_post_received_flag && waited_ms < WIFI_PAITRING_TIMEOUT)
    {
        vTaskDelay(pdMS_TO_TICKS(check_interval_ms));
        waited_ms += check_interval_ms;
    }

    stop_wifi_pairing_task(NULL);
    if (s_post_received_flag)
    {
        ESP_LOGI(TAG, "POST received!");
        return true;
    }
    else
    {
        ESP_LOGW(TAG, "Timeout wating POST.");
        return false;
    }
}

void notify_post_received_event(void)
{
    s_post_received_flag = true;
}

esp_err_t deinit_wifi_access_point(void)
{
    // if (ap_is_on) {
        esp_wifi_stop();
        esp_wifi_deinit();
        esp_event_handler_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_ap_event_handler);

        // Properly delete the web event group if it exists
        if (s_web_event_group != NULL) {
            vEventGroupDelete(s_web_event_group);
            s_web_event_group = NULL;
        }

        // Stop the HTTP server if it's running
        if (s3_http_server_handler != NULL) {
            stop_webserver();
        }

        ap_is_on = false;
        ESP_LOGI(TAG, "Wifi access point deinitialized!");
    // }
    return ESP_OK;
}

void wifi_pairing_task(void *pvParameters)
{
	//	E (20165) lcd_panel.io.spi: panel_io_spi_tx_color(390): spi transmit (queue) color failed
	//  E (20165) lcd_panel.st7789: panel_st7789_draw_bitmap(225): io tx color failed
	vTaskDelay(pdMS_TO_TICKS(500)); // Avoid updating the UI and enabling Wi-Fi at the same time
    ESP_LOGD(TAG, "ENTERED FUNCTION: %s", __func__);
    char msg[50] = "Unknown error";
    bool success = false;
    app_timeout_stop();
    // Reset the post received flag for a new pairing session
    s_post_received_flag = false;
    init_wifi_access_point();
    if(wait_to_rcv_wifi_data() == true)
    {
        ESP_LOGI(TAG, "ready to init the station");
        strncpy(msg, "success on rcv data", 50);
        success = true;
    }
    else
    {
        ESP_LOGE(TAG, "Fail on rcv data");
        strncpy(msg, "Fail on rcv data", 50);
        if (ap_is_on) {
            deinit_wifi_access_point();
        }
    }

    ESP_LOGI(TAG, "Wi-Fi pairing result: %s - result: %d", msg, success);
    if (success == true)
    {
        ESP_LOGI(TAG, "Success to get wifi data");
        // Wait a bit for the stop_wifi_pairing_task to finish if it was created
        vTaskDelay(pdMS_TO_TICKS(600));

        // Additional delay to ensure AP cleanup is complete
        vTaskDelay(pdMS_TO_TICKS(500));

        // Clear task handle first to mark task as finished
        wifi_pairing_task_handle = NULL;

        // Only call callback after task is marked as finished
        set_current_screen(WIFI_SEARCH_SCREEN, NULL_SCREEN);
    }
    else
    {
        ESP_LOGW(TAG, "Returnig to network setup");
        vTaskDelay(pdMS_TO_TICKS(1 * 1000));

        // Clear task handle first to mark task as finished
        wifi_pairing_task_handle = NULL;

        // Only call callback after task is marked as finished
		int oob_status = 0;
		read_oob_status(&oob_status);
		if (oob_status == 0)
		{
			set_current_screen(WIFI_UNKNOWN_SCREEN, HOME_SCREEN);
		}
		else
		{
			set_current_screen(WIFI_DISCONNECT_SCREEN, NULL_SCREEN);
		}
    }
    app_timeout_init();
    wifi_pairing_task_handle = NULL;
    vTaskDelete(NULL);
}

void start_wifi_pairing(void)
{
    if (wifi_pairing_task_handle == NULL) {
        // DMA optimization: WiFi pairing UI task doesn't need DMA-capable memory, use PSRAM (saves 5KB DMA)
        xTaskCreatePinnedToCoreWithCaps(wifi_pairing_task, "wifi_pairing_task", (5 * 1024), NULL, 0, &wifi_pairing_task_handle, 0, MALLOC_CAP_SPIRAM);
    }else {
        ESP_LOGW(TAG, "wifi_pairing_task is already running.");
    }
}

void stop_wifi_pairing(void)
{
    if (wifi_pairing_task_handle != NULL)
    {
        eTaskState state = eTaskGetState(wifi_pairing_task_handle);
        if (state != eDeleted)
        {
            ESP_LOGI(TAG, "Stopping wifi pairing task");

            // First, try to stop the WiFi gracefully
            if (ap_is_on) {
                deinit_wifi_access_point();
            }

            // Give the task some time to finish its current operation
            vTaskDelay(pdMS_TO_TICKS(100));

            // Check if task is still alive after graceful shutdown attempt
            state = eTaskGetState(wifi_pairing_task_handle);
            if (state != eDeleted) {
                ESP_LOGW(TAG, "Force deleting wifi pairing task");
                vTaskDelete(wifi_pairing_task_handle);
            }

            wifi_pairing_task_handle = NULL;
        }
        else
        {
            ESP_LOGI(TAG, "WiFi pairing task already terminated");
            wifi_pairing_task_handle = NULL;
        }
    }
}

// HTTP ACCESS POINT RELATED =======================================================================
void stop_wifi_pairing_task(void *arg)
{
    ESP_LOGD(TAG, "ENTERED FUNCTION: %s", __func__);
    vTaskDelay(pdMS_TO_TICKS(500));

    stop_webserver();
    deinit_wifi_access_point();
    // vTaskDelete(NULL);
}

esp_err_t post_handler(httpd_req_t *req)
{
    char content[HTTP_CONTENT_SIZE];
    size_t recv_size = MIN(req->content_len, sizeof(content) - 1);
    char sn[32] = {0};
    int ret = httpd_req_recv(req, content, recv_size);
    if (ret <= 0)
    {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to receive post data");
        return ESP_FAIL;
    }

    content[recv_size] = '\0';
    ESP_LOGI(TAG, "Received POST data: %s", content);

    cJSON *root = cJSON_Parse(content);
    if (root == NULL)
    {
        ESP_LOGE(TAG, "Error on parsing JSON content!");
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Failed to parse data");
        return ESP_FAIL;
    }
    else
    {
        const cJSON *wifi_ssid = cJSON_GetObjectItem(root, "wifi_ssid");
        const cJSON *wifi_password = cJSON_GetObjectItem(root, "wifi_password");
        const cJSON *timezone = cJSON_GetObjectItem(root, "timezone");
        const cJSON *secret_key = cJSON_GetObjectItem(root, "secret_key");

        if (cJSON_IsString(wifi_ssid) && cJSON_IsString(wifi_password))
        {
            if (write_wifi_credentials(wifi_ssid->valuestring, wifi_password->valuestring) != ESP_OK)
            {
                ESP_LOGE(TAG, "Fail to save wifi credentials");
                cJSON_Delete(root);
                httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to save data");
                return ESP_FAIL;
            }
            ESP_LOGI(TAG, "wifi_ssid: %s", wifi_ssid->valuestring);
            ESP_LOGI(TAG, "wifi_password: %s", wifi_password->valuestring);
        }
        else
        {
            ESP_LOGW(TAG, "Missing or invalid fields");
        }

        if (cJSON_IsString(timezone))
        {
            write_timezone(timezone->valuestring);
            set_timezone((const char*)timezone->valuestring);
        }

        int oob_status = 0;
        read_oob_status(&oob_status);
        if (oob_status == 0 && cJSON_IsString(secret_key))
        {
            write_secret_key(secret_key->valuestring);
        }
        cJSON_Delete(root);
        read_serial_number(sn);
        if(httpd_resp_sendstr(req, sn) == ESP_OK)
        {
            notify_post_received_event();
        }
        return ESP_OK;
    }
}

httpd_handle_t start_webserver(void)
	{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = HTTP_PORT;
    httpd_handle_t server = NULL;
    esp_err_t error;
    error = httpd_start(&server, &config);

    if (error == ESP_OK)
    {
        httpd_uri_t uri_post = {
            .uri = "/wifi/credentials",
            .method = HTTP_POST,
            .handler = post_handler,
            .user_ctx = NULL,
        };

        error = httpd_register_uri_handler(server, &uri_post);
        if(error != ESP_OK)
        {
            ESP_LOGE(TAG, "Fail to register HTTP server handler");
        }
    }
    else
    {
        ESP_LOGE(TAG, "Fail to start HTTP server - %s", esp_err_to_name(error));
    }
    return server;
}

esp_err_t stop_webserver(void)
{
    if (s3_http_server_handler){
    	esp_err_t ret = httpd_stop(s3_http_server_handler);
    	if (ret == ESP_OK) {
        	s3_http_server_handler = NULL;
        	ESP_LOGI(TAG, "HTTP server stopped");
    	}
    }
    return ESP_OK;
}

esp_err_t force_start_wifi(char *ssid, char *pass) {
    deinit_wifi_station();
	init_wifi_station(false);  // Normal WiFi operation - preserve audio/BT

	if (connect_wifi(ssid, pass, JOIN_CMD) != WIFI_SUCCESS_ON_CONNECT) {
		ESP_LOGE(TAG, "Fail to connect to Wi-Fi");
        set_pixsee_status(S3ER_SETUP_CHANGE_WIFI_FAIL);
		return ESP_FAIL;
	}

    ESP_LOGI(TAG, "Wifi connected OK");
    set_pixsee_status(S3ER_SETUP_CHANGE_WIFI_SUCCESS);
	return ESP_OK;
}

esp_err_t WTH_SwitchWiFi_PowerSave(void) {

    ESP_ERROR_CHECK(esp_netif_init());
    if (esp_netif_sta_handle == NULL) {
        esp_netif_sta_handle = esp_netif_create_default_wifi_sta();
    }

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_start());

    vTaskDelay(pdMS_TO_TICKS(100));

    ESP_ERROR_CHECK(esp_wifi_stop());
    ESP_ERROR_CHECK(esp_wifi_deinit());

    if (esp_netif_sta_handle) {
        esp_netif_destroy(esp_netif_sta_handle);
        esp_netif_sta_handle = NULL;
    }
    // ESP_ERROR_CHECK(esp_netif_deinit()); // system crash
    ESP_LOGI(TAG, "Wi-Fi stack started and deinitialized cleanly");

    return ESP_OK;
}

esp_err_t setup_wifi(int argc, char *argv[]) {
    char _ssid[WIFI_SSID_SIZE + 1];     // Use consistent buffer size with storage component
    char _password[WIFI_PASSWORD_SIZE + 1]; // Use consistent buffer size with storage component

    // Clear the buffers first for safety
    memset(_ssid, 0, sizeof(_ssid));
    memset(_password, 0, sizeof(_password));

    if (argc == 0) {
        // Use stored WiFi credentials
        if (read_wifi_credentials(_ssid, _password) != ESP_OK) {
            ESP_LOGE(TAG, "No stored WiFi credentials found. Use: join <ssid> <password>");
            set_pixsee_status(S3ER_SETUP_WIFI_NO_CREDENTIALS);
            return ESP_FAIL;
        }
        
        // Check if credentials are empty/invalid after successful read
        if (strlen(_ssid) == 0) {
            ESP_LOGE(TAG, "Invalid SSID or PASSWORD");
            return ESP_ERR_INVALID_ARG;
        }
    } else {
        switch (argc) {
            case 1:
                strcpy(_ssid, argv[0]);
                break;
            case 2:
                strcpy(_ssid, argv[0]);
                strcpy(_password, argv[1]);
                break;
            default:
                ESP_LOGE(TAG, "Invalid SSID or PASSWORD");
                return ESP_ERR_INVALID_ARG;
        }
    }
    ESP_LOGI(TAG, "Connecting Wi-Fi, SSID:\"%s\" PASSWORD:\"%s\"", _ssid, _password);
    force_start_wifi(_ssid, _password);
    return ESP_OK;
}

// NFC sync task handle for preventing multiple concurrent sync tasks
static TaskHandle_t nfc_sync_task_handle = NULL;

// Legacy wrapper for backward compatibility - converts nfc_sync_param_t to unified_sync_param_t
void start_nfc_sync(void *pvParameters) {
    if (nfc_sync_task_handle != NULL) {
        ESP_LOGW(TAG, "NFC sync task is already running, skipping");
        // Clean up the parameter since we're not using it
        if (pvParameters) {
            free(pvParameters);
        }
        return;
    }
    
    nfc_sync_param_t *nfc_param = (nfc_sync_param_t *)pvParameters;
    
    // Create unified sync parameter with NFC mode
    unified_sync_param_t *unified_param = malloc(sizeof(unified_sync_param_t));
    if (unified_param == NULL) {
        ESP_LOGE(TAG, "Failed to allocate memory for unified sync parameter");
        if (nfc_param) {
            free(nfc_param);
        }
        return;
    }
    
    unified_param->sync_mode = SYNC_MODE_NFC;
    unified_param->callback = nfc_param ? nfc_param->callback : NULL;
    unified_param->is_from_cli = false;  // Keep simple for now to avoid breaking NFC detection
    
    ESP_LOGI(TAG, "NFC sync parameter transfer: callback=%p", unified_param->callback);
    
    // Free the original NFC parameter since we've copied the callback
    if (nfc_param) {
        free(nfc_param);
    }
    
    // Create NFC sync task with same stack size as WiFi sync for consistency
    ESP_LOGI(TAG, "Creating NFC sync task with 12KB stack");
    BaseType_t result = xTaskCreatePinnedToCore(
        unified_sync_task, 
        "nfc_sync_task", 
        (12 * 1024),  // 12KB stack - must match WiFi sync to prevent stack overflow
        unified_param, 
        5,  // Higher priority than WiFi sync to handle NFC responsively
        &nfc_sync_task_handle, 
        1   // Run on core 1 like WiFi sync
    );
    
    if (result != pdPASS) {
        ESP_LOGE(TAG, "Failed to create NFC sync task");
        if (unified_param) {
            free(unified_param);
        }
        nfc_sync_task_handle = NULL;
    }
}

// Unified sync task that handles both full WiFi sync and NFC sync modes
void unified_sync_task(void *pvParameters) {
    gWiFi_SYNC_USER_INTERRUPT = false;
    gSyncInProgress = true;
    ESP_LOGI(TAG, "unified_sync_task: Setting sync_flag to DISABLE refresh [LVGL]");
    wifi_exception_screen_e wifi_exception_screen = WIFI_ERROR_UNKNOWN_ERROR;
    
    // PREPARATION STAGE - Display data_sync0.jpg
    s3_sync_stage = 0;
    set_current_screen(DATA_SYNC_SCREEN, NULL_SCREEN);
    vTaskDelay(pdMS_TO_TICKS(300)); // Allow UI to update

    // CRITICAL: Stop NFC completely at the beginning to prevent ALL race conditions during WiFi sync
    ESP_LOGW(TAG, "[0.1] CRITICAL: Shutting down NFC completely to prevent race conditions during WiFi sync");
    extern void stop_nfc(void);
    stop_nfc();
    vTaskDelay(pdMS_TO_TICKS(500)); // Allow complete NFC shutdown
    ESP_LOGI(TAG, "[0.2] NFC completely shut down - proceeding with WiFi sync");

    unified_sync_param_t *param = (unified_sync_param_t *)pvParameters;
    int sync_mode = param ? param->sync_mode : SYNC_MODE_FULL;
    
    const char *mode_str = (sync_mode == SYNC_MODE_FULL) ? "FULL" :
                           (sync_mode == SYNC_MODE_NFC) ? "NFC" : "BLE";
    ESP_LOGI(TAG, "unified_sync_task: mode=%s", mode_str);
    vTaskDelay(pdMS_TO_TICKS(500)); // Avoid updating the UI and enabling Wi-Fi at the same time

    char ssid[WIFI_SSID_SIZE] = {0};
    char pass[WIFI_PASSWORD_SIZE] = {0};
    char tz[TIMEZONE_STR_SIZE] = {0};
    char secret[SECRET_KEY_STR_SIZE] = {0};
    const char *msg = NULL;
    bool success = false;
    bool out_error = false;
    int oob_status = OOB_FACTORY_RESET;
    esp_err_t sync_status = ESP_FAIL;
    esp_err_t ret = ESP_FAIL;
    int binding_code = -1;

    read_oob_status(&oob_status);
    int i = 0;

    app_timeout_stop();
    stop_alarm_timer();
    
    // Set the sync screen is not needed anymore as the clouds are being shown
    
    if (read_wifi_credentials(ssid, pass) != ESP_OK) {
        msg = "Fail to access credentials file";
        wifi_exception_screen = WIFI_ERROR_DISCONNECT;
        goto FINISH;
    }
    ESP_LOGW(TAG, "Wifi credentials - SSID: (%s) - PASS: (%s)", ssid, pass);

    // STAGE 1: WiFi Connection - Display data_sync1.jpg
    s3_sync_stage = 1;
    set_current_screen(DATA_SYNC_SCREEN, NULL_SCREEN);
    vTaskDelay(pdMS_TO_TICKS(300)); // Allow UI to update

    ESP_LOGI(TAG, "[1.0] init_wifi_station");
    
    // DIAGNOSTIC: Log state before WiFi init attempt
    size_t dma_pre_init_kb;
    int dma_pre_init_percent;
    get_dma_usage(&dma_pre_init_kb, &dma_pre_init_percent);
    ESP_LOGI(TAG, "[DIAG] Before init_wifi_station: DMA=%dKB (%d%%), BT=%d, BLE=%d",
             (int)dma_pre_init_kb, (int)dma_pre_init_percent,
             s3_bt_classic_is_connected(), s3_ble_manager_is_connected());
    
    esp_err_t wifi_init_result = init_wifi_station(true);  // This is unified_sync_task - use emergency cleanup

    if (wifi_init_result != ESP_OK) {
        ESP_LOGE(TAG, "WiFi initialization failed: %s", esp_err_to_name(wifi_init_result));
        if (wifi_init_result == ESP_ERR_NO_MEM) {
            ESP_LOGE(TAG, "ESP_ERR_NO_MEM during WiFi init - this helps us find the real DMA limit!");
            // Get DMA usage at failure point for analysis
            size_t dma_fail_kb;
            int dma_fail_percent;
            get_dma_usage(&dma_fail_kb, &dma_fail_percent);
            ESP_LOGE(TAG, "DMA at failure point: %d KB (%d%%) - CRITICAL DATA for threshold analysis", 
                     (int)dma_fail_kb, dma_fail_percent);
            ESP_LOGE(TAG, "[DIAG] DMA delta: %d KB consumed during failed init attempt",
                     (int)(dma_fail_kb - dma_pre_init_kb));
            msg = "Insufficient memory for WiFi";
        } else {
            msg = "WiFi initialization failed";
        }
        wifi_exception_screen = WIFI_ERROR_DISCONNECT;
        set_pixsee_status(S3ER_SETUP_CONNECT_FAIL);
        set_pixsee_msg(S3MSG_WIFI_CONNECT, S3MSG_FAIL);
        out_error = true;
        goto FINISH;
    }
    
    ESP_LOGI(TAG, "[1.1] connect_wifi");
    // gPixseeStatus inside the connect_wifi function will indicate the specific error code
    if (connect_wifi(ssid, pass, WIFI_CMD) != WIFI_SUCCESS_ON_CONNECT) {
        if (sync_mode == SYNC_MODE_NFC) {
            set_current_screen(NFC_WIFI_DISCONNECT_SCREEN, HOME_SCREEN);
        } else if (sync_mode == SYNC_MODE_BLE) {
            // BLE sync will handle screen transition at the end
            msg = "Fail to connect to Wi-Fi";
        } else {
            msg = "Fail to connect to Wi-Fi";
        }
        // set_pixsee_status(S3ER_SETUP_CONNECT_FAIL);  // sent inside connect_wifi
        set_pixsee_msg(S3MSG_WIFI_CONNECT, S3MSG_FAIL);
        wifi_exception_screen = WIFI_ERROR_TIMEOUT;
        out_error = true;
        goto FINISH;
    }

    // set_pixsee_status(S3ER_SETUP_CONNECT_SUCCESS);   // sent inside connect_wifi
    set_pixsee_msg(S3MSG_WIFI_CONNECT, S3MSG_SUCCESS);
    ESP_LOGI(TAG, "[1.1] WiFi connected successfully, proceeding with sync steps (mode=%s)",
             sync_mode == SYNC_MODE_FULL ? "FULL" : "NFC");

    // Disable WiFi power saving for maximum download performance (same optimization as speed_test and manual OTA)
    ESP_LOGI(TAG, "[1.2] Optimizing WiFi performance for faster downloads...");
    esp_wifi_set_ps(WIFI_PS_NONE);
    ESP_LOGI(TAG, "WiFi power saving disabled for optimal sync speed");

    // Set BLE as higher priority for coexistence - allows BLE connections during WiFi sync
    ESP_LOGI(TAG, "[1.3] Setting BLE coexistence priority to allow BLE connections during WiFi");
    esp_coex_preference_set(ESP_COEX_PREFER_BT);

    set_current_screen(DATA_SYNC_SCREEN, NULL_SCREEN);
    
    // FULL and BLE sync mode: SNTP time synchronization
    if (sync_mode == SYNC_MODE_FULL || sync_mode == SYNC_MODE_BLE) {
        ESP_LOGI(TAG, "[2.0] sntp");
        ESP_LOGI(TAG, "Available heap: %u, SPIRAM: %u", heap_caps_get_free_size(MALLOC_CAP_8BIT), heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
        if (read_timezone(tz) == ESP_OK) {
            init_sntp(tz);
            sync_status = wait_for_time_sync();
            deinit_sntp();
            if (sync_status != ESP_OK) {
                wifi_exception_screen = WIFI_ERROR_DATA_SYNC_FAIL;
                goto FINISH;
            }
        }
        set_pixsee_status(sync_status == ESP_OK ? S3ER_FULL_SYNC_SNTP_SUCCESS : S3ER_FULL_SYNC_SNTP_FAIL);

        // FULL sync mode: OOB binding
        ESP_LOGI(TAG, "[3.0] oob");
        ESP_LOGI(TAG, "Available heap: %u, SPIRAM: %u", heap_caps_get_free_size(MALLOC_CAP_8BIT), heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
        if (oob_status == OOB_FACTORY_RESET) {
            ESP_LOGI(TAG, "[3.1] OOB==OOB_FACTORY_RESET -> binding");
            if (read_secret_key(secret) == ESP_OK) {
                out_error = true;
                if (cei_complete_binding_of_device(&binding_code) == ESP_OK) {
                    msg = "Success on cloud binding";

                    oob_status = OOB_NORMAL;
                    write_oob_status(&oob_status);
                    ESP_LOGW(TAG, "[3.2] OOB==OOB_FACTORY_RESET -> binding success, change oob to [OOB_NORMAL] --- Biniding code: %d", binding_code);
                    
                    set_pixsee_status(S3ER_BIND_DEV_SUCCESS);
                    set_pixsee_msg(S3MSG_ACC_BOUND, S3MSG_SUCCESS);
                    success = true;
                } else {
                    msg = "Fail on cloud binding";
                    ESP_LOGE(TAG, "[3.2] OOB==OOB_FACTORY_RESET -> binding fail, keep oob to [OOB_FACTORY_RESET] --- Binding code: %d", binding_code);

                    wifi_exception_screen = WIFI_ERROR_DATA_SYNC_FAIL;
                    if(binding_code == BOUND_BY_OTHERS_CODE)
                    {
                        set_pixsee_status(S3ER_SETUP_SECK_NOT_IN_OOB);
                        set_pixsee_msg(S3MSG_ACC_BOUND, S3MSG_FAIL);
                        ESP_LOGW(TAG, "[3.2] Bound by others");
                    }
                    else
                    {
                        set_pixsee_status(S3ER_BIND_DEV_FAIL);
                        set_pixsee_msg(S3MSG_ACC_BOUND, S3MSG_FAIL);
                    }
                    success = false;
                    // goto FINISH; // changed to bind ONLY
                }
            } else {
                ESP_LOGI(TAG, "3.2 OOB==OOB_FACTORY_RESET -> Not find a valid secret key");
                set_pixsee_status(S3ER_BIND_DEV_SKIP);
                set_pixsee_msg(S3MSG_ACC_BOUND, S3MSG_FAIL);
            }

            ESP_LOGI(TAG, "3.3 OOB==OOB_FACTORY_RESET -> End, close task in both cases");
            // vTaskDelay(pdMS_TO_TICKS(S3ER_BLE_TASK_MS)); // Allow user to see the binding result
            goto FINISH; // When we do OOB binding [OOB_FACTORY_RESET], we finish immediately after binding (success or fail)
        } else {
            ESP_LOGI(TAG, "3.1 OOB==OOB_NORMAL -> skip binding");
            set_pixsee_status(S3ER_BIND_DEV_SKIP);
            set_pixsee_msg(S3MSG_ACC_BOUND, S3MSG_SUCCESS);
        }
    }

    // STAGE 2: Resource Update - Display data_sync2.jpg
    // MOVED: Don't update screen here - will update after [6.0] cei_upload_device_info to avoid DMA conflict
    s3_sync_stage = 2;
    // set_current_screen(DATA_SYNC_SCREEN, NULL_SCREEN);  // COMMENTED OUT
    // vTaskDelay(pdMS_TO_TICKS(300)); // Allow UI to update

	if (access("/sdcard/animation_gif/wifi/data_sync.gif", F_OK) != 0){
		s3_remove("/sdcard/resource_ver.txt");
	}

    // Resource updates (both modes)
    ESP_LOGI(TAG, "[4.0] resource");
    char *resource_version = NULL;
    char *resource_url = NULL;
    parser_ota_resource_info(&resource_version, &resource_url);
    char tmp[16];
    read_resource_version_or_default(tmp, sizeof(tmp));
    ESP_LOGW(TAG, "[4.1] check resource version remote:%s, local:%s",
             resource_version ? resource_version : "NULL", tmp);
    bool doResource = resource_version ? version_gt(resource_version, tmp) : false;
    // Always verify GraphicData files exist, even if version matches (file-by-file check inside sync_resource_without_mp3)
    // if (resource_version && resource_url) {
    if (doResource) {
        s3_wifi_downloading = true;
        for (i = 0; i < 2; i++) {
            ESP_LOGI(TAG, "[GraphicData n%d - start]: %s", i+1, resource_url ? resource_url : "NULL");
            if (sync_resource_without_mp3(resource_url,i) == ESP_OK) {
                write_resource_version_to_file(resource_version);
                ESP_LOGI(TAG, "[GraphicData n%d - success]: write_resource_version_to_file %s", i+1,
                         resource_version ? resource_version : "NULL");
                break;
            }
            ESP_LOGW(TAG, "[GraphicData n%d - fail]: sync_resource_without_mp3 failed", i+1);
        }
        s3_wifi_downloading = false;
    }
    if (resource_version)
        free(resource_version);
    if (resource_url)
        free(resource_url);

    // FULL and BLE sync mode: OTA firmware update
    if (sync_mode == SYNC_MODE_FULL || sync_mode == SYNC_MODE_BLE) {
        // Check if OTA should be skipped
        if (skip_ota_flag) {
            ESP_LOGI(TAG, "[5.0] Skipping OTA verification (developer skip mode enabled)");
            set_pixsee_status(S3ER_FULL_SYNC_OTA_NOT_REQUIRED);
        } else {
            ESP_LOGI(TAG, "[5.0] fw version api");
            const esp_partition_t *running = esp_ota_get_running_partition();
            esp_app_desc_t app_desc;
            esp_ota_get_partition_description(running, &app_desc);
            char *version = NULL;
            char *ota_url = NULL;
            parser_ota_info(&version, &ota_url);
            bool doOTA = version_gt(version, app_desc.version + 1);
            ESP_LOGW(TAG, "[5.1] doOTA=%d, rVersion=%s,lVersion=%s", doOTA,
                     version ? version : "NULL", app_desc.version + 1);
            if (doOTA) {
                // Notify APP that OTA is required - device will reboot after OTA
                ESP_LOGI(TAG, "[5.1] OTA required - notifying APP (BLE will disconnect after OTA)");
                set_pixsee_status(S3ER_FULL_SYNC_OTA_REQUIRED);
                vTaskDelay(pdMS_TO_TICKS(1000)); // Give APP time to receive notification and prepare for disconnect
                set_current_screen(OTA_SCREEN, NULL_SCREEN);
                vTaskDelay(pdMS_TO_TICKS(300)); // Allow UI to update

                gOTA_in_progress = true; // Mark OTA as in progress
                for (int i = 0; i < 4; i++) {
                    ESP_LOGI(TAG, "[OTA n%d - start]: %s", i+1, ota_url ? ota_url : "NULL");
                    ret = OTA_Update(ota_url);
                    if (ret == ESP_OK) {
                        ESP_LOGI(TAG, "[OTA n%d - success]: OTA_Update completed", i+1);
                        break;
                    }
                    ESP_LOGW(TAG, "[OTA n%d - fail]: OTA_Update failed", i+1);
                    vTaskDelay(pdMS_TO_TICKS(100));
                }
                gOTA_in_progress = false; // OTA completed (success or fail)
            } else {
                // No OTA update needed
                ESP_LOGI(TAG, "[5.1] OTA not required - firmware is up to date");
                set_pixsee_status(S3ER_FULL_SYNC_OTA_NOT_REQUIRED);
            }
            if (version)
                free(version);
            if (ota_url)
                free(ota_url);
        }
        if (gWiFi_SYNC_USER_INTERRUPT == true)
            goto FINISH;
    }

    // FULL and BLE sync mode: Device info upload (NO screen update to avoid DMA conflict)
    set_pixsee_status(S3ER_SETUP_CONNECT_SUCCESS);
    if (sync_mode == SYNC_MODE_FULL || sync_mode == SYNC_MODE_BLE) {
        ESP_LOGI(TAG, "[6.0] cei_upload_device_info - preparing data");
        uint8_t mac[6];
        esp_read_mac(mac, ESP_MAC_WIFI_STA);
        char patch_data[256] = {0};
        const esp_partition_t *running = esp_ota_get_running_partition();
        esp_app_desc_t app_desc;
        esp_ota_get_partition_description(running, &app_desc);
        sprintf(patch_data, "{\"battery\":%d,\"wifi\":\"%s\",\"fwVersion\":\"%s\",\"mac\":\"%02x:%02x:%02x:%02x:%02x:%02x\"}", s3_battery_level, ssid,
                app_desc.version, mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
        ESP_LOGI(TAG, "patch_data:%s", patch_data);

        // Give LVGL time to finish any pending screen updates before HTTP operation
        ESP_LOGI(TAG, "[6.0] Waiting for LVGL to stabilize before HTTP upload...");
        vTaskDelay(pdMS_TO_TICKS(500));

        for (int i = 0; i < 3; i++) {
            ESP_LOGI(TAG, "[DeviceInfo n%d - start]: uploading device info", i+1);
            ret = cei_upload_device_info(patch_data);
            if (ret == ESP_OK) {
                ESP_LOGI(TAG, "[DeviceInfo n%d - success]: cei_upload_device_info completed", i+1);
                break;
            } else if (ret == CEI_INVALID_SECRET_KEY) {
                ESP_LOGE(TAG, "[#15457][Delete account] Go back to factory mode");
                set_current_screen(ACC_INV_FAC_RESET_SCREEN, NULL_SCREEN);
                goto FINISH_WITHOUT_UI;
            }
            ESP_LOGW(TAG, "[DeviceInfo n%d - fail]: cei_upload_device_info failed", i+1);
            vTaskDelay(pdMS_TO_TICKS(500)); // Longer delay between retries
        }

        ESP_LOGI(TAG, "[6.1] s3_cloud_upload_tracking_info");
        exec_upload_tracking_info();
    }

    // Now update screen for stage 2 AFTER upload completes to avoid SDMMC DMA conflict
    ESP_LOGI(TAG, "[6.2] Updating screen for stage 2 (Resource Update)");
    set_current_screen(DATA_SYNC_SCREEN, NULL_SCREEN);
    vTaskDelay(pdMS_TO_TICKS(300)); // Allow UI to update

    // Download account file and sync content (both modes)
    ESP_LOGI(TAG, "[7.0] account");
    i = 0;
    while (!gWiFi_SYNC_USER_INTERRUPT) {
        ESP_LOGI(TAG, "[AccountFile n%d - start]: downloading account file", i+1);
        ret = https_download_account_file(NULL);
        if (ret == ESP_OK) {
            ESP_LOGI(TAG, "[AccountFile n%d - success]: https_download_account_file completed", i+1);
            break;
        } else if (i > 2) {
            ESP_LOGW(TAG, "[AccountFile n%d - fail]: reached max retries", i+1);
            break;
        }
        ESP_LOGW(TAG, "[AccountFile n%d - fail]: https_download_account_file failed", i+1);
        i++;
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    if (ret != ESP_OK || gWiFi_SYNC_USER_INTERRUPT)
    {
        wifi_exception_screen = WIFI_ERROR_DATA_SYNC_FAIL;
        goto FINISH;
    }

    ESP_LOGI(TAG, "[7.1] parser_and_contents_sync");
    s3_wifi_downloading = true;
    ret = parser_account_contents(PARSE_AND_DOWNLOAD);
    s3_wifi_downloading = false;
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "[7.2] Content download completed, waiting for SD card write completion...");
        vTaskDelay(pdMS_TO_TICKS(1000)); // Allow time for SD card writes to complete
        
        ESP_LOGI(TAG, "[7.3] Checking album availability after download completion");
        bool album_check_result = s3_albums_dynamic_build();
        
        // If no albums were found available, retry once after additional delay
        // This handles timing issues where files aren't fully written yet
        if (!album_check_result && s3_albums_get_size() == 0) {
            ESP_LOGW(TAG, "[7.4] No albums available after first check, retrying after additional delay...");
            vTaskDelay(pdMS_TO_TICKS(1500)); // Additional delay for slower SD cards
            ESP_LOGI(TAG, "[7.5] Retry album availability check");
            s3_albums_dynamic_build();
        }
        
        success = true;
        // STAGE 3: Account Content Sync - Display data_sync3.jpg
        s3_sync_stage = 3;
        set_current_screen(DATA_SYNC_SCREEN, NULL_SCREEN);
        vTaskDelay(pdMS_TO_TICKS(3000)); // Allow UI to update
    }
    else
    {
        success = false;
        wifi_exception_screen = WIFI_ERROR_DATA_SYNC_FAIL;
        goto FINISH;
    }


FINISH:
    // Reset sync stage
    s3_sync_stage = 0;

    gSyncInProgress = false;
    // Execute callback if provided (NFC mode only) - ALWAYS call regardless of sync success
    // User needs feedback about what happened after sync attempt
    if (sync_mode == SYNC_MODE_NFC && param && param->callback) {
        ESP_LOGI(TAG, "[7.6] Executing NFC post-sync callback (sync_success=%s)", success ? "true" : "false");
        param->callback();
        ESP_LOGI(TAG, "[7.6] NFC post-sync callback completed");
    } else if (sync_mode == SYNC_MODE_NFC) {
        ESP_LOGI(TAG, "[7.6] No NFC callback to execute (param=%p, callback=%p)", param, param ? param->callback : NULL);
    }
    
    if (msg) {
        ESP_LOGI(TAG, "[8.0] unified_sync_task end: %s", msg);
        // set_pixsee_status(S3ER_SYNC_FAIL);
    } else {
        ESP_LOGI(TAG, "[8.0] unified_sync_task end");
        // set_pixsee_status(S3ER_SYNC_SUCCESS);
    }

    if (!out_error)
       set_pixsee_status(success ? S3ER_SYNC_SUCCESS : S3ER_SYNC_FAIL);
    
    if (!gWiFi_SYNC_USER_INTERRUPT) {
        if (sync_mode == SYNC_MODE_FULL) {
            if (success) {
                // Show success screen for 3 seconds, then return to data sync menu
                set_current_screen(WIFI_SYNC_SUC_SCREEN, WIFI_SYNC_MAI_SCREEN);
            } else {
                // Sync failed - show error screen for 3 seconds, then return to data sync menu
                switch (wifi_exception_screen)
                {
                case WIFI_ERROR_TIMEOUT:
                case WIFI_ERROR_DISCONNECT:
                    if (oob_status == 0) {
                        set_current_screen(WIFI_ERR_SCREEN, HOME_SCREEN);
                    } else {
                        set_current_screen(WIFI_ERR_SCREEN, WIFI_SYNC_MAI_SCREEN);
                    }
                    break;

                case WIFI_ERROR_DATA_SYNC_FAIL:
                    set_current_screen(WIFI_SYNC_ERR_SCREEN, WIFI_SYNC_MAI_SCREEN);
                    break;

                case WIFI_ERROR_NO_FIRMWARE:
                case WIFI_ERROR_UNKNOWN_ERROR:
                    set_current_screen(WIFI_ERR_SCREEN, WIFI_SYNC_MAI_SCREEN);
                    break;

                default:
                    set_current_screen(WIFI_SYNC_ERR_SCREEN, WIFI_SYNC_MAI_SCREEN);
                    break;
                }
            }
        } else if (sync_mode == SYNC_MODE_NFC) {
            // For NFC sync, let callback handle screen transitions when callback exists
            // Otherwise return to previous screen (CLI case)
            if (param && param->callback) {
                ESP_LOGI(TAG, "NFC sync completed, callback will handle screen transitions");
                // No screen setting here - callback takes full control
            } else {
                ESP_LOGI(TAG, "NFC sync completed, returning to previous screen");
                set_current_screen(get_previous_screen(), NULL_SCREEN);
            }
        } else if (sync_mode == SYNC_MODE_BLE) {
            // For BLE sync, always return to HOME_SCREEN after sync (success or failure)
            if (success) {
                ESP_LOGI(TAG, "BLE sync completed successfully, returning to HOME_SCREEN");
                set_current_screen(WIFI_SYNC_SUC_SCREEN, HOME_SCREEN);
            } else {
                ESP_LOGI(TAG, "BLE sync failed, returning to HOME_SCREEN");
                // Show error screen briefly, then return to HOME_SCREEN
                switch (wifi_exception_screen)
                {
                case WIFI_ERROR_TIMEOUT:
                case WIFI_ERROR_DISCONNECT:
                    set_current_screen(WIFI_ERR_SCREEN, HOME_SCREEN);
                    break;

                case WIFI_ERROR_DATA_SYNC_FAIL:
                    set_current_screen(WIFI_SYNC_ERR_SCREEN, HOME_SCREEN);
                    break;

                case WIFI_ERROR_NO_FIRMWARE:
                case WIFI_ERROR_UNKNOWN_ERROR:
                    set_current_screen(WIFI_ERR_SCREEN, HOME_SCREEN);
                    break;

                default:
                    set_current_screen(WIFI_SYNC_ERR_SCREEN, HOME_SCREEN);
                    break;
                }
            }
        }
    } else {
        // Handle screen transitions even when user interrupted sync
        if (sync_mode == SYNC_MODE_NFC) {
            // For NFC sync, always return to previous screen even if interrupted
            ESP_LOGI(TAG, "NFC sync interrupted by user, returning to previous screen");
            set_current_screen(get_previous_screen(), NULL_SCREEN);
        } else if (sync_mode == SYNC_MODE_BLE) {
            // For BLE sync, return to HOME_SCREEN even if interrupted
            ESP_LOGI(TAG, "BLE sync interrupted by user, returning to HOME_SCREEN");
            set_current_screen(HOME_SCREEN, NULL_SCREEN);
        }
        // FULL sync mode doesn't need special handling when interrupted
        // as it will naturally return to its appropriate state
    }

FINISH_WITHOUT_UI:
    ESP_LOGI(TAG, "[8.1] WIFI_DEINIT");
    get_alarm_setting(TIMER_SOURCE_ESP_TIMER);

    // Reset appropriate task handle based on sync mode
    if (sync_mode == SYNC_MODE_FULL || sync_mode == SYNC_MODE_BLE) {
        wifi_connecting_task_handle = NULL;
    } else if (sync_mode == SYNC_MODE_NFC) {
        nfc_sync_task_handle = NULL;
    }

    // deinit_wifi_station() will reset coexistence to PREFER_BT
    deinit_wifi_station();

    // If we previously disconnected BT Classic for WiFi, attempt to restore it now
    if (s_bt_was_disconnected_for_wifi) {
        ESP_LOGI(TAG, "Restoring BT Classic connection after WiFi usage");
        s_bt_was_disconnected_for_wifi = false;
        // Best-effort reconnect in background
        bt_manager_connect();
    }

    resume_audio_tasks_after_wifi();
    
    // CRITICAL: Restart NFC after all WiFi operations are completely finished
    ESP_LOGW(TAG, "[8.2] CRITICAL: Restarting NFC after complete WiFi sync finish");
    extern void start_nfc(void);
    start_nfc();
    ESP_LOGI(TAG, "[8.3] NFC restarted successfully - normal operation restored");
    
    gWiFi_SYNC_USER_INTERRUPT = true;
    if (param) {
        free(param);
    }

    // Resume audio tasks after WiFi initialization completes (success or failure)
    resume_audio_tasks_after_wifi();
    app_timeout_restart();
    vTaskDelete(NULL);
}

// Function to toggle the skip OTA flag - persists until toggled again or device reset
esp_err_t set_skip_ota_flag(esp_periph_handle_t periph, int argc, char *argv[])
{
    skip_ota_flag = !skip_ota_flag; // Toggle the flag
    if (skip_ota_flag) {
        ESP_LOGI(TAG, "OTA skip ENABLED - all syncs will skip OTA verification until disabled");
    } else {
        ESP_LOGI(TAG, "OTA skip DISABLED - syncs will perform normal OTA verification");
    }
    return ESP_OK;
}
