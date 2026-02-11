#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <ctype.h>

#include "file_transfer.h"
#include "esp_log.h"
#include "esp_vfs.h"
#include "esp_http_server.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "WiFi.h"
#include "storage.h"
#include "s3_definitions.h"
#include "s3_logger.h"

// Define MIN macro if not available
#ifndef MIN
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#endif

// HTTP status codes for older ESP-IDF versions
#ifndef HTTPD_413_PAYLOAD_TOO_LARGE
#define HTTPD_413_PAYLOAD_TOO_LARGE HTTPD_500_INTERNAL_SERVER_ERROR
#endif

#ifndef HTTPD_409_CONFLICT
#define HTTPD_409_CONFLICT HTTPD_400_BAD_REQUEST
#endif

static const char *TAG = "FILE_TRANSFER";
static file_transfer_service_t g_file_service = {0};

// HTTP server configuration
#define CONFIG_EXAMPLE_WEB_MOUNT_POINT "/sdcard"
#define FILE_PATH_MAX 256
#define SCRATCH_BUFSIZE 8192

// Helper function to create directories recursively
static esp_err_t create_directories(const char *path) 
{
    char temp_path[256];
    char *pos = NULL;
    
    strncpy(temp_path, path, sizeof(temp_path) - 1);
    temp_path[sizeof(temp_path) - 1] = '\0';
    
    // Find the last '/' to separate directory from filename
    pos = strrchr(temp_path, '/');
    if (pos == NULL) {
        return ESP_OK; // No directory part
    }
    
    *pos = '\0'; // Terminate at last slash to get directory path
    
    // Try to create the directory (mkdir will fail if it exists, which is fine)
    struct stat st;
    if (stat(temp_path, &st) != 0) {
        // Directory doesn't exist, try to create it
        ESP_LOGI(TAG, "Creating directory: %s", temp_path);
        if (mkdir(temp_path, 0755) != 0 && errno != EEXIST) {
            ESP_LOGE(TAG, "Failed to create directory: %s (errno: %d)", temp_path, errno);
            return ESP_FAIL;
        }
    }
    
    return ESP_OK;
}

// URL decoding function to handle encoded paths like %2F -> /
static void url_decode(char *dst, const char *src)
{
    char a, b;
    while (*src) {
        if ((*src == '%') &&
            ((a = src[1]) && (b = src[2])) &&
            (isxdigit(a) && isxdigit(b))) {
            if (a >= 'a')
                a -= 'a'-'A';
            if (a >= 'A')
                a -= ('A' - 10);
            else
                a -= '0';
            if (b >= 'a')
                b -= 'a'-'A';
            if (b >= 'A')
                b -= ('A' - 10);
            else
                b -= '0';
            *dst++ = 16*a+b;
            src+=3;
        } else if (*src == '+') {
            *dst++ = ' ';
            src++;
        } else {
            *dst++ = *src++;
        }
    }
    *dst = '\0';
}

// HTTP server now uses cached JSON files instead of live directory scanning

