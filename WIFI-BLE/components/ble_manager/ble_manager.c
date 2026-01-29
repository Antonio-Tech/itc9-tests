#include "ble_manager.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_system.h"
#include "esp_log.h"
#include "esp_bt.h"
#include "esp_gap_ble_api.h"
#include "esp_gatts_api.h"
#include "esp_bt_main.h"
#include "esp_gatt_common_api.h"

#define GATTS_TAG "BLE_MGR"

extern void get_terminal_input(char *buffer, size_t size);

#define PROFILE_NUM 1
#define PROFILE_APP_ID 0
#define SVC_INST_ID 0
#define GATTS_SERVICE_UUID_TEST      0x00FF
#define GATTS_CHAR_UUID_TX           0xFF01
#define GATTS_NUM_HANDLE_TEST        8 

#define TEST_DEVICE_NAME            "ESP32_BLE" 
#define GATTS_DEMO_CHAR_VAL_LEN_MAX 0x40

static uint8_t char1_str[] = {0x11, 0x22, 0x33};
static esp_gatt_char_prop_t a_property = 0;
static esp_attr_value_t gatts_demo_char1_val = { .attr_max_len = GATTS_DEMO_CHAR_VAL_LEN_MAX, .attr_len = sizeof(char1_str), .attr_value = char1_str };

static uint16_t gatts_conn_id = 0xffff;
static uint16_t tx_handle = 0;
static bool is_subscribed = false;

static esp_bd_addr_t s_remote_bda; 
static bool s_ble_active = false;  

static esp_ble_adv_params_t adv_params = {
    .adv_int_min        = 0xA0, 
    .adv_int_max        = 0x140, 
    .adv_type           = ADV_TYPE_IND,
    .own_addr_type      = BLE_ADDR_TYPE_PUBLIC,
    .channel_map        = ADV_CHNL_ALL,
    .adv_filter_policy  = ADV_FILTER_ALLOW_SCAN_ANY_CON_ANY,
};

struct gatts_profile_inst {
    esp_gatts_cb_t gatts_cb;
    uint16_t gatts_if;
    uint16_t app_id;
    uint16_t conn_id;
    uint16_t service_handle;
    esp_gatt_srvc_id_t service_id;
    uint16_t char_handle;
    esp_bt_uuid_t char_uuid;
    esp_gatt_perm_t perm;
    esp_gatt_char_prop_t property;
    uint16_t descr_handle;
    esp_bt_uuid_t descr_uuid;
};

static void gatts_profile_event_handler(esp_gatts_cb_event_t event, esp_gatt_if_t gatts_if, esp_ble_gatts_cb_param_t *param);

static struct gatts_profile_inst gl_profile_tab[PROFILE_NUM] = {
    [PROFILE_APP_ID] = { .gatts_cb = gatts_profile_event_handler, .gatts_if = ESP_GATT_IF_NONE },
};

static void gap_event_handler(esp_gap_ble_cb_event_t event, esp_ble_gap_cb_param_t *param) {
    switch (event) {
    case ESP_GAP_BLE_ADV_DATA_RAW_SET_COMPLETE_EVT:
        if (s_ble_active) {
            esp_ble_gap_start_advertising(&adv_params);
        }
        break;
    case ESP_GAP_BLE_ADV_START_COMPLETE_EVT:
        if (param->adv_start_cmpl.status != ESP_BT_STATUS_SUCCESS) ESP_LOGE(GATTS_TAG, "Adv Start Failed");
        else ESP_LOGI(GATTS_TAG, "Advertising Started (Visivel como %s)", TEST_DEVICE_NAME);
        break;
    case ESP_GAP_BLE_ADV_STOP_COMPLETE_EVT:
        ESP_LOGI(GATTS_TAG, "Advertising Parado.");
        break;
    default: break;
    }
}

