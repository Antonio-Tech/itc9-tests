/* Console example.

   This example code is in the Public Domain (or CC0 licensed, at your option.)

   Unless required by applicable law or agreed to in writing, this
   software is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
   CONDITIONS OF ANY KIND, either express or implied.
*/
/* Modified by Compal and Venturus teams in 2025/05 */

#include <string.h>
#include <ctype.h>
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/timers.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "input_key_com_user_id.h"
#include "nvs_flash.h"
#include "esp_efuse.h"
#include "esp_efuse_table.h"
#include "esp_log.h"
#include "sdkconfig.h"
#include "audio_element.h"
#include "audio_mem.h"
#include "board.h"
#include "soc/rtc.h"
#include "audio_common.h"
#include "fatfs_stream.h"
#include "raw_stream.h"
#include "i2s_stream.h"
#include "esp_audio.h"
#include "esp_peripherals.h"
#include "periph_sdcard.h"
#include "periph_wifi.h"
#include "periph_console.h"
#include "esp_decoder.h"
#include "amr_decoder.h"
#include "flac_decoder.h"
#include "ogg_decoder.h"
#include "opus_decoder.h"
#include "mp3_decoder.h"
#include "wav_decoder.h"
#include "aac_decoder.h"
#include "http_stream.h"
#include "wav_encoder.h"
#include "display_service.h"
#include "led_bar_is31x.h"
#include <dirent.h>
#include <sys/stat.h>
#include "cjson_psram_hooks.h"

#include "sdcard_list.h"
#include "sdcard_scan.h"
#include "audio_sys.h"
#include "periph_button.h"
#include "input_key_service.h"
#include "esp_lcd_panel_interface.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"

#include "audio_idf_version.h"
#include "esp_id3_parse.h"

#include "esp_netif.h"

#include "tca8418e.h"
#include "periph_sgm41513.h"

#include "i2c_bus.h"
#include "KTD2026.h"

#include "fac_wifi.h"
#include "fac_bt.h"
#include "manual_ota.h"
#include "audio_player.h"
#include "esp_spiffs.h"
#include "s3_bluetooth.h"
#include "esp_sleep.h"

#include "lvgl.h"
#include "lv_port.h"
#include "lv_screen_mgr.h"
#include "lv_decoders.h"
#include "app_state_machine.h"
#include "s3_nvs_item.h"
#include "backlight.h"
#include "s3_logger.h"
#include "s3_sync_account_contents.h"
#include "s3_album_mgr.h"
#include "s3_alarm_mgr.h"
#include "file_transfer.h"
#include "nfc-service.h"

#include "power_management.h"
#include "app_timeout.h"
#include "s3_https_cloud.h"
#include "voltage_kalman.h"
#include "s3_tracking.h"
#include "s3_definitions.h"
#include "WiFi.h"
#include "storage.h"

static const char *TAG = "MAIN_HW";

static esp_periph_set_handle_t set;
audio_board_handle_t board_handle;
esp_periph_handle_t sgm_handle = NULL;
static TaskHandle_t lvgl_task_handle = NULL;
periph_service_handle_t battery_service;
periph_service_handle_t input_ser;

// ============================================================================
// VARIÁVEIS GLOBAIS
// ============================================================================
power_mode_t global_poweroff = POWER_MODE_NORMAL;
bool global_plugged_in = false;
int gVoltage = 0;
uint8_t gPixseeStatus = 0;
uint8_t gPixseeMsg = 0;
bool gSyncInProgress = false;
bool gBTReconnectInProgress = false;
bool s3_ble_ready = false;
bool sleep_flag = false;
esp_err_t g_init_sdcard;

// Variáveis externas 
extern s3_screens_t s3_preLowBattery_screen;
extern bool s3_wifi_downloading;
extern int s3_battery_percent;
extern int s3_battery_level;
extern int s3_charger_status;
extern bool s3_boot_completed;
extern bool s3_show_higher_99;
extern bool s3_show_lower_10;
extern bool s3_show_lower_5;
extern bool s3_shutdown_started;
extern bool system_transition_in_progress;

// ============================================================================
// KALMAN FILTER 
// ============================================================================
void kalman_init(Kalman1D *kf, double init_x, double init_P, double Q, double R)
{
    if (kf) {
        kf->x = init_x;
        kf->P = init_P;
        kf->Q = Q;
        kf->R = R;
    }
}

double kalman_update(Kalman1D *kf, double z)
{
    if (!kf) return z;
    // Implementação padrão simples para leitura de voltagem
    double K = kf->P / (kf->P + kf->R);
    kf->x = kf->x + K * (z - kf->x);
    kf->P = (1.0 - K) * kf->P + kf->Q;
    return kf->x;
}

// ============================================================================
// UI HELLO WORLD
// ============================================================================
void draw_hello_world(void)
{
    lv_obj_t *scr = lv_scr_act();
    lv_obj_clean(scr);

    static lv_style_t style_bg;
    lv_style_init(&style_bg);
    lv_style_set_bg_color(&style_bg, lv_color_white());
    lv_style_set_bg_opa(&style_bg, LV_OPA_COVER);
    lv_obj_add_style(scr, &style_bg, 0);

    static lv_style_t style_text;
    lv_style_init(&style_text);
    lv_style_set_text_color(&style_text, lv_color_black());

    lv_obj_t *label = lv_label_create(scr);
    lv_label_set_text(label, "Hello World!");
    lv_obj_add_style(label, &style_text, 0);
    lv_obj_align(label, LV_ALIGN_CENTER, 0, -20);


    ESP_LOGI(TAG, "Hello World UI Desenhada");
}

