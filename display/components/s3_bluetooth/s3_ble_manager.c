#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_system.h"
#include "esp_log.h"
#include "esp_bt.h"

#include "esp_gap_ble_api.h"
#include "esp_gatts_api.h"
#include "esp_bt_defs.h"
#include "esp_bt_main.h"
#include "esp_bt_device.h"
#include "esp_gatt_common_api.h"
#include "s3_bluetooth.h"
#include "storage.h"
#include "WiFi.h"
#include "s3_definitions.h"
#include "cJSON.h"
#include "lv_screen_mgr.h"
#include "app_timeout.h"
#include "sntp_syncer.h"
#include "backlight.h"
#include "esp_timer.h"
#include "s3_nfc_handler.h"
#include "app_state_machine.h"
#include "backlight.h"

#include "sdkconfig.h"

#define PIXSEE_BLE_ADV_NAME_LEN_MAX (29)
#define WIFI_CREDENTIALS_CONTENT_MAX_SIZE (200)
#define PREPARE_BUF_MAX_SIZE        (1024)

// #define SERIAL_NUMBER_LEN       (14)
// #define WIFI_SSID_MAX_LEN       (33)
// #define WIFI_PASSWORD_MAX_LEN   (33)
// #define SECRET_KEY_MAX_LEN      (37)
// #define TIMEZONE_MAX_LEN        (7)

#define BLE_SERVICE_MAX_LEN (GATT_SERVICE_CHAR_MAX_LEN)

/* ========================= Connection/CCCD tracking ========================= */
#define INVALID_CONN_ID 0xFFFF

static volatile bool s_connected = false;
static volatile bool s_cccd_enabled = false;     // notify OR indicate enabled
static volatile bool s_cccd_indications = false; // true=indicate, false=notify
static volatile bool s_congested = false;        // set by ESP_GATTS_CONGEST_EVT
static volatile bool s_service_recreating = false; // service recreation in progress
/* =========================================================================== */

/* 128-bit UUIDs (optimized - single characteristic) */
static const uint8_t service_uuid[16] = {
    0xea, 0xb5, 0xa6, 0xfd, 0x15, 0x82, 0x0d, 0xa4,
    0xa4, 0x48, 0xca, 0x54, 0xcf, 0x26, 0xaa, 0x68
};
static const uint8_t char_uuid[16] = {
    0x49, 0xbb, 0xf0, 0x15, 0x1f, 0xb7, 0xbc, 0xab,
    0x0f, 0x4e, 0x4a, 0x19, 0x8f, 0x0c, 0x4a, 0x94
};

#define GATT_SERVICE_HANDLERS       (16)  // Service + dev_ctrl char + dev_ctrl CCCD + 5 dev_msg chars + 8 extra
#define GATT_SERVICE_HANDLERS_DEV_CTRL_ONLY (4)  // Service + dev_ctrl char + dev_ctrl CCCD + 1 extra
#define GATT_SERVICE_CHAR_MAX_LEN   (0x40)

#define adv_config_flag      (1 << 0)
#define scan_rsp_config_flag (1 << 1)
#define WIFI_BINDING_TIMEOUT (90 * 1000)

static const char *TAG = "S3_BLE_MGR";

/* Coexistence callback to notify main component about BLE state changes */
static void (*s_coexistence_callback)(bool ble_active) = NULL;

static void gatts_profile_a_event_handler(esp_gatts_cb_event_t event,
                                          esp_gatt_if_t gatts_if,
                                          esp_ble_gatts_cb_param_t *param);
void add_dev_msg_characteristics(void);
void remove_dev_msg_characteristics(void);
static void recreate_service(bool enable_dev_msg);
static void create_dev_ctrl_service(esp_gatt_if_t gatts_if);
static void create_full_service(esp_gatt_if_t gatts_if);

/* Device control 4-byte data - always kept updated */
static uint8_t dev_ctrl_data[4] = {0, 0, 0, 0}; // [screen][message][status][control]
static esp_gatt_char_prop_t a_property = 0;

static esp_attr_value_t gatts_demo_char1_val = {
    .attr_max_len = GATT_SERVICE_CHAR_MAX_LEN,
    .attr_len     = 4,
    .attr_value   = dev_ctrl_data,
};

static uint8_t adv_config_done = 0;

/* Advertising service UUID (128-bit) - same as light service */
static const uint8_t adv_service_uuid128[16] = {
    0xea, 0xb5, 0xa6, 0xfd, 0x15, 0x82, 0x0d, 0xa4,
    0xa4, 0x48, 0xca, 0x54, 0xcf, 0x26, 0xaa, 0x68
};

/* Advertising data */
static esp_ble_adv_data_t adv_data = {
    .set_scan_rsp = false,
    .include_name = false,
    .include_txpower = false,
    .min_interval = 0x0006,
    .max_interval = 0x0010,
    .appearance = 0x00,
    .manufacturer_len = 0,
    .p_manufacturer_data = NULL,
    .service_data_len = 0,
    .p_service_data = NULL,
    .service_uuid_len = sizeof(adv_service_uuid128),
    .p_service_uuid = (uint8_t *)adv_service_uuid128,
    .flag = (ESP_BLE_ADV_FLAG_GEN_DISC | ESP_BLE_ADV_FLAG_BREDR_NOT_SPT),
};

/* Scan response data */
static esp_ble_adv_data_t scan_rsp_data = {
    .set_scan_rsp = true,
    .include_name = true,
    .include_txpower = true,
    .appearance = 0x00,
    .manufacturer_len = 0,
    .p_manufacturer_data = NULL,
    .service_data_len = 0,
    .p_service_data = NULL,
    .service_uuid_len = 0,
    .p_service_uuid = NULL,
    .flag = (ESP_BLE_ADV_FLAG_GEN_DISC | ESP_BLE_ADV_FLAG_BREDR_NOT_SPT),
};

static esp_ble_adv_params_t adv_params = {
    .adv_int_min        = 0x0320,  // 500ms (was 0x20 = 20ms) - 2 interrupts/sec
    .adv_int_max        = 0x0320,  // 500ms (was 0x40 = 40ms) - reduces BLE overhead 25x
    .adv_type           = ADV_TYPE_IND,
    .own_addr_type      = BLE_ADDR_TYPE_PUBLIC,
    .channel_map        = ADV_CHNL_ALL,
    .adv_filter_policy  = ADV_FILTER_ALLOW_SCAN_ANY_CON_ANY,
};

#define PROFILE_NUM 1
#define PROFILE_A_APP_ID 0

struct gatts_profile_inst {
    esp_gatts_cb_t       gatts_cb;
    uint16_t             gatts_if;
    uint16_t             app_id;
    uint16_t             conn_id;
    uint16_t             service_handle;
    esp_gatt_srvc_id_t   service_id;
    uint16_t             char_handle;
    esp_bt_uuid_t        char_uuid;
    esp_gatt_perm_t      perm;
    esp_gatt_char_prop_t property;
    uint16_t             descr_handle;
    esp_bt_uuid_t        descr_uuid;
};

/* Forward declarations - removed obsolete send_ble_msg functions */

/* Device control GATT functions */
void dev_ctrl_update_values(int battery, int screen, int alarm); // -1 = don't update
void dev_ctrl_handle_command(uint8_t command);
void dev_ctrl_sync_gatt_server(void);
void handle_sync_status_request(void);

/* Store GATTs if/handles per profile */
static struct gatts_profile_inst gl_profile_tab[PROFILE_NUM] = {
    [PROFILE_A_APP_ID] = {
        .gatts_cb = gatts_profile_a_event_handler,
        .gatts_if = ESP_GATT_IF_NONE,
        .conn_id  = INVALID_CONN_ID,
    },
};

/* ========================= STATIC VARIABLES ========================= */
typedef struct {
    uint8_t *prepare_buf;
    int      prepare_len;
} prepare_type_env_t;
static prepare_type_env_t service_prepare_write_env;

/* Global state tracking */
static bool is_pixsee_binding = false;
static TaskHandle_t oob_pairing_task_handle = NULL;

/* Pre-allocated GATT response buffer to avoid malloc during audio streaming */
static esp_gatt_rsp_t static_gatt_rsp;
static SemaphoreHandle_t gatt_rsp_mutex = NULL;

/* Device name cache */
static char cached_device_name[PIXSEE_BLE_ADV_NAME_LEN_MAX] = {0};
static bool device_name_cached = false;

/* Dev_msg GATT characteristics (dev_msg mode) - Pre-allocated */
static bool dev_msg_mode_active = false;
static uint16_t dev_msg_serial_number_handle = 0;
static uint16_t dev_msg_wifi_ssid_handle = 0;
static uint16_t dev_msg_wifi_password_handle = 0;
static uint16_t dev_msg_secret_key_handle = 0;
static uint16_t dev_msg_timezone_handle = 0;
static uint16_t dev_msg_album_handle = 0;


/* Dev_msg characteristic data - Pre-allocated */
static uint8_t dev_msg_serial_number_data[SERIAL_NUMBER_SIZE] = {0};
static uint8_t dev_msg_wifi_ssid_data[WIFI_SSID_SIZE] = {0};
static uint8_t dev_msg_wifi_password_data[WIFI_PASSWORD_SIZE] = {0};
static uint8_t dev_msg_secret_key_data[SECRET_KEY_STR_SIZE] = {0};
static uint8_t dev_msg_timezone_data[TIMEZONE_STR_SIZE] = {0};


