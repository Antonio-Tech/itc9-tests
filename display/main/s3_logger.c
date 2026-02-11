#include "s3_logger.h"

#include <stdio.h>
#include <string.h>
#include <stdbool.h>
#include <sys/stat.h>
#include <dirent.h>

#if USE_S3_LOGGER
#include <stdarg.h>
#include <sys/time.h>
#include <time.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_system.h"

static const char *TAG = "S3_LOGGER";
static TaskHandle_t logger_task_handle = NULL;
static bool logger_initialized = false;
static volatile uint32_t vprintf_call_count = 0;
static const char *log_file_path = "/sdcard/esp32.log";

// Circular buffer configuration
#define LOG_BUFFER_SIZE (16 * 1024)  // 16KB circular buffer
#define FLUSH_WHEN_FULL 1            // Only flush when buffer is full

static char log_buffer[LOG_BUFFER_SIZE];
static volatile size_t write_pos = 0;
static volatile size_t read_pos = 0;
static SemaphoreHandle_t buffer_mutex = NULL;
static volatile bool flush_requested = false;

// Helper function to get available space in circular buffer
static size_t get_available_space(void) {
    size_t w = write_pos;
    size_t r = read_pos;
    if (w >= r) {
        return LOG_BUFFER_SIZE - (w - r) - 1; // Keep 1 byte gap to distinguish full from empty
    } else {
        return r - w - 1;
    }
}

// Helper function to get used space in circular buffer
static size_t get_used_space(void) {
    size_t w = write_pos;
    size_t r = read_pos;
    if (w >= r) {
        return w - r;
    } else {
        return LOG_BUFFER_SIZE - (r - w);
    }
}

// Fast, non-blocking function to add data to circular buffer
static void add_to_buffer(const char *data, size_t len) {
    if (!buffer_mutex || !data || len == 0) return;
    
    // Take mutex with short timeout to avoid blocking calling task
    if (xSemaphoreTake(buffer_mutex, pdMS_TO_TICKS(5)) != pdTRUE) {
        return; // Skip this log if we can't get mutex quickly
    }
    
    size_t available = get_available_space();
    if (len > available) {
        // If not enough space, advance read pointer to make room (drop old data)
        size_t need_to_drop = len - available;
        read_pos = (read_pos + need_to_drop) % LOG_BUFFER_SIZE;
    }
    
    // Add data to circular buffer
    size_t w = write_pos;
    for (size_t i = 0; i < len; i++) {
        log_buffer[w] = data[i];
        w = (w + 1) % LOG_BUFFER_SIZE;
    }
    write_pos = w;
    
    // Request flush ONLY when buffer is completely full
    if (get_available_space() == 0) {
        flush_requested = true;
    }
    
    xSemaphoreGive(buffer_mutex);
}

// Async logging vprintf hook - runs in caller's task context
int s3_log_vprintf(const char *fmt, va_list args) {
    // Increment call counter to track hook usage
    vprintf_call_count++;
    
    // Always print to UART first (immediate feedback)
    int ret = vprintf(fmt, args);
    
    // If logger not ready, just return
    if (!logger_initialized || !buffer_mutex) {
        return ret;
    }
    
    // Format the log message into a temporary buffer
    // Use a very small buffer to minimize stack usage
    char temp_buffer[128]; // Reduced to 128 bytes to minimize stack impact
    
    // Get timestamp
    struct timeval tv;
    gettimeofday(&tv, NULL);
    
    // Format timestamp + message (reuse va_list by creating temp copy)
    va_list args_copy;
    va_copy(args_copy, args);
    
    int timestamp_len = snprintf(temp_buffer, sizeof(temp_buffer), "[%ld.%03ld] ", 
                                tv.tv_sec, tv.tv_usec / 1000);
    
    if (timestamp_len > 0 && timestamp_len < sizeof(temp_buffer)) {
        int msg_len = vsnprintf(temp_buffer + timestamp_len, 
                               sizeof(temp_buffer) - timestamp_len, fmt, args_copy);
        
        if (msg_len > 0) {
            int total_len = timestamp_len + msg_len;
            if (total_len < sizeof(temp_buffer)) {
                // Add to circular buffer (non-blocking)
                add_to_buffer(temp_buffer, total_len);
            }
        }
    }
    
    va_end(args_copy);
    return ret;
}

