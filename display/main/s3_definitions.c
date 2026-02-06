//
// Created by Antonio_Pereira on 2025/5/20.
//

#include <stdio.h>
#include <errno.h>
#include <string.h>
#include <dirent.h>
#include <sys/stat.h>
#include "s3_definitions.h"
#include "s3_logger.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "cJSON.h"
#include "s3_sync_account_contents.h"
#include "s3_album_mgr.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "S3_DEFINITIONS";

// Content type names for logging
const char *content_type_names[] = {
    "COVER",
    "LANGUAGE_BADGE",
    "BATTERY_BADGE",
    "PLAYER_BADGE",
    "BT_BADGE",
    "POPUP",
    "MENU"
};

// Note: Album management moved to s3_album_mgr.c
// Album variables and functions now in s3_album_mgr.c

// PUBLIC API FUNCTIONS - END

// Separated album arrays - END

// ALARM: definitions - START
// NOTE: s3_alarm_mgr will be moved to a dedicated component and this definition will stay on main
const s3_alarm_handler_t s3_alarms[] = {
    { ALARM_1, "Morning",     AUDIO_ALARM_1, ICON_ALARM_1, ANIM_ALARM_1 },  // Morning uses morning sound/icon/animation
    { ALARM_2, "Yummy",       AUDIO_ALARM_2, ICON_ALARM_2, ANIM_ALARM_2 },  // Yummy uses yummy sound/icon/animation  
    { ALARM_3, "Move",        AUDIO_ALARM_3, ICON_ALARM_3, ANIM_ALARM_3 },  // Move uses move sound/icon/animation
    { ALARM_4, "Ready",       AUDIO_ALARM_4, ICON_ALARM_4, ANIM_ALARM_4 },  // Ready uses ready sound/icon/animation
    { ALARM_5, "Clean",       AUDIO_ALARM_5, ICON_ALARM_5, ANIM_ALARM_5 },  // Clean uses clean sound/icon/animation
    { ALARM_6, "Night",       AUDIO_ALARM_6, ICON_ALARM_6, ANIM_ALARM_6 },  // Night uses night sound/icon/animation
    { ALARM_7, "Naptime",     AUDIO_ALARM_7, ICON_ALARM_7, ANIM_ALARM_7 }   // Naptime uses naptime sound/icon/animation
};

// Use ALARMS_QTD enum for array size instead of s3_alarms_count
// ALARM: definitions - END