/* UUID definitions for dev_msg characteristics */
static const uint8_t wifi_ssid_uuid[16] = {
    0xa1, 0xb2, 0xc3, 0xd4, 0xe5, 0xf6, 0x07, 0x18,
    0x29, 0x3a, 0x4b, 0x5c, 0x6d, 0x7e, 0x8f, 0x90
};
static const uint8_t wifi_password_uuid[16] = {
    0xa2, 0xb3, 0xc4, 0xd5, 0xe6, 0xf7, 0x08, 0x19,
    0x2a, 0x3b, 0x4c, 0x5d, 0x6e, 0x7f, 0x80, 0x91
};
static const uint8_t secret_key_uuid[16] = {
    0xa3, 0xb4, 0xc5, 0xd6, 0xe7, 0xf8, 0x09, 0x1a,
    0x2b, 0x3c, 0x4d, 0x5e, 0x6f, 0x70, 0x81, 0x92
};
static const uint8_t timezone_uuid[16] = {
    0xa4, 0xb5, 0xc6, 0xd7, 0xe8, 0xf9, 0x0a, 0x1b,
    0x2c, 0x3d, 0x4e, 0x5f, 0x60, 0x71, 0x82, 0x93
};
static const uint8_t serial_number_uuid[16] = {
    0x12, 0x34, 0x56, 0x78, 0x9a, 0xbc, 0xde, 0xf0,
    0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88
    // 0xa5, 0xb6, 0xc7, 0xd8, 0xe9, 0xfa, 0x0b, 0x1c,
    // 0x2d, 0x3e, 0x4f, 0x50, 0x61, 0x72, 0x83, 0x94
};

/* ========================= UTILS ========================= */
static uint64_t transfer_start = 0;
static uint64_t last_chunk_time = 0;
static uint64_t total_bytes = 0;

static void get_device_name(char *dev_name)
{
    char sn[32] = {0};
    char sn_final_numbers[5] = {0};

    read_serial_number(sn);
    if (strlen(sn) > 0) {
        strncpy(sn_final_numbers, &sn[strlen(sn) - 4], 4);
        sn_final_numbers[4] = '\0';
        snprintf(dev_name, PIXSEE_BLE_ADV_NAME_LEN_MAX, "Pixsee_%s", sn_final_numbers);
    } else {
        snprintf(dev_name, PIXSEE_BLE_ADV_NAME_LEN_MAX, "Pixsee_XXXX");
    }
    ESP_LOGW(TAG, "Device name: %s", dev_name);
}

static void exec_write_event_env(prepare_type_env_t *prepare_write_env,
                                 esp_ble_gatts_cb_param_t *param)
{
    if (param->exec_write.exec_write_flag == ESP_GATT_PREP_WRITE_EXEC) {
        int dump_len = prepare_write_env->prepare_len > 32 ? 32 : prepare_write_env->prepare_len;
        ESP_LOGI(TAG, "prepared len %d (dump %d)", prepare_write_env->prepare_len, dump_len);
        if (dump_len > 0) ESP_LOG_BUFFER_HEX(TAG, prepare_write_env->prepare_buf, dump_len);
    } else {
        ESP_LOGI(TAG, "Prepare write cancel");
    }
    if (prepare_write_env->prepare_buf) {
        free(prepare_write_env->prepare_buf);
        prepare_write_env->prepare_buf = NULL;
    }
    prepare_write_env->prepare_len = 0;
}