/* Handler to list ALL files in SD card - serves cached JSON file */
static esp_err_t http_list_handler(httpd_req_t *req)
{
    const char *json_cache_path = "/sdcard/tmp/file_tree_cache.json";
    
    ESP_LOGI(TAG, "[LIST] Received list request - serving cached file tree");
    ESP_LOGI(TAG, "[LIST] Available stack space: %d bytes", uxTaskGetStackHighWaterMark(NULL));
    
    // Check if cached JSON file exists
    struct stat st;
    if (stat(json_cache_path, &st) != 0) {
        ESP_LOGW(TAG, "[LIST] Cached file tree not found at %s (errno: %d), generating fallback response", json_cache_path, errno);
        httpd_resp_set_type(req, "application/json");
        const char* error_response = "{\"status\":\"error\",\"message\":\"File tree cache not found. Please run 'tree' command first.\"}";
        esp_err_t resp_result = httpd_resp_sendstr(req, error_response);
        ESP_LOGI(TAG, "[LIST] Fallback response sent, result: %s", esp_err_to_name(resp_result));
        return ESP_OK;
    }
    
    ESP_LOGI(TAG, "[LIST] Cache file found: %s (%ld bytes)", json_cache_path, st.st_size);
    
    // Open and serve the cached JSON file
    FILE *json_file = fopen(json_cache_path, "r");
    if (!json_file) {
        ESP_LOGE(TAG, "[LIST] Failed to open cached file tree: %s (errno: %d)", json_cache_path, errno);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to read file tree cache");
        return ESP_FAIL;
    }
    
    ESP_LOGI(TAG, "[LIST] File opened successfully, setting response headers");
    httpd_resp_set_type(req, "application/json");
    
    // Read and send file in chunks (much more memory efficient)
    char buffer[1024];  // Small buffer for HTTP server task
    size_t bytes_read;
    size_t total_sent = 0;
    
    ESP_LOGI(TAG, "[LIST] Starting to send file in chunks");
    while ((bytes_read = fread(buffer, 1, sizeof(buffer), json_file)) > 0) {
        ESP_LOGD(TAG, "[LIST] Read %d bytes from file, total sent so far: %d", bytes_read, total_sent);
        
        esp_err_t send_result = httpd_resp_send_chunk(req, buffer, bytes_read);
        if (send_result != ESP_OK) {
            ESP_LOGE(TAG, "[LIST] Error sending JSON chunk (result: %s, bytes_read: %d, total_sent: %d)", 
                     esp_err_to_name(send_result), bytes_read, total_sent);
            fclose(json_file);
            return ESP_FAIL;
        }
        
        total_sent += bytes_read;
        
        // Check stack space periodically during large transfers
        if (total_sent % (4 * 1024) == 0) {  // Every 4KB
            ESP_LOGD(TAG, "[LIST] Stack space remaining: %d bytes (sent %d bytes)", 
                     uxTaskGetStackHighWaterMark(NULL), total_sent);
        }
    }
    
    ESP_LOGI(TAG, "[LIST] File reading complete, closing file");
    fclose(json_file);
    
    // Send empty chunk to signal end of response
    ESP_LOGI(TAG, "[LIST] Sending final empty chunk to complete response");
    esp_err_t final_result = httpd_resp_send_chunk(req, NULL, 0);
    
    if (final_result == ESP_OK) {
        ESP_LOGI(TAG, "[LIST] List request completed successfully - served cached file tree (%ld bytes, %d total sent)", 
                 st.st_size, total_sent);
    } else {
        ESP_LOGE(TAG, "[LIST] Failed to send final chunk: %s", esp_err_to_name(final_result));
        return ESP_FAIL;
    }
    
    ESP_LOGI(TAG, "[LIST] Final stack space: %d bytes", uxTaskGetStackHighWaterMark(NULL));
    return ESP_OK;
}

