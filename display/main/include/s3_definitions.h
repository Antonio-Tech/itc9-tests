//
// Created by Antonio_Pereira on 2025/5/20.
//

#ifndef S3_DEFINITIONS_H
#define S3_DEFINITIONS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

// RESOURCE DEFINES - START

#define LOG_DEFAULT "/sdcard/esp32.log"

// Content type enumeration for LVGL decoder system
typedef enum {
    CONTENT_TYPE_COVER,        // Album covers (home_cover, play_cover)
    CONTENT_TYPE_LANGUAGE_BADGE,
    CONTENT_TYPE_BATTERY_BADGE,
    CONTENT_TYPE_PLAYER_BADGE,
    CONTENT_TYPE_BT_BADGE,     // Bluetooth badge (future use)
    CONTENT_TYPE_POPUP,        // Popup content that shares screen
    CONTENT_TYPE_MENU,         // Menu icons
    CONTENT_TYPE_MAX           // Keep this last
} content_type_t;

typedef enum {
    EVENT_BTN_A_SHORT = 0,
    EVENT_BTN_B_SHORT,
    EVENT_BTN_C_SHORT,
    EVENT_BTN_D_SHORT,
    EVENT_BTN_E_SHORT,
    EVENT_BTN_A_LONG,
    EVENT_BTN_B_LONG,
    EVENT_BTN_C_LONG,
    EVENT_BTN_D_LONG,
    EVENT_BTN_E_LONG,
    EVENT_BTN_A_CONTINUOUS,
    EVENT_BTN_B_CONTINUOUS,
    EVENT_BTN_MACRO_B_N_C_LONG,
    EVENT_BTN_MACRO_A_N_D_LONG,
    EVENT_BTN_MACRO_A_N_B_LONG,
    EVENT_TIMEOUT_SHORT,
    EVENT_TIMEOUT_LONG,
    EVENT_NFC_DETECTED,
    EVENT_ALARM_TRIGGERED,
    EVENT_ALARM_AUTO_DISMISS,
    EVENT_LEAVE_PLAYING_TO_HOME,
    EVENT_ENTER_STANDBY,
    EVENT_LEAVE_STANDBY,
    EVENT_ENTER_POWER_OFF,
    EVENT_LEAVE_POWER_OFF,
    // EVENT_BT_PAUSE removed - no longer needed (BT status callback handles screen transition)
} AppEvent;

// Content type names for logging
extern const char *content_type_names[];

// Custom colors - START
#define LV_CUSTOM_BLACK     0x000000
#define LV_CUSTOM_WHITE     0xFFFFFF
#define LV_CUSTOM_GRAY      0x545455
#define LV_CUSTOM_YELLOW    0xE9E65C
#define LV_CUSTOM_BLUE      0xCC0000
#define LV_CUSTOM_GREEN     0x5CB85C
#define LV_CUSTOM_PINK      0xEC619D
#define LV_CUSTOM_CYAN      0x19B8D5
#define LV_CUSTOM_ORANGE    0xF16723

// Custom colors - END

// Provisory resources - START
#define DEFAULT_ALBUM_CONTENT_PATH  "/sdcard/tmp/default_albums.json"
#define IMAGE_OP2       "/sdcard/spiffs/images/album_cover_1.jpg"
#define IMAGE_OP1       "/sdcard/spiffs/images/album_cover_2.jpg"

#define GIF_OP1         "/sdcard/animation_gif/wifi/kid_sync/kid1_sync.gif"
#define BOOT_ANIMATION  "/sdcard/spiffs/animations/bulb_lamp.gif"
#define ALARM_ANIMATION "/sdcard/spiffs/animations/small_duck.gif"

#define LOTTIE_OP1      "/sdcard/spiffs/animations/charging.json"

#define BOOT_SOUND      "/sdcard/sound/PIX-WE-01-Power_on.mp3"
#define SHUTDOWN_SOUND  "/sdcard/sound/PIX-WE-02-Power_off.mp3"
#define VOLUME_SOUND    "/sdcard/sound/PIX-WE-03-Volume.mp3"
#define SAMPLE_SOUND    "/sdcard/sound/sample.mp3"
// Provisory resources - END

// GIF animations - START
#define ANIM_BATT_CHRG  "/sdcard/animation_gif/battery/Charging.gif"
#define ANIM_BATT_FULL  "/sdcard/animation_gif/battery/Charging_100.gif"
#define ANIM_BATT_LOW   "/sdcard/animation_gif/battery/Battery_low.gif"
#define ANIM_BATT_LOW_10 "/sdcard/animation_gif/battery/Battery_low_10.gif"
#define ANIM_BATT_LOW_5  "/sdcard/animation_gif/battery/Battery_low_5.gif"
#define ANIM_BATT_LOW_1  "/sdcard/animation_gif/battery/Battery_low_1.gif"

#define ANIM_BRIGHT_1   "/sdcard/animation_gif/brightness/brightness1.gif"
#define ANIM_BRIGHT_2   "/sdcard/animation_gif/brightness/brightness2.gif"
#define ANIM_BRIGHT_3   "/sdcard/animation_gif/brightness/brightness3.gif"

#define ANIM_BT_ANIM    "/sdcard/animation_gif/bt/BT_searching.gif"

#define ANIM_FW_ANIM    "/sdcard/animation_gif/firmeware/firmware.gif"

#define ANIM_POWER_ON   "/sdcard/animation_gif/power/power_on.gif"
#define ANIM_POWER_OFF  "/sdcard/animation_gif/power/power_off.gif"

#define ANIM_VOLUME_1   "/sdcard/animation_gif/volume/volume1.gif"
#define ANIM_VOLUME_2   "/sdcard/animation_gif/volume/volume2.gif"
#define ANIM_VOLUME_3   "/sdcard/animation_gif/volume/volume3.gif"
#define ANIM_VOLUME_4   "/sdcard/animation_gif/volume/volume4.gif"
#define ANIM_VOLUME_5   "/sdcard/animation_gif/volume/volume5.gif"
#define ANIM_VOLUME_6   "/sdcard/animation_gif/volume/volume6.gif"

#define ANIM_WIFI_AP    "/sdcard/animation_gif/wifi/WiFi_ap.gif"
#define ANIM_WIFI_SECH  "/sdcard/animation_gif/wifi/WiFi_search.gif"
#define ANIM_WIFI_SYNC  "/sdcard/animation_gif/wifi/WiFi_sync_up.gif"

#define ANIM_KID_0      "/sdcard/animation_gif/wifi/kid_sync/kid0.gif"
#define ANIM_KID_1      "/sdcard/animation_gif/wifi/kid_sync/kid1.gif"
#define ANIM_KID_2      "/sdcard/animation_gif/wifi/kid_sync/kid2.gif"
#define ANIM_KID_3      "/sdcard/animation_gif/wifi/kid_sync/kid3.gif"
#define ANIM_KID_4      "/sdcard/animation_gif/wifi/kid_sync/kid4.gif"
#define ANIM_KID_5      "/sdcard/animation_gif/wifi/kid_sync/kid5.gif"