static void gatts_profile_event_handler(esp_gatts_cb_event_t event, esp_gatt_if_t gatts_if, esp_ble_gatts_cb_param_t *param) {
    switch (event) {
    case ESP_GATTS_REG_EVT:
        gl_profile_tab[PROFILE_APP_ID].service_id.is_primary = true;
        gl_profile_tab[PROFILE_APP_ID].service_id.id.inst_id = SVC_INST_ID;
        gl_profile_tab[PROFILE_APP_ID].service_id.id.uuid.len = ESP_UUID_LEN_16;
        gl_profile_tab[PROFILE_APP_ID].service_id.id.uuid.uuid.uuid16 = GATTS_SERVICE_UUID_TEST;
        
        esp_ble_gap_set_device_name(TEST_DEVICE_NAME);
        
        uint8_t raw_adv_data[] = { 
            0x02, 0x01, 0x06, 
            0x03, 0x03, 0xFF, 0x00, 
            0x0A, 0x09, 'E','S','P','3','2','_','B','L','E' 
        };
        esp_ble_gap_config_adv_data_raw(raw_adv_data, sizeof(raw_adv_data));
        
        esp_ble_gatts_create_service(gatts_if, &gl_profile_tab[PROFILE_APP_ID].service_id, GATTS_NUM_HANDLE_TEST);
        break;
    case ESP_GATTS_CREATE_EVT:
        gl_profile_tab[PROFILE_APP_ID].service_handle = param->create.service_handle;
        gl_profile_tab[PROFILE_APP_ID].char_uuid.len = ESP_UUID_LEN_16;
        gl_profile_tab[PROFILE_APP_ID].char_uuid.uuid.uuid16 = GATTS_CHAR_UUID_TX;
        esp_ble_gatts_start_service(gl_profile_tab[PROFILE_APP_ID].service_handle);
        a_property = ESP_GATT_CHAR_PROP_BIT_READ | ESP_GATT_CHAR_PROP_BIT_NOTIFY;
        esp_ble_gatts_add_char(gl_profile_tab[PROFILE_APP_ID].service_handle, &gl_profile_tab[PROFILE_APP_ID].char_uuid,
                                ESP_GATT_PERM_READ, a_property, &gatts_demo_char1_val, NULL);
        break;
    case ESP_GATTS_ADD_CHAR_EVT:
        tx_handle = param->add_char.attr_handle;
        gl_profile_tab[PROFILE_APP_ID].descr_uuid.len = ESP_UUID_LEN_16;
        gl_profile_tab[PROFILE_APP_ID].descr_uuid.uuid.uuid16 = ESP_GATT_UUID_CHAR_CLIENT_CONFIG;
        esp_ble_gatts_add_char_descr(gl_profile_tab[PROFILE_APP_ID].service_handle, &gl_profile_tab[PROFILE_APP_ID].descr_uuid,
                                     ESP_GATT_PERM_READ | ESP_GATT_PERM_WRITE, NULL, NULL);
        break;
    case ESP_GATTS_CONNECT_EVT:
        gatts_conn_id = param->connect.conn_id;
        gl_profile_tab[PROFILE_APP_ID].gatts_if = gatts_if;
        memcpy(s_remote_bda, param->connect.remote_bda, sizeof(esp_bd_addr_t));
        is_subscribed = false; 
        esp_ble_tx_power_set(ESP_BLE_PWR_TYPE_CONN_HDL0, ESP_PWR_LVL_P9);
        ESP_LOGI(GATTS_TAG, "Dispositivo Conectado. Aguardando inscricao...");
        break;
    case ESP_GATTS_DISCONNECT_EVT:
        gatts_conn_id = 0xffff;
        is_subscribed = false;
        ESP_LOGI(GATTS_TAG, "Dispositivo Desconectado.");
        if (s_ble_active) {
            esp_ble_gap_start_advertising(&adv_params);
            ESP_LOGI(GATTS_TAG, "Reiniciando Advertising...");
        } else {
            ESP_LOGI(GATTS_TAG, "Modo BLE inativo. Nao reiniciando advertising.");
        }
        break;
    case ESP_GATTS_WRITE_EVT:
        if (!param->write.is_prep && param->write.len == 2) {
            uint16_t descr_value = param->write.value[1] << 8 | param->write.value[0];
            if (descr_value == 0x0001) {
                is_subscribed = true;
                ESP_LOGI(GATTS_TAG, "Notificacoes Ativadas!");
                printf("\n[BLE] Cliente pronto. Digite a mensagem: "); fflush(stdout);
            } else {
                is_subscribed = false;
                ESP_LOGI(GATTS_TAG, "Notificacoes Desativadas.");
            }
        }
        if (param->write.need_rsp) esp_ble_gatts_send_response(gatts_if, param->write.conn_id, param->write.trans_id, ESP_GATT_OK, NULL);
        break;
    default: break;
    }
}

