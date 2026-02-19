//
// Created by Shane_Hwang on 2025/4/29.
//

#ifndef FAC_BT_H
#define FAC_BT_H

esp_err_t cli_setup_bt();

esp_err_t fac_bt_scan(esp_periph_handle_t periph, int argc, char *argv[]);

esp_err_t fac_ping_bt(esp_periph_handle_t periph, int argc, char *argv[]);

#endif // FAC_BT_H
