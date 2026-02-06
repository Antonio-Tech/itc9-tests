/**
 * @file app_screen.c
 * @author Igor Oliveira
 * @date 2025-06-12
 * @brief Implementation of application screens
 */

#include <s3_logger.h>
#include "app_screen.h"
#include "esp_log.h"
#include "lvgl.h"

static const char *TAG = "APP_SCREEN";
// static lv_obj_t *current_screen = NULL;

void home_screen(void) {
    ESP_LOGI(TAG, "Home Screen");
    set_current_screen(HOME_SCREEN, NULL_SCREEN);
    // ToDo
}

void boot_screen(void) {
    ESP_LOGI(TAG, "Boot Screen");
    set_current_screen(BOOT_SCREEN, NULL_SCREEN);
    // ToDo
}

void play_screen(void) {
    ESP_LOGI(TAG, "Play Screen");
    set_current_screen(PLAY_SCREEN, NULL_SCREEN);
    // ToDo
}

void volume_screen(void) {
    ESP_LOGI(TAG, "Volume Screen");
    set_current_screen(VOLUME_UP_SCREEN, NULL_SCREEN);
    // ToDo
}

void clock_screen(void) {
    ESP_LOGI(TAG, "Clock Screen");
    set_current_screen(CLOCK_SCREEN, NULL_SCREEN);
    // ToDo
}

void alarm_screen(void) {
    ESP_LOGI(TAG, "Alarm Screen");
    set_current_screen(ALARM_SCREEN, NULL_SCREEN);
    // ToDo
}

void display_screen(void) {
    ESP_LOGI(TAG, "Display Screen");
    set_current_screen(DISPLAY_SCREEN, NULL_SCREEN);
    // ToDo
}

void bluetooth_screen(void) {
    ESP_LOGI(TAG, "Bluetooth Screen");
    set_current_screen(BLUETOOTH_SCREEN, NULL_SCREEN);
    // ToDo
}

void bluetooth_settings_screen(void) {
    ESP_LOGI(TAG, "Bluetooth Settings Screen");
    set_current_screen(BLUETOOTH_SCREEN, NULL_SCREEN);
    // ToDo
}

void wifi_settings_screen(void) {
    ESP_LOGI(TAG, "WiFi Settings Screen");
    set_current_screen(WIFI_SEARCH_SCREEN, NULL_SCREEN);
    // ToDo
}

void wifi_pairing_screen(void) {
    ESP_LOGI(TAG, "WiFi Pairing Screen");
    set_current_screen(BLE_PAIRING_SCREEN, NULL_SCREEN);
    // ToDo
}

void data_sync_screen(void) {
    ESP_LOGI(TAG, "Data Sync Screen");
    set_current_screen(DATA_SYNC_SCREEN, NULL_SCREEN);
    // ToDo
}

void ota_screen(void) {
    ESP_LOGI(TAG, "OTA Screen");
    set_current_screen(OTA_SCREEN, NULL_SCREEN);
    // ToDo
}

void nfc_screen(void) {
    ESP_LOGI(TAG, "NFC Screen");
    set_current_screen(NFC_SCREEN, NULL_SCREEN);
    // ToDo
}

void power_low_screen(void) {
    ESP_LOGI(TAG, "Power Low Screen");
    set_current_screen(POWER_LOW_SCREEN, NULL_SCREEN);
    // ToDo
}

void standby_screen(void) {
    ESP_LOGI(TAG, "Standby Screen");
    set_current_screen(STANDBY_SCREEN, NULL_SCREEN);
    // ToDo
}

void shutdown_screen(void) {
    ESP_LOGI(TAG, "Shutdown Screen");
    set_current_screen(SHUTDOWN_SCREEN, NULL_SCREEN);
    // ToDo
}

// void network_screen(void) {
//     ESP_LOGI(TAG, "Network Screen");
//     set_current_screen(NETWORK_SCREEN);
//     // ToDo
// }

void nfc_language_screen(void) {
    ESP_LOGI(TAG, "NFC Language Screen");
    set_current_screen(NFC_LANGUAGE_SCREEN, NULL_SCREEN);
    // ToDo
}

// void idle_mode(void) {
//     ESP_LOGI(TAG, "Idle Mode");
//     set_current_screen(IDLE_MODE);
//     // ToDo
// }