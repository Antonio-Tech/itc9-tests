#ifndef BLE_MANAGER_H
#define BLE_MANAGER_H

#include "esp_err.h"

// Inicializa a stack Bluetooth
esp_err_t ble_init_module(void);

// Desativa e libera recursos do Bluetooth
esp_err_t ble_deactivate(void);

// Entra no modo console do BLE (bloqueante até usuário digitar 'sair')
void ble_run_console(void);

#endif