// SCREEN: definitions - START
// NOTE: s3_screen_mgr will be moved to a dedicated component and this definition will stay on main
const s3_screen_assmbler_t s3_screen_resources[] = {
    { BOOT_SCREEN,                "Boot [Animation]",         NO_RESOURCE,        BOOT_TRANSITION,          BASE_ANIMATION  },
    { POWER_LOW_SCREEN,           "LowPower [Popup]",         NO_RESOURCE,        PWR_POP_TRANSITION,       BASE_ANIMATION  },
    { STANDBY_SCREEN,             "Standby [Exception]",      NO_RESOURCE,        NO_TRANSITION,            BASE_EXCEPTION  },
    { POWER_OFF_SCREEN,           "Power off [Exception]",    NO_RESOURCE,        NO_TRANSITION,            BASE_EXCEPTION  }, 
    { SHUTDOWN_SCREEN,            "Shutdown [Animation]",     ICON_POWER_OFF,     SHUTDOWN_TRANSITION,      BASE_ANIMATION  },
    { HOME_SCREEN,                "Home [Screen]",            NO_RESOURCE,        NO_TRANSITION,            BASE_ANCHOR     },
    { PLAY_SCREEN,                "Player [Screen]",          NO_RESOURCE,        NO_TRANSITION,            BASE_ANCHOR     },
    { PAUSE_SCREEN,               "Pause [Screen]",           NO_RESOURCE,        NO_TRANSITION,            BASE_ANCHOR     },
    { VOLUME_UP_SCREEN,           "Volume Up [Animation]",    NO_RESOURCE,        VOLUME_TRANSITION,        BASE_ANIMATION  },
    { VOLUME_DOWN_SCREEN,         "Volume Dn [Animation]",    NO_RESOURCE,        VOLUME_TRANSITION,        BASE_ANIMATION  },
    { CLOCK_SCREEN,               "Clock [Screen]",           NO_RESOURCE,        NO_TRANSITION,            BASE_ANCHOR     },
    { ALARM_SCREEN,               "Alarm [Animation]",        ICON_ALARM_1,       ALARM_TRANSITION,         BASE_ANIMATION  },
    { DISPLAY_SCREEN,             "Display [Menu]",           ICON_BIRGHTNESS,    NO_TRANSITION,            BASE_ANCHOR     },
    { DISPLAY_SETTINGS_SCREEN,    "DY Setings [Screen]",      NO_RESOURCE,        NO_TRANSITION,            BASE_EXCEPTION  },
    { BRIGHTNESS_UP_SCREEN,       "Brightness Up [Screen]",   NO_RESOURCE,        BRIGHT_TRANSITION,        BASE_ANIMATION  },
    { BRIGHTNESS_DOWN_SCREEN,     "Brightness Dn [Screen]",   NO_RESOURCE,        BRIGHT_TRANSITION,        BASE_ANIMATION  },
    { BLUETOOTH_SCREEN,           "Bluetooth [Menu]",         ICON_BT,            NO_TRANSITION,            BASE_ANCHOR     },
    { BLUETOOTH_SCAN_SCREEN,      "BT Pair [Screen]",         ICON_BT_SEARCH,     NO_TRANSITION,            BASE_EXCEPTION  },
    { WIFI_SEARCH_SCREEN,         "Network [Menu]",           ICON_WIFI_SECH,     NO_TRANSITION,            BASE_EXCEPTION  },
    { BLE_PAIRING_SCREEN,         "NT Pairing [Screen]",      ICON_WIFI_SECH,     PAIRING_TRANSITION,       BASE_EXCEPTION  },
    { WIFI_UNKNOWN_SCREEN,        "NT Unknow [Screen]",       ICON_WIFI_UNKW,     BACK_TO_HOME_TRANSITION,  BASE_ANCHOR     },
    { WIFI_DISCONNECT_SCREEN,     "NT Disconnect [Screen]",   ICON_WIFI_DISC,     NO_TRANSITION,            BASE_ANCHOR     },
    { DATA_SYNC_SCREEN,           "Data Sync [Screen]",       NO_RESOURCE,        NO_TRANSITION,            BASE_EXCEPTION  },
    { OTA_SCREEN,                 "Ota [Screen]",             ICON_FIRMWARE,      NO_TRANSITION,            BASE_EXCEPTION  },
    { WIFI_PLUG_IN_SCREEN,        "NT Plug_IN[Screen]",       ICON_PLUG_IN,       NO_TRANSITION,            BASE_EXCEPTION  },
    { WIFI_SYNCED_SCREEN,         "NT SyncEnd[Screen]",       ICON_WIFI_SYNCED,   WIFI_SYNCED_TRANSITION,   BASE_EXCEPTION  },
    { NFC_SCREEN,                 "NFC [Menu]",               ICON_NFC,           NO_TRANSITION,            BASE_ANCHOR     },
    { NFC_LANGUAGE_SCREEN,        "NFC Language [Screen]",    NO_RESOURCE,        NO_TRANSITION,            BASE_EXCEPTION  },
    { NFC_ACTIVATION_SCREEN,      "NFC Activation [Screen]",  NO_RESOURCE,        NO_TRANSITION,            BASE_EXCEPTION  },
    { NFC_CONTENT_SCREEN,         "NFC Content [Screen]",     NO_RESOURCE,        NFC_TIMEOUT_TRANSITION,   BASE_EXCEPTION  },
    { POWER_CHARGE_SCREEN,        "Charging [Screen]",        NO_RESOURCE,        PWR_POP_TRANSITION,       BASE_ANIMATION  },
    { POWER_FULL_SCREEN,          "Full power [Screen]",      NO_RESOURCE,        PWR_POP_TRANSITION,       BASE_ANIMATION  },
    { FAC_RESET_SCREEN,           "Factory mode [Exception]", NO_RESOURCE,        NO_TRANSITION,            BASE_EXCEPTION  },
    { COUNTDOWN_SCREEN,           "Countdn mode [Exception]", NO_RESOURCE,        NO_TRANSITION,            BASE_EXCEPTION  },
    { POWER_ON_KID_SCREEN,        "Power on Kid [Exception]", NO_RESOURCE,        POWER_ON_KID_TRANSITION,  BASE_EXCEPTION  },
    { NFC_WIFI_SEARCH_SCREEN,     "NFC WiFi Search",          ICON_WIFI_SECH,     NO_TRANSITION,            BASE_EXCEPTION  },
    { NFC_WIFI_DISCONNECT_SCREEN, "NFC WiFi Discconnect",     ICON_WIFI_DISC,     NFC_WIFI_DSC_TRANSITION,  BASE_EXCEPTION  },
    { NFC_NO_CONTENT_SCREEN,      "NFC No Content [Screen]",  ICON_NFC_NO_CONTENT, NFC_TIMEOUT_TRANSITION,  BASE_EXCEPTION  },
    { POWER_LOW_PLUG_IN_SCREEN,   "LowPower [plug-in]",       ICON_PLUG_IN,       PWR_POP_TRANSITION,       BASE_ANIMATION  },
    { VOLUME_SCREEN,              "VOLUME SCREEN",            ICON_VOLUME,        NO_TRANSITION,            BASE_ANCHOR     },
    { WIFI_SYNC_MAI_SCREEN,       "WIFI_SYNC_MAI SCREEN",     ICON_WIFI_SYNC_MAI, NO_TRANSITION,            BASE_ANCHOR     },
    { WIFI_SYNC_ERR_SCREEN,       "WIFI_SYNC_ERR SCREEN",     ICON_WIFI_SYNC_ERR, WIFI_SYNCED_TRANSITION,   BASE_EXCEPTION  },
    { WIFI_SYNC_SUC_SCREEN,       "WIFI_SYNC_SUC SCREEN",     ICON_WIFI_SYNC_SUC, WIFI_SYNCED_TRANSITION,   BASE_EXCEPTION  },
    { WIFI_SYNC_N_SCREEN,         "WIFI_SYNC_N SCREEN",		  ICON_WIFI_SYNC0,    NO_TRANSITION,            BASE_EXCEPTION  },
    { WIFI_ERR_SCREEN,            "WIFI_ERR",        		  ICON_WIFI_ERROR,    WIFI_SYNCED_TRANSITION,   BASE_EXCEPTION  },
    { ACC_INV_FAC_RESET_SCREEN,   "Account invalid do factory mode [Exception]",  NO_RESOURCE,   NO_TRANSITION,   BASE_EXCEPTION  },
    { DUMMY_SCREEN,               "Dummy [Exception]",        NO_RESOURCE,        NO_TRANSITION,            BASE_EXCEPTION  }
};

