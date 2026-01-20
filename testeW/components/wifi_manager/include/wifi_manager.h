//include guard
#ifndef WIFI_MANAGER_H
#define WIFI_MANAGER_H

//biblioteca que controla o hardware fisico de Wi-Fi
#include "esp_wifi.h"
//quando as coisas acontecem
#include "esp_event.h"
//quem o ESP32 é na rede
#include "esp_netif.h"
//define a "linguagem de erros" do ESP32
#include "esp_err.h"

#define MAX_SCAN_RECORDS 20

typedef struct {
    char ssid[33];
    int8_t rssi;
} wifi_info_t;


void wifi_init(void);

int wifi_scan_and_list(wifi_info_t *records);

esp_err_t wifi_connect(const char *ssid, const char *password);

void wifi_ping(const char *target_ip, int count);

void get_terminal_input(char *buffer, size_t size);

#endif