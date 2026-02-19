#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>
#include <stdarg.h> 

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_system.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "esp_event.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"

#include "board.h"
#include "esp_peripherals.h"
#include "periph_sdcard.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_io.h"
#include "lvgl.h"
#include "backlight.h"
#include "tca8418e.h"
#include "periph_sgm41513.h"
#include "periph_button.h"
#include "input_key_service.h"

#include "cjson_psram_hooks.h"
#include "voltage_kalman.h"
#include "s3_definitions.h" 

#include "wifi_manager.h"
#include "ble_manager.h"

static const char *TAG = "MAIN";

static esp_periph_set_handle_t set;
static audio_board_handle_t board_handle;
static esp_periph_handle_t sgm_handle = NULL;
static TaskHandle_t lvgl_task_handle = NULL;
static periph_service_handle_t input_ser;
static periph_service_handle_t battery_service;

bool global_plugged_in = false;
int gVoltage = 0;
extern int s3_charger_status;

static SemaphoreHandle_t xGuiSemaphore = NULL;
static lv_obj_t *console_cont = NULL;
static lv_obj_t *console_label = NULL;

static lv_disp_drv_t disp_drv; 
#define LVGL_BUFFER_SIZE (240 * 40)
static lv_disp_draw_buf_t disp_buf;
static lv_color_t buf_1[LVGL_BUFFER_SIZE];
static lv_color_t buf_2[LVGL_BUFFER_SIZE];

void *lv_malloc(size_t size) { return malloc(size); }
void lv_free(void *p) { free(p); }
void *lv_realloc(void *p, size_t new_size) { return realloc(p, new_size); }

void silence_noisy_logs(void) {
    esp_log_level_set("*", ESP_LOG_INFO);
    
    esp_log_level_set("i2c.master", ESP_LOG_NONE);
    esp_log_level_set("gpio", ESP_LOG_NONE);
    esp_log_level_set("ALC5616", ESP_LOG_NONE);
    esp_log_level_set("KTD2026", ESP_LOG_NONE);
    esp_log_level_set("TCA8418E", ESP_LOG_NONE);
    esp_log_level_set("I2C_BUS", ESP_LOG_NONE);
    esp_log_level_set("AUDIO_BOARD", ESP_LOG_NONE);
    esp_log_level_set("AUDIO_PIPELINE", ESP_LOG_NONE);
    esp_log_level_set("PERIPH_SGM41513", ESP_LOG_NONE);
    esp_log_level_set("HEADPHONE", ESP_LOG_NONE);
    esp_log_level_set("S3_NVS", ESP_LOG_NONE);
    
    // BLE e Wi-Fi aparecem
    esp_log_level_set("BLE_MGR", ESP_LOG_INFO);
    esp_log_level_set("BTDM_INIT", ESP_LOG_INFO);
    esp_log_level_set("WIFI_MGR", ESP_LOG_INFO);
    esp_log_level_set("wifi", ESP_LOG_INFO);
    
    esp_log_level_set(TAG, ESP_LOG_INFO);
}

static void lvgl_flush_cb(lv_disp_drv_t *drv, const lv_area_t *area, lv_color_t *color_map) {
    esp_lcd_panel_handle_t panel_handle = (esp_lcd_panel_handle_t) drv->user_data;
    int offsetx1 = area->x1; int offsetx2 = area->x2;
    int offsety1 = area->y1; int offsety2 = area->y2;
    esp_lcd_panel_draw_bitmap(panel_handle, offsetx1, offsety1, offsetx2 + 1, offsety2 + 1, color_map);
}

void lcd_trans_done_cb(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_io_event_data_t *edata, void *user_ctx) {
    lv_disp_flush_ready(&disp_drv);
}

static void lv_tick_task(void *arg) { lv_tick_inc(5); }

static void lv_port_init_local(esp_lcd_panel_handle_t lcd_handle) {
    lv_init();
    lv_disp_draw_buf_init(&disp_buf, buf_1, buf_2, LVGL_BUFFER_SIZE);
    
    lv_disp_drv_init(&disp_drv);
    disp_drv.hor_res = 240; 
    disp_drv.ver_res = 240;
    disp_drv.flush_cb = lvgl_flush_cb;
    disp_drv.draw_buf = &disp_buf;
    disp_drv.user_data = lcd_handle;
    lv_disp_drv_register(&disp_drv);
    
    const esp_timer_create_args_t periodic_timer_args = { .callback = &lv_tick_task, .name = "lvgl_tick" };
    esp_timer_handle_t periodic_timer;
    ESP_ERROR_CHECK(esp_timer_create(&periodic_timer_args, &periodic_timer));
    ESP_ERROR_CHECK(esp_timer_start_periodic(periodic_timer, 5000));
}

