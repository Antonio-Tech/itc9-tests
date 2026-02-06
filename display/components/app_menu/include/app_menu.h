#ifndef APP_MENU_H
#define APP_MENU_H

#include "esp_err.h"

esp_err_t app_menu_init(void);
esp_err_t app_menu_handle_key(char key_id);

#endif // APP_MENU_H