// Use SCREENS_QTD enum for array size instead of s3_screen_resources_count
// SCREEN: definitions - END

// Current album and alarm pointers - managed by respective managers
// NOTE: s3_album_mgr and s3_alarm_mgr will be moved to dedicated components
s3_album_handler_t *s3_current_album = NULL;
s3_alarm_handler_t *s3_current_alarm = NULL;

// Album navigation state - managed by album manager
// NOTE: s3_album_mgr will be moved to a dedicated component and these variables will stay on main
size_t s3_current_idx = 0;
size_t s3_current_size = 0;
size_t s3_current_idx_track = 0;
size_t s3_current_size_track = 0;

// Audio settings - managed by audio player
// NOTE: s3_audio_player will be moved to a dedicated component and these variables will stay on main
audio_sink_t s3_active_sink = AUDIO_SINK_AUTO;
playback_mode_t s3_playback_mode = PLAYBACK_MODE_SEQUENTIAL;
auto_play_mode_t s3_auto_play_mode = AUTO_PLAY_OFF;

// System state variables - managed by main application
int s3_volume_level = VOLUME_LEVEL_3;               // Default to middle volume
int s3_brightness_level = BRIGHTNESS_LEVEL_2;       // Default to middle brightness
int s3_battery_level = BATTERY_UNREAD;              // Will be read from hardware
int s3_battery_percent = 0;                         // Will be read from hardware
int s3_charger_status = BATTERY_DISCHARGE;          // Default to discharge
int s3_selected_language = LANGUAGE_ENGLISH;        // Default to English
char s3_qr_payload[QRCODE_CONTENT_LEN] = {0};       // Empty by default
char s3_binding_msg[BINDING_MSG_LEN] = BIND_MSG_NONE; // Default to "No results"
int s3_pairing_status = BT_UNPAIRED;                // Default to unpaired
int s3_nfc_content_type = NFC_CONT_DEFAULT;         // Default NFC content type
bool s3_use_animations = true;                      // Default to animations enabled
bool s3_boot_completed = false;                     // Boot not completed initially
bool s3_shutdown_started = false;                   // Not shutting down initially
int s3_sync_stage = 0;                              // Current data sync stage (0=prepare, 1=wifi, 2=resource, 3=account)
bool s3_data_sync_show_wait = false;                // Flag to show wait screen during data sync
bool s3_show_lower_5 = true;                        // Show low battery warning initially
bool s3_show_lower_10 = true;                       // Show low battery warning initially
bool s3_show_higher_99 = true;                      // Show full battery indication initially