/* Handler to download a file */
static esp_err_t http_download_handler(httpd_req_t *req)
{
    char filepath[FILE_PATH_MAX];
    char filename[FILE_PATH_MAX];
    FILE *fd = NULL;
    struct stat file_stat;
    char *chunk;
    
    /* Get filename from query string */
    size_t query_len = httpd_req_get_url_query_len(req) + 1;
    if (query_len > 1) {
        char *query = malloc(query_len);
        if (httpd_req_get_url_query_str(req, query, query_len) == ESP_OK) {
            char encoded_filename[FILE_PATH_MAX];
            if (httpd_query_key_value(query, "path", encoded_filename, sizeof(encoded_filename)) == ESP_OK) {
                // URL decode the filename to handle %2F -> / conversion
                url_decode(filename, encoded_filename);
                
                // Construct full file path with bounds checking
                int ret = snprintf(filepath, sizeof(filepath), "%s/%s", CONFIG_EXAMPLE_WEB_MOUNT_POINT, filename);
                if (ret >= sizeof(filepath)) {
                    free(query);
                    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "File path too long");
                    return ESP_FAIL;
                }
                ESP_LOGI(TAG, "Download request for: %s (decoded from: %s)", filepath, encoded_filename);
            } else {
                free(query);
                httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing file parameter");
                return ESP_FAIL;
            }
        } else {
            free(query);
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid query string");
            return ESP_FAIL;
        }
        free(query);
    } else {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing file parameter");
        return ESP_FAIL;
    }
    
    ESP_LOGI(TAG, "Attempting to stat file: %s", filepath);
    if (stat(filepath, &file_stat) == -1) {
        ESP_LOGW(TAG, "Ghost file - File does not exist: %s (errno: %d), but ignoring gracefully", filepath, errno);
        
        // Try alternative path constructions
        char alt_path[FILE_PATH_MAX];
        int ret = snprintf(alt_path, sizeof(alt_path), "/sdcard/%s", filename);
        if (ret >= sizeof(alt_path)) {
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "File path too long");
            return ESP_FAIL;
        }
        ESP_LOGI(TAG, "Trying alternative path: %s", alt_path);
        
        if (stat(alt_path, &file_stat) == 0) {
            ESP_LOGI(TAG, "Alternative path works, using: %s", alt_path);
            strlcpy(filepath, alt_path, sizeof(filepath));
        } else {
            ESP_LOGW(TAG, "Ghost file - Alternative path also failed: %s (errno: %d), returning 404 gracefully", alt_path, errno);
            httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "File does not exist (ghost file - may have been deleted)");
            return ESP_FAIL;
        }
    }
    
    if (!S_ISREG(file_stat.st_mode)) {
        ESP_LOGE(TAG, "Not a regular file : %s", filepath);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Not a regular file");
        return ESP_FAIL;
    }
    
    fd = fopen(filepath, "r");
    if (!fd) {
        ESP_LOGE(TAG, "Failed to read existing file : %s", filepath);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to read existing file");
        return ESP_FAIL;
    }
    
    ESP_LOGI(TAG, "Sending file : %s (%ld bytes)...", filepath, file_stat.st_size);
    httpd_resp_set_type(req, "application/octet-stream");
    
    /* Add filename to content disposition header */
    char *filename_only = strrchr(filepath, '/');
    if (filename_only) {
        filename_only++; // Skip the '/'
        char content_disp[256];
        snprintf(content_disp, sizeof(content_disp), "attachment; filename=\"%s\"", filename_only);
        httpd_resp_set_hdr(req, "Content-Disposition", content_disp);
    }
    
    /* Allocate buffer with stack safety in mind */
    chunk = malloc(SCRATCH_BUFSIZE);
    if (!chunk) {
        ESP_LOGE(TAG, "Failed to allocate memory for chunk buffer");
        fclose(fd);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Memory allocation failed");
        return ESP_FAIL;
    }
    
    size_t chunksize;
    do {
        /* Read file in chunks into the scratch buffer */
        chunksize = fread(chunk, 1, SCRATCH_BUFSIZE, fd);
        
        if (chunksize > 0) {
            /* Send the buffer contents as HTTP response chunk */
            if (httpd_resp_send_chunk(req, chunk, chunksize) != ESP_OK) {
                fclose(fd);
                free(chunk);
                ESP_LOGE(TAG, "File sending failed!");
                /* Abort sending file */
                httpd_resp_sendstr_chunk(req, NULL);
                /* Respond with 500 Internal Server Error */
                httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to send file");
                return ESP_FAIL;
            }
        }
    } while (chunksize != 0);
    
    /* Close file after sending complete */
    fclose(fd);
    free(chunk);
    ESP_LOGI(TAG, "File sending complete");
    
    /* Respond with an empty chunk to signal HTTP response completion */
    httpd_resp_send_chunk(req, NULL, 0);
    return ESP_OK;
}

