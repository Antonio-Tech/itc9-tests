#ifndef FILE_TRANSFER_H
#define FILE_TRANSFER_H

#include "esp_err.h"
#include "esp_http_server.h"
#include "esp_peripherals.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#ifdef __cplusplus
extern "C" {
#endif

#define FILE_TRANSFER_PORT 33468
#define FILE_TRANSFER_MAX_FILE_SIZE (10 * 1024 * 1024)  // 10MB max file size
#define FILE_TRANSFER_BUFFER_SIZE 4096

typedef struct {
    httpd_handle_t server;
    bool is_running;
} file_transfer_service_t;

/**
 * @brief Start the HTTP file server
 * 
 * @return esp_err_t ESP_OK on success, error otherwise
 */
esp_err_t http_server_start(void);

/**
 * @brief Stop the HTTP file server
 * 
 * @return esp_err_t ESP_OK on success, error otherwise
 */
esp_err_t http_server_stop(void);

/**
 * @brief Check if HTTP file server is running
 * 
 * @return true if running, false otherwise
 */
bool http_server_is_running(void);

/**
 * @brief CLI command handler for file_transfer command (toggles service on/off)
 * 
 * @param periph Peripheral handle
 * @param argc Argument count
 * @param argv Argument values
 * @return esp_err_t ESP_OK on success, error otherwise
 */
esp_err_t file_transfer(esp_periph_handle_t periph, int argc, char *argv[]);

/**
 * @brief CLI command handler for file_transfer_status command (shows current status)
 * 
 * @param periph Peripheral handle
 * @param argc Argument count
 * @param argv Argument values
 * @return esp_err_t ESP_OK on success, error otherwise
 */
esp_err_t file_transfer_status(esp_periph_handle_t periph, int argc, char *argv[]);

#ifdef __cplusplus
}
#endif

#endif // FILE_TRANSFER_H