// ============================================================================
// TASKS & CALLBACKS
// ============================================================================

void lvgl_task(void *pvParameters)
{
    ESP_LOGI(TAG, "LVGL Task Started");

    vTaskDelay(pdMS_TO_TICKS(100));

    draw_hello_world();

    while (1) {
        lvgl_process_step(5);
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

static esp_err_t sgm41513_event_handler(audio_event_iface_msg_t *event, void *context)
{
    if (event->source_type == PERIPH_ID_SGM41513) {
        switch (event->cmd) {
        case PERIPH_SGM41513_PLUGGED_IN:
            s3_charger_status = BATTERY_CHARGE;
            global_plugged_in = true;
            backlight_on();
            break;
        case PERIPH_SGM41513_UNPLUGGED:
            s3_charger_status = BATTERY_DISCHARGE;
            global_plugged_in = false;
            break;
        case PERIPH_SGM41513_CHARGE_DONE:
            s3_charger_status = BATTERY_CHARGE_FULL;
            break;
        }
    }
    return ESP_OK;
}

static esp_err_t battery_service_cb(periph_service_handle_t handle, periph_service_event_t *evt, void *ctx)
{
    if (evt && evt->data) {
        gVoltage = ((int)evt->data);
    }
    return ESP_OK;
}

static esp_err_t keys_ev_cb(periph_service_handle_t handle, periph_service_event_t *evt, void *ctx)
{
    ESP_LOGI(TAG, "Key Event: %d", (int)evt->data);
    return ESP_OK;
}

// ============================================================================
// APP MAIN
// ============================================================================

void app_main(void)
{   
    esp_log_level_set("*", ESP_LOG_INFO);
    esp_log_level_set(TAG, ESP_LOG_INFO);
    
    // Suprime logs de áudio
    esp_log_level_set("AUDIO_PIPELINE", ESP_LOG_WARN);
    esp_log_level_set("AUDIO_ELEMENT", ESP_LOG_WARN);
    esp_log_level_set("AUDIO_THREAD", ESP_LOG_WARN);

    ESP_LOGI(TAG, "Iniciando Hello World");

    cjson_init_with_psram();

    // NVS e System
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);
    
    s3_nvs_init();
    init_nvs();

    ESP_LOGI(TAG, "Audio Board Init...");
    board_handle = audio_board_init();
    
    // Peripherals Set
    esp_periph_config_t periph_cfg = DEFAULT_ESP_PERIPH_SET_CONFIG();
    set = esp_periph_set_init(&periph_cfg);

    // SD Card
    ESP_LOGI(TAG, "SD Card Init...");
    audio_board_sdcard_init(set, SD_MODE_1_LINE);

    // LCD e LVGL
    ESP_LOGI(TAG, "LCD & LVGL Init...");
    esp_lcd_panel_handle_t lcd_handle = audio_board_lcd_init(set, lcd_trans_done_cb);
    lv_port_init(lcd_handle);

    // Backlight
    ESP_LOGI(TAG, "Backlight On...");
    init_backlight();
    backlight_on();
    esp_lcd_panel_disp_on_off(lcd_handle, true);
    
    // Audio HAL 
    audio_board_audio_init();

    // SGM41513 (Carregador)
    ESP_LOGI(TAG, "SGM41513 Init...");
    esp_periph_set_register_callback(set, sgm41513_event_handler, NULL);
    periph_sgm41513_cfg_t sgm_cfg = PERIPH_SGM41513_DEFAULT_CONFIG();
    sgm_cfg.charge_current_ma = 1080.0;
    sgm_cfg.charge_voltage_mv = 4208.0;
    sgm_cfg.input_current_limit_ma = 1500.0; 
    sgm_handle = periph_sgm41513_init(&sgm_cfg);
    if(sgm_handle) {
        esp_periph_start(set, sgm_handle);
    }

    // Keys
    ESP_LOGI(TAG, "Keys Init...");
    audio_board_key_init(set);
    input_key_service_info_t input_key_info[] = INPUT_KEY_DEFAULT_INFO();
    input_key_service_cfg_t input_cfg = INPUT_KEY_SERVICE_DEFAULT_CONFIG();
    input_cfg.handle = set;
    input_cfg.based_cfg.task_stack = (4 * 1024);
    input_ser = input_key_service_create(&input_cfg);
    input_key_service_add_key(input_ser, input_key_info, INPUT_KEY_NUM);
    periph_service_set_callback(input_ser, keys_ev_cb, NULL);

    // Battery Service
    ESP_LOGI(TAG, "Battery Service Init...");
    battery_service = audio_board_battery_init(battery_service_cb);
    if(battery_service) periph_service_start(battery_service);

    sync_volume_with_hardware();

    ESP_LOGI(TAG, "Hardware Init Complete. Starting LVGL Task.");

    // Inicia a task do LVGL no Core 0
    xTaskCreatePinnedToCore(lvgl_task, "LVGL_task", (12 * 1024), NULL, 21, &lvgl_task_handle, 0);

    ESP_LOGI(TAG, "App Main Finalizado.");
}