static void gui_setup_console(void) {
    lv_obj_clean(lv_scr_act());

    // Fundo Preto
    static lv_style_t style_bg;
    if (style_bg.prop_cnt == 0) {
        lv_style_init(&style_bg);
        lv_style_set_bg_color(&style_bg, lv_color_black());
        lv_style_set_bg_opa(&style_bg, LV_OPA_COVER);
    }
    lv_obj_add_style(lv_scr_act(), &style_bg, 0);

    // Container
    console_cont = lv_obj_create(lv_scr_act());
    lv_obj_set_size(console_cont, LV_PCT(100), LV_PCT(100));
    lv_obj_add_style(console_cont, &style_bg, 0);
    lv_obj_set_scrollbar_mode(console_cont, LV_SCROLLBAR_MODE_AUTO); 

    // Estilo Texto
    static lv_style_t style_console;
    if (style_console.prop_cnt == 0) {
        lv_style_init(&style_console);
        lv_style_set_text_color(&style_console, lv_color_make(0, 255, 0)); 
        lv_style_set_text_font(&style_console, &lv_font_montserrat_14);
    }

    // Label
    console_label = lv_label_create(console_cont);
    lv_obj_add_style(console_label, &style_console, 0);
    lv_label_set_long_mode(console_label, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(console_label, LV_PCT(95)); 
    lv_label_set_text(console_label, "Iniciando Sistema...\n");
}

void gui_mirror_text(const char *text) {
    if (xGuiSemaphore != NULL && console_label != NULL) {
        if (xSemaphoreTake(xGuiSemaphore, 0) == pdTRUE) {
            #ifndef LV_LABEL_POS_LAST
            #define LV_LABEL_POS_LAST 0xFFFF
            #endif
            lv_label_ins_text(console_label, LV_LABEL_POS_LAST, text);
            lv_obj_scroll_to_y(console_cont, 0x7FFF, LV_ANIM_OFF); 
            xSemaphoreGive(xGuiSemaphore);
        }
    }
}

void console_printf(const char *fmt, ...) {
    char buffer[256];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buffer, sizeof(buffer), fmt, args);
    va_end(args);

    printf("%s", buffer);
    fflush(stdout);

    gui_mirror_text(buffer);
}

void console_clear_display() {
    if (xGuiSemaphore != NULL && console_label != NULL) {
        if (xSemaphoreTake(xGuiSemaphore, 0) == pdTRUE) {
            lv_label_set_text(console_label, "");
            xSemaphoreGive(xGuiSemaphore);
        }
    }
}

