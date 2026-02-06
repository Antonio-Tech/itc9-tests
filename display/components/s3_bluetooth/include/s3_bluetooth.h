/**
 * @file s3_bluetooth.h
 * @brief Unified S3 Bluetooth Manager with Coexistence Support
 *
 * Manages both BLE and Bluetooth Classic operations with proper coexistence
 * to prevent GAP operation conflicts. Provides unified BLE and Bluetooth APIs
 * with DMA optimizations.
 */

#ifndef S3_BLUETOOTH_H
#define S3_BLUETOOTH_H

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"
#include "esp_a2dp_api.h"
#include "esp_avrc_api.h"

/* =================== BACKWARD COMPATIBILITY APIs =================== */

/* BLE APIs - unified interface */
void ble_init(void);
void ble_init_task(void *pvParameters);
void dev_ctrl_update_values(int screen, int msg, int status); // Use NO_UPDATE (-1) to skip
void dev_msg_enable_characteristics(void);
void dev_msg_disable_characteristics(void);
void start_binding(void);
void stop_binding(void);

/* Bluetooth Classic APIs (from bt_a2dp_source.h) - maintaining exact same interface */
typedef enum {
    BT_APP_EVENT_CONNECTION_SUCCESS,    // A2DP connection successfully established
    BT_APP_EVENT_CONNECTION_FAILED,     // Failed to connect
    BT_APP_EVENT_DISCONNECTED,          // Device has been disconnected (manual)
    BT_APP_EVENT_SCAN_FINISHED_NOT_FOUND, // Device scan finished without finding
    BT_APP_EVENT_CONNECTION_TIMEOUT,    // General connection timeout
    BT_APP_EVENT_ABRUPT_DISCONNECTION   // Abrupt disconnection (battery, range, etc)
} bt_app_event_t;

typedef void (*bt_app_event_callback_t)(bt_app_event_t event);
typedef void (*bt_audio_stop_callback_t)(void);

void bt_register_app_callback(bt_app_event_callback_t cb);
void bt_register_audio_stop_callback(bt_audio_stop_callback_t cb);
esp_err_t bt_start_a2dp_source(void);
esp_err_t bt_stop_a2dp_source(void);
esp_err_t bt_deinit_a2dp_source(void);
esp_err_t bt_scan_and_connect_to_strongest(uint8_t scan_duration_seconds);
esp_err_t bt_connect_to_device(const char *device_addr);
bool bt_is_a2dp_connected(void);
bool bt_is_a2dp_scanning(void);
bool bt_is_initialized(void);
esp_err_t bt_ensure_initialized(void);
void bt_a2dp_start_media(void);
void bt_a2dp_stop_media(void);
bool bt_a2dp_wait_for_media_stop(uint32_t timeout_ms);
void bt_notify_audio_stopped(void);  // Notify BT that audio playback stopped (for deferred A2DP connection)

/* Bluetooth Manager APIs (from bt_manager.h) - maintaining exact same interface */
typedef enum {
    BT_STATUS_OFF,           // Bluetooth disabled/deinitialized (was IDLE)
    BT_STATUS_SCANNING,      // User scanning from BT menu (also used for retries)
    BT_STATUS_RECONNECTING,  // Silent background reconnect (was BACKGROUND_SEARCHING)
    BT_STATUS_CONNECTED,     // Successfully connected
    BT_STATUS_FAILED         // Failed all retries (was FAILED_FINAL)
} bt_manager_status_t;

typedef void (*bt_manager_event_cb_t)(bt_manager_status_t status);

void bt_manager_init(bt_manager_event_cb_t callback);
void bt_manager_connect(void);
void bt_manager_disconnect(void);
void bt_manager_mark_disconnection_as_user_initiated(void);
bt_manager_status_t bt_manager_get_status(void);

/* =================== NEW S3 COEXISTENCE APIs =================== */

/**
 * @brief Coexistence states between BLE and BT Classic
 */
typedef enum {
    S3_BT_COEX_IDLE,              // Neither BLE nor BT Classic active
    S3_BT_COEX_BLE_ADVERTISING,   // BLE advertising active
    S3_BT_COEX_BT_SCANNING,       // BT Classic scanning active
    S3_BT_COEX_BLE_CONNECTED,     // BLE connection active
    S3_BT_COEX_BT_CONNECTED,      // BT Classic connection active
    S3_BT_COEX_BOTH_CONNECTED,    // Both BLE and BT Classic connected (ideal state)
    S3_BT_COEX_CONFLICT           // Conflicting operations detected
} s3_bt_coexistence_state_t;

