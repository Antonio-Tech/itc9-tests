// S3 Bluetooth Manager V2 - Single Task Architecture
// Refactored to eliminate race conditions by using one dedicated task

#include "s3_bluetooth.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_gap_bt_api.h"
#include "esp_err.h"
#include "esp_log.h"
#include "lvgl.h"
#include "s3_definitions.h"
#include "esp_bt_device.h"

static const char *TAG = "S3_BT_MANAGER";

// --- BT Scan Configuration ---
#define MAX_BT_RETRIES              5
#define BT_SCAN_DURATION_SEC        6  // Each scan runs for 6 seconds
#define BT_CONNECTION_TIMEOUT_SEC   (BT_SCAN_DURATION_SEC + 2)  // Total timeout: scan + A2DP handshake
#define BT_STACK_RECOVER_SEC        1  // Breathing room between retries for BT stack

// --- Command Queue ---
#define BT_CMD_QUEUE_SIZE 10

typedef enum {
    BT_CMD_CONNECT,              // User-initiated connect from BT menu
    BT_CMD_DISCONNECT,           // User-initiated disconnect
    BT_CMD_RETRY,                // Normal retry after failure
    BT_CMD_ABRUPT_DISCONNECT,    // Handle abrupt disconnect with special flow
    BT_CMD_BACKGROUND_RETRY,     // Silent background retry
    BT_CMD_FINAL_CLEANUP,        // Final cleanup after max retries
    BT_CMD_CONNECTION_SUCCESS,   // Connection succeeded
    BT_CMD_CONNECTION_FAILED,    // Connection/scan failed
} bt_cmd_type_t;

typedef struct {
    bt_cmd_type_t type;
    uint32_t param;  // Optional parameter (e.g. retry count)
} bt_cmd_t;

// --- Task State ---
typedef struct {
    TaskHandle_t task_handle;
    QueueHandle_t cmd_queue;
    lv_timer_t *timeout_timer;      // Single timer for timeouts
    TaskHandle_t deinit_task_handle;
    
    // State variables (owned by task - no races!)
    int retry_count;
    bool silent_mode;               // Silent background retries
    bool abrupt_disconnect_mode;    // In abrupt disconnect flow
    bool user_initiated_disconnect;
    bool deinit_in_progress;        // Flag to block operations during deinit
    bt_manager_status_t current_status;
    
    // Callbacks
    bt_manager_event_cb_t app_callback;
} bt_task_state_t;

static bt_task_state_t g_bt_state = {0};

// --- Forward Declarations ---
static void bt_connection_task(void *param);
static void bt_timeout_cb(lv_timer_t *timer);
static void clear_all_bonded_devices(void);
static void bt_manager_deinit_task(void *param);
static void bt_manager_deinit_final_task(void *param);

// --- Utility Functions ---

static void update_status_and_notify(bt_manager_status_t new_status) {
    g_bt_state.current_status = new_status;
    ESP_LOGI(TAG, "New status: %d", new_status);
    
    if (new_status == BT_STATUS_OFF) {
        s3_bt_clear_connection_attempt();
        s3_bt_trigger_coexistence_update();
    }
    
    if (g_bt_state.app_callback) {
        g_bt_state.app_callback(new_status);
    }
}

static bool send_command(bt_cmd_type_t type, uint32_t param) {
    bt_cmd_t cmd = {.type = type, .param = param};
    
    if (xQueueSend(g_bt_state.cmd_queue, &cmd, pdMS_TO_TICKS(100)) != pdTRUE) {
        ESP_LOGW(TAG, "Failed to send command %d to queue", type);
        return false;
    }
    return true;
}