// GIF animations - END

// JPG placeholders - START
#define ICON_POWER_ON   "/sdcard/animation_jpg/power/power_on.jpg"
#define ICON_POWER_OFF  "/sdcard/animation_jpg/power/power_off.jpg"
#define ICON_POWER_FAC  "/sdcard/animation_jpg/power/Reset.jpg"
#define ICON_POWER_FAC_ACC_INV  "/sdcard/animation_jpg/power/Account_Removed.jpg"

#define ICON_VOLUME     "/sdcard/animation_jpg/volume/volume.jpg"  // main screen
#define ICON_VOLUME_1   "/sdcard/animation_jpg/volume/volume1.jpg"
#define ICON_VOLUME_2   "/sdcard/animation_jpg/volume/volume2.jpg"
#define ICON_VOLUME_3   "/sdcard/animation_jpg/volume/volume3.jpg"
#define ICON_VOLUME_4   "/sdcard/animation_jpg/volume/volume4.jpg"
#define ICON_VOLUME_5   "/sdcard/animation_jpg/volume/volume5.jpg"
#define ICON_VOLUME_6   "/sdcard/animation_jpg/volume/volume6.jpg"

#define ICON_BATT_CHRG  "/sdcard/animation_jpg/battery/charging.jpg"
#define ICON_BATT_FULL  "/sdcard/animation_jpg/battery/charging_100.jpg"
#define ICON_BATT_LOW   "/sdcard/animation_jpg/battery/battery_low.jpg"
#define ICON_BATT_LOW_10 "/sdcard/animation_jpg/battery/battery_low_10.jpg"
#define ICON_BATT_LOW_5  "/sdcard/animation_jpg/battery/battery_low_5.jpg"
#define ICON_BATT_LOW_1  "/sdcard/animation_jpg/battery/battery_low_1.jpg"

#define ICON_BATT_0_JPG "/sdcard/animation_jpg/battery/battery_charging_6.jpg"
#define ICON_BATT_1_JPG "/sdcard/animation_jpg/battery/battery_charging_5.jpg"
#define ICON_BATT_2_JPG "/sdcard/animation_jpg/battery/battery_charging_4.jpg"
#define ICON_BATT_3_JPG "/sdcard/animation_jpg/battery/battery_charging_3.jpg"
#define ICON_BATT_4_JPG "/sdcard/animation_jpg/battery/battery_charging_2.jpg"
#define ICON_BATT_5_JPG "/sdcard/animation_jpg/battery/battery_charging_1.jpg"
#define ICON_BATT_6_JPG "/sdcard/animation_jpg/battery/battery_charging_0.jpg"

#define ICON_BATT_0_PNG "/sdcard/animation_png/battery/battery_charging_6_0_5.png"
#define ICON_BATT_1_PNG "/sdcard/animation_png/battery/battery_charging_5_6_21.png"
#define ICON_BATT_2_PNG "/sdcard/animation_png/battery/battery_charging_4_21_40.png"
#define ICON_BATT_3_PNG "/sdcard/animation_png/battery/battery_charging_3_41_60.png"
#define ICON_BATT_4_PNG "/sdcard/animation_png/battery/battery_charging_2_61_80.png"
#define ICON_BATT_5_PNG "/sdcard/animation_png/battery/battery_charging_1_81_99.png"
#define ICON_BATT_6_PNG "/sdcard/animation_png/battery/battery_charging_0_100.png"

#define ICON_BATT_CHARGING_PNG "/sdcard/animation_png/battery/battery_charging.png"

#define ICON_BATT_NORMAL_0_PNG "/sdcard/animation_png/battery/battery_6_0_5.png"
#define ICON_BATT_NORMAL_1_PNG "/sdcard/animation_png/battery/battery_5_6_21.png"
#define ICON_BATT_NORMAL_2_PNG "/sdcard/animation_png/battery/battery_4_21_40.png"
#define ICON_BATT_NORMAL_3_PNG "/sdcard/animation_png/battery/battery_3_41_60.png"
#define ICON_BATT_NORMAL_4_PNG "/sdcard/animation_png/battery/battery_2_61_80.png"
#define ICON_BATT_NORMAL_5_PNG "/sdcard/animation_png/battery/battery_1_81_99.png"
#define ICON_BATT_NORMAL_6_PNG "/sdcard/animation_png/battery/battery_0_100.png"

#define ICON_BIRGHTNESS "/sdcard/animation_jpg/brightness/brightness.jpg"
#define ICON_BRIGHT_1   "/sdcard/animation_jpg/brightness/brightness1.jpg"
#define ICON_BRIGHT_2   "/sdcard/animation_jpg/brightness/brightness2.jpg"
#define ICON_BRIGHT_3   "/sdcard/animation_jpg/brightness/brightness3.jpg"

#define ICON_WIFI_AP    "/sdcard/animation_jpg/wifi/wifi_ap.jpg"
#define ICON_WIFI_SECH  "/sdcard/animation_jpg/wifi/wifi_search.jpg"
#define ICON_WIFI_SYNC  "/sdcard/animation_jpg/wifi/wifi_syncup.jpg"
#define ICON_WIFI_UNKW  "/sdcard/animation_jpg/wifi/wifi_unknown.jpg"
#define ICON_WIFI_DISC  "/sdcard/animation_jpg/wifi/wifi_disconnect.jpg"
#define ICON_WIFI_SYNCED  "/sdcard/animation_jpg/wifi/wifi_synced.jpg"

#define ICON_WIFI_SYNC_MAI  "/sdcard/animation_jpg/wifi/data_sync.jpg"
#define ICON_WIFI_SYNC_ERR  "/sdcard/animation_jpg/wifi/data_sync_error.jpg"
#define ICON_WIFI_SYNC_SUC  "/sdcard/animation_jpg/wifi/data_sync_successfull.jpg"
#define ICON_WIFI_SYNC0  "/sdcard/animation_jpg/wifi/data_sync0.jpg"
#define ICON_WIFI_SYNC1  "/sdcard/animation_jpg/wifi/data_sync1.jpg"
#define ICON_WIFI_SYNC2  "/sdcard/animation_jpg/wifi/data_sync2.jpg"
#define ICON_WIFI_SYNC3  "/sdcard/animation_jpg/wifi/data_sync3.jpg"
#define ICON_WIFI_SYNC_WAIT  "/sdcard/animation_jpg/wifi/data_sync_wait.jpg"
#define ICON_WIFI_ERROR  "/sdcard/animation_jpg/wifi/wifi_error.jpg"

#define ICON_KID_SYNC_N "/sdcard/animation_jpg/kid_sync/%s.jpg"
#define ICON_KID_SYNCED_N "/sdcard/animation_jpg/kid_sync/%s_synced.jpg"
#define ICON_KID_N      "/sdcard/animation_jpg/kids/%s.jpg"

