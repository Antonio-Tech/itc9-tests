/**
 * @file s3_bluetooth.c
 * @brief Unified S3 Bluetooth Manager with Coexistence Support
 *
 * DMA OPTIMIZATION: This unified component prevents BLE advertising and
 * Bluetooth Classic scanning conflicts, reducing DMA memory pressure and
 * audio streaming issues when both protocols are active.
 */

#include "s3_bluetooth.h"
#include "s3_definitions.h"  // For S3ER_STOP_BLE_FOR_A2DP
#include "WiFi.h"  // For WiFi auto-disconnect when BT starts
#include "esp_log.h"
#include "esp_bt.h"
#include "esp_bt_main.h"
#include "esp_gap_ble_api.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/timers.h"
#include "esp_heap_caps.h"

static const char *TAG = "S3_BLUETOOTH";

/* =================== API FUNCTIONS =================== */
static const uint32_t LONGER_TIMEOUT_MS = 60000;   // 1 minute - for PLAY_SCREEN or first connection
static const uint32_t SHORTER_TIMEOUT_MS = 30000;  // 30 seconds - for other screens

/* =================== FORWARD DECLARATIONS =================== */
static void bt_timeout_timer_callback(TimerHandle_t xTimer);
static void bt_classic_state_callback(bool bt_scanning);

/* =================== COEXISTENCE STATE MANAGEMENT =================== */

typedef struct {
    s3_bt_coexistence_state_t state;
    bool ble_advertising_active;
    bool ble_connected;
    bool bt_scanning_active;
    bool bt_connected;
    bool bt_streaming_active;  // Track A2DP streaming state
    bool bt_connection_attempting;  // Track if BT is trying to connect
    int bt_retry_count;  // Track BT connection retry attempts for coexistence logic
    bool ble_paused_for_bt;
    TimerHandle_t ble_resume_timer;
    TimerHandle_t bt_timeout_timer;  // Timeout if BT doesn't start streaming
    SemaphoreHandle_t state_mutex;
    bool initialized;
} s3_bt_coexistence_context_t;

static s3_bt_coexistence_context_t s_coex_ctx = {
    .state = S3_BT_COEX_IDLE,
    .ble_advertising_active = false,
    .ble_connected = false,
    .bt_scanning_active = false,
    .bt_connected = false,
    .bt_streaming_active = false,
    .bt_connection_attempting = false,
    .bt_retry_count = 0,
    .ble_paused_for_bt = false,
    .ble_resume_timer = NULL,
    .bt_timeout_timer = NULL,
    .state_mutex = NULL,
    .initialized = false
};

/* =================== MEMORY EMERGENCY FUNCTIONS =================== */

/**
 * @brief Emergency cleanup of A2DP/BT Classic to free DMA memory while preserving BLE
 * @note This is used during WiFi initialization to free ~48KB DMA memory from A2DP
 */
void s3_bt_emergency_cleanup(void)
{
    ESP_LOGW(TAG, "[EMERGENCY] Performing targeted A2DP cleanup while preserving BLE");
    
    // 1. Force stop any A2DP streaming to free massive DMA memory (~48KB)
    if (s3_bt_classic_is_streaming()) {
        ESP_LOGW(TAG, "[EMERGENCY] Force-stopping A2DP streaming to free DMA memory");
        bt_a2dp_stop_media(); // Use available unified API
        vTaskDelay(pdMS_TO_TICKS(100)); // Allow A2DP stop to complete
    }
    
    // 2. Force disconnect BT Classic connections (preserving BLE)
    if (s3_bt_classic_is_connected()) {
        ESP_LOGW(TAG, "[EMERGENCY] Force-disconnecting BT Classic to free resources");
        bt_manager_disconnect(); // Use available unified API
        vTaskDelay(pdMS_TO_TICKS(150)); // Allow disconnect to complete
    }
    
    // 3. Reset BT timeout timer to clean state (if context is initialized)
    if (s_coex_ctx.initialized && s_coex_ctx.bt_timeout_timer != NULL) {
        xTimerStop(s_coex_ctx.bt_timeout_timer, 0);
        xTimerReset(s_coex_ctx.bt_timeout_timer, 0);
        ESP_LOGI(TAG, "[EMERGENCY] BT timeout timer reset");
    }
    
    // 4. Update coexistence state but keep BLE active
    ESP_LOGW(TAG, "[EMERGENCY] BLE remains active, only A2DP/BT Classic cleaned up");
    
    ESP_LOGI(TAG, "[EMERGENCY] A2DP emergency cleanup complete - BLE preserved");
}

/* =================== COEXISTENCE LOGIC =================== */

