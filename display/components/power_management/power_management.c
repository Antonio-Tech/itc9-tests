#include "KTD2026.h"
#include "esp_log.h"
#include "power_management.h"
#include "hal/i2c_types.h"
#include "s3_definitions.h"
#include "s3_tracking.h"
#include "esp_sleep.h"
#include "sdcard.h"
#include "tca8418e.h"
#include "ulp_adc.h"
#include "ulp_main.h"
#include "driver/gpio.h"
#include "esp_timer.h"
#include "periph_sgm41513.h"
#include "backlight.h"
#include "board.h"
#include "nfc-service.h"
#include "clock.h"
#include "storage.h"
#include "alarm_mgr.h"
#include <sys/time.h>
#include "audio_player.h"
#include <nvs_flash.h>

#include <time.h>
#include "s3_nvs_item.h"
#include "soc/rtc_cntl_reg.h"
#include "soc/soc.h"
#include "soc/soc_ulp.h"

bool normal_sleep = true;
static const char *TAG = "POWER_MANAGEMENT";
static esp_periph_handle_t sgm_handle = NULL;
static int HOME_KEY = GPIO_NUM_34;

static RTC_DATA_ATTR time_t saved_epoch = 0;
static RTC_DATA_ATTR int64_t saved_us = 0;
RTC_DATA_ATTR int64_t last_alarm = 0;
static bool s_alarm_wakeup = false;

static power_periph_info_t power_periph_info = {
    .battery_service = NULL,
    .board_handle = NULL,
    .input_ser = NULL,
    .set = NULL,
    .sgm_handle = NULL,
};

extern bool input_shutdown;

bool is_wakeup_from_alarm(void)
{
    return s_alarm_wakeup;
}

void set_wakeup_from_alarm_false(void)
{
    s_alarm_wakeup = false;
}

static void save_system_time(void)
{
    struct timeval tv_now;

    time(&saved_epoch);
    gettimeofday(&tv_now, NULL);
    saved_us = ((int64_t)tv_now.tv_sec * 1000000L) + (int64_t)tv_now.tv_usec;
}

static void restore_system_time_settings(void)
{
    char timezone[TIMEZONE_STR_SIZE] = "";
    struct timeval tv_new_settings;
    struct timeval tv_now;
    int64_t time_diff = 0;
    int64_t time_us = 0;
    time_t now;

    time(&now);
    gettimeofday(&tv_now, NULL);
    time_us = (int64_t)tv_now.tv_sec * 1000000L + (int64_t)tv_now.tv_usec;
    time_diff = (time_us - saved_us) / 1000000;

    tv_new_settings.tv_sec = (saved_epoch + time_diff);
    tv_new_settings.tv_usec = 0;

    settimeofday(&tv_new_settings, NULL);

    read_timezone(timezone);
    setenv("TZ", timezone, 1);
    tzset();
}

void set_power_info(periph_service_handle_t *battery_service, periph_service_handle_t *input_ser, audio_board_handle_t *board_handle, esp_periph_set_handle_t *set, esp_periph_handle_t *sgm_handle)
{
    power_periph_info.battery_service = battery_service;
    power_periph_info.board_handle = board_handle;
    power_periph_info.input_ser = input_ser;
    power_periph_info.set = set;
    power_periph_info.sgm_handle = sgm_handle;
}

void sys_shutdown(void)
{
    start_ulp_program();
    gpio_config_t io_conf = {
        .pin_bit_mask = 1ULL << HOME_KEY,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&io_conf);

    ESP_ERROR_CHECK(esp_sleep_enable_ext1_wakeup((1ULL << HOME_KEY),ESP_EXT1_WAKEUP_ALL_LOW));
    ESP_ERROR_CHECK( esp_sleep_enable_ulp_wakeup() );
    esp_sleep_pd_config(ESP_PD_DOMAIN_RTC_PERIPH, ESP_PD_OPTION_ON);
    fflush(stdout); // flush log buffer
    vTaskDelay(pdMS_TO_TICKS(100));

    ESP_LOGI(TAG, "System Deep Sleep");
    esp_deep_sleep_start();
}