#define ICON_BT         "/sdcard/animation_jpg/bt/bt.jpg"
#define ICON_BT_CONNECT "/sdcard/animation_jpg/bt/bt_connected.jpg"
#define ICON_BT_SEARCH  "/sdcard/animation_jpg/bt/bt_searching.jpg"
// #define ICON_BT_HEAD_CN "/sdcard/animation_jpg/bt/bt_headset_connected.jpg"
// #define ICON_BT_SEPK_CN "/sdcard/animation_jpg/bt/bt_speaker_connected.jpg"
// #define ICON_BT_TIMEOUT "/sdcard/animation_jpg/bt/bt_timeout.jpg"

#define ICON_NFC        "/sdcard/animation_jpg/nfc/nfc.jpg"
#define ICON_NFC_EN     "/sdcard/animation_jpg/nfc/nfc_en.jpg"
#define ICON_NFC_CH     "/sdcard/animation_jpg/nfc/nfc_ch.jpg"
#define ICON_NFC_ALL    "/sdcard/animation_jpg/nfc/nfc_playall.jpg"
#define ICON_NFC_ACTIVATE "/sdcard/animation_jpg/nfc/nfc_go_to_activate.jpg"
#define ICON_NFC_NO_CONTENT "/sdcard/animation_jpg/nfc/nfc_no_content.jpg"

#define ICON_NFC_GO_ACT "/sdcard/animation_jpg/nfc/nfc_go_to_activate.jpg"
#define ICON_NFC_NO_DOW "/sdcard/animation_jpg/nfc/NFC_not_fully_download.jpg"
#define ICON_NFC_NO_CON "/sdcard/animation_jpg/nfc/nfc_no_content.jpg"
#define ICON_NFC_OTHER  "/sdcard/animation_jpg/nfc/nfc_not_under_account.jpg"

#define ICON_MINI_EN    "/sdcard/animation_png/nfc/nfc_en.png"
#define ICON_MINI_CH    "/sdcard/animation_png/nfc/nfc_ch.png"
#define ICON_MINI_ALL   "/sdcard/animation_png/nfc/nfc_en.png"

#define ICON_MINI_CONN  "/sdcard/animation_png/bt/bt_connected.png"
#define ICON_MINI_TIME  "/sdcard/animation_png/bt/bt_timeout.png"

#define ICON_FIRMWARE   "/sdcard/animation_jpg/firmware/firemware_update.jpg"
#define ICON_PLUG_IN   	"/sdcard/animation_jpg/firmware/plug_in.jpg"

#define ICON_PLAYER_PAUSED  "/sdcard/animation_jpg/player/paused.jpg"
// JPG placeholders - END

// PNG placeholders - START
#define ICON_PLAYER_PAUSE   "/sdcard/animation_png/player/s_3_icon_011_pause.png"
// PNG placeholders - END

// ALBUM: animations - START 
#define ANIM_SKU_007        "/sdcard/animation_gif/album_cover/SKU-00007.gif"
#define ANIM_SKU_008        "/sdcard/animation_gif/album_cover/SKU-00008.gif"
#define ANIM_SKU_009        "/sdcard/animation_gif/album_cover/SKU-00009.gif"
#define ANIM_SKU_010        "/sdcard/animation_gif/album_cover/SKU-00010.gif"
#define ANIM_SKU_013        "/sdcard/animation_gif/album_cover/SKU-00013.gif"
#define ANIM_SKU_014        "/sdcard/animation_gif/album_cover/SKU-00014.gif"
#define ANIM_SKU_019        "/sdcard/animation_gif/album_cover/SKU-00019.gif"
#define ANIM_SKU_020        "/sdcard/animation_gif/album_cover/SKU-00020.gif"
#define ANIM_SKU_021        "/sdcard/animation_gif/album_cover/SKU-00021.gif"
#define ANIM_SKU_022        "/sdcard/animation_gif/album_cover/SKU-00022.gif"
#define ANIM_SKU_023        "/sdcard/animation_gif/album_cover/SKU-00023.gif"
#define ANIM_SKU_024        "/sdcard/animation_gif/album_cover/SKU-00024.gif"
#define ANIM_SKU_025        "/sdcard/animation_gif/album_cover/SKU-00025.gif"
#define ANIM_SKU_027        "/sdcard/animation_gif/album_cover/SKU-00027.gif"
#define ANIM_SKU_999        "/sdcard/animation_gif/album_cover/SKU-00027.gif"
#define ANIM_SKU_UKW        ""          // Unknown animation for custom albums
// ALBUM: animations - END

// ALBUM: covers - START
// #define IMG_SKU_007     "/sdcard/animation_gif/album_cover/SKU-00007.png"
// #define IMG_SKU_008     "/sdcard/animation_gif/album_cover/SKU-00008.png"
// #define IMG_SKU_009     "/sdcard/animation_gif/album_cover/SKU-00009.png"
// #define IMG_SKU_010     "/sdcard/animation_gif/album_cover/SKU-00010.png"
// #define IMG_SKU_013     "/sdcard/animation_gif/album_cover/SKU-00013.png"
// #define IMG_SKU_014     "/sdcard/animation_gif/album_cover/SKU-00014.png"
// #define IMG_SKU_023     "/sdcard/animation_gif/album_cover/SKU-00023.png"
// #define IMG_SKU_024     "/sdcard/animation_gif/album_cover/SKU-00024.png"

#define IMG_PLAY_SKU_007     "/sdcard/cover/device/SKU-00007_D.jpg"
#define IMG_PLAY_SKU_008     "/sdcard/cover/device/SKU-00008_D.jpg"
#define IMG_PLAY_SKU_009     "/sdcard/cover/device/SKU-00009_D.jpg"
#define IMG_PLAY_SKU_010     "/sdcard/cover/device/SKU-00010_D.jpg"
#define IMG_PLAY_SKU_013     "/sdcard/cover/device/SKU-00013_D.jpg"
#define IMG_PLAY_SKU_014     "/sdcard/cover/device/SKU-00014_D.jpg"
#define IMG_PLAY_SKU_019     "/sdcard/cover/device/SKU-00019_D.jpg"
#define IMG_PLAY_SKU_020     "/sdcard/cover/device/SKU-00020_D.jpg"
#define IMG_PLAY_SKU_021     "/sdcard/cover/device/SKU-00021_D.jpg"
#define IMG_PLAY_SKU_022     "/sdcard/cover/device/SKU-00022_D.jpg"
#define IMG_PLAY_SKU_023     "/sdcard/cover/device/SKU-00023_D.jpg"
#define IMG_PLAY_SKU_025     "/sdcard/cover/device/SKU-00025_D.jpg"
#define IMG_PLAY_SKU_027     "/sdcard/cover/device/SKU-00027_D.jpg"
#define IMG_PLAY_SKU_999     "/sdcard/cover/device/SKU-00027_D.jpg"
#define IMG_PLAY_SKU_UKW     ""          // Unknown play cover for custom albums

