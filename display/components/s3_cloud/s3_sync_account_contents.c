//
// Created by Shane_Hwang on 2025/7/8.
//

#include <errno.h>
#include <s3_logger.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <ctype.h>
#include "cJSON.h"
#include "esp_err.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/ringbuf.h"
#include "freertos/semphr.h"
#include "stdbool.h"

#include "s3_definitions.h"
#include "s3_https_cloud.h"
#include "s3_sync_account_contents.h"
#if CONFIG_MBEDTLS_CERTIFICATE_BUNDLE
#include "esp_crt_bundle.h"
#endif

#include "esp_vfs_fat.h"
#include "esp_wifi.h"

static const char *TAG = "SYNC_CONTENTS";
extern bool gWiFi_SYNC_USER_INTERRUPT;

// Forward declaration - actual implementation will be provided by lv_decoders.c
extern void jpeg_cache_invalidate(const char *path);
extern void png_cache_invalidate(const char *path);
static void invalidate_image_cache(const char *path);

static int gBabyPackCount;
static s3_babyPack_t *gBabyPack = NULL;
static int gAlarmsCount;
static s3_alarm_t *gAlarms = NULL;
static int gNfcCount;
static s3_nfc_t *gNfcs = NULL;
extern char s3_WiFiSyncKidIcon[8];
extern char s3_babyId[32];

// Filename to ContentId mapping data structures
typedef struct {
    char* filename_lowercase;  // Lowercase version of filename
    char* contentId;          // Corresponding contentId
} filename_contentid_entry_t;

static filename_contentid_entry_t* gFilenameContentIdMap = NULL;
static int gFilenameContentIdMapCount = 0;

// Global HTTP client for connection reuse to avoid repeated SSL handshakes
static esp_http_client_handle_t g_reusable_client = NULL;
static bool g_client_initialized = false;
static int g_connection_failures = 0;  // Track consecutive connection failures
static int64_t g_last_successful_connection = 0;  // Track last successful connection time

// Connection reuse functions to solve SSL allocation failures
static void cleanup_reusable_http_client(void); // Forward declaration
static esp_err_t recover_connection_if_needed(void); // Connection recovery

static esp_err_t init_reusable_http_client(void) {
    if (g_client_initialized && g_reusable_client) {
        ESP_LOGD(TAG, "Reusable HTTP client already initialized");
        return ESP_OK;
    }
    
    // Clean up any existing client first
    cleanup_reusable_http_client();
    
    ESP_LOGI(TAG, "Initializing TURBO reusable HTTP client for maximum speed");
    
    // Memory cleanup before SSL connection initialization
    ESP_LOGI(TAG, "Pre-client init: Free heap: %u bytes, Free SPIRAM: %u bytes", 
        heap_caps_get_free_size(MALLOC_CAP_8BIT), 
        heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    
    esp_http_client_config_t config = {
        .url = "https://stagingcontent.blob.core.windows.net",
        .buffer_size = 64*1024,        // Increased RX buffer to 64KB
        .timeout_ms = 30000,           // Reduced timeout for memory efficiency
        .crt_bundle_attach = esp_crt_bundle_attach,  // Certificate verification disabled for speed
        .keep_alive_enable = true,     // Enable keep-alive for reuse
        .skip_cert_common_name_check = true,  // Azure blob flexibility
        .buffer_size_tx = 32*1024,     // Increased TX buffer to 32KB
        .disable_auto_redirect = false,// Allow redirects for Azure CDN optimization
        .max_redirection_count = 3,    // Reduced redirects for memory efficiency
        .transport_type = HTTP_TRANSPORT_OVER_SSL,
        .use_global_ca_store = false,
        .is_async = false,
        .max_authorization_retries = 1,// Reduced retries for memory efficiency
        .user_agent = "ESP32/1.0",     // Shorter user agent for memory efficiency
        .method = HTTP_METHOD_GET,
    };
    
    g_reusable_client = esp_http_client_init(&config);
    if (!g_reusable_client) {
        ESP_LOGE(TAG, "Failed to initialize TURBO HTTP client");
        return ESP_FAIL;
    }
    
    g_client_initialized = true;
    ESP_LOGI(TAG, "TURBO reusable HTTP client initialized with 64KB RX / 32KB TX buffers");
    return ESP_OK;
}

static void cleanup_reusable_http_client(void) {
    if (g_reusable_client) {
        esp_http_client_cleanup(g_reusable_client);
        g_reusable_client = NULL;
    }
    g_client_initialized = false;
    ESP_LOGD(TAG, "Reusable HTTP client cleaned up");
}

static esp_err_t configure_reusable_client_for_url(const char *url) {
    if (!g_reusable_client || !g_client_initialized) {
        ESP_LOGE(TAG, "Reusable client not initialized");
        return ESP_FAIL;
    }
    
    // Set the new URL for this download
    esp_err_t err = esp_http_client_set_url(g_reusable_client, url);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to set URL for reusable client: %s", esp_err_to_name(err));
        return err;
    }
    
    // Reset client state for new request with optimized headers
    esp_http_client_set_method(g_reusable_client, HTTP_METHOD_GET);
    esp_http_client_set_header(g_reusable_client, "Connection", "keep-alive");
    esp_http_client_set_header(g_reusable_client, "Cache-Control", "no-cache");
    esp_http_client_set_header(g_reusable_client, "User-Agent", "ESP32-Azure/1.0");
    esp_http_client_set_header(g_reusable_client, "Accept", "*/*");
    esp_http_client_set_header(g_reusable_client, "Accept-Encoding", "identity"); // avoid compression on device
    
    return ESP_OK;
}

// Connection recovery function to handle SSL timeout failures
static esp_err_t recover_connection_if_needed(void) {
    int64_t current_time = esp_timer_get_time();
    
    // If we've had too many failures recently, force recovery
    if (g_connection_failures > 3) {
        ESP_LOGW(TAG, "Too many connection failures (%d), forcing connection recovery", g_connection_failures);
        
        // Clean up existing connection
        cleanup_reusable_http_client();
        
        // Wait a bit to let network stabilize
        vTaskDelay(pdMS_TO_TICKS(2000));
        
        // Reinitialize
        esp_err_t result = init_reusable_http_client();
        if (result == ESP_OK) {
            g_connection_failures = 0;
            g_last_successful_connection = current_time;
            ESP_LOGI(TAG, "Connection recovery successful");
        } else {
            ESP_LOGE(TAG, "Connection recovery failed");
        }
        return result;
    }
    
    return ESP_OK;
}

// Removed write_task function - now using direct file write instead of ring buffer system
// ---- sync & re-download
int get_file_size(const char *path) {
    if (!path)
        return -1;

    struct stat st;
    if (stat(path, &st) != 0)
        return -1;

    if (st.st_size < 0 || st.st_size > 0x7FFFFFFF)
        return -1;

    return (int) st.st_size;
}

// Network condition detection
typedef enum {
    NETWORK_FAST = 0,   // Taiwan-like: >100KB/s, low latency
    NETWORK_SLOW,       // Brazil-like: <50KB/s, high latency  
    NETWORK_UNKNOWN     // Initial state
} network_condition_t;

// Ring buffer download state
typedef struct {
    RingbufHandle_t ring_buffer;
    TaskHandle_t write_task_handle;
    FILE *file_handle;
    volatile bool download_complete;
    volatile bool write_error;
    volatile esp_err_t error_code;
    int total_downloaded;
    int content_length;
    char temp_path[512];
    SemaphoreHandle_t completion_semaphore;
    
    // Network adaptation
    network_condition_t network_condition;
    int64_t download_start_time;
    int ring_buffer_timeout_ms;
    int write_task_timeout_ms;
    bool use_direct_fallback;
} download_context_t;

// Central download speed calculation function - eliminates code duplication and ensures consistent calculation
typedef struct {
    float current_speed_kbps;    // Speed for current measurement window
    float overall_avg_speed_kbps; // Overall average speed since download started
    float mb_downloaded;         // Total MB downloaded
    float completion_percent;    // Download completion percentage (-1 if unknown)
} download_speed_stats_t;

static download_speed_stats_t calculate_download_speed(
    int64_t measurement_start_us,    // Start time of current measurement window
    int64_t measurement_end_us,      // End time of current measurement window
    int bytes_in_window,             // Bytes downloaded in current measurement window
    int total_bytes_downloaded,      // Total bytes downloaded since start
    int64_t download_start_us,       // Overall download start time
    int expected_total_bytes         // Expected total file size (-1 if unknown)
) {
    download_speed_stats_t stats = {0};
    
    // Calculate time intervals
    float window_seconds = (measurement_end_us - measurement_start_us) / 1000000.0f;
    float total_seconds = (measurement_end_us - download_start_us) / 1000000.0f;
    
    // Current speed for this measurement window (accurate calculation)
    if (window_seconds > 0.0f) {
        stats.current_speed_kbps = bytes_in_window / (1024.0f * window_seconds);
    }
    
    // Overall average speed since download started
    if (total_seconds > 0.0f) {
        stats.overall_avg_speed_kbps = total_bytes_downloaded / (1024.0f * total_seconds);
    }
    
    // Total MB downloaded
    stats.mb_downloaded = total_bytes_downloaded / (1024.0f * 1024.0f);
    
    // Completion percentage
    if (expected_total_bytes > 0) {
        stats.completion_percent = (total_bytes_downloaded * 100.0f) / expected_total_bytes;
    } else {
        stats.completion_percent = -1.0f; // Unknown total size
    }
    
    return stats;
}

// Network condition detection
static network_condition_t detect_network_condition(int64_t download_time_us, int bytes_downloaded) {
    if (bytes_downloaded < 1024) return NETWORK_UNKNOWN; // Need some data first
    
    float speed_kbps = (bytes_downloaded / 1024.0f) / (download_time_us / 1000000.0f);
    
    if (speed_kbps > 80.0f) {
        return NETWORK_FAST;  // Taiwan-like performance
    } else if (speed_kbps < 50.0f) {
        return NETWORK_SLOW;  // Brazil-like performance
    }
    
    return NETWORK_UNKNOWN;
}

