/**
 * @file bt_a2dp_source.c
 * @author Igor Oliveira
 * @email igor.oliveira@venturus.org.br
 * @date 2025-06-24
 * @brief A2DP source implementation for Bluetooth streaming
 *
 * Contains the logic for initializing the Bluetooth stack, handling A2DP and
 * AVRCP callbacks, and managing connection to remote Bluetooth sink devices.
 */

#include "s3_bluetooth.h"

#include "esp_bt.h"
#include "esp_bt_device.h"
#include "esp_bt_main.h"
#include "esp_gap_bt_api.h"
#include "esp_a2dp_api.h"
#include "esp_avrc_api.h"
#include "esp_log.h"
#include <string.h>
#include <inttypes.h>
#include <ctype.h>
#include "freertos/semphr.h"
#include "audio_player.h"  // For is_audio_playing() check

#if !defined(CONFIG_BT_CLASSIC_ENABLED) || !defined(CONFIG_BT_A2DP_ENABLE)
#error "Bluetooth Classic and A2DP must be enabled in menuconfig"
#endif

#define TAG "S3_BT_CLASSIC"

/* Performance mode for optimized A2DP */
static bool s3_performance_mode = false;

/* Coexistence callback to notify main component about BT scanning and streaming state changes */
static void (*s_coexistence_callback)(bool bt_scanning) = NULL;
static bool coex_callback_enabled = true;  // Flag to disable callbacks during deinitialization

typedef struct {
  char name[ESP_BT_GAP_MAX_BDNAME_LEN + 1];
  esp_bd_addr_t bda;
  int8_t rssi;
  uint32_t cod;
} bt_discovered_device_t;

// Static variable to store the application's callback function pointer
static bt_app_event_callback_t s_app_event_cb = NULL;
static bool a2dp_connected = false;
static bool a2dp_streaming = false;  // Track A2DP streaming state for coexistence
static esp_bd_addr_t s_peer_bda;
static bool is_scanning = false;
static bool connection_in_progress = false;  // Fast flag set immediately when connection initiated
static bt_discovered_device_t strongest_device;
static bt_audio_stop_callback_t s_audio_stop_cb = NULL;
static SemaphoreHandle_t s_media_stop_sem = NULL;
static bool abrupt_disconnection_handled = false; // Flag to prevent double event handling
static bool a2dp_connection_pending = false;  // Flag to defer A2DP connection event during active audio

void bt_register_audio_stop_callback(bt_audio_stop_callback_t cb) {
    s_audio_stop_cb = cb;
}

/**
 * @brief Notify Bluetooth that audio playback has stopped
 * This triggers any deferred A2DP connection events to prevent crash
 */
void bt_notify_audio_stopped(void) {
    if (a2dp_connection_pending) {
        ESP_LOGI(TAG, "Audio stopped - triggering deferred A2DP connection event");
        a2dp_connection_pending = false;
        
        if (s_app_event_cb != NULL && a2dp_connected) {
            s_app_event_cb(BT_APP_EVENT_CONNECTION_SUCCESS);
        }
    }
}

/**
 * @brief Enable performance mode for A2DP (static optimization through sdkconfig)
 */
void bt_a2dp_set_performance_mode(bool enable) {
    s3_performance_mode = enable;
    ESP_LOGI(TAG, "A2DP performance mode %s (optimized via sdkconfig)", 
             enable ? "ENABLED" : "DISABLED");
}

// A2DP SBC codec performance optimizations are now configured at Bluedroid level
// in bta_av_co.c: 32kHz sample rate and reduced bitpool for ~27% CPU reduction

// Implementation of the registration function
void bt_register_app_callback(void (*cb)(bt_app_event_t)) {
  s_app_event_cb = cb;
  ESP_LOGI(TAG, "Callback registered. Pointer is %p", s_app_event_cb);
}

/**
 * @brief Ensures Bluetooth is initialized
 */
bool bt_is_initialized(void) {
  return (esp_bluedroid_get_status() == ESP_BLUEDROID_STATUS_ENABLED);
}

/**
 * @brief Ensures Bluetooth is initialized
 */
esp_err_t bt_ensure_initialized(void) {
  if (bt_is_initialized()) {
    return ESP_OK;
  }
  return bt_start_a2dp_source();
}

void bt_a2dp_start_media(void) {
  ESP_LOGI(TAG, "Requesting A2DP media start");
  esp_a2d_media_ctrl(ESP_A2D_MEDIA_CTRL_START);
}

void bt_a2dp_stop_media(void) {
  ESP_LOGI(TAG, "Requesting A2DP media stop");
  esp_a2d_media_ctrl(ESP_A2D_MEDIA_CTRL_SUSPEND);
}

// Helper function to parse device name from scan result properties
static void get_device_name_from_eir(esp_bt_gap_cb_param_t *scan_result,
                                     char *bdname, uint8_t *bdname_len) {
  if (scan_result == NULL || bdname == NULL || bdname_len == NULL) {
    return;
  }

  uint8_t *rmt_bdname = NULL;
  *bdname_len = 0;

  for (int i = 0; i < scan_result->disc_res.num_prop; i++) {
    if (scan_result->disc_res.prop[i].type == ESP_BT_GAP_DEV_PROP_BDNAME) {
      *bdname_len = scan_result->disc_res.prop[i].len;
      rmt_bdname = (uint8_t *)scan_result->disc_res.prop[i].val;
      break;
    }
  }

  if (rmt_bdname) {
    if (*bdname_len > ESP_BT_GAP_MAX_BDNAME_LEN) {
      *bdname_len = ESP_BT_GAP_MAX_BDNAME_LEN;
    }
    memcpy(bdname, rmt_bdname, *bdname_len);
    bdname[*bdname_len] = '\0';
  }
}

