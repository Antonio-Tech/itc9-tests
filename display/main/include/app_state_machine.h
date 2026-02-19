/**
 * @file app_state_machine.h
 * @author Igor Oliveira
 * @date 2025-06-09
 * @brief Header for application state machine
 *
 * Defines the finite state machine used to control screen and system behavior
 * based on user interaction (buttons), timeouts, and external events like NFC
 * detection or alarm triggers. This abstraction replaces the previous carousel logic.
 */

#ifndef APP_STATE_MACHINE_H
#define APP_STATE_MACHINE_H

#include "stdbool.h"
#include "s3_definitions.h"

typedef enum {
    STATE_OFF,
    STATE_STANBY,
    STATE_ALAR,
    STATE_HOME,
    STATE_TIME,
    STATE_ALBU_LIST,
    STATE_NOW_LAYING,
    STATE_BLUEOOTH,
    STATE_SETU_MODE,
    STATE_BT_CNN,
    STATE_BT_PIR,
    STATE_NETWRK,
    STATE_WIFICONN,
    STATE_WIFIPAIR,
    STATE_NFC,
    STATE_DISPAY,
    STATE_SETU,
    STATE_POWE_ON,
    STATE_SHUTOWN
} AppState;

void app_state_init(void);
void app_state_handle_event(AppEvent event);

#endif // APP_STATE_MACHINE_H