void system_deep_sleep()
{
    s3_tracking_save_now(); // Save records before sleeping
    get_alarm_setting(TIMER_SOURCE_DEEP_SLEEP);
    tca8418e_shipmode_reg_setting();

    // CRITICAL: Always disable NFC before deep sleep to prevent shutdown timeout crash
    // This must be done regardless of normal_sleep flag
    nfc_disable();
    audio_power_off();
    ESP_LOGI(TAG, "audio_board_deinit");

    if(normal_sleep)
    {
        //vTaskDelay(pdMS_TO_TICKS(5000)); // wait power off image & sound finish.
        sgm41513_disable_watchdog(*power_periph_info.sgm_handle);
        backlight_off();
    }
    // CRITICAL: Stop all peripherals FIRST to prevent new events from being generated
    // This ensures the PERIPH_SERVICE_STATE_STOPPED message will be the last message in the queue
    if (*power_periph_info.set) {
        esp_periph_set_stop_all(*power_periph_info.set);
        ESP_LOGI(TAG, "All peripherals stopped");
    }
    // Manually unregister TCA8418E IRQ callback to stop hardware interrupts immediately
    // This is critical because esp_periph_set_stop_all() only sets a flag, it doesn't stop IRQs
    tca8418e_unregister_keyevent_callback();
    ESP_LOGI(TAG, "TCA button IRQ unregistered");
    // Wait for any in-flight IRQs to complete and flush any pending events
    vTaskDelay(pdMS_TO_TICKS(50));

    if (*power_periph_info.input_ser) {
        periph_service_destroy(*power_periph_info.input_ser);
        *power_periph_info.input_ser = NULL;
        ESP_LOGI(TAG, "Input key service stopped and destroyed");
    }
    // Wait for input key service task to exit cleanly before destroying peripheral set
    // This prevents use-after-free crash when the task tries to access freed periph_set_handle

    if (*power_periph_info.battery_service) {
        periph_service_stop(*power_periph_info.battery_service);
        battery_service_vol_report_switch(*power_periph_info.battery_service, false);
        vol_monitor_handle_t vol_monitor = battery_service_get_vol_monitor(*power_periph_info.battery_service);
        periph_service_destroy(*power_periph_info.battery_service);
        vol_monitor_destroy(vol_monitor);
        *power_periph_info.battery_service = NULL;
        ESP_LOGI(TAG, "battery service stopped and destroyed");
    }
	vTaskDelay(pdMS_TO_TICKS(10));
    if (*power_periph_info.set) {
        esp_periph_set_destroy(*power_periph_info.set); 
        *power_periph_info.set = NULL;
        // g_init_sdcard = NULL;
        ESP_LOGI(TAG, "esp_periph_set_destroy");
    }

    if(is_clock_initialized() == true) deinit_clock();
  
    spi_bus_free(SPI2_HOST);  //LCM release spi bus
    // SPI pin set input (LCM、SD Card)
    //const gpio_num_t lcd_pins[] = {GPIO_NUM_21, GPIO_NUM_19, GPIO_NUM_22, GPIO_NUM_4, GPIO_NUM_14, GPIO_NUM_15, GPIO_NUM_2};
    const gpio_num_t lcd_pins[] = {GPIO_NUM_21, GPIO_NUM_19, GPIO_NUM_22, GPIO_NUM_4, GPIO_NUM_14, GPIO_NUM_15, GPIO_NUM_2, GPIO_NUM_0, GPIO_NUM_5, GPIO_NUM_25, GPIO_NUM_26, GPIO_NUM_35};
    for (int i = 0; i < sizeof(lcd_pins)/sizeof(gpio_num_t); i++) 
    {
        gpio_reset_pin(lcd_pins[i]);
        gpio_set_direction(lcd_pins[i], GPIO_MODE_INPUT);
        gpio_pullup_dis(lcd_pins[i]);
        gpio_pulldown_dis(lcd_pins[i]);
    }

    tca8418e_off_gpio();

    ESP_LOGW(TAG, "Charger %s", tca8418_read_gpio(IO_PORT2, GPIO_CHARGE) == 1 ? "None" : "Connect"); 

#if 0
    start_ulp_program();
    gpio_config_t io_conf = {
        .pin_bit_mask = 1ULL << HOME_KEY,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&io_conf);

    ESP_ERROR_CHECK(esp_sleep_enable_ext1_wakeup((1ULL << HOME_KEY),ESP_EXT1_WAKEUP_ALL_LOW));
    ESP_ERROR_CHECK( esp_sleep_enable_ulp_wakeup() );
    esp_sleep_pd_config(ESP_PD_DOMAIN_RTC_PERIPH, ESP_PD_OPTION_ON);
    fflush(stdout); // flush log buffer
    vTaskDelay(pdMS_TO_TICKS(100));
    save_system_time();

    ESP_LOGI(TAG, "System Deep Sleep");
    esp_deep_sleep_start();
#else
    save_system_time();
    sys_shutdown();

#endif
}

void shutdown_screen_task(void *pvParamters)
{
    vTaskDelay(pdMS_TO_TICKS(3 * 1000));
    backlight_off();
    vTaskDelete(NULL);
}

void start_shutdown(void)
{
    // DMA optimization: Shutdown task doesn't need DMA-capable memory, use PSRAM (saves 3KB DMA)
    xTaskCreatePinnedToCoreWithCaps(shutdown_screen_task, "shutdown_task", (3 * 1024), NULL, 0, NULL, 0, MALLOC_CAP_SPIRAM);
}