static void write_event_env(esp_gatt_if_t gatts_if,
                            prepare_type_env_t *prepare_write_env,
                            esp_ble_gatts_cb_param_t *param)
{
    esp_gatt_status_t status = ESP_GATT_OK;

    if (param->write.need_rsp) {
        if (param->write.is_prep) {
            if (param->write.offset > PREPARE_BUF_MAX_SIZE) {
                status = ESP_GATT_INVALID_OFFSET;
            } else if ((param->write.offset + param->write.len) > PREPARE_BUF_MAX_SIZE) {
                status = ESP_GATT_INVALID_ATTR_LEN;
            }
            if (status == ESP_GATT_OK && prepare_write_env->prepare_buf == NULL) {
                prepare_write_env->prepare_buf = (uint8_t *)malloc(PREPARE_BUF_MAX_SIZE);
                prepare_write_env->prepare_len = 0;
                if (prepare_write_env->prepare_buf == NULL) {
                    ESP_LOGE(TAG, "prep no mem");
                    status = ESP_GATT_NO_RESOURCES;
                }
            }

            /*
             * DMA OPTIMIZATION: Use pre-allocated GATT response buffer to avoid malloc/free
             * during audio streaming. This reduces DMA memory fragmentation and pressure
             * when BLE and A2DP are both active, preventing audio skips/freezes.
             */
            if (gatt_rsp_mutex != NULL && xSemaphoreTake(gatt_rsp_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
                static_gatt_rsp.attr_value.len      = param->write.len;
                static_gatt_rsp.attr_value.handle   = param->write.handle;
                static_gatt_rsp.attr_value.offset   = param->write.offset;
                static_gatt_rsp.attr_value.auth_req = ESP_GATT_AUTH_REQ_NONE;
                memcpy(static_gatt_rsp.attr_value.value, param->write.value, param->write.len);
                esp_err_t response_err = esp_ble_gatts_send_response(
                    gatts_if, param->write.conn_id, param->write.trans_id, status, &static_gatt_rsp);
                if (response_err != ESP_OK) {
                    ESP_LOGE(TAG, "Send response error");
                }
                xSemaphoreGive(gatt_rsp_mutex);
            } else {
                ESP_LOGE(TAG, "Failed to acquire GATT response mutex or mutex not initialized");
                status = ESP_GATT_NO_RESOURCES;
            }
            if (status != ESP_GATT_OK) return;

            memcpy(prepare_write_env->prepare_buf + param->write.offset,
                   param->write.value,
                   param->write.len);
            prepare_write_env->prepare_len += param->write.len;

        } else {
            esp_ble_gatts_send_response(gatts_if,
                                        param->write.conn_id,
                                        param->write.trans_id,
                                        status, NULL);
        }
    }
}

static bool handle_dev_msg_write(uint16_t handle, const char* data, int data_len) {
    esp_err_t nvs_err = ESP_OK;
    bool handled = false;
    int oob_status = 0;

    if (handle == dev_msg_wifi_ssid_handle) {
        nvs_err = write_ssid(data);
        gPixseeStatus = (nvs_err == ESP_OK) ? S3ER_SETUP_SSID_SUCCESS : S3ER_SETUP_SSID_FAIL;
        handled = true;

        ESP_LOGI(TAG, "📡 WiFi SSID received: [%.*s] → %s",
                 data_len, data, nvs_err == ESP_OK ? "✅ Saved to NVS" : "❌ Failed to save");
    } else if (handle == dev_msg_wifi_password_handle) {
        nvs_err = write_pass(data);
        gPixseeStatus = (nvs_err == ESP_OK) ? S3ER_SETUP_PASS_SUCCESS : S3ER_SETUP_PASS_FAIL;
        handled = true;

        ESP_LOGI(TAG, "🔑 WiFi Password received: [%.*s] → %s",
                 data_len, data, nvs_err == ESP_OK ? "✅ Saved to NVS" : "❌ Failed to save");
    } else if (handle == dev_msg_secret_key_handle) {
        read_oob_status(&oob_status);
        if (oob_status == OOB_FACTORY_RESET) {
            nvs_err = write_secret_key(data);
            gPixseeStatus = (nvs_err == ESP_OK) ? S3ER_SETUP_SECK_SUCCESS : S3ER_SETUP_SECK_FAIL;
        } else { // In normal mode, ignore secret key writes (already set)
            gPixseeStatus = S3ER_SETUP_SECK_NOT_IN_OOB;
        }

        handled = true;

        ESP_LOGI(TAG, "🔐 Secret Key received: [%.*s] → %s (OOB=%d)",
                 data_len, data,
                 oob_status == OOB_FACTORY_RESET ? (nvs_err == ESP_OK ? "✅ Saved" : "❌ Failed") : "⚠️ Ignored (not in OOB)",
                 oob_status);
    } else if (handle == dev_msg_timezone_handle) {
        nvs_err = write_timezone(data);
        set_timezone(data);
        gPixseeStatus = (nvs_err == ESP_OK) ? S3ER_SETUP_TIMZ_SUCCESS : S3ER_SETUP_TIMZ_FAIL;
        handled = true;

        ESP_LOGI(TAG, "🌍 Timezone received: [%.*s] → %s",
                 data_len, data, nvs_err == ESP_OK ? "✅ Saved & Applied" : "❌ Failed to save");
    }

    return handled;
}

static esp_err_t handle_exec_write_from_ble_service(const prepare_type_env_t *prepare_write_env, esp_ble_gatts_cb_param_t *param, esp_gatt_if_t gatts_if)
{
    bool handled = false;
    int received_str_len = 0;
    char received_str[BLE_SERVICE_MAX_LEN] = {0};

    received_str_len = (prepare_write_env->prepare_len >= sizeof(received_str))? (sizeof(received_str) - 1) : prepare_write_env->prepare_len;
    memcpy(received_str, prepare_write_env->prepare_buf, received_str_len);
    received_str[received_str_len] = '\0';

    ESP_LOGW(TAG, "Execute Write buffer content: %s", received_str);


    if (dev_msg_mode_active) {
        handled = handle_dev_msg_write(param->write.handle, received_str, received_str_len);

        if (handled) {
            esp_ble_gatts_send_response(gatts_if, param->write.conn_id,
                                        param->write.trans_id, ESP_GATT_OK, NULL);
        }
    }
    return ESP_OK;
}

static bool check_cloud_access(void)
{
    int oob_status = OOB_NORMAL;
    read_oob_status(&oob_status);
    
    // Check what's actually stored in NVS
    char ssid[32] = {0}, pass[32] = {0}, tz[32] = {0}, secret[64] = {0};
    bool has_wifi = (read_wifi_credentials(ssid, pass) == ESP_OK && strlen(ssid) > 0 && strlen(pass) > 0);
    bool has_timezone = (read_timezone(tz) == ESP_OK && strlen(tz) > 0);
    bool has_secret = (read_secret_key(secret) == ESP_OK && strlen(secret) > 0);
    
    if (oob_status == OOB_FACTORY_RESET) {
        // Factory reset state: need ALL 4 credentials in NVS for binding
        if (has_wifi && has_timezone && has_secret) {
            ESP_LOGI(TAG, "All binding credentials available in NVS (SSID, password, timezone, secret_key)");
            return true;
        } else {
            ESP_LOGW(TAG, "Missing binding credentials in NVS - WiFi: %d, Timezone: %d, Secret: %d", 
                     has_wifi, has_timezone, has_secret);
            return false;
        }
    } else {
        // Normal state: need 3 credentials in NVS for WiFi change
        if (has_wifi && has_timezone) {
            ESP_LOGI(TAG, "WiFi change credentials available in NVS (SSID, password, timezone)");
            return true;
        } else {
            ESP_LOGW(TAG, "Missing WiFi change credentials in NVS - WiFi: %d, Timezone: %d", 
                     has_wifi, has_timezone);
            return false;
        }
    }
}

void oob_pairing_task(void *pvParameters)
{
    esp_err_t success = ESP_FAIL;
    app_timeout_stop();

    // Check if we have the required credentials (no waiting, immediate check)
    if(check_cloud_access() == true)
    {
        ESP_LOGI(TAG, "Required credentials available, ready to init the station");
        success = ESP_OK;
    }
    else
    {
        ESP_LOGE(TAG, "Missing required credentials for cloud access");
        success = ESP_FAIL;
        is_pixsee_binding = false;
    }

    ESP_LOGI(TAG, "OOB pairing result: %s", esp_err_to_name(success));
    if (success == ESP_OK)
    {
        ESP_LOGI(TAG, "Success - credentials available for WiFi connection");
        set_current_screen(WIFI_SEARCH_SCREEN, NULL_SCREEN);
    }
    else
    {
        ESP_LOGW(TAG, "Returning to network setup - missing credentials");
		int oob_status = OOB_NORMAL;
		read_oob_status(&oob_status);
		if (oob_status == OOB_FACTORY_RESET)
		{
			set_current_screen(HOME_SCREEN, HOME_SCREEN);
		}
		else
		{
			set_current_screen(WIFI_DISCONNECT_SCREEN, NULL_SCREEN);
		}
    }
    app_timeout_restart();
    // Note: No need to reset flags since we check NVS directly
    
    oob_pairing_task_handle = NULL;
    vTaskDelete(NULL);
}

/* ========================= GAP HANDLER ========================= */
static void gap_event_handler(esp_gap_ble_cb_event_t event, esp_ble_gap_cb_param_t *param)
{
    switch (event) {
#ifdef CONFIG_SET_RAW_ADV_DATA
    case ESP_GAP_BLE_ADV_DATA_RAW_SET_COMPLETE_EVT:
        adv_config_done &= (~adv_config_flag);
        if (adv_config_done == 0) esp_ble_gap_start_advertising(&adv_params);
        break;
    case ESP_GAP_BLE_SCAN_RSP_DATA_RAW_SET_COMPLETE_EVT:
        adv_config_done &= (~scan_rsp_config_flag);
        if (adv_config_done == 0) esp_ble_gap_start_advertising(&adv_params);
        break;
#else
    case ESP_GAP_BLE_ADV_DATA_SET_COMPLETE_EVT:
        adv_config_done &= (~adv_config_flag);
        if (adv_config_done == 0) esp_ble_gap_start_advertising(&adv_params);
        break;
    case ESP_GAP_BLE_SCAN_RSP_DATA_SET_COMPLETE_EVT:
        adv_config_done &= (~scan_rsp_config_flag);
        if (adv_config_done == 0) esp_ble_gap_start_advertising(&adv_params);
        break;
#endif
    case ESP_GAP_BLE_ADV_START_COMPLETE_EVT:
        if (param->adv_start_cmpl.status != ESP_BT_STATUS_SUCCESS) {
            ESP_LOGE(TAG, "Advertising start failed, status %d", param->adv_start_cmpl.status);
            // Check if this is an L2CAP resource allocation failure
            if (param->adv_start_cmpl.status == ESP_BT_STATUS_NOMEM ||
                param->adv_start_cmpl.status == ESP_BT_STATUS_BUSY) {
                ESP_LOGW(TAG, "L2CAP resource allocation failure detected - implementing recovery");
                // Call dedicated L2CAP failure handler
                extern esp_err_t s3_bt_handle_l2cap_failure(void);
                s3_bt_handle_l2cap_failure();
            }
            break;
        }
        ESP_LOGI(TAG, "Advertising started");
        break;
    case ESP_GAP_BLE_ADV_STOP_COMPLETE_EVT:
        if (param->adv_stop_cmpl.status != ESP_BT_STATUS_SUCCESS) {
            ESP_LOGE(TAG, "Advertising stop failed, status %d", param->adv_stop_cmpl.status);
            break;
        }
        ESP_LOGI(TAG, "Advertising stopped");
        break;
    case ESP_GAP_BLE_UPDATE_CONN_PARAMS_EVT:
        ESP_LOGI(TAG, "Conn params: status %d, int %d, lat %d, to %d",
                 param->update_conn_params.status,
                 param->update_conn_params.conn_int,
                 param->update_conn_params.latency,
                 param->update_conn_params.timeout);
        break;
    case ESP_GAP_BLE_SET_PKT_LENGTH_COMPLETE_EVT:
        ESP_LOGI(TAG, "Pkt length: status %d, rx %d, tx %d",
                 param->pkt_data_length_cmpl.status,
                 param->pkt_data_length_cmpl.params.rx_len,
                 param->pkt_data_length_cmpl.params.tx_len);
        break;
    default:
        break;
    }
}

/* ========================= GATTS DISPATCHER ========================= */
static void gatts_event_handler(esp_gatts_cb_event_t event,
                                esp_gatt_if_t gatts_if,
                                esp_ble_gatts_cb_param_t *param)
{
    if (event == ESP_GATTS_REG_EVT) {
        if (param->reg.status == ESP_GATT_OK) {
            gl_profile_tab[param->reg.app_id].gatts_if = gatts_if;
        } else {
            ESP_LOGI(TAG, "Reg app failed, app_id %04x, status %d",
                     param->reg.app_id, param->reg.status);
            return;
        }
    }

    for (int idx = 0; idx < PROFILE_NUM; idx++) {
        if (gatts_if == ESP_GATT_IF_NONE || gatts_if == gl_profile_tab[idx].gatts_if) {
            if (gl_profile_tab[idx].gatts_cb) {
                gl_profile_tab[idx].gatts_cb(event, gatts_if, param);
            }
        }
    }
}

int get_current_msg(void){
    return 44;
}


/* ========================= PROFILE HANDLER ========================= */
static void gatts_profile_a_event_handler(esp_gatts_cb_event_t event,
                                          esp_gatt_if_t gatts_if,
                                          esp_ble_gatts_cb_param_t *param)
{
    /* Local variables for conventional C style */
    char *device_name = cached_device_name;
    esp_gatt_rsp_t rsp;
    uint8_t command;
    bool handled;
    esp_err_t nvs_err;
    uint16_t cccd;
    esp_ble_conn_update_params_t conn_params;
    int dump_len;
    esp_bt_uuid_t char_uuid_temp;
    esp_attr_value_t char_val_temp;
    esp_err_t ret = ESP_OK;
    
    if (!device_name_cached) {
        get_device_name(cached_device_name);
        device_name_cached = true;
    }

    switch (event) {
    case ESP_GATTS_REG_EVT:
        ESP_LOGI(TAG, "GATT register, status %d, app_id %d, if %d",
                 param->reg.status, param->reg.app_id, gatts_if);

        gl_profile_tab[PROFILE_A_APP_ID].service_id.is_primary    = true;
        gl_profile_tab[PROFILE_A_APP_ID].service_id.id.inst_id    = 0x00;
        gl_profile_tab[PROFILE_A_APP_ID].service_id.id.uuid.len   = ESP_UUID_LEN_128;
        memcpy(gl_profile_tab[PROFILE_A_APP_ID].service_id.id.uuid.uuid.uuid128,
               service_uuid, 16);

        /* Clean state */
        gl_profile_tab[PROFILE_A_APP_ID].conn_id = INVALID_CONN_ID;
        s_connected = false;
        s_cccd_enabled = false;
        s_cccd_indications = false;
        s_congested = false;

        esp_err_t set_dev_name_ret = esp_ble_gap_set_device_name(device_name);
        if (set_dev_name_ret) {
            ESP_LOGE(TAG, "set name failed, code=%x", set_dev_name_ret);
        }

        esp_err_t ret = esp_ble_gap_config_adv_data(&adv_data);
        if (ret) ESP_LOGE(TAG, "config adv data failed, code=%x", ret);
        adv_config_done |= adv_config_flag;

        ret = esp_ble_gap_config_adv_data(&scan_rsp_data);
        if (ret) ESP_LOGE(TAG, "config scan rsp failed, code=%x", ret);
        adv_config_done |= scan_rsp_config_flag;

        /* Start with minimal dev_ctrl-only service (saves memory) */
        dev_msg_mode_active = false;
        create_dev_ctrl_service(gatts_if);
        break;

    case ESP_GATTS_READ_EVT:
        ESP_LOGI(TAG, "Dev_ctrl read, conn_id %d, trans_id %" PRIu32 ", handle %d",
                 param->read.conn_id, param->read.trans_id, param->read.handle);
        
        if (param->read.handle == gl_profile_tab[PROFILE_A_APP_ID].char_handle) {
            /* Return current 4-byte dev_ctrl data */
            memset(&rsp, 0, sizeof(rsp));
            rsp.attr_value.handle = param->read.handle;
            rsp.attr_value.len = 4;

            memcpy(rsp.attr_value.value, dev_ctrl_data, 4);
            
            ESP_LOGI(TAG, "Returning dev_ctrl data: [%02x][%02x][%02x][%02x]", 
                     dev_ctrl_data[0], dev_ctrl_data[1], dev_ctrl_data[2], dev_ctrl_data[3]);
            
            esp_ble_gatts_send_response(gatts_if, param->read.conn_id, param->read.trans_id,
                                        ESP_GATT_OK, &rsp);
        } else if (param->read.handle == dev_msg_serial_number_handle) {
            /* Return serial number data */
            memset(&rsp, 0, sizeof(rsp));
            rsp.attr_value.handle = param->read.handle;
            rsp.attr_value.len = strlen((char*)dev_msg_serial_number_data);

            memcpy(rsp.attr_value.value, dev_msg_serial_number_data, rsp.attr_value.len);

            ESP_LOGI(TAG, "Returning serial number: %s", (char*)dev_msg_serial_number_data);

            esp_ble_gatts_send_response(gatts_if, param->read.conn_id, param->read.trans_id,
                                        ESP_GATT_OK, &rsp);
        } else {
            /* Unknown handle */
            esp_ble_gatts_send_response(gatts_if, param->read.conn_id, param->read.trans_id,
                                        ESP_GATT_READ_NOT_PERMIT, NULL);
        }
        break;

    case ESP_GATTS_WRITE_EVT: {
        ESP_LOGI(TAG, "📝 BLE Write Event: conn_id=%d, handle=%d, len=%d",
                 param->write.conn_id, param->write.handle, param->write.len);

        /* Handle dev_ctrl characteristic write (4-byte format) */
        if (!param->write.is_prep && 
            param->write.handle == gl_profile_tab[PROFILE_A_APP_ID].char_handle) {

            esp_ble_gatts_send_response(gatts_if, param->write.conn_id, 
                                        param->write.trans_id, ESP_GATT_OK, NULL);
            
            if (param->write.len == 1) {
                /* Single byte write = control command to byte 4 */
                command = param->write.value[0];
                const char *cmd_name = "UNKNOWN";
                switch (command) {
                    case BLE_CMD_START_BINDING:       cmd_name = "START_BINDING"; break;
                    case BLE_CMD_START_FULL_SYNC:     cmd_name = "START_FULL_SYNC"; break;
                    case BLE_CMD_START_CONTENT_SYNC:  cmd_name = "START_CONTENT_SYNC"; break;
                    case BLE_CMD_CHECK_CONNECTION:    cmd_name = "CHECK_CONNECTION"; break;
                    case BLE_CMD_ENABLE_MSG:          cmd_name = "ENABLE_MSG"; break;
                    case BLE_CMD_DISABLE_MSG:         cmd_name = "DISABLE_MSG"; break;
                    case BLE_CMD_STATUS_REQ:          cmd_name = "STATUS_REQ"; break;
                    case BLE_CMD_SYNC_STATUS_REQ:     cmd_name = "SYNC_STATUS_REQ"; break;
                }
                ESP_LOGI(TAG, "🎯 Command received: %s (0x%02x)", cmd_name, command);
                dev_ctrl_handle_command(command);
            } else if (param->write.len == 4) {
                /* 4-byte write = full status update */
                command = param->write.value[3];
                const char *cmd_name = "UNKNOWN";
                switch (command) {
                    case BLE_CMD_START_BINDING:       cmd_name = "START_BINDING"; break;
                    case BLE_CMD_START_FULL_SYNC:     cmd_name = "START_FULL_SYNC"; break;
                    case BLE_CMD_START_CONTENT_SYNC:  cmd_name = "START_CONTENT_SYNC"; break;
                    case BLE_CMD_CHECK_CONNECTION:    cmd_name = "CHECK_CONNECTION"; break;
                    case BLE_CMD_ENABLE_MSG:          cmd_name = "ENABLE_MSG"; break;
                    case BLE_CMD_DISABLE_MSG:         cmd_name = "DISABLE_MSG"; break;
                    case BLE_CMD_STATUS_REQ:          cmd_name = "STATUS_REQ"; break;
                    case BLE_CMD_SYNC_STATUS_REQ:     cmd_name = "SYNC_STATUS_REQ"; break;
                }
                ESP_LOGI(TAG, "🎯 4-byte write: [battery=%d, screen=%d, msg=0x%02x, cmd=%s(0x%02x)]",
                         param->write.value[0], param->write.value[1], 
                         param->write.value[2], cmd_name, command);
                dev_ctrl_handle_command(param->write.value[3]); // Control byte
            }

            break;
        }

        /* Handle dev_msg characteristics writes (dev_msg mode) */
        if (!param->write.is_prep && dev_msg_mode_active) {
            char data_str[BLE_SERVICE_MAX_LEN] = {0};
            int data_len = (param->write.len >= sizeof(data_str)) ? (sizeof(data_str) - 1) : param->write.len;
            memcpy(data_str, param->write.value, data_len);
            data_str[data_len] = '\0';

            handled = handle_dev_msg_write(param->write.handle, data_str, data_len);

            
            if (handled) {
                esp_ble_gatts_send_response(gatts_if, param->write.conn_id,
                                          param->write.trans_id, ESP_GATT_OK, NULL);
                break;
            }
        }

        /* CCCD handling for notifications (len==2) */
        if (!param->write.is_prep && param->write.len == 2) {
            cccd = (uint16_t)param->write.value[1] << 8 | param->write.value[0];

            if (gl_profile_tab[PROFILE_A_APP_ID].descr_handle == param->write.handle) {
                /* Dev_ctrl CCCD */
                if (cccd == 0x0001) {
                    s_cccd_enabled = true;
                    s_cccd_indications = false;
                    ESP_LOGI(TAG, "🔔 CCCD: Notifications ENABLED for dev_ctrl (handle %d)", param->write.handle);

                    /* Send initial 4-byte status immediately */
                    dev_ctrl_update_values(get_current_screen(), get_current_msg(), gPixseeStatus);

                } else if (cccd == 0x0000) {
                    s_cccd_enabled = false;
                    s_cccd_indications = false;
                    ESP_LOGI(TAG, "🔕 CCCD: Notifications DISABLED for dev_ctrl (handle %d)", param->write.handle);
                }

                esp_ble_gatts_send_response(gatts_if, param->write.conn_id,
                                            param->write.trans_id, ESP_GATT_OK, NULL);
                break;
            }
        }

        /* Fallback for unhandled writes */
        if(!param->write.is_prep)
        {
            ESP_LOGW(TAG, "⚠️ Unhandled write to handle %d (len=%d, dev_ctrl_handle=%d, descr_handle=%d)",
                     param->write.handle, param->write.len,
                     gl_profile_tab[PROFILE_A_APP_ID].char_handle,
                     gl_profile_tab[PROFILE_A_APP_ID].descr_handle);
            if (param->write.len > 0 && param->write.len <= 16) {
                ESP_LOG_BUFFER_HEX(TAG, param->write.value, param->write.len);
            }
            esp_ble_gatts_send_response(gatts_if, param->write.conn_id,
                                        param->write.trans_id, ESP_GATT_WRITE_NOT_PERMIT, NULL);
        }

        if(param->write.is_prep == true)
        {

            int chunk_size = param->write.len;
            
            if (transfer_start == 0) {
                transfer_start = esp_timer_get_time();
                last_chunk_time = transfer_start;
                total_bytes = 0;
            }
            
            total_bytes += chunk_size;
            
            uint64_t now = esp_timer_get_time();
            double elapsed_chunk = (now - last_chunk_time) / 1000000.0; // segundos
            if (elapsed_chunk > 0) {
                double chunk_speed = chunk_size / elapsed_chunk;
                ESP_LOGW(TAG, "Received Chunk: %d bytes em %.4f s (%.2f B/s) - (%.2f KB/s)", 
                    chunk_size, elapsed_chunk, chunk_speed, (chunk_speed / 1024.00));
            }

            last_chunk_time = now;
            write_event_env(gatts_if, &service_prepare_write_env, param);
        }
        break;
    }

    case ESP_GATTS_EXEC_WRITE_EVT:
        ESP_LOGW(TAG, "Execute write");
        esp_ble_gatts_send_response(gatts_if, param->write.conn_id,
                                    param->write.trans_id, ESP_GATT_OK, NULL);
        handle_exec_write_from_ble_service(&service_prepare_write_env, param, gatts_if);
        exec_write_event_env(&service_prepare_write_env, param);

        transfer_start = 0;
        total_bytes = 0;
        last_chunk_time = 0;
        break;

    case ESP_GATTS_MTU_EVT:
        ESP_LOGI(TAG, "MTU %d", param->mtu.mtu);
        break;

    case ESP_GATTS_CREATE_EVT:
        if (param->create.status == ESP_GATT_OK) {
            ESP_LOGI(TAG, "✅ Service created successfully: handle %d, mode=%s",
                     param->create.service_handle,
                     dev_msg_mode_active ? "full" : "dev_ctrl_only");
        } else {
            ESP_LOGE(TAG, "❌ Service creation failed: status %d", param->create.status);
        }

        gl_profile_tab[PROFILE_A_APP_ID].service_handle = param->create.service_handle;
        gl_profile_tab[PROFILE_A_APP_ID].char_uuid.len = ESP_UUID_LEN_128;
        memcpy(gl_profile_tab[PROFILE_A_APP_ID].char_uuid.uuid.uuid128, char_uuid, 16);

        /* Reset recreation flag */
        s_service_recreating = false;
        
        ESP_LOGI(TAG, "🚀 Starting GATT service...");
        esp_err_t start_ret = esp_ble_gatts_start_service(gl_profile_tab[PROFILE_A_APP_ID].service_handle);
        if (start_ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to start service: %s", esp_err_to_name(start_ret));
        }
        a_property = ESP_GATT_CHAR_PROP_BIT_READ |
                     ESP_GATT_CHAR_PROP_BIT_WRITE |
                     ESP_GATT_CHAR_PROP_BIT_NOTIFY;

        esp_err_t add_char_ret = esp_ble_gatts_add_char(
            gl_profile_tab[PROFILE_A_APP_ID].service_handle,
            &gl_profile_tab[PROFILE_A_APP_ID].char_uuid,
            ESP_GATT_PERM_READ | ESP_GATT_PERM_WRITE,
            a_property,
            &gatts_demo_char1_val, NULL);
        if (add_char_ret) {
            ESP_LOGE(TAG, "add char failed, code=%x", add_char_ret);
        }
        break;

    case ESP_GATTS_ADD_CHAR_EVT:
        ESP_LOGI(TAG, "Char added, status %d, attr_handle %d, svc_handle %d",
                 param->add_char.status, param->add_char.attr_handle, param->add_char.service_handle);

        /* Track which characteristic was added and create them all in sequence */
        if (gl_profile_tab[PROFILE_A_APP_ID].char_handle == 0) {
            /* This is the main dev_ctrl characteristic */
            gl_profile_tab[PROFILE_A_APP_ID].char_handle = param->add_char.attr_handle;
            ESP_LOGI(TAG, "Dev_ctrl char handle: %d", param->add_char.attr_handle);
            
            /* Add dev_ctrl CCCD descriptor */
            gl_profile_tab[PROFILE_A_APP_ID].descr_uuid.len = ESP_UUID_LEN_16;
            gl_profile_tab[PROFILE_A_APP_ID].descr_uuid.uuid.uuid16 = ESP_GATT_UUID_CHAR_CLIENT_CONFIG;
            esp_err_t add_descr_ret = esp_ble_gatts_add_char_descr(
                gl_profile_tab[PROFILE_A_APP_ID].service_handle,
                &gl_profile_tab[PROFILE_A_APP_ID].descr_uuid,
                ESP_GATT_PERM_READ | ESP_GATT_PERM_WRITE,
                NULL, NULL);
            if (add_descr_ret) {
                ESP_LOGE(TAG, "add dev_ctrl descr failed, code=%x", add_descr_ret);
            }
            
        } else if (dev_msg_serial_number_handle == 0) {
            /* Serial Number characteristic */
            dev_msg_serial_number_handle = param->add_char.attr_handle;
            ESP_LOGI(TAG, "Serial Number char handle: %d", dev_msg_serial_number_handle);

            /* Add WiFi SSID characteristic */
            char_uuid_temp.len = ESP_UUID_LEN_128;
            memcpy(char_uuid_temp.uuid.uuid128, wifi_ssid_uuid, 16);

            char_val_temp.attr_max_len = 32;
            char_val_temp.attr_len = 1;
            char_val_temp.attr_value = dev_msg_wifi_ssid_data;

            ret = esp_ble_gatts_add_char(
                gl_profile_tab[PROFILE_A_APP_ID].service_handle,
                &char_uuid_temp,
                ESP_GATT_PERM_READ | ESP_GATT_PERM_WRITE,
                ESP_GATT_CHAR_PROP_BIT_READ | ESP_GATT_CHAR_PROP_BIT_WRITE,
                &char_val_temp,
                NULL
            );

            if (ret != ESP_OK) {
                ESP_LOGE(TAG, "Failed to add WiFi SSID char: %s", esp_err_to_name(ret));
            }

        } else if (dev_msg_wifi_ssid_handle == 0) {
            /* WiFi SSID characteristic */
            dev_msg_wifi_ssid_handle = param->add_char.attr_handle;
            ESP_LOGI(TAG, "WiFi SSID char handle: %d", dev_msg_wifi_ssid_handle);

            /* Add WiFi Password characteristic */
            char_uuid_temp.len = ESP_UUID_LEN_128;
            memcpy(char_uuid_temp.uuid.uuid128, wifi_password_uuid, 16);

            char_val_temp.attr_max_len = 32;
            char_val_temp.attr_len = 1;
            char_val_temp.attr_value = dev_msg_wifi_password_data;

            ret = esp_ble_gatts_add_char(
                gl_profile_tab[PROFILE_A_APP_ID].service_handle,
                &char_uuid_temp,
                ESP_GATT_PERM_READ | ESP_GATT_PERM_WRITE,
                ESP_GATT_CHAR_PROP_BIT_READ | ESP_GATT_CHAR_PROP_BIT_WRITE,
                &char_val_temp,
                NULL
            );

            if (ret != ESP_OK) {
                ESP_LOGE(TAG, "Failed to add WiFi Password char: %s", esp_err_to_name(ret));
            }


        } else if (dev_msg_wifi_password_handle == 0) {
            /* WiFi Password characteristic */
            dev_msg_wifi_password_handle = param->add_char.attr_handle;
            ESP_LOGI(TAG, "WiFi Password char handle: %d", dev_msg_wifi_password_handle);
            
            /* Add Secret Key characteristic */
            char_uuid_temp.len = ESP_UUID_LEN_128;
            memcpy(char_uuid_temp.uuid.uuid128, secret_key_uuid, 16);
            
            char_val_temp.attr_max_len = SECRET_KEY_STR_SIZE;
            char_val_temp.attr_len = 1;
            char_val_temp.attr_value = dev_msg_secret_key_data;
            
            ret = esp_ble_gatts_add_char(
                gl_profile_tab[PROFILE_A_APP_ID].service_handle,
                &char_uuid_temp,
                ESP_GATT_PERM_READ | ESP_GATT_PERM_WRITE,
                ESP_GATT_CHAR_PROP_BIT_READ | ESP_GATT_CHAR_PROP_BIT_WRITE,
                &char_val_temp,
                NULL
            );
            
            if (ret != ESP_OK) {
                ESP_LOGE(TAG, "Failed to add Secret Key char: %s", esp_err_to_name(ret));
            }
            
        } else if (dev_msg_secret_key_handle == 0) {
            /* Secret Key characteristic */
            dev_msg_secret_key_handle = param->add_char.attr_handle;
            ESP_LOGI(TAG, "Secret Key char handle: %d", dev_msg_secret_key_handle);
            
            /* Add Timezone characteristic */
            char_uuid_temp.len = ESP_UUID_LEN_128;
            memcpy(char_uuid_temp.uuid.uuid128, timezone_uuid, 16);
            
            char_val_temp.attr_max_len = 8;
            char_val_temp.attr_len = 1;
            char_val_temp.attr_value = dev_msg_timezone_data;
            
            ret = esp_ble_gatts_add_char(
                gl_profile_tab[PROFILE_A_APP_ID].service_handle,
                &char_uuid_temp,
                ESP_GATT_PERM_READ | ESP_GATT_PERM_WRITE,
                ESP_GATT_CHAR_PROP_BIT_READ | ESP_GATT_CHAR_PROP_BIT_WRITE,
                &char_val_temp,
                NULL
            );
            
            if (ret != ESP_OK) {
                ESP_LOGE(TAG, "Failed to add Timezone char: %s (0x%x)", esp_err_to_name(ret), ret);
                ESP_LOGE(TAG, "GATT handlers may be exhausted - check GATT_SERVICE_HANDLERS count");
            }
        } else if (dev_msg_timezone_handle == 0) {
            /* Timezone characteristic */
            dev_msg_timezone_handle = param->add_char.attr_handle;
            ESP_LOGI(TAG, "Timezone char handle: %d", dev_msg_timezone_handle);

            ESP_LOGI(TAG, "All 5 dev_msg characteristics added successfully (serial, ssid, password, secret_key, timezone)");
        }
        break;

    case ESP_GATTS_ADD_CHAR_DESCR_EVT:
        ESP_LOGI(TAG, "Descr added, status %d, attr_handle %d, svc_handle %d",
                 param->add_char_descr.status,
                 param->add_char_descr.attr_handle,
                 param->add_char_descr.service_handle);

        /* This is the main dev_ctrl CCCD */
        if (gl_profile_tab[PROFILE_A_APP_ID].descr_handle == 0) {
            gl_profile_tab[PROFILE_A_APP_ID].descr_handle = param->add_char_descr.attr_handle;
            ESP_LOGI(TAG, "Dev_ctrl CCCD handle: %d", param->add_char_descr.attr_handle);
        }
        
        /* After dev_ctrl CCCD is added, add dev_msg characteristics if in full mode */
        if (dev_msg_mode_active && dev_msg_serial_number_handle == 0) {
            ESP_LOGI(TAG, "Adding dev_msg characteristics (full service mode)");

            /* Add Serial Number characteristic first (read-only) */
            char_uuid_temp.len = ESP_UUID_LEN_128;
            memcpy(char_uuid_temp.uuid.uuid128, serial_number_uuid, 16);

            /* Load serial number from storage */
            char serial_number[32] = {0};
            esp_err_t sn_ret = read_serial_number(serial_number);
            if (sn_ret == ESP_OK && strlen(serial_number) > 0) {
                strcpy((char*)dev_msg_serial_number_data, serial_number);
            } else {
                strcpy((char*)dev_msg_serial_number_data, "UNKNOWN");
            }

            char_val_temp.attr_max_len = 32;
            char_val_temp.attr_len = strlen((char*)dev_msg_serial_number_data);
            char_val_temp.attr_value = dev_msg_serial_number_data;

            ret = esp_ble_gatts_add_char(
                gl_profile_tab[PROFILE_A_APP_ID].service_handle,
                &char_uuid_temp,
                ESP_GATT_PERM_READ,  // Read-only for app use
                ESP_GATT_CHAR_PROP_BIT_READ,
                &char_val_temp,
                NULL
            );

            if (ret != ESP_OK) {
                ESP_LOGE(TAG, "Failed to add Serial Number char: %s", esp_err_to_name(ret));
            }
        }
        break;

    case ESP_GATTS_START_EVT:
        if (param->start.status == ESP_GATT_OK) {
            ESP_LOGI(TAG, "✅ Service started successfully: handle %d", param->start.service_handle);
        } else {
            ESP_LOGE(TAG, "❌ Service start failed: status %d, handle %d",
                     param->start.status, param->start.service_handle);
        }
        break;

    case ESP_GATTS_CONNECT_EVT:
        // note: Restart timeout on user interaction and ensure backlight is on for user feedback
        app_timeout_restart();
        backlight_on();

        memset(&conn_params, 0, sizeof(conn_params));
        memcpy(conn_params.bda, param->connect.remote_bda, sizeof(esp_bd_addr_t));
        conn_params.latency = 0;
        conn_params.max_int = 0x20;    // 40ms
        conn_params.min_int = 0x10;    // 20ms
        conn_params.timeout = 400;     // 4000ms

        ESP_LOGI(TAG, "✅ Connected, conn_id %u, remote " ESP_BD_ADDR_STR "",
                 param->connect.conn_id, ESP_BD_ADDR_HEX(param->connect.remote_bda));

        // Log GATT service status for debugging
        ESP_LOGI(TAG, "📋 GATT Service Status: service_handle=%d, char_handle=%d, descr_handle=%d, gatts_if=%d",
                 gl_profile_tab[PROFILE_A_APP_ID].service_handle,
                 gl_profile_tab[PROFILE_A_APP_ID].char_handle,
                 gl_profile_tab[PROFILE_A_APP_ID].descr_handle,
                 gl_profile_tab[PROFILE_A_APP_ID].gatts_if);

        if (gl_profile_tab[PROFILE_A_APP_ID].service_handle == 0) {
            ESP_LOGE(TAG, "⚠️ WARNING: Service not created! This will cause connection issues.");
        }

        gl_profile_tab[PROFILE_A_APP_ID].conn_id = param->connect.conn_id;
        s_connected = true;
        // CCCD will be set by client

        // Reset status to IDLE when APP connects, allowing repeated sync operations
        // Otherwise APP sees previous SYNC_SUCCESS status and won't trigger new sync
        ESP_LOGI(TAG, "Resetting status from 0x%02x to S3ER_SYSTEM_IDLE for new sync session", gPixseeStatus);
        set_pixsee_status(S3ER_SYSTEM_IDLE);

        esp_ble_gap_update_conn_params(&conn_params);

        // Notify coexistence manager about connection state change
        s3_bt_trigger_coexistence_update();
        break;

    case ESP_GATTS_DISCONNECT_EVT:
        // Log disconnect reason with human-readable explanation
        {
            const char *reason_str = "UNKNOWN";
            switch (param->disconnect.reason) {
                case 0x08: reason_str = "CONNECTION_TIMEOUT"; break;
                case 0x13: reason_str = "REMOTE_USER_TERMINATED"; break;
                case 0x16: reason_str = "CONNECTION_TERMINATED_BY_LOCAL_HOST"; break;
                case 0x3E: reason_str = "CONNECTION_FAILED_TO_ESTABLISH"; break;
                case ESP_GATT_CONN_UNKNOWN: reason_str = "CONN_UNKNOWN"; break;
                case ESP_GATT_CONN_L2C_FAILURE: reason_str = "L2C_FAILURE"; break;
                case ESP_GATT_CONN_LMP_TIMEOUT: reason_str = "LMP_TIMEOUT"; break;
                case ESP_GATT_CONN_CONN_CANCEL: reason_str = "CONN_CANCEL"; break;
                case ESP_GATT_CONN_NONE: reason_str = "CONN_NONE"; break;
                default: reason_str = "UNKNOWN"; break;
            }
            ESP_LOGI(TAG, "❌ Disconnected: " ESP_BD_ADDR_STR ", reason=0x%02x (%s)",
                     ESP_BD_ADDR_HEX(param->disconnect.remote_bda),
                     param->disconnect.reason, reason_str);
        }

        gl_profile_tab[PROFILE_A_APP_ID].conn_id = INVALID_CONN_ID;
        s_connected = false;
        s_cccd_enabled = false;
        s_cccd_indications = false;
        s_congested = false;

        // Only restart advertising if BT Classic is not connected or streaming
        if (!s3_bt_classic_is_connected() && !s3_bt_classic_is_streaming()) {
            esp_ble_gap_start_advertising(&adv_params);
        } else {
            ESP_LOGI(TAG, "Not restarting BLE advertising - BT Classic is connected/streaming");
        }
        set_pixsee_status(S3ER_SYSTEM_IDLE);

        // Notify coexistence manager about connection state change
        s3_bt_trigger_coexistence_update();

        break;

    case ESP_GATTS_CONF_EVT:
        ESP_LOGI(TAG, "✅ Notification confirmed by APP: handle %d, status %d (APP received our data)",
                 param->conf.handle, param->conf.status);
        if (param->conf.status != ESP_GATT_OK && param->conf.len > 0) {
            dump_len = param->conf.len > 32 ? 32 : param->conf.len;
            ESP_LOGI(TAG, "conf len %d (dump %d)", param->conf.len, dump_len);
            ESP_LOG_BUFFER_HEX(TAG, param->conf.value, dump_len);
        }
        break;

    case ESP_GATTS_CONGEST_EVT:
        s_congested = param->congest.congested;
        ESP_LOGI(TAG, "GATT congested = %d", s_congested);
        break;

    default:
        break;
    }
}

/* ========================= S3 API RELATED ========================= */

void stop_binding(void)
{
    is_pixsee_binding = false;
    
    if (oob_pairing_task_handle != NULL)
    {
        eTaskState state = eTaskGetState(oob_pairing_task_handle);
        if (state != eDeleted)
        {
            ESP_LOGI(TAG, "Stopping OOB pairing task");
            state = eTaskGetState(oob_pairing_task_handle);
            if (state != eDeleted) {
                ESP_LOGW(TAG, "Force deleting OOB pairing task");
                vTaskDelete(oob_pairing_task_handle);
            }
            oob_pairing_task_handle = NULL;
        }
        else
        {
            ESP_LOGI(TAG, "OOB pairing task already terminated");
            oob_pairing_task_handle = NULL;
        }
    }
}

void start_binding(void)
{
    if(oob_pairing_task_handle == NULL)
    {
        is_pixsee_binding = true;
        xTaskCreatePinnedToCore(oob_pairing_task, "oob_pairing_task", (5 * 1024), NULL, 2, &oob_pairing_task_handle, 0);
    }
}

void dev_msg_enable_characteristics(void)
{
    is_pixsee_binding = true;
    recreate_service(true); // Recreate service with dev_msg characteristics
    ESP_LOGI(TAG, "Dev_msg mode activated - service recreated with characteristics");
}

void dev_msg_disable_characteristics(void)
{
    is_pixsee_binding = false;
    recreate_service(false); // Recreate minimal service (saves memory)
    ESP_LOGI(TAG, "Dev_msg mode disabled - service recreated in minimal mode");
}

/* ========================= BLE INIT ========================= */
// ble_init_task moved to s3_bluetooth.c for unified component management

esp_err_t s3_ble_manager_init(void)
{
    ESP_LOGI(TAG, "Initializing S3 BLE Manager");
    esp_err_t ret;

    /* Clean initial state */
    gl_profile_tab[PROFILE_A_APP_ID].conn_id = INVALID_CONN_ID;
    s_connected = false;
    s_cccd_enabled = false;
    s_cccd_indications = false;
    s_congested = false;

    /* Initialize pre-allocated GATT response mutex */
    if (gatt_rsp_mutex == NULL) {
        gatt_rsp_mutex = xSemaphoreCreateMutex();
        if (gatt_rsp_mutex == NULL) {
            ESP_LOGE(TAG, "Failed to create GATT response mutex");
        }
    }

    /* Check if Bluetooth controller is already initialized (by ESP-ADF bluetooth_service) */
    esp_bt_controller_status_t ctrl_status = esp_bt_controller_get_status();
    if (ctrl_status == ESP_BT_CONTROLLER_STATUS_IDLE) {
        ESP_LOGI(TAG, "Initializing Bluetooth controller (not yet initialized)");
        esp_bt_controller_config_t bt_cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
        bt_cfg.mode = ESP_BT_MODE_BTDM;
        ret = esp_bt_controller_init(&bt_cfg);
        if (ret) {
            ESP_LOGE(TAG, "%s controller init failed: %s", __func__, esp_err_to_name(ret));
            return ret;
        }

        ret = esp_bt_controller_enable(ESP_BT_MODE_BTDM);
        if (ret) {
            ESP_LOGE(TAG, "%s controller enable failed: %s", __func__, esp_err_to_name(ret));
            return ret;
        }
    } else {
        ESP_LOGI(TAG, "Bluetooth controller already initialized (status: %d), reusing existing controller", ctrl_status);
    }

    /* Check if Bluedroid is already initialized (by ESP-ADF bluetooth_service) */
    esp_bluedroid_status_t bluedroid_status = esp_bluedroid_get_status();
    if (bluedroid_status == ESP_BLUEDROID_STATUS_UNINITIALIZED) {
        ESP_LOGI(TAG, "Initializing Bluedroid (not yet initialized)");
        ret = esp_bluedroid_init();
        if (ret) {
            ESP_LOGE(TAG, "%s bluedroid init failed: %s", __func__, esp_err_to_name(ret));
            return ret;
        }
        ret = esp_bluedroid_enable();
        if (ret) {
            ESP_LOGE(TAG, "%s bluedroid enable failed: %s", __func__, esp_err_to_name(ret));
            return ret;
        }
    } else {
        ESP_LOGI(TAG, "Bluedroid already initialized (status: %d), reusing existing Bluedroid", bluedroid_status);
    }

    ret = esp_ble_gatts_register_callback(gatts_event_handler);
    if (ret) {
        ESP_LOGE(TAG, "gatts register error, code=%x", ret);
        return ret;
    }
    ret = esp_ble_gap_register_callback(gap_event_handler);
    if (ret) {
        ESP_LOGE(TAG, "gap register error, code=%x", ret);
        return ret;
    }
    ret = esp_ble_gatts_app_register(PROFILE_A_APP_ID);
    if (ret) {
        ESP_LOGE(TAG, "gatts app register error, code=%x", ret);
        return ret;
    }

    /* IMPORTANT: reduce local MTU to reduce internal BT stack pressure */
    // esp_err_t local_mtu_ret = esp_ble_gatt_set_local_mtu(247); // (was 500)
    esp_err_t local_mtu_ret = esp_ble_gatt_set_local_mtu(185); // (was 500)
    if (local_mtu_ret) {
        ESP_LOGE(TAG, "set local MTU failed, code=%x", local_mtu_ret);
    }

    s3_ble_ready = true;
    ESP_LOGI(TAG, "S3 BLE Manager initialization complete, setting s3_ble_ready = true");
    return ESP_OK;
}

/* ========================= DEVICE CONTROL GATT FUNCTIONS ========================= */

/* Update values efficiently - only update non-negative values */
/* Updated status word system: [Screen][Message][Status][Control] */
void dev_ctrl_update_values(int screen, int msg, int status)
{
    // OPTIMIZATION: Early return if no BLE client connected - avoids unnecessary updates
    // This prevents DMA conflicts with SD card operations when BLE is not in use
    if (!s_connected) {
        ESP_LOGD(TAG, "BLE not connected - skipping dev_ctrl update");
        return;
    }

    bool changed = false;

    if (screen >= 0 && dev_ctrl_data[0] != (uint8_t)screen) {
        dev_ctrl_data[0] = (uint8_t)screen;
        changed = true;
        ESP_LOGW(TAG, "Screen updated: [%d]", screen);
    }

    if (msg >= 0 && dev_ctrl_data[1] != (uint8_t)msg) {
        dev_ctrl_data[1] = (uint8_t)msg;
        changed = true;
        ESP_LOGW(TAG, "System msg updated: [0x%02X]", msg);
    }

    if (status >= 0 && dev_ctrl_data[2] != (uint8_t)status) {
        dev_ctrl_data[2] = (uint8_t)status;
        changed = true;
        ESP_LOGW(TAG, "System status updated: [0x%02X]", status);
    }

    if (changed) {
        dev_ctrl_sync_gatt_server();
    }
}

/* Handle sync status request - responds with OTA/syncing/completed status */
void handle_sync_status_request(void)
{
    extern bool gSyncInProgress;

    uint8_t sync_status = S3ER_SYNC_STATUS_COMPLETED; // Default: completed/idle

    // Priority 1: Check if OTA is in progress
    if (gOTA_in_progress) {
        sync_status = S3ER_SYNC_STATUS_OTA_IN_PROGRESS;
        ESP_LOGI(TAG, "Sync status query: OTA in progress");
    }
    // Priority 2: Check if unified_sync_task is running OR gSyncInProgress flag is set
    else if (wifi_connecting_task_handle != NULL || gSyncInProgress) {
        sync_status = S3ER_SYNC_STATUS_DATA_SYNCING;
        ESP_LOGI(TAG, "Sync status query: Data sync in progress (task=%s, flag=%d)",
                 wifi_connecting_task_handle ? "RUNNING" : "NULL", gSyncInProgress);
    }
    // Priority 3: Idle/completed
    else {
        ESP_LOGI(TAG, "Sync status query: Sync completed/idle");
    }

    // Update status and send notification
    set_pixsee_status(sync_status);
}

void dev_ctrl_sync_gatt_server(void)
{
    /* Safety checks - same as old send_ble_msg_val() */
    if (!s3_ble_ready ||
        !s_connected ||
        gl_profile_tab[PROFILE_A_APP_ID].char_handle == 0 ||
        gl_profile_tab[PROFILE_A_APP_ID].gatts_if == ESP_GATT_IF_NONE ||
        gl_profile_tab[PROFILE_A_APP_ID].conn_id == INVALID_CONN_ID) {
        ESP_LOGD(TAG, "BLE not ready for dev_ctrl sync");
        return;
    }

    /* DMA COORDINATION: Take SD card DMA mutex to prevent hardware conflicts
     * BLE GATT notifications use DMA which conflicts with SDMMC DMA reads
     * Short timeout (10ms) - SD card operations take longer, BLE notifications are fast
     * If SD card is busy, defer this notification (next update will catch it)
     */
    bool mutex_taken = false;
    if (g_sdcard_dma_mutex) {
        mutex_taken = (xSemaphoreTake(g_sdcard_dma_mutex, pdMS_TO_TICKS(10)) == pdTRUE);
        if (!mutex_taken) {
            ESP_LOGD(TAG, "SD card DMA busy - deferring BLE notification");
            return;  // Skip this notification cycle if SD card DMA is active
        }
    }

    /* Update the GATT attribute value */
    esp_err_t ret = esp_ble_gatts_set_attr_value(
        gl_profile_tab[PROFILE_A_APP_ID].char_handle,
        4,
        dev_ctrl_data
    );

    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to update GATT attribute: %s", esp_err_to_name(ret));
        if (mutex_taken && g_sdcard_dma_mutex) {
            xSemaphoreGive(g_sdcard_dma_mutex);
        }
        return;
    }

    /* Send notification if enabled */
    if (s_cccd_enabled && !s_congested) {
        esp_err_t err = esp_ble_gatts_send_indicate(
            gl_profile_tab[PROFILE_A_APP_ID].gatts_if,
            gl_profile_tab[PROFILE_A_APP_ID].conn_id,
            gl_profile_tab[PROFILE_A_APP_ID].char_handle,
            4,
            dev_ctrl_data,
            false  // notification
        );

        if (err == ESP_OK) {
            ESP_LOGI(TAG, "📤 Sent to APP: [battery=%d, msg=0x%02x, status=0x%02x, reserved=0x%02x]",
                     dev_ctrl_data[0], dev_ctrl_data[1], dev_ctrl_data[2], dev_ctrl_data[3]);
        } else {
            ESP_LOGE(TAG, "Failed to send notification: %s", esp_err_to_name(err));
        }
    }

    /* Release DMA mutex */
    if (mutex_taken && g_sdcard_dma_mutex) {
        xSemaphoreGive(g_sdcard_dma_mutex);
    }
}