static void update_coexistence_state(void) {
    if (!s_coex_ctx.initialized || xSemaphoreTake(s_coex_ctx.state_mutex, portMAX_DELAY) != pdTRUE) {
        return;
    }
    
    // Rate limiting during active streaming to reduce CPU overhead
    static TickType_t last_update_time = 0;
    TickType_t current_time = xTaskGetTickCount();
    
    // If streaming is active, limit updates to max every 100ms (reduce CPU load)
    if (s_coex_ctx.bt_streaming_active) {
        TickType_t time_since_last = current_time - last_update_time;
        if (time_since_last < pdMS_TO_TICKS(100)) {
            xSemaphoreGive(s_coex_ctx.state_mutex);
            return; // Skip this update to reduce CPU overhead during streaming
        }
    }

    // Get real-time status from all components
    bool bt_connected = s3_bt_classic_is_connected();
    bool bt_a2dp_session = s3_bt_classic_is_streaming();  // A2DP session active
    bool ble_connected = s3_ble_manager_is_connected();

    // Real streaming state: BT connected AND audio player actually playing
    extern bool is_audio_playing(void);  // From audio_player.h
    bool audio_is_playing = is_audio_playing();
    bool bt_streaming = bt_connected && bt_a2dp_session && audio_is_playing;
    
    // Detect state transitions for notifications
    bool bt_streaming_started = (!s_coex_ctx.bt_streaming_active && bt_streaming);
    bool bt_streaming_stopped = (s_coex_ctx.bt_streaming_active && !bt_streaming);
    
    // DEBUG: Log all state variables to identify timing issues (reduced frequency during streaming)
    static int debug_counter = 0;
    bool should_log = false;
    
    if (bt_streaming) {
        // During active streaming, log much less frequently to reduce CPU overhead
        should_log = ((++debug_counter % 100) == 1) || bt_streaming_started || bt_streaming_stopped;
    } else {
        // During non-streaming, log more frequently for debugging
        should_log = ((++debug_counter % 20) == 1) || bt_streaming_started || bt_streaming_stopped;
    }
    
    if (should_log) {
        ESP_LOGI(TAG, "[COEX_DEBUG] bt_conn:%d a2dp_sess:%d audio_play:%d -> bt_stream:%d | ble_conn:%d", 
                 bt_connected, bt_a2dp_session, audio_is_playing, bt_streaming, ble_connected);
    }

    // Update cached values
    s_coex_ctx.bt_connected = bt_connected;
    s_coex_ctx.bt_streaming_active = bt_streaming;
    s_coex_ctx.ble_connected = ble_connected;

    // ================= 5-STAGE COEXISTENCE LOGIC =================

    // STAGE 1: Handled in bt_classic_state_callback when scan starts

    // STAGE 2: BT connection established while BLE connected -> Try to coexist, send attention if needed
    bool bt_just_connected = (!s_coex_ctx.bt_connected && bt_connected);

    // Reset retry count on any successful BT connection
    if (bt_just_connected) {
        s_coex_ctx.bt_retry_count = 0;
        ESP_LOGI(TAG, "BT connection established - reset retry count");
    }

    if (bt_just_connected && ble_connected && !bt_streaming) {
        ESP_LOGW(TAG, "STAGE 2: BT connected while BLE connected - PRE-EMPTIVELY disconnecting BLE to prevent L2CAP conflicts");
        
        // Send S3ER_STOP_BLE_STREAM_A2DP before disconnecting (maintaining proper message sequence)
        gPixseeStatus = S3ER_STOP_BLE_STREAM_A2DP;
        ESP_LOGI(TAG, "Sent S3ER_STOP_BLE_STREAM_A2DP - pre-emptively preparing for potential A2DP streaming");
        
        // Give app brief time to process the notification
        vTaskDelay(pdMS_TO_TICKS(50));
        
        // Pre-emptively disconnect BLE to free L2CAP resources
        esp_err_t ret = s3_ble_manager_disconnect_client();
        if (ret == ESP_OK) {
            ESP_LOGI(TAG, "*** BLE client PRE-EMPTIVELY disconnected - L2CAP resources freed for A2DP ***");
            
            // Also stop advertising to prevent app reconnection attempts
            esp_err_t adv_ret = s3_ble_manager_stop_advertising();
            if (adv_ret == ESP_OK) {
                ESP_LOGI(TAG, "BLE advertising stopped - preventing reconnection during A2DP preparation");
            } else {
                ESP_LOGW(TAG, "Failed to stop BLE advertising: %s", esp_err_to_name(adv_ret));
            }
            
            // Allow L2CAP resources to be fully freed
            vTaskDelay(pdMS_TO_TICKS(100));
            ESP_LOGI(TAG, "L2CAP resource pre-emptive cleanup completed");
        } else {
            ESP_LOGE(TAG, "*** FAILED TO PRE-EMPTIVELY DISCONNECT BLE: %s ***", esp_err_to_name(ret));
        }

        // Start timeout timer in case streaming doesn't start
        if (s_coex_ctx.bt_timeout_timer != NULL) {
            xTimerChangePeriod(s_coex_ctx.bt_timeout_timer, pdMS_TO_TICKS(LONGER_TIMEOUT_MS), 0);
            xTimerStart(s_coex_ctx.bt_timeout_timer, 0);
            ESP_LOGI(TAG, "Started %lums timeout timer for BT streaming (BLE pre-disconnected)", LONGER_TIMEOUT_MS);
        }
    }

    // STAGE 3: A2DP streaming starts -> Stop timeout and disconnect BLE if still connected
    if (bt_streaming_started) {
        ESP_LOGW(TAG, "*** STAGE 3: A2DP STREAMING STARTED - CRITICAL L2CAP CONFLICT CHECK ***");
        
        // Stop and reset timeout timer since streaming started successfully
        if (s_coex_ctx.bt_timeout_timer != NULL) {
            xTimerStop(s_coex_ctx.bt_timeout_timer, 0);
            // Reset timer to prevent leftover time from first connection timeout
            xTimerReset(s_coex_ctx.bt_timeout_timer, 0);
            ESP_LOGI(TAG, "Stopped and reset BT timeout timer - streaming started successfully");
        }

        if (ble_connected) {
            ESP_LOGE(TAG, "*** CRITICAL: BLE STILL CONNECTED DURING A2DP STREAMING - L2CAP CONGESTION WILL OCCUR! ***");
            ESP_LOGW(TAG, "STAGE 3: A2DP streaming started while BLE connected - L2CAP conflict imminent!");
            gPixseeStatus = S3ER_STOP_BLE_STREAM_A2DP;
            ESP_LOGI(TAG, "Sent S3ER_STOP_BLE_STREAM_A2DP - disconnecting BLE for A2DP streaming");

            // EMERGENCY: Disconnect BLE immediately without delay to prevent L2CAP flood
            ESP_LOGE(TAG, "*** EMERGENCY BLE DISCONNECT - NO DELAY TO PREVENT L2CAP FLOOD ***");
            esp_err_t ret = s3_ble_manager_disconnect_client();
            if (ret == ESP_OK) {
                ESP_LOGI(TAG, "*** BLE client EMERGENCY disconnected to prioritize A2DP streaming ***");

                // Also stop advertising to prevent app reconnection attempts
                esp_err_t adv_ret = s3_ble_manager_stop_advertising();
                if (adv_ret == ESP_OK) {
                    ESP_LOGI(TAG, "BLE advertising stopped - preventing app reconnection during streaming");
                } else {
                    ESP_LOGW(TAG, "Failed to stop BLE advertising: %s", esp_err_to_name(adv_ret));
                }

                // Allow L2CAP resources to be freed before A2DP streaming intensifies
                vTaskDelay(pdMS_TO_TICKS(50));
                ESP_LOGI(TAG, "L2CAP resource cleanup delay completed");
            } else {
                ESP_LOGE(TAG, "*** CRITICAL: FAILED TO EMERGENCY DISCONNECT BLE: %s ***", esp_err_to_name(ret));
            }
        } else {
            ESP_LOGI(TAG, "*** GOOD: BLE already disconnected before A2DP streaming - no L2CAP conflict expected ***");
        }
    }

    // STAGE 4: A2DP streaming stops -> Start idle timeout (check timeout values)
    if (bt_streaming_stopped && bt_connected) {
        ESP_LOGI(TAG, "STAGE 4: A2DP streaming stopped - starting idle timeout");

        // Start timeout timer - use longer or shorter timeout based on current screen
        if (s_coex_ctx.bt_timeout_timer != NULL) {
            extern int get_current_screen(void);
            uint32_t timeout_ms = (get_current_screen() == PLAY_SCREEN) ? LONGER_TIMEOUT_MS : SHORTER_TIMEOUT_MS;

            xTimerChangePeriod(s_coex_ctx.bt_timeout_timer, pdMS_TO_TICKS(timeout_ms), 0);
            xTimerStart(s_coex_ctx.bt_timeout_timer, 0);
            ESP_LOGI(TAG, "Started %lums A2DP idle timeout (current screen: %d)", timeout_ms, get_current_screen());
        } else {
            ESP_LOGE(TAG, "Failed to start A2DP idle timeout - timer not available");
        }
    }

    // STAGE 5: BT disconnects completely -> Resume full BLE operations
    if (!bt_connected && (s_coex_ctx.bt_connected || s_coex_ctx.bt_streaming_active)) {
        ESP_LOGI(TAG, "STAGE 5: BT disconnected completely - resuming full BLE operations");

        // Only send notification if BLE was actually affected (i.e., there was a previous connection)
        if (s_coex_ctx.ble_connected || s_coex_ctx.bt_streaming_active) {
            gPixseeStatus = S3ER_RESUME_BLE_STOP_A2DP;
            ESP_LOGI(TAG, "Sent S3ER_RESUME_BLE_STOP_A2DP - BT fully disconnected");
        }

        esp_err_t ret = s3_ble_manager_start_advertising();
        if (ret == ESP_OK) {
            ESP_LOGI(TAG, "BLE advertising resumed after BT disconnection");
        } else {
            ESP_LOGW(TAG, "Failed to resume BLE advertising: %s", esp_err_to_name(ret));
        }
    }

    // Update state enum for monitoring
    s3_bt_coexistence_state_t new_state = S3_BT_COEX_IDLE;
    if (bt_streaming && ble_connected) {
        new_state = S3_BT_COEX_CONFLICT;  // This should be prevented by stage 3
    } else if (bt_streaming) {
        new_state = S3_BT_COEX_BT_CONNECTED;  // Streaming takes priority
    } else if (bt_connected && ble_connected) {
        new_state = S3_BT_COEX_BOTH_CONNECTED;  // Dual connection coexistence
    } else if (bt_connected) {
        new_state = S3_BT_COEX_BT_CONNECTED;
    } else if (ble_connected) {
        new_state = S3_BT_COEX_BLE_CONNECTED;
    } else if (s_coex_ctx.bt_scanning_active && s_coex_ctx.ble_advertising_active) {
        new_state = S3_BT_COEX_CONFLICT;  // GAP operation conflict
    } else if (s_coex_ctx.bt_scanning_active) {
        new_state = S3_BT_COEX_BT_SCANNING;
    } else if (s_coex_ctx.ble_advertising_active) {
        new_state = S3_BT_COEX_BLE_ADVERTISING;
    }

    if (new_state != s_coex_ctx.state) {
        ESP_LOGI(TAG, "Coexistence state: %d -> %d", s_coex_ctx.state, new_state);
        s_coex_ctx.state = new_state;
    }
    
    // Update the last update time for rate limiting
    last_update_time = current_time;

    xSemaphoreGive(s_coex_ctx.state_mutex);
}