void system_wake_up()
{
    restore_system_time_settings();
    esp_sleep_wakeup_cause_t wakeup_reason = esp_sleep_get_wakeup_cause();
    ESP_LOGI(TAG, "Deep sleep wakeup, Wakeup cause: %d", wakeup_reason);

    if(wakeup_reason == ESP_SLEEP_WAKEUP_TIMER)
    {
        s_alarm_wakeup = true;
        ESP_LOGI(TAG, "RTC wake up from alarm");
        REG_SET_FIELD(RTC_CNTL_STATE0_REG, RTC_CNTL_ULP_CP_SLP_TIMER_EN, 0);
    }
    if (wakeup_reason != ESP_SLEEP_WAKEUP_ULP && wakeup_reason != ESP_SLEEP_WAKEUP_EXT1) {
        ESP_LOGI(TAG, "Cold boot");
        init_ulp_program();
    }
        ulp_last_result &= UINT16_MAX;
        ESP_LOGI(TAG, "Thresholds:  low=%"PRIu32"  ADC Value=%"PRIu32, ulp_low_thr, ulp_last_result);
    if (wakeup_reason == ESP_SLEEP_WAKEUP_EXT1) {
        ESP_LOGI(TAG, "Wake up from HOME KEY Press");
        int64_t start_time = esp_timer_get_time();
        while ((esp_timer_get_time() - start_time) < 500000) { 
            if (gpio_get_level(HOME_KEY) == 1) {
                normal_sleep = false;
                ESP_LOGI(TAG, "HOME KEY released before 3 seconds, sleeping again.");
                // We need to battery service init/deinit to save 2mA.
                // TODO: Proper setting ADC.
                periph_service_handle_t bat_service = audio_board_battery_init(NULL);
                vTaskDelay(pdMS_TO_TICKS(10));
                periph_service_stop(bat_service);
                battery_service_vol_report_switch(bat_service, false);
                vol_monitor_handle_t vol_monitor = battery_service_get_vol_monitor(bat_service);
                periph_service_destroy(bat_service);
                vol_monitor_destroy(vol_monitor);

                // Reset alarm
                time_t now;
                time(&now);
                if (last_alarm > now) {
                    ESP_LOGI(TAG, "Set up alarm!");
                    esp_sleep_enable_timer_wakeup((last_alarm - now) * 1 * 1000 * 1000);
                }

                spi_bus_free(SPI2_HOST);  //LCM release spi bus
                // SPI pin set input (LCM、SD Card)
                const gpio_num_t lcd_pins[] = {GPIO_NUM_21, GPIO_NUM_19, GPIO_NUM_22, GPIO_NUM_4, GPIO_NUM_14, GPIO_NUM_15, GPIO_NUM_2, GPIO_NUM_0, GPIO_NUM_5, GPIO_NUM_25, GPIO_NUM_26, GPIO_NUM_35};
                for (int i = 0; i < sizeof(lcd_pins)/sizeof(gpio_num_t); i++) 
                {
                    gpio_reset_pin(lcd_pins[i]);
                    gpio_set_direction(lcd_pins[i], GPIO_MODE_INPUT);
                    gpio_pullup_dis(lcd_pins[i]);
                    gpio_pulldown_dis(lcd_pins[i]);
                }
                tca8418e_i2c_init();
                tca8418e_off_gpio();

                init_ulp_program();
                sys_shutdown();
            }
        }
        ESP_LOGI(TAG, "HOME KEY Long press, wake up normal boot");
    }
    if(wakeup_reason == ESP_SLEEP_WAKEUP_ULP){
        ulp_wake_up_state &= UINT16_MAX;
        ulp_last_state &= UINT16_MAX;
        g_init_sdcard = ESP_FAIL;
            
         ESP_LOGD(TAG, "ULP Wake up state %"PRIu32,ulp_wake_up_state);
         ESP_LOGD(TAG, "ULP last state %"PRIu32,ulp_last_state);
        if(ulp_wake_up_state == 1)
        {
            ESP_LOGW(TAG, "GPIO WAKE UP.");
        }
        else if(ulp_wake_up_state == 2)
        {
            ESP_LOGW(TAG, "ADC WAKE UP.");
            if(ulp_last_result < ulp_low_thr)
            {
                ESP_LOGW(TAG, "Battery low, entering ship mode.");
                sgm41513_direct_enter_ship_mode(I2C_NUM_0, GPIO_NUM_18, GPIO_NUM_23, 100000);
                vTaskDelay(pdMS_TO_TICKS(100));
                ESP_LOGW(TAG, "[LOWBAT]Ship mode will be activated in 15 seconds");
                //system_deep_sleep();
                sys_shutdown();
                do{}while(1);
            }
        }
    }
}