void dev_ctrl_handle_command(uint8_t command)
{
    ESP_LOGI(TAG, "Processing dev_ctrl command: 0x%02x", command);
    uint8_t cmd_answer = command + 1; // Default to command + 1 (success)
    dev_ctrl_update_values(NO_UPDATE, NO_UPDATE, cmd_answer); // Update control byte only
    app_timeout_restart(); // Restart timeout on user interaction
    backlight_on(); // Ensure backlight is on for user feedback
    switch (command) {
        case BLE_CMD_START_BINDING:
            ESP_LOGI(TAG, "Start binding command received");
            set_current_screen(HOME_SCREEN, NULL_SCREEN);
            vTaskDelay(pdMS_TO_TICKS(500));
            start_binding();
            break;

        case BLE_CMD_CHECK_CONNECTION:
            ESP_LOGI(TAG, "WiFi connection check command received");

            // Test WiFi connection with stored credentials
            esp_err_t check_result = setup_wifi(USE_NVS_CREDENTIALS);

            // Disconnect WiFi immediately after checking - this is just a connectivity test
            if (check_result == ESP_OK) {
                ESP_LOGI(TAG, "WiFi check successful, disconnecting...");
                dev_ctrl_update_values(NO_UPDATE, NO_UPDATE, S3ER_SETUP_CHANGE_WIFI_SUCCESS);
                deinit_wifi_station();
            } else {
                ESP_LOGW(TAG, "WiFi check failed");
                dev_ctrl_update_values(NO_UPDATE, NO_UPDATE, S3ER_SETUP_CHANGE_WIFI_FAIL);
            }
            break;

        case BLE_CMD_START_CONTENT_SYNC:
            ESP_LOGI(TAG, "Start content sync command received");

            if (gSyncInProgress) {
                ESP_LOGW(TAG, "Sync already in progress - ignoring content sync command");
                dev_ctrl_update_values(NO_UPDATE, NO_UPDATE, S3ER_SYNCING);
            } else {
                // Trigger NFC sync (content-only sync)
                extern void nfc_sync_cmd(void);
                nfc_sync_cmd();
            }
            break;

        case BLE_CMD_START_FULL_SYNC:
            (get_current_screen() == PLAY_SCREEN) ? app_state_handle_event(EVENT_LEAVE_PLAYING_TO_HOME): \
                                                    set_current_screen(HOME_SCREEN, NULL_SCREEN);
            ESP_LOGI(TAG, "Start full sync command received from BLE");
            if (gSyncInProgress) {
                ESP_LOGW(TAG, "Sync already in progress - ignoring full sync command");
                dev_ctrl_update_values(NO_UPDATE, NO_UPDATE, S3ER_SYNCING); // Update control byte only
            } else {
                // Use BLE-specific WiFi sync that returns to HOME_SCREEN after completion
                extern void start_ble_wifi_sync(void);
                start_ble_wifi_sync();
            }
            break;

        case BLE_CMD_ENABLE_MSG:
            ESP_LOGI(TAG, "Enable dev_msg mode command received");
            if (dev_msg_mode_active) {
                ESP_LOGW(TAG, "Dev_msg mode already active - ignoring command");
                break;
            }
            stop_nfc();
            recreate_service(true);  // Recreate service with dev_msg characteristics
            ESP_LOGI(TAG, "Service recreated with dev_msg characteristics");
            break;

        case BLE_CMD_DISABLE_MSG:
            ESP_LOGI(TAG, "Disable dev_msg mode command received");
            if (!dev_msg_mode_active) {
                ESP_LOGW(TAG, "Dev_msg mode already disabled - ignoring command");
                break;
            }
            start_nfc();
            recreate_service(false); // Recreate minimal service (saves memory)
            ESP_LOGI(TAG, "Service recreated in dev_ctrl-only mode (memory saved)");
            break;

        case BLE_CMD_STATUS_REQ:
            ESP_LOGI(TAG, "Status request command received");
            // Trigger notification with current status
            dev_ctrl_sync_gatt_server();
            break;

        case BLE_CMD_SYNC_STATUS_REQ:
            ESP_LOGI(TAG, "Sync status request command received");
            // Respond with current sync/OTA status
            handle_sync_status_request();
            break;

        default:
            ESP_LOGW(TAG, "Unknown dev_ctrl command: 0x%02x", command);
            break;
    }

    /* Clear control byte after processing */
    dev_ctrl_data[3] = 0x00;
    dev_ctrl_sync_gatt_server();
}

