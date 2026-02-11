//
// Created by Shane_Hwang on 2025/4/29.
//

#include <inttypes.h>
#include <stdint.h>
#include <string.h>
#include "esp_bt.h"
#include "esp_bt_device.h"
#include "esp_bt_main.h"
#include "esp_err.h"
#include "esp_gap_bt_api.h"
#include "esp_log.h"
#include "esp_peripherals.h"

#include "fac_bt.h"

#define GAP_TAG "GAP"

bool can_shutdown_bt = false;
char gTestBtName[64];

typedef enum {
    APP_GAP_STATE_IDLE = 0,
    APP_GAP_STATE_DEVICE_DISCOVERING,
    APP_GAP_STATE_DEVICE_DISCOVER_COMPLETE,
    APP_GAP_STATE_SERVICE_DISCOVERING,
    APP_GAP_STATE_SERVICE_DISCOVER_COMPLETE,
} app_gap_state_t;

typedef struct {
    bool dev_found;
    uint8_t bdname_len;
    uint8_t eir_len;
    uint8_t rssi;
    uint32_t cod;
    uint8_t eir[ESP_BT_GAP_EIR_DATA_LEN];
    uint8_t bdname[ESP_BT_GAP_MAX_BDNAME_LEN + 1];
    esp_bd_addr_t bda;
    app_gap_state_t state;
} app_gap_cb_t;

static app_gap_cb_t m_dev_info;

static char *bda2str(esp_bd_addr_t bda, char *str, size_t size) {
    if (bda == NULL || str == NULL || size < 18) {
        return NULL;
    }

    uint8_t *p = bda;
    sprintf(str, "%02x:%02x:%02x:%02x:%02x:%02x", p[0], p[1], p[2], p[3], p[4], p[5]);
    return str;
}

esp_err_t cli_setup_bt() {
    ESP_LOGI(GAP_TAG, "Start BT");
    char bda_str[18] = {0};
    esp_err_t ret = ESP_OK;

    esp_bt_controller_config_t bt_cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
    if ((ret = esp_bt_controller_init(&bt_cfg)) != ESP_OK) {
        ESP_LOGE(GAP_TAG, "%s initialize controller failed: %s", __func__, esp_err_to_name(ret));
        return ret;
    }

    if ((ret = esp_bt_controller_enable(ESP_BT_MODE_CLASSIC_BT)) != ESP_OK) {
        ESP_LOGE(GAP_TAG, "%s enable controller failed: %s", __func__, esp_err_to_name(ret));
        return ret;
    }

    esp_bluedroid_config_t bluedroid_cfg = BT_BLUEDROID_INIT_CONFIG_DEFAULT();
    if ((ret = esp_bluedroid_init_with_cfg(&bluedroid_cfg)) != ESP_OK) {
        ESP_LOGE(GAP_TAG, "%s initialize bluedroid failed: %s", __func__, esp_err_to_name(ret));
        return ret;
    }

    if ((ret = esp_bluedroid_enable()) != ESP_OK) {
        ESP_LOGE(GAP_TAG, "%s enable bluedroid failed: %s", __func__, esp_err_to_name(ret));
        return ret;
    }

    ESP_LOGI(GAP_TAG, "Own address:[%s]", bda2str((uint8_t *) esp_bt_dev_get_address(), bda_str, sizeof(bda_str)));
    return ret;
}

esp_err_t cli_stop_bt(void) {
    ESP_LOGI(GAP_TAG, "cli_stop_bt");
    esp_bluedroid_disable();
    esp_bluedroid_deinit();
    esp_bt_controller_disable();
    esp_bt_controller_deinit();
    ESP_LOGI(GAP_TAG, "esp_bt_controller_get_status[%d]", esp_bt_controller_get_status());

    return ESP_OK;
}