static void start_timeout_timer(uint32_t timeout_ms, bt_cmd_type_t timeout_cmd) {
    // Cancel existing timer safely
    if (g_bt_state.timeout_timer) {
        lv_timer_pause(g_bt_state.timeout_timer);
        lv_timer_set_repeat_count(g_bt_state.timeout_timer, 0);
        lv_timer_del(g_bt_state.timeout_timer);
        g_bt_state.timeout_timer = NULL;
    }
    
    // Store command type in timer user data
    g_bt_state.timeout_timer = lv_timer_create(bt_timeout_cb, timeout_ms, (void*)(intptr_t)timeout_cmd);
    lv_timer_set_repeat_count(g_bt_state.timeout_timer, 1);
}

static void cancel_timeout_timer(void) {
    if (g_bt_state.timeout_timer) {
        // Pause and mark for deletion instead of immediate delete
        // This prevents crash when called during LVGL timer iteration
        lv_timer_pause(g_bt_state.timeout_timer);
        lv_timer_set_repeat_count(g_bt_state.timeout_timer, 0);
        lv_timer_del(g_bt_state.timeout_timer);
        g_bt_state.timeout_timer = NULL;
    }
}

// Timer callback - just sends command to task
static void bt_timeout_cb(lv_timer_t *timer) {
    bt_cmd_type_t cmd = (bt_cmd_type_t)(intptr_t)timer->user_data;
    g_bt_state.timeout_timer = NULL;  // Timer fired, clear reference
    send_command(cmd, 0);
}

// --- Main Connection Task ---

