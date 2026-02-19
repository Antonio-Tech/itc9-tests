//
// Created by Shane_Hwang on 2025/4/29.
//

#include <netinet/in.h>
#include <stdint.h>
#include <string.h>
#include <sys/socket.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_err.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_peripherals.h"
#include "esp_wifi.h"
#include "periph_wifi.h"

#include "lwip/err.h"
#include "lwip/sys.h"

#include "fac_wifi.h"
#include "s3_nvs_item.h"

uint16_t g_scan_ap_num;
wifi_ap_record_t *g_ap_list_buffer;

static const char *TAG = "FAC_WIFI";
static void print_auth_mode(int authmode) {
    switch (authmode) {
        case WIFI_AUTH_OPEN:
            ESP_LOGI(TAG, "Authmode \tWIFI_AUTH_OPEN");
            break;
        case WIFI_AUTH_OWE:
            ESP_LOGI(TAG, "Authmode \tWIFI_AUTH_OWE");
            break;
        case WIFI_AUTH_WEP:
            ESP_LOGI(TAG, "Authmode \tWIFI_AUTH_WEP");
            break;
        case WIFI_AUTH_WPA_PSK:
            ESP_LOGI(TAG, "Authmode \tWIFI_AUTH_WPA_PSK");
            break;
        case WIFI_AUTH_WPA2_PSK:
            ESP_LOGI(TAG, "Authmode \tWIFI_AUTH_WPA2_PSK");
            break;
        case WIFI_AUTH_WPA_WPA2_PSK:
            ESP_LOGI(TAG, "Authmode \tWIFI_AUTH_WPA_WPA2_PSK");
            break;
        case WIFI_AUTH_ENTERPRISE:
            ESP_LOGI(TAG, "Authmode \tWIFI_AUTH_ENTERPRISE");
            break;
        case WIFI_AUTH_WPA3_PSK:
            ESP_LOGI(TAG, "Authmode \tWIFI_AUTH_WPA3_PSK");
            break;
        case WIFI_AUTH_WPA2_WPA3_PSK:
            ESP_LOGI(TAG, "Authmode \tWIFI_AUTH_WPA2_WPA3_PSK");
            break;
        case WIFI_AUTH_WPA3_ENT_192:
            ESP_LOGI(TAG, "Authmode \tWIFI_AUTH_WPA3_ENT_192");
            break;
        default:
            ESP_LOGI(TAG, "Authmode \tWIFI_AUTH_UNKNOWN");
            break;
    }
}