// Logger task - handles actual file I/O ONLY when buffer is full
static void logger_task(void *pvParameters) {
    printf("[S3_LOGGER] Logger task started! Will only write when buffer is full.\n");
    
    while (1) {
        // Check if we should flush (ONLY when buffer is full or manually requested)
        if (xSemaphoreTake(buffer_mutex, pdMS_TO_TICKS(500)) == pdTRUE) {
            size_t used = get_used_space();
            
            // Only flush when explicitly requested (buffer full) 
            if (flush_requested && used > 0) {
                printf("[S3_LOGGER] Buffer full! Writing %u bytes to SD card\n", (unsigned int)used);
                
                // Open file, write all buffered data, close file
                FILE *log_file = fopen(log_file_path, "a");
                if (!log_file) {
                    printf("[S3_LOGGER] ERROR: Failed to open log file: %s\n", log_file_path);
                } else {
                    size_t r = read_pos;
                    size_t to_write = used;
                    size_t total_written = 0;
                    
                    // Write all data from circular buffer (handle wrap-around)
                    while (to_write > 0) {
                        size_t chunk_size = (r + to_write <= LOG_BUFFER_SIZE) ? 
                                           to_write : (LOG_BUFFER_SIZE - r);
                        
                        size_t written = fwrite(&log_buffer[r], 1, chunk_size, log_file);
                        total_written += written;
                        
                        if (written != chunk_size) {
                            printf("[S3_LOGGER] ERROR: Failed to write chunk (wrote %u/%u bytes)\n", (unsigned int)written, (unsigned int)chunk_size);
                            break;
                        }
                        
                        r = (r + chunk_size) % LOG_BUFFER_SIZE;
                        to_write -= chunk_size;
                    }
                    
                    // Force write to disk and close file
                    fflush(log_file);
                    fclose(log_file);
                    
                    // Clear buffer - reset positions
                    read_pos = 0;
                    write_pos = 0;
                    
                    printf("[S3_LOGGER] Successfully wrote %u bytes to SD card and cleared buffer\n", (unsigned int)total_written);
                }
                
                flush_requested = false;
            }
            
            xSemaphoreGive(buffer_mutex);
        }
        
        // Sleep longer since we only work when buffer is full
        vTaskDelay(pdMS_TO_TICKS(500));
    }
}

esp_err_t s3_logger_init(const char *log_file_path) {
    if (logger_initialized) {
        ESP_LOGW(TAG, "Logger already initialized");
        return ESP_OK;
    }

    printf("[S3_LOGGER] Initializing async logger with circular buffer\n");
    printf("[S3_LOGGER] Buffer size: %d bytes - flushes when full\n", LOG_BUFFER_SIZE);
    
    // Create mutex for buffer access
    buffer_mutex = xSemaphoreCreateMutex();
    if (!buffer_mutex) {
        printf("[S3_LOGGER] Failed to create buffer mutex\n");
        return ESP_FAIL;
    }
    
    printf("[S3_LOGGER] Log file path: %s\n", log_file_path);
    
    // Initialize circular buffer
    write_pos = 0;
    read_pos = 0;
    flush_requested = false;
    
    // Create logger task with adequate stack
    BaseType_t task_created = xTaskCreate(
        logger_task,
        "s3_logger",
        4096,  // 4KB stack for the logger task
        NULL,
        1,     // Lower priority to not interfere with other tasks
        &logger_task_handle
    );
    
    if (task_created == pdPASS) {
        printf("[S3_LOGGER] Logger task created successfully\n");
    } else {
        printf("[S3_LOGGER] Logger task creation failed\n");
        vSemaphoreDelete(buffer_mutex);
        buffer_mutex = NULL;
        return ESP_FAIL;
    }
    
    // Set the ESP-IDF vprintf hook to redirect logs
    esp_log_set_vprintf(s3_log_vprintf);
    printf("[S3_LOGGER] ESP log vprintf hook set - async logging active\n");
    
    // Write today's date to mark the start of logging session
    printf("[S3_LOGGER] Writing session start marker to log file...\n");
    FILE *temp_log_file = fopen(log_file_path, "a");
    if (temp_log_file) {
        time_t now = time(NULL);
        struct tm *local_time = localtime(&now);
        fprintf(temp_log_file, "\n=== S3_LOGGER SESSION START: %04d-%02d-%02d %02d:%02d:%02d ===\n",
                local_time->tm_year + 1900, local_time->tm_mon + 1, local_time->tm_mday,
                local_time->tm_hour, local_time->tm_min, local_time->tm_sec);
        fflush(temp_log_file);
        fclose(temp_log_file);
        printf("[S3_LOGGER] Session start marker written successfully\n");
    } else {
        printf("[S3_LOGGER] WARNING: Could not write session start marker\n");
    }
    
    logger_initialized = true;
    
    // Force some ESP-IDF logs to test the hook
    ESP_LOGI(TAG, "Testing ESP-IDF log hook - this should increment call count");
    ESP_LOGW(TAG, "Warning test log");
    ESP_LOGE(TAG, "Error test log");
    
    vTaskDelay(pdMS_TO_TICKS(2000)); // Wait 2 seconds
    printf("[S3_LOGGER] vprintf call count after ESP-IDF test: %u\n", vprintf_call_count);

    return ESP_OK;
}

