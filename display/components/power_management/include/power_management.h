#ifndef POWER_MANAGEMENT_H
#define POWER_MANAGEMENT_H

#include "esp_peripherals.h"
#include "board.h"
#include "tca8418e.h"

typedef struct
{
    periph_service_handle_t *battery_service;
    periph_service_handle_t *input_ser;
    audio_board_handle_t *board_handle;
    esp_periph_set_handle_t *set;
    esp_periph_handle_t *sgm_handle;
}power_periph_info_t;

void set_power_info(periph_service_handle_t *battery_service, periph_service_handle_t *input_ser, audio_board_handle_t *board_handle, esp_periph_set_handle_t *set, esp_periph_handle_t *sgm_handle);
void system_deep_sleep(void);
void system_wake_up(void);
void start_shutdown(void);

bool is_wakeup_from_alarm(void);
void set_wakeup_from_alarm_false(void);

#endif
