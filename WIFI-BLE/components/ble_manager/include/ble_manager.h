#ifndef BLE_MANAGER_H
#define BLE_MANAGER_H

// Inicializa a stack Bluetooth
void ble_init_module(void);

// Desativa e libera recursos do Bluetooth
void ble_deactivate(void);

// Entra no modo console do BLE (bloqueante até usuário digitar 'sair')
void ble_run_console(void);

#endif