// Manual flush function for compatibility
void s3_logger_flush_buffer(void) {
    if (!logger_initialized || !buffer_mutex) {
        return;
    }
    
    // Request immediate flush
    flush_requested = true;
    
    // Give some time for the logger task to process the flush
    vTaskDelay(pdMS_TO_TICKS(100));
}

// Test function to get vprintf call count
uint32_t s3_logger_get_call_count(void) {
    return vprintf_call_count;
}

// Thread-safe SD card wrappers (using global DMA mutex from s3_definitions)
// This mutex coordinates DMA operations between SDMMC and BLE to prevent conflicts

void s3_logger_init_mutex(void) {
    // Delegate to s3_definitions - ensures single global mutex instance
    init_sdcard_dma_mutex();
}

void s3_logger_deinit_mutex(void) {
    // Delegate to s3_definitions
    deinit_sdcard_dma_mutex();
}

FILE *s3_fopen(const char *path, const char *mode) {
    FILE *fp = NULL;
    if (g_sdcard_dma_mutex) xSemaphoreTake(g_sdcard_dma_mutex, portMAX_DELAY);
    fp = fopen(path, mode);
    if (g_sdcard_dma_mutex) xSemaphoreGive(g_sdcard_dma_mutex);
    return fp;
}

size_t s3_fread(void *ptr, size_t size, size_t count, FILE *stream) {
    size_t ret;
    if (g_sdcard_dma_mutex) xSemaphoreTake(g_sdcard_dma_mutex, portMAX_DELAY);
    ret = fread(ptr, size, count, stream);
    if (g_sdcard_dma_mutex) xSemaphoreGive(g_sdcard_dma_mutex);
    return ret;
}

size_t s3_fwrite(const void *ptr, size_t size, size_t count, FILE *stream) {
    size_t ret;
    if (g_sdcard_dma_mutex) xSemaphoreTake(g_sdcard_dma_mutex, portMAX_DELAY);
    ret = fwrite(ptr, size, count, stream);
    if (g_sdcard_dma_mutex) xSemaphoreGive(g_sdcard_dma_mutex);
    return ret;
}

int s3_fclose(FILE *stream) {
    int ret;
    if (g_sdcard_dma_mutex) xSemaphoreTake(g_sdcard_dma_mutex, portMAX_DELAY);
    ret = fclose(stream);
    if (g_sdcard_dma_mutex) xSemaphoreGive(g_sdcard_dma_mutex);
    return ret;
}

int s3_remove(const char *path) {
    int ret;
    if (g_sdcard_dma_mutex) xSemaphoreTake(g_sdcard_dma_mutex, portMAX_DELAY);
    ret = remove(path);
    if (g_sdcard_dma_mutex) xSemaphoreGive(g_sdcard_dma_mutex);
    return ret;
}

int s3_rename(const char *oldpath, const char *newpath) {
    int ret;
    if (g_sdcard_dma_mutex) xSemaphoreTake(g_sdcard_dma_mutex, portMAX_DELAY);
    ret = rename(oldpath, newpath);
    if (g_sdcard_dma_mutex) xSemaphoreGive(g_sdcard_dma_mutex);
    return ret;
}

int s3_fseek(FILE *stream, long offset, int whence) {
    int ret;
    if (g_sdcard_dma_mutex) xSemaphoreTake(g_sdcard_dma_mutex, portMAX_DELAY);
    ret = fseek(stream, offset, whence);
    if (g_sdcard_dma_mutex) xSemaphoreGive(g_sdcard_dma_mutex);
    return ret;
}
#endif // USE_S3_LOGGER

// SD Card listing and JSON cache generation functions (available regardless of USE_S3_LOGGER)
// These functions handle SD card directory listing and JSON file tree cache for the HTTP file server