static void gatts_event_handler(esp_gatts_cb_event_t event, esp_gatt_if_t gatts_if, esp_ble_gatts_cb_param_t *param) {
    if (event == ESP_GATTS_REG_EVT) {
        if (param->reg.status == ESP_GATT_OK) gl_profile_tab[param->reg.app_id].gatts_if = gatts_if;
        else return;
    }
    do {
        int idx;
        for (idx = 0; idx < PROFILE_NUM; idx++) {
            if (gatts_if == ESP_GATT_IF_NONE || gatts_if == gl_profile_tab[idx].gatts_if) {
                if (gl_profile_tab[idx].gatts_cb) gl_profile_tab[idx].gatts_cb(event, gatts_if, param);
            }
        }
    } while (0);
}

void ble_init_module(void) {
    // Verifica se já está habilitado para não crashar ao reentrar no menu
    if (esp_bt_controller_get_status() == ESP_BT_CONTROLLER_STATUS_ENABLED) {
        ESP_LOGI(GATTS_TAG, "BLE já está habilitado.");
        return;
    }

    ESP_ERROR_CHECK(esp_bt_controller_mem_release(ESP_BT_MODE_CLASSIC_BT));
    esp_bt_controller_config_t bt_cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_bt_controller_init(&bt_cfg));
    ESP_ERROR_CHECK(esp_bt_controller_enable(ESP_BT_MODE_BLE));
    
    esp_ble_tx_power_set(ESP_BLE_PWR_TYPE_ADV, ESP_PWR_LVL_P9);
    ESP_ERROR_CHECK(esp_bluedroid_init());
    ESP_ERROR_CHECK(esp_bluedroid_enable());
    ESP_ERROR_CHECK(esp_ble_gatts_register_callback(gatts_event_handler));
    ESP_ERROR_CHECK(esp_ble_gap_register_callback(gap_event_handler));
    ESP_ERROR_CHECK(esp_ble_gatts_app_register(PROFILE_APP_ID));
    
    ESP_LOGI(GATTS_TAG, "BLE Stack Inicializada.");
}

void ble_deactivate(void) {
    // Processo reverso para desligar o BLE
    esp_bluedroid_disable();
    esp_bluedroid_deinit();
    esp_bt_controller_disable();
    esp_bt_controller_deinit();
    ESP_LOGI(GATTS_TAG, "BLE Desativado e recursos liberados.");
}

void ble_run_console(void) {
    s_ble_active = true;

    if (gatts_conn_id == 0xffff) {
        esp_ble_gap_start_advertising(&adv_params);
    }

    char line[128];
    printf("\n=== MODO BLE ATIVO ===\n");
    printf("Nome do Dispositivo: %s\n", TEST_DEVICE_NAME);
    printf("Digite 'sair' para encerrar a conexao e voltar.\n");
    
    while (1) {
        if (is_subscribed) printf("\n[BLE SEND]: ");
        else printf("\n[BLE WAITING]: ");
        
        get_terminal_input(line, sizeof(line));
        
        if (strcmp(line, "sair") == 0) {
            printf("Encerrando BLE...\n");
            
            // Parar de aceitar conexões
            s_ble_active = false;
            if (gatts_conn_id != 0xffff) {
                esp_ble_gap_disconnect(s_remote_bda);
            } else {
                esp_ble_gap_stop_advertising();
            }
            vTaskDelay(pdMS_TO_TICKS(500)); // Espera eventos processarem
            break; 
        }

        size_t len = strlen(line);
        if (len > 0) {
            if (gatts_conn_id != 0xffff) {
                if (is_subscribed) {
                    esp_ble_gatts_set_attr_value(tx_handle, len, (uint8_t *)line);
                    esp_ble_gatts_send_indicate(gl_profile_tab[PROFILE_APP_ID].gatts_if, gatts_conn_id, tx_handle, len, (uint8_t *)line, false);
                    ESP_LOGI(GATTS_TAG, "Enviado: %s", line);
                } else {
                    ESP_LOGW(GATTS_TAG, "Cliente nao inscrito (Notificacoes OFF). Mensagem ignorada.");
                }
            } else {
                ESP_LOGW(GATTS_TAG, "Ninguem conectado.");
            }
        }
    }
}