/* Legacy send_ble_msg functions completely removed - use dev_ctrl_update_values() instead */

/* ========================= DEV_MSG GATT FUNCTIONS (DEV_MSG MODE) ========================= */

/* Add dev_msg characteristics to existing service */
void add_dev_msg_characteristics(void)
{
    if (dev_msg_mode_active || 
        gl_profile_tab[PROFILE_A_APP_ID].gatts_if == ESP_GATT_IF_NONE ||
        gl_profile_tab[PROFILE_A_APP_ID].service_handle == 0) {
        ESP_LOGW(TAG, "Cannot add dev_msg chars: mode=%d, if=%d, handle=%d", 
                 dev_msg_mode_active, gl_profile_tab[PROFILE_A_APP_ID].gatts_if, 
                 gl_profile_tab[PROFILE_A_APP_ID].service_handle);
        return;
    }

    ESP_LOGI(TAG, "Adding dev_msg characteristics for WiFi configuration");
    dev_msg_mode_active = true;

    /* WiFi SSID characteristic */
    esp_bt_uuid_t ssid_uuid = {
        .len = ESP_UUID_LEN_128,
    };
    memcpy(ssid_uuid.uuid.uuid128, wifi_ssid_uuid, 16);
    esp_attr_value_t ssid_val = {
        .attr_max_len = 32,
        .attr_len = 1,
        .attr_value = dev_msg_wifi_ssid_data,
    };
    
    esp_err_t ret = esp_ble_gatts_add_char(
        gl_profile_tab[PROFILE_A_APP_ID].service_handle,
        &ssid_uuid,
        ESP_GATT_PERM_READ | ESP_GATT_PERM_WRITE,
        ESP_GATT_CHAR_PROP_BIT_READ | ESP_GATT_CHAR_PROP_BIT_WRITE,
        &ssid_val,
        NULL
    );
    
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to add WiFi SSID char: %s", esp_err_to_name(ret));
        dev_msg_mode_active = false;
    }
}