static bool get_name_from_eir(uint8_t *eir, uint8_t *bdname, uint8_t *bdname_len) {
    uint8_t *rmt_bdname = NULL;
    uint8_t rmt_bdname_len = 0;

    if (!eir) {
        ESP_LOGI(GAP_TAG, "EIR is NULL");
        return false;
    }

    rmt_bdname = esp_bt_gap_resolve_eir_data(eir, ESP_BT_EIR_TYPE_CMPL_LOCAL_NAME, &rmt_bdname_len);
    if (!rmt_bdname) {
        rmt_bdname = esp_bt_gap_resolve_eir_data(eir, ESP_BT_EIR_TYPE_SHORT_LOCAL_NAME, &rmt_bdname_len);
    }

    if (rmt_bdname) {
        if (rmt_bdname_len > ESP_BT_GAP_MAX_BDNAME_LEN) {
            rmt_bdname_len = ESP_BT_GAP_MAX_BDNAME_LEN;
        }

        if (bdname) {
            memcpy(bdname, rmt_bdname, rmt_bdname_len);
            bdname[rmt_bdname_len] = '\0';
        }
        if (bdname_len) {
            *bdname_len = rmt_bdname_len;
        }
        return true;
    }

    return false;
}

static void update_device_info(esp_bt_gap_cb_param_t *param) {
    char bda_str[18];
    uint32_t cod = 0;
    int32_t rssi = -129; /* invalid value */
    uint8_t *bdname = NULL;
    uint8_t bdname_len = 0;
    uint8_t *eir = NULL;
    uint8_t eir_len = 0;
    esp_bt_gap_dev_prop_t *p;

    ESP_LOGI(GAP_TAG, "Device found: %s", bda2str(param->disc_res.bda, bda_str, 18));
    for (int i = 0; i < param->disc_res.num_prop; i++) {
        p = param->disc_res.prop + i;
        switch (p->type) {
            case ESP_BT_GAP_DEV_PROP_COD:
                cod = *(uint32_t *) (p->val);
                break;
            case ESP_BT_GAP_DEV_PROP_RSSI:
                rssi = *(int8_t *) (p->val);
                break;
            case ESP_BT_GAP_DEV_PROP_BDNAME:

                bdname_len = (p->len > ESP_BT_GAP_MAX_BDNAME_LEN) ? ESP_BT_GAP_MAX_BDNAME_LEN : (uint8_t) p->len;
                bdname = (uint8_t *) (p->val);

                ESP_LOGI(GAP_TAG, "Device bdname_len: %d", bdname_len);
                if (bdname_len > 0) {
                    ESP_LOGI(GAP_TAG, "Device bdname: %s", bdname);
                }

                break;
            case ESP_BT_GAP_DEV_PROP_EIR: {
                eir_len = p->len;
                eir = (uint8_t *) (p->val);
                break;
            }
            default:
                break;
        }
    }

    /* search for device with Major device type "PHONE" or "Audio/Video" in COD */
    app_gap_cb_t *p_dev = &m_dev_info;
    //    if (p_dev->dev_found) {
    //        return;
    //    }

    if (!esp_bt_gap_is_valid_cod(cod) || (!(esp_bt_gap_get_cod_major_dev(cod) == ESP_BT_COD_MAJOR_DEV_PHONE) && !(esp_bt_gap_get_cod_major_dev(cod) == ESP_BT_COD_MAJOR_DEV_AV))) {
        return;
    }

    memcpy(p_dev->bda, param->disc_res.bda, ESP_BD_ADDR_LEN);
    //    p_dev->dev_found = true;

    p_dev->cod = cod;
    p_dev->rssi = rssi;
    p_dev->bdname_len = 0;

    if (bdname_len > 0) {
        memcpy(p_dev->bdname, bdname, bdname_len);
        p_dev->bdname[bdname_len] = '\0';
        p_dev->bdname_len = bdname_len;
    }
    if (eir_len > 0) {
        memcpy(p_dev->eir, eir, eir_len);
        p_dev->eir_len = eir_len;
    }

    int resName = 0;
    if (p_dev->bdname_len == 0) {
        resName = get_name_from_eir(p_dev->eir, p_dev->bdname, &p_dev->bdname_len);
    }

    if (strlen(gTestBtName) > 0) {
        if (strcmp((const char *) p_dev->bdname, gTestBtName) == 0) {
            if (resName) {
                ESP_LOGI(GAP_TAG, "Found a target device, address %s, name %s rssi %" PRId32 " cod 0x%" PRIx32, bda2str(param->disc_res.bda, bda_str, 18), p_dev->bdname, rssi,
                         cod);
            }
            p_dev->state = APP_GAP_STATE_DEVICE_DISCOVER_COMPLETE;
            ESP_LOGI(GAP_TAG, "Cancel device discovery ...");
            esp_bt_gap_cancel_discovery();
        }
    } else {
        if (resName) {
            ESP_LOGI(GAP_TAG, "Found a target device, address %s, name %s rssi %" PRId32 " cod 0x%" PRIx32, bda2str(param->disc_res.bda, bda_str, 18), p_dev->bdname, rssi, cod);
        }
    }
}