#define IMG_HOME_SKU_007     "/sdcard/cover/device/SKU-00007.jpg"
#define IMG_HOME_SKU_008     "/sdcard/cover/device/SKU-00008.jpg"
#define IMG_HOME_SKU_009     "/sdcard/cover/device/SKU-00009.jpg"
#define IMG_HOME_SKU_010     "/sdcard/cover/device/SKU-00010.jpg"
#define IMG_HOME_SKU_013     "/sdcard/cover/device/SKU-00013.jpg"
#define IMG_HOME_SKU_014     "/sdcard/cover/device/SKU-00014.jpg"
#define IMG_HOME_SKU_019     "/sdcard/cover/device/SKU-00019.jpg"
#define IMG_HOME_SKU_020     "/sdcard/cover/device/SKU-00020.jpg"
#define IMG_HOME_SKU_021     "/sdcard/cover/device/SKU-00021.jpg"
#define IMG_HOME_SKU_022     "/sdcard/cover/device/SKU-00022.jpg"
#define IMG_HOME_SKU_023     "/sdcard/cover/device/SKU-00023.jpg"
#define IMG_HOME_SKU_025     "/sdcard/cover/device/SKU-00025.jpg"
#define IMG_HOME_SKU_027     "/sdcard/cover/device/SKU-00027.jpg"
#define IMG_HOME_SKU_999     "/sdcard/cover/device/SKU-00027.jpg"
#define IMG_HOME_SKU_UKW     ""          // Unknown home cover for custom albums

// ALBUM: covers - END

// ALBUM: paths - START
#define PATH_SKU_007        "/sdcard/content/full/SKU-00007/"
#define PATH_SKU_008        "/sdcard/content/full/SKU-00008/"
#define PATH_SKU_009        "/sdcard/content/full/SKU-00009/"
#define PATH_SKU_010        "/sdcard/content/full/SKU-00010/"
#define PATH_SKU_013        "/sdcard/content/full/SKU-00013/"
#define PATH_SKU_014        "/sdcard/content/full/SKU-00014/"
#define PATH_SKU_019        "/sdcard/content/full/SKU-00019/"
#define PATH_SKU_020        "/sdcard/content/full/SKU-00020/"
#define PATH_SKU_021        "/sdcard/content/full/SKU-00021/"
#define PATH_SKU_022        "/sdcard/content/full/SKU-00022/"
#define PATH_SKU_023        "/sdcard/content/full/SKU-00023/"
#define PATH_SKU_025        "/sdcard/content/full/SKU-00025/"
#define PATH_SKU_027        "/sdcard/content/full/SKU-00027/"
#define PATH_SKU_999        "/sdcard/content/full/SKU-00999/"
#define PATH_SKU_UKW        ""          // Unknown path for custom albums
// ALBUM: paths - END

// ALARM: animation - START
#define ANIM_ALARM_1        "/sdcard/animation_gif/alarms/morning.gif"
#define ANIM_ALARM_2        "/sdcard/animation_gif/alarms/yummy.gif"
#define ANIM_ALARM_3        "/sdcard/animation_gif/alarms/move.gif"
#define ANIM_ALARM_4        "/sdcard/animation_gif/alarms/Ready.gif"
#define ANIM_ALARM_5        "/sdcard/animation_gif/alarms/clean.gif"
#define ANIM_ALARM_6        "/sdcard/animation_gif/alarms/nightnight.gif"
#define ANIM_ALARM_7        "/sdcard/animation_gif/alarms/naptime.gif"
// ALARM: animation - END

// ALARM: icon - START
#define ICON_ALARM_1        "/sdcard/animation_jpg/alarms/morning.jpg"
#define ICON_ALARM_2        "/sdcard/animation_jpg/alarms/yummy.jpg"
#define ICON_ALARM_3        "/sdcard/animation_jpg/alarms/move.jpg"
#define ICON_ALARM_4        "/sdcard/animation_jpg/alarms/read.jpg"
#define ICON_ALARM_5        "/sdcard/animation_jpg/alarms/clean.jpg"
#define ICON_ALARM_6        "/sdcard/animation_jpg/alarms/nightnight.jpg"
#define ICON_ALARM_7        "/sdcard/animation_jpg/alarms/naptime.jpg"
// ALARM: icon - END

// ALARM: audio - START
#define AUDIO_ALARM_1       "/sdcard/sound/PIX-WA-01-Moring_sunshine.mp3"
#define AUDIO_ALARM_2       "/sdcard/sound/PIX-WA-02-Yummy_time.mp3"
#define AUDIO_ALARM_3       "/sdcard/sound/PIX-WA-03-Lets_move_about.mp3"
#define AUDIO_ALARM_4       "/sdcard/sound/PIX-WA-04-Reading_time.mp3"
#define AUDIO_ALARM_5       "/sdcard/sound/PIX-WA-05-Squeaky_clean_fun.mp3"
#define AUDIO_ALARM_6       "/sdcard/sound/PIX-WA-06-Night_night.mp3"
#define AUDIO_ALARM_7       "/sdcard/sound/PIX-WA-07-Naptime.mp3"
// ALARM: audio - END

// ALBUM: content - START
#define COUNT_SKU_007       5
#define COUNT_SKU_008       5
#define COUNT_SKU_009       3
#define COUNT_SKU_010       3
#define COUNT_SKU_013       3
#define COUNT_SKU_014       3
#define COUNT_SKU_019       5
#define COUNT_SKU_020       5
#define COUNT_SKU_021       3
#define COUNT_SKU_022       3
#define COUNT_SKU_023       3
#define COUNT_SKU_025       4
#define COUNT_SKU_027       5
#define COUNT_SKU_999       -1
#define COUNT_SKU_UKW       0           // Unknown count for custom albums

#define IS_DOWNLOADED       false
#define IS_PLAY_ENABLED     false
#define IS_NFC_ENABLED      false
// ALBUM: content - END

// ALBUM: Flag variables moved to s3_definitions.c to avoid unused variable warnings
// These are referenced by pointers in the s3_albums[] array

// Environment variables - START
#define NO_RESOURCE         NULL
#define PARSE_AND_DOWNLOAD  0   // Parse JSON and download files
#define PARSE_ONLY          1   // Parse JSON only, skip downloads

// Sync mode constants for unified sync task
#define SYNC_MODE_FULL      0   // Full WiFi sync: SNTP, resource updates, OTA, OOB binding, device info upload, albums, pictures, alarms
#define SYNC_MODE_NFC       1   // NFC sync mode: albums, pictures, alarms only (no SNTP, OTA, OOB binding, device info upload)
#define SYNC_MODE_BLE       2   // BLE sync mode: same as FULL but returns to HOME_SCREEN after sync

#define USE_CARROUCEL       true
#define NO_CARROUCEL        false

#define USE_TRANSPARENCY    true
#define NO_TRANSPARENCY     false

#define USE_ANIM_GIF        0
#define USE_ANIM_LVGL       1
#define USE_ANIM_JPG        2
#define USE_ANIM_PNG        3

#define VALUE_ON            true
#define VALUE_OFF           false

#define VALUE_UP            true
#define VALUE_DOWN          false

/* BLE GATT dev_ctrl update macros */
#define NO_UPDATE           -1
#define BLE_MSG_ERROR       0xFF