// Global system state flags
bool system_transition_in_progress = false;         // Blocks buttons during audio/screen transitions

// Timer for WiFi pairing - managed by WiFi component
// NOTE: WiFi components will be moved to dedicated components and this variable will stay on main
lv_timer_t *wifi_pairing_defer_timer = NULL;
static bool memory_logs_track_f = false;

// Message handler bit manipulation functions - START
void set_pixsee_msg(uint8_t bit_position, bool value) {
    if (bit_position > 7) return; // Safety check for 8-bit value

    if (value) {
        gPixseeMsg |= (1 << bit_position);  // Set bit
    } else {
        gPixseeMsg &= ~(1 << bit_position); // Clear bit
    }

    ESP_LOGI(TAG, "set_pixsee_msg: bit %d = %s, gPixseeMsg = 0x%02X",
             bit_position, value ? "true" : "false", gPixseeMsg);

    // Automatically sync status with BLE if connected
    // This ensures the app developer always gets the latest status via BLE notifications
    dev_ctrl_update_values(NO_UPDATE, gPixseeMsg, NO_UPDATE);
}

void set_pixsee_status(uint8_t status) {
    gPixseeStatus = status;
    ESP_LOGI(TAG, "set_pixsee_status: gPixseeStatus = 0x%02X", gPixseeStatus);
    
    // Automatically sync status with BLE if connected
    // This ensures the app developer always gets the latest status via BLE notifications
    dev_ctrl_update_values(NO_UPDATE, NO_UPDATE, gPixseeStatus);
}

bool get_pixsee_msg(uint8_t bit_position) {
    if (bit_position > 7) return false; // Safety check for 8-bit value
    return (gPixseeMsg & (1 << bit_position)) != 0;
}

void set_default_pixsee_info(void) {
    gPixseeMsg = S3ER_SYSTEM_IDLE;
    ESP_LOGI(TAG, "clear_all_pixsee_msg: gPixseeMsg = 0x%02X", gPixseeMsg);

    gPixseeMsg = S3MSG_SYSTEM_RESET;
    ESP_LOGI(TAG, "set_all_pixsee_msg: gPixseeMsg = 0x%02X", gPixseeMsg);

    // Automatically sync status with BLE if connected
    // This ensures the app developer always gets the latest status via BLE notifications
    dev_ctrl_update_values(NO_UPDATE, gPixseeMsg, gPixseeStatus);
}

void memory_status(void)
{
    // ===== HEAP (RAM interna de 8 bits) =====
    size_t total_8bit = heap_caps_get_total_size(MALLOC_CAP_8BIT);
    size_t free_8bit  = heap_caps_get_free_size(MALLOC_CAP_8BIT);
    size_t used_8bit  = total_8bit - free_8bit;
    int percent_8bit  = (total_8bit > 0) ? (used_8bit * 100 / total_8bit) : 0;

    size_t free_heap = esp_get_free_heap_size();
    size_t min_free_heap = esp_get_minimum_free_heap_size();
    
    ESP_LOGW(TAG, "===============================================");
    ESP_LOGW(TAG, "Memory Allocation Check");
    ESP_LOGW(TAG, "Total Heap memory (8bit): (%d / %d) [%d%%]", (int)used_8bit, (int)total_8bit, percent_8bit);
    ESP_LOGW(TAG, "Current free memory: %d bytes", free_heap);
    ESP_LOGW(TAG, "Minimum free heap size: %d bytes", min_free_heap);
    ESP_LOGW(TAG, "===============================================");

    // ===== DRAM interna =====
    size_t total_dram = heap_caps_get_total_size(MALLOC_CAP_INTERNAL);
    size_t free_dram  = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    size_t used_dram  = total_dram - free_dram;
    int percent_dram  = (total_dram > 0) ? (used_dram * 100 / total_dram) : 0;

    ESP_LOGW(TAG, "DRAM: (%d / %d) [%d%%]", (int)used_dram, (int)total_dram, percent_dram);

    // ===== PSRAM =====
    size_t total_psram = heap_caps_get_total_size(MALLOC_CAP_SPIRAM);
    if (total_psram > 0) {
        size_t free_psram = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
        size_t used_psram = total_psram - free_psram;
        int percent_psram = (used_psram * 100 / total_psram);

        ESP_LOGW(TAG, "PSRAM: (%d / %d) [%d%%]", (int)used_psram, (int)total_psram, percent_psram);
    }

    // ===== DMA =====
    size_t total_dmaram = heap_caps_get_total_size(MALLOC_CAP_DMA);
    if (total_dmaram > 0) {
        size_t free_dmaram = heap_caps_get_free_size(MALLOC_CAP_DMA);
        size_t used_dmaram = total_dmaram - free_dmaram;
        int percent_dmaram = (used_dmaram * 100 / total_dmaram);
        ESP_LOGW(TAG, "DMA RAM: (%d / %d) [%d%%]", (int)used_dmaram, (int)total_dmaram, percent_dmaram);
    }
    ESP_LOGW(TAG, "===============================================");
}