// Direct download fallback (without ring buffer)
static esp_err_t direct_download_fallback(char *url, char *tempPath) {
    ESP_LOGW(TAG, "Using enhanced direct download fallback (resume + retry enabled) ");

    FILE *file = NULL;
    long file_offset = 0;
    bool resume_mode = false;

    struct stat st;
    if (stat(tempPath, &st) == 0 && st.st_size > 0) {
        file_offset = st.st_size;
        resume_mode = true;
        ESP_LOGW(TAG, "Existing temp file found with size: %ld bytes", file_offset);
    }

    esp_http_client_config_t config = {
            .url = url,
            .buffer_size = 4096,  // Smaller buffer for slow networks
            .timeout_ms = 30000,  // Longer timeout for slow networks
            .crt_bundle_attach = esp_crt_bundle_attach,  // Disabled for max speed
            .keep_alive_enable = true,
    };

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == NULL) {
        ESP_LOGE(TAG, "Failed to init HTTP client");
        return ESP_FAIL;
    }

    // Ensure no compression to reduce CPU and latency
    esp_http_client_set_header(client, "Accept-Encoding", "identity");

    // 若續傳則加上 Range header
    char range_header[64];
    if (resume_mode) {
        snprintf(range_header, sizeof(range_header), "bytes=%ld-", file_offset);
        esp_http_client_set_header(client, "Range", range_header);
        ESP_LOGI(TAG, "Resuming download from offset: %ld bytes", file_offset);
    }

    esp_err_t err = esp_http_client_open(client, 0);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Direct fallback: Failed to open HTTP connection: %s", esp_err_to_name(err));
        esp_http_client_cleanup(client);
        return err;
    }

    int actual_content_length = esp_http_client_fetch_headers(client);
    if (resume_mode)
        actual_content_length = actual_content_length + file_offset;
    ESP_LOGI(TAG, "Direct fallback: Content length: %d", actual_content_length);

    file = s3_fopen(tempPath, resume_mode ? "ab" : "wb");
    if (file == NULL) {
        ESP_LOGE(TAG, "Direct fallback: Failed to open file for writing: %s", tempPath);
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return ESP_FAIL;
    }

    char buffer[4096];
    int total_downloaded = 0;
    int64_t start_us = esp_timer_get_time();
    int64_t last_log_us = start_us;
    int read_len_this_second = 0;
    int zero_read_count = 0;    // 記錄連續 0 bytes 的次數
    const int max_zero_read_retry = 0; // 最大 retry 次數

    total_downloaded = file_offset;
    while (!gWiFi_SYNC_USER_INTERRUPT) {
        int read_len = esp_http_client_read(client, buffer, sizeof(buffer));
        if (read_len <= 0) {
            zero_read_count++;
            ESP_LOGI(TAG, "No data %d/5 retries", zero_read_count);
            if (zero_read_count < max_zero_read_retry) {
                vTaskDelay(50 / portTICK_PERIOD_MS);
                continue;
            }
            else
                break;
        }
        zero_read_count = 0;

        s3_fwrite(buffer, 1, read_len, file);
        total_downloaded += read_len;
        read_len_this_second += read_len;

        // Log speed every second using central speed calculation
        int64_t now_us = esp_timer_get_time();
        if (now_us - last_log_us >= 1000000) {
            download_speed_stats_t stats = calculate_download_speed(
                    last_log_us, now_us, read_len_this_second,
                    total_downloaded, start_us, actual_content_length
            );

            ESP_LOGI(TAG, "Link: %.1f KB/s | Downloaded: %.1f MB (%.1f%%) | Method: Direct Fallback",
                     stats.current_speed_kbps, stats.mb_downloaded, stats.completion_percent);
            last_log_us = now_us;
            read_len_this_second = 0;
        }
        vTaskDelay(10 / portTICK_PERIOD_MS);
    }

    s3_fclose(file);
    esp_http_client_close(client);
    esp_http_client_cleanup(client);

    if (gWiFi_SYNC_USER_INTERRUPT) {
        ESP_LOGW(TAG, "Direct fallback: Download interrupted");
        return ESP_FAIL;
    }

    if (actual_content_length > 0 && total_downloaded < actual_content_length) {
        ESP_LOGW(TAG, "File incomplete (%d/%d bytes) — can resume next time", total_downloaded, actual_content_length);
        return 999;
    }else{
        ESP_LOGI(TAG, "Direct fallback: Downloaded %d bytes successfully", total_downloaded);
        return ESP_OK;
    }

}

static void write_task(void *param) {
    download_context_t *ctx = (download_context_t *)param;
    size_t received_size = 0;
    void *received_data = NULL;
    
    // Monitor stack usage for debugging
    UBaseType_t stack_high_water_mark = uxTaskGetStackHighWaterMark(NULL);
    ESP_LOGI(TAG, "Write task started for: %s (stack available: %d bytes)", ctx->temp_path, stack_high_water_mark * sizeof(StackType_t));
    
    // Use very short timeout initially for maximum responsiveness
    TickType_t short_timeout = pdMS_TO_TICKS(50);   // 50ms for fast response
    TickType_t long_timeout = pdMS_TO_TICKS(1000);  // 1s fallback timeout
    
    int no_data_count = 0;
    
    while (1) {
        // Dynamically adjust timeout based on data availability
        TickType_t current_timeout = (no_data_count > 2) ? long_timeout : short_timeout;
        
        // Receive data from ring buffer with adaptive timeout
        received_data = xRingbufferReceive(ctx->ring_buffer, &received_size, current_timeout);
        
        if (received_data) {
            // Reset no-data counter on successful receive
            no_data_count = 0;
            
            // Write received data to file immediately
            s3_fwrite(received_data, 1, received_size, ctx->file_handle);
            vRingbufferReturnItem(ctx->ring_buffer, received_data);
            
            // Optional: Force flush for small files to ensure data is written
            if (received_size < 4096) {
                fflush(ctx->file_handle);
            }
        } else {
            no_data_count++;
            
            if (ctx->download_complete) {
                break;
            }
            
            // If we've been waiting too long, something might be wrong
            if (no_data_count > 20) {  // 20 * 1000ms = 20 seconds max
                ESP_LOGW(TAG, "Write task: Long wait for data, checking completion status");
                if (ctx->download_complete) break;
                no_data_count = 10; // Reset but stay in long timeout mode
            }
        }
    }
    
    // Ensure all data is written to disk
    fflush(ctx->file_handle);
    
    ESP_LOGI(TAG, "Write task completed for: %s", ctx->temp_path);
    
    // Signal completion
    if (ctx->completion_semaphore) {
        xSemaphoreGive(ctx->completion_semaphore);
    }
    
    vTaskDelete(NULL);
}

esp_err_t content_download(char *url, char *fullPath) {
    // Use safe download: download to temporary file first
    char tempPath[320];
    snprintf(tempPath, sizeof(tempPath), "%s.tmp", fullPath);
    // Remove old temp file if exists to ensure clean download
    if (access(tempPath, F_OK) == 0) {
        ESP_LOGW(TAG, "content_download remove:%s", tempPath);
        s3_remove(tempPath);
    }
    // Ensure directory exists for new download
    create_directories(tempPath);

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

    // FORCE: Always use direct fallback to minimize DMA/internal heap usage and fragmentation
    for (int i = 0; i < 3 ; i++) {
        int res = direct_download_fallback(url, tempPath);
        if (res == 999)
            continue;
        else
            return res;
    }
    return ESP_FAIL;
}

void free_baby_packs(void) {
    if (!gBabyPack || gBabyPackCount <= 0)
        return;

    for (int i = 0; i < gBabyPackCount; i++) {
        free(gBabyPack[i].skuId);
        free(gBabyPack[i].language);
    }
    free(gBabyPack);
    gBabyPack = NULL;
    gBabyPackCount = 0;
}

void free_alarms(void) {
    if (!gAlarms || gAlarmsCount <= 0)
        return;
    for (int i = 0; i < gAlarmsCount; i++) {
        free(gAlarms[i].time);
        free(gAlarms[i].period);
        free(gAlarms[i].filename);
    }
    free(gAlarms);
    gAlarms = NULL;
    gAlarmsCount = 0;
}

void free_NFCs(void) {
    if (!gNfcs || gNfcCount <= 0)
        return;
    for (int i = 0; i < gNfcCount; i++) {
        for (int j = 0; j < gNfcs[i].skusCount; j++) {
            free(gNfcs[i].skus[j].skuId);
            free(gNfcs[i].skus[j].language);
        }
        free(gNfcs[i].skus);
        free(gNfcs[i].sn);
        free(gNfcs[i].linked);
    }
    free(gNfcs);
    gNfcs = NULL;
    gNfcCount = 0;
}

void free_filename_contentid_map(void) {
    if (!gFilenameContentIdMap || gFilenameContentIdMapCount <= 0)
        return;
    
    for (int i = 0; i < gFilenameContentIdMapCount; i++) {
        free(gFilenameContentIdMap[i].filename_lowercase);
        free(gFilenameContentIdMap[i].contentId);
    }
    free(gFilenameContentIdMap);
    gFilenameContentIdMap = NULL;
    gFilenameContentIdMapCount = 0;
    ESP_LOGI(TAG, "Filename-ContentId mapping freed");
}

// Helper function to convert filename to lowercase
static char* to_lowercase(const char* str) {
    if (!str) return NULL;
    
    int len = strlen(str);
    char* lower = heap_caps_malloc(len + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!lower) {
        ESP_LOGE(TAG, "Failed to allocate memory for lowercase filename");
        return NULL;
    }
    
    for (int i = 0; i <= len; i++) {
        lower[i] = tolower((unsigned char)str[i]);
    }
    
    return lower;
}