// Main GAP callback function to handle discovery results
static void bt_gap_cb(esp_bt_gap_cb_event_t event,
                      esp_bt_gap_cb_param_t *param) {
  switch (event) {
  case ESP_BT_GAP_DISC_STATE_CHANGED_EVT: {
    if (param->disc_st_chg.state == ESP_BT_GAP_DISCOVERY_STARTED) {
      ESP_LOGI(TAG, "Scan started...");
      // Notify coexistence manager that scanning actually started (only if callbacks enabled)
      if (s_coexistence_callback && coex_callback_enabled) {
          s_coexistence_callback(true);
      }
    } else if (param->disc_st_chg.state == ESP_BT_GAP_DISCOVERY_STOPPED) {
      ESP_LOGI(TAG, "Scan stopped.");
      is_scanning = false;

      // Notify coexistence manager that scanning stopped (only if callbacks enabled)
      if (s_coexistence_callback && coex_callback_enabled) {
          s_coexistence_callback(false);
      }

      ESP_LOGI(TAG, "Checking callback pointer before notification. Pointer is %p",
               s_app_event_cb);

      if (s_app_event_cb == NULL) {
        ESP_LOGE(TAG, "Callback is NULL, cannot notify app or continue connection. Aborting.");
        return;
      }

      // Check if a strongest device was found during the scan
      if (strongest_device.rssi > -127) {

        // CRITICAL: Don't attempt connection if already connected or connection in progress
        if (a2dp_connected || connection_in_progress) {
          ESP_LOGI(TAG, "Scan finished but A2DP already connected/connecting - ignoring scan result");
        } else if (esp_bt_gap_is_valid_cod(strongest_device.cod) &&
                   (esp_bt_gap_get_cod_major_dev(strongest_device.cod) ==
                    ESP_BT_COD_MAJOR_DEV_AV)) {
          // If it's an audio device, attempt to connect
          ESP_LOGI(TAG, "Suitable audio device found: [%s]. Connecting...",
                   strongest_device.name);
          char bda_str[18];
          sprintf(bda_str, ESP_BD_ADDR_STR,
                  ESP_BD_ADDR_HEX(strongest_device.bda));
          bt_connect_to_device(bda_str);
        } else {
          // If it's not an audio device, treat it as "not found"
          ESP_LOGW(TAG,
                   "Strongest device found [%s], but it is not an audio device "
                   "(COD: 0x%" PRIx32 "). Ignoring.",
                   strongest_device.name, strongest_device.cod);
          
          // Important: notify callback that no suitable device was found
          if (s_app_event_cb != NULL) {
            s_app_event_cb(BT_APP_EVENT_SCAN_FINISHED_NOT_FOUND);
          }
        }
      } else {
        // This block runs if no BT devices were detected at all
        // CRITICAL: Don't report failure if connection already established/in-progress
        if (a2dp_connected) {
          ESP_LOGI(TAG, "Scan finished with no devices, but A2DP already connected - ignoring");
        } else {
          ESP_LOGW(TAG, "Scan finished, but no devices were found at all.");
          // Notify the manager that the scan failed to find anyone
          if (s_app_event_cb != NULL) {
            s_app_event_cb(BT_APP_EVENT_SCAN_FINISHED_NOT_FOUND);
          }
        }
      }
    }
      break;
    }

  case ESP_BT_GAP_DISC_RES_EVT: {
    esp_bt_gap_cb_param_t *scan_result = (esp_bt_gap_cb_param_t *)param;
    int8_t current_rssi = -127;
    uint32_t current_cod = 0;
    uint8_t name_len = 0;
    char device_name[ESP_BT_GAP_MAX_BDNAME_LEN + 1] = {0};

    // Parse properties from the scan result
    for (int i = 0; i < scan_result->disc_res.num_prop; i++) {
      if (scan_result->disc_res.prop[i].type == ESP_BT_GAP_DEV_PROP_RSSI) {
        current_rssi = *(int8_t *)scan_result->disc_res.prop[i].val;
      } else if (scan_result->disc_res.prop[i].type ==
                 ESP_BT_GAP_DEV_PROP_COD) {
        current_cod = *(uint32_t *)(scan_result->disc_res.prop[i].val);
      }
    }

    get_device_name_from_eir(scan_result, device_name, &name_len);

    ESP_LOGD(TAG, "Device found: " ESP_BD_ADDR_STR ", Name: '%s', RSSI: %d",
             ESP_BD_ADDR_HEX(scan_result->disc_res.bda), device_name,
             current_rssi);

    // Logic to find the strongest device
    if (current_rssi > strongest_device.rssi) {
      ESP_LOGW(TAG, "New strongest device! Name: %s, RSSI: %d, COD: 0x%" PRIx32,
         device_name, current_rssi, current_cod);
      strongest_device.rssi = current_rssi;
      strongest_device.cod = current_cod;
      memcpy(strongest_device.bda, scan_result->disc_res.bda,
             sizeof(esp_bd_addr_t));
      strncpy(strongest_device.name, device_name, ESP_BT_GAP_MAX_BDNAME_LEN);
    }
    break;
  }
  case ESP_BT_GAP_ACL_DISCONN_CMPL_STAT_EVT: {
    // ACL disconnection - detects abrupt disconnections (battery died, out of range, etc)
    // Only log as warning if we think we're still connected, otherwise it's expected cleanup
    if (a2dp_connected && memcmp(s_peer_bda, param->acl_disconn_cmpl_stat.bda, sizeof(esp_bd_addr_t)) == 0) {
      ESP_LOGW(TAG, "GAP ACL disconnection detected for device " ESP_BD_ADDR_STR ", reason: %d",
               ESP_BD_ADDR_HEX(param->acl_disconn_cmpl_stat.bda),
               param->acl_disconn_cmpl_stat.reason);
      ESP_LOGE(TAG, "ABRUPT DISCONNECTION DETECTED! A2DP layer missed it, forcing disconnection event");
      
      // Reset our internal state
      a2dp_connected = false;
      memset(s_peer_bda, 0, sizeof(esp_bd_addr_t));
      
      // Mark that abrupt disconnection was handled to prevent A2DP callback from interfering
      abrupt_disconnection_handled = true;
      
      // Notify the bt_manager of the abrupt disconnection
      // This will trigger timeout icon + 1 background reconnection attempt
      if (s_app_event_cb != NULL) {
        s_app_event_cb(BT_APP_EVENT_ABRUPT_DISCONNECTION);
      }
    }
    break;
  }
  case ESP_BT_GAP_AUTH_CMPL_EVT: {
    // Handle authentication completion events
    if (param->auth_cmpl.stat != ESP_BT_STATUS_SUCCESS) {
      ESP_LOGW(TAG, "Authentication failed with device " ESP_BD_ADDR_STR ", status: %d",
               ESP_BD_ADDR_HEX(param->auth_cmpl.bda), param->auth_cmpl.stat);
      
      // Clear connection_in_progress flag to allow future attempts
      connection_in_progress = false;
      ESP_LOGI(TAG, "Connection in progress flag cleared (auth failed)");
      
      // Immediately notify connection failure to trigger timeout flow
      if (s_app_event_cb != NULL) {
        s_app_event_cb(BT_APP_EVENT_CONNECTION_FAILED);
      }
    } else {
      ESP_LOGI(TAG, "Authentication successful with device " ESP_BD_ADDR_STR,
               ESP_BD_ADDR_HEX(param->auth_cmpl.bda));
    }
    break;
  }
  default:
    ESP_LOGD(TAG, "Unhandled GAP event: %d", event);
    break;
  }
}