/* Handler to upload a file */
static esp_err_t http_upload_handler(httpd_req_t *req)
{
    char filepath[FILE_PATH_MAX];
    FILE *fd = NULL;
    struct stat file_stat;
    
    /* Get filename from query string */
    size_t query_len = httpd_req_get_url_query_len(req) + 1;
    if (query_len > 1) {
        char *query = malloc(query_len);
        if (httpd_req_get_url_query_str(req, query, query_len) == ESP_OK) {
            char filename[FILE_PATH_MAX];
            char encoded_filename[FILE_PATH_MAX];
            if (httpd_query_key_value(query, "path", encoded_filename, sizeof(encoded_filename)) == ESP_OK) {
                // URL decode the filename to handle %2F -> / conversion
                url_decode(filename, encoded_filename);
                
                // Construct full file path with bounds checking
                int ret = snprintf(filepath, sizeof(filepath), "%s/%s", CONFIG_EXAMPLE_WEB_MOUNT_POINT, filename);
                if (ret >= sizeof(filepath)) {
                    free(query);
                    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "File path too long");
                    return ESP_FAIL;
                }
                ESP_LOGI(TAG, "Upload request for: %s (decoded from: %s)", filepath, encoded_filename);
            } else {
                free(query);
                httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing file parameter");
                return ESP_FAIL;
            }
        } else {
            free(query);
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid query string");
            return ESP_FAIL;
        }
        free(query);
    } else {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing file parameter");
        return ESP_FAIL;
    }
    
    /* Create directories if needed */
    if (create_directories(filepath) != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to create directories");
        return ESP_FAIL;
    }
    
    /* File cannot be larger than a limit */
    if (req->content_len > (1024 * 1024)) { // 1MB limit
        ESP_LOGE(TAG, "File too large : %d bytes", req->content_len);
        /* Respond with 413 Payload Too Large */
        httpd_resp_send_err(req, HTTPD_413_PAYLOAD_TOO_LARGE, "File size must be less than 1MB");
        return ESP_FAIL;
    }
    
    /* Check if path exists and handle appropriately */
    if (stat(filepath, &file_stat) == 0) {
        if (S_ISDIR(file_stat.st_mode)) {
            /* Cannot overwrite a directory with a file */
            ESP_LOGE(TAG, "Cannot overwrite directory with file: %s", filepath);
            httpd_resp_send_err(req, HTTPD_409_CONFLICT, "Cannot overwrite directory with file");
            return ESP_FAIL;
        } else if (S_ISREG(file_stat.st_mode)) {
            /* File exists, allow overwrite by removing it first */
            ESP_LOGI(TAG, "File exists, will overwrite: %s", filepath);
            unlink(filepath);  // Delete existing file to allow overwrite
        } else {
            /* Other file types (symlinks, etc.) - prevent overwrite */
            ESP_LOGE(TAG, "Cannot overwrite special file: %s", filepath);
            httpd_resp_send_err(req, HTTPD_409_CONFLICT, "Cannot overwrite special file");
            return ESP_FAIL;
        }
    }
    
    fd = fopen(filepath, "w");
    if (!fd) {
        ESP_LOGE(TAG, "Failed to create file : %s", filepath);
        /* Respond with 500 Internal Server Error */
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to create file");
        return ESP_FAIL;
    }
    
    ESP_LOGI(TAG, "Receiving file : %s (%d bytes)...", filepath, req->content_len);
    
    /* Allocate buffer with stack safety in mind */
    char *buf = malloc(SCRATCH_BUFSIZE);
    if (!buf) {
        ESP_LOGE(TAG, "Failed to allocate memory for receive buffer");
        fclose(fd);
        unlink(filepath);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Memory allocation failed");
        return ESP_FAIL;
    }
    
    int remaining = req->content_len;
    
    while (remaining > 0) {
        ESP_LOGI(TAG, "Remaining size : %d", remaining);
        /* Receive the file part by part into a buffer */
        int recv_len = httpd_req_recv(req, buf, MIN(remaining, SCRATCH_BUFSIZE));
        if (recv_len <= 0) {
            if (recv_len == HTTPD_SOCK_ERR_TIMEOUT) {
                /* Retry if timeout occurred */
                continue;
            }
            
            ESP_LOGE(TAG, "File reception failed!");
            /* Respond with 500 Internal Server Error */
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to receive file");
            fclose(fd);
            unlink(filepath);
            free(buf);
            return ESP_FAIL;
        }
        
        /* Write buffer content to file on storage */
        if (recv_len != fwrite(buf, 1, recv_len, fd)) {
            ESP_LOGE(TAG, "File write failed!");
            /* Respond with 500 Internal Server Error */
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to write file");
            fclose(fd);
            unlink(filepath);
            free(buf);
            return ESP_FAIL;
        }
        
        remaining -= recv_len;
    }
    
    /* Close file upon upload completion */
    fclose(fd);
    free(buf);
    ESP_LOGI(TAG, "File reception complete");
    
    /* Update JSON cache after successful upload */
    ESP_LOGI(TAG, "Updating JSON cache after file upload");
    cache_sdcard_contents();
    
    /* Redirect onto root to see the updated file list */
    httpd_resp_set_status(req, "303 See Other");
    httpd_resp_set_hdr(req, "Location", "/");
    httpd_resp_sendstr(req, "File uploaded successfully");
    return ESP_OK;
}

