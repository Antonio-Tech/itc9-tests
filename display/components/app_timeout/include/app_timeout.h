#ifndef APP_TIMEOUT_H
#define APP_TIMEOUT_H

#include "esp_err.h"
#include "s3_definitions.h"
#include "stdbool.h"

typedef void (*evt_state_machine_cb_t)(int event);


/**
 * @brief Initializes the inactivity timer for standby.
 */
esp_err_t app_timeout_init(void);

/**
 * @brief Resets the inactivity countdown timer for standby.
 */
esp_err_t app_timeout_reset(void);

/**
 * @brief Stops the inactivity timer for standby.
 */
esp_err_t app_timeout_stop(void);

/**
 * @brief Initializes the deep sleep timer.
 */
void app_timeout_deepsleep_init(void);

/**
 * @brief Starts the deep sleep countdown timer.
 */
void app_timeout_deepsleep_start(void);

/**
 * @brief Stops the deep sleep countdown timer.
 */
void app_timeout_deepsleep_stop(void);

void setup_state_handle_cb(evt_state_machine_cb_t *s_set_standby_screen_cb);

/**
 * @brief Restarts the standby sleep countdown timer.
 */
void app_timeout_restart(void);

// Add here to guarantee the visibility of the global poweroff flag
extern power_mode_t global_poweroff; // Global flag to indicate power off

#endif // APP_TIMEOUT_H