/**
 * @brief Starts the device discovery process and automatically connects to the
 * strongest device found.
 *
 * @param scan_duration_seconds The duration in seconds to scan for devices.
 * @return esp_err_t ESP_OK if scan started, or an error code otherwise.
 */
static esp_err_t bt_scan_and_connect_to_strongest_internal(uint8_t scan_duration_seconds) {
  if (is_scanning) {
    ESP_LOGW(TAG, "Scan is already in progress.");
    return ESP_ERR_INVALID_STATE;
  }

  if (a2dp_connected || connection_in_progress) {
    ESP_LOGW(TAG, "Device is already connected or connection in progress. Disconnect first.");
    return ESP_ERR_INVALID_STATE;
  }

  // 1. Reset the previous "champion" device info
  ESP_LOGI(TAG, "Preparing for a new scan...");
  memset(&strongest_device, 0, sizeof(bt_discovered_device_t));
  strongest_device.rssi = -127; // Set to lowest possible RSSI

  // 2. Set the scanning flag
  is_scanning = true;

  // 3. Start discovery
  // The duration is in units of 1.28 seconds.
  uint8_t duration_in_units = (uint8_t)(scan_duration_seconds / 1.28);
  if (duration_in_units == 0) {
    duration_in_units = 1; // Minimum duration
  }
  ESP_LOGI(TAG, "Starting discovery for %d seconds (%d units)...",
           scan_duration_seconds, duration_in_units);
  esp_err_t ret = esp_bt_gap_start_discovery(ESP_BT_INQ_MODE_GENERAL_INQUIRY,
                                             duration_in_units, 0);

  if (ret != ESP_OK) {
    ESP_LOGE(TAG, "Failed to start discovery: %s", esp_err_to_name(ret));
    is_scanning = false; // Reset flag on failure
  }

  return ret;
}

/**
 * @brief A2DP event handler callback.
 *
 * This function is called by the stack on A2DP events such as connection,
 * disconnection, and streaming.
 *
 * @param event The type of A2DP event.
 * @param param Parameters associated with the event.
 */