#define VOLUME_LEVEL_1      1
#define VOLUME_LEVEL_2      2
#define VOLUME_LEVEL_3      3
#define VOLUME_LEVEL_4      4
#define VOLUME_LEVEL_5      5
#define VOLUME_LEVEL_6      6

#define BRIGHTNESS_LEVEL_1  0
#define BRIGHTNESS_LEVEL_2  1
#define BRIGHTNESS_LEVEL_3  2

#define KID_AVATAR_0  0
#define KID_AVATAR_1  1
#define KID_AVATAR_2  2
#define KID_AVATAR_3  3
#define KID_AVATAR_4  4
#define KID_AVATAR_5  5

#define BATTERY_UNREAD      -1
#define BATTERY_CHARGE      0
#define BATTERY_DISCHARGE   1
#define BATTERY_CHARGE_FULL 2

#define LOW_BATT_POLL_FREQ  2
#define NORMAL_BATT_POLL_FREQ 10
#define LOW_BATT_THRESHOLD 20  // Percentage threshold for fast sampling mode

#define BATTERY_LEVEL_0     0
#define BATTERY_LEVEL_1     1
#define BATTERY_LEVEL_2     2
#define BATTERY_LEVEL_3     3
#define BATTERY_LEVEL_4     4
#define BATTERY_LEVEL_5     5
#define BATTERY_LEVEL_6     6

#define BATTERY_SMALL       false
#define BATTERY_LARGE       true

#define LANGUAGE_QTD            3
#define NO_LANGUAGE             -1
#define LANGUAGE_ENGLISH        0
#define LANGUAGE_CHINESE        1
#define LANGUAGE_ALL            2

// NFC Content Types - for lv_nfc_content_screen
#define NFC_CONT_DEFAULT        0   // Not used now
#define NFC_CONT_UPDATING       1   // Show sync data icon
#define NFC_CONT_GO_ACTIVE      2   // NFC tag is not activated, but can be activated
#define NFC_CONT_NO_CONTENT     3   // Blankee NFC has no content
#define NFC_CONT_NOT_DOWNLOADED 4   // Blankee NFC is not downloaded


// NFC System Constants
#define NFC_UID_LEN             7   // NFC tag UID length in bytes
#define NFC_QUEUE_SIZE          1   // NFC event queue size

// NFC Sync Context Types - for start_nfc_sync_task
#define NFC_SYNC_CONTEXT_CONTENT_UPDATE    0  // Regular content download (album exists but needs download)
#define NFC_SYNC_CONTEXT_ACTIVATION_CHECK  1  // Check if user activated/added content (tag not found or no content)

#define LANGUAGE_MINI           40
#define ICON_1MUL               256
#define ICON_2MUL               (ICON_1MUL * 2)
#define ICON_2DIV               (ICON_1MUL / 2)
#define ICON_4DIV               (ICON_1MUL / 4)
#define ICON_20DIV              (ICON_1MUL / 20)
#define LANGUAGE_ZOOM           ((LANGUAGE_MINI * 256) / 240)
#define HOME_ZOOM               (((240 * 256) + 999) / 2000) 

#define OOB_FACTORY_RESET       0 // Out-of-box status indicating factory reset
#define OOB_NORMAL              1 // Out-of-box status indicating non-factory reset

#define MAX_DOTS                10

#define BIND_MSG_NONE           "No results"
#define BIND_MSG_SUCCESS        "Success on cloud binding"
#define BIND_MSG_FAIL           "Fail on cloud binding"
#define BIND_MSG_CREDENTIAL     "Fail to access credentials file"
#define BIND_MSG_WIFI           "Fail to connect to Wi-Fi"
#define BIND_MSG_SECRET_KEY     "Fail to retrieve secret key"

// WiFi connection macros
#define WIFI_NVS_CREDENTIAL     0, NULL  // Connect using stored NVS credentials

#define BT_UNPAIRED             1   // Initial state,                                   remove the mini icon badge ( )
#define BT_SCAN                 2   // Does not mean paired, just scanning for devices, remove the mini icon badge ( )
#define BT_PAIRED               3   // Paired with a device,                            show the mini icon badge (B)
#define BT_TIMEOUT              4   // Timeout state,                                   show the mini icon badge (Bx)
#define BT_FAILED               5   // Connection failed state,                         show the mini icon badge (Bx)

#define QRCODE_CONTENT_LEN      64
#define BINDING_MSG_LEN         64

#define DUMMY_READY             0
#define DUMMY_FINISH            1

#define BASE_ANCHOR             0
#define BASE_ANIMATION          1
#define BASE_EXCEPTION          2

#define IS_ALBUM_CONTENT        false
#define IS_NFC_CONTENT          true

#define COUNTDOWN_START_VALUE   5

#define USE_ALBUM               true
#define USE_MENU                false
#define LANG_HOME               true
#define LANG_PLAYER             false
typedef enum {
    IDX_VOLUME = 0,
    IDX_WIFI,
    // IDX_BLUETOOTH,  // Hidden - removed from menu
    IDX_BRIGHTNESS,
    IDX_NFC,
    IDX_MENU_SIZE
} eS3_MENU_IDX;


#define INSTANT_TRANSITION      0
#define DEFAULT_TRANSITION      1000
#define BOOT_TRANSITION         5000
#define SHUTDOWN_TRANSITION     3000    // 2000
#define VOLUME_TRANSITION       1400    // 2000
#define ALARM_TRANSITION        5000
#define BRIGHT_TRANSITION       1400    // 2000
#define PWR_POP_TRANSITION      3000    // 2000
#define WIFI_SYNCED_TRANSITION  3000
#define PAIRING_TRANSITION      1000
#define GENERIC_TRANSITION      1000
#define POWER_ON_KID_TRANSITION 3000
#define NFC_WIFI_DSC_TRANSITION 3000
#define NFC_TIMEOUT_TRANSITION  3000
#define COUNTDOWN_TRANSITION    1400
#define BACK_TO_HOME_TRANSITION 4000    // 3 seconds
#define NO_TRANSITION       -1

#define VOLUME_TIMER_MS 1500  // 3 seconds

#define ALARM_LIST_SIZE     5
#define BUTTON_PRESS_TIME_MS               1500


#define ALARM_LIST_SIZE     5
// Environment variables - END
// REOURCES DEFINES - END

// BLE COMMANDS - START
/* Control commands for dev_ctrl mode */
#define BLE_CMD_START_BINDING       0x01
#define BLE_CMD_START_FULL_SYNC     0x02
#define BLE_CMD_START_CONTENT_SYNC  0x03
#define BLE_CMD_CHECK_CONNECTION    0x04
#define BLE_CMD_ENABLE_MSG          0x08  // Enable dev_msg mode for WiFi config
#define BLE_CMD_DISABLE_MSG         0x10
#define BLE_CMD_STATUS_REQ          0x20  // Request device status update
#define BLE_CMD_SYNC_STATUS_REQ     0x40  // Request data sync status (OTA/syncing/completed)
// BLE COMMANDS - END