/**
 * @brief Initialize the unified S3 Bluetooth manager (BLE only at boot)
 * This replaces both ble_init() and bt_start_a2dp_source() calls
 */
esp_err_t s3_bluetooth_init(void);

/**
 * @brief Initialize BT Classic when user accesses BT menu
 * Call this when user first accesses BT menu to enable BT Classic functionality
 */
esp_err_t s3_bluetooth_init_bt_classic(void);

/**
 * @brief Get current coexistence state
 */
s3_bt_coexistence_state_t s3_bt_get_coexistence_state(void);

/**
 * @brief Temporarily pause BLE for BT Classic operations
 * @param pause_duration_ms Duration to pause BLE (0 = indefinite)
 * @return ESP_OK if BLE paused successfully
 */
esp_err_t s3_bt_pause_ble_for_bt_operation(uint32_t pause_duration_ms);

/**
 * @brief Resume BLE operations after BT Classic operations complete
 */
esp_err_t s3_bt_resume_ble_operations(void);

/**
 * @brief Check if a given operation would cause coexistence conflict
 * @param bt_scanning Set to true if BT scanning will be started
 * @param ble_advertising Set to true if BLE advertising will be started
 * @return true if conflict would occur
 */
bool s3_bt_would_operations_conflict(bool bt_scanning, bool ble_advertising);

/**
 * @brief Force resolve current coexistence conflict (prioritizes audio streaming)
 */
esp_err_t s3_bt_resolve_coexistence_conflict(void);

/**
 * @brief Handle L2CAP resource allocation failures and implement recovery
 */
esp_err_t s3_bt_handle_l2cap_failure(void);

/**
 * @brief Get DMA memory usage statistics for Bluetooth operations
 * @param total_dma_bt Total DMA memory used by Bluetooth stack
 * @param free_dma_bt Free DMA memory available for Bluetooth
 */
void s3_bt_get_dma_usage(size_t *total_dma_bt, size_t *free_dma_bt);

/**
 * @brief Enable/disable DMA optimizations for audio streaming
 * @param enable true to enable optimizations, false to disable
 */
esp_err_t s3_bt_set_dma_optimizations(bool enable);

/**
 * @brief Emergency cleanup of A2DP/BT Classic to free DMA memory while preserving BLE
 * @note This frees ~48KB DMA memory from A2DP but keeps BLE active
 */
void s3_bt_emergency_cleanup(void);

/**
 * @brief Lightweight performance monitoring for A2DP debugging
 * @note Only call this periodically during A2DP issues for debugging
 */
void s3_bt_log_performance_stats(void);

/* =================== INTERNAL MODULE INTERFACES =================== */

/* These are used internally by the s3_bluetooth component modules */

// BLE Manager internal interface (s3_ble_manager.c)
esp_err_t s3_ble_manager_init(void);
esp_err_t s3_ble_manager_start_advertising(void);
esp_err_t s3_ble_manager_stop_advertising(void);
bool s3_ble_manager_is_advertising(void);
bool s3_ble_manager_is_connected(void);
esp_err_t s3_ble_manager_disconnect_client(void);
void s3_ble_manager_set_coexistence_callback(void (*cb)(bool ble_active));

// BT Classic Manager internal interface (s3_bt_classic.c)
esp_err_t s3_bt_classic_init(void);
esp_err_t s3_bt_classic_start_scan(uint8_t duration);
esp_err_t s3_bt_classic_stop_scan(void);
bool s3_bt_classic_is_scanning(void);
bool s3_bt_classic_is_connected(void);
bool s3_bt_classic_is_streaming(void);
void s3_bt_classic_set_coexistence_callback(void (*cb)(bool bt_scanning));

// BT Manager internal interface (s3_bt_manager.c)
esp_err_t s3_bt_manager_internal_init(bt_manager_event_cb_t callback);

// Coexistence Manager internal interface
void s3_bt_trigger_coexistence_update(void);
void s3_bt_handle_connection_failure(void);
void s3_bt_mark_connection_attempt(void);
void s3_bt_clear_connection_attempt(void);
void s3_bt_handle_scan_no_devices(void);

#endif // S3_BLUETOOTH_H