#ifndef APP_EVENT_HANDLER_H
#define APP_EVENT_HANDLER_H

#include "esp_event.h"

void app_event_handler(esp_event_base_t base, int32_t id, void *data);

#endif // APP_EVENT_HANDLER_H