/* Handler to delete a file */
static esp_err_t http_delete_handler(httpd_req_t *req)
{
    char filepath[FILE_PATH_MAX];
    struct stat file_stat;
    
    /* Get filename from query string */
    size_t query_len = httpd_req_get_url_query_len(req) + 1;
    if (query_len > 1) {
        char *query = malloc(query_len);
        if (httpd_req_get_url_query_str(req, query, query_len) == ESP_OK) {
            char filename[FILE_PATH_MAX];
            char encoded_filename[FILE_PATH_MAX];
            if (httpd_query_key_value(query, "path", encoded_filename, sizeof(encoded_filename)) == ESP_OK) {
                // URL decode the filename to handle %2F -> / conversion
                url_decode(filename, encoded_filename);
                
                // Construct full file path with bounds checking
                int ret = snprintf(filepath, sizeof(filepath), "%s/%s", CONFIG_EXAMPLE_WEB_MOUNT_POINT, filename);
                if (ret >= sizeof(filepath)) {
                    free(query);
                    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "File path too long");
                    return ESP_FAIL;
                }
                ESP_LOGI(TAG, "Delete request for: %s (decoded from: %s)", filepath, encoded_filename);
            } else {
                free(query);
                httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing file parameter");
                return ESP_FAIL;
            }
        } else {
            free(query);
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid query string");
            return ESP_FAIL;
        }
        free(query);
    } else {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing file parameter");
        return ESP_FAIL;
    }
    
    /* Check if file exists */
    if (stat(filepath, &file_stat) == -1) {
        ESP_LOGW(TAG, "Ghost file - Delete request for non-existing file: %s (errno: %d), returning success gracefully", filepath, errno);
        
        /* Update JSON cache anyway since this might be a ghost file removal */
        ESP_LOGI(TAG, "Updating JSON cache after ghost file delete attempt");
        cache_sdcard_contents();
        
        /* Respond with success - the file is "deleted" (doesn't exist anyway) */
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, "{\"success\":true,\"message\":\"File deleted successfully (was ghost file)\"}");
        return ESP_OK;
    }
    
    /* Delete file */
    if (unlink(filepath) != 0) {
        ESP_LOGE(TAG, "Failed to delete file : %s (errno: %d)", filepath, errno);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to delete file");
        return ESP_FAIL;
    }
    
    ESP_LOGI(TAG, "File deleted : %s", filepath);
    
    /* Update JSON cache after successful delete */
    ESP_LOGI(TAG, "Updating JSON cache after file deletion");
    cache_sdcard_contents();
    
    /* Respond with JSON success message */
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"success\":true,\"message\":\"File deleted successfully\"}");
    return ESP_OK;
}

/* Function to start the file server */
esp_err_t http_server_start(void)
{
    if (g_file_service.server) {
        ESP_LOGW(TAG, "HTTP server already running");
        return ESP_OK;
    }
    
    // Check memory state before starting HTTP server
    size_t free_heap = esp_get_free_heap_size();
    if (free_heap < 100000) {  // Need at least 100KB free
        ESP_LOGE(TAG, "Insufficient memory for HTTP server: %d bytes free", free_heap);
        return ESP_ERR_NO_MEM;
    }
    
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = FILE_TRANSFER_PORT;  // Use defined port 33468
    config.lru_purge_enable = true;

    // Increase stack size to prevent stack overflow during file operations
    config.stack_size = 8192;  // Increased from default 4KB to 8KB
    config.task_priority = 5;  // Set appropriate priority

    // Configure limits to prevent memory exhaustion
    config.max_uri_handlers = 8;
    config.max_resp_headers = 8;
    config.backlog_conn = 3;  // Limit concurrent connections
    config.recv_wait_timeout = 10;  // 10 seconds timeout
    config.send_wait_timeout = 10;  // 10 seconds timeout
    config.max_open_sockets = 3;  // Limit to 3 sockets to fit within LWIP_MAX_SOCKETS=6 (3 used internally)
    
    // Start the httpd server - avoid complex logging that triggers newlib locks
    printf("FILE_TRANSFER: Starting server on port: %d\n", config.server_port);
    if (httpd_start(&g_file_service.server, &config) == ESP_OK) {
        // Set URI handlers - use simple printf to avoid ESP_LOGI lock issues
        printf("FILE_TRANSFER: Registering URI handlers\n");
        
        httpd_uri_t list_uri = {
            .uri       = "/list",
            .method    = HTTP_GET,
            .handler   = http_list_handler,
            .user_ctx  = NULL
        };
        httpd_register_uri_handler(g_file_service.server, &list_uri);
        
        httpd_uri_t download_uri = {
            .uri       = "/dw",
            .method    = HTTP_GET,
            .handler   = http_download_handler,
            .user_ctx  = NULL
        };
        httpd_register_uri_handler(g_file_service.server, &download_uri);
        
        httpd_uri_t upload_uri = {
            .uri       = "/up",
            .method    = HTTP_POST,
            .handler   = http_upload_handler,
            .user_ctx  = NULL
        };
        httpd_register_uri_handler(g_file_service.server, &upload_uri);
        
        httpd_uri_t delete_uri = {
            .uri       = "/rm",
            .method    = HTTP_DELETE,
            .handler   = http_delete_handler,
            .user_ctx  = NULL
        };
        httpd_register_uri_handler(g_file_service.server, &delete_uri);
        
        g_file_service.is_running = true;
        return ESP_OK;
    }
    
    ESP_LOGI(TAG, "Error starting server!");
    return ESP_FAIL;
}

