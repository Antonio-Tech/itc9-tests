#include "app_menu.h"
#include "esp_log.h"

static const char *TAG = "app_menu";

esp_err_t app_menu_init(void) {
    ESP_LOGI(TAG, "Initializing menu");
    // TODO: Setup menu state and logic
    return ESP_OK;
}

esp_err_t app_menu_handle_key(char key_id) {
    ESP_LOGI(TAG, "Handling key: %c", key_id);
    // TODO: React to key press
    return ESP_OK;
}