/* Remove dev_msg characteristics */
void remove_dev_msg_characteristics(void)
{
    if (!dev_msg_mode_active) {
        return;
    }

    ESP_LOGI(TAG, "Removing dev_msg characteristics, returning to dev_ctrl mode");
    
    /* Note: ESP-IDF doesn't have a direct way to remove individual characteristics
     * from a running service. The characteristics will be cleaned up when service
     * is recreated or device disconnects. For now, we just mark them as inactive.
     */
    
    dev_msg_mode_active = false;
    dev_msg_serial_number_handle = 0;
    dev_msg_wifi_ssid_handle = 0;
    dev_msg_wifi_password_handle = 0;
    dev_msg_secret_key_handle = 0;
    dev_msg_timezone_handle = 0;
    dev_msg_album_handle = 0;

    ESP_LOGI(TAG, "Dev_msg mode deactivated - dev_ctrl continues normally");
}

/* ========================= SERVICE RECREATION ========================= */

/* Recreate GATT service without disconnection for memory efficiency */
static void recreate_service(bool enable_dev_msg)
{
    if (s_service_recreating) {
        ESP_LOGW(TAG, "Service recreation already in progress");
        return;
    }
    
    if (gl_profile_tab[PROFILE_A_APP_ID].gatts_if == ESP_GATT_IF_NONE) {
        ESP_LOGE(TAG, "Cannot recreate service - no GATT interface");
        return;
    }
    
    s_service_recreating = true;
    ESP_LOGI(TAG, "Recreating service: dev_msg=%s", enable_dev_msg ? "enabled" : "disabled");
    
    /* Stop current service (connection stays alive) */
    if (gl_profile_tab[PROFILE_A_APP_ID].service_handle != 0) {
        esp_ble_gatts_stop_service(gl_profile_tab[PROFILE_A_APP_ID].service_handle);
        esp_ble_gatts_delete_service(gl_profile_tab[PROFILE_A_APP_ID].service_handle);
    }
    
    /* Reset handles */
    gl_profile_tab[PROFILE_A_APP_ID].service_handle = 0;
    gl_profile_tab[PROFILE_A_APP_ID].char_handle = 0;
    gl_profile_tab[PROFILE_A_APP_ID].descr_handle = 0;
    dev_msg_serial_number_handle = 0;
    dev_msg_wifi_ssid_handle = 0;
    dev_msg_wifi_password_handle = 0;
    dev_msg_secret_key_handle = 0;
    dev_msg_timezone_handle = 0;
    dev_msg_album_handle = 0;

    /* Set mode before creating service */
    dev_msg_mode_active = enable_dev_msg;
    
    /* Create new service with appropriate size */
    esp_gatt_if_t gatts_if = gl_profile_tab[PROFILE_A_APP_ID].gatts_if;
    if (enable_dev_msg) {
        create_full_service(gatts_if);
    } else {
        create_dev_ctrl_service(gatts_if);
    }
}