esp_err_t s3_bt_resolve_coexistence_conflict(void) {
    ESP_LOGI(TAG, "Resolving coexistence conflict...");

    // CONFLICT TYPE 1: BLE connected + A2DP streaming = L2CAP congestion
    if (s_coex_ctx.bt_connected && s_coex_ctx.ble_connected) {
        ESP_LOGW(TAG, "CRITICAL: BLE connection during A2DP streaming causes L2CAP congestion!");

        // Send notification to app before disconnecting
        gPixseeStatus = S3ER_STOP_BLE_STREAM_A2DP;
        ESP_LOGI(TAG, "Sent S3ER_STOP_BLE_FOR_A2DP notification to app");

        // Force disconnect BLE to free L2CAP resources
        esp_err_t ret = s3_ble_manager_disconnect_client();
        if (ret == ESP_OK) {
            ESP_LOGI(TAG, "BLE client disconnected to prioritize A2DP streaming");
        } else {
            ESP_LOGE(TAG, "Failed to disconnect BLE client: %s", esp_err_to_name(ret));
        }
        return ret;
    }

    // CONFLICT TYPE 2: BLE advertising + BT scanning = GAP operation conflict
    if (s_coex_ctx.bt_scanning_active && s_coex_ctx.ble_advertising_active) {
        ESP_LOGI(TAG, "Pausing BLE advertising during BT scan to prevent GAP conflicts");
        esp_err_t ret = s3_ble_manager_stop_advertising();
        if (ret == ESP_OK) {
            ESP_LOGI(TAG, "BLE advertising paused during BT scan");
        } else {
            ESP_LOGE(TAG, "Failed to stop BLE advertising: %s", esp_err_to_name(ret));
        }
        return ret;
    }

    ESP_LOGW(TAG, "No active coexistence conflict to resolve");
    return ESP_OK;
}