// Helper function to add filename-contentId mapping
static void add_filename_contentid_mapping(const char* filename, const char* contentId) {
    if (!filename || !contentId) {
        ESP_LOGW(TAG, "Invalid filename or contentId for mapping");
        return;
    }
    
    // Convert filename to lowercase
    char* filename_lower = to_lowercase(filename);
    if (!filename_lower) {
        ESP_LOGE(TAG, "Failed to convert filename to lowercase: %s", filename);
        return;
    }
    
    // Check if mapping already exists
    for (int i = 0; i < gFilenameContentIdMapCount; i++) {
        if (strcmp(gFilenameContentIdMap[i].filename_lowercase, filename_lower) == 0) {
            ESP_LOGD(TAG, "Mapping already exists for filename: %s -> %s", filename, contentId);
            free(filename_lower);
            return;
        }
    }
    
    // Reallocate map array
    filename_contentid_entry_t* new_map = heap_caps_realloc(
        gFilenameContentIdMap,
        (gFilenameContentIdMapCount + 1) * sizeof(filename_contentid_entry_t),
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT
    );
    
    if (!new_map) {
        ESP_LOGE(TAG, "Failed to reallocate filename-contentId map");
        free(filename_lower);
        return;
    }
    
    gFilenameContentIdMap = new_map;
    
    // Add new mapping
    gFilenameContentIdMap[gFilenameContentIdMapCount].filename_lowercase = filename_lower;
    gFilenameContentIdMap[gFilenameContentIdMapCount].contentId = heap_caps_malloc(strlen(contentId) + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    
    if (!gFilenameContentIdMap[gFilenameContentIdMapCount].contentId) {
        ESP_LOGE(TAG, "Failed to allocate memory for contentId: %s", contentId);
        free(filename_lower);
        return;
    }
    
    strcpy(gFilenameContentIdMap[gFilenameContentIdMapCount].contentId, contentId);
    gFilenameContentIdMapCount++;
    
    ESP_LOGD(TAG, "Added mapping: %s -> %s", filename, contentId);
}

// Public API function to get contentId by filename
const char* GetContentId(const char* filename) {
    if (!filename) {
        ESP_LOGW(TAG, "GetContentId called with NULL filename");
        return NULL;
    }
    
    if (!gFilenameContentIdMap || gFilenameContentIdMapCount <= 0) {
        ESP_LOGW(TAG, "Filename-ContentId map is empty or not initialized");
        return NULL;
    }
    
    // Convert input filename to lowercase
    char* filename_lower = to_lowercase(filename);
    if (!filename_lower) {
        ESP_LOGE(TAG, "Failed to convert query filename to lowercase: %s", filename);
        return NULL;
    }
    
    // Search for matching filename
    for (int i = 0; i < gFilenameContentIdMapCount; i++) {
        if (strcmp(gFilenameContentIdMap[i].filename_lowercase, filename_lower) == 0) {
            free(filename_lower);
            ESP_LOGD(TAG, "Found contentId for %s: %s", filename, gFilenameContentIdMap[i].contentId);
            return gFilenameContentIdMap[i].contentId;
        }
    }
    
    free(filename_lower);
    ESP_LOGD(TAG, "No contentId found for filename: %s", filename);
    return NULL;
}

// Comparison result enum (used for both resource and content differential updates)
typedef enum {
    RESOURCE_UNCHANGED = 0,  // Identical in both manifests
    RESOURCE_MODIFIED = 1,   // Path exists but URL/size changed
    RESOURCE_NEW = 2         // New file in updated manifest
} resource_change_type_t;

// Content entry for differential updates (account_file.json)
typedef struct {
    char *fullPath;    // Full file path (e.g., "/sdcard/content/full/SKU123/audio.mp3")
    char *url;         // Download URL
    int size;          // File size in bytes
    char *contentType; // "babypack", "alarm", "nfc"
} content_entry_t;

// Forward declarations for content manifest helper functions
static content_entry_t* parse_account_contents_manifest(const char *json_path, int *out_count);
static void free_content_entries(content_entry_t *entries, int count);
static resource_change_type_t compare_content_entry(
    const content_entry_t *new_entry,
    const content_entry_t *old_entries,
    int old_count
);


esp_err_t parser_account_contents(int justPaserContent) {
    ESP_LOGI(TAG, "parser_account_contents justPaser? %d", justPaserContent );

    // Clean up existing data and filename-contentId map
    free_baby_packs();
    free_alarms();
    free_NFCs();
    free_filename_contentid_map();

    char *buf = read_file_to_spiram("/sdcard/tmp/" CLOUD_ACCOUNT_FILENAME);
    if (buf == NULL) {
        return ESP_FAIL;
    }
    ESP_LOGD(TAG, "%s",buf);  // print account_file.json

    cJSON *root = cJSON_Parse(buf);
    if (root == NULL) {
        ESP_LOGE(TAG, "Failed to parse JSON");
        free(buf);
        return ESP_FAIL;
    }
    const cJSON *result = cJSON_GetObjectItem(root, "result");
    if (!cJSON_IsObject(result)) {
        ESP_LOGE(TAG, "No 'result' object in JSON");
        goto _FAIL;
    }

    const cJSON *babyId = cJSON_GetObjectItem(result, "babyId");
    if (!cJSON_IsString(babyId)) {
        ESP_LOGE(TAG, "'babyId' is not an string");
        goto _FAIL;
    }
	strncpy(s3_babyId, babyId->valuestring, 32 - 1);
	s3_babyId[32 - 1] = '\0';
	ESP_LOGI(TAG, "babyId:%s", s3_babyId);

    const cJSON *kid = cJSON_GetObjectItem(result, "kidCharacter");
    if (!cJSON_IsString(kid)) {
        ESP_LOGE(TAG, "'kid' is not an string");
        goto _FAIL;
    }
    strcpy(s3_WiFiSyncKidIcon, kid->valuestring);
    ESP_LOGI(TAG, "kid:%s",s3_WiFiSyncKidIcon);

    // ========== DIFFERENTIAL UPDATE PREPARATION START ==========

    // Skip differential logic if justPaserContent (only parsing metadata)
    content_entry_t *new_entries = NULL;
    content_entry_t *old_entries = NULL;
    int new_count = 0;
    int old_count = 0;
    bool has_old_manifest = false;

    if (!justPaserContent) {
        // Log memory status before parsing to diagnose issues
        ESP_LOGI(TAG, "Pre-parse memory: Free heap: %u bytes, Free SPIRAM: %u bytes",
            heap_caps_get_free_size(MALLOC_CAP_8BIT),
            heap_caps_get_free_size(MALLOC_CAP_SPIRAM));

        // Step 1: Parse NEW manifest FIRST (critical - must succeed)
        new_entries = parse_account_contents_manifest("/sdcard/tmp/" CLOUD_ACCOUNT_FILENAME, &new_count);
        if (!new_entries) {
            ESP_LOGW(TAG, "Failed to parse new content manifest - proceeding without differential update");
        } else {
            ESP_LOGI(TAG, "Parsed %d entries from new content manifest", new_count);

            // Log memory after parsing new manifest
            ESP_LOGI(TAG, "Post-new-manifest memory: Free heap: %u bytes, Free SPIRAM: %u bytes",
                heap_caps_get_free_size(MALLOC_CAP_8BIT),
                heap_caps_get_free_size(MALLOC_CAP_SPIRAM));

            // Step 2: Check for and parse OLD manifest (optional - for differential comparison)
            char backup_path[256];
            snprintf(backup_path, sizeof(backup_path), "/sdcard/tmp/%s.bak", CLOUD_ACCOUNT_FILENAME);
            has_old_manifest = (access(backup_path, F_OK) == 0);

            if (has_old_manifest) {
                ESP_LOGI(TAG, "Found old content manifest backup, parsing for differential update");
                old_entries = parse_account_contents_manifest(backup_path, &old_count);
                if (old_entries) {
                    ESP_LOGI(TAG, "Parsed %d entries from old content manifest", old_count);
                } else {
                    ESP_LOGW(TAG, "Failed to parse old content manifest, will download all files");
                    has_old_manifest = false;
                }
            } else {
                ESP_LOGI(TAG, "No old content manifest backup found, will download all files");
            }

            // Step 3: Compare manifests and count changes
            int unchanged_count = 0;
            int modified_count = 0;
            int new_file_count = 0;

            for (int i = 0; i < new_count; i++) {
                content_entry_t *entry = &new_entries[i];
                resource_change_type_t change_type = compare_content_entry(entry, old_entries, old_count);

                switch (change_type) {
                    case RESOURCE_UNCHANGED:
                        unchanged_count++;
                        break;
                    case RESOURCE_MODIFIED:
                        modified_count++;
                        break;
                    case RESOURCE_NEW:
                        new_file_count++;
                        break;
                }
            }

            ESP_LOGI(TAG, "========== CONTENT DIFFERENTIAL UPDATE SUMMARY ==========");
            ESP_LOGI(TAG, "Total files in new manifest: %d", new_count);
            ESP_LOGI(TAG, "Unchanged files: %d (will skip if exists)", unchanged_count);
            ESP_LOGI(TAG, "Modified files: %d (will re-download)", modified_count);
            ESP_LOGI(TAG, "New files: %d (will download)", new_file_count);
            ESP_LOGI(TAG, "Expected downloads: %d", modified_count + new_file_count);
            ESP_LOGI(TAG, "=========================================================");

            // Note: Keep old_entries for download loops - will free at function end
        }
    }

    // ========== DIFFERENTIAL UPDATE PREPARATION END ==========

    char path[128];
    char fullName[128];

    // babyPacks
    const cJSON *babyPacks = cJSON_GetObjectItem(result, "babyPacks");
    if (!cJSON_IsArray(babyPacks)) {
        ESP_LOGE(TAG, "'babyPacks' is not an array");
        goto _FAIL;
    }

    gBabyPackCount = 0;
    gBabyPackCount = cJSON_GetArraySize(babyPacks);
    ESP_LOGI(TAG, "babyPacks count: %d", gBabyPackCount);
    if (gBabyPackCount <= 0) {
        ESP_LOGE(TAG, "No babyPacks in JSON");
    }else {
        gBabyPack = heap_caps_calloc(gBabyPackCount, sizeof(s3_babyPack_t), MALLOC_CAP_SPIRAM);
    }

    for (int i = 0; i < gBabyPackCount; i++) {

        cJSON *pack = cJSON_GetArrayItem(babyPacks, i);
        if (!cJSON_IsObject(pack))
            continue;

        cJSON *skuId = cJSON_GetObjectItem(pack, "skuId");
        cJSON *language = cJSON_GetObjectItem(pack, "language");
        cJSON *contents = cJSON_GetObjectItem(pack, "contents");
        if (!cJSON_IsArray(contents))
            continue;
        int contentCount = cJSON_GetArraySize(contents);

        if (cJSON_IsString(skuId))
            gBabyPack[i].skuId = strdup_spiram(skuId->valuestring);
        
        if (cJSON_IsString(language))
            gBabyPack[i].language = strdup_spiram(language->valuestring);
        else
            gBabyPack[i].language = strdup_spiram("en-us"); // Default fallback
        
        // Store the content count from JSON
        gBabyPack[i].contentCount = contentCount;

        cJSON *expiresAt = cJSON_GetObjectItem(pack, "expiresAt");
        if (cJSON_IsNumber(expiresAt)) {
            gBabyPack[i].expiresAt = (unsigned int)expiresAt->valuedouble;
        } else {
            gBabyPack[i].expiresAt = 0;
        }

        // ~~~ get contents ~~~
        for (int j = 0; j < contentCount; j++) {
        	if (gWiFi_SYNC_USER_INTERRUPT)
        		goto _FAIL;
            cJSON *item = cJSON_GetArrayItem(contents, j);
            if (!cJSON_IsObject(item))
                continue;

            cJSON *url = cJSON_GetObjectItem(item, "url");
            cJSON *fileSize = cJSON_GetObjectItem(item, "fileSize");
            cJSON *filename = cJSON_GetObjectItem(item, "filename");
            cJSON *contentId = cJSON_GetObjectItem(item, "contentId");

            if (!(cJSON_IsString(filename) && cJSON_IsString(url) && cJSON_IsNumber(fileSize)))
                continue;
                
            // Add filename-contentId mapping
            if (cJSON_IsString(contentId)) {
                add_filename_contentid_mapping(filename->valuestring, contentId->valuestring);
            }
            if (justPaserContent)
            	continue;

            ESP_LOGD(TAG, "  [%d] filename: %s", j, cJSON_IsString(filename) ? filename->valuestring : "N/A");
            ESP_LOGD(TAG, "      url: %s", cJSON_IsString(url) ? url->valuestring : "N/A");
            ESP_LOGD(TAG, "      size: %d", cJSON_IsNumber(fileSize) ? fileSize->valueint : -1);

            memset(path, 0, sizeof(path));
            memset(fullName, 0, sizeof(fullName));
            sprintf(path, SDCARD_CONTENT_PATH, skuId->valuestring);
            sprintf(fullName, SDCARD_CONTENT_FULLNAME, skuId->valuestring, filename->valuestring);

            // ========== DIFFERENTIAL UPDATE LOGIC (BabyPacks) ==========
            // Find this file in the new_entries array to determine change type
            resource_change_type_t change_type = RESOURCE_NEW;
            content_entry_t *matching_entry = NULL;

            if (new_entries && new_count > 0) {
                for (int k = 0; k < new_count; k++) {
                    if (strcmp(new_entries[k].fullPath, fullName) == 0) {
                        matching_entry = &new_entries[k];
                        change_type = compare_content_entry(matching_entry, old_entries, old_count);
                        break;
                    }
                }
            }

            int size = get_file_size(fullName);

            // Check if file is unchanged and exists with correct size
            if (change_type == RESOURCE_UNCHANGED && size >= 0 && size == fileSize->valueint) {
                ESP_LOGI(TAG, "[BabyPack UNCHANGED - skipped]: %s (size: %d bytes)", fullName, size);
                continue;  // Skip downloading this file
            }

            // File needs to be downloaded (either MODIFIED or NEW, or UNCHANGED but missing/corrupted)
            if (change_type == RESOURCE_MODIFIED) {
                ESP_LOGI(TAG, "[BabyPack MODIFIED - download]: %s", fullName);
            } else if (change_type == RESOURCE_NEW) {
                ESP_LOGI(TAG, "[BabyPack NEW - download]: %s", fullName);
            }

            // Initialize attempt counter for this file
            int attempt = 1;
            ESP_LOGI(TAG, "[AudioData n%d - start]: %s", attempt, fullName);

            if (size >= 0 && size == fileSize->valueint) {
                ESP_LOGI(TAG, "[AudioData n%d - success]: %s", attempt, fullName);
            } else {
                ESP_LOGW(TAG, "[AudioData n%d - fail]: %s, received:%d, expected:%d", attempt, fullName, size, fileSize->valueint);
                attempt++; // Increment for actual download attempts
                
                // Download to tmp/downloads instead of overwriting original
                char downloadPath[300];
                const char* filename_only = strrchr(fullName, '/');
                if (filename_only) filename_only++; else filename_only = fullName;
                snprintf(downloadPath, sizeof(downloadPath), "/sdcard/tmp/downloads/%s", filename_only);
                create_directories(downloadPath);

                // Try download with retry tracking
                esp_err_t result = ESP_FAIL;
                while (attempt <= MAX_DOWNLOAD_ATTEMPTS && result != ESP_OK) {
                    ESP_LOGI(TAG, "[AudioData n%d - start]: %s", attempt, downloadPath);
                    result = content_download(url->valuestring, downloadPath);
                    
                    if (result != ESP_OK) {
                        ESP_LOGW(TAG, "[AudioData n%d - fail]: download failed", attempt);
                        attempt++;
                    } else {
                        ESP_LOGI(TAG, "[AudioData n%d - success]: %s", attempt, downloadPath);
                    }
                }
                
                if (result == ESP_OK) {
                    // content_download leaves file at downloadPath.tmp, check that file
                    char tempDownloadPath[320];
                    snprintf(tempDownloadPath, sizeof(tempDownloadPath), "%s.tmp", downloadPath);
                    int download_size = get_file_size(tempDownloadPath);
                    if (download_size == fileSize->valueint) {
                        // Download successful and size matches - replace original atomically with backup strategy
                        // Ensure destination directory exists before replacement
                        create_directories(fullName);
                        
                        char backupPath[256];
                        snprintf(backupPath, sizeof(backupPath), "%s.bak", fullName);
                        if (access(backupPath, F_OK) == 0) {
                            unlink(backupPath);
                        }

                        // Step 1: Rename original to backup (if original exists)
                        bool had_original = (access(fullName, F_OK) == 0);
                        if (had_original == false)
                            create_directories(backupPath);
                        if (had_original && s3_rename(fullName, backupPath) != 0) {
                            ESP_LOGE(TAG, "Failed to backup original file %s", fullName);
                        } else {
                            // Step 2: Move temp to original place
                            if (s3_rename(tempDownloadPath, fullName) == 0) {
                                // Step 3: Success - remove backup
                                if (had_original) s3_remove(backupPath);
                                ESP_LOGI(TAG, "Successfully replaced %s (%d bytes)", fullName, download_size);
                            } else {
                                // Step 4: Failed - restore original from backup
                                if (had_original) s3_rename(backupPath, fullName);
                                ESP_LOGE(TAG, "Failed to replace %s - original restored, downloaded file kept at %s", fullName, tempDownloadPath);
                            }
                        }
                    } else {
                        ESP_LOGE(TAG, "Downloaded file size mismatch: expected %d, got %d - kept at %s", fileSize->valueint, download_size, downloadPath);
                    }
                } else {
                    ESP_LOGE(TAG, "Download failed for %s", fullName);
                }
            }
        }
    }

    if (gWiFi_SYNC_USER_INTERRUPT)
        goto _FAIL;

    // alarm
    const cJSON *alarms = cJSON_GetObjectItem(result, "alarms");
    if (!cJSON_IsArray(alarms)) {
        ESP_LOGE(TAG, "'alarms' is not an array");
        goto _FAIL;
    }

    gAlarmsCount = 0;
    gAlarmsCount = cJSON_GetArraySize(alarms);
    ESP_LOGI(TAG, "alarms count: %d", gAlarmsCount);
    if (gAlarmsCount <= 0) {
        ESP_LOGE(TAG, "No alarms in JSON");
    } else {
        gAlarms = heap_caps_calloc(gAlarmsCount, sizeof(s3_alarm_t), MALLOC_CAP_SPIRAM);
    }

    for (int i = 0; i < gAlarmsCount; i++) {
        cJSON *alarm = cJSON_GetArrayItem(alarms, i);
        if (!cJSON_IsObject(alarm))
            continue;

        cJSON *time = cJSON_GetObjectItem(alarm, "time");
        cJSON *period = cJSON_GetObjectItem(alarm, "period");
        cJSON *audio = cJSON_GetObjectItem(alarm, "audio");
        cJSON *filename = cJSON_GetObjectItem(alarm, "filename");
        cJSON *fileSize = cJSON_GetObjectItem(alarm, "fileSize");
        cJSON *days = cJSON_GetObjectItem(alarm, "days");
        if (!(cJSON_IsString(audio) && cJSON_IsString(filename) && cJSON_IsNumber(fileSize) && cJSON_IsArray(days)))
            continue;

        gAlarms[i].time = strdup_spiram(cJSON_IsString(time) ? time->valuestring : "UNKNOWN");
        gAlarms[i].period = strdup_spiram(cJSON_IsString(period) ? period->valuestring : "UNKNOWN");
        gAlarms[i].filename = strdup_spiram(cJSON_IsString(filename) ? filename->valuestring : "UNKNOWN");
        int day_count = cJSON_GetArraySize(days);
        for (int d = 0; d < Days_Size; d++) {
            gAlarms[i].days[d] = 0;
        }
        for (int j = 0; j < day_count; j++) {
            cJSON *day = cJSON_GetArrayItem(days, j);
            if (cJSON_IsString(day)) {
                for (int d = 0; d < Days_Size; d++) {
                    if (strcmp(day->valuestring, s3_days_array[d]) == 0) {
                        gAlarms[i].days[d] = 1;
                        break;
                    }
                }
                ESP_LOGD(TAG, "Day %d: %s", j, day->valuestring);
            }
        }
        ESP_LOGD(TAG, "  [%d] filename: %s", i, cJSON_IsString(filename) ? filename->valuestring : "N/A");
        ESP_LOGD(TAG, "      url: %s", cJSON_IsString(audio) ? audio->valuestring : "N/A");
        ESP_LOGD(TAG, "      size: %d", cJSON_IsNumber(fileSize) ? fileSize->valueint : -1);
        if (justPaserContent)
            continue;



        memset(path, 0, sizeof(path));
        memset(fullName, 0, sizeof(fullName));
        sprintf(path, SDCARD_CONTENT_PATH, "alarms");
        sprintf(fullName, SDCARD_CONTENT_FULLNAME, "alarms", filename->valuestring);

        // ========== DIFFERENTIAL UPDATE LOGIC (Alarms) ==========
        // Find this file in the new_entries array to determine change type
        resource_change_type_t change_type = RESOURCE_NEW;
        content_entry_t *matching_entry = NULL;

        if (new_entries && new_count > 0) {
            for (int k = 0; k < new_count; k++) {
                if (strcmp(new_entries[k].fullPath, fullName) == 0) {
                    matching_entry = &new_entries[k];
                    change_type = compare_content_entry(matching_entry, old_entries, old_count);
                    break;
                }
            }
        }

        int size = get_file_size(fullName);

        // Check if file is unchanged and exists with correct size
        if (change_type == RESOURCE_UNCHANGED && size >= 0 && size == fileSize->valueint) {
            ESP_LOGI(TAG, "[Alarm UNCHANGED - skipped]: %s (size: %d bytes)", fullName, size);
            continue;  // Skip downloading this file
        }

        // File needs to be downloaded (either MODIFIED or NEW, or UNCHANGED but missing/corrupted)
        if (change_type == RESOURCE_MODIFIED) {
            ESP_LOGI(TAG, "[Alarm MODIFIED - download]: %s", fullName);
        } else if (change_type == RESOURCE_NEW) {
            ESP_LOGI(TAG, "[Alarm NEW - download]: %s", fullName);
        }

        // Initialize attempt counter for this file
        int attempt = 1;
        ESP_LOGI(TAG, "[AudioData n%d - start]: %s", attempt, fullName);

        if (size >= 0 && size == fileSize->valueint) {
            ESP_LOGI(TAG, "[AudioData n%d - success]: %s", attempt, fullName);
        } else {
            ESP_LOGW(TAG, "[AudioData n%d - fail]: %s, received:%d, expected:%d", attempt, fullName, size, fileSize->valueint);
            attempt++; // Increment for actual download attempts
            
            // Download to tmp/downloads instead of overwriting original
            char downloadPath[300];
            const char* filename_only = strrchr(fullName, '/');
            if (filename_only) filename_only++; else filename_only = fullName;
            snprintf(downloadPath, sizeof(downloadPath), "/sdcard/tmp/downloads/%s", filename_only);
            create_directories(downloadPath);

            // Try download with retry tracking
            esp_err_t result = ESP_FAIL;
            while (attempt <= MAX_DOWNLOAD_ATTEMPTS && result != ESP_OK) {
                ESP_LOGI(TAG, "[AudioData n%d - start]: %s", attempt, downloadPath);
                result = content_download(audio->valuestring, downloadPath);
                
                if (result != ESP_OK) {
                    ESP_LOGW(TAG, "[AudioData n%d - fail]: download failed", attempt);
                    attempt++;
                } else {
                    ESP_LOGI(TAG, "[AudioData n%d - success]: %s", attempt, downloadPath);
                }
            }
            
                if (result == ESP_OK) {
                    // content_download leaves file at downloadPath.tmp, check that file
                    char tempDownloadPath[320];
                    snprintf(tempDownloadPath, sizeof(tempDownloadPath), "%s.tmp", downloadPath);
                    int download_size = get_file_size(tempDownloadPath);
                    if (download_size == fileSize->valueint) {
                        // Download successful and size matches - replace original atomically with backup strategy
                        // Ensure destination directory exists before replacement
                        create_directories(fullName);
                        
                        char backupPath[256];
                        snprintf(backupPath, sizeof(backupPath), "%s.bak", fullName);
                        if (access(backupPath, F_OK) == 0) {
                            unlink(backupPath);
                        }
                    
                    // Step 1: Rename original to backup (if original exists)
                    bool had_original = (access(fullName, F_OK) == 0);
                    if (had_original == false)
                        create_directories(backupPath);
                    if (had_original && s3_rename(fullName, backupPath) != 0) {
                        ESP_LOGE(TAG, "Failed to backup original file %s", fullName);
                    } else {
                        // Step 2: Move temp to original place
                        if (s3_rename(tempDownloadPath, fullName) == 0) {
                            // Step 3: Success - remove backup
                            if (had_original) s3_remove(backupPath);
                            ESP_LOGI(TAG, "Successfully replaced %s (%d bytes)", fullName, download_size);
                        } else {
                            // Step 4: Failed - restore original from backup
                            if (had_original) s3_rename(backupPath, fullName);
                            ESP_LOGE(TAG, "Failed to replace %s - original restored, downloaded file kept at %s", fullName, tempDownloadPath);
                        }
                    }
                } else {
                    ESP_LOGE(TAG, "Downloaded file size mismatch: expected %d, got %d - kept at %s", fileSize->valueint, download_size, downloadPath);
                }
            } else {
                ESP_LOGE(TAG, "Download failed for %s", fullName);
            }
        }
    }
    if (gWiFi_SYNC_USER_INTERRUPT)
        goto _FAIL;

    // NFC
    const cJSON *NFCs = cJSON_GetObjectItem(result, "NFCs");
    if (!cJSON_IsArray(NFCs)) {
        ESP_LOGE(TAG, "'NFCs' is not an array");
        goto _FAIL;
    }

    gNfcCount = 0;
    gNfcCount = cJSON_GetArraySize(NFCs);
    ESP_LOGI(TAG, "NFCs count: %d", gNfcCount);
    if (gNfcCount <= 0) {
        ESP_LOGE(TAG, "No NFCs in JSON");
    } else {
        gNfcs = heap_caps_calloc(gNfcCount, sizeof(s3_nfc_t), MALLOC_CAP_SPIRAM);
    }

    for (int i = 0; i < gNfcCount; i++) {
        cJSON *nfc = cJSON_GetArrayItem(NFCs, i);
        if (!cJSON_IsObject(nfc))
            continue;
        cJSON *sn = cJSON_GetObjectItem(nfc, "sn");
        cJSON *linked = cJSON_GetObjectItem(nfc, "linked");
        cJSON *skus = cJSON_GetObjectItem(nfc, "skus");
        if (!(cJSON_IsArray(skus) && cJSON_IsString(sn) && cJSON_IsString(linked)))
            continue;
        gNfcs[i].linked = strdup_spiram(linked->valuestring);
        gNfcs[i].sn = strdup_spiram(sn->valuestring);

        int sku_count = cJSON_GetArraySize(skus);
        gNfcs[i].skusCount = sku_count;
        gNfcs[i].skus = heap_caps_calloc(sku_count, sizeof(s3_nfc_skus_t), MALLOC_CAP_SPIRAM);
        // ESP_LOGD(TAG, "sn:%s, linked:%s, files: %d", pNFCs[i].sn, pNFCs[i].linked, sku_count);
        // ~~~ get skus ~~~
        for (int j = 0; j < sku_count; j++) {
        	if (gWiFi_SYNC_USER_INTERRUPT)
        		goto _FAIL;
            cJSON *sku = cJSON_GetArrayItem(skus, j);
            if (!cJSON_IsObject(sku))
                continue;
            cJSON *skuId = cJSON_GetObjectItem(sku, "skuId");
            cJSON *language = cJSON_GetObjectItem(sku, "language");
            if (!(cJSON_IsString(skuId) && cJSON_IsString(language)))
                continue;

            // Count the contents array size to get file count, fallback to -1 if not available
            cJSON *contents = cJSON_GetObjectItem(sku, "contents");
            int content_count = cJSON_IsArray(contents) ? cJSON_GetArraySize(contents) : -1;
            
            ESP_LOGD(TAG, "skuid:%s, language:%s, contentCount:%d", skuId->valuestring, language->valuestring, content_count);
            gNfcs[i].skus[j].skuId = strdup_spiram(skuId->valuestring);
            gNfcs[i].skus[j].language = strdup_spiram(language->valuestring);
            gNfcs[i].skus[j].contentCount = content_count;

            cJSON *expiresAt = cJSON_GetObjectItem(sku, "expiresAt");
            if (cJSON_IsNumber(expiresAt)) {
                gNfcs[i].skus[j].expiresAt = (unsigned int)expiresAt->valuedouble;
            } else {
                gNfcs[i].skus[j].expiresAt = 0;
            }

            if (!cJSON_IsArray(contents))
                continue;
            for (int k = 0; k < content_count; k++) {
            	if (gWiFi_SYNC_USER_INTERRUPT)
            		goto _FAIL;
                cJSON *content = cJSON_GetArrayItem(contents, k);
                if (!cJSON_IsObject(content))
                    continue;

                cJSON *url = cJSON_GetObjectItem(content, "url");
                cJSON *fileSize = cJSON_GetObjectItem(content, "fileSize");
                cJSON *filename = cJSON_GetObjectItem(content, "filename");
                cJSON *contentId = cJSON_GetObjectItem(content, "contentId");

                if (!(cJSON_IsString(url) && cJSON_IsString(filename) && cJSON_IsNumber(fileSize)))
                    continue;
                    
                // Add filename-contentId mapping for NFC content
                if (cJSON_IsString(contentId)) {
                    add_filename_contentid_mapping(filename->valuestring, contentId->valuestring);
                }

                if (justPaserContent)
                	continue;

                ESP_LOGD(TAG, "  [%d] filename: %s", i, cJSON_IsString(filename) ? filename->valuestring : "N/A");
                ESP_LOGD(TAG, "      url: %s", cJSON_IsString(url) ? url->valuestring : "N/A");
                ESP_LOGD(TAG, "      size: %d", cJSON_IsNumber(fileSize) ? fileSize->valueint : -1);

                memset(path, 0, sizeof(path));
                memset(fullName, 0, sizeof(fullName));
                sprintf(path, SDCARD_CONTENT_PATH, skuId->valuestring);
                sprintf(fullName, SDCARD_CONTENT_FULLNAME, skuId->valuestring, filename->valuestring);

                // ========== DIFFERENTIAL UPDATE LOGIC (NFCs) ==========
                // Find this file in the new_entries array to determine change type
                resource_change_type_t change_type = RESOURCE_NEW;
                content_entry_t *matching_entry = NULL;

                if (new_entries && new_count > 0) {
                    for (int m = 0; m < new_count; m++) {
                        if (strcmp(new_entries[m].fullPath, fullName) == 0) {
                            matching_entry = &new_entries[m];
                            change_type = compare_content_entry(matching_entry, old_entries, old_count);
                            break;
                        }
                    }
                }

                int size = get_file_size(fullName);

                // Check if file is unchanged and exists with correct size
                if (change_type == RESOURCE_UNCHANGED && size >= 0 && size == fileSize->valueint) {
                    ESP_LOGI(TAG, "[NFC UNCHANGED - skipped]: %s (size: %d bytes)", fullName, size);
                    continue;  // Skip downloading this file
                }

                // File needs to be downloaded (either MODIFIED or NEW, or UNCHANGED but missing/corrupted)
                if (change_type == RESOURCE_MODIFIED) {
                    ESP_LOGI(TAG, "[NFC MODIFIED - download]: %s", fullName);
                } else if (change_type == RESOURCE_NEW) {
                    ESP_LOGI(TAG, "[NFC NEW - download]: %s", fullName);
                }

                // Initialize attempt counter for this file
                int attempt = 1;
                ESP_LOGI(TAG, "[AudioData n%d - start]: %s", attempt, fullName);

                if (size >= 0 && size == fileSize->valueint) {
                    ESP_LOGI(TAG, "[AudioData n%d - success]: %s", attempt, fullName);
                } else {
                    ESP_LOGW(TAG, "[AudioData n%d - fail]: %s, received:%d, expected:%d", attempt, fullName, size, fileSize->valueint);
                    attempt++; // Increment for actual download attempts
                    
                    // Download to tmp/downloads instead of overwriting original
                    char downloadPath[300];
                    const char* filename_only = strrchr(fullName, '/');
                    if (filename_only) filename_only++; else filename_only = fullName;
                    snprintf(downloadPath, sizeof(downloadPath), "/sdcard/tmp/downloads/%s", filename_only);
                    create_directories(downloadPath);

                    
                    // Try download with retry tracking
                    esp_err_t result = ESP_FAIL;
                    while (attempt <= MAX_DOWNLOAD_ATTEMPTS && result != ESP_OK) {
                        ESP_LOGI(TAG, "[AudioData n%d - start]: %s", attempt, downloadPath);
                        result = content_download(url->valuestring, downloadPath);
                        
                        if (result != ESP_OK) {
                            ESP_LOGW(TAG, "[AudioData n%d - fail]: download failed", attempt);
                            attempt++;
                        } else {
                            ESP_LOGI(TAG, "[AudioData n%d - success]: %s", attempt, downloadPath);
                        }
                    }
                    
                    if (result == ESP_OK) {
                        // content_download leaves file at downloadPath.tmp, check that file
                        char tempDownloadPath[320];
                        snprintf(tempDownloadPath, sizeof(tempDownloadPath), "%s.tmp", downloadPath);
                        int download_size = get_file_size(tempDownloadPath);
                        if (download_size == fileSize->valueint) {
                            // Download successful and size matches - replace original atomically with backup strategy
                            // Ensure destination directory exists before replacement
                            create_directories(fullName);
                            
                            char backupPath[256];
                            snprintf(backupPath, sizeof(backupPath), "%s.bak", fullName);
                            if (access(backupPath, F_OK) == 0) {
                                unlink(backupPath);
                            }

                            // Step 1: Rename original to backup (if original exists)
                            bool had_original = (access(fullName, F_OK) == 0);
                            if (had_original == false)
                                create_directories(backupPath);
                            if (had_original && s3_rename(fullName, backupPath) != 0) {
                                ESP_LOGE(TAG, "Failed to backup original file %s", fullName);
                            } else {
                                // Step 2: Move temp to original place
                                if (s3_rename(tempDownloadPath, fullName) == 0) {
                                    // Step 3: Success - remove backup
                                    if (had_original) s3_remove(backupPath);
                                    ESP_LOGI(TAG, "Successfully replaced %s (%d bytes)", fullName, download_size);
                                } else {
                                    // Step 4: Failed - restore original from backup
                                    if (had_original) s3_rename(backupPath, fullName);
                                    ESP_LOGE(TAG, "Failed to replace %s - original restored, downloaded file kept at %s", fullName, tempDownloadPath);
                                }
                            }
                        } else {
                            ESP_LOGE(TAG, "Downloaded file size mismatch: expected %d, got %d - kept at %s", fileSize->valueint, download_size, downloadPath);
                        }
                    } else {
                        ESP_LOGE(TAG, "Download failed for %s", fullName);
                    }
                }
            }
        }
    }
    if (gWiFi_SYNC_USER_INTERRUPT)
        goto _FAIL;


    cJSON_Delete(root);
    free(buf);
    buf = NULL;

    // Log filename-contentId mapping statistics
    ESP_LOGI(TAG, "Built filename-contentId mapping with %d entries", gFilenameContentIdMapCount);

    // ========== DIFFERENTIAL UPDATE CLEANUP AND FINAL STATISTICS ==========
    // Note: new_entries and old_entries are initialized to NULL and only assigned in !justPaserContent block
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmaybe-uninitialized"
    if (new_entries) {
        ESP_LOGI(TAG, "========== DIFFERENTIAL UPDATE FINAL RESULTS ==========");
        ESP_LOGI(TAG, "Differential update completed successfully");
        ESP_LOGI(TAG, "Bandwidth optimization: Unchanged files were skipped");
        ESP_LOGI(TAG, "======================================================");

        free_content_entries(new_entries, new_count);
        new_entries = NULL;
    }
    if (old_entries) {
        free_content_entries(old_entries, old_count);
        old_entries = NULL;
    }
#pragma GCC diagnostic pop

    // Log memory after cleanup
    if (!justPaserContent) {
        ESP_LOGI(TAG, "Post-cleanup memory: Free heap: %u bytes, Free SPIRAM: %u bytes",
            heap_caps_get_free_size(MALLOC_CAP_8BIT),
            heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    }

    return ESP_OK;

_FAIL:
    // Cleanup differential update resources on failure
    // Note: new_entries and old_entries are initialized to NULL and only assigned in !justPaserContent block
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmaybe-uninitialized"
    if (new_entries) {
        free_content_entries(new_entries, new_count);
        new_entries = NULL;
    }
    if (old_entries) {
        free_content_entries(old_entries, old_count);
        old_entries = NULL;
    }
#pragma GCC diagnostic pop

    cJSON_Delete(root);
    free(buf);
    buf = NULL;
    return ESP_FAIL;

}

void get_babyPacks(const s3_babyPack_t **babyPack, int *count) {
    *babyPack = gBabyPack;
    *count = gBabyPackCount;
}

void get_alarms(const s3_alarm_t **alarm, int *count) {
    *alarm = gAlarms;
    *count = gAlarmsCount;
}

void get_nfcs(const s3_nfc_t **nfc, int *count) {
    *nfc = gNfcs;
    *count = gNfcCount;
}

/**
 * @brief Check if device has at least one normal NFC
 * 
 * Normal NFCs: linked field is EMPTY ("")
 * Blankee NFCs: linked field has a UUID (e.g., "SKURC-045CA493C12A81")
 * 
 * Only normal NFCs enable the NFC menu.
 * 
 * @return HAS_NFC_ENABLED if at least one normal NFC exists, HAS_NFC_DISABLED otherwise
 */
int haveNFC(void){
    ESP_LOGI(TAG, "[haveNFC] Total NFC count: %d", gNfcCount);
    
    if (gNfcCount == 0) {
        ESP_LOGI(TAG, "[haveNFC] No NFCs at all - returning DISABLED");
        return HAS_NFC_DISABLED;
    }
    
    // Check if at least one NFC is a NORMAL NFC (empty linked field)
    for (int i = 0; i < gNfcCount; i++) {
        const char *linked = gNfcs[i].linked;
        // Normal NFC: linked field is NULL or empty string
        int is_normal = (linked == NULL || strlen(linked) == 0);
        ESP_LOGI(TAG, "[haveNFC] NFC[%d]: SN=%s, linked='%s', is_normal=%d", 
                 i, gNfcs[i].sn ? gNfcs[i].sn : "NULL", 
                 linked ? linked : "NULL", is_normal);
        
        if (is_normal) {
            ESP_LOGI(TAG, "[haveNFC] Found NORMAL NFC at index %d (empty linked) - ENABLED", i);
            return HAS_NFC_ENABLED;
        }
    }
    
    // Only blankee NFCs found (all have UUID in linked field)
    ESP_LOGI(TAG, "[haveNFC] Checked all %d NFCs - only BLANKEE NFCs found - DISABLED", gNfcCount);
    return HAS_NFC_DISABLED;
}

// FW content sync (skip mp3)

void read_resource_version_or_default(char *out_buf, size_t buf_size) {
    FILE *fp = s3_fopen("/sdcard/resource_ver.txt", "r");
    if (!fp) {
        strncpy(out_buf, "1.0.0", buf_size - 1);
        out_buf[buf_size - 1] = '\0';
        return;
    }

    if (!fgets(out_buf, buf_size, fp)) {
        strncpy(out_buf, "1.0.0", buf_size - 1);
        out_buf[buf_size - 1] = '\0';
        s3_fclose(fp);
        return;
    }

    out_buf[strcspn(out_buf, "\r\n")] = '\0';
    if (strlen(out_buf) == 0) {
        strncpy(out_buf, "1.0.0", buf_size - 1);
        out_buf[buf_size - 1] = '\0';
    }
    s3_fclose(fp);
}

void write_resource_version_to_file(char *version_str) {
    //ensure_dir_exists("/sdcard/tmp");
    FILE *fp = s3_fopen("/sdcard/resource_ver.txt", "w");
    if (fp == NULL) {
        printf("Failed to open file for writing\n");
        return;
    }

    fprintf(fp, "%s\n", version_str);  // 寫入版本
    s3_fclose(fp);
    printf("Version %s written to file.\n", version_str);
}

// Resource entry for differential updates
typedef struct {
    char *path;        // File path (e.g., "GraphicData/icon.jpg")
    char *url;         // Download URL
    int size;          // File size in bytes
} resource_entry_t;

// Parse resource.json into array of entries
static resource_entry_t* parse_resource_manifest(const char *json_path, int *out_count) {
    *out_count = 0;

    char *buf = read_file_to_spiram((char*)json_path);
    if (buf == NULL) {
        ESP_LOGW(TAG, "Failed to read resource manifest: %s", json_path);
        return NULL;
    }

    cJSON *root = cJSON_Parse(buf);
    if (root == NULL || !cJSON_IsArray(root)) {
        ESP_LOGE(TAG, "Failed to parse resource JSON: %s", json_path);
        free(buf);
        return NULL;
    }

    int count = cJSON_GetArraySize(root);
    resource_entry_t *entries = heap_caps_calloc(count, sizeof(resource_entry_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!entries) {
        ESP_LOGE(TAG, "Failed to allocate memory for resource entries");
        cJSON_Delete(root);
        free(buf);
        return NULL;
    }

    int entry_idx = 0;
    for (int i = 0; i < count; i++) {
        cJSON *item = cJSON_GetArrayItem(root, i);
        if (!cJSON_IsObject(item)) continue;

        cJSON *entry = item->child;
        if (!entry || !entry->string || !cJSON_IsObject(entry)) continue;

        cJSON *url = cJSON_GetObjectItem(entry, "url");
        cJSON *size = cJSON_GetObjectItem(entry, "size");

        if (cJSON_IsString(url) && cJSON_IsNumber(size)) {
            entries[entry_idx].path = strdup_spiram(entry->string);
            entries[entry_idx].url = strdup_spiram(url->valuestring);
            entries[entry_idx].size = size->valueint;
            entry_idx++;
        }
    }

    *out_count = entry_idx;
    cJSON_Delete(root);
    free(buf);

    ESP_LOGI(TAG, "Parsed %d resource entries from %s", entry_idx, json_path);
    return entries;
}

// Free resource entries array
static void free_resource_entries(resource_entry_t *entries, int count) {
    if (!entries) return;
    for (int i = 0; i < count; i++) {
        free(entries[i].path);
        free(entries[i].url);
    }
    free(entries);
}

// Compare new resource entry against old manifest
static resource_change_type_t compare_resource_entry(
    const resource_entry_t *new_entry,
    const resource_entry_t *old_entries,
    int old_count
) {
    if (!old_entries || old_count == 0) {
        return RESOURCE_NEW; // No old manifest, everything is new
    }

    // Search for matching path in old manifest
    for (int i = 0; i < old_count; i++) {
        if (strcmp(new_entry->path, old_entries[i].path) == 0) {
            // Path matches - check if content changed
            if (new_entry->size == old_entries[i].size &&
                strcmp(new_entry->url, old_entries[i].url) == 0) {
                return RESOURCE_UNCHANGED;
            }
            return RESOURCE_MODIFIED;
        }
    }
    return RESOURCE_NEW; // Not found in old manifest
}

// ========== CONTENT MANIFEST HELPER FUNCTIONS (for account_file.json differential updates) ==========

// Parse account_file.json into array of content entries
static content_entry_t* parse_account_contents_manifest(const char *json_path, int *out_count) {
    *out_count = 0;

    char *buf = read_file_to_spiram((char*)json_path);
    if (buf == NULL) {
        ESP_LOGW(TAG, "Failed to read content manifest: %s", json_path);
        return NULL;
    }

    cJSON *root = cJSON_Parse(buf);
    if (root == NULL) {
        ESP_LOGE(TAG, "Failed to parse content manifest JSON: %s", json_path);
        free(buf);
        return NULL;
    }

    const cJSON *result = cJSON_GetObjectItem(root, "result");
    if (!cJSON_IsObject(result)) {
        ESP_LOGE(TAG, "No 'result' object in content manifest");
        cJSON_Delete(root);
        free(buf);
        return NULL;
    }

    // Pre-count total entries across all three loops
    int total_count = 0;

    // Count babyPacks contents
    const cJSON *babyPacks = cJSON_GetObjectItem(result, "babyPacks");
    if (cJSON_IsArray(babyPacks)) {
        int pack_count = cJSON_GetArraySize(babyPacks);
        for (int i = 0; i < pack_count; i++) {
            cJSON *pack = cJSON_GetArrayItem(babyPacks, i);
            if (cJSON_IsObject(pack)) {
                cJSON *contents = cJSON_GetObjectItem(pack, "contents");
                if (cJSON_IsArray(contents)) {
                    total_count += cJSON_GetArraySize(contents);
                }
            }
        }
    }

    // Count alarms
    const cJSON *alarms = cJSON_GetObjectItem(result, "alarms");
    if (cJSON_IsArray(alarms)) {
        total_count += cJSON_GetArraySize(alarms);
    }

    // Count NFCs contents
    const cJSON *NFCs = cJSON_GetObjectItem(result, "NFCs");
    if (cJSON_IsArray(NFCs)) {
        int nfc_count = cJSON_GetArraySize(NFCs);
        for (int i = 0; i < nfc_count; i++) {
            cJSON *nfc = cJSON_GetArrayItem(NFCs, i);
            if (cJSON_IsObject(nfc)) {
                cJSON *skus = cJSON_GetObjectItem(nfc, "skus");
                if (cJSON_IsArray(skus)) {
                    int sku_count = cJSON_GetArraySize(skus);
                    for (int j = 0; j < sku_count; j++) {
                        cJSON *sku = cJSON_GetArrayItem(skus, j);
                        if (cJSON_IsObject(sku)) {
                            cJSON *contents = cJSON_GetObjectItem(sku, "contents");
                            if (cJSON_IsArray(contents)) {
                                total_count += cJSON_GetArraySize(contents);
                            }
                        }
                    }
                }
            }
        }
    }

    if (total_count == 0) {
        ESP_LOGW(TAG, "No content entries found in manifest");
        cJSON_Delete(root);
        free(buf);
        return NULL;
    }

    // Allocate array for all entries
    content_entry_t *entries = heap_caps_calloc(total_count, sizeof(content_entry_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!entries) {
        ESP_LOGE(TAG, "Failed to allocate memory for content entries");
        cJSON_Delete(root);
        free(buf);
        return NULL;
    }

    int entry_idx = 0;

    // Parse babyPacks contents
    if (cJSON_IsArray(babyPacks)) {
        int pack_count = cJSON_GetArraySize(babyPacks);
        for (int i = 0; i < pack_count; i++) {
            cJSON *pack = cJSON_GetArrayItem(babyPacks, i);
            if (!cJSON_IsObject(pack)) continue;

            cJSON *skuId = cJSON_GetObjectItem(pack, "skuId");
            cJSON *contents = cJSON_GetObjectItem(pack, "contents");
            if (!cJSON_IsArray(contents) || !cJSON_IsString(skuId)) continue;

            int content_count = cJSON_GetArraySize(contents);
            for (int j = 0; j < content_count; j++) {
                cJSON *item = cJSON_GetArrayItem(contents, j);
                if (!cJSON_IsObject(item)) continue;

                cJSON *url = cJSON_GetObjectItem(item, "url");
                cJSON *fileSize = cJSON_GetObjectItem(item, "fileSize");
                cJSON *filename = cJSON_GetObjectItem(item, "filename");

                if (cJSON_IsString(filename) && cJSON_IsString(url) && cJSON_IsNumber(fileSize)) {
                    char fullPath[256];
                    snprintf(fullPath, sizeof(fullPath), SDCARD_CONTENT_FULLNAME,
                            skuId->valuestring, filename->valuestring);

                    entries[entry_idx].fullPath = strdup_spiram(fullPath);
                    entries[entry_idx].url = strdup_spiram(url->valuestring);
                    entries[entry_idx].size = fileSize->valueint;
                    entries[entry_idx].contentType = strdup_spiram("babypack");
                    entry_idx++;
                }
            }
        }
    }

    // Parse alarms
    if (cJSON_IsArray(alarms)) {
        int alarm_count = cJSON_GetArraySize(alarms);
        for (int i = 0; i < alarm_count; i++) {
            cJSON *alarm = cJSON_GetArrayItem(alarms, i);
            if (!cJSON_IsObject(alarm)) continue;

            cJSON *audio = cJSON_GetObjectItem(alarm, "audio");
            cJSON *filename = cJSON_GetObjectItem(alarm, "filename");
            cJSON *fileSize = cJSON_GetObjectItem(alarm, "fileSize");

            if (cJSON_IsString(audio) && cJSON_IsString(filename) && cJSON_IsNumber(fileSize)) {
                char fullPath[256];
                snprintf(fullPath, sizeof(fullPath), SDCARD_CONTENT_FULLNAME,
                        "alarms", filename->valuestring);

                entries[entry_idx].fullPath = strdup_spiram(fullPath);
                entries[entry_idx].url = strdup_spiram(audio->valuestring);
                entries[entry_idx].size = fileSize->valueint;
                entries[entry_idx].contentType = strdup_spiram("alarm");
                entry_idx++;
            }
        }
    }

    // Parse NFCs contents
    if (cJSON_IsArray(NFCs)) {
        int nfc_count = cJSON_GetArraySize(NFCs);
        for (int i = 0; i < nfc_count; i++) {
            cJSON *nfc = cJSON_GetArrayItem(NFCs, i);
            if (!cJSON_IsObject(nfc)) continue;

            cJSON *skus = cJSON_GetObjectItem(nfc, "skus");
            if (!cJSON_IsArray(skus)) continue;

            int sku_count = cJSON_GetArraySize(skus);
            for (int j = 0; j < sku_count; j++) {
                cJSON *sku = cJSON_GetArrayItem(skus, j);
                if (!cJSON_IsObject(sku)) continue;

                cJSON *skuId = cJSON_GetObjectItem(sku, "skuId");
                cJSON *contents = cJSON_GetObjectItem(sku, "contents");
                if (!cJSON_IsArray(contents) || !cJSON_IsString(skuId)) continue;

                int content_count = cJSON_GetArraySize(contents);
                for (int k = 0; k < content_count; k++) {
                    cJSON *content = cJSON_GetArrayItem(contents, k);
                    if (!cJSON_IsObject(content)) continue;

                    cJSON *url = cJSON_GetObjectItem(content, "url");
                    cJSON *fileSize = cJSON_GetObjectItem(content, "fileSize");
                    cJSON *filename = cJSON_GetObjectItem(content, "filename");

                    if (cJSON_IsString(url) && cJSON_IsString(filename) && cJSON_IsNumber(fileSize)) {
                        char fullPath[256];
                        snprintf(fullPath, sizeof(fullPath), SDCARD_CONTENT_FULLNAME,
                                skuId->valuestring, filename->valuestring);

                        entries[entry_idx].fullPath = strdup_spiram(fullPath);
                        entries[entry_idx].url = strdup_spiram(url->valuestring);
                        entries[entry_idx].size = fileSize->valueint;
                        entries[entry_idx].contentType = strdup_spiram("nfc");
                        entry_idx++;
                    }
                }
            }
        }
    }

    *out_count = entry_idx;
    cJSON_Delete(root);
    free(buf);

    ESP_LOGI(TAG, "Parsed %d content entries from %s", entry_idx, json_path);
    return entries;
}

// Free content entries array
static void free_content_entries(content_entry_t *entries, int count) {
    if (!entries) return;
    for (int i = 0; i < count; i++) {
        free(entries[i].fullPath);
        free(entries[i].url);
        free(entries[i].contentType);
    }
    free(entries);
}

// Compare new content entry against old manifest
static resource_change_type_t compare_content_entry(
    const content_entry_t *new_entry,
    const content_entry_t *old_entries,
    int old_count
) {
    if (!old_entries || old_count == 0) {
        return RESOURCE_NEW; // No old manifest, everything is new
    }

    // Search for matching fullPath in old manifest
    for (int i = 0; i < old_count; i++) {
        if (strcmp(new_entry->fullPath, old_entries[i].fullPath) == 0) {
            // Path matches - check if content changed (size only, URLs can change with tokens)
            if (new_entry->size == old_entries[i].size) {
                return RESOURCE_UNCHANGED;
            }
            return RESOURCE_MODIFIED;
        }
    }
    return RESOURCE_NEW; // Not found in old manifest
}

esp_err_t sync_resource_without_mp3(char *url,int cnt) {
    esp_err_t ret = ESP_FAIL;
    if (cnt == 0) {
        ret = content_download(url, "/sdcard/resource.json");
        if (ret == ESP_OK) {
            // cei_download_file leaves file at .tmp location, move it to final location with backup strategy
            const char *finalPath = "/sdcard/resource.json";
            const char *tempPath = "/sdcard/resource.json.tmp";
            const char *backupPath = "/sdcard/resource.json.bak";

            // Step 1: Rename original to backup (if exists)
            bool had_original = (access(finalPath, F_OK) == 0);
            if (had_original && s3_rename(finalPath, backupPath) != 0) {
                ESP_LOGE(TAG, "Failed to backup resource.json");
                ret = ESP_FAIL;
            } else {
                // Step 2: Move temp to final place
                if (s3_rename(tempPath, finalPath) == 0) {
                    // Step 3: KEEP backup for differential comparison (DO NOT remove)
                    ESP_LOGI(TAG, "Successfully moved resource.json to final location, backup preserved for differential update");
                } else {
                    // Step 4: Failed - restore original from backup
                    if (had_original) s3_rename(backupPath, finalPath);
                    ESP_LOGE(TAG, "Failed to move resource.json - original restored");
                    ret = ESP_FAIL;
                }
            }
        }
    }

    // ========== DIFFERENTIAL UPDATE IMPLEMENTATION START ==========

    // Log memory status before parsing to diagnose issues
    ESP_LOGI(TAG, "Pre-parse memory: Free heap: %u bytes, Free SPIRAM: %u bytes",
        heap_caps_get_free_size(MALLOC_CAP_8BIT),
        heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    ESP_LOGI(TAG, "Pre-parse DRAM: Free internal: %u bytes, Free DMA: %u bytes, Largest DMA block: %u bytes",
        heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
        heap_caps_get_free_size(MALLOC_CAP_DMA),
        heap_caps_get_largest_free_block(MALLOC_CAP_DMA));

    // Step 2.1: Parse NEW manifest FIRST (critical - must succeed)
    resource_entry_t *new_entries = NULL;
    int new_count = 0;
    new_entries = parse_resource_manifest("/sdcard/resource.json", &new_count);
    if (!new_entries) {
        ESP_LOGE(TAG, "Failed to parse new manifest - aborting differential update");
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "Parsed %d entries from new manifest", new_count);

    // Log memory after parsing new manifest
    ESP_LOGI(TAG, "Post-new-manifest memory: Free heap: %u bytes, Free SPIRAM: %u bytes",
        heap_caps_get_free_size(MALLOC_CAP_8BIT),
        heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    ESP_LOGI(TAG, "Post-new-manifest DRAM: Free internal: %u bytes, Free DMA: %u bytes, Largest DMA block: %u bytes",
        heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
        heap_caps_get_free_size(MALLOC_CAP_DMA),
        heap_caps_get_largest_free_block(MALLOC_CAP_DMA));

    // Step 2.2: Check for and parse OLD manifest (optional - for differential comparison)
    resource_entry_t *old_entries = NULL;
    int old_count = 0;
    bool has_old_manifest = (access("/sdcard/resource.json.bak", F_OK) == 0);

    if (has_old_manifest) {
        ESP_LOGI(TAG, "Found old manifest backup, parsing for differential update");
        old_entries = parse_resource_manifest("/sdcard/resource.json.bak", &old_count);
        if (old_entries) {
            ESP_LOGI(TAG, "Parsed %d entries from old manifest", old_count);
        } else {
            ESP_LOGW(TAG, "Failed to parse old manifest, will download all files");
            has_old_manifest = false;  // Disable differential mode if parse fails
        }
    } else {
        ESP_LOGI(TAG, "No old manifest backup found, will download all files");
    }

    // Step 2.3: Compare manifests and count changes
    int unchanged_count = 0;
    int modified_count = 0;
    int new_file_count = 0;
    int skipped_by_filter = 0;

    for (int i = 0; i < new_count; i++) {
        resource_entry_t *entry = &new_entries[i];

        // Apply filters (skip sound/, animation/, and .mp3 files)
        const char *ext = strrchr(entry->path, '.');
        if (strncmp(entry->path, "sound/", strlen("sound/")) == 0) {
            skipped_by_filter++;
            continue;
        } else if (strncmp(entry->path, "animation/", strlen("animation/")) == 0 ||
                   (ext && strcasecmp(ext, ".mp3") == 0)) {
            skipped_by_filter++;
            continue;
        }

        // Compare with old manifest
        resource_change_type_t change_type = compare_resource_entry(entry, old_entries, old_count);

        switch (change_type) {
            case RESOURCE_UNCHANGED:
                unchanged_count++;
                break;
            case RESOURCE_MODIFIED:
                modified_count++;
                break;
            case RESOURCE_NEW:
                new_file_count++;
                break;
        }
    }

    ESP_LOGI(TAG, "========== DIFFERENTIAL UPDATE SUMMARY ==========");
    ESP_LOGI(TAG, "Total files in new manifest: %d", new_count);
    ESP_LOGI(TAG, "Skipped by filter: %d", skipped_by_filter);
    ESP_LOGI(TAG, "Unchanged files: %d (will skip if exists)", unchanged_count);
    ESP_LOGI(TAG, "Modified files: %d (will re-download)", modified_count);
    ESP_LOGI(TAG, "New files: %d (will download)", new_file_count);
    ESP_LOGI(TAG, "Expected downloads: %d", modified_count + new_file_count);
    ESP_LOGI(TAG, "===============================================");

    // Free old manifest NOW - we only need it for comparison, not during download
    if (old_entries) {
        free_resource_entries(old_entries, old_count);
        old_entries = NULL;  // Mark as freed
        old_count = 0;
        ESP_LOGI(TAG, "Freed old manifest to reclaim memory before downloads");

        // Log memory after freeing old manifest
        ESP_LOGI(TAG, "Post-cleanup memory: Free heap: %u bytes, Free SPIRAM: %u bytes",
            heap_caps_get_free_size(MALLOC_CAP_8BIT),
            heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
        ESP_LOGI(TAG, "Post-cleanup DRAM: Free internal: %u bytes, Free DMA: %u bytes, Largest DMA block: %u bytes",
            heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
            heap_caps_get_free_size(MALLOC_CAP_DMA),
            heap_caps_get_largest_free_block(MALLOC_CAP_DMA));
    }

    // ========== DIFFERENTIAL UPDATE IMPLEMENTATION END ==========

    char *buf = read_file_to_spiram("/sdcard/resource.json");
    ret = ESP_FAIL;
    if (buf == NULL) {
        free_resource_entries(new_entries, new_count);
        if (old_entries) free_resource_entries(old_entries, old_count);
        return ret;
    }

    cJSON *root = cJSON_Parse(buf);
    if (root == NULL || !cJSON_IsArray(root)) {
        ESP_LOGE(TAG, "Failed to parse JSON");
        free(buf);
        free_resource_entries(new_entries, new_count);
        if (old_entries) free_resource_entries(old_entries, old_count);
        return ret;
    }
    int count = cJSON_GetArraySize(root);
    ESP_LOGI(TAG, "count is %d", count);
    int tmp_count = 0;
    int _count = 0;
    int actually_downloaded = 0;  // Track files actually downloaded
    int skipped_unchanged = 0;     // Track files skipped due to being unchanged

    for (int i = 0; i < count; i++) {
        cJSON *item = cJSON_GetArrayItem(root, i);
        if (!cJSON_IsObject(item))
            continue;

        cJSON *entry = item->child;
        if (!entry || !entry->string || !cJSON_IsObject(entry))
            continue;

        const char *ext = strrchr(entry->string, '.');
        if (strncmp(entry->string, "sound/", strlen("sound/")) == 0) {

        } else if ( strncmp(entry->string, "animation/", strlen("animation/")) == 0 ||
            (ext && strcasecmp(ext, ".mp3") == 0 )) {
            continue;
        }
        _count++;

        cJSON *url = cJSON_GetObjectItem(entry, "url");
        cJSON *r_size = cJSON_GetObjectItem(entry, "size");
        char fullPath[128];
        sprintf(fullPath, "/sdcard/%s", entry->string);

        if (cJSON_IsString(url) && cJSON_IsNumber(r_size)) {
            // ========== STEP 2.4: DIFFERENTIAL UPDATE LOGIC ==========
            // Find this file in the new_entries array to determine change type
            resource_change_type_t change_type = RESOURCE_NEW;
            resource_entry_t *matching_entry = NULL;

            for (int j = 0; j < new_count; j++) {
                if (strcmp(new_entries[j].path, entry->string) == 0) {
                    matching_entry = &new_entries[j];
                    change_type = compare_resource_entry(matching_entry, old_entries, old_count);
                    break;
                }
            }

            int attempt = 1;
            int size = get_file_size(fullPath);

            // Check if file is unchanged and exists with correct size
            if (change_type == RESOURCE_UNCHANGED && size >= 0 && size == r_size->valueint) {
                ESP_LOGI(TAG, "[GraphicData UNCHANGED - skipped]: %s (size: %d bytes)", fullPath, size);
                skipped_unchanged++;
                tmp_count++;
                continue;  // Skip downloading this file
            }

            // File needs to be downloaded (either MODIFIED or NEW, or UNCHANGED but missing/corrupted)
            if (change_type == RESOURCE_MODIFIED) {
                ESP_LOGI(TAG, "[GraphicData MODIFIED - download]: %s", fullPath);
            } else if (change_type == RESOURCE_NEW) {
                ESP_LOGI(TAG, "[GraphicData NEW - download]: %s", fullPath);
            }

            ESP_LOGI(TAG, "[GraphicData n%d - start]: %s", attempt, fullPath);

            if (size >= 0 && size == r_size->valueint) {
                ESP_LOGI(TAG, "[GraphicData n%d - success]: %s", attempt, fullPath);
                tmp_count++;
            } else {
                ESP_LOGW(TAG, "[GraphicData n%d - fail]: %s, received:%d, expected:%d", attempt, fullPath, size, r_size->valueint);
                attempt++;

                // Download to tmp/downloads instead of overwriting original
                char downloadPath[300];
                const char* filename_only = strrchr(fullPath, '/');
                if (filename_only) filename_only++; else filename_only = fullPath;
                snprintf(downloadPath, sizeof(downloadPath), "/sdcard/tmp/downloads/%s", filename_only);
                create_directories(downloadPath);


                ESP_LOGI(TAG, "[GraphicData n%d - start]: %s", attempt, downloadPath);
                esp_err_t result = content_download(url->valuestring, downloadPath);
                if (result == ESP_OK) {
                    // content_download leaves file at downloadPath.tmp, check that file
                    char tempDownloadPath[320];
                    snprintf(tempDownloadPath, sizeof(tempDownloadPath), "%s.tmp", downloadPath);
                    int download_size = get_file_size(tempDownloadPath);
                    if (download_size == r_size->valueint) {
                        // Download successful and size matches - replace original atomically with backup strategy
                        // Ensure destination directory exists before replacement
                        create_directories(fullPath);
                        
                        char backupPath[256];
                        snprintf(backupPath, sizeof(backupPath), "%s.bak", fullPath);
                        if (access(backupPath, F_OK) == 0) {
                            unlink(backupPath);
                        }
                        
                        // Step 1: Rename original to backup (if original exists)
                        bool had_original = (access(fullPath, F_OK) == 0);
                        if (had_original == false)
                            create_directories(backupPath);
                        if (had_original && s3_rename(fullPath, backupPath) != 0) {
                            ESP_LOGW(TAG, "[GraphicData n%d - fail]: backup failed", attempt);
                        } else {
                            // Step 2: Move temp to original place
                            if (s3_rename(tempDownloadPath, fullPath) == 0) {
                                // Step 3: Success - remove backup
                                if (had_original) s3_remove(backupPath);
                                ESP_LOGI(TAG, "[GraphicData n%d - success]: %s", attempt, fullPath);
                                tmp_count++; // Count this as success
                                actually_downloaded++; // Track files actually downloaded
                                // Step 4: Invalidate image cache for updated file
                                invalidate_image_cache(fullPath);
                            } else {
                                // Step 4: Failed - restore original from backup
                                if (had_original) s3_rename(backupPath, fullPath);
                                ESP_LOGW(TAG, "[GraphicData n%d - fail]: move failed", attempt);
                            }
                        }
                    } else {
                        ESP_LOGW(TAG, "[GraphicData n%d - fail]: size mismatch, expected:%d, got:%d", attempt, r_size->valueint, download_size);
                    }
                } else {
                    ESP_LOGW(TAG, "[GraphicData n%d - fail]: download failed", attempt);
                }
            }
        }
    }

    // ========== STEP 3: FINAL STATISTICS AND CLEANUP ==========

    ESP_LOGI(TAG, "[ fileNum ] %d , %d", tmp_count, _count);

    // Enhanced differential update statistics
    ESP_LOGI(TAG, "========== DIFFERENTIAL UPDATE RESULTS ==========");
    ESP_LOGI(TAG, "Files skipped (unchanged): %d", skipped_unchanged);
    ESP_LOGI(TAG, "Files downloaded (new/modified): %d", actually_downloaded);
    ESP_LOGI(TAG, "Total bandwidth saved: ~%d files not re-downloaded", skipped_unchanged);
    ESP_LOGI(TAG, "===============================================");

    // Cleanup: Free manifest entry arrays
    free_resource_entries(new_entries, new_count);
    if (old_entries) free_resource_entries(old_entries, old_count);

    cJSON_Delete(root);
    free(buf);

    esp_err_t final_result = (tmp_count == _count) ? ESP_OK : ESP_FAIL;

    // Step 3: Remove backup manifest after successful sync
    if (final_result == ESP_OK && has_old_manifest) {
        if (s3_remove("/sdcard/resource.json.bak") == 0) {
            ESP_LOGI(TAG, "Successfully removed resource.json.bak after completion");
        } else {
            ESP_LOGW(TAG, "Failed to remove resource.json.bak (not critical)");
        }
    } else if (final_result != ESP_OK) {
        ESP_LOGI(TAG, "Sync incomplete, keeping resource.json.bak for next attempt");
    }

    return final_result;
}

// Public function to cleanup connection reuse resources
void cleanup_sync_connection_reuse(void) {
    cleanup_reusable_http_client();
    ESP_LOGI(TAG, "Connection reuse resources cleaned up");
}

// PURE NETWORK SPEED TEST - Downloads data and throws it away to measure raw performance
esp_err_t test_pure_download_speed(char *url, int test_duration_seconds) {
    ESP_LOGI(TAG, "STARTING PURE NETWORK SPEED TEST");
    ESP_LOGI(TAG, "URL: %s", url);
    ESP_LOGI(TAG, "Test Duration: %d seconds", test_duration_seconds);
    ESP_LOGI(TAG, "NOTE: Data will be downloaded but NOT STORED (pure speed test)");
    
    // Memory status before test
    ESP_LOGI(TAG, "Pre-test memory: Free heap: %u bytes, Free SPIRAM: %u bytes", 
        heap_caps_get_free_size(MALLOC_CAP_8BIT), 
        heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    
    // Optimized HTTP client for pure speed testing
    esp_http_client_config_t config = {
        .url = url,
        .buffer_size = 64*1024,        // Large buffer for speed
        .timeout_ms = 10000,
        .crt_bundle_attach = esp_crt_bundle_attach,  // Disabled for max speed
        .keep_alive_enable = true,     
        .skip_cert_common_name_check = true,
        .buffer_size_tx = 8*1024,      
        .disable_auto_redirect = false,
        .max_redirection_count = 2,
        .user_agent = "ESP32-SpeedTest/1.0",
        .method = HTTP_METHOD_GET,
    };
    
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == NULL) {
        ESP_LOGE(TAG, "Failed to init speed test HTTP client");
        return ESP_FAIL;
    }
    
    // Set headers for maximum speed
    esp_http_client_set_header(client, "Connection", "keep-alive");
    esp_http_client_set_header(client, "Accept-Encoding", "identity"); // No compression
    esp_http_client_set_header(client, "Accept", "*/*");
    
    esp_err_t err = esp_http_client_open(client, 0);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open speed test connection: %s", esp_err_to_name(err));
        esp_http_client_cleanup(client);
        return err;
    }
    
    int content_length = esp_http_client_fetch_headers(client);
    ESP_LOGI(TAG, "Content length: %d bytes (%.2f MB)", 
        content_length, content_length / (1024.0f * 1024.0f));
    
    // Allocate buffer for reading (data will be discarded)
    const int BUFFER_SIZE = 128*1024; // 128KB buffer
    char *buffer = heap_caps_malloc(BUFFER_SIZE, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!buffer) {
        ESP_LOGW(TAG, "Failed to allocate 128KB, trying 64KB");
        buffer = heap_caps_malloc(64*1024, MALLOC_CAP_8BIT);
        if (!buffer) {
            ESP_LOGE(TAG, "Failed to allocate buffer for speed test");
            esp_http_client_cleanup(client);
            return ESP_FAIL;
        }
    }
    
    int buffer_size = buffer ? BUFFER_SIZE : 64*1024;
    ESP_LOGI(TAG, "Using %dKB buffer for speed test (data will be DISCARDED)", buffer_size/1024);
    
    // Speed test variables
    int total_downloaded = 0;
    int64_t start_us = esp_timer_get_time();
    int64_t last_log_us = start_us;
    int64_t test_end_us = start_us + (test_duration_seconds * 1000000LL);
    int read_len_this_second = 0;
    int max_speed_kb = 0;
    int min_speed_kb = INT_MAX;
    int speed_samples = 0;
    long long total_speed_kb = 0;
    bool test_completed = false;
    
    ESP_LOGI(TAG, "\n========== STARTING SPEED TEST ==========\n");
    
    while (!gWiFi_SYNC_USER_INTERRUPT && esp_timer_get_time() < test_end_us) {
        int read_len = esp_http_client_read(client, buffer, buffer_size);
        
        if (read_len <= 0) {
            if (read_len < 0) {
                ESP_LOGW(TAG, "HTTP read error: %d", read_len);
            } else {
                ESP_LOGI(TAG, "End of content reached, restarting download...");
                // Restart download to continue speed test
                esp_http_client_close(client);
                esp_http_client_open(client, 0);
                esp_http_client_fetch_headers(client);
                continue;
            }
            break;
        }
        
        // DATA IS INTENTIONALLY DISCARDED HERE - NO FILE WRITE!
        // This isolates pure network performance
        
        total_downloaded += read_len;
        read_len_this_second += read_len;
        
        // Log every second with detailed statistics
        int64_t now_us = esp_timer_get_time();
        if (now_us - last_log_us >= 1000000) {
            float current_kb_per_sec = read_len_this_second / 1024.0f;
            float elapsed_seconds = (now_us - start_us) / 1000000.0f;
            float avg_speed = (total_downloaded / 1024.0f) / elapsed_seconds;
            float remaining_seconds = (test_end_us - now_us) / 1000000.0f;
            
            // Track speed statistics
            if (current_kb_per_sec > max_speed_kb) {
                max_speed_kb = (int)current_kb_per_sec;
            }
            if (current_kb_per_sec < min_speed_kb && current_kb_per_sec > 0) {
                min_speed_kb = (int)current_kb_per_sec;
            }
            speed_samples++;
            total_speed_kb += (int)current_kb_per_sec;
            
            ESP_LOGI(TAG, "%.1f KB/s | Avg: %.1f KB/s | Downloaded: %.1f MB | Remaining: %.0fs",
                current_kb_per_sec, avg_speed, 
                total_downloaded/(1024.0f*1024.0f), remaining_seconds);
            
            last_log_us = now_us;
            read_len_this_second = 0;
        }
        vTaskDelay(10 / portTICK_PERIOD_MS);
    }
    
    test_completed = !gWiFi_SYNC_USER_INTERRUPT;
    
    // Cleanup
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    free(buffer);
    
    // Final results
    int64_t total_time_us = esp_timer_get_time() - start_us;
    float total_seconds = total_time_us / 1000000.0f;
    float final_avg_speed = (total_downloaded / 1024.0f) / total_seconds;
    float mb_downloaded = total_downloaded / (1024.0f * 1024.0f);
    int avg_speed_from_samples = speed_samples > 0 ? total_speed_kb / speed_samples : 0;
    
    ESP_LOGI(TAG, "\n========== PURE SPEED TEST RESULTS ==========\n");
    
    if (test_completed) {
        ESP_LOGI(TAG, "TEST COMPLETED SUCCESSFULLY");
    } else {
        ESP_LOGI(TAG, "TEST INTERRUPTED");
    }
    
    ESP_LOGI(TAG, "PERFORMANCE METRICS:");
    ESP_LOGI(TAG, "   Average Speed: %.1f KB/s", final_avg_speed);
    ESP_LOGI(TAG, "   Peak Speed: %d KB/s", max_speed_kb);
    ESP_LOGI(TAG, "   Minimum Speed: %d KB/s", min_speed_kb == INT_MAX ? 0 : min_speed_kb);
    ESP_LOGI(TAG, "   Sample Avg: %d KB/s (%d samples)", avg_speed_from_samples, speed_samples);
    ESP_LOGI(TAG, "   Total Downloaded: %.2f MB", mb_downloaded);
    ESP_LOGI(TAG, "   ❱︝  Duration: %.2f seconds", total_seconds);
    ESP_LOGI(TAG, "   Target (Taiwan): 100-150 KB/s");
    
    if (final_avg_speed >= 100.0f) {
        ESP_LOGI(TAG, "   EXCELLENT: Speed meets Taiwan performance!");
    } else if (final_avg_speed >= 50.0f) {
        ESP_LOGI(TAG, "   GOOD: Decent speed, but below Taiwan target");
    } else {
        ESP_LOGI(TAG, "   POOR: Speed significantly below expectations");
    }
    
    ESP_LOGI(TAG, "\n===========================================\n");

    return test_completed ? ESP_OK : ESP_FAIL;
}

/**
 * @brief Invalidate image cache for a downloaded file
 * @param path File path to invalidate cache for
 *
 * This function checks the file extension and calls the appropriate
 * cache invalidation function (JPEG or PNG).
 */
static void invalidate_image_cache(const char *path) {
    if (!path) return;

    const char *ext = strrchr(path, '.');
    if (ext) {
        if (strcasecmp(ext, ".jpg") == 0 || strcasecmp(ext, ".jpeg") == 0) {
            jpeg_cache_invalidate(path);
            ESP_LOGI(TAG, "Invalidated JPEG cache for: %s", path);
        } else if (strcasecmp(ext, ".png") == 0) {
            png_cache_invalidate(path);
            ESP_LOGI(TAG, "Invalidated PNG cache for: %s", path);
        }
    }
}