void lvgl_task(void *pvParameters) {
    ESP_LOGI(TAG, "LVGL Task Started");
    xGuiSemaphore = xSemaphoreCreateMutex();

    if (xSemaphoreTake(xGuiSemaphore, portMAX_DELAY) == pdTRUE) {
        gui_setup_console();
        lv_timer_handler(); 
        xSemaphoreGive(xGuiSemaphore);
    }

    vTaskDelay(pdMS_TO_TICKS(200));

    init_backlight();
    backlight_on();
    
    gui_mirror_text("Display Ativo.\n");

    while (1) {
        if (xSemaphoreTake(xGuiSemaphore, pdMS_TO_TICKS(20)) == pdTRUE) {
            lv_timer_handler();
            xSemaphoreGive(xGuiSemaphore);
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

void kalman_init(Kalman1D *kf, double init_x, double init_P, double Q, double R) {
    if (kf) { kf->x = init_x; kf->P = init_P; kf->Q = Q; kf->R = R; }
}


static esp_err_t sgm41513_event_handler(audio_event_iface_msg_t *event, void *context) {
    if (event->source_type == PERIPH_ID_SGM41513 && event->cmd == PERIPH_SGM41513_PLUGGED_IN) {
        backlight_on();
    }
    return ESP_OK;
}

static esp_err_t battery_service_cb(periph_service_handle_t handle, periph_service_event_t *evt, void *ctx) { 
    if (evt && evt->data) gVoltage = (int)evt->data;
    return ESP_OK; 
}
static esp_err_t keys_ev_cb(periph_service_handle_t handle, periph_service_event_t *evt, void *ctx) { return ESP_OK; }

void hardware_setup(void) {
    silence_noisy_logs();

    s3_nvs_init();
    board_handle = audio_board_init();
    
    esp_periph_config_t periph_cfg = DEFAULT_ESP_PERIPH_SET_CONFIG();
    set = esp_periph_set_init(&periph_cfg);
    audio_board_sdcard_init(set, SD_MODE_1_LINE);

    esp_lcd_panel_handle_t lcd_handle = audio_board_lcd_init(set, lcd_trans_done_cb);
    
    if (lcd_handle) {
        lv_port_init_local(lcd_handle); 
        esp_lcd_panel_disp_on_off(lcd_handle, true);
    }

    audio_board_audio_init();

    esp_periph_set_register_callback(set, sgm41513_event_handler, NULL);
    periph_sgm41513_cfg_t sgm_cfg = PERIPH_SGM41513_DEFAULT_CONFIG();
    sgm_cfg.charge_current_ma = 1080.0;
    sgm_cfg.input_current_limit_ma = 1500.0; 
    sgm_handle = periph_sgm41513_init(&sgm_cfg);
    if(sgm_handle) esp_periph_start(set, sgm_handle);

    battery_service = audio_board_battery_init(battery_service_cb);
    if(battery_service) periph_service_start(battery_service);
}

void get_terminal_input(char *buffer, size_t size) {
    int c;
    size_t index = 0;
    fflush(stdout); 

    while (index < size - 1) {
        c = getchar();
        if (c == 0xFF || c == EOF) { vTaskDelay(pdMS_TO_TICKS(50)); continue; }
        
        if (c == 0x08 || c == 0x7F) { // Backspace
            if (index > 0) { 
                index--; 
                printf("\b \b"); fflush(stdout); // Terminal
                gui_mirror_text("\b \b");        // Display
            }
            continue;
        }
        if (c == '\n' || c == '\r') { break; }
        
        putchar(c); fflush(stdout); // Terminal
        
        char str_char[2] = {(char)c, '\0'};
        gui_mirror_text(str_char);  // Display
        
        buffer[index++] = (char)c;
    }
    buffer[index] = '\0';
    
    printf("\n"); fflush(stdout);
    gui_mirror_text("\n");
}

bool is_numeric_string(const char *str) {
    if (!str || !*str) return false;
    while (*str) { if (!isdigit((unsigned char)*str)) return false; str++; }
    return true;
}

void wifi_console_workflow() {
    wifi_init_module(); 
    wifi_info_t ap_list[MAX_SCAN_RECORDS];
    
    console_printf("Escaneando Wi-Fi...\n");
    
    int ap_count = wifi_scan_and_list(ap_list);
    
    if (ap_count == 0) {
        console_printf("Nenhuma rede encontrada.\n");
        wifi_deactivate();
        return;
    }

    gui_mirror_text("\n--- Redes (Espelho) ---\n");
    for(int i=0; i<ap_count; i++) {
        char buf[64];
        snprintf(buf, sizeof(buf), "[%d] %s (%d)\n", i, ap_list[i].ssid, ap_list[i].rssi);
        gui_mirror_text(buf);
    }

    char input[64];
    int selection = -1;

    while (1) {
        console_printf("Digite o NUMERO da rede para conectar (ou 'v' para voltar): ");
        get_terminal_input(input, sizeof(input));
        
        if (input[0] == 'v' || input[0] == 'V') {
            wifi_deactivate();
            return;
        }

        if (strlen(input) > 0 && is_numeric_string(input)) {
            selection = atoi(input);
            if (selection >= 0 && selection < ap_count) {
                break;
            } else {
                console_printf("Numero invalido. Escolha entre 0 e %d.\n", ap_count - 1);
            }
        } else {
            console_printf("Entrada invalida. Digite apenas o NUMERO do indice.\n");
        }
    }

    console_printf("Digite a SENHA para '%s': ", ap_list[selection].ssid);
    char password[64];
    get_terminal_input(password, sizeof(password));

    if (wifi_connect(ap_list[selection].ssid, password) == ESP_OK) {
        while(1) {
            console_printf("\n--- MENU WIFI CONECTADO ---\n");
            console_printf("1. Fazer Ping\n");
            console_printf("2. Voltar ao menu principal\n");
            console_printf("Escolha: ");
            get_terminal_input(input, sizeof(input));
            
            if (input[0] == '2') break;
            if (input[0] == '1') {
                console_printf("Digite o IP para ping (ex: 8.8.8.8): ");
                char ip_str[32];
                get_terminal_input(ip_str, sizeof(ip_str));
                wifi_ping(ip_str, 5); 
            }
        }
    }
    wifi_deactivate();
}

// ============================================================================
// APP MAIN
// ============================================================================

void app_main(void) {
    silence_noisy_logs();

    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        nvs_flash_init();
    }
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    hardware_setup();

    xTaskCreatePinnedToCore(lvgl_task, "GUI_Task", 12*1024, NULL, 5, &lvgl_task_handle, 1);

    vTaskDelay(pdMS_TO_TICKS(1500));
    console_clear_display();
    console_printf("Terminal Pronto.\n");

    char option[10];

    while (1) {
        console_clear_display();
        
        console_printf("\n==================================\n");
        console_printf("   SISTEMA INTEGRADO ESP32        \n");
        console_printf("==================================\n");
        console_printf("1. Modo Wi-Fi (Scan / Conectar / Ping)\n");
        console_printf("2. Modo Bluetooth LE \n");
        console_printf("==================================\n");
        console_printf("Escolha uma opcao: ");
        
        get_terminal_input(option, sizeof(option));

        if (option[0] == '1') {
            console_clear_display();
            wifi_console_workflow();
        } 
        else if (option[0] == '2') {
            console_clear_display();
            
            ble_init_module(); 
            
            
            ble_run_console();
            
            ble_deactivate(); 
        } 
        else {
            console_printf("Opcao invalida.\n");
            vTaskDelay(pdMS_TO_TICKS(1000));
        }
        vTaskDelay(pdMS_TO_TICKS(200));
    }
}