/**
 * @brief Handle L2CAP resource allocation failures and implement recovery
 */
esp_err_t s3_bt_handle_l2cap_failure(void) {
    if (!s_coex_ctx.initialized) return ESP_ERR_INVALID_STATE;

    ESP_LOGW(TAG, "L2CAP resource allocation failure detected - implementing recovery strategy");

    // If BT Classic is connected but not streaming, temporarily disconnect it
    if (s_coex_ctx.bt_connected && !s_coex_ctx.bt_streaming_active) {
        ESP_LOGI(TAG, "Temporarily disconnecting idle BT connection to free L2CAP resources");
        // Note: bt_stop_a2dp_source() will trigger disconnection
        esp_err_t ret = bt_stop_a2dp_source();
        if (ret == ESP_OK) {
            // Set a timer to restart BT scanning after a delay
            if (s_coex_ctx.bt_timeout_timer == NULL) {
                s_coex_ctx.bt_timeout_timer = xTimerCreate(
                    "bt_l2cap_recovery",
                    pdMS_TO_TICKS(2000),  // 2 second recovery delay
                    pdFALSE,
                    NULL,
                    bt_timeout_timer_callback
                );
            }
            if (s_coex_ctx.bt_timeout_timer != NULL) {
                xTimerStart(s_coex_ctx.bt_timeout_timer, 0);
                ESP_LOGI(TAG, "BT recovery timer started - will retry BT connection in 2 seconds");
            }
        }
        return ret;
    }

    // If no active BT connection, try restarting BLE advertising after a delay
    ESP_LOGI(TAG, "Retrying BLE advertising after L2CAP resource cleanup delay");
    vTaskDelay(pdMS_TO_TICKS(1000));  // 1 second delay for resource cleanup

    return s3_ble_manager_start_advertising();
}