/* Create minimal dev_ctrl-only service (saves ~1-2KB) */
static void create_dev_ctrl_service(esp_gatt_if_t gatts_if)
{
    ESP_LOGI(TAG, "Creating dev_ctrl-only service (minimal memory)");
    
    gl_profile_tab[PROFILE_A_APP_ID].service_id.is_primary = true;
    gl_profile_tab[PROFILE_A_APP_ID].service_id.id.inst_id = 0x00;
    gl_profile_tab[PROFILE_A_APP_ID].service_id.id.uuid.len = ESP_UUID_LEN_128;
    memcpy(gl_profile_tab[PROFILE_A_APP_ID].service_id.id.uuid.uuid.uuid128, service_uuid, 16);
    
    esp_ble_gatts_create_service(gatts_if, &gl_profile_tab[PROFILE_A_APP_ID].service_id, 
                                GATT_SERVICE_HANDLERS_DEV_CTRL_ONLY);
}

/* Create full service with dev_msg characteristics */
static void create_full_service(esp_gatt_if_t gatts_if)
{
    ESP_LOGI(TAG, "Creating full service with dev_msg characteristics");
    
    gl_profile_tab[PROFILE_A_APP_ID].service_id.is_primary = true;
    gl_profile_tab[PROFILE_A_APP_ID].service_id.id.inst_id = 0x00;
    gl_profile_tab[PROFILE_A_APP_ID].service_id.id.uuid.len = ESP_UUID_LEN_128;
    memcpy(gl_profile_tab[PROFILE_A_APP_ID].service_id.id.uuid.uuid.uuid128, service_uuid, 16);
    
    esp_ble_gatts_create_service(gatts_if, &gl_profile_tab[PROFILE_A_APP_ID].service_id,
                                GATT_SERVICE_HANDLERS);
}