static void bt_app_a2d_cb(esp_a2d_cb_event_t event, esp_a2d_cb_param_t *param) {
  ESP_LOGD(TAG, "A2DP event: %d", event);
  
  // CRITICAL: Check for NULL param to prevent crash during A2DP init failures
  if (param == NULL) {
    ESP_LOGE(TAG, "A2DP callback received NULL param for event %d - ignoring", event);
    return;
  }
  
  esp_a2d_cb_param_t *a2d = (esp_a2d_cb_param_t *)(param);

  switch (event) {
  case ESP_A2D_CONNECTION_STATE_EVT: {
    uint8_t *bda = a2d->conn_stat.remote_bda;
    ESP_LOGI(TAG,
             "A2DP connection state changed: %s, remote MAC: " ESP_BD_ADDR_STR,
             a2d->conn_stat.state == ESP_A2D_CONNECTION_STATE_CONNECTED
                 ? "CONNECTED"
                 : "DISCONNECTED",
             ESP_BD_ADDR_HEX(bda));

    if (a2d->conn_stat.state == ESP_A2D_CONNECTION_STATE_CONNECTED) {
      a2dp_connected = true;
      connection_in_progress = false;  // Clear flag on successful connection
      memcpy(s_peer_bda, a2d->conn_stat.remote_bda, sizeof(esp_bd_addr_t));
      
      // Check if audio is currently playing to prevent concurrent resource cleanup crash
      // This protects against race conditions between I2S pipeline and A2DP initialization
      if (is_audio_playing()) {
        ESP_LOGW(TAG, "A2DP connected while audio playing - marking connection pending");
        ESP_LOGW(TAG, "Connection event will be deferred until audio stops to prevent crash");
        a2dp_connection_pending = true;
      } else {
        // Safe to notify immediately - no active audio pipeline
        if (s_app_event_cb != NULL) {
          s_app_event_cb(BT_APP_EVENT_CONNECTION_SUCCESS);
        }
      }
      
      ESP_LOGI(TAG, "A2DP connection established, ready for audio stream.");

      // Notify coexistence manager about connection state change
      s3_bt_trigger_coexistence_update();

    } else if (a2d->conn_stat.state == ESP_A2D_CONNECTION_STATE_DISCONNECTED) {
      ESP_LOGW(TAG, "A2DP link is down.");
      a2dp_connected = false;
      connection_in_progress = false;  // Clear flag on disconnect
      a2dp_connection_pending = false;  // Clear pending flag on disconnect
      memset(s_peer_bda, 0, sizeof(esp_bd_addr_t));
      
      // Set device to non-connectable to prevent unwanted auto-reconnections
      // This only affects BT Classic, BLE advertising continues working
      esp_bt_gap_set_scan_mode(ESP_BT_NON_CONNECTABLE, ESP_BT_NON_DISCOVERABLE);
      ESP_LOGI(TAG, "Device set to non-connectable mode after disconnect");

      // Notify coexistence manager about connection state change
      s3_bt_trigger_coexistence_update();

      // Check if abrupt disconnection was already handled by GAP callback
      if (abrupt_disconnection_handled) {
        ESP_LOGI(TAG, "A2DP disconnection ignored - already handled as abrupt disconnection");
        abrupt_disconnection_handled = false; // Reset flag for next connection
      } else {
        // Normal disconnection (manual)
        if (s_app_event_cb != NULL) {
          s_app_event_cb(BT_APP_EVENT_DISCONNECTED);
        }
      }
    }
    break;
  }
  case ESP_A2D_AUDIO_STATE_EVT: {
    bool new_streaming_state = (a2d->audio_stat.state == ESP_A2D_AUDIO_STATE_STARTED);
    ESP_LOGI(TAG, "A2DP audio state: %s",
             new_streaming_state ? "STARTED" : "STOPPED/SUSPENDED");

    // Update streaming state and trigger coexistence update if changed
    if (a2dp_streaming != new_streaming_state) {
      a2dp_streaming = new_streaming_state;
      ESP_LOGI(TAG, "A2DP streaming state changed to: %s", a2dp_streaming ? "ACTIVE" : "INACTIVE");

      // Trigger coexistence state update via the unified bluetooth manager
      extern void s3_bt_trigger_coexistence_update(void);
      s3_bt_trigger_coexistence_update();
    }
    break;
  }
  case ESP_A2D_AUDIO_CFG_EVT:
    ESP_LOGI(TAG, "A2DP audio stream configured, codec: %s",
             a2d->audio_cfg.mcc.type == ESP_A2D_MCT_SBC ? "SBC" : "NON-SBC");
    
    // Log detailed SBC configuration for verification of 32kHz sample rate
    if (a2d->audio_cfg.mcc.type == ESP_A2D_MCT_SBC) {
      ESP_LOGI(TAG, "=== SBC AUDIO CONFIG VERIFICATION ===");
      uint8_t sbc_cfg = a2d->audio_cfg.mcc.cie.sbc[0];
      
      // Decode sample rate from SBC configuration byte 0 (bits 7-4)
      const char* sample_rate_str;
      if (sbc_cfg & 0x80) sample_rate_str = "16kHz";
      else if (sbc_cfg & 0x40) sample_rate_str = "32kHz";
      else if (sbc_cfg & 0x20) sample_rate_str = "44.1kHz";
      else if (sbc_cfg & 0x10) sample_rate_str = "48kHz";
      else sample_rate_str = "UNKNOWN";
      
      // Decode channel mode from SBC configuration byte 0 (bits 3-2)
      const char* channel_mode_str;
      if (sbc_cfg & 0x08) channel_mode_str = "JOINT_STEREO";
      else if (sbc_cfg & 0x04) channel_mode_str = "STEREO";
      else if (sbc_cfg & 0x02) channel_mode_str = "DUAL_CHANNEL";
      else if (sbc_cfg & 0x01) channel_mode_str = "MONO";
      else channel_mode_str = "UNKNOWN";
      
      ESP_LOGI(TAG, "Sample Rate: %s (config byte: 0x%02X)", sample_rate_str, sbc_cfg);
      ESP_LOGI(TAG, "Channel Mode: %s", channel_mode_str);
      ESP_LOGI(TAG, "Bitpool: %d", a2d->audio_cfg.mcc.cie.sbc[3]);
      ESP_LOGI(TAG, "Block Length: %d", (a2d->audio_cfg.mcc.cie.sbc[1] & 0x30) >> 4);
      ESP_LOGI(TAG, "Subbands: %d", (a2d->audio_cfg.mcc.cie.sbc[1] & 0x04) ? 8 : 4);
      ESP_LOGI(TAG, "Allocation: %s", (a2d->audio_cfg.mcc.cie.sbc[1] & 0x02) ? "SNR" : "LOUDNESS");
      ESP_LOGI(TAG, "=======================================");
    } else {
      ESP_LOGW(TAG, "Non-SBC codec configured. This might not be supported by "
                    "all sinks.");
    }
    break;
    
  case ESP_A2D_MEDIA_CTRL_ACK_EVT:
    ESP_LOGI(TAG, "A2DP media_ctrl_ack: cmd %d, status %d",
             a2d->media_ctrl_stat.cmd, a2d->media_ctrl_stat.status);

    if (a2d->media_ctrl_stat.cmd == ESP_A2D_MEDIA_CTRL_SUSPEND && 
      a2d->media_ctrl_stat.status == ESP_A2D_MEDIA_CTRL_ACK_SUCCESS) {
        if (s_media_stop_sem != NULL) {
          ESP_LOGI(TAG, "Media stream suspend ACK received. Signaling semaphore.");
          xSemaphoreGive(s_media_stop_sem);
        }
    }
    break;

  case ESP_A2D_PROF_STATE_EVT:
    ESP_LOGI(TAG, "A2DP profile state: %s",
             a2d->a2d_prof_stat.init_state == ESP_A2D_INIT_SUCCESS
                 ? "INITIALIZED"
                 : "INIT_FAILED");
    break;
  default:
    ESP_LOGW(TAG, "Unhandled A2DP event: %d", event);
    break;
  }
}

