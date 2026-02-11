#include "app_setup.h"
#include "esp_log.h"

static const char *TAG = "app_setup";

esp_err_t app_setup_run(void) {
    ESP_LOGI(TAG, "Running setup routine");
    // TODO: Handle first-time setup
    return ESP_OK;
}