// ALBUM: definition - START
#define ALBUM_DEFAULT   0
#define ALBUM_SKU       1
#define ALBUM_SKURC     2
#define ALBUM_ISR       3

// Album persistence defines
#define S3_LAST_ALBUM_SKU_LENGTH 32  // Max SKU length for last played album file persistence

// SKU string constants for easy reference
#define ALL_SKU     "all"
#define NO_SKU      -1
#define NO_ALBUM_FOUND      -1  // Album not found indicator
#define SKU_DEFAULT "none"  // Default/fallback SKU when no valid SKU is found
#define SKU_007     "SKU-00007"
#define SKU_008     "SKU-00008"
#define SKU_009     "SKU-00009"
#define SKU_010     "SKU-00010"
#define SKU_013     "SKU-00013"
#define SKU_014     "SKU-00014"
#define SKU_019     "SKU-00019"
#define SKU_020     "SKU-00020"
#define SKU_021     "SKU-00021"
#define SKU_022     "SKU-00022"
#define SKU_023     "SKU-00023"
#define SKU_025     "SKU-00025"
#define SKU_027     "SKU-00027"
#define SKURC_1     "SKURC-001"
#define SKURC_2     "SKURC-002"
#define SKU_ISR     "SKU-ISR"
#define SKU_TST     "SKU-00999"
#define SKU_UKW     ""          // Unknown SKU for custom albums
#define SKU_LEN     22  // Match sku[22] array size in s3_album_handler_t struct

// Availability defaults for album structure initialization
#define IS_DOWNLOADED       false
#define IS_PLAY_ENABLED     false
#define IS_NFC_ENABLED      false
#define ALBUM_DEFAULT       0
// Availability control - albums are now filtered by is_available flag instead of language
// Only albums with is_available=true will appear in playlists
// Language field is still maintained for individual album metadata

// Simple integer defines for special album identification if needed
#define DEFAULT_ALBUM_CH    1   // Default Chinese album ID
#define DEFAULT_ALBUM_EN    2   // Default English album ID

// Maximum buffer size for static album arrays
#define MAX_ALBUMS_BUFFER   20  // Maximum number of albums for static buffers

// File count indicators
#define FILES_AVAILABLE_UNKNOWN   -1  // Used when file count is not available from cloud data

// Album path template macros for easy editing and consistency
// Regular albums (default, kidspack, nfc) - have separate home/player covers and animations
#define ALBUM_CONTENT_PATH_TEMPLATE     "/sdcard/content/full/%s/"
#define ALBUM_PLAY_COVER_PATH_TEMPLATE  "/sdcard/cover/device/%s_D.jpg"
#define ALBUM_HOME_COVER_PATH_TEMPLATE  "/sdcard/cover/device/%s.jpg"
#define ALBUM_ANIMATION_PATH_TEMPLATE   "/sdcard/animation_gif/album_cover/%s.gif"

// Blankee albums (SKURC/ISR) - use single cover for both home/player, no animation
#define BLANKEE_CONTENT_PATH_TEMPLATE   "/sdcard/content/full/%s/"
#define BLANKEE_COVER_PATH_TEMPLATE     "/sdcard/cover/device/%s.jpg"  // Try SKU-specific cover first
#define BLANKEE_COVER_FALLBACK          "/sdcard/cover/device/album_recorder.jpg"  // Fallback to generic recorder cover
#define BLANKEE_NO_ANIMATION            ""  // Blankee albums never have animations

/* Error handler - START */

#define SERIAL_NUMBER_SIZE  14
#define WIFI_SSID_SIZE      33
#define WIFI_PASSWORD_SIZE  33
#define SECRET_KEY_STR_SIZE 37
#define TIMEZONE_STR_SIZE   7
#define USE_NVS_CREDENTIALS 0, NULL
#define MEMO_MSG_MS         5000

#define JOIN_CMD            false       // Use do one try
#define WIFI_CMD            true        // Use default 2 tries (20sec)
#define S3ER_CMD_SHIFT      0x16        // Value displacement for cmd
#define S3ER_BLE_TASK_MS    500         // BLE task interval in ms (WiFi idle)
#define S3ER_BLE_TASK_WIFI  1000        // BLE task interval in ms (WiFi active)