/**
 * @brief AVRCP controller callback
 */
void bt_app_avrc_ct_cb(esp_avrc_ct_cb_event_t event,
                       esp_avrc_ct_cb_param_t *param) {
  ESP_LOGI(TAG, "AVRC event: %d", event);

  switch (event) {
  case ESP_AVRC_CT_CONNECTION_STATE_EVT:
    ESP_LOGI(TAG, "AVRC Connection state: %d", param->conn_stat.connected);
    break;
  case ESP_AVRC_CT_PASSTHROUGH_RSP_EVT:
    ESP_LOGI(TAG, "AVRC passthrough rsp: key_code 0x%x, key_state %d",
             param->psth_rsp.key_code, param->psth_rsp.key_state);
    break;
  default:
    ESP_LOGW(TAG, "Unhandled AVRC event: %d", event);
    break;
  }
}

/**
 * @brief Starts A2DP source
 */
esp_err_t bt_start_a2dp_source(void) {
  esp_err_t ret;

  // 1. Initializes the Bluetooth controller (if not already active)
  if (esp_bt_controller_get_status() == ESP_BT_CONTROLLER_STATUS_IDLE) {
    // esp_bt_controller_config_t bt_cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
    // if ((ret = esp_bt_controller_init(&bt_cfg)) != ESP_OK) {
    //   ESP_LOGE(TAG, "BT controller init failed: %s", esp_err_to_name(ret));
    //   return ret;
    // }
    // if ((ret = esp_bt_controller_enable(ESP_BT_MODE_CLASSIC_BT)) != ESP_OK) {
    //   ESP_LOGE(TAG, "BT controller enable failed: %s", esp_err_to_name(ret));
    //   return ret;
    // }
  }

  // 2. Initializes Bluedroid
  // if (esp_bluedroid_get_status() == ESP_BLUEDROID_STATUS_UNINITIALIZED) {
  //   if ((ret = esp_bluedroid_init()) != ESP_OK) {
  //     ESP_LOGE(TAG, "Bluedroid init failed: %s", esp_err_to_name(ret));
  //     return ret;
  //   }
  // }

  // if (esp_bluedroid_get_status() == ESP_BLUEDROID_STATUS_INITIALIZED) {
  //   if ((ret = esp_bluedroid_enable()) != ESP_OK) {
  //     ESP_LOGE(TAG, "Bluedroid enable failed: %s", esp_err_to_name(ret));
  //     return ret;
  //   }
  // }

  // 3. Register GAP callback to handle scan results
  // Note: We don't unregister first because A2DP initialization depends on GAP events
  // Callback cleanup is handled properly in bt_deinit_a2dp_source()
  ret = esp_bt_gap_register_callback(bt_gap_cb);
  if (ret != ESP_OK) {
    ESP_LOGE(TAG, "GAP callback register failed: %s", esp_err_to_name(ret));
    return ret;
  }
  ESP_LOGI(TAG, "GAP callback registered successfully");

  // 4. Basic configuration (device name and discovery mode)
  esp_bt_gap_set_device_name("Pixsee-s3");
  esp_bt_gap_set_scan_mode(ESP_BT_CONNECTABLE, ESP_BT_GENERAL_DISCOVERABLE);
  
  // Allow time for GAP mode to stabilize after reinit
  vTaskDelay(pdMS_TO_TICKS(100));

  // 5. Register A2DP callbacks first (skip if already registered by ESP-ADF)
  ret = esp_a2d_register_callback(&bt_app_a2d_cb);
  if (ret == ESP_ERR_INVALID_STATE) {
    ESP_LOGI(TAG, "A2DP callback already registered (by ESP-ADF), continuing");
    ret = ESP_OK;  // Continue as if registration succeeded
  } else if (ret != ESP_OK) {
    ESP_LOGE(TAG, "A2DP callback register failed: %s", esp_err_to_name(ret));
    return ret;
  }

  // 6. AVRC (optional, only after A2DP is enabled)
  ret = esp_avrc_ct_init();
  if (ret == ESP_ERR_INVALID_STATE) {
    ESP_LOGI(TAG, "AVRC already initialized (by ESP-ADF), reusing existing instance");
    esp_avrc_ct_register_callback(bt_app_avrc_ct_cb);
  } else if (ret != ESP_OK) {
    ESP_LOGE(TAG, "AVRC init failed: %s", esp_err_to_name(ret));
  } else {
    esp_avrc_ct_register_callback(bt_app_avrc_ct_cb);
  }

  // 5. Initializes A2DP Source (skip if already initialized by ESP-ADF bluetooth_service)
  ret = esp_a2d_source_init();
  if (ret == ESP_ERR_INVALID_STATE) {
    ESP_LOGI(TAG, "A2DP source already initialized (by ESP-ADF), reusing existing instance");
    ret = ESP_OK;  // Continue as if initialization succeeded
  } else if (ret != ESP_OK) {
    ESP_LOGE(TAG, "A2DP source init failed: %s", esp_err_to_name(ret));
    return ret;
  }

  // Re-enable coexistence callbacks after successful initialization
  coex_callback_enabled = true;
  ESP_LOGI(TAG, "Coexistence callbacks re-enabled after BT initialization");
  
  // Reset scan state to ensure clean scanning after reinit
  is_scanning = false;
  memset(&strongest_device, 0, sizeof(bt_discovered_device_t));
  strongest_device.rssi = -127;
  ESP_LOGI(TAG, "BT scan state reset after reinit");
  
  ESP_LOGI(TAG, "A2DP Source initialized successfully!");
  return ESP_OK;
}

