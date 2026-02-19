#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "wifi_manager.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "APP_MAIN";

void app_main(void) {
    ESP_LOGI(TAG, "Iniciando Aplicacao de Gerenciamento Wi-Fi...");
    
    wifi_init();
    vTaskDelay(pdMS_TO_TICKS(500));

    // Alocação para o scan
    wifi_info_t *results = malloc(sizeof(wifi_info_t) * MAX_SCAN_RECORDS);
    if (results == NULL) {
        ESP_LOGE(TAG, "Erro fatal: memoria insuficiente.");
        return;
    }

    bool connected = false;

    while (!connected) {
        int count = wifi_scan_and_list(results);

        if (count > 0) {
            char input_ssid[64];
            char input_pass[64];
            
            printf("\n[PROMPT] Digite o numero da rede desejada: ");
            get_terminal_input(input_ssid, sizeof(input_ssid));
            int idx = atoi(input_ssid);

            if (idx >= 0 && idx < count) {
                printf("[PROMPT] Digite a senha para '%s': ", results[idx].ssid);
                get_terminal_input(input_pass, sizeof(input_pass));
                
                esp_err_t ret = wifi_connect(results[idx].ssid, input_pass);
                
                if (ret == ESP_OK) {
                    ESP_LOGI(TAG, "Sistema online e pronto.");
                    connected = true; 
                } else {
                    ESP_LOGW(TAG, "Falha na tentativa. Vamos escanear e tentar novamente...");
                    vTaskDelay(pdMS_TO_TICKS(2000));
                }
            } else {
                ESP_LOGE(TAG, "Indice %d invalido.", idx);
            }
        } else {
            ESP_LOGE(TAG, "Nao foi possivel listar redes. Tentando novamente em 5s...");
            vTaskDelay(pdMS_TO_TICKS(5000));
        }
    }

    free(results);

    char ip_dest[64];
    printf("\nconectado! Digite um IP para executar ping (ex: 8.8.8.8) ou 'pular': ");
    get_terminal_input(ip_dest, sizeof(ip_dest));

    if (strcmp(ip_dest, "pular") != 0) {
        wifi_ping(ip_dest, 4); // Dispara 4 pings para o IP fornecido
    }

    while(1) {
        ESP_LOGI(TAG, "Task main executando...");
        vTaskDelay(pdMS_TO_TICKS(30000));
    }
}