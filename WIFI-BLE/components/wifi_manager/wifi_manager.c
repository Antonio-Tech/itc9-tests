#include "wifi_manager.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "ping/ping_sock.h"
#include "lwip/inet.h"
#include <string.h>

static const char *TAG = "WIFI_MGR";
static EventGroupHandle_t s_wifi_event_group;
#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT      BIT1

static int s_retry_num = 0;
#define EXAMPLE_ESP_MAXIMUM_RETRY 3

static bool s_reconnect_allowed = true; 
static bool s_system_initialized = false; 
static bool s_driver_initialized = false;

static void event_handler(void* arg, esp_event_base_t event_base,
                                int32_t event_id, void* event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
    }
    else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        wifi_event_sta_disconnected_t* event = (wifi_event_sta_disconnected_t*) event_data;
        if (s_reconnect_allowed && s_retry_num < EXAMPLE_ESP_MAXIMUM_RETRY) {
            esp_wifi_connect();
            s_retry_num++;
            ESP_LOGI(TAG, "Tentando reconectar... %d/%d", s_retry_num, EXAMPLE_ESP_MAXIMUM_RETRY);
        } else {
            if (!s_reconnect_allowed) {
                ESP_LOGI(TAG, "Desconexao intencional ou Wi-Fi desativado.");
            } else {
                ESP_LOGE(TAG, "Falha na conexao. Motivo: %d", event->reason);
            }
            xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
        }
    } 
    else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t* event = (ip_event_got_ip_t*) event_data;
        ESP_LOGI(TAG, "IP Recebido: " IPSTR, IP2STR(&event->ip_info.ip));
        s_retry_num = 0;
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
    }
}

esp_err_t wifi_init_module(void) {
    esp_err_t err;

    // Configura infraestrutura do sistema (Netif)
    if (!s_system_initialized) {
        s_wifi_event_group = xEventGroupCreate();
        if (s_wifi_event_group == NULL) return ESP_ERR_NO_MEM;

        err = esp_netif_init();
        if (err != ESP_OK) return err;

        err = esp_event_loop_create_default();
        if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) return err; // Ignora se já criado

        esp_netif_create_default_wifi_sta();
        s_system_initialized = true;
    }

    // Inicializa o Driver Wi-Fi
    if (!s_driver_initialized) {
        wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
        err = esp_wifi_init(&cfg);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Falha ao alocar recursos do Wi-Fi: %s", esp_err_to_name(err));
            return err;
        }
        
        err = esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &event_handler, NULL, NULL);
        if (err != ESP_OK) return err;
        
        err = esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &event_handler, NULL, NULL);
        if (err != ESP_OK) return err;
        
        err = esp_wifi_set_mode(WIFI_MODE_STA);
        if (err != ESP_OK) return err;
        
        s_driver_initialized = true;
    }

    // Liga o Rádio
    err = esp_wifi_start();
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "Wi-Fi Iniciado.");
        s_reconnect_allowed = true;
    } else if (err == ESP_ERR_WIFI_STATE) {
        ESP_LOGW(TAG, "Wi-Fi ja estava ativo.");
        return ESP_OK;
    } else {
        ESP_LOGE(TAG, "Erro ao iniciar Wi-Fi: %s", esp_err_to_name(err));
        return err;
    }
    
    return ESP_OK;
}

esp_err_t wifi_deactivate(void) {
    s_reconnect_allowed = false; 
    
    esp_wifi_disconnect();
    esp_err_t err = esp_wifi_stop();
    if (err == ESP_ERR_WIFI_NOT_INIT) {
        return ESP_OK; // Já estava parado/desiniciado
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Erro ao desativar Wi-Fi: %s", esp_err_to_name(err));
        return err;
    }

    // Desinicializa o Driver
    err = esp_wifi_deinit();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Erro ao desinicializar driver Wi-Fi: %s", esp_err_to_name(err));
        return err;
    }

    s_driver_initialized = false;
    ESP_LOGI(TAG, "Wi-Fi totalmente desativado e recursos liberados.");
    
    return ESP_OK;
}