static void bt_connection_task(void *param) {
    ESP_LOGI(TAG, "BT connection task started");
    bt_cmd_t cmd;
    
    while (1) {
        // Wait for commands
        if (xQueueReceive(g_bt_state.cmd_queue, &cmd, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        
        ESP_LOGI(TAG, "Processing command: %d", cmd.type);
        
        switch (cmd.type) {
            case BT_CMD_CONNECT: {
                // User-initiated connection from BT menu
                ESP_LOGI(TAG, "CMD_CONNECT: Starting user connection");
                
                // Block if deinit is still in progress (prevents race on slow hardware)
                if (g_bt_state.deinit_in_progress) {
                    ESP_LOGW(TAG, "Deinit still in progress, delaying connect by 500ms");
                    vTaskDelay(pdMS_TO_TICKS(500));
                    // Re-check after delay
                    if (g_bt_state.deinit_in_progress) {
                        ESP_LOGE(TAG, "Deinit still not complete after delay, aborting connect");
                        break;
                    }
                }
                
                // Reset state
                g_bt_state.retry_count = 0;
                g_bt_state.silent_mode = false;
                g_bt_state.abrupt_disconnect_mode = false;
                g_bt_state.user_initiated_disconnect = false;
                
                // Send 0x47 if BLE connected
                if (s3_ble_manager_is_connected()) {
                    extern uint8_t gPixseeStatus;
                    gPixseeStatus = S3ER_ATTENTION_BLE_SCAN_A2DP; // enum value from s3_definitions.h
                    ESP_LOGI(TAG, "Sent S3ER_ATTENTION_BLE_SCAN_A2DP (0x47)");
                }
                
                // Initialize BT stack
                esp_err_t init_result = bt_start_a2dp_source();
                if (init_result != ESP_OK) {
                    ESP_LOGE(TAG, "Failed to initialize BT: %s", esp_err_to_name(init_result));;
                    update_status_and_notify(BT_STATUS_FAILED);
                    break;
                }
                
                // Re-register callback (might be lost during init)
                extern void bt_register_app_callback(void (*cb)(bt_app_event_t));
                extern void manager_internal_event_handler(bt_app_event_t event);
                bt_register_app_callback(manager_internal_event_handler);
                
                // Update status and start scan
                update_status_and_notify(BT_STATUS_SCANNING);
                
                esp_err_t scan_result = bt_scan_and_connect_to_strongest(BT_SCAN_DURATION_SEC);
                if (scan_result != ESP_OK) {
                    ESP_LOGE(TAG, "Failed to start scan: %s", esp_err_to_name(scan_result));
                    update_status_and_notify(BT_STATUS_FAILED);
                    break;
                }
                
                // Start timeout timer (scan + connection time)
                start_timeout_timer(BT_CONNECTION_TIMEOUT_SEC * 1000, BT_CMD_CONNECTION_FAILED);
                break;
            }
            
            case BT_CMD_RETRY: {
                // Normal retry (from BT menu after failure)
                ESP_LOGI(TAG, "CMD_RETRY: Attempt %d/%d", g_bt_state.retry_count + 1, MAX_BT_RETRIES);
                
                update_status_and_notify(BT_STATUS_SCANNING);
                
                // Cancel any previous scan
                esp_bt_gap_cancel_discovery();
                vTaskDelay(pdMS_TO_TICKS(1000));  // Wait for scan to stop
                
                esp_err_t scan_result = bt_scan_and_connect_to_strongest(BT_SCAN_DURATION_SEC);
                if (scan_result != ESP_OK) {
                    ESP_LOGE(TAG, "Retry scan failed: %s", esp_err_to_name(scan_result));
                    send_command(BT_CMD_CONNECTION_FAILED, 0);
                    break;
                }
                
                start_timeout_timer(BT_CONNECTION_TIMEOUT_SEC * 1000, BT_CMD_CONNECTION_FAILED);
                break;
            }
            
            case BT_CMD_ABRUPT_DISCONNECT: {
                // Handle abrupt disconnection
                ESP_LOGI(TAG, "CMD_ABRUPT_DISCONNECT: Starting special flow");
                
                cancel_timeout_timer();
                
                g_bt_state.abrupt_disconnect_mode = true;
                g_bt_state.silent_mode = false;
                g_bt_state.retry_count = 0;
                
                // Pause audio immediately (fast path - don't wait for state machine)
                extern void pause_audio_for_bt_disconnect(void);
                pause_audio_for_bt_disconnect();
                
                // Start background retries immediately (no wait)
                send_command(BT_CMD_BACKGROUND_RETRY, 0);
                break;
            }
            
            case BT_CMD_BACKGROUND_RETRY: {
                // Silent background retry
                if (g_bt_state.retry_count == 0) {
                    // First background retry - transition to silent mode
                    ESP_LOGI(TAG, "CMD_BACKGROUND_RETRY: Starting silent retries");
                    g_bt_state.silent_mode = true;
                    g_bt_state.abrupt_disconnect_mode = false;
                    update_status_and_notify(BT_STATUS_RECONNECTING);
                }
                
                ESP_LOGI(TAG, "Background retry %d/%d", g_bt_state.retry_count + 1, MAX_BT_RETRIES);
                
                // Cancel previous scan
                esp_bt_gap_cancel_discovery();
                vTaskDelay(pdMS_TO_TICKS(500));
                
                esp_err_t scan_result = bt_scan_and_connect_to_strongest(BT_SCAN_DURATION_SEC);
                if (scan_result != ESP_OK) {
                    ESP_LOGE(TAG, "Background scan failed: %s", esp_err_to_name(scan_result));
                    send_command(BT_CMD_CONNECTION_FAILED, 0);
                    break;
                }
                
                // Start timeout timer for this retry attempt (scan + connection time)
                start_timeout_timer(BT_CONNECTION_TIMEOUT_SEC * 1000, BT_CMD_CONNECTION_FAILED);
                break;
            }
            
            case BT_CMD_CONNECTION_SUCCESS: {
                // Connection succeeded
                ESP_LOGI(TAG, "CMD_CONNECTION_SUCCESS");
                
                cancel_timeout_timer();
                esp_bt_gap_cancel_discovery();
                
                // Reset state
                g_bt_state.retry_count = 0;
                g_bt_state.silent_mode = false;
                g_bt_state.abrupt_disconnect_mode = false;
                
                update_status_and_notify(BT_STATUS_CONNECTED);
                break;
            }
            
            case BT_CMD_CONNECTION_FAILED: {
                // Connection/scan failed
                ESP_LOGI(TAG, "CMD_CONNECTION_FAILED");
                
                cancel_timeout_timer();
                
                // CRITICAL: Ignore if already connected (race condition - timeout fired after success)
                if (g_bt_state.current_status == BT_STATUS_CONNECTED) {
                    ESP_LOGW(TAG, "Already connected, ignoring timeout failure event");
                    break;
                }
                
                // CRITICAL: Ignore if already at or beyond max retries (prevents double-cleanup)
                if (g_bt_state.retry_count >= MAX_BT_RETRIES) {
                    ESP_LOGW(TAG, "Already at max retries (%d), ignoring failure event", g_bt_state.retry_count);
                    break;
                }
                
                // Notify coexistence manager
                extern void s3_bt_handle_connection_failure(void);
                s3_bt_handle_connection_failure();
                
                g_bt_state.retry_count++;
                ESP_LOGI(TAG, "Attempt %d/%d failed", g_bt_state.retry_count, MAX_BT_RETRIES);
                
                if (g_bt_state.retry_count >= MAX_BT_RETRIES) {
                    // All retries exhausted
                    send_command(BT_CMD_FINAL_CLEANUP, 0);
                    break;
                }
                
                // Schedule next retry
                if (g_bt_state.silent_mode) {
                    // Background retry - allow BT stack to recover before next attempt
                    start_timeout_timer(BT_STACK_RECOVER_SEC * 1000, BT_CMD_BACKGROUND_RETRY);
                } else {
                    // Normal retry - longer delay (keep showing SCANNING)
                    update_status_and_notify(BT_STATUS_SCANNING);
                    start_timeout_timer(5000, BT_CMD_RETRY);
                }
                break;
            }
            
            case BT_CMD_FINAL_CLEANUP: {
                // Final cleanup after max retries
                ESP_LOGI(TAG, "CMD_FINAL_CLEANUP: All retries exhausted");
                
                cancel_timeout_timer();
                esp_bt_gap_cancel_discovery();
                
                g_bt_state.retry_count = MAX_BT_RETRIES;
                g_bt_state.silent_mode = false;
                g_bt_state.abrupt_disconnect_mode = false;
                g_bt_state.deinit_in_progress = true;  // Block operations during cleanup
                ESP_LOGI(TAG, "Final cleanup: deinit flag set");
                
                // Set non-connectable to prevent auto-reconnection
                esp_bt_gap_set_scan_mode(ESP_BT_NON_CONNECTABLE, ESP_BT_NON_DISCOVERABLE);
                
                update_status_and_notify(BT_STATUS_FAILED);
                
                // Deinitialize BT in background
                BaseType_t result = xTaskCreate(
                    bt_manager_deinit_final_task,
                    "bt_deinit_final",
                    4096,
                    NULL,
                    5,
                    &g_bt_state.deinit_task_handle
                );
                
                if (result != pdPASS) {
                    ESP_LOGW(TAG, "Failed to create deinit task");
                }
                break;
            }
            
            case BT_CMD_DISCONNECT: {
                // User-initiated disconnect
                ESP_LOGI(TAG, "CMD_DISCONNECT: User requested disconnect");
                
                cancel_timeout_timer();
                esp_bt_gap_cancel_discovery();
                
                g_bt_state.user_initiated_disconnect = true;
                g_bt_state.retry_count = 0;
                g_bt_state.silent_mode = false;
                g_bt_state.abrupt_disconnect_mode = false;
                g_bt_state.deinit_in_progress = true;  // Block new operations during deinit
                ESP_LOGI(TAG, "Deinit flag set, blocking new BT operations");
                
                // Going directly to OFF (no separate DISCONNECTING status)
                
                // Deinitialize in background
                BaseType_t result = xTaskCreate(
                    bt_manager_deinit_task,
                    "bt_deinit",
                    4096,
                    NULL,
                    5,
                    &g_bt_state.deinit_task_handle
                );
                
                if (result != pdPASS) {
                    ESP_LOGE(TAG, "Failed to create deinit task");
                    bt_deinit_a2dp_source();
                    update_status_and_notify(BT_STATUS_OFF);
                }
                break;
            }
        }
    }
}

// --- Event Handler (called from BT stack callbacks) ---

void manager_internal_event_handler(bt_app_event_t event) {
    switch (event) {
        case BT_APP_EVENT_CONNECTION_SUCCESS:
            send_command(BT_CMD_CONNECTION_SUCCESS, 0);
            break;
            
        case BT_APP_EVENT_DISCONNECTED:
            // Ignore if deinit is in progress (prevents duplicate status updates)
            if (g_bt_state.deinit_in_progress) {
                ESP_LOGI(TAG, "Ignoring DISCONNECTED event (deinit in progress)");
                break;
            }
            
            // Ignore if in special flows
            if (g_bt_state.abrupt_disconnect_mode || g_bt_state.silent_mode) {
                ESP_LOGI(TAG, "Ignoring DISCONNECTED event (special flow active)");
                break;
            }
            
            // Ignore if we're in scanning/retry flow (not actually connected)
            if (g_bt_state.current_status == BT_STATUS_SCANNING || g_bt_state.current_status == BT_STATUS_FAILED) {
                ESP_LOGI(TAG, "Ignoring DISCONNECTED event (scanning/retry in progress)");
                break;
            }
            
            // Check if it was unexpected
            if (g_bt_state.current_status == BT_STATUS_CONNECTED && !g_bt_state.user_initiated_disconnect) {
                ESP_LOGI(TAG, "Unexpected disconnect - treating as ABRUPT");
                send_command(BT_CMD_ABRUPT_DISCONNECT, 0);
            } else {
                // Normal disconnect
                g_bt_state.user_initiated_disconnect = false;
                update_status_and_notify(BT_STATUS_OFF);
            }
            break;
            
        case BT_APP_EVENT_ABRUPT_DISCONNECTION:
            // Ignore if already in retry mode (prevent duplicate abrupt disconnect detection)
            if (g_bt_state.abrupt_disconnect_mode || g_bt_state.silent_mode) {
                ESP_LOGI(TAG, "Ignoring ABRUPT_DISCONNECTION event (already retrying)");
                break;
            }
            send_command(BT_CMD_ABRUPT_DISCONNECT, 0);
            break;
            
        case BT_APP_EVENT_SCAN_FINISHED_NOT_FOUND:
        case BT_APP_EVENT_CONNECTION_FAILED:
        case BT_APP_EVENT_CONNECTION_TIMEOUT:
            // Ignore failures during abrupt disconnect flow (handled by timeout)
            if (g_bt_state.abrupt_disconnect_mode) {
                ESP_LOGI(TAG, "Ignoring failure during abrupt disconnect flow");
                break;
            }
            
            send_command(BT_CMD_CONNECTION_FAILED, 0);
            break;
            
        default:
            ESP_LOGW(TAG, "Unhandled event: %d", event);
            break;
    }
}

// --- Public API ---

void bt_manager_init(bt_manager_event_cb_t callback) {
    ESP_LOGI(TAG, "Initializing BT Manager V2");
    
    g_bt_state.app_callback = callback;
    g_bt_state.current_status = BT_STATUS_OFF;
    
    // Create command queue
    g_bt_state.cmd_queue = xQueueCreate(BT_CMD_QUEUE_SIZE, sizeof(bt_cmd_t));
    if (!g_bt_state.cmd_queue) {
        ESP_LOGE(TAG, "Failed to create command queue!");
        return;
    }
    
    // Create connection task
    BaseType_t result = xTaskCreate(
        bt_connection_task,
        "bt_conn",
        8192,  // Larger stack for BT operations
        NULL,
        10,    // High priority
        &g_bt_state.task_handle
    );
    
    if (result != pdPASS) {
        ESP_LOGE(TAG, "Failed to create connection task!");
        return;
    }
    
    // Register callback with BT stack
    extern void bt_register_app_callback(void (*cb)(bt_app_event_t));
    bt_register_app_callback(manager_internal_event_handler);
    
    ESP_LOGI(TAG, "BT Manager V2 initialized successfully");
}

void bt_manager_connect(void) {
    ESP_LOGI(TAG, "Connect requested");
    send_command(BT_CMD_CONNECT, 0);
}

void bt_manager_disconnect(void) {
    ESP_LOGI(TAG, "Disconnect requested");
    send_command(BT_CMD_DISCONNECT, 0);
}

void bt_manager_mark_disconnection_as_user_initiated(void) {
    g_bt_state.user_initiated_disconnect = true;
}

bt_manager_status_t bt_manager_get_status(void) {
    return g_bt_state.current_status;
}

// --- Deinit Tasks (kept from original) ---

static void bt_manager_deinit_task(void *param) {
    ESP_LOGI(TAG, "Deinit task started");
    
    esp_err_t result = bt_deinit_a2dp_source();
    if (result != ESP_OK) {
        ESP_LOGW(TAG, "BT deinit had issues: %s", esp_err_to_name(result));
    }
    
    ESP_LOGI(TAG, "Clearing bonded devices after user disconnect");
    clear_all_bonded_devices();
    
    update_status_and_notify(BT_STATUS_OFF);
    
    // Clear deinit flag AFTER sending final status (prevents race)
    g_bt_state.deinit_in_progress = false;
    ESP_LOGI(TAG, "Deinit completed, BT operations now allowed");
    
    g_bt_state.deinit_task_handle = NULL;
    vTaskDelete(NULL);
}

static void bt_manager_deinit_final_task(void *param) {
    ESP_LOGI(TAG, "Final deinit task started");
    
    esp_err_t result = bt_deinit_a2dp_source();
    if (result != ESP_OK) {
        ESP_LOGW(TAG, "BT final deinit had issues: %s", esp_err_to_name(result));
    }
    
    ESP_LOGI(TAG, "Clearing bonded devices");
    clear_all_bonded_devices();
    
    // Clear deinit flag AFTER cleanup (prevents race)
    g_bt_state.deinit_in_progress = false;
    ESP_LOGI(TAG, "Final deinit completed, BT operations now allowed");
    
    // Don't change status - already set to FAILED_FINAL
    
    g_bt_state.deinit_task_handle = NULL;
    vTaskDelete(NULL);
}

static void clear_all_bonded_devices(void) {
    int dev_num = esp_bt_gap_get_bond_device_num();
    
    if (dev_num == 0) {
        ESP_LOGI(TAG, "No bonded devices to clear");
        return;
    }
    
    ESP_LOGI(TAG, "Found %d bonded devices, removing all...", dev_num);
    
    esp_bd_addr_t *dev_list = (esp_bd_addr_t *)malloc(sizeof(esp_bd_addr_t) * dev_num);
    if (dev_list == NULL) {
        ESP_LOGE(TAG, "Failed to allocate memory for device list");
        return;
    }
    
    esp_err_t ret = esp_bt_gap_get_bond_device_list(&dev_num, dev_list);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to get bond device list: %s", esp_err_to_name(ret));
        free(dev_list);
        return;
    }
    
    for (int i = 0; i < dev_num; i++) {
        ret = esp_bt_gap_remove_bond_device(dev_list[i]);
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "Failed to remove device %d: %s", i, esp_err_to_name(ret));
        } else {
            ESP_LOGI(TAG, "Removed bonded device %d", i);
        }
    }
    
    free(dev_list);
    ESP_LOGI(TAG, "All bonded devices cleared");
}