typedef enum {
    S3ER_SYSTEM_IDLE = S3ER_CMD_SHIFT,  // [16] System idle - no error
    S3ER_SETUP_CONNECT_FAIL,            // [17] Setup connection failed
    S3ER_SETUP_CONNECT_SUCCESS,         // [18] Setup connection successful
    S3ER_SETUP_SSID_FAIL,               // [19] SSID setup failed
    S3ER_SETUP_SSID_SUCCESS,            // [1A] SSID setup successful
    S3ER_SETUP_PASS_FAIL,               // [1B] Password setup failed
    S3ER_SETUP_PASS_SUCCESS,            // [1C] Password setup successful
    S3ER_SETUP_SECK_FAIL,               // [1D] Secret key setup failed
    S3ER_SETUP_SECK_SUCCESS,            // [1E] Secret key setup successful
    S3ER_SETUP_SECK_NOT_IN_OOB,         // [1F] Secret key setup not in OOB binding mode
    S3ER_SETUP_TIMZ_FAIL,               // [20] Timezone setup failed
    S3ER_SETUP_TIMZ_SUCCESS,            // [21] Timezone setup successful
    S3ER_BIND_CLOUD_ERROR,              // [22] Cloud binding error
    S3ER_BIND_DEV_FAIL,                 // [23] Device binding failed
    S3ER_BIND_DEV_SUCCESS,              // [24] Device binding successful
    S3ER_BIND_DEV_SKIP,                 // [25] Device binding skipped
    S3ER_FULL_SYNC_SNTP_FAIL,           // [26] Full sync SNTP failed
    S3ER_FULL_SYNC_SNTP_SUCCESS,        // [27] Full sync SNTP successful
    S3ER_FULL_SYNC_OTA_FAIL,            // [28] Full sync OTA failed
    S3ER_FULL_SYNC_OTA_SUCCESS,         // [29] Full sync OTA successful
    S3ER_FULL_SYNC_ASSETS_FAIL,         // [2A] Full sync assets failed
    S3ER_FULL_SYNC_ASSETS_SUCCESS,      // [2B] Full sync assets successful
    S3ER_FULL_SYNC_ACCINFO_FAIL,        // [2C] Full sync account info failed
    S3ER_FULL_SYNC_ACCINFO_SUCCESS,     // [2D] Full sync account info successful
    S3ER_NFC_SYNC_ALBUM_1_FAIL,         // [2E] NFC sync album 1 failed
    S3ER_NFC_SYNC_ALBUM_1_SUCCESS,      // [2F] NFC sync album 1 successful
    S3ER_NFC_SYNC_ALBUM_2_FAIL,         // [30] NFC sync album 2 failed
    S3ER_NFC_SYNC_ALBUM_2_SUCCESS,      // [31] NFC sync album 2 successful
    S3ER_NFC_SYNC_ALBUM_3_FAIL,         // [32] NFC sync album 3 failed
    S3ER_NFC_SYNC_ALBUM_3_SUCCESS,      // [33] NFC sync album 3 successful
    S3ER_NFC_SYNC_ALBUM_4_FAIL,         // [34] NFC sync album 4 failed
    S3ER_NFC_SYNC_ALBUM_4_SUCCESS,      // [35] NFC sync album 4 successful
    S3ER_NFC_SYNC_ALBUM_5_FAIL,         // [36] NFC sync album 5 failed
    S3ER_NFC_SYNC_ALBUM_5_SUCCESS,      // [37] NFC sync album 5 successful
    S3ER_NFC_SYNC_ALBUM_6_FAIL,         // [38] NFC sync album 6 failed
    S3ER_NFC_SYNC_ALBUM_6_SUCCESS,      // [39] NFC sync album 6 successful
    S3ER_NFC_SYNC_ALBUM_7_FAIL,         // [3A] NFC sync album 7 failed
    S3ER_NFC_SYNC_ALBUM_7_SUCCESS,      // [3B] NFC sync album 7 successful
    S3ER_NFC_SYNC_ALBUM_8_FAIL,         // [3C] NFC sync album 8 failed
    S3ER_NFC_SYNC_ALBUM_8_SUCCESS,      // [3D] NFC sync album 8 successful
    S3ER_NFC_SYNC_ALBUM_9_FAIL,         // [3E] NFC sync album 9 failed
    S3ER_NFC_SYNC_ALBUM_9_SUCCESS,      // [3F] NFC sync album 9 successful
    S3ER_SYNC_FAIL,                     // [40] Complete sync failed
    S3ER_SYNC_SUCCESS,                  // [41] Complete sync successful
    S3ER_SETUP_CHANGE_WIFI_FAIL,        // [42] Change WiFi credentials failed
    S3ER_SETUP_CHANGE_WIFI_SUCCESS,     // [43] Change WiFi credentials successful
    S3ER_SETUP_WIFI_NO_CREDENTIALS,     // [44] WiFi setup no credentials available
    S3ER_SYNCING,                       // [45] WiFi setup syncing in progress
    S3ER_STOP_BLE_STREAM_A2DP,          // [46] Stop BLE for A2DP steaming
    S3ER_ATTENTION_BLE_SCAN_A2DP,       // [47] Attention BLE for A2DP scan
    S3ER_ATTENTION_BLE_IDLE_A2DP,       // [48] Attention BLE for A2DP idle
    S3ER_RESUME_BLE_STOP_A2DP,          // [49] Resume BLE and A2DP stop
    S3ER_FULL_SYNC_OTA_REQUIRED,        // [4A] Full sync OTA required - device will reboot
    S3ER_FULL_SYNC_OTA_NOT_REQUIRED,    // [4B] Full sync OTA not required - no update needed
    S3ER_SYNC_STATUS_OTA_IN_PROGRESS,   // [4C] OTA update in progress
    S3ER_SYNC_STATUS_DATA_SYNCING,      // [4D] Data sync in progress
    S3ER_SYNC_STATUS_COMPLETED,         // [4E] Data sync completed

    S3ER_MAX_VALUE                       // Keep this last for range checking
} s3_error_code_t;

// Manipulation functions for gPixseeStatus
void set_pixsee_status(uint8_t status);
/* Error handler - END */

/* Message handler - START */
#define S3MSG_FAIL          false
#define S3MSG_SUCCESS       true

#define S3MSG_WIFI_CONNECT  0 // Internet access bit
#define S3MSG_ACC_BOUND     1 // Account bounded bit
#define S3MSG_ACC_INFO      2 // Account info received bit
#define S3MSG_FULL_SYNCED   3 // (SNTP, OTA, Assets) synced bit
#define S3MSG_NFC_SYNCED    4 // (Albums, Covers) synced bit
#define S3MSG_RESERVED_1    5 // Not in use 1 bit
#define S3MSG_SYSTEM_NON    6 // Non Fatal Error bit
#define S3MSG_SYSTEM_FATAL  7 // Fatal Error bit
#define S3MSG_SYSTEM_RESET  0xFF // System reset required bit

// Bit manipulation functions for gPixseeMsg
void set_pixsee_msg(uint8_t bit_position, bool value);
bool get_pixsee_msg(uint8_t bit_position);
void set_default_pixsee_info(void);
/* Message handler - END */
typedef struct 
{
    int id;
    char name[64];
    char sku[22];
    char path[128];             // File path for album content (MP3 files)
    char play_cover[128];       // File path for playback screen album cover
    char home_cover[128];       // File path for home screen album cover
    char anim[128];             // File path for animation (GIF, Lottie, etc.)
    int  files_available;
    int  language;              // LANGUAGE_ENGLISH = 0, LANGUAGE_CHINESE = 1
    int  album_type;            // Album type (0: dafault, 1: SKU, 2: SKURC, 3: ISR)
    bool is_downloaded;         // Boolean indicating if the album is downloaded
    bool is_available_player;   // Boolean indicating if the album is available for player usage
    bool is_available_nfc;      // Boolean indicating if the album is available for NFC usage
} s3_album_handler_t;

/* TODO: This table should be downloaded from cloud */
// NOTE: s3_album_mgr will be moved to a dedicated component and these declarations will stay on main

// Legacy s3_albums removed - using separated album arrays approach

// NEW: Dynamic album management system
extern s3_album_handler_t *s3_dynamic_albums;   // Comprehensive dynamic album array (replaces static s3_albums)

// Note: Album management functions moved to s3_album_mgr.h
// Only essential forward declarations remain here for backward compatibility

typedef enum {
    AUDIO_SINK_AUTO = -1,   /* choose I2S unless BT-A2DP is connected   */
    AUDIO_SINK_I2S  = 0,
    AUDIO_SINK_A2DP = 1,
} audio_sink_t;

// Playback mode enumeration
typedef enum {
    PLAYBACK_MODE_SEQUENTIAL = 0,    // Play tracks in order
    PLAYBACK_MODE_SHUFFLE = 1        // Play tracks randomly
} playback_mode_t;

// Auto-play settings
typedef enum {
    AUTO_PLAY_OFF = 0,               // No auto-play (stop when album finishes)
    AUTO_PLAY_FOLDER = 1,            // Keep playing the same folder/album over and over
    AUTO_PLAY_ALL = 2                // Go to next album when current album finishes
} auto_play_mode_t;

extern s3_album_handler_t   *s3_current_album;
extern size_t               s3_current_idx;
extern size_t               s3_current_size;

extern size_t               s3_current_idx_track;     /* next MP3 inside the album      */
extern size_t               s3_current_size_track;    /* filled by build_playlist()     */
extern char**               s3_current_track_list;    /* track paths in current playlist */

extern audio_sink_t         s3_active_sink;
extern playback_mode_t      s3_playback_mode;         /* Sequential or shuffle playback */
extern auto_play_mode_t     s3_auto_play_mode;        /* Auto-play behavior */
// ALBUM: definition - END