int wifi_scan_and_list(wifi_info_t *records) {
    s_reconnect_allowed = false;
    esp_wifi_disconnect();
    vTaskDelay(pdMS_TO_TICKS(100)); 
    
    wifi_scan_config_t scan_config = { .show_hidden = true };
    
    ESP_LOGI(TAG, "Iniciando scan WiFi...");
    esp_err_t res = esp_wifi_scan_start(&scan_config, true);
    
    if (res != ESP_OK) {
        ESP_LOGE(TAG, "Falha no scan (Erro: %s)", esp_err_to_name(res));
        s_reconnect_allowed = true; 
        return 0;
    }

    uint16_t ap_count = 0;
    esp_wifi_scan_get_ap_num(&ap_count);
    
    uint16_t limit = (ap_count > MAX_SCAN_RECORDS) ? MAX_SCAN_RECORDS : ap_count;
    wifi_ap_record_t *ap_records = malloc(sizeof(wifi_ap_record_t) * limit);
    
    ESP_ERROR_CHECK(esp_wifi_scan_get_ap_records(&limit, ap_records));

    printf("\n--- Redes Encontradas ---\n");
    printf("%-4s | %-32s | %s\n", "ID", "SSID", "RSSI");
    for (int i = 0; i < limit; i++) {
        strncpy(records[i].ssid, (char *)ap_records[i].ssid, 33);
        records[i].rssi = ap_records[i].rssi;
        printf("[%2d] | %-32s | %d dBm\n", i, records[i].ssid, records[i].rssi);
    }
    printf("-------------------------\n");
    
    free(ap_records);
    return limit;
}

esp_err_t wifi_connect(const char *ssid, const char *password) {
    s_retry_num = 0;
    s_reconnect_allowed = false; 
    esp_wifi_disconnect();
    vTaskDelay(pdMS_TO_TICKS(200)); 
    
    wifi_config_t wifi_config = {0};
    strncpy((char *)wifi_config.sta.ssid, ssid, sizeof(wifi_config.sta.ssid));
    strncpy((char *)wifi_config.sta.password, password, sizeof(wifi_config.sta.password));
    wifi_config.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;

    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    
    ESP_LOGI(TAG, "Conectando a %s...", ssid);
    xEventGroupClearBits(s_wifi_event_group, WIFI_CONNECTED_BIT | WIFI_FAIL_BIT);
    
    s_reconnect_allowed = true; 
    esp_wifi_connect();

    EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group, 
                                           WIFI_CONNECTED_BIT | WIFI_FAIL_BIT, 
                                           pdFALSE, pdFALSE, portMAX_DELAY);

    if (bits & WIFI_CONNECTED_BIT) {
        ESP_LOGI(TAG, "Conectado com sucesso!");
        return ESP_OK;
    } else {
        ESP_LOGE(TAG, "Falha ao conectar.");
        return ESP_FAIL;
    }
}

// Callbacks do Ping 
static void on_ping_success(esp_ping_handle_t hdl, void *args) {
    uint8_t ttl;
    uint16_t seqno;
    uint32_t elapsed_time, recv_len;
    ip_addr_t target_addr;
    esp_ping_get_profile(hdl, ESP_PING_PROF_SEQNO, &seqno, sizeof(seqno));
    esp_ping_get_profile(hdl, ESP_PING_PROF_TTL, &ttl, sizeof(ttl));
    esp_ping_get_profile(hdl, ESP_PING_PROF_IPADDR, &target_addr, sizeof(target_addr));
    esp_ping_get_profile(hdl, ESP_PING_PROF_SIZE, &recv_len, sizeof(recv_len));
    esp_ping_get_profile(hdl, ESP_PING_PROF_TIMEGAP, &elapsed_time, sizeof(elapsed_time));
    printf("%d bytes from %s: icmp_seq=%d ttl=%d time=%d ms\n",
           (int)recv_len, ipaddr_ntoa(&target_addr), seqno, ttl, (int)elapsed_time);
}
static void on_ping_timeout(esp_ping_handle_t hdl, void *args) {
    uint16_t seqno;
    ip_addr_t target_addr;
    esp_ping_get_profile(hdl, ESP_PING_PROF_SEQNO, &seqno, sizeof(seqno));
    esp_ping_get_profile(hdl, ESP_PING_PROF_IPADDR, &target_addr, sizeof(target_addr));
    printf("Request timeout for icmp_seq=%d from %s\n", seqno, ipaddr_ntoa(&target_addr));
}
static void on_ping_end(esp_ping_handle_t hdl, void *args) {
    esp_ping_delete_session(hdl);
}

void wifi_ping(const char *target_ip, int count) {
    esp_ping_config_t ping_config = ESP_PING_DEFAULT_CONFIG();
    if (ipaddr_aton(target_ip, &ping_config.target_addr) == 0) {
        printf("IP Invalido.\n");
        return;
    }
    ping_config.count = count;
    esp_ping_callbacks_t cbs = { .on_ping_success = on_ping_success, .on_ping_timeout = on_ping_timeout, .on_ping_end = on_ping_end };
    esp_ping_handle_t ping;
    esp_ping_new_session(&ping_config, &cbs, &ping);
    esp_ping_start(ping);
    vTaskDelay(pdMS_TO_TICKS(count * 1000 + 1000));
}