/**
 * @brief Initialize BT Classic when user first accesses BT menu
 */
esp_err_t s3_bluetooth_init_bt_classic(void) {
    if (!s_coex_ctx.initialized) {
        ESP_LOGE(TAG, "S3 Bluetooth not initialized - call s3_bluetooth_init() first");
        return ESP_ERR_INVALID_STATE;
    }

    // Check if BT Classic is already initialized
    if (bt_is_initialized()) {
        ESP_LOGI(TAG, "BT Classic already initialized");
        return ESP_OK;
    }

    ESP_LOGI(TAG, "Initializing BT Classic for user menu access");

    // Initialize BT Classic manager (reuses existing controller)
    esp_err_t ret = s3_bt_classic_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize BT Classic manager: %s", esp_err_to_name(ret));
        return ret;
    }

    // Register BT Classic coexistence callback
    s3_bt_classic_set_coexistence_callback(bt_classic_state_callback);

    ESP_LOGI(TAG, "BT Classic initialized successfully for user access");
    return ESP_OK;
}

static void ble_state_callback(bool ble_active) {
    s_coex_ctx.ble_advertising_active = ble_active;
    ESP_LOGD(TAG, "BLE advertising state: %s", ble_active ? "ACTIVE" : "STOPPED");
    update_coexistence_state();
}

static void bt_classic_state_callback(bool bt_scanning) {
    bool scan_starting = (!s_coex_ctx.bt_scanning_active && bt_scanning);
    s_coex_ctx.bt_scanning_active = bt_scanning;
    ESP_LOGD(TAG, "BT Classic scanning state: %s", bt_scanning ? "ACTIVE" : "STOPPED");

    // STAGE 1: BT scan starts while BLE connected (0x47 already sent when user initiated connection)
    if (scan_starting && s_coex_ctx.ble_connected) {
        ESP_LOGD(TAG, "STAGE 1: BT scan started while BLE connected (0x47 already sent)");
    }

    // Auto-resume BLE advertising when BT scanning stops (only if not connected to BT)
    if (!bt_scanning && !s_coex_ctx.bt_connected) {
        ESP_LOGI(TAG, "BT scanning stopped - resuming BLE advertising");
        esp_err_t ret = s3_ble_manager_start_advertising();
        if (ret == ESP_OK) {
            ESP_LOGI(TAG, "BLE advertising resumed after BT scan completion");
        } else {
            ESP_LOGW(TAG, "Failed to resume BLE advertising: %s", esp_err_to_name(ret));
        }
    }
    update_coexistence_state();
}

static void ble_resume_timer_callback(TimerHandle_t xTimer) {
    ESP_LOGI(TAG, "BLE resume timer expired, resuming BLE advertising");
    s3_bt_resume_ble_operations();
}