/* Function to stop the file server */
esp_err_t http_server_stop(void)
{
    if (!g_file_service.server) {
        ESP_LOGW(TAG, "HTTP server not running");
        return ESP_OK;
    }
    
    ESP_LOGI(TAG, "Stopping HTTP server");
    esp_err_t ret = httpd_stop(g_file_service.server);
    if (ret == ESP_OK) {
        g_file_service.server = NULL;
        g_file_service.is_running = false;
        ESP_LOGI(TAG, "HTTP server stopped");
    }
    return ret;
}

bool http_server_is_running(void)
{
    return g_file_service.is_running;
}


// CLI Commands
esp_err_t file_transfer(esp_periph_handle_t periph, int argc, char *argv[])
{
    if (http_server_is_running()) {
        // Turning OFF file server
        ESP_LOGI(TAG, "Stopping file transfer service...");
        esp_err_t ret = http_server_stop();
        
        // Always disconnect WiFi when turning off file server
        ESP_LOGI(TAG, "Disconnecting WiFi...");
        esp_wifi_disconnect();
        deinit_wifi_station();
        
        return ret;
    } else {
        // Turning ON file server
        ESP_LOGI(TAG, "Starting file transfer service...");
        
        // Check if WiFi is connected
        if (!is_wifi_connected()) {
            ESP_LOGI(TAG, "WiFi not connected, attempting to connect using stored credentials...");
            
            // Use setup_wifi with stored NVS credentials
            esp_err_t wifi_ret = setup_wifi(WIFI_NVS_CREDENTIAL);
            if (wifi_ret != ESP_OK) {
                ESP_LOGE(TAG, "Failed to connect to WiFi. Cannot start file server without network.");
                return wifi_ret;
            }
            
            // Wait a bit for WiFi connection to establish
            vTaskDelay(pdMS_TO_TICKS(3000));
            
            // Check again if connection was successful
            if (!is_wifi_connected()) {
                ESP_LOGE(TAG, "WiFi connection failed. Cannot start file server.");
                return ESP_FAIL;
            }
        }
        
        // WiFi is connected, start the file server
        esp_err_t ret = http_server_start();
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to start file transfer service");
        } else {
            ESP_LOGI(TAG, "File transfer service started successfully");
        }
        return ret;
    }
}

esp_err_t file_transfer_status(esp_periph_handle_t periph, int argc, char *argv[])
{
    if (http_server_is_running()) {
        ESP_LOGI(TAG, "File transfer service: RUNNING (HTTP server mode)");
        ESP_LOGI(TAG, "Server port: %d", FILE_TRANSFER_PORT);
        
        // Get network information
        esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
        if (netif != NULL) {
            // Get IP address
            esp_netif_ip_info_t ip_info;
            esp_err_t ret = esp_netif_get_ip_info(netif, &ip_info);
            if (ret == ESP_OK) {
                ESP_LOGI(TAG, "Server IP: " IPSTR, IP2STR(&ip_info.ip));
                ESP_LOGI(TAG, "Server URL: http://" IPSTR ":%d", IP2STR(&ip_info.ip), FILE_TRANSFER_PORT);
            }
            
            // Get WiFi SSID
            wifi_ap_record_t ap_info;
            ret = esp_wifi_sta_get_ap_info(&ap_info);
            if (ret == ESP_OK) {
                ESP_LOGI(TAG, "Connected SSID: %s", (char*)ap_info.ssid);
                ESP_LOGI(TAG, "Signal strength: %d dBm", ap_info.rssi);
            } else {
                ESP_LOGW(TAG, "Not connected to WiFi");
            }
        } else {
            ESP_LOGW(TAG, "WiFi network interface not available");
        }
        
        ESP_LOGI(TAG, "Available endpoints:");
        ESP_LOGI(TAG, "  GET  /list                - List files and directories");
        ESP_LOGI(TAG, "  GET  /dw?path=file        - Download a file");
        ESP_LOGI(TAG, "  POST /up?path=file        - Upload a file");
        ESP_LOGI(TAG, "  DEL  /rm?path=file        - Delete a file");
    } else {
        ESP_LOGI(TAG, "File transfer service: STOPPED");
    }
    return ESP_OK;
}