static void print_cipher_type(int pairwise_cipher, int group_cipher) {
    switch (pairwise_cipher) {
        case WIFI_CIPHER_TYPE_NONE:
            ESP_LOGI(TAG, "Pairwise Cipher \tWIFI_CIPHER_TYPE_NONE");
            break;
        case WIFI_CIPHER_TYPE_WEP40:
            ESP_LOGI(TAG, "Pairwise Cipher \tWIFI_CIPHER_TYPE_WEP40");
            break;
        case WIFI_CIPHER_TYPE_WEP104:
            ESP_LOGI(TAG, "Pairwise Cipher \tWIFI_CIPHER_TYPE_WEP104");
            break;
        case WIFI_CIPHER_TYPE_TKIP:
            ESP_LOGI(TAG, "Pairwise Cipher \tWIFI_CIPHER_TYPE_TKIP");
            break;
        case WIFI_CIPHER_TYPE_CCMP:
            ESP_LOGI(TAG, "Pairwise Cipher \tWIFI_CIPHER_TYPE_CCMP");
            break;
        case WIFI_CIPHER_TYPE_TKIP_CCMP:
            ESP_LOGI(TAG, "Pairwise Cipher \tWIFI_CIPHER_TYPE_TKIP_CCMP");
            break;
        case WIFI_CIPHER_TYPE_AES_CMAC128:
            ESP_LOGI(TAG, "Pairwise Cipher \tWIFI_CIPHER_TYPE_AES_CMAC128");
            break;
        case WIFI_CIPHER_TYPE_SMS4:
            ESP_LOGI(TAG, "Pairwise Cipher \tWIFI_CIPHER_TYPE_SMS4");
            break;
        case WIFI_CIPHER_TYPE_GCMP:
            ESP_LOGI(TAG, "Pairwise Cipher \tWIFI_CIPHER_TYPE_GCMP");
            break;
        case WIFI_CIPHER_TYPE_GCMP256:
            ESP_LOGI(TAG, "Pairwise Cipher \tWIFI_CIPHER_TYPE_GCMP256");
            break;
        default:
            ESP_LOGI(TAG, "Pairwise Cipher \tWIFI_CIPHER_TYPE_UNKNOWN");
            break;
    }

    switch (group_cipher) {
        case WIFI_CIPHER_TYPE_NONE:
            ESP_LOGI(TAG, "Group Cipher \tWIFI_CIPHER_TYPE_NONE");
            break;
        case WIFI_CIPHER_TYPE_WEP40:
            ESP_LOGI(TAG, "Group Cipher \tWIFI_CIPHER_TYPE_WEP40");
            break;
        case WIFI_CIPHER_TYPE_WEP104:
            ESP_LOGI(TAG, "Group Cipher \tWIFI_CIPHER_TYPE_WEP104");
            break;
        case WIFI_CIPHER_TYPE_TKIP:
            ESP_LOGI(TAG, "Group Cipher \tWIFI_CIPHER_TYPE_TKIP");
            break;
        case WIFI_CIPHER_TYPE_CCMP:
            ESP_LOGI(TAG, "Group Cipher \tWIFI_CIPHER_TYPE_CCMP");
            break;
        case WIFI_CIPHER_TYPE_TKIP_CCMP:
            ESP_LOGI(TAG, "Group Cipher \tWIFI_CIPHER_TYPE_TKIP_CCMP");
            break;
        case WIFI_CIPHER_TYPE_SMS4:
            ESP_LOGI(TAG, "Group Cipher \tWIFI_CIPHER_TYPE_SMS4");
            break;
        case WIFI_CIPHER_TYPE_GCMP:
            ESP_LOGI(TAG, "Group Cipher \tWIFI_CIPHER_TYPE_GCMP");
            break;
        case WIFI_CIPHER_TYPE_GCMP256:
            ESP_LOGI(TAG, "Group Cipher \tWIFI_CIPHER_TYPE_GCMP256");
            break;
        default:
            ESP_LOGI(TAG, "Group Cipher \tWIFI_CIPHER_TYPE_UNKNOWN");
            break;
    }
}

static bool wifi_perform_scan(const char *ssid, bool internal) {
    wifi_scan_config_t scan_config = {0};
    scan_config.ssid = (uint8_t *) ssid;
    uint8_t i;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    if (ESP_OK != esp_wifi_scan_start(&scan_config, true)) {
        ESP_LOGI(TAG, "Failed to perform scan");
        return false;
    }

    esp_wifi_scan_get_ap_num(&g_scan_ap_num);
    if (g_scan_ap_num == 0) {
        ESP_LOGI(TAG, "No matching AP found");
        return false;
    }

    if (g_ap_list_buffer) {
        free(g_ap_list_buffer);
    }
    g_ap_list_buffer = malloc(g_scan_ap_num * sizeof(wifi_ap_record_t));
    if (g_ap_list_buffer == NULL) {
        ESP_LOGE(TAG, "Failed to malloc buffer to print scan results");
        esp_wifi_clear_ap_list();
        return false;
    }

    if (esp_wifi_scan_get_ap_records(&g_scan_ap_num, (wifi_ap_record_t *) g_ap_list_buffer) == ESP_OK) {
        if (!internal) {
            for (i = 0; i < g_scan_ap_num; i++) {
                ESP_LOGI(TAG,
                         "%s rssi %d "
                         "%s",
                         g_ap_list_buffer[i].ssid, g_ap_list_buffer[i].rssi, g_ap_list_buffer[i].ftm_responder ? "[FTM Responder]" : "");
                print_auth_mode(g_ap_list_buffer[i].authmode);
                if (g_ap_list_buffer[i].authmode != WIFI_AUTH_WEP) {
                    print_cipher_type(g_ap_list_buffer[i].pairwise_cipher, g_ap_list_buffer[i].group_cipher);
                }
                ESP_LOGI(TAG, "Channel \t\t%d", g_ap_list_buffer[i].primary);
            }
        }
    }

    ESP_LOGI(TAG, "sta scan done");
    return true;
}

