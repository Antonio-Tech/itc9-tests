#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h> 
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_system.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "esp_event.h"

#include "wifi_manager.h"
#include "ble_manager.h"

static const char *TAG = "MAIN_MENU";

// Função utilitária para ler do terminal
void get_terminal_input(char *buffer, size_t size) {
    int c;
    size_t index = 0;
    fflush(stdout); 
    while (index < size - 1) {
        c = getchar();
        if (c == 0xFF || c == EOF) { vTaskDelay(pdMS_TO_TICKS(50)); continue; }
        // Backspace handling
        if (c == 0x08 || c == 0x7F) {
            if (index > 0) { index--; printf("\b \b"); fflush(stdout); }
            continue;
        }
        if (c == '\n' || c == '\r') { break; }
        putchar(c); fflush(stdout); 
        buffer[index++] = (char)c;
    }
    buffer[index] = '\0';
    putchar('\n'); fflush(stdout);
}

// Verifica se uma string contém apenas números
bool is_numeric_string(const char *str) {
    if (str == NULL || *str == '\0') return false;
    while (*str) {
        if (!isdigit((unsigned char)*str)) return false;
        str++;
    }
    return true;
}

void wifi_menu_workflow() {
    wifi_init_module(); 

    // Tenta iniciar o Wi-Fi e verifica erro
    esp_err_t err = wifi_init_module(); 
    if (err != ESP_OK) {
        printf("ERRO CRITICO: Nao foi possivel iniciar o modulo Wi-Fi (0x%x).\n", err);
        printf("Verifique o hardware ou reinicie o sistema.\n");
        return; 
    }
    
    // Scan
    wifi_info_t ap_list[MAX_SCAN_RECORDS];
    int ap_count = wifi_scan_and_list(ap_list);
    
    if (ap_count == 0) {
        printf("Nenhuma rede encontrada.\n");
        wifi_deactivate(); // <--- DESLIGA WI-FI
        return;
    }

    // Escolha com validação
    char input[64];
    int selection = -1;

    while (1) {
        printf("Digite o NUMERO da rede para conectar (ou 'v' para voltar): ");
        get_terminal_input(input, sizeof(input));
        
        // Opção de voltar
        if (input[0] == 'v' || input[0] == 'V') {
            wifi_deactivate(); // <--- DESLIGA WI-FI
            return;
        }

        // Verifica se digitou algo e se são apenas números
        if (strlen(input) > 0 && is_numeric_string(input)) {
            selection = atoi(input);
            
            // Verifica se o número está dentro da lista
            if (selection >= 0 && selection < ap_count) {
                break; // Entrada válida, sai do loop
            } else {
                printf("Numero invalido. Escolha entre 0 e %d.\n", ap_count - 1);
            }
        } else {
            printf("Entrada invalida. Digite apenas o NUMERO do indice.\n");
        }
    }

    printf("Digite a SENHA para '%s': ", ap_list[selection].ssid);
    char password[64];
    get_terminal_input(password, sizeof(password));

    // Conexão
    if (wifi_connect(ap_list[selection].ssid, password) == ESP_OK) {
        // Sub-menu pós conexão
        while(1) {
            printf("\n--- MENU WIFI CONECTADO ---\n");
            printf("1. Fazer Ping\n");
            printf("2. Voltar ao menu principal\n");
            printf("Escolha: ");
            get_terminal_input(input, sizeof(input));
            
            if (input[0] == '2') break;
            if (input[0] == '1') {
                printf("Digite o IP para ping (ex: 8.8.8.8): ");
                char ip_str[32];
                get_terminal_input(ip_str, sizeof(ip_str));
                wifi_ping(ip_str, 5);
            }
        }
    }
    wifi_deactivate();
}

void app_main(void) {
    // Inicialização global do NVS
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    char option[10];

    while (1) {
        printf("\n==================================\n");
        printf("   SISTEMA INTEGRADO ESP32        \n");
        printf("==================================\n");
        printf("1. Modo Wi-Fi (Scan / Conectar / Ping)\n");
        printf("2. Modo Bluetooth LE \n");
        printf("==================================\n");
        printf("Escolha uma opcao: ");
        
        get_terminal_input(option, sizeof(option));

        if (option[0] == '1') {
            wifi_menu_workflow();
        } 
        else if (option[0] == '2') {
            esp_err_t err = ble_init_module();
            if (err == ESP_OK) {
                ble_run_console();
                ble_deactivate(); 
            } else {
                ESP_LOGE(TAG, "Falha ao iniciar Bluetooth: %s", esp_err_to_name(err));
                printf("Erro ao iniciar subsistema Bluetooth.\n");
            }
        } 
        else {
            printf("Opcao invalida.\n");
        }
        
        vTaskDelay(pdMS_TO_TICKS(500));
    }
}