static bool skip_memo_call = false;

void sys_memory_status(const char *tag, const char *msg)
{
    static size_t last_free_heap = 0;
    static size_t last_dram_used = 0;
    static bool first_call = true;
    static bool in_progress = false;

    // Use yellow for periodic calls, white for manual calls
    bool is_periodic = (strcmp(tag, "MEMORY_PERIODIC") == 0);
    if (is_periodic) {
        ESP_LOGW(tag, "[Memory Test] %s", msg);  // Yellow
    } else {
        ESP_LOGI(tag, "[Memory Test] %s", msg);  // White
    }

    // Avoid recursion
    if (in_progress || !memory_logs_track_f) {
        return;
    }

    // If this is a manual call (not from periodic task), pause periodic for a bit
    if (strcmp(tag, "MEMORY_PERIODIC") != 0) {
        skip_memo_call = true;
    }

    in_progress = true;

    size_t current_free = esp_get_free_heap_size();
    size_t dram_used = heap_caps_get_total_size(MALLOC_CAP_INTERNAL) -
                      heap_caps_get_free_size(MALLOC_CAP_INTERNAL);

    last_free_heap = current_free;
    last_dram_used = dram_used;

    memory_status();

    in_progress = false;
    vTaskDelay(pdMS_TO_TICKS(100));

    // Reset skip flag after delay if this was a manual call
    if (strcmp(tag, "MEMORY_PERIODIC") != 0) {
        skip_memo_call = false;
    }
}

bool skip_memory_check(void)
{
    return skip_memo_call;
}

bool get_memory_logs_status(void)
{
    return memory_logs_track_f;
}
void toogle_memory_logs_flag(void)
{
    memory_logs_track_f ^= 1;
}
// Message handler bit manipulation functions - END

// SD card DMA mutex - for coordinating DMA operations between SDMMC and BLE
SemaphoreHandle_t g_sdcard_dma_mutex = NULL;

void init_sdcard_dma_mutex(void) {
    if (!g_sdcard_dma_mutex) {
        g_sdcard_dma_mutex = xSemaphoreCreateMutex();
        if (g_sdcard_dma_mutex) {
            ESP_LOGI(TAG, "SD card DMA mutex created for BLE/SDMMC coordination");
        } else {
            ESP_LOGE(TAG, "Failed to create SD card DMA mutex");
        }
    }
}

void deinit_sdcard_dma_mutex(void) {
    if (g_sdcard_dma_mutex) {
        vSemaphoreDelete(g_sdcard_dma_mutex);
        g_sdcard_dma_mutex = NULL;
        ESP_LOGI(TAG, "SD card DMA mutex deleted");
    }
}

char *read_file_to_spiram(const char *filename) {
    if (!filename) {
        ESP_LOGE(TAG, "Invalid file name.");
        return NULL;
    }

    struct stat st;
    if (stat(filename, &st) != 0 || st.st_size <= 0) {
        ESP_LOGE(TAG, "Empty file or invalid size");
        return NULL;
    }

    long filesize = st.st_size;

    FILE *f = s3_fopen(filename, "rb");
    if (!f) {
        ESP_LOGE(TAG, "Fail on openning file to read");
        return NULL;
    }

    char *buffer = heap_caps_malloc(filesize + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!buffer) {
        ESP_LOGE(TAG, "Error on allocating memory to read the file");
        s3_fclose(f);
        return NULL;
    }

    size_t read_size = s3_fread(buffer, 1, filesize, f);
    s3_fclose(f);

    buffer[read_size] = '\0';
    return buffer;
}

char *strdup_spiram(const char *str) {
    if (!str)
        return NULL;
    char *new_str = heap_caps_malloc(strlen(str) + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (new_str) {
        strcpy(new_str, str);
    }
    return new_str;
}