/* =================== COEXISTENCE MANAGEMENT FUNCTIONS =================== */

void s3_ble_manager_set_coexistence_callback(void (*cb)(bool ble_active)) {
    s_coexistence_callback = cb;
    ESP_LOGI(TAG, "Coexistence callback registered");
}

esp_err_t s3_ble_manager_start_advertising(void) {
    ESP_LOGI(TAG, "Starting BLE advertising");
    esp_err_t ret = esp_ble_gap_start_advertising(&adv_params);
    if (ret == ESP_OK && s_coexistence_callback) {
        s_coexistence_callback(true);  // Notify that BLE advertising is active
    }
    return ret;
}

esp_err_t s3_ble_manager_stop_advertising(void) {
    ESP_LOGI(TAG, "Stopping BLE advertising");
    esp_err_t ret = esp_ble_gap_stop_advertising();
    if (ret == ESP_OK && s_coexistence_callback) {
        s_coexistence_callback(false);  // Notify that BLE advertising is stopped
    }
    return ret;
}

bool s3_ble_manager_is_advertising(void) {
    // This would need to track advertising state - for now return basic check
    return s3_ble_ready;
}

bool s3_ble_manager_is_connected(void) {
    return s_connected;
}

esp_err_t s3_ble_manager_disconnect_client(void) {
    ESP_LOGI(TAG, "Disconnecting BLE client to resolve L2CAP conflict");

    if (!s_connected) {
        ESP_LOGW(TAG, "No BLE client connected to disconnect");
        return ESP_OK;
    }

    // Get connection info from the profile
    esp_gatt_if_t gatts_if = gl_profile_tab[PROFILE_A_APP_ID].gatts_if;
    uint16_t conn_id = gl_profile_tab[PROFILE_A_APP_ID].conn_id;

    if (gatts_if == ESP_GATT_IF_NONE || conn_id == INVALID_CONN_ID) {
        ESP_LOGE(TAG, "Invalid GATT interface or connection ID");
        return ESP_ERR_INVALID_STATE;
    }

    // Force close the GATT connection
    esp_err_t ret = esp_ble_gatts_close(gatts_if, conn_id);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to close BLE connection: %s", esp_err_to_name(ret));
    } else {
        ESP_LOGI(TAG, "BLE connection forcefully closed to free L2CAP resources");
    }

    return ret;
}

/* =================== BACKWARD COMPATIBILITY WRAPPER =================== */

void ble_init(void) {
    s3_ble_manager_init();
}

