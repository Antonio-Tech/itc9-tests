//
// Created by Shane_Hwang on 2025/4/29.
//

#ifndef FAC_WIFI_H
#define FAC_WIFI_H

esp_err_t fac_wifi_scan(esp_periph_handle_t periph, int argc, char *argv[]);

esp_err_t fac_wifi_mac(esp_periph_handle_t periph, int argc, char *argv[]);

esp_err_t fac_bt_mac(esp_periph_handle_t periph, int argc, char *argv[]);

void wifi_init_softap(void);

void wifi_deinit_softap(void);

#endif //FAC_WIFI_H