/**
 * @brief INITIATES the A2DP source disconnection process.
 * The actual finalization happens in the event callback.
 */
esp_err_t bt_stop_a2dp_source(void) {
  ESP_LOGI(TAG, "Starting polite shutdown process...");
  
  // Disable coexistence callbacks during deinitialization to prevent interference
  coex_callback_enabled = false;
  ESP_LOGI(TAG, "Coexistence callbacks disabled during BT deinitialization");

  // If a device is connected, just start the disconnection.
  // The rest of the logic will be handled in the A2DP callback.
  if (a2dp_connected) {
    ESP_LOGI(TAG, "Requesting A2DP disconnection from " ESP_BD_ADDR_STR "...",
             ESP_BD_ADDR_HEX(s_peer_bda));
    esp_a2d_source_disconnect(s_peer_bda);
  } else {
    // If no device is connected, there's nothing to disconnect.
    // DO NOT send BT_APP_EVENT_DISCONNECTED here - it's only for actual disconnections.
    // Sending it during scan cleanup causes premature exit from reconnect flow.
    ESP_LOGI(TAG, "Not connected, no active connection to stop. Cleanup complete.");
  }

  return ESP_OK;
}

/**
 * @brief Checks if A2DP is connected
 */
bool bt_is_a2dp_connected(void) { return a2dp_connected; }

/**
 * @brief Checks if A2DP is scanning
 */
bool bt_is_a2dp_scanning(void) { return is_scanning; }

/**
 * @brief Connects to a Bluetooth device
 */
esp_err_t bt_connect_to_device(const char *device_addr) {
  if (!device_addr) {
    ESP_LOGE(TAG, "Device address is NULL");
    return ESP_ERR_INVALID_ARG;
  }

  if (bt_ensure_initialized() != ESP_OK) {
    ESP_LOGE(TAG, "Bluetooth initialization failed, cannot connect");
    return ESP_FAIL;
  }

  ESP_LOGI(TAG, "Attempting to connect to: %s", device_addr);

  // Notify coexistence manager that BT connection attempt is starting
  extern void s3_bt_mark_connection_attempt(void);
  s3_bt_mark_connection_attempt();

  // Debug MAC address string
  // ESP_LOGI(TAG, "Debug MAC: len=%d", strlen(device_addr));
  // for (int i = 0; i < strlen(device_addr); i++) {
  //   ESP_LOGI(TAG, "char[%d] = 0x%02X ('%c')", i, (unsigned char)device_addr[i],
  //            isprint((unsigned char)device_addr[i]) ? device_addr[i] : '?');
  // }

  // Convert MAC to uppercase for sscanf compatibility
  char upper_mac[18];
  for (int i = 0; i < strlen(device_addr); i++) {
    upper_mac[i] = toupper((unsigned char)device_addr[i]);
  }
  upper_mac[strlen(device_addr)] = '\0';

  ESP_LOGI(TAG, "Uppercase MAC: %s", upper_mac);

  esp_bd_addr_t addr;
  unsigned int temp[6];
  int scan_result = sscanf(upper_mac, "%x:%x:%x:%x:%x:%x", &temp[0], &temp[1],
                          &temp[2], &temp[3], &temp[4], &temp[5]);
  ESP_LOGI(TAG, "sscanf result: %d (expected 6)", scan_result);

  // Convert to uint8_t
  for (int i = 0; i < 6; i++) {
    addr[i] = (uint8_t)temp[i];
  }

  if (scan_result != 6) {
    ESP_LOGE(TAG, "Invalid MAC format. Use 'XX:XX:XX:XX:XX:XX'");
    return ESP_ERR_INVALID_ARG;
  }

  ESP_LOGI(TAG, "Converted MAC: %02X:%02X:%02X:%02X:%02X:%02X", addr[0],
           addr[1], addr[2], addr[3], addr[4], addr[5]);

  // Set connection_in_progress flag immediately (before slow A2DP callback)
  connection_in_progress = true;
  ESP_LOGI(TAG, "Connection in progress flag set");

  esp_err_t ret = esp_a2d_source_connect(addr);
  if (ret != ESP_OK) {
    ESP_LOGE(TAG, "Connection failed: %s", esp_err_to_name(ret));
    
    // Clear connection_in_progress flag to allow future attempts
    connection_in_progress = false;
    ESP_LOGI(TAG, "Connection in progress flag cleared (connect API failed)");
    
    // Notify manager of connection failure so it can retry
    if (s_app_event_cb != NULL) {
      s_app_event_cb(BT_APP_EVENT_CONNECTION_FAILED);
    }
    return ret;
  }
  
  ESP_LOGI(TAG, "Connection initiated successfully");
  return ESP_OK;
}