esp_err_t fac_wifi_scan(esp_periph_handle_t periph, int argc, char *argv[]) {
    if (argc == 1) {
        ESP_LOGI(TAG, "sta start to scan argc=%d,argv[0]=%s", argc, argv[0]);
        wifi_perform_scan(argv[0], false);
    } else {
        wifi_perform_scan(NULL, false);
    }
    return ESP_OK;
}

esp_err_t fac_wifi_mac(esp_periph_handle_t periph, int argc, char *argv[]) {
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    ESP_LOGI(TAG, "WiFi MAC: %02x:%02x:%02x:%02x:%02x:%02x", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);

    return ESP_OK;
}

esp_err_t fac_bt_mac(esp_periph_handle_t periph, int argc, char *argv[]) {
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_BT);
    ESP_LOGI(TAG, "Bluetooth MAC: %02x:%02x:%02x:%02x:%02x:%02x", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);

    return ESP_OK;
}

#define EXAMPLE_ESP_WIFI_SSID "ShaneESP32"
#define EXAMPLE_ESP_WIFI_PASS "0123456789"
#define EXAMPLE_ESP_WIFI_CHANNEL 6
#define EXAMPLE_MAX_STA_CONN 3

static void wifi_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data) {
    if (event_id == WIFI_EVENT_AP_STACONNECTED) {
        wifi_event_ap_staconnected_t *event = (wifi_event_ap_staconnected_t *) event_data;
        ESP_LOGI(TAG, "station " MACSTR " join, AID=%d", MAC2STR(event->mac), event->aid);
    } else if (event_id == WIFI_EVENT_AP_STADISCONNECTED) {
        wifi_event_ap_stadisconnected_t *event = (wifi_event_ap_stadisconnected_t *) event_data;
        ESP_LOGI(TAG, "station " MACSTR " leave, AID=%d, reason=%d", MAC2STR(event->mac), event->aid, event->reason);
    }
}

static esp_netif_t *ap = NULL;
void wifi_init_softap(void) {

    ESP_ERROR_CHECK(esp_event_loop_create_default());
    ap = esp_netif_create_default_wifi_ap();
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL, NULL));

    char sn[S3_NVS_SN_LENGTH] = {0};
    s3_nvs_get(NVS_S3_DEVICE_SN, sn);
    if (strlen(sn) > 0)
        ESP_LOGI(TAG, "get_sn:%s", sn);
    else {
        ESP_LOGI(TAG, "get_sn:failed");
        strcpy(sn, EXAMPLE_ESP_WIFI_SSID);
        strcpy(sn, EXAMPLE_ESP_WIFI_PASS);
    }

    wifi_config_t wifi_config = {
            .ap =
                    {
                            .channel = EXAMPLE_ESP_WIFI_CHANNEL,
                            .max_connection = EXAMPLE_MAX_STA_CONN,
                            .authmode = WIFI_AUTH_WPA2_PSK,
                            .pmf_cfg =
                                    {
                                            .required = true,
                                    },
                    },
    };
    strncpy((char *) wifi_config.ap.ssid, sn, sizeof(wifi_config.ap.ssid));
    strncpy((char *) wifi_config.ap.password, sn, sizeof(wifi_config.ap.password));
    wifi_config.ap.ssid_len = strlen((char *) wifi_config.ap.ssid);

    if (strlen((char *) wifi_config.ap.password) == 0) {
        wifi_config.ap.authmode = WIFI_AUTH_OPEN;
    }

    ESP_LOGI(TAG, "wifi_init_softap finished. SSID:%s len:%d, password:%s channel:%d", (char *) wifi_config.ap.ssid, strlen((char *) wifi_config.ap.ssid),
             (char *) wifi_config.ap.password, EXAMPLE_ESP_WIFI_CHANNEL);
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());
}

void wifi_deinit_softap(void) {
    if (ap) {
        ESP_LOGI(TAG, "wifi_deinit_softap");
        esp_wifi_stop();
        esp_wifi_deinit();
        esp_event_loop_delete_default();
        esp_netif_destroy_default_wifi(ap);
        ap = NULL;
    }
}