// Tree-style directory listing function
static void list_dir(const char *base_path, int depth, const char *prefix)
{
    DIR *dir = opendir(base_path);
    if (!dir)
    {
        printf("[SD_LIST] Cannot open dir %s\n", base_path);
        return;
    }

    struct dirent *entry;
    char fullpath[256];

    // First, count entries (not counting "." and "..") for last-child logic
    int n_entries = 0;
    while ((entry = readdir(dir)) != NULL)
    {
        if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, "..")) continue;
        n_entries++;
    }
    rewinddir(dir); // Go back to start for actual listing

    int i = 0;
    while ((entry = readdir(dir)) != NULL)
    {
        if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, "..")) continue;

        snprintf(fullpath, sizeof(fullpath), "%s/%s", base_path, entry->d_name);

        struct stat st;
        if (stat(fullpath, &st) != 0) {
            printf("[SD_LIST] stat failed on %s\n", fullpath);
            continue;
        }

        bool is_last = (++i == n_entries);

        // Print the prefix (branches)
        printf("%s", prefix);

        if (depth > 0)
            printf("%s", is_last ? "└── " : "├── ");

        // Print entry
        if (S_ISDIR(st.st_mode)) {
            printf("%s/\n", entry->d_name);

            // Prepare next prefix: add "    " if last, or "│   " if not
            char next_prefix[256];
            snprintf(next_prefix, sizeof(next_prefix), "%s%s", prefix, (depth > 0 ? (is_last ? "    " : "│   ") : ""));

            // Recursive call
            list_dir(fullpath, depth + 1, next_prefix);
        } else {
            printf("%s  [%ld bytes]\n", entry->d_name, (long)st.st_size);
        }
    }
    closedir(dir);
}

// Print SD card contents and generate JSON cache
void print_sdcard_contents(void)
{
    printf("[SD_LIST] SD Card Contents:\n");
    list_dir("/sdcard", 0, "");
    
    // Also generate JSON cache for HTTP server
    cache_sdcard_contents();
}

// Helper function to add JSON file entry to file
static void add_json_entry_to_file(FILE *json_file, const char *name, const char *relative_path,
                                   const char *type, long size, int depth, bool *first_entry)
{
    if (!*first_entry) {
        fprintf(json_file, ",");
    } else {
        *first_entry = false;
    }
    
    fprintf(json_file, "{\"name\":\"%s\",\"type\":\"%s\",\"size\":%ld,\"depth\":%d,\"path\":\"%s\"}",
            name, type, size, depth, relative_path);
}

// JSON version of list_dir for HTTP server caching
static void list_dir_json_to_file(const char *base_path, const char *relative_base, int depth,
                                  FILE *json_file, bool *first_entry)
{
    DIR *dir = opendir(base_path);
    if (!dir) {
        printf("[JSON_CACHE] Cannot open dir %s\n", base_path);
        return;
    }

    struct dirent *entry;
    char fullpath[256];
    char relativepath[256];
    
    while ((entry = readdir(dir)) != NULL) {
        if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, "..")) {
            continue;
        }

        snprintf(fullpath, sizeof(fullpath), "%s/%s", base_path, entry->d_name);
        
        // Build relative path
        if (strlen(relative_base) == 0) {
            strlcpy(relativepath, entry->d_name, sizeof(relativepath));
        } else {
            snprintf(relativepath, sizeof(relativepath), "%s/%s", relative_base, entry->d_name);
        }

        struct stat st;
        if (stat(fullpath, &st) != 0) {
            printf("[JSON_CACHE] stat failed on %s\n", fullpath);
            continue;
        }

        if (S_ISDIR(st.st_mode)) {
            // Add directory entry
            add_json_entry_to_file(json_file, entry->d_name, relativepath, "directory", 0, depth, first_entry);
            
            // Recursively process subdirectory
            list_dir_json_to_file(fullpath, relativepath, depth + 1, json_file, first_entry);
        } else {
            // Add file entry
            add_json_entry_to_file(json_file, entry->d_name, relativepath, "file", (long)st.st_size, depth, first_entry);
        }
    }
    closedir(dir);
}

// Generate JSON file tree cache for HTTP server
void cache_sdcard_contents(void)
{
    const char *json_cache_path = "/sdcard/tmp/file_tree_cache.json";
    
    printf("[JSON_CACHE] Regenerating SD card file tree cache...\n");
    
    // Create tmp directory if it doesn't exist
    struct stat st;
    if (stat("/sdcard/tmp", &st) != 0) {
        if (mkdir("/sdcard/tmp", 0755) != 0) {
            printf("[JSON_CACHE] Failed to create /sdcard/tmp directory\n");
            return;
        }
    }
    
    FILE *json_file = fopen(json_cache_path, "w");
    if (!json_file) {
        printf("[JSON_CACHE] Failed to create JSON cache file: %s\n", json_cache_path);
        return;
    }
    
    // Write JSON header
    fprintf(json_file, "{\"status\":\"success\",\"base_path\":\"/sdcard\",\"files\":[");
    
    bool first_entry = true;
    list_dir_json_to_file("/sdcard", "", 0, json_file, &first_entry);
    
    // Write JSON footer
    fprintf(json_file, "]}");
    
    fclose(json_file);
    printf("[JSON_CACHE] JSON file tree cache generated: %s\n", json_cache_path);
}
