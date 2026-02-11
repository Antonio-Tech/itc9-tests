/*
 * SPDX-FileCopyrightText: 2022-2023 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "sdkconfig.h"
#include "ulp_adc.h"
#include "esp_err.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_adc/adc_oneshot.h"
#include "hal/adc_hal_common.h"
#include "esp_private/esp_sleep_internal.h"
#include "esp_private/adc_share_hw_ctrl.h"

#include "ulp.h"
#include "ulp_main.h"
#include "driver/rtc_io.h"

#include "ulp/example_config.h"

static const char *TAG = "ulp_adc";
static adc_oneshot_unit_handle_t s_adc1_handle = NULL;

extern const uint8_t ulp_main_bin_start[] asm("_binary_ulp_main_bin_start");
extern const uint8_t ulp_main_bin_end[]   asm("_binary_ulp_main_bin_end");

esp_err_t ulp_adc_init(const ulp_adc_cfg_t *cfg)
{
    esp_err_t ret = ESP_OK;

    ESP_RETURN_ON_FALSE(cfg, ESP_ERR_INVALID_ARG, TAG, "cfg == NULL");
    ESP_RETURN_ON_FALSE(cfg->adc_n == ADC_UNIT_1, ESP_ERR_INVALID_ARG, TAG, "Only ADC_UNIT_1 is supported for now");

    //-------------ADC1 Init---------------//
    adc_oneshot_unit_init_cfg_t init_config1 = {
        .unit_id = cfg->adc_n,
        .ulp_mode = cfg->ulp_mode,
    };

    if (init_config1.ulp_mode == ADC_ULP_MODE_DISABLE) {
        /* Default to RISCV for backward compatibility */
        ESP_LOGI(TAG, "No ulp mode specified in cfg struct, default to riscv");
        init_config1.ulp_mode = ADC_ULP_MODE_RISCV;
    }

    ret = adc_oneshot_new_unit(&init_config1, &s_adc1_handle);
    if (ret != ESP_OK) {
        return ret;
    }

    //-------------ADC1 Config---------------//
    adc_oneshot_chan_cfg_t config = {
        .bitwidth = cfg->width,
        .atten = cfg->atten,
    };
    ret = adc_oneshot_config_channel(s_adc1_handle, cfg->channel, &config);
    if (ret != ESP_OK) {
        return ret;
    }

    //Calibrate the ADC
#if SOC_ADC_CALIBRATION_V1_SUPPORTED
    adc_set_hw_calibration_code(cfg->adc_n, cfg->atten);
#endif

    return ret;
}

esp_err_t ulp_adc_deinit(void)
{
    // No need to check for null-pointer and stuff, oneshot driver already does that
    return adc_oneshot_del_unit(s_adc1_handle);
}


void init_ulp_program(void)
{
    esp_err_t err = ulp_load_binary(0, ulp_main_bin_start,
            (ulp_main_bin_end - ulp_main_bin_start) / sizeof(uint32_t));
    if (err != ESP_OK) {
        printf("ulp_load_binary failed: %d\n", err);
    }

}

void start_ulp_program(void)
{
    rtc_gpio_init(GPIO_NUM_36);
    rtc_gpio_set_direction(GPIO_NUM_36, RTC_GPIO_MODE_DISABLED);
    rtc_gpio_pulldown_dis(GPIO_NUM_36);
    rtc_gpio_pullup_dis(GPIO_NUM_36);

 
     gpio_num_t gpio_num = GPIO_NUM_39;
    int rtcio_num = rtc_io_number_get(gpio_num);
    assert(rtc_gpio_is_valid_gpio(gpio_num) && "GPIO used for pulse counting must be an RTC IO");
    ulp_io_number = rtcio_num; /* map from GPIO# to RTC_IO# */

    rtc_gpio_init(gpio_num);
    rtc_gpio_set_direction(gpio_num, RTC_GPIO_MODE_INPUT_ONLY);
    rtc_gpio_pulldown_dis(gpio_num);
    rtc_gpio_pullup_dis(gpio_num);
    rtc_gpio_hold_en(gpio_num);

    ulp_adc_cfg_t cfg = {
        .adc_n    = EXAMPLE_ADC_UNIT,
        .channel  = EXAMPLE_ADC_CHANNEL,
        .width    = EXAMPLE_ADC_WIDTH,
        .atten    = EXAMPLE_ADC_ATTEN,
        .ulp_mode = ADC_ULP_MODE_FSM,
    };
    esp_err_t err1 = ulp_adc_init(&cfg);
    if (err1 != ESP_OK) {
        printf("ulp_adc_init failed: %d\n", err1);
    }
    ulp_low_thr = EXAMPLE_ADC_LOW_TRESHOLD;
    ulp_high_thr = EXAMPLE_ADC_HIGH_TRESHOLD;

    /* Set ULP wake up period to 5000 ms */
    // ESP_ERROR_CHECK(ulp_set_wakeup_period(0, 200000));  // 200ms
    ESP_ERROR_CHECK(ulp_set_wakeup_period(0, 5000000));  // 5000ms

#if CONFIG_IDF_TARGET_ESP32
    /* Disconnect GPIO12 and GPIO15 to remove current drain through
     * pullup/pulldown resistors on modules which have these (e.g. ESP32-WROVER)
     * GPIO12 may be pulled high to select flash voltage.
     */
    rtc_gpio_isolate(GPIO_NUM_12);
    // rtc_gpio_isolate(GPIO_NUM_15);
#endif // CONFIG_IDF_TARGET_ESP32

    esp_deep_sleep_disable_rom_logging(); // suppress boot messages

        /* Reset sample counter */
    ulp_sample_counter = 0;
    ulp_last_state = 1;
    ulp_wake_up_state = 0;

    /* Start the program */
    esp_err_t r1 = ulp_run(&ulp_entry - RTC_SLOW_MEM);
    if (r1 != ESP_OK) {
        printf("ulp_run failed: %d\n", err1);
    }
    // ESP_ERROR_CHECK(ulp_run(&ulp_entry - RTC_SLOW_MEM));
    ESP_LOGI(TAG, "start ulp program");
}