// ALARM: definition - START
typedef enum
{
    ALARM_1 = 0,
    ALARM_2,
    ALARM_3,
    ALARM_4,
    ALARM_5,
    ALARM_6,
    ALARM_7,

    ALARMS_QTD
}s3_alarms_t;

typedef struct 
{
    s3_alarms_t id;
    const char *name;
    const char *audio;
    const char *cover;
    const char *anim;
} s3_alarm_handler_t;

// NOTE: s3_alarm_mgr will be moved to a dedicated component and these declarations will stay on main
extern const s3_alarm_handler_t s3_alarms[];

extern s3_alarm_handler_t  *s3_current_alarm;
// ALARM: definition - END

// SYSTEM: definition - START
typedef enum
{
    BOOT_SCREEN = 0,
    POWER_LOW_SCREEN,
    STANDBY_SCREEN,         // Black screen, for power saving
    POWER_OFF_SCREEN,       // Black screen, for power saving
    SHUTDOWN_SCREEN,        // Shutdown animation
    HOME_SCREEN,
    PLAY_SCREEN,
    PAUSE_SCREEN,
    VOLUME_UP_SCREEN,
    VOLUME_DOWN_SCREEN,
    CLOCK_SCREEN,
    ALARM_SCREEN,
    DISPLAY_SCREEN,
    DISPLAY_SETTINGS_SCREEN,
    BRIGHTNESS_UP_SCREEN,
    BRIGHTNESS_DOWN_SCREEN,
    BLUETOOTH_SCREEN,
    BLUETOOTH_SCAN_SCREEN, 
    WIFI_SEARCH_SCREEN,            // search
    BLE_PAIRING_SCREEN,
	WIFI_UNKNOWN_SCREEN,
	WIFI_DISCONNECT_SCREEN,
    DATA_SYNC_SCREEN,       // kid sync
    OTA_SCREEN,
    WIFI_PLUG_IN_SCREEN,
    WIFI_SYNCED_SCREEN,
    NFC_SCREEN,
    NFC_LANGUAGE_SCREEN,
    NFC_ACTIVATION_SCREEN,
    NFC_CONTENT_SCREEN,
    POWER_CHARGE_SCREEN,
    POWER_FULL_SCREEN,
    FAC_RESET_SCREEN,
    COUNTDOWN_SCREEN,
    POWER_ON_KID_SCREEN,
    NFC_WIFI_SEARCH_SCREEN,
    NFC_WIFI_DISCONNECT_SCREEN,
    NFC_NO_CONTENT_SCREEN,
    POWER_LOW_PLUG_IN_SCREEN,
    VOLUME_SCREEN,
    WIFI_SYNC_MAI_SCREEN, // wifi menu
    WIFI_SYNC_ERR_SCREEN,
    WIFI_SYNC_SUC_SCREEN,
    WIFI_SYNC_N_SCREEN,
    WIFI_ERR_SCREEN,
    ACC_INV_FAC_RESET_SCREEN,
  
    SCREENS_QTD,
    DUMMY_SCREEN,
    NULL_SCREEN
}s3_screens_t;

typedef struct 
{
    s3_screens_t id;
    const char *name;
    const char *resource;
    int32_t duration_ms;
    int8_t base_type;
} s3_screen_assmbler_t;

// NOTE: s3_screen_mgr will be moved to a dedicated component and this declaration will stay on main
extern const s3_screen_assmbler_t s3_screen_resources[];

typedef enum {
    TOP_GRADIENT,      // Dark at top, transparent at bottom (y=0)
    BOTTOM_GRADIENT    // Transparent at top, dark at bottom (y=160)
} gradient_type_t;

extern int          s3_volume_level;        // VOLUME_LEVEL_1 - VOLUME_LEVEL_5
extern int          s3_brightness_level;    // BRIGHTNESS_LEVEL_1 - BRIGHTNESS_LEVEL_3
extern int          s3_battery_level;       // GET FROM HARDWARE
extern int          s3_battery_percent;     // GET FROM HARDWARE
extern int          s3_charger_status;      // BATTERY_CHARGE, BATTERY_DISCHARGE  
extern int          s3_selected_language;   // LANGUAGE_ENGLISH, LANGUAGE_CHINESE
extern char         s3_qr_payload[QRCODE_CONTENT_LEN];
extern char         s3_binding_msg[64];     // "none", "binded", "unbound"
extern int          s3_pairing_status;      // BT_UNPAIRED, BT_SCAN, BT_PAIRED
extern int          s3_nfc_content_type;    // NFC_CONT_NOT_AVAIL, NFC_CONT_UPDATING, etc.
extern bool         s3_use_animations;      // USE_ANIMATION, NO_ANIMATION
extern bool         s3_boot_completed;      // Flag to prevent events during boot
extern bool         s3_shutdown_started;    // Flag to prevent events during shutdown
extern int          s3_sync_stage;          // Current data sync stage (0=prepare, 1=wifi, 2=resource, 3=account)
extern bool         s3_data_sync_show_wait; // Flag to show wait screen during data sync
extern bool         gOTA_in_progress;       // Flag to track if OTA update is in progress
extern TaskHandle_t wifi_connecting_task_handle; // unified_sync_task handle (for status query)
extern bool         s3_ble_ready;           // Flag to prevent BLE messages during BLE initialization
extern bool         s3_show_lower_5;        // Flag to prevent show again when lower than 5%
extern bool         s3_show_lower_10;       // Flag to prevent show again when lower than 10%
extern bool         s3_show_higher_99;      // Flag to prevent show again when higher than 99%

typedef struct _lv_timer_t lv_timer_t;
extern lv_timer_t   *wifi_pairing_defer_timer;  /* NEW */
// extern TaskHandle_t wifi_pairing_task_handle;   /* Moved from wifi */

// Global variables from main.c - START
typedef enum {
    POWER_MODE_NORMAL = 0,
    POWER_MODE_SHUTDOWN = 1,
    POWER_MODE_RESTART = 2
} power_mode_t;

extern power_mode_t global_poweroff;
extern bool         global_plugged_in;
extern bool         system_transition_in_progress;  // Flag to block buttons during audio/screen transitions
extern int          gVoltage;
extern uint8_t      gPixseeStatus;
extern uint8_t      gPixseeMsg;
extern bool         gSyncInProgress;
extern bool         gBTReconnectInProgress;
extern bool         sleep_flag;
extern esp_err_t    g_init_sdcard;
// Global variables from main.c - END

void memory_status(void);
void sys_memory_status(const char *tag, const char *msg);
bool skip_memory_check(void);
bool get_memory_logs_status(void);
void toogle_memory_logs_flag(void);

// SD card DMA mutex - for coordinating DMA operations between SDMMC and BLE
extern SemaphoreHandle_t g_sdcard_dma_mutex;
void init_sdcard_dma_mutex(void);
void deinit_sdcard_dma_mutex(void);

char *read_file_to_spiram(const char *filename);
char *strdup_spiram(const char *str);

// SYSTEM: definition - END
#endif /* S3_DEFINITIONS_H */