static void bt_app_gap_init(void) {
    app_gap_cb_t *p_dev = &m_dev_info;
    memset(p_dev, 0, sizeof(app_gap_cb_t));

    p_dev->state = APP_GAP_STATE_IDLE;
}

static void bt_app_gap_cb(esp_bt_gap_cb_event_t event, esp_bt_gap_cb_param_t *param) {
    app_gap_cb_t *p_dev = &m_dev_info;
    switch (event) {
        case ESP_BT_GAP_DISC_RES_EVT: {
            update_device_info(param);
            break;
        }
        case ESP_BT_GAP_DISC_STATE_CHANGED_EVT: {
            if (param->disc_st_chg.state == ESP_BT_GAP_DISCOVERY_STOPPED) {
                ESP_LOGI(GAP_TAG, "Device discovery stopped.");
                if ((p_dev->state == APP_GAP_STATE_DEVICE_DISCOVER_COMPLETE || p_dev->state == APP_GAP_STATE_DEVICE_DISCOVERING)) {
                    p_dev->state = APP_GAP_STATE_SERVICE_DISCOVERING;
                    ESP_LOGI(GAP_TAG, "Discover services ...");
                    esp_bt_gap_get_remote_services(p_dev->bda);
                    can_shutdown_bt = true;
                }
            } else if (param->disc_st_chg.state == ESP_BT_GAP_DISCOVERY_STARTED) {
                ESP_LOGI(GAP_TAG, "Discovery started.");
            }
            break;
        }

        case ESP_BT_GAP_RMT_SRVC_REC_EVT:
        default: {
            ESP_LOGI(GAP_TAG, "event: %d", event);
            break;
        }
    }
    return;
}

esp_err_t fac_bt_scan(esp_periph_handle_t periph, int argc, char *argv[]) {
    cli_setup_bt();
    memset(gTestBtName, 0, sizeof(char) * 64);
    if (argc == 1) {
        ESP_LOGI(GAP_TAG, "sta start to scan argc=%d,argv[0]=%s", argc, argv[0]);
        strcpy(gTestBtName, argv[0]);
    }

    /* register GAP callback function */
    esp_bt_gap_register_callback(bt_app_gap_cb);

    char *dev_name = "Pixsee-S3";
    esp_bt_gap_set_device_name(dev_name);

    /* set discoverable and connectable mode, wait to be connected */
    esp_bt_gap_set_scan_mode(ESP_BT_CONNECTABLE, ESP_BT_GENERAL_DISCOVERABLE);

    /* initialize device information and status */
    bt_app_gap_init();

    /* start to discover nearby Bluetooth devices */
    app_gap_cb_t *p_dev = &m_dev_info;
    p_dev->state = APP_GAP_STATE_DEVICE_DISCOVERING;
    esp_bt_gap_start_discovery(ESP_BT_INQ_MODE_GENERAL_INQUIRY, 10, 0);

    while (1) {
        if (can_shutdown_bt) {
            can_shutdown_bt = false;
            cli_stop_bt();
            return ESP_OK;
        }
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

esp_err_t fac_ping_bt(esp_periph_handle_t periph, int argc, char *argv[]) {

    if (cli_setup_bt() == ESP_OK)
        ESP_LOGI(GAP_TAG, "ping_bt:success");
    else
        ESP_LOGI(GAP_TAG, "ping_bt:failed");

    cli_stop_bt();
    return ESP_OK;
}