static void bt_timeout_timer_callback(TimerHandle_t xTimer) {
    ESP_LOGW(TAG, "BT timeout expired - checking if BT is still idle");

    s_coex_ctx.bt_connection_attempting = false;

    // Only resume BLE if BT is NOT streaming AND handle L2CAP conflicts
    if (!s_coex_ctx.bt_streaming_active) {
        if (s_coex_ctx.bt_connected) {
            // BT is connected but idle - disconnect it to avoid L2CAP conflicts with BLE
            ESP_LOGI(TAG, "BT is connected but idle for timeout period - disconnecting to allow BLE resume");

            gPixseeStatus = S3ER_RESUME_BLE_STOP_A2DP;
            ESP_LOGI(TAG, "Sent S3ER_RESUME_BLE_STOP_A2DP - BT idle timeout, disconnecting BT");

            // Mark as user-initiated to prevent automatic reconnection
            bt_manager_mark_disconnection_as_user_initiated();

            // Disconnect idle BT connection to free L2CAP resources
            esp_err_t bt_ret = bt_stop_a2dp_source();
            if (bt_ret == ESP_OK) {
                ESP_LOGI(TAG, "Idle BT connection stopped to free L2CAP resources for BLE");
            } else {
                ESP_LOGW(TAG, "Failed to stop idle BT connection: %s", esp_err_to_name(bt_ret));
            }
        } else {
            // BT is already disconnected - safe to resume BLE
            ESP_LOGI(TAG, "BT is disconnected - resuming BLE operations");

            gPixseeStatus = S3ER_RESUME_BLE_STOP_A2DP;
            ESP_LOGI(TAG, "Sent S3ER_RESUME_BLE_STOP_A2DP - BT idle timeout");

            esp_err_t ret = s3_ble_manager_start_advertising();
            if (ret == ESP_OK) {
                ESP_LOGI(TAG, "BLE advertising resumed after BT idle timeout");
            } else {
                ESP_LOGW(TAG, "Failed to resume BLE advertising: %s", esp_err_to_name(ret));
            }
        }
    } else {
        ESP_LOGI(TAG, "BT is currently streaming - timeout cancelled, not resuming BLE");
    }

    // Clear timer reference
    s_coex_ctx.bt_timeout_timer = NULL;
}

// Function to handle BT connection failures - called from BT manager when connection fails
void s3_bt_handle_connection_failure(void) {
    if (!s_coex_ctx.initialized) return;

    ESP_LOGW(TAG, "BT connection failed - L2CAP allocation or connection error detected");

    if (s_coex_ctx.ble_connected && s_coex_ctx.bt_connection_attempting) {
        s_coex_ctx.bt_retry_count++;
        ESP_LOGI(TAG, "BT connection attempt %d failed while BLE connected", s_coex_ctx.bt_retry_count);

        if (s_coex_ctx.bt_retry_count == 1) {
            // First failure: Disconnect BLE and stop advertising (0x46)
            ESP_LOGW(TAG, "First BT attempt failed - disconnecting BLE to free L2CAP resources");

            gPixseeStatus = S3ER_STOP_BLE_STREAM_A2DP;
            ESP_LOGI(TAG, "Sent S3ER_STOP_BLE_STREAM_A2DP (0x46) - freeing L2CAP resources for BT retry");

            // Give app time to receive and process the notification before disconnecting
            vTaskDelay(pdMS_TO_TICKS(S3ER_BLE_TASK_MS));  // 500ms delay for app notification processing

            esp_err_t ret = s3_ble_manager_disconnect_client();
            if (ret == ESP_OK) {
                ESP_LOGI(TAG, "BLE client disconnected to free L2CAP resources for BT connection");
            } else {
                ESP_LOGE(TAG, "Failed to disconnect BLE client: %s", esp_err_to_name(ret));
            }

            // Also stop advertising to prevent app reconnection attempts
            esp_err_t adv_ret = s3_ble_manager_stop_advertising();
            if (adv_ret == ESP_OK) {
                ESP_LOGI(TAG, "BLE advertising stopped - preventing app reconnection during BT retry");
            } else {
                ESP_LOGW(TAG, "Failed to stop BLE advertising: %s", esp_err_to_name(adv_ret));
            }
        } else {
            // Second+ failure: Nothing more to do with BLE
            ESP_LOGI(TAG, "Additional BT failures - BLE already disconnected, no further action needed");
        }
    }

    s_coex_ctx.bt_connection_attempting = false;
}

/* =================== PUBLIC COEXISTENCE APIs =================== */

