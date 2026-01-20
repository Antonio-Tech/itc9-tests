#include "wifi_manager.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include "lwip/inet.h"
#include "ping/ping_sock.h" 

static const char *TAG = "WIFI_MGR";

static EventGroupHandle_t s_wifi_event_group;
#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT      BIT1

static int s_retry_num = 0;
#define EXAMPLE_ESP_MAXIMUM_RETRY 3

static void event_handler(void* arg, esp_event_base_t event_base,
                                int32_t event_id, void* event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        wifi_event_sta_disconnected_t* event = (wifi_event_sta_disconnected_t*) event_data;
        
        if (event->reason == 15) {
            ESP_LOGE(TAG, "Falha: Handshake Timeout (Provavelmente senha incorreta)");
        } else {
            ESP_LOGW(TAG, "Desconectado. Motivo: %d", event->reason);
        }

        if (s_retry_num < EXAMPLE_ESP_MAXIMUM_RETRY) {
            esp_wifi_connect();
            s_retry_num++;
            ESP_LOGI(TAG, "Tentativa de reconexao %d/%d", s_retry_num, EXAMPLE_ESP_MAXIMUM_RETRY);
        } else {
            xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
        }
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t* event = (ip_event_got_ip_t*) event_data;
        ESP_LOGI(TAG, "IP Recebido: " IPSTR, IP2STR(&event->ip_info.ip));
        s_retry_num = 0;
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
    }
}

void wifi_init(void) {
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    s_wifi_event_group = xEventGroupCreate();

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &event_handler, NULL, NULL));

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "Wi-Fi Inicializado.");
}

int wifi_scan_and_list(wifi_info_t *records) {
    esp_wifi_disconnect(); 
    wifi_scan_config_t scan_config = { .show_hidden = true };
    
    ESP_LOGI(TAG, "Iniciando scan...");
    esp_err_t res = esp_wifi_scan_start(&scan_config, true);
    if (res != ESP_OK) return 0;

    uint16_t ap_count = 0;
    esp_wifi_scan_get_ap_num(&ap_count);
    
    uint16_t limit = (ap_count > MAX_SCAN_RECORDS) ? MAX_SCAN_RECORDS : ap_count;
    wifi_ap_record_t *ap_records = malloc(sizeof(wifi_ap_record_t) * limit);
    
    ESP_ERROR_CHECK(esp_wifi_scan_get_ap_records(&limit, ap_records));

    ESP_LOGI(TAG, "--------------------------------------------------");
    ESP_LOGI(TAG, "ID  | SSID                             | RSSI");
    ESP_LOGI(TAG, "--------------------------------------------------");
    for (int i = 0; i < limit; i++) {
        strncpy(records[i].ssid, (char *)ap_records[i].ssid, 33);
        records[i].rssi = ap_records[i].rssi;
        ESP_LOGI(TAG, "[%2d] | %-32s | %d dBm", i, records[i].ssid, records[i].rssi);
    }
    ESP_LOGI(TAG, "--------------------------------------------------");
    
    free(ap_records);
    return limit;
}

esp_err_t wifi_connect(const char *ssid, const char *password) {
    s_retry_num = 0;
    wifi_config_t wifi_config = {0};
    strncpy((char *)wifi_config.sta.ssid, ssid, sizeof(wifi_config.sta.ssid));
    strncpy((char *)wifi_config.sta.password, password, sizeof(wifi_config.sta.password));

    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_LOGI(TAG, "Conectando a %s...", ssid);
    
    xEventGroupClearBits(s_wifi_event_group, WIFI_CONNECTED_BIT | WIFI_FAIL_BIT);
    esp_wifi_connect();

    EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group, 
                                           WIFI_CONNECTED_BIT | WIFI_FAIL_BIT, 
                                           pdFALSE, pdFALSE, portMAX_DELAY);

    if (bits & WIFI_CONNECTED_BIT) {
        ESP_LOGI(TAG, "Conexao estabelecida com sucesso!");
        return ESP_OK;
    } else {
        ESP_LOGE(TAG, "Falha ao conectar em %s.", ssid);
        return ESP_FAIL;
    }
}

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

    printf("%d bytes de %s: icmp_seq=%d ttl=%d tempo=%d ms\n",
           (int)recv_len, ipaddr_ntoa(&target_addr), seqno, ttl, (int)elapsed_time);
}

static void on_ping_timeout(esp_ping_handle_t hdl, void *args) {
    uint16_t seqno;
    ip_addr_t target_addr;
    esp_ping_get_profile(hdl, ESP_PING_PROF_SEQNO, &seqno, sizeof(seqno));
    esp_ping_get_profile(hdl, ESP_PING_PROF_IPADDR, &target_addr, sizeof(target_addr));
    printf("Resposta de %s: Timeout para icmp_seq=%d\n", ipaddr_ntoa(&target_addr), seqno);
}

static void on_ping_end(esp_ping_handle_t hdl, void *args) {
    uint32_t transmitted, received, total_time_ms;
    esp_ping_get_profile(hdl, ESP_PING_PROF_REQUEST, &transmitted, sizeof(transmitted));
    esp_ping_get_profile(hdl, ESP_PING_PROF_REPLY, &received, sizeof(received));
    esp_ping_get_profile(hdl, ESP_PING_PROF_DURATION, &total_time_ms, sizeof(total_time_ms));

    printf("\n--- Estatisticas ---\n");
    printf("%d pacotes enviados, %d recebidos, tempo %dms\n", (int)transmitted, (int)received, (int)total_time_ms);
    
    // Deleta a sessão para não vazar memória
    esp_ping_delete_session(hdl);
}

void wifi_ping(const char *target_ip, int count) {
    esp_ping_config_t ping_config = ESP_PING_DEFAULT_CONFIG();
    
    // Usa ipaddr_aton do LWIP para validar o IP
    if (ipaddr_aton(target_ip, &ping_config.target_addr) == 0) {
        printf("IP invalido: %s\n", target_ip);
        return;
    }
    
    ping_config.count = count;

    esp_ping_callbacks_t cbs = {
        .on_ping_success = on_ping_success,
        .on_ping_timeout = on_ping_timeout,
        .on_ping_end = on_ping_end,
        .cb_args = NULL
    };

    esp_ping_handle_t ping;
    // Cria a sessão baseada no cabeçalho ping_sock.h (presente no lwip)
    esp_ping_new_session(&ping_config, &cbs, &ping);
    esp_ping_start(ping);
}


void get_terminal_input(char *buffer, size_t size) {
    int c;
    size_t index = 0;
    
    fflush(stdout); 

    while (index < size - 1) {
        c = getchar();
        
        if (c == 0xFF || c == EOF) {
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }

        if (c == 0x08 || c == 0x7F) {
            if (index > 0) {
                index--;
                printf("\b \b");
                fflush(stdout);
            }
            continue;
        }

        if (c == '\n' || c == '\r') {
            if (index > 0) break;
            else continue;
        }

        putchar(c);
        fflush(stdout); 
        buffer[index++] = (char)c;
    }
    buffer[index] = '\0';
    putchar('\n');
    fflush(stdout);
}