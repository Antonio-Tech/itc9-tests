#include "app_event_handler.h"
#include "esp_log.h"

static const char *TAG = "app_event";

void app_event_handler(esp_event_base_t base, int32_t id, void *data) {
    ESP_LOGI(TAG, "Handling event base %s, id %" PRId32, base, id);
    // TODO: Central dispatcher
}