esp_err_t s3_bluetooth_init(void) {
    ESP_LOGI(TAG, "Initializing S3 Bluetooth Coexistence Manager (BLE only at boot)");

    if (s_coex_ctx.initialized) {
        ESP_LOGW(TAG, "S3 Bluetooth already initialized");
        return ESP_OK;
    }

    // Create mutex for state management
    s_coex_ctx.state_mutex = xSemaphoreCreateMutex();
    if (s_coex_ctx.state_mutex == NULL) {
        ESP_LOGE(TAG, "Failed to create coexistence mutex");
        return ESP_ERR_NO_MEM;
    }

    // Create BLE resume timer
    s_coex_ctx.ble_resume_timer = xTimerCreate(
        "ble_resume_timer",
        pdMS_TO_TICKS(1000),  // 1 second default
        pdFALSE,              // One-shot timer
        NULL,
        ble_resume_timer_callback
    );

    if (s_coex_ctx.ble_resume_timer == NULL) {
        ESP_LOGE(TAG, "Failed to create BLE resume timer");
        return ESP_ERR_NO_MEM;
    }

    // Create BT timeout timer - check timeout values
    s_coex_ctx.bt_timeout_timer = xTimerCreate(
        "bt_timeout_timer",
        pdMS_TO_TICKS(SHORTER_TIMEOUT_MS),  // Default shorter timeout (will be changed as needed)
        pdFALSE,               // One-shot timer
        NULL,
        bt_timeout_timer_callback
    );

    if (s_coex_ctx.bt_timeout_timer == NULL) {
        ESP_LOGE(TAG, "Failed to create BT timeout timer");
        return ESP_ERR_NO_MEM;
    }

    // Initialize BLE manager first (it initializes the BT controller)
    esp_err_t ret = s3_ble_manager_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize BLE manager: %s", esp_err_to_name(ret));
        return ret;
    }

    // Boot: BT Classic initialization is deferred until user accesses BT menu
    // This prevents auto-reconnection to speakers during boot
    ESP_LOGI(TAG, "BT Classic initialization deferred - will initialize when user accesses BT menu");

    // Register BLE coexistence callback
    s3_ble_manager_set_coexistence_callback(ble_state_callback);
    // BT Classic callback will be registered when BT is initialized by user

    s_coex_ctx.initialized = true;
    ESP_LOGI(TAG, "S3 Bluetooth Coexistence Manager initialized successfully");

    // Update initial connection states
    s_coex_ctx.ble_connected = s3_ble_manager_is_connected();
    s_coex_ctx.bt_connected = s3_bt_classic_is_connected();
    s_coex_ctx.bt_streaming_active = s3_bt_classic_is_streaming();
    update_coexistence_state();

    return ESP_OK;
}

s3_bt_coexistence_state_t s3_bt_get_coexistence_state(void) {
    return s_coex_ctx.state;
}

esp_err_t s3_bt_pause_ble_for_bt_operation(uint32_t pause_duration_ms) {
    ESP_LOGI(TAG, "Pausing BLE for %lu ms to allow BT Classic operations", pause_duration_ms);

    if (s_coex_ctx.ble_advertising_active) {
        esp_err_t ret = s3_ble_manager_stop_advertising();
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to stop BLE advertising: %s", esp_err_to_name(ret));
            return ret;
        }
        s_coex_ctx.ble_paused_for_bt = true;
    }

    // Set resume timer if duration specified
    if (pause_duration_ms > 0) {
        xTimerChangePeriod(s_coex_ctx.ble_resume_timer, pdMS_TO_TICKS(pause_duration_ms), 0);
        xTimerStart(s_coex_ctx.ble_resume_timer, 0);
    }

    return ESP_OK;
}

esp_err_t s3_bt_resume_ble_operations(void) {
    ESP_LOGI(TAG, "Resuming BLE operations");

    if (s_coex_ctx.ble_paused_for_bt) {
        esp_err_t ret = s3_ble_manager_start_advertising();
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to resume BLE advertising: %s", esp_err_to_name(ret));
            return ret;
        }
        s_coex_ctx.ble_paused_for_bt = false;
    }

    // Stop resume timer if active
    xTimerStop(s_coex_ctx.ble_resume_timer, 0);

    return ESP_OK;
}

bool s3_bt_would_operations_conflict(bool bt_scanning, bool ble_advertising) {
    return bt_scanning && ble_advertising;  // GAP operations conflict
}


void s3_bt_get_dma_usage(size_t *total_dma_bt, size_t *free_dma_bt) {
    if (total_dma_bt) {
        *total_dma_bt = heap_caps_get_total_size(MALLOC_CAP_DMA);
    }
    if (free_dma_bt) {
        *free_dma_bt = heap_caps_get_free_size(MALLOC_CAP_DMA);
    }
}

/**
 * @brief Lightweight performance monitoring for A2DP debugging
 * @note Use existing CLI commands instead: 'system', 'stat', 'tasks', 'memo'
 * @note This function just provides a quick summary, use CLI for detailed analysis
 */
