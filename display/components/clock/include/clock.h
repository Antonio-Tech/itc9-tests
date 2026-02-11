#ifndef CLOCK_H
#define CLOCK_H

#include "esp_err.h"
#include <stdbool.h>

typedef void (*clock_screen_cb_t)(void);

void setup_clock_update_screen_cb(clock_screen_cb_t clock_screen_cb);
bool is_clock_initialized(void);
esp_err_t init_clock(void);
esp_err_t deinit_clock(void);


#endif