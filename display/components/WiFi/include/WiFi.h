#ifndef WIFI_H
#define WIFI_H

#include "esp_err.h"
#include "esp_http_server.h"
#include "esp_peripherals.h"

#define BINDING_MSG_LEN     64

// WIFI STATION RELATED ----------------------------------
#define WIFI_SUCCESS_ON_CONNECT (1 << 0)
#define WIFI_FAILURE_ON_CONNECT (1 << 1)
#define MAX_CONNECT_TRIES (2)

esp_err_t WTH_SwitchWiFi_PowerSave(void);
esp_err_t force_start_wifi(char *ssid, char *pass);
esp_err_t init_wifi_station(bool sync_mode);
esp_err_t deinit_wifi_station(void);
esp_err_t disconnect_wifi_with_cleanup(void);  // Disconnect and stop WiFi without driver deinit
bool is_wifi_connected(void);
esp_err_t setup_wifi(int argc, char *argv[]);

void start_wifi_pairing(void);
void stop_wifi_pairing(void);

void start_wifi_connecting(void);
void start_ble_wifi_sync(void);  // BLE-triggered WiFi sync (returns to HOME_SCREEN after completion)
bool conn_task_running(void);  // bool is_on_data_sync(void);

// connect wifi and download file, UI: SyncUP , call back download completed
typedef void (*nfc_sync_callback_t)(void);

typedef struct {
  nfc_sync_callback_t callback;
  bool is_from_cli;  // true if sync called from CLI, false if from NFC tag reading
} nfc_sync_param_t;

// Unified sync parameter structure
typedef struct {
    int sync_mode;                  // SYNC_MODE_FULL or SYNC_MODE_NFC
    nfc_sync_callback_t callback;   // Callback function (can be NULL for full sync)
    bool is_from_cli;              // true if sync called from CLI, false if from NFC tag reading
} unified_sync_param_t;

void start_nfc_sync(void *pvParameters);
void unified_sync_task(void *pvParameters);

// Function to set skip OTA flag for next sync
esp_err_t set_skip_ota_flag(esp_periph_handle_t periph, int argc, char *argv[]);

#endif