void s3_bt_log_performance_stats(void) {
    // Use existing system memory monitoring to avoid complex logging
    extern void sys_memory_status(const char *tag, const char *msg);
    sys_memory_status("A2DP_PERF", "A2DP performance check - use 'system' CLI for details");
    
    // Quick summary without flooding logs
    UBaseType_t task_count = uxTaskGetNumberOfTasks();
    size_t min_free_heap = esp_get_minimum_free_heap_size();
    
    ESP_LOGI(TAG, "[A2DP_PERF] Quick summary: %d tasks | Min free heap: %u KB",
             task_count, (unsigned int)(min_free_heap/1024));
    ESP_LOGI(TAG, "[A2DP_PERF] For detailed analysis use CLI commands: 'system', 'stat', 'tasks', 'memo'");
}

esp_err_t s3_bt_set_dma_optimizations(bool enable) {
    ESP_LOGI(TAG, "DMA optimizations %s", enable ? "ENABLED" : "DISABLED");
    // DMA optimizations already implemented in the migrated components:
    // - Pre-allocated GATT response buffer
    // - Reduced MTU size (185 bytes)
    // - Optimized prepare buffer handling
    return ESP_OK;
}

void s3_bt_trigger_coexistence_update(void) {
    update_coexistence_state();
}

void s3_bt_mark_connection_attempt(void) {
    if (!s_coex_ctx.initialized) return;

    ESP_LOGI(TAG, "Marking BT connection attempt in progress");
    s_coex_ctx.bt_connection_attempting = true;
    // Reset retry count for new connection attempt sequence
    s_coex_ctx.bt_retry_count = 0;
    ESP_LOGI(TAG, "Reset retry count for new BT connection attempt");

    // Send 0x47 immediately when user starts BT scan if BLE is connected
    if (s_coex_ctx.ble_connected) {
        gPixseeStatus = S3ER_ATTENTION_BLE_SCAN_A2DP;
        ESP_LOGI(TAG, "User initiated BT scan while BLE connected - sent S3ER_ATTENTION_BLE_SCAN_A2DP (0x47)");
    }

    update_coexistence_state();
}

void s3_bt_handle_scan_no_devices(void) {
    if (!s_coex_ctx.initialized) return;

    ESP_LOGI(TAG, "BT scan completed with no devices found");

    // Send idle notification if BLE is connected (scan finished but no connection will be attempted)
    if (s_coex_ctx.ble_connected) {
        ESP_LOGW(TAG, "BT scan found no devices while BLE connected - sending idle notification");
        gPixseeStatus = S3ER_ATTENTION_BLE_IDLE_A2DP;
        ESP_LOGI(TAG, "Sent S3ER_ATTENTION_BLE_IDLE_A2DP - BT scan idle, BLE remains active");
    }

    // Clear connection attempt flag since no connection will be made
    s_coex_ctx.bt_connection_attempting = false;
}

/* =================== BACKWARD COMPATIBILITY WRAPPERS =================== */

// BLE APIs - direct pass-through to BLE manager
void ble_init_task(void *pvParameters) {
    s3_bluetooth_init();
    vTaskDelete(NULL);
}

// BT Classic scan wrapper with coexistence management
esp_err_t bt_scan_and_connect_to_strongest(uint8_t scan_duration_seconds) {
    // Auto-disconnect WiFi to free DMA RAM for BT operations
    if (is_wifi_connected()) {
        ESP_LOGW(TAG, "WiFi connected detected - auto-disconnecting to free DMA RAM for BT");
        memory_status();  // Show memory status before WiFi disconnect
        deinit_wifi_station();
        ESP_LOGI(TAG, "WiFi disconnected - DMA RAM freed for BT operations");
        memory_status();  // Show memory status after WiFi disconnect
    }
    
    // Check for conflicts before starting scan
    if (s3_bt_would_operations_conflict(true, s_coex_ctx.ble_advertising_active)) {
        ESP_LOGI(TAG, "BT scan would conflict with BLE, pausing BLE first");
        s3_bt_pause_ble_for_bt_operation(scan_duration_seconds * 1000 + 2000);  // Extra 2s buffer

        // Give BLE time to stop advertising before starting scan
        vTaskDelay(pdMS_TO_TICKS(100));
    }

    return s3_bt_classic_start_scan(scan_duration_seconds);
}

void s3_bt_clear_connection_attempt(void) {
    if (!s_coex_ctx.initialized) return;

    ESP_LOGI(TAG, "Clearing BT connection attempt flag");
    s_coex_ctx.bt_connection_attempting = false;
}

/* Additional backward compatibility wrappers can be added as needed */