/**
 * @brief Deinitializes the A2DP source and Bluetooth stack.
 */
esp_err_t bt_deinit_a2dp_source(void) {

    ESP_LOGI(TAG, "Deinitializing A2DP Source...");
    
    // Disable coexistence callbacks during deinitialization to prevent interference
    coex_callback_enabled = false;
    ESP_LOGI(TAG, "Coexistence callbacks disabled during BT deinitialization");
    
    // CRITICAL: Save and disable callback to prevent events during cleanup
    bt_app_event_callback_t saved_callback = s_app_event_cb;
    s_app_event_cb = NULL;
    esp_err_t ret;
    
    // Cancel discovery first
    if (is_scanning) {
        ESP_LOGI(TAG, "Cancelling discovery...");
        esp_bt_gap_cancel_discovery();
        vTaskDelay(pdMS_TO_TICKS(200));
        ESP_LOGI(TAG, "Scan stopped.");
        is_scanning = false;
    }
    
    // CRITICAL: Wait for any pending connection attempts to abort
    // Connection attempts take time and callbacks may still fire during deinit
    ESP_LOGI(TAG, "Waiting for pending connection attempts to abort...");
    vTaskDelay(pdMS_TO_TICKS(500));

    // Step 2: Disconnect if connected (and wait for completion)
    if (a2dp_connected) {
        ESP_LOGI(TAG, "Disconnecting A2DP...");
        esp_a2d_source_disconnect(s_peer_bda);
        
        // Wait for disconnection to complete
        int timeout_count = 0;
        while (a2dp_connected && timeout_count < 50) { // 5 second timeout
            vTaskDelay(pdMS_TO_TICKS(100));
            timeout_count++;
        }
        
        if (a2dp_connected) {
            ESP_LOGW(TAG, "Disconnection timeout, forcing state reset");
            a2dp_connected = false;
        }
    }

    // Step 3: CRITICAL - Unregister callbacks BEFORE deinit to prevent callbacks during teardown
    ESP_LOGI(TAG, "Unregistering A2DP and GAP callbacks before deinit...");
    esp_a2d_register_callback(NULL);
    esp_bt_gap_register_callback(NULL);
    vTaskDelay(pdMS_TO_TICKS(100)); // Allow pending callbacks to complete
    
    // Step 4: Deinitialize AVRC
    ESP_LOGI(TAG, "Deinitializing AVRC...");
    ret = esp_avrc_ct_deinit();
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "AVRC deinit failed: %s", esp_err_to_name(ret));
    }
    vTaskDelay(pdMS_TO_TICKS(100));

    // Step 5: Deinitialize A2DP source
    ESP_LOGI(TAG, "Deinitializing A2DP source...");
    ret = esp_a2d_source_deinit();
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "A2DP source deinit failed: %s", esp_err_to_name(ret));
    }
    vTaskDelay(pdMS_TO_TICKS(200)); // Wait for A2DP to fully deinit

    // Step 6: Set device as non-connectable to prevent unwanted reconnection attempts
    // This only affects BT Classic, not BLE advertising
    ESP_LOGI(TAG, "Setting device to non-connectable mode...");
    esp_bt_gap_set_scan_mode(ESP_BT_NON_CONNECTABLE, ESP_BT_NON_DISCOVERABLE);

    // SKIP THIS FOR KEEPING BLE ACTIVE
    // // Step 6: Disable Bluedroid
    // if (esp_bluedroid_get_status() == ESP_BLUEDROID_STATUS_ENABLED) {
    //     ESP_LOGI(TAG, "Disabling Bluedroid...");
    //     if ((ret = esp_bluedroid_disable()) != ESP_OK) {
    //         ESP_LOGW(TAG, "Bluedroid disable failed: %s", esp_err_to_name(ret));
    //     }
    //     vTaskDelay(pdMS_TO_TICKS(200)); // Wait for disable to complete
    // }
    
    // // Step 7: Deinitialize Bluedroid
    // if (esp_bluedroid_get_status() == ESP_BLUEDROID_STATUS_INITIALIZED) {
    //     ESP_LOGI(TAG, "Deinitializing Bluedroid...");
    //     if ((ret = esp_bluedroid_deinit()) != ESP_OK) {
    //         ESP_LOGW(TAG, "Bluedroid deinit failed: %s", esp_err_to_name(ret));
    //     }
    //     vTaskDelay(pdMS_TO_TICKS(200));
    // }

    // // Step 8: Force disable BT Controller
    // ESP_LOGI(TAG, "Checking controller status: %d", esp_bt_controller_get_status());
    
    // if (esp_bt_controller_get_status() == ESP_BT_CONTROLLER_STATUS_ENABLED) {
    //     ESP_LOGI(TAG, "Force disabling BT controller...");
        
    //     // First attempt: Normal disable
    //     if ((ret = esp_bt_controller_disable()) != ESP_OK) {
    //         ESP_LOGW(TAG, "First disable failed: %s", esp_err_to_name(ret));
    //     }
        
    //     // Wait and check
    //     vTaskDelay(pdMS_TO_TICKS(1000));
        
    //     // If still enabled, force second disable
    //     if (esp_bt_controller_get_status() == ESP_BT_CONTROLLER_STATUS_ENABLED) {
    //         ESP_LOGW(TAG, "Controller still enabled, forcing second disable...");
    //         esp_bt_controller_disable(); // Ignore return
    //         vTaskDelay(pdMS_TO_TICKS(2000));
    //     }
        
    //     // Wait for controller to reach IDLE state
    //     int timeout_count = 0;
    //     while(esp_bt_controller_get_status() != ESP_BT_CONTROLLER_STATUS_IDLE && timeout_count < 100) {
    //         vTaskDelay(pdMS_TO_TICKS(50));
    //         timeout_count++;
    //     }
    // }

    // // Step 9: Force deinit regardless of status
    // ESP_LOGI(TAG, "Final controller status: %d", esp_bt_controller_get_status());
    
    // // Force deinit even if not in IDLE state
    // ESP_LOGI(TAG, "Force deinitializing BT controller...");
    // esp_bt_controller_deinit(); // Ignore return and status
    
    // vTaskDelay(pdMS_TO_TICKS(3000)); // 3 seconds delay
    // SKIP THIS FOR KEEPING BLE ACTIVE

    // Step 10: Reset internal state
    a2dp_connected = false;
    is_scanning = false;
    memset(s_peer_bda, 0, sizeof(esp_bd_addr_t));
    memset(&strongest_device, 0, sizeof(bt_discovered_device_t));
    strongest_device.rssi = -127;

    // RESTORE callback only at the very end
    s_app_event_cb = saved_callback;
    
    ESP_LOGI(TAG, "A2DP Source deinitialization completed.");
    return ESP_OK;
}

