#ifndef WIFI_MANAGER_H
#define WIFI_MANAGER_H

#include "esp_err.h"

#define MAX_SCAN_RECORDS 20

typedef struct {
    char ssid[33];
    int8_t rssi;
} wifi_info_t;

// Inicializa a stack WiFi
esp_err_t wifi_init_module(void);

// Scaneia e preenche o array records. Retorna o número de APs encontrados.
int wifi_scan_and_list(wifi_info_t *records);

// Conecta no WiFi
esp_err_t wifi_connect(const char *ssid, const char *password);

// Desliga o rádio Wi-Fi para economizar energia
esp_err_t wifi_deactivate(void);

// Executa o ping
void wifi_ping(const char *target_ip, int count);

#endif