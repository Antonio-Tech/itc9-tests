/* Console example.

   This example code is in the Public Domain (or CC0 licensed, at your option.)

   Unless required by applicable law or agreed to in writing, this
   software is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
   CONDITIONS OF ANY KIND, either express or implied.
*/
/* Modified by Compal and Venturus teams in 2025/05 */

#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "nvs_flash.h"

// Hardware Abstraction
#include "board.h"
#include "esp_peripherals.h"
#include "periph_sdcard.h"

// Display & Graphics
#include "esp_lcd_panel_ops.h"
#include "lvgl.h"
#include "lv_port.h"
#include "backlight.h"

// Drivers Específicos
#include "tca8418e.h"
#include "periph_sgm41513.h"

// Necessários para inicialização
#include "s3_nvs_item.h"
#include "cjson_psram_hooks.h"
#include "voltage_kalman.h"
#include "s3_definitions.h" 

static const char *TAG = "MAIN_HW";

static esp_periph_set_handle_t set;
static audio_board_handle_t board_handle;
static esp_periph_handle_t sgm_handle = NULL;
static TaskHandle_t lvgl_task_handle = NULL;

// ============================================================================
// VARIÁVEIS 
// ============================================================================
bool global_plugged_in = false;
int gVoltage = 0; // Necessário para o callback de bateria
extern int s3_charger_status;

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
    if (kf) kf->x = z;
    return z;
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
// TASKS & HANDLERS
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


    // LCD e LVGL
    ESP_LOGI(TAG, "LCD & LVGL Init...");
    esp_lcd_panel_handle_t lcd_handle = audio_board_lcd_init(set, lcd_trans_done_cb);
    lv_port_init(lcd_handle);

    // Backlight
    ESP_LOGI(TAG, "Backlight On...");
    init_backlight();
    backlight_on();
    esp_lcd_panel_disp_on_off(lcd_handle, true);
    
    // Audio 
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

    ESP_LOGI(TAG, "Hardware Init Complete. Starting LVGL Task.");

    // Inicia a task do LVGL no Core 0
    xTaskCreatePinnedToCore(lvgl_task, "LVGL_task", (12 * 1024), NULL, 21, &lvgl_task_handle, 0);

    ESP_LOGI(TAG, "App Main Finalizado.");
}