/**
 * @brief Waits for the A2DP media stream to be confirmed as stopped.
 * @param timeout_ms Timeout in milliseconds to wait.
 * @return true if the stream stopped, false on timeout.
 */
bool bt_a2dp_wait_for_media_stop(uint32_t timeout_ms) {
    if (s_media_stop_sem == NULL) {
        s_media_stop_sem = xSemaphoreCreateBinary();
    } else {
        xSemaphoreTake(s_media_stop_sem, 0); 
    }

    if (xSemaphoreTake(s_media_stop_sem, pdMS_TO_TICKS(timeout_ms)) == pdTRUE) {
        ESP_LOGI(TAG, "Successfully waited for media stop confirmation.");
        return true;
    } else {
        ESP_LOGW(TAG, "Timeout waiting for media stop confirmation.");
        return false;
    }
}

/* =================== COEXISTENCE MANAGEMENT FUNCTIONS =================== */

void s3_bt_classic_set_coexistence_callback(void (*cb)(bool bt_scanning)) {
    s_coexistence_callback = cb;
    ESP_LOGI(TAG, "Coexistence callback registered");
}

esp_err_t s3_bt_classic_init(void) {
    ESP_LOGI(TAG, "Initializing S3 BT Classic Manager");
    return bt_start_a2dp_source();
}

esp_err_t s3_bt_classic_start_scan(uint8_t duration) {
    ESP_LOGI(TAG, "Starting BT Classic scan (coexistence managed)");

    // Coexistence notifications are handled by GAP callback events
    // ESP_BT_GAP_DISCOVERY_STARTED -> callback(true)
    // ESP_BT_GAP_DISCOVERY_STOPPED -> callback(false)
    esp_err_t ret = bt_scan_and_connect_to_strongest_internal(duration);

    return ret;
}

esp_err_t s3_bt_classic_stop_scan(void) {
    ESP_LOGI(TAG, "Stopping BT Classic scan");

    if (is_scanning) {
        esp_bt_gap_cancel_discovery();
        is_scanning = false;

        // Notify coexistence manager that scanning stopped
        if (s_coexistence_callback) {
            s_coexistence_callback(false);
        }
    }

    return ESP_OK;
}

bool s3_bt_classic_is_scanning(void) {
    return is_scanning;
}

bool s3_bt_classic_is_connected(void) {
    return a2dp_connected;
}

bool s3_bt_classic_is_streaming(void) {
    return a2dp_streaming;
}