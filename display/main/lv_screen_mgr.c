//
// Created by Antonio_Pereira on 2025/5/20.
//

// Antonio current version

static void ui_add_top_badge(void);

#include "esp_log.h"
#include "esp_random.h"  // For esp_random()
#include "lvgl.h"
#include "esp_heap_caps.h"
#include "lv_decoders.h"
#include "lv_screen_mgr.h"
#include "audio_player.h"
#include "s3_definitions.h"
#include "lvgl__lvgl/src/misc/lv_color.h"
#include "lvgl__lvgl/src/extra/libs/qrcode/lv_qrcode.h"
#include "lvgl__lvgl/src/extra/libs/gif/lv_gif.h"
#include "fonts/cherry_bomb_fonts.h"
#include "storage.h"
#include "sntp_syncer.h"
#include "WiFi.h"
#include "s3_https_cloud.h"
#include "s3_nvs_item.h"
#include "manual_ota.h"
#include "s3_bluetooth.h"
#include "backlight.h"
#include "power_management.h"
#include "app_timeout.h"
#include "s3_album_mgr.h"
#if 0 // #ifndef NO_LOTTIE
// #include "lv_lottie.h"
#endif

#include "freertos/semphr.h"
static SemaphoreHandle_t gui_mutex = NULL;
static bool gui_mutex_initialized = false;

// lottie buffer
// static uint8_t *lottie_buf = NULL;

// Environment variables for animations
static const char *TAG = "LV_SCREEN_MGR";

static lv_obj_t *bars[5];                   // 5 level segments
static lv_obj_t *dot_objs[MAX_DOTS];        // 10 dots below player
static lv_obj_t *animated_objects[4];       // Objects for animations in built-in animation demo

uint32_t lv_bkg_color = LV_CUSTOM_WHITE;    // Default white background color
uint32_t lv_crc_color = LV_CUSTOM_BLACK;    // Default black circle color
uint32_t lv_fnt_color = LV_CUSTOM_BLACK;    // Default black font color
uint32_t lv_dot_color = LV_CUSTOM_BLUE;     // Default blue dot color
uint32_t lv_helper_color = LV_CUSTOM_GRAY;  // Default black font color

// Static variables for Lottie monitoring
// static lv_timer_t *lottie_watchdog_timer = NULL;
// static lv_obj_t *current_lottie_obj = NULL;
// static uint32_t lottie_render_count = 0;
// static bool lottie_corruption_detected = false;
static bool screen_update_pending = false;

// Singleton state for language badge
static lv_obj_t *lang_badge_img = NULL;
static lv_timer_t *lang_badge_timer = NULL;

// Cached pause badge system
static bool pause_overlay_drawn = false;

// Track last displayed album for optimization
static s3_album_handler_t *last_displayed_album = NULL;
static s3_album_handler_t *last_displayed_home_album = NULL;

// Badge user data tracking system
// User data bit layout (32-bit):
// Bits 24-31: Magic marker (0x1C = badge marker)
// Bits 16-23: Badge type (CONTENT_TYPE_*)
// Bits 8-15:  Badge subtype/position info
// Bits 0-7:   Status bits
#define BADGE_MAGIC_MARKER    0x1C
#define BADGE_MAGIC_SHIFT     24
#define BADGE_TYPE_SHIFT      16
#define BADGE_SUBTYPE_SHIFT   8
#define BADGE_STATUS_SHIFT    0

// Badge subtypes for different badge purposes
#define BADGE_SUBTYPE_TOP_BATTERY    0x01  // Battery icon at top
#define BADGE_SUBTYPE_TOP_BT         0x02  // BT icon at top
#define BADGE_SUBTYPE_LANG           0x03  // Language badge
#define BADGE_SUBTYPE_NUMBER         0x04  // Track number label
#define BADGE_SUBTYPE_PAUSE          0x05  // Pause icon badge
#define BADGE_SUBTYPE_DOT_INDICATOR  0x06  // Dot indicator badge

// Dot badge state tracking
static uint8_t last_num_dots = 0;
static uint8_t last_selected_dot = 0;
static bool last_was_album_usage = false;

// Data sync screen state tracking (for GIF animation)
static int last_sync_stage = -1;
static lv_obj_t *data_sync_gif_obj = NULL;

// Status bits for battery badge
#define BATT_STATUS_CHARGING         0x01
#define BATT_STATUS_FULL             0x02
#define BATT_STATUS_LOW              0x04

// Status bits for BT badge
#define BT_STATUS_CONNECTED          0x01
#define BT_STATUS_TIMEOUT            0x02
#define BT_STATUS_FAILED             0x04

// Status bits for language badge
#define LANG_STATUS_ENGLISH          0x01
#define LANG_STATUS_CHINESE          0x02
#define LANG_STATUS_ALL              0x03

// Status bits for number badge - stores the actual track number (1-255)
// The status byte directly contains the track number value

// Screen handlers for state
lv_timer_t              *screen_timer           = NULL;
s3_screens_t            s3_current_screen       = DUMMY_SCREEN;
s3_screens_t            s3_previous_screen      = DUMMY_SCREEN;
s3_screens_t            s3_last_screen          = DUMMY_SCREEN;
s3_screens_t            s3_next_screen          = DUMMY_SCREEN;
s3_screens_t            s3_preLowBattery_screen = DUMMY_SCREEN;
s3_screen_assmbler_t    s3_recover              = {0};
bool                    s3_carroucel            = false;

// Post-transition callback support
static post_transition_cb_t pending_callback = NULL;
static post_transition_cb_t pending_single_callback = NULL;
static s3_screens_t callback_current_screen = NULL_SCREEN;
static s3_screens_t callback_next_screen = NULL_SCREEN;
// Global variables s3_brightness_level, s3_battery_level, s3_battery_percent, s3_charger_status,
// s3_selected_language, s3_boot_completed, s3_shutdown_started, s3_show_lower_5, s3_show_lower_10,
// s3_show_higher_99, s3_pairing_status, s3_binding_msg, and s3_qr_payload are defined in
// s3_definitions.c and declared in s3_definitions.h

char                    *s3_language_resource   = NO_RESOURCE;
char                    *s3_mini_lang_resource  = NO_RESOURCE;
char                    *s3_mini_bt_resource    = NO_RESOURCE;
char                    s3_babyId[32]	        = "";
char                    s3_WiFiSyncKidIcon[8]	= "";
int                     dummy_bt                = DUMMY_READY;
int                     dummy_wifi              = DUMMY_READY;
bool                    s3_show_default_syncUp  = false;
bool                    s3_wifi_downloading     = false;

static bool             s3_player_update    = true;
static bool             s3_lang_state       = true;
static bool             s3_pause_state      = false;
static lv_timer_t       *s3_countdown_timer = NULL;
static lv_obj_t         *s3_countdown_label = NULL;
static int              s3_countdown_value = COUNTDOWN_START_VALUE;
uint8_t                 s3_menu_idx             = 0;

// Auxiliary callers
void ui_set_battery_level(int pct);
void ui_set_dots_bar(lv_obj_t *anchor, uint8_t num_dots, uint8_t selected_dot);
// static void ensure_lottie_buf(int width, int height);
static void screen_timer_cb(lv_timer_t *t);
static const char* get_mini_language_resource(void);

/* Forward declarations for callbacks */
void update_screen_display(void);
void refresh_screen_display(void);

// Initialize the GUI mutex - call this once during startup
static void gui_mutex_init(void)
{
    if (!gui_mutex_initialized) {
        gui_mutex = xSemaphoreCreateRecursiveMutex();
        if (gui_mutex != NULL) {
            gui_mutex_initialized = true;
            ESP_LOGI(TAG, "GUI mutex initialized successfully");
        } else {
            ESP_LOGE(TAG, "Failed to create GUI mutex!");
        }
    }
}

// Deinitialize the GUI mutex - call this during shutdown
static void gui_mutex_deinit(void)
{
    if (gui_mutex_initialized && gui_mutex) {
        vSemaphoreDelete(gui_mutex);
        gui_mutex = NULL;
        gui_mutex_initialized = false;
        ESP_LOGI(TAG, "GUI mutex deinitialized");
    }
}

static inline void gui_lock(void)
{
    if (gui_mutex && gui_mutex_initialized) {
        if (xSemaphoreTakeRecursive(gui_mutex, pdMS_TO_TICKS(1000)) != pdTRUE) {
            ESP_LOGW(TAG, "GUI lock timeout after 1000ms!");
        }
    }
}

static inline void gui_unlock(void)
{
    if (gui_mutex && gui_mutex_initialized) {
        xSemaphoreGiveRecursive(gui_mutex);
    }
}

void enable_player_update(void)
{
    if (!s3_player_update) {
        s3_player_update = true;
        ESP_LOGI(TAG, "activate_player_update: Player screen update flag set");
    }
}

void enable_lang_badge_update(void)
{
    if (!s3_lang_state) {
        s3_lang_state = true;
        ESP_LOGI(TAG, "activate_player_update: Lang badge update flag set");
        enable_player_update();
    }
}

void enable_pause_update(void)
{
    if (!s3_pause_state) {
        s3_pause_state = true;  // Set to true for pause overlay
        s3_lang_state = false;  // Prevent lang badge on pause
        ESP_LOGI(TAG, "activate_player_update: Pause update flag set");
        enable_player_update();
    }
}

void enable_resume_update(void)
{
    if (s3_pause_state) {
        s3_pause_state = false; // Set to false for normal badges
        s3_lang_state = false;  // Prevent lang badge on resume
        ESP_LOGI(TAG, "activate_player_update: Resume update flag set");
        enable_player_update();
    }
}

/* Callback functions for animations */
static void bounce_anim_y_cb(void *var, int32_t value)
{
    lv_obj_set_y(var, value);
}

static void size_anim_cb(void *var, int32_t value)
{
    lv_obj_set_size(var, value, value);
    lv_obj_align(var, LV_ALIGN_CENTER, 0, 0);
}

static void color_anim_cb(void *var, int32_t value)
{
    lv_color_t color = lv_color_mix(lv_color_hex(0xFF0000), lv_color_hex(0x00FF00), value);
    lv_obj_set_style_bg_color(var, color, 0);
}

static void opacity_anim_cb(void *var, int32_t value)
{
    lv_obj_set_style_opa(var, value, 0);
}

static void anim_timer_cb(lv_timer_t *timer)
{
    ESP_LOGD(TAG, "Animation timer tick");
    // No specific action needed here, as timer itself triggers LVGL update
}

static void screen_timer_cb(lv_timer_t *t)
{
    if (s3_carroucel)
        s3_next_screen = (s3_current_screen + 1) % SCREENS_QTD;
	lv_async_call((lv_async_cb_t)update_screen_display, NULL);
}

/**
 * @brief Callback to restore last-played album after HOME screen is first rendered
 *        This is called as a post-transition callback to ensure HOME screen is fully drawn
 *        before triggering album switch, which prevents display cache issues
 */
static void restore_last_played_album_callback(void)
{
    // Only restore once at boot - not on subsequent HOME screen visits
    static bool restore_attempted = false;
    if (restore_attempted) {
        ESP_LOGD(TAG, "[LAST_ALBUM] Restore already done, skipping");
        return;
    }
    restore_attempted = true;
    
    ESP_LOGI(TAG, "[LAST_ALBUM] Restoring after HOME screen rendered");
    
    // Try to restore album from SD card file
    int album_idx = s3_albums_restore_last_played();
    if (album_idx >= 0) {
        ESP_LOGI(TAG, "[LAST_ALBUM] Switching to album index %d", album_idx);
        // Switch to the restored album - this triggers a screen redraw exactly like a user button press
        n_step_album((size_t)album_idx);
        
        // Trigger immediate screen refresh to redraw HOME with new album cover and badges
        // This eliminates the ~11s delay and makes boot experience feel instant
        // Must call refresh_screen_display() instead of just lv_refr_now() to ensure
        // the screen redraw logic executes and updates album cover/badges
        refresh_screen_display();
        ESP_LOGI(TAG, "[LAST_ALBUM] Triggered immediate screen refresh");
    } else {
        ESP_LOGI(TAG, "[LAST_ALBUM] Using default album");
    }
}

static void red_color_fade_cb(void *var, int32_t value)
{
    lv_color_t color = lv_color_make((uint8_t)value, 0, 0);  // RGB = (value, 0, 0)
    lv_obj_set_style_bg_color((lv_obj_t *)var, color, 0);
}

/*
// Watchdog callback to monitor Lottie health
static void lottie_watchdog_cb(lv_timer_t *timer)
{
    ESP_LOGD(TAG, "Lottie watchdog tick - renders: %lu", lottie_render_count);

    // Check if we've exceeded safe render count
    if (lottie_render_count > 100) {
        ESP_LOGW(TAG, "Lottie render count high (%lu), potential memory pressure", lottie_render_count);
    }

    // Check object validity
    if (current_lottie_obj && !lv_obj_is_valid(current_lottie_obj)) {
        ESP_LOGE(TAG, "Lottie object corruption detected!");
        lottie_corruption_detected = true;

        if (lottie_watchdog_timer) {
            lv_timer_del(lottie_watchdog_timer);
            lottie_watchdog_timer = NULL;
        }
        current_lottie_obj = NULL;
    }
}
*/

static void lang_badge_hide_cb(lv_timer_t *timer)
{
    gui_lock();
    lv_obj_t *obj = (lv_obj_t *)timer->user_data;
    if (obj && lv_obj_is_valid(obj)) {
        lv_obj_del(obj);
    }
    // Clear singleton state if matching
    if (obj == lang_badge_img) {
        lang_badge_img = NULL;
    }
    if (timer == lang_badge_timer) {
        lang_badge_timer = NULL;
    }
    lv_timer_del(timer);
    
    // Trigger player screen refresh after language badge timeout
    // This ensures the remaining UI elements are properly redrawn
    if (s3_current_screen == PLAY_SCREEN) {
        s3_player_update = true;
        ESP_LOGI(TAG, "Language badge timeout - triggering player screen refresh");
    }
    
    gui_unlock();
}

/* Badge helper functions - User data tracking system */

// Create user data with all components
static inline uint32_t make_badge_user_data(content_type_t type, uint8_t subtype, uint8_t status)
{
    return (BADGE_MAGIC_MARKER << BADGE_MAGIC_SHIFT) |
           ((uint32_t)type << BADGE_TYPE_SHIFT) |
           ((uint32_t)subtype << BADGE_SUBTYPE_SHIFT) |
           ((uint32_t)status << BADGE_STATUS_SHIFT);
}

// Check if object is a badge
static bool is_badge_object(lv_obj_t *obj)
{
    if (!obj || !lv_obj_is_valid(obj)) return false;
    uint32_t data = (uint32_t)(uintptr_t)lv_obj_get_user_data(obj);
    return ((data >> BADGE_MAGIC_SHIFT) & 0xFF) == BADGE_MAGIC_MARKER;
}

// Get badge type
static content_type_t get_badge_type(lv_obj_t *obj)
{
    uint32_t data = (uint32_t)(uintptr_t)lv_obj_get_user_data(obj);
    return (content_type_t)((data >> BADGE_TYPE_SHIFT) & 0xFF);
}

// Get badge subtype
static uint8_t get_badge_subtype(lv_obj_t *obj)
{
    uint32_t data = (uint32_t)(uintptr_t)lv_obj_get_user_data(obj);
    return (uint8_t)((data >> BADGE_SUBTYPE_SHIFT) & 0xFF);
}

// Get badge status
static uint8_t get_badge_status(lv_obj_t *obj)
{
    uint32_t data = (uint32_t)(uintptr_t)lv_obj_get_user_data(obj);
    return (uint8_t)(data & 0xFF);
}

// Find badge by type and subtype
static lv_obj_t* find_badge(content_type_t type, uint8_t subtype)
{
    lv_obj_t *screen = lv_scr_act();
    if (!screen) return NULL;

    uint32_t child_cnt = lv_obj_get_child_cnt(screen);
    for (uint32_t i = 0; i < child_cnt; i++) {
        lv_obj_t *child = lv_obj_get_child(screen, i);
        if (is_badge_object(child) &&
            get_badge_type(child) == type &&
            get_badge_subtype(child) == subtype) {
            return child;
        }
    }
    return NULL;
}

// Delete all badges of specific type
static void delete_badges_by_type(content_type_t type)
{
    lv_obj_t *screen = lv_scr_act();
    if (!screen) return;

    uint32_t child_cnt = lv_obj_get_child_cnt(screen);
    for (int32_t i = child_cnt - 1; i >= 0; i--) {  // Iterate backwards when deleting
        lv_obj_t *child = lv_obj_get_child(screen, i);
        if (is_badge_object(child) && get_badge_type(child) == type) {
            lv_obj_del(child);
        }
    }
}

// Dump badge status for debugging
static void dump_badge_status(const char *context)
{
    lv_obj_t *screen = lv_scr_act();
    if (!screen) {
        ESP_LOGW(TAG, "[BADGE_DUMP] %s: No active screen", context);
        return;
    }

    uint32_t child_cnt = lv_obj_get_child_cnt(screen);
    int badge_count = 0;

    ESP_LOGI(TAG, "[BADGE_DUMP] %s: Scanning %lu screen objects...", context, child_cnt);

    for (uint32_t i = 0; i < child_cnt; i++) {
        lv_obj_t *child = lv_obj_get_child(screen, i);
        if (is_badge_object(child)) {
            badge_count++;
            content_type_t type = get_badge_type(child);
            uint8_t subtype = get_badge_subtype(child);
            uint8_t status = get_badge_status(child);

            const char *type_name = (type < CONTENT_TYPE_MAX) ? content_type_names[type] : "UNKNOWN";
            const char *subtype_name = "UNKNOWN";

            switch (subtype) {
                case BADGE_SUBTYPE_TOP_BATTERY: subtype_name = "TOP_BATTERY"; break;
                case BADGE_SUBTYPE_TOP_BT:      subtype_name = "TOP_BT"; break;
                case BADGE_SUBTYPE_LANG:        subtype_name = "LANG"; break;
                case BADGE_SUBTYPE_NUMBER:      subtype_name = "NUMBER"; break;
                case BADGE_SUBTYPE_PAUSE:       subtype_name = "PAUSE"; break;
                case BADGE_SUBTYPE_DOT_INDICATOR: subtype_name = "DOT_INDICATOR"; break;
            }

            if (subtype == BADGE_SUBTYPE_NUMBER) {
                ESP_LOGI(TAG, "[BADGE_DUMP]   Badge #%d: type=%s subtype=%s track_num=%d",
                         badge_count, type_name, subtype_name, status);
            } else {
                ESP_LOGI(TAG, "[BADGE_DUMP]   Badge #%d: type=%s subtype=%s status=0x%02X",
                         badge_count, type_name, subtype_name, status);
            }
        }
    }

    if (badge_count == 0) {
        ESP_LOGI(TAG, "[BADGE_DUMP] %s: No badges found on screen", context);
    } else {
        ESP_LOGI(TAG, "[BADGE_DUMP] %s: Total badges found: %d", context, badge_count);
    }
}

// Dot-specific helper functions
// For dot badges: subtype is always BADGE_SUBTYPE_DOT_INDICATOR
// Status byte contains the dot index (0-9)

static inline uint32_t make_dot_user_data(uint8_t dot_index)
{
    // Use CONTENT_TYPE_MENU for dots (they're navigation indicators)
    return make_badge_user_data(CONTENT_TYPE_MENU, BADGE_SUBTYPE_DOT_INDICATOR, dot_index);
}

// Find dot badge by index
static lv_obj_t* find_dot_badge(uint8_t dot_index)
{
    lv_obj_t *screen = lv_scr_act();
    if (!screen) return NULL;

    uint32_t child_cnt = lv_obj_get_child_cnt(screen);
    for (uint32_t i = 0; i < child_cnt; i++) {
        lv_obj_t *child = lv_obj_get_child(screen, i);
        if (is_badge_object(child) &&
            get_badge_type(child) == CONTENT_TYPE_MENU &&
            get_badge_subtype(child) == BADGE_SUBTYPE_DOT_INDICATOR &&
            get_badge_status(child) == dot_index) {
            return child;
        }
    }
    return NULL;
}

// Get dot index from badge
static inline uint8_t get_dot_index(lv_obj_t *obj)
{
    return get_badge_status(obj);
}

// Dot position structure and helper function
typedef struct {
    lv_coord_t x;
    lv_coord_t y;
} DotPos;

// Get dot positions based on number of dots
static const DotPos* get_dot_positions(uint8_t num_dots)
{
    static const DotPos pos1[] = { {115, 211} };
    static const DotPos pos2[] = { {105, 210}, {125, 210} };
    static const DotPos pos3[] = { {95, 209}, {115, 211}, {135, 209} };
    static const DotPos pos4[] = { {85, 206}, {105, 210}, {125, 210}, {145, 206} };
    static const DotPos pos5[] = { {76, 203}, {95, 209}, {115, 211}, {135, 209}, {154, 203} };
    static const DotPos pos6[] = { {67, 198}, {85, 206}, {105, 210}, {125, 210}, {145, 206}, {163, 198} };
    static const DotPos pos7[] = { {59, 193}, {76, 203}, {95, 209}, {115, 211}, {135, 209}, {154, 203}, {171, 193} };
    static const DotPos pos8[] = { {51, 186}, {67, 198}, {85, 206}, {105, 210}, {125, 210}, {145, 206}, {163, 198}, {179, 186} };
    static const DotPos pos9[] = { {44, 179}, {59, 193}, {76, 203}, {95, 209}, {115, 211}, {135, 209}, {154, 203}, {171, 193}, {186, 179} };

    switch (num_dots) {
        case 1: return pos1;
        case 2: return pos2;
        case 3: return pos3;
        case 4: return pos4;
        case 5: return pos5;
        case 6: return pos6;
        case 7: return pos7;
        case 8: return pos8;
        case 9: return pos9;
        default: return pos1;
    }
}

static void ui_add_lang_badge(bool in_home_screen)
{
    // Check if language badge should be displayed
    if (!s3_lang_state) {
        ESP_LOGI(TAG, "[LVGL] ui_add_lang_badge: Flag is false, skipping badge display");
        return;
    }
    
    // Check if this is a music album (ALBUM_SKU) with empty language field
    // If language is NO_LANGUAGE (-1) for ALBUM_SKU type, don't show language badge
    if (s3_current_album && s3_current_album->album_type == ALBUM_SKU &&
        s3_current_album->language == NO_LANGUAGE) {
        ESP_LOGI(TAG, "[LVGL] ui_add_lang_badge: Music album with empty language, skipping badge display");
        return;
    }

    s3_mini_lang_resource = get_mini_language_resource();

    ESP_LOGI(TAG, "[LVGL] ui_add_lang_badge: png decode %s (system_lang=%d, album_lang=%d)",
             s3_mini_lang_resource, s3_selected_language,
             s3_current_album ? s3_current_album->language : -1);

    // Load language icon into dedicated language badge buffer (PNG instead of JPG)
    if (lvgl_load_content_png(CONTENT_TYPE_LANGUAGE_BADGE, s3_mini_lang_resource) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to load language PNG icon %s", s3_mini_lang_resource);
        return;
    }

    // Get the content descriptor for language badge
    const lv_img_dsc_t *icon_dsc = lvgl_get_content_dsc(CONTENT_TYPE_LANGUAGE_BADGE);
    if (!icon_dsc || !icon_dsc->data) {
        ESP_LOGE(TAG, "Language icon descriptor is NULL or invalid");
        return;
    }

    ESP_LOGI(TAG, "[LVGL] ui_add_lang_badge: PNG icon loaded, size: %dx%d", icon_dsc->header.w, icon_dsc->header.h);

    // Find or create language badge using new tracking system
    lv_obj_t *lang_img = find_badge(CONTENT_TYPE_LANGUAGE_BADGE, BADGE_SUBTYPE_LANG);

    if (!lang_img) {
        // Create new language badge
        lang_img = lv_img_create(lv_scr_act());

        // Set user data with language status
        uint8_t lang_status = (s3_selected_language == LANGUAGE_ENGLISH) ? LANG_STATUS_ENGLISH :
                             (s3_selected_language == LANGUAGE_CHINESE) ? LANG_STATUS_CHINESE :
                             LANG_STATUS_ALL;

        uint32_t user_data = make_badge_user_data(
            CONTENT_TYPE_LANGUAGE_BADGE,
            BADGE_SUBTYPE_LANG,
            lang_status
        );
        lv_obj_set_user_data(lang_img, (void*)(uintptr_t)user_data);

        // Also keep singleton reference for backward compatibility
        lang_badge_img = lang_img;
    }

    // Update image and position
    lv_img_set_src(lang_img, icon_dsc);
    if (in_home_screen)
        lv_obj_set_pos(lang_img, 94, 158);
    else
        lv_obj_set_pos(lang_img, 146, 174);
    lv_obj_move_foreground(lang_img);

    // Reset one-shot timer to hide after 2 seconds
    if (lang_badge_timer) {
        lv_timer_del(lang_badge_timer);
        lang_badge_timer = NULL;
    }
    lang_badge_timer = lv_timer_create(lang_badge_hide_cb, 2000, lang_img);
    if (!lang_badge_timer) {
        ESP_LOGW(TAG, "Failed to create timer to hide language badge");
    }
    
    // Clear the flag to prevent badge from being shown again until next trigger
    s3_lang_state = false;
    ESP_LOGI(TAG, "[LVGL] ui_add_lang_badge: Badge displayed, cleared s3_ui_lang_state flag");
}

/* Helper functions */
typedef void (*wifi_connect_result_cb_t)(bool success, const char *msg);
static wifi_connect_result_cb_t wifi_sta_result_cb = NULL;
static wifi_connect_result_cb_t wifi_ap_result_cb = NULL;

void wifi_station_result_callback(wifi_connect_result_cb_t cb)
{
    wifi_sta_result_cb = cb;
}

void wifi_ap_result_callback(wifi_connect_result_cb_t cb)
{
    wifi_ap_result_cb = cb;
}

static void wifi_update_qr_values(void)
{
    char sn[SERIAL_NUMBER_SIZE] = {0};
    char sn_final_numbers[5] = {0};
    const char *auth_type;

    read_serial_number(sn);
    strncpy(sn_final_numbers, &sn[strlen(sn) - 4], 4);
    sn_final_numbers[4] = '\0';

    // Determine authentication type based on compile-time WPA3 support
#if 0
    auth_type = "WPA3";  // WPA3-SAE
#else
    auth_type = "WPA";   // WPA2-PSK
#endif

    // WiFi QR code format for smartphone compatibility
    // Standard format: WIFI:T:AUTH_TYPE;S:SSID;P:PASSWORD;H:false;;
    snprintf(s3_qr_payload, sizeof(s3_qr_payload), "WIFI:T:%s;S:Pixsee_%s;P:%s;H:false;;", auth_type, sn_final_numbers, CONFIG_S3_AP_WIFI_PASSWORD);
}

static void countdown_timer_cb(lv_timer_t *timer)
{
    gui_lock();
    char buf[4];
    snprintf(buf, sizeof(buf), "%d", s3_countdown_value);

    if (s3_countdown_value == 5) {
        lv_fnt_color = LV_CUSTOM_YELLOW;
    } else if (s3_countdown_value == 4) {
        lv_fnt_color = LV_CUSTOM_GREEN;
    } else if (s3_countdown_value == 3) {
        lv_fnt_color = LV_CUSTOM_PINK;
    } else if (s3_countdown_value == 2) {
        lv_fnt_color = LV_CUSTOM_CYAN;
    } else if (s3_countdown_value == 1) {
        lv_fnt_color = LV_CUSTOM_ORANGE;
    } else if (s3_countdown_value == 0) {
        lv_fnt_color = LV_CUSTOM_BLACK; // Means 0 will not appear
        global_poweroff = POWER_MODE_SHUTDOWN;         // System shutdown
    } else {
        ESP_LOGW(TAG, "Invalid countdown value %d, defaulting to yellow", s3_countdown_value);
        lv_fnt_color = LV_CUSTOM_YELLOW;
    }

    lv_obj_set_style_text_color(s3_countdown_label, lv_color_hex(lv_fnt_color), 0);
    lv_label_set_text(s3_countdown_label, buf);

    if (s3_countdown_value == 0) {
        // Delete timer so it doesn't run again
        lv_timer_del(s3_countdown_timer);
        s3_countdown_timer = NULL;

        gui_unlock();
        return;
    }
    s3_countdown_value--;
    gui_unlock();
}

void ui_update_language_data(void)
{
    ESP_LOGW(TAG, "[LANGUAGE] ui_update_language_data: load %d", s3_selected_language);
    assert(s3_selected_language != NO_LANGUAGE && "NO_LANGUAGE should not be set after initialization");

    switch (s3_selected_language)
    {
        case LANGUAGE_ENGLISH:
            s3_language_resource = ICON_NFC_EN;
            s3_mini_lang_resource = ICON_MINI_EN;
            ESP_LOGI(TAG, "[LANGUAGE] DEBUG ENGLISH: lang=%s, mini=%s", s3_language_resource, s3_mini_lang_resource);
            break;
        case LANGUAGE_CHINESE:
            s3_language_resource = ICON_NFC_CH;
            s3_mini_lang_resource = ICON_MINI_CH;
            ESP_LOGI(TAG, "[LANGUAGE] DEBUG CHINESE: lang=%s, mini=%s", s3_language_resource, s3_mini_lang_resource);
            break;
        case LANGUAGE_ALL:
            s3_language_resource = ICON_NFC_ALL;
            s3_mini_lang_resource = get_mini_language_resource();
            ESP_LOGI(TAG, "[LANGUAGE] DEBUG ALL: lang=%s, mini=%s", s3_language_resource, s3_mini_lang_resource);
            break;
        default:
            ESP_LOGW(TAG, "[LANGUAGE] ui_update_language_data PROBLEM: %d - Forcing CH", s3_selected_language);
            s3_selected_language = LANGUAGE_CHINESE;
            s3_language_resource = ICON_NFC_CH;
            s3_mini_lang_resource = ICON_MINI_CH;
            ESP_LOGI(TAG, "[LANGUAGE] DEBUG DEFAULT: lang=%s, mini=%s", s3_language_resource, s3_mini_lang_resource);
            break;
    }
    ui_save_language();
}

void ui_init_language(void)
{
    esp_err_t err = s3_nvs_get(NVS_S3_DEVICE_NFC_Language, &s3_selected_language);

    if (err != ESP_OK || s3_selected_language == NO_LANGUAGE)
    {
        ESP_LOGW(TAG, "[LANGUAGE] NVS read failed or invalid (%d). Setting default to CHINESE.", err);
        s3_selected_language = LANGUAGE_CHINESE;
        ui_save_language();
    }
    else
        ESP_LOGI(TAG, "[LANGUAGE] ui_init_language loaded: %d", s3_selected_language);

    ui_update_language_data();
}

void ui_change_language(void)
{
    ESP_LOGI(TAG, "[LANGUAGE] ui_change_language change: %d", s3_selected_language);

    if (s3_selected_language == NO_LANGUAGE)
        s3_selected_language = LANGUAGE_CHINESE;
    else
        s3_selected_language = (s3_selected_language + 1) % LANGUAGE_QTD;

    // Only update display for preview - don't save yet
    ui_update_language_data();
}

int ui_get_language(void)
{
    if (s3_selected_language == NO_LANGUAGE)
        ui_init_language();

    return s3_selected_language;
}

void ui_save_language(void)
{
    int32_t lang_org = NO_LANGUAGE;
    s3_nvs_get(NVS_S3_DEVICE_NFC_Language, &lang_org);
    ESP_LOGI(TAG, "[LANGUAGE] ui_save_language: [%d]<->[%d]", s3_selected_language, lang_org);

    if (lang_org != s3_selected_language) {
        s3_nvs_set_cache(NVS_S3_DEVICE_NFC_Language, &s3_selected_language);
    }
}

/**
 * Create an opacity gradient overlay on current screen with predefined positioning
 * Creates multiple overlapping rectangles to simulate opacity gradient effect
 * @param type gradient type (top or bottom)
 * @return pointer to the container gradient object
 */
lv_obj_t * ui_add_gradient_overlay(gradient_type_t type) {
    ESP_LOGI(TAG, "[LVGL] ui_add_gradient_overlay: opacity gradient");

    lv_coord_t x = 0;
    lv_coord_t width = 240;
    lv_coord_t height = 120;
    lv_coord_t y;
    
    // Set Y position based on type
    switch(type) {
        case TOP_GRADIENT:
            y = 0;      // Top section
            break;
        case BOTTOM_GRADIENT:
            y = 120;    // Bottom section (240 - 120 = 120)
            break;
        default:
            return NULL;
    }
    
    // Create container for the gradient
    lv_obj_t * gradient_container = lv_obj_create(lv_scr_act());
    lv_obj_set_size(gradient_container, width, height);
    lv_obj_set_pos(gradient_container, x, y);
    
    // Make container transparent and remove styling
    lv_obj_clear_flag(gradient_container, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_opa(gradient_container, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(gradient_container, 0, 0);
    lv_obj_set_style_outline_width(gradient_container, 0, 0);
    lv_obj_set_style_pad_all(gradient_container, 0, 0);
    
    // Create multiple layers to simulate opacity gradient
    // Use many thin layers for a smooth gradient
    const int num_layers = 40;
    const int layer_height = height / num_layers;  // e.g. 3px per layer for 120px/40
    const lv_opa_t max_opacity = LV_OPA_80;        // target max opacity at the edge (80%)
    const int denom = (num_layers > 1) ? (num_layers - 1) : 1; // avoid div-by-zero, ensure exact endpoints
    
    for (int i = 0; i < num_layers; i++) {
        lv_obj_t *layer = lv_obj_create(gradient_container);
        lv_obj_set_size(layer, width, layer_height);
        
        // Calculate opacity based on gradient type and layer position
        lv_opa_t opacity;
        if (type == TOP_GRADIENT) {
            // Top half: 80% at very top, linearly to 0% at center
            // i=0 (top) -> max_opacity, i=last (near center) -> 0
            opacity = (lv_opa_t)((max_opacity * (denom - i)) / denom);
        } else { // BOTTOM_GRADIENT
            // Bottom half: 0% at center, linearly to 80% at very bottom
            // i=0 (top/center) -> 0, i=last (bottom) -> max_opacity
            opacity = (lv_opa_t)((max_opacity * i) / denom);
        }
        
        // Position layer
        lv_obj_set_pos(layer, 0, i * layer_height);
        
        // Style the layer
        lv_obj_clear_flag(layer, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_style_bg_color(layer, lv_color_hex(LV_CUSTOM_BLACK), 0);  // Black
        lv_obj_set_style_bg_opa(layer, opacity, 0);
        lv_obj_set_style_border_width(layer, 0, 0);
        lv_obj_set_style_outline_width(layer, 0, 0);
        lv_obj_set_style_pad_all(layer, 0, 0);
        lv_obj_set_style_radius(layer, 0, 0);
    }
    
    // Store type in user data for later retrieval
    lv_obj_set_user_data(gradient_container, (void*)(uintptr_t)type);
    
    return gradient_container;
}

void ui_set_battery_level(int pct)
{
    if (pct > 100) pct = 100;

    uint8_t active_bars = (pct + 19) / 20;  // Round up to 5 bars max

    lv_color_t col =
        (pct < 20) ? lv_color_hex(0xFF5555) :
        (pct < 50) ? lv_color_hex(0xF4C242) :
        (pct < 70) ? lv_color_hex(0xA4E439) :
        (pct < 90) ? lv_color_hex(0x44DD44) :
                     lv_color_hex(0x228822);  // dark green

    for (int i = 0; i < 5; ++i) {
        if (i < active_bars) {
            lv_obj_clear_flag(bars[i], LV_OBJ_FLAG_HIDDEN);
            lv_obj_set_style_bg_color(bars[i], col, 0);
        } else {
            lv_obj_add_flag(bars[i], LV_OBJ_FLAG_HIDDEN);
        }
    }
}

static void ui_add_number_badge(void)
{
    ESP_LOGI(TAG, "[LVGL] ui_add_number_badge: displaying track number");

    uint8_t num_track = get_current_track_display_position();
    char num_str[8];

    if (num_track > s3_current_size_track)
        num_track = s3_current_size_track + 1;

    ESP_LOGI(TAG, "[LVGL] ui_add_number_badge: SHOW NUMBER: [%d]", num_track);

    // Hide dots when showing number
    for (int i = 0; i < MAX_DOTS; ++i) {
        if (dot_objs[i]) {
            lv_obj_add_flag(dot_objs[i], LV_OBJ_FLAG_HIDDEN);
        }
    }

    // Find or create number label badge
    lv_obj_t *label = find_badge(CONTENT_TYPE_PLAYER_BADGE, BADGE_SUBTYPE_NUMBER);

    if (!label) {
        // Create new number label
        label = lv_label_create(lv_scr_act());

        // Set user data for number badge - status contains the track number
        uint32_t user_data = make_badge_user_data(
            CONTENT_TYPE_PLAYER_BADGE,
            BADGE_SUBTYPE_NUMBER,
            (uint8_t)num_track  // Store track number in status byte
        );
        lv_obj_set_user_data(label, (void*)(uintptr_t)user_data);

        // Configure label style (only needed on creation)
        lv_obj_set_style_text_font(label, &cherry_bomb_48, 0);
        lv_obj_set_style_text_color(label, lv_color_hex(lv_fnt_color), 0);
        lv_obj_align(label, LV_ALIGN_CENTER, 0, 65); // Y offset for bottom area
    } else {
        // Update user data with new track number
        uint32_t user_data = make_badge_user_data(
            CONTENT_TYPE_PLAYER_BADGE,
            BADGE_SUBTYPE_NUMBER,
            (uint8_t)num_track
        );
        lv_obj_set_user_data(label, (void*)(uintptr_t)user_data);
    }

    // Update text
    snprintf(num_str, sizeof(num_str), "%d", num_track);
    lv_label_set_text(label, num_str);
    lv_obj_move_foreground(label); // Ensure number appears above gradients
}

static void ui_add_dots_badge(bool album_usage)
{
    uint8_t num_dots = 0;
    uint8_t selected_dot = 0;
    const char *use_type = "";

    // Determine dot count and selection
    if (album_usage == USE_ALBUM) {
        use_type = "Album";
        num_dots = s3_current_size;
        selected_dot = s3_current_idx;
        // Limit album dots to 1..9 as per provided coordinates
        if (num_dots > 9) num_dots = 9;
        if (selected_dot >= num_dots) selected_dot = 0;
    } else {
        use_type = "Menu";
        // IDX_MENU_SIZE already excludes Bluetooth (it's commented out in enum)
        // Check if WIFI should be hidden (OOB = 0)
        bool hide_wifi = s3_album_mgr_factory_reset_status();
        
        if (haveNFC()) {
            if (hide_wifi)
                num_dots = IDX_MENU_SIZE - 1;  // 3 dots: VOLUME, BRIGHTNESS, NFC (no WIFI)
            else
                num_dots = IDX_MENU_SIZE;      // 4 dots: VOLUME, WIFI, BRIGHTNESS, NFC
        } else {
            if (hide_wifi)
                num_dots = IDX_MENU_SIZE - 2;  // 2 dots: VOLUME, BRIGHTNESS (no WIFI, no NFC)
            else
                num_dots = IDX_MENU_SIZE - 1;  // 3 dots: VOLUME, WIFI, BRIGHTNESS (no NFC)
        }
        
        // Map s3_menu_idx to actual dot index when WIFI is hidden
        if (hide_wifi) {
            // WIFI (IDX_WIFI=1) is hidden, need to remap indices
            // IDX_VOLUME (0) -> dot 0
            // IDX_BRIGHTNESS (2) -> dot 1
            // IDX_NFC (3) -> dot 2
            if (s3_menu_idx == IDX_VOLUME) {
                selected_dot = 0;
            } else if (s3_menu_idx == IDX_BRIGHTNESS) {
                selected_dot = 1;
            } else if (s3_menu_idx == IDX_NFC) {
                selected_dot = 2;
            } else {
                selected_dot = 0;  // Default fallback
            }
        } else {
            // Normal mapping when WIFI is visible
            selected_dot = s3_menu_idx;
        }
    }

    if (num_dots == 0) {
        ESP_LOGI(TAG, "[LVGL] ui_add_dots_badge: No dots to display");
        return;
    }

    // Check what changed since last call
    bool state_changed = (num_dots != last_num_dots) ||
                         (selected_dot != last_selected_dot) ||
                         (album_usage != last_was_album_usage);

    if (!state_changed) {
        // FAST PATH: Nothing changed, skip all operations
        ESP_LOGD(TAG, "[LVGL] ui_add_dots_badge: FAST PATH - no changes [%s %d/%d]",
                 use_type, selected_dot, num_dots);
        return;
    }

    // Determine optimization path
    bool only_selection_changed = (num_dots == last_num_dots) &&
                                   (selected_dot != last_selected_dot) &&
                                   (album_usage == last_was_album_usage);

    if (only_selection_changed && last_num_dots > 0) {
        // OPTIMIZED PATH: Only selection changed, update 2 dots (old + new selection)
        ESP_LOGI(TAG, "[LVGL] ui_add_dots_badge: OPTIMIZED PATH - selection %d->%d [%s %d/%d]",
                 last_selected_dot, selected_dot, use_type, selected_dot, num_dots);

        const DotPos *positions = get_dot_positions(num_dots);

        // Update previously selected dot (make it small)
        lv_obj_t *old_dot = find_dot_badge(last_selected_dot);
        if (old_dot) {
            lv_obj_set_size(old_dot, 10, 10);
            lv_obj_set_pos(old_dot, positions[last_selected_dot].x, positions[last_selected_dot].y);
        }

        // Update newly selected dot (make it large)
        lv_obj_t *new_dot = find_dot_badge(selected_dot);
        if (new_dot) {
            lv_obj_set_size(new_dot, 18, 18);
            lv_obj_set_pos(new_dot, positions[selected_dot].x - 4, positions[selected_dot].y - 4);
            lv_obj_move_foreground(new_dot);
        }

        // Update state tracking
        last_selected_dot = selected_dot;
        return;
    }

    // FULL PATH: Count changed or first call, rebuild all dots
    ESP_LOGI(TAG, "[LVGL] ui_add_dots_badge: FULL PATH - rebuilding [%s %d/%d]",
             use_type, selected_dot, num_dots);

    const DotPos *positions = get_dot_positions(num_dots);

    // Create or update all dots
    for (int i = 0; i < num_dots; i++) {
        lv_obj_t *dot = find_dot_badge(i);

        if (!dot) {
            // Create new dot with badge tracking
            dot = lv_obj_create(lv_scr_act());
            lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
            lv_obj_set_style_border_width(dot, 0, 0);
            lv_obj_set_style_pad_all(dot, 0, 0);
            lv_obj_set_scrollbar_mode(dot, LV_SCROLLBAR_MODE_OFF);

            // Set user data for badge tracking
            uint32_t user_data = make_dot_user_data(i);
            lv_obj_set_user_data(dot, (void*)(uintptr_t)user_data);

            // Store in dot_objs array for compatibility
            if (i < MAX_DOTS) {
                dot_objs[i] = dot;
            }
        }

        // Configure dot appearance
        lv_obj_clear_flag(dot, LV_OBJ_FLAG_HIDDEN);

        int size = (i == selected_dot) ? 18 : 10;
        lv_obj_set_size(dot, size, size);
        lv_obj_set_style_bg_color(dot, lv_color_hex(LV_CUSTOM_WHITE), 0);
        lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);

        // Position dot (adjust for larger selected dot)
        if (i == selected_dot) {
            lv_obj_set_pos(dot, positions[i].x - 4, positions[i].y - 4);
        } else {
            lv_obj_set_pos(dot, positions[i].x, positions[i].y);
        }

        lv_obj_move_foreground(dot);
    }

    // Hide dots beyond num_dots (if count decreased)
    for (int i = num_dots; i < MAX_DOTS; i++) {
        lv_obj_t *dot = find_dot_badge(i);
        if (dot) {
            lv_obj_add_flag(dot, LV_OBJ_FLAG_HIDDEN);
        }
        // Clear from dot_objs array
        if (i < MAX_DOTS) {
            dot_objs[i] = NULL;
        }
    }

    // Update state tracking
    last_num_dots = num_dots;
    last_selected_dot = selected_dot;
    last_was_album_usage = album_usage;
}

// Helper function to detect language from track path (consistent with audio_player.c)
static int detect_track_language_for_badge(const char* track_path) {
    if (!track_path) {
        return LANGUAGE_CHINESE;  // Default fallback
    }

    // Check for English markers (case-insensitive)
    if (strstr(track_path, "-EN") || strstr(track_path, "_EN") ||
        strstr(track_path, "-en") || strstr(track_path, "_en")) {
        ESP_LOGI(TAG, "[BADGE] Detected ENGLISH from path: %s", track_path);
        return LANGUAGE_ENGLISH;
    }

    // Check for Chinese markers (case-insensitive, support both ZH and CH)
    if (strstr(track_path, "-ZH") || strstr(track_path, "_ZH") ||
        strstr(track_path, "-zh") || strstr(track_path, "_zh") ||
        strstr(track_path, "-CH") || strstr(track_path, "_CH") ||
        strstr(track_path, "-ch") || strstr(track_path, "_ch")) {
        ESP_LOGI(TAG, "[BADGE] Detected CHINESE from path: %s", track_path);
        return LANGUAGE_CHINESE;
    }

    // Default to Chinese if no marker found
    ESP_LOGW(TAG, "[BADGE] No language marker found in path, defaulting to CHINESE: %s", track_path);
    return LANGUAGE_CHINESE;
}

static const char* get_mini_language_resource(void) {
    // Check system language setting first - if user selected LANGUAGE_ALL,
    // we need to detect from current track regardless of album's language field
    if (s3_selected_language == LANGUAGE_ALL) {
        ESP_LOGI(TAG, "[BADGE] System language: ALL - detecting from current track");
        // For LANGUAGE_ALL mode: detect language from current track path
        if (s3_current_track_list && s3_current_idx_track < s3_current_size_track) {
            const char* current_track_path = s3_current_track_list[s3_current_idx_track];
            int detected_lang = detect_track_language_for_badge(current_track_path);
            return (detected_lang == LANGUAGE_ENGLISH) ? ICON_MINI_EN : ICON_MINI_CH;
        }
        ESP_LOGW(TAG, "[BADGE] LANGUAGE_ALL: No track list available, defaulting to CHINESE");
        return ICON_MINI_CH;  // Default fallback for LANGUAGE_ALL
    }

    // For single language mode, use album's language or system language
    if (s3_current_album != NULL) {
        switch (s3_current_album->language) {
            case LANGUAGE_ENGLISH:
                ESP_LOGI(TAG, "[BADGE] Album language: ENGLISH");
                return ICON_MINI_EN;
            case LANGUAGE_CHINESE:
                ESP_LOGI(TAG, "[BADGE] Album language: CHINESE");
                return ICON_MINI_CH;
            case LANGUAGE_ALL:
                // This case should not happen in single language mode,
                // but handle it by detecting from track
                ESP_LOGI(TAG, "[BADGE] Album language: ALL (unexpected) - detecting from current track");
                if (s3_current_track_list && s3_current_idx_track < s3_current_size_track) {
                    const char* current_track_path = s3_current_track_list[s3_current_idx_track];
                    int detected_lang = detect_track_language_for_badge(current_track_path);
                    return (detected_lang == LANGUAGE_ENGLISH) ? ICON_MINI_EN : ICON_MINI_CH;
                }
                ESP_LOGW(TAG, "[BADGE] LANGUAGE_ALL: No track list available, defaulting to CHINESE");
                return ICON_MINI_CH;
            default:
                ESP_LOGW(TAG, "[BADGE] Unknown album language, defaulting to CHINESE");
                return ICON_MINI_CH;  // Default fallback
        }
    } else {
        // Fallback to system language if no current album
        ESP_LOGI(TAG, "[BADGE] No current album, using system language: %d", s3_selected_language);
        switch (s3_selected_language) {
            case LANGUAGE_ENGLISH: return ICON_MINI_EN;
            case LANGUAGE_CHINESE: 
            default: return ICON_MINI_CH;
        }
    }
}

static void ui_add_bt_badge(void)
{
    if (s3_pairing_status == BT_PAIRED) { s3_mini_bt_resource = ICON_MINI_CONN; }
    else if (s3_pairing_status == BT_TIMEOUT) { s3_mini_bt_resource = ICON_MINI_TIME; }
    else 
    {
        ESP_LOGI(TAG, "[LVGL] ui_add_bt_badge: s3_pairing_status=%d, neither bt_timeout or bt_paired, returning.", s3_pairing_status); 
        return;
    }

    ESP_LOGI(TAG, "[LVGL] ui_add_bt_badge: png decode %s (bt_status=%d)", s3_mini_bt_resource, s3_pairing_status);

    // Load Bluetooth icon into dedicated BT badge buffer (PNG)
    if (lvgl_load_content_png(CONTENT_TYPE_BT_BADGE, s3_mini_bt_resource) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to load Bluetooth PNG icon %s", s3_mini_bt_resource);
        return;
    }

    // Get the content descriptor for BT badge
    const lv_img_dsc_t *icon_dsc = lvgl_get_content_dsc(CONTENT_TYPE_BT_BADGE);
    if (!icon_dsc || !icon_dsc->data) {
        ESP_LOGE(TAG, "Bluetooth icon descriptor is NULL or invalid");
        return;
    }

    ESP_LOGI(TAG, "[LVGL] ui_add_bt_badge: PNG icon loaded, size: %dx%d", icon_dsc->header.w, icon_dsc->header.h);

    // Create badge container - use PNG size directly plus padding
    uint16_t side = LV_MAX(icon_dsc->header.w, icon_dsc->header.h) + 0; // 4px padding on each side
    
    lv_obj_t *badge = lv_obj_create(lv_scr_act());
    lv_obj_set_size(badge, side, side);
    lv_obj_clear_flag(badge, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(badge, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_style_radius(badge, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_clip_corner(badge, true, 0);
    
    // Set black background with 25% opacity for Bluetooth
    lv_obj_set_style_bg_color(badge, lv_color_hex(LV_CUSTOM_BLACK), 0);     // Black color for BT
    lv_obj_set_style_bg_opa(badge, ICON_20DIV, 0);                          // 2div for 50% opacity, 4div for 25% and 40div for 5%
    
    lv_obj_set_style_pad_all(badge, 4, 0);
    lv_obj_set_style_border_width(badge, 0, 0);
    lv_obj_set_style_border_opa(badge, LV_OPA_TRANSP, 0);
    
    // Position at 9 o'clock (left side, center vertically)
    lv_obj_align(badge, LV_ALIGN_LEFT_MID, 10, 0);
    lv_obj_move_foreground(badge);

    // Create image object inside badge - use original PNG size (no zoom)
    lv_obj_t *glyph = lv_img_create(badge);
    lv_img_set_src(glyph, icon_dsc);
    lv_obj_center(glyph);  // Center the PNG icon in the badge
}

static void ui_add_batt_badge(int use_animation)
{
    // Don't show battery badge until first battery reading is available
    if (s3_battery_level == BATTERY_UNREAD) {
        ESP_LOGD(TAG, "[LVGL] ui_add_batt_badge: Battery not initialized yet, skipping badge");
        return;
    }

    bool use_png = use_animation == USE_ANIM_PNG ? true : false;
    char *batt_icon = NULL;

    if (s3_charger_status == BATTERY_CHARGE) {
        switch (s3_battery_level) {
            case BATTERY_LEVEL_1: batt_icon = ICON_BATT_1_PNG; break;
            case BATTERY_LEVEL_2: batt_icon = ICON_BATT_2_PNG; break;
            case BATTERY_LEVEL_3: batt_icon = ICON_BATT_3_PNG; break;
            case BATTERY_LEVEL_4: batt_icon = ICON_BATT_4_PNG; break;
            case BATTERY_LEVEL_5: batt_icon = ICON_BATT_5_PNG; break;
            case BATTERY_LEVEL_6: batt_icon = ICON_BATT_6_PNG; break;
            default:              batt_icon = ICON_BATT_0_PNG; break;
        }
    }
    else {
        switch (s3_battery_level) {
            case BATTERY_LEVEL_1: batt_icon = ICON_BATT_NORMAL_1_PNG; break;
            case BATTERY_LEVEL_2: batt_icon = ICON_BATT_NORMAL_2_PNG; break;
            case BATTERY_LEVEL_3: batt_icon = ICON_BATT_NORMAL_3_PNG; break;
            case BATTERY_LEVEL_4: batt_icon = ICON_BATT_NORMAL_4_PNG; break;
            case BATTERY_LEVEL_5: batt_icon = ICON_BATT_NORMAL_5_PNG; break;
            case BATTERY_LEVEL_6: batt_icon = ICON_BATT_NORMAL_6_PNG; break;
            default:              batt_icon = ICON_BATT_NORMAL_0_PNG; break;
        }
    }

    ESP_LOGI(TAG, "[LVGL] ui_add_batt_badge: loading icon %s [%d - %d]", batt_icon, s3_battery_level, s3_battery_percent);

    // Use the appropriate content-specific loader based on file type
    if (use_png) {
        if (lvgl_load_content_png(CONTENT_TYPE_BATTERY_BADGE, batt_icon) != ESP_OK) {
            ESP_LOGE(TAG, "ui_add_batt_badge: cannot load PNG %s", batt_icon);
            return;
        }
    } else {
        if (lvgl_load_content_jpg(CONTENT_TYPE_BATTERY_BADGE, batt_icon) != ESP_OK) {
            ESP_LOGE(TAG, "ui_add_batt_badge: cannot load JPEG %s", batt_icon);
            return;
        }
    }

    // Get the content descriptor for battery badge
    const lv_img_dsc_t *dsc = lvgl_get_content_dsc(CONTENT_TYPE_BATTERY_BADGE);
    if (!dsc || !dsc->data) {
        ESP_LOGE(TAG, "Battery icon descriptor is NULL or invalid");
        return;
    }

    ESP_LOGI(TAG, "[LVGL] ui_add_batt_badge: icon loaded, size: %dx%d", dsc->header.w, dsc->header.h);

    // No frame needed for transparent background
    uint16_t side = LV_MAX(dsc->header.w, dsc->header.h);

    lv_obj_t *badge = lv_obj_create(lv_scr_act());
    lv_obj_set_size(badge, side, side);
    lv_obj_clear_flag(badge, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(badge, LV_SCROLLBAR_MODE_OFF);

    lv_obj_set_style_radius(badge, 0, 0);                   // << square

    lv_obj_set_style_clip_corner(badge, true, 0);           // crop anything outside
    lv_obj_set_style_bg_opa(badge, LV_OPA_TRANSP, 0);       // transparent background
    lv_obj_set_style_pad_all(badge, 0, 0);                  // no padding for transparent
    lv_obj_set_style_border_width(badge, 0, 0);             // no border
    lv_obj_set_style_border_opa(badge, LV_OPA_TRANSP, 0);   // transparent border

    lv_obj_align(badge, LV_ALIGN_TOP_MID, 0, 0);            // position
    lv_obj_move_foreground(badge);

    lv_obj_t *glyph = lv_img_create(badge);
    lv_img_set_src(glyph, dsc);                             // already decoded
    
    lv_obj_center(glyph);
}

static esp_err_t get_current_internal_time(char *hour, char *minute, char *am_pm)
{
    time_t now;
    struct tm timeinfo;
    char strftime_buf[64];

    get_current_time(&now, &timeinfo);

    // Fixed version (Buffer size is 3: '1', '2', '\0')
    strftime(hour,   3, "%I", &timeinfo);
    strftime(minute, 3, "%M", &timeinfo);
    strftime(am_pm,  3, "%p", &timeinfo);

    strftime(strftime_buf, sizeof(strftime_buf), "%c", &timeinfo);

    ESP_LOGI(TAG, "---------------------------------");
    ESP_LOGI(TAG, "Timestamp: %ld", (long)now);
    ESP_LOGI(TAG, "Current time: %s", strftime_buf);
    ESP_LOGI(TAG, "Hour: %s", hour);
    ESP_LOGI(TAG, "Minute: %s", minute);
    ESP_LOGI(TAG, "AM/PM: %s", am_pm);
    ESP_LOGI(TAG, "---------------------------------");
    return ESP_OK;
}

/*
static void ensure_lottie_buf(int width, int height)
{
    if (lottie_buf) {
        ESP_LOGD(TAG, "Lottie buffer already allocated @ %p", lottie_buf);
        return;
    }

    size_t bytes = width * height * 4;  // RGBA

#if CONFIG_SPIRAM_SUPPORT
    ESP_LOGI(TAG, "Allocating %u bytes for Lottie canvas in PSRAM…", (unsigned int)bytes);
    lottie_buf = heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (lottie_buf) {
        ESP_LOGI(TAG, "Lottie canvas placed in PSRAM @ %p", lottie_buf);
    }
#endif

    if (!lottie_buf) {
        ESP_LOGW(TAG, "Falling back to internal RAM for Lottie canvas (%u bytes)", (unsigned int)bytes);
        lottie_buf = heap_caps_malloc(bytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        if (lottie_buf) {
            ESP_LOGI(TAG, "Lottie canvas placed in internal RAM @ %p", lottie_buf);
        } else {
            ESP_LOGE(TAG, "Failed to allocate %u bytes for Lottie canvas!", (unsigned int)bytes);
        }
    }

    assert(lottie_buf && "Out of heap for Lottie canvas");
}
*/

/**
 * @brief Clear shared UI object pointers after screen cleanup.
 *
 * This function must be called right after lv_obj_clean(scr).
 * It ensures that any static/global lv_obj_t* references used across screens
 * are nullified to avoid invalid access to freed memory.
 */
static void clear_static_lv_objects(void) {
    for (int i = 0; i < 5; i++) bars[i] = NULL;
    for (int i = 0; i < MAX_DOTS; i++) dot_objs[i] = NULL;
    for (int i = 0; i < 4; i++) animated_objects[i] = NULL;
    // current_lottie_obj = NULL;

    // Clear language badge objects
    if (lang_badge_timer) {
        lv_timer_del(lang_badge_timer);
        lang_badge_timer = NULL;
    }
    lang_badge_img = NULL;

    // Clear last displayed album tracking
    last_displayed_album = NULL;

    // Clear dot badge state tracking
    last_num_dots = 0;
    last_selected_dot = 0;
    last_was_album_usage = false;

    // Clear data sync screen tracking (for GIF animation)
    data_sync_gif_obj = NULL;
    last_sync_stage = -1;
}

void *lv_malloc(size_t s) {
    // Three-tier allocation strategy to minimize DMA usage:
    // 1. Try PSRAM first (external SPIRAM)
    void *p = heap_caps_malloc(s, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (p) {
        return p;
    }

    // 2. Try internal RAM without DMA capability
    p = heap_caps_malloc(s, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (p) {
        ESP_LOGW(TAG, "lv_malloc using internal RAM (non-DMA)");
        return p;
    }

    // 3. Last resort: DMA-capable internal RAM
    ESP_LOGW(TAG, "lv_malloc fallback to DMA RAM");
    p = heap_caps_malloc(s, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT | MALLOC_CAP_DMA);

    /* Original implementation:
    void *p = heap_caps_malloc(s, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!p) {
        ESP_LOGW(TAG, "lv_malloc fallback to internal RAM");
        p = heap_caps_malloc(s, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT | MALLOC_CAP_DMA);
    }
    */

    return p;
}

void lv_free(void *p) {
    if (p == NULL) {
        return;
    }
    // Check heap integrity to catch corruption early (debug aid)
    heap_caps_check_integrity_all(true);
    heap_caps_free(p);
}

void *lv_realloc(void *p, size_t s) {
    // Three-tier reallocation strategy to minimize DMA usage:
    // 1. Try PSRAM first (external SPIRAM)
    void *new_p = heap_caps_realloc(p, s, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (new_p) {
        return new_p;
    }

    // 2. Try internal RAM without DMA capability
    new_p = heap_caps_realloc(p, s, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (new_p) {
        ESP_LOGW(TAG, "lv_realloc using internal RAM (non-DMA)");
        return new_p;
    }

    // 3. Last resort: DMA-capable internal RAM
    ESP_LOGW(TAG, "lv_realloc fallback to DMA RAM");
    new_p = heap_caps_realloc(p, s, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT | MALLOC_CAP_DMA);

    /* Original implementation:
    void *new_p = heap_caps_realloc(p, s, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!new_p) {
        ESP_LOGW(TAG, "lv_realloc fallback to internal RAM");
        new_p = heap_caps_realloc(p, s, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT | MALLOC_CAP_DMA);
    }
    */

    return new_p;
}

/* Permanent screens */
/**
 * @brief Hybrid implementation of lv_clean_ui with improved styling
 */
lv_obj_t *lv_clean_ui(void)
{
    lv_obj_t *scr = lv_scr_act();                                       // get the active screen
    lv_obj_clean(scr);                                                  // clean the screen
    clear_static_lv_objects();

    // Use proper part specification for future compatibility
    lv_obj_set_style_bg_color(scr, lv_color_hex(lv_bkg_color), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, LV_PART_MAIN);

    lv_obj_t *clean_ui = lv_obj_create(scr);                            // create a new object
    lv_obj_set_size(clean_ui, 240, 240);                                // 240x240 object
    lv_obj_clear_flag(clean_ui, LV_OBJ_FLAG_SCROLLABLE);                // Get the flag
    lv_obj_set_scrollbar_mode(clean_ui, LV_SCROLLBAR_MODE_OFF);         // Remove scrolling
    lv_obj_set_style_bg_color(clean_ui, lv_color_hex(lv_crc_color), LV_PART_MAIN); // black color
    lv_obj_set_style_clip_corner(clean_ui, true, LV_PART_MAIN);                    // clip corners
    lv_obj_set_style_border_width(clean_ui, 0, LV_PART_MAIN);                      // no border
    lv_obj_set_style_border_opa(clean_ui, LV_OPA_TRANSP, LV_PART_MAIN);             // transparent border
    lv_obj_set_style_pad_all(clean_ui, 0, LV_PART_MAIN);                           // no padding
    lv_obj_center(clean_ui);                                            // center it on the screen

    return clean_ui;
}

static void lv_pause_screen(void)
{
    // Check if pause overlay is already drawn to prevent duplicates
    ESP_LOGI(TAG, "Audio is playing, pausing...");
    play_pause();
    restart_dimmer_timer();
    enable_pause_update();

    ESP_LOGI(TAG, "[LVGL] lv_pause_screen: loading paused image from SD card");

    // Use the same approach as lv_animation_ui for consistency
    lvgl_free_previous_buffer();
    lv_obj_t *pause_ui = lv_clean_ui();

    // Load pause image from SD card
    esp_err_t ret = lvgl_load_content_jpg(CONTENT_TYPE_POPUP, ICON_PLAYER_PAUSED);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "lv_pause_screen: Failed to load pause image %s (error: %s)",
                 ICON_PLAYER_PAUSED, esp_err_to_name(ret));
        return;
    }

    // Create and display the pause image
    lv_obj_t *img = lv_img_create(pause_ui);
    lv_img_set_src(img, lvgl_get_content_dsc(CONTENT_TYPE_POPUP));
    lv_obj_center(img);

    // Mark overlay as drawn
    pause_overlay_drawn = true;
}

/**
 * @brief Hybrid implementation of lv_base_ui combining best practices from both versions
 *
 * This implementation uses:
 * - Screen cleaning strategy (Version 1) for better performance
 * - Robust cleanup and style management (Version 2) for stability
 * - Proper part specification (Version 2) for future compatibility
 * - Explicit transparency handling (Version 2) for clarity
 */
lv_obj_t *lv_base_ui(bool use_transparent_bkg)
{
    // FROM VERSION 1: Use screen cleaning for better performance
    lv_obj_t *scr = lv_scr_act();                                       // Get active screen
    lv_obj_clean(scr);                                                  // Clean the active screen
    clear_static_lv_objects();                                          // Clear static objects

    // FROM VERSION 2: Ensure completely clean state for robustness
    lv_obj_remove_style_all(scr);                                       // Remove all styles for clean state

    // FROM VERSION 2: Set background with proper part specification
    lv_obj_set_style_bg_color(scr, lv_color_hex(lv_bkg_color), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, LV_PART_MAIN);

    // FROM VERSION 1: Create main UI container on existing screen
    lv_obj_t *main_ui = lv_obj_create(scr);                             // Create UI on existing screen
    lv_obj_set_size(main_ui, 240, 240);                                 // 240x240 object
    lv_obj_center(main_ui);                                             // Center the container

    // FROM VERSION 2: Style setup with proper part specification
    lv_obj_set_style_radius(main_ui, LV_RADIUS_CIRCLE, LV_PART_MAIN);   // Make it round
    lv_obj_set_style_clip_corner(main_ui, true, LV_PART_MAIN);          // Clip corners
    lv_obj_set_style_bg_color(main_ui, lv_color_hex(lv_crc_color), LV_PART_MAIN); // Set color

    // FROM VERSION 2: Explicit transparency handling for both cases
    if (use_transparent_bkg) {
        lv_obj_set_style_bg_opa(main_ui, LV_OPA_TRANSP, LV_PART_MAIN);  // Transparent background
    } else {
        lv_obj_set_style_bg_opa(main_ui, LV_OPA_COVER, LV_PART_MAIN);   // Opaque background
    }

    // FROM VERSION 2: Border and padding setup with proper part specification
    lv_obj_set_style_border_width(main_ui, 0, LV_PART_MAIN);            // No border
    lv_obj_set_style_border_opa(main_ui, LV_OPA_TRANSP, LV_PART_MAIN);  // Transparent border
    lv_obj_set_style_pad_all(main_ui, 0, LV_PART_MAIN);                 // No padding

    return main_ui;
}

// Keep original implementations for reference and potential fallback
#if 0
// ORIGINAL VERSION 1: Screen cleaning approach
lv_obj_t *lv_base_ui_v1(bool use_transparent_bkg)
{
    lv_obj_t *scr = lv_scr_act();                                       // Recover active screen
    lv_obj_clean(scr);                                                  // Clean the active screen
    clear_static_lv_objects();

    lv_obj_set_style_bg_color(scr, lv_color_hex(lv_bkg_color), 0);      // custom white background
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);                      // fully opaque

    lv_obj_t *main_ui = lv_obj_create(scr);                             // create a new object
    lv_obj_set_size(main_ui, 240, 240);                                 // 240x240 object
    lv_obj_center(main_ui);

    lv_obj_set_style_radius(main_ui, LV_RADIUS_CIRCLE, 0);              // make it round
    lv_obj_set_style_clip_corner(main_ui, true, 0);                     // clip corners
    lv_obj_set_style_bg_color(main_ui, lv_color_hex(lv_crc_color), 0);  // black color
    if (use_transparent_bkg)
    {
        lv_obj_set_style_bg_opa(main_ui, LV_OPA_TRANSP, 0);             // transparent background
    }
    lv_obj_set_style_border_width(main_ui, 0, 0);                       // no border
    lv_obj_set_style_border_opa(main_ui, LV_OPA_TRANSP, 0);             // transparent border
    lv_obj_set_style_pad_all(main_ui, 0, 0);                            // no padding

    lv_obj_center(main_ui);

    return main_ui;
}

// ORIGINAL VERSION 2: New screen creation approach
lv_obj_t *lv_base_ui_v2(bool use_transparent_bkg)
{
    lv_obj_t *new_scr = lv_obj_create(NULL);
    lv_obj_remove_style_all(new_scr);  // 確保乾淨
    lv_scr_load(new_scr);              // 載入新畫面

    // 設定背景色與透明度
    lv_obj_set_style_bg_color(new_scr, lv_color_hex(lv_bkg_color), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(new_scr, LV_OPA_COVER, LV_PART_MAIN);
	// 建立主 UI 容器（圓形框）
    lv_obj_t *main_ui = lv_obj_create(new_scr);
    lv_obj_set_size(main_ui, 240, 240);
    lv_obj_center(main_ui);
    lv_obj_set_style_radius(main_ui, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_clip_corner(main_ui, true, 0);
    lv_obj_set_style_bg_color(main_ui, lv_color_hex(lv_crc_color), 0);
    if (use_transparent_bkg)
    {
        lv_obj_set_style_bg_opa(main_ui, LV_OPA_TRANSP, 0);
    }
    else
    {
        lv_obj_set_style_bg_opa(main_ui, LV_OPA_COVER, 0);
    }
    lv_obj_set_style_border_width(main_ui, 0, 0);
    lv_obj_set_style_border_opa(main_ui, LV_OPA_TRANSP, 0);
    lv_obj_set_style_pad_all(main_ui, 0, 0);
    return main_ui;
}
#endif

/**
 * @brief Hybrid implementation of lv_menu_ui - clean and efficient
 */
lv_obj_t *lv_menu_ui(char *resource)
{
    lvgl_free_previous_buffer();
	lv_img_cache_set_size(0);
    ESP_LOGI(TAG, "[ * ] [LVGL] lv_menu_ui: jpg decode %s", resource);
    esp_err_t ret = lvgl_load_content_jpg(CONTENT_TYPE_MENU, resource);

    lv_obj_t *menu_ui = lv_base_ui(USE_TRANSPARENCY);

    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Failed to load menu JPEG %s (error: %s), showing blank screen",
                 resource, esp_err_to_name(ret));
        // Return the base UI without image - graceful degradation
        return menu_ui;
    }

    lv_obj_t *img = lv_img_create(menu_ui);
    lv_img_set_src(img, lvgl_get_content_dsc(CONTENT_TYPE_MENU));
    lv_obj_center(img);

    return menu_ui;
}

/**
 * @brief Hybrid implementation of lv_animation_ui - handles both GIF and JPG
 * NOTE: Logic inverted - s3_use_animations true = GIF, false = JPG
 */
lv_obj_t *lv_animation_ui(char *animation, int use_animation)
{
    lvgl_free_previous_buffer();
    lv_obj_t *animation_ui = lv_clean_ui();

    if (use_animation == USE_ANIM_GIF)
    {
        ESP_LOGI(TAG, "[ * ] [LVGL] lv_animation_ui: gif decode %s", animation);
        esp_err_t ret = lvgl_load_gif_from_sdcard(animation);
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "Failed to load GIF %s (error: %s), skipping animation",
                     animation, esp_err_to_name(ret));
            // Return the container without animation - graceful degradation
            return animation_ui;
        }
        ESP_LOG_BUFFER_HEX(TAG, animation, strlen(animation));

        lv_obj_t *gif = lv_gif_create(animation_ui);
        if (!lvgl_validate_gif_dsc(lvgl_get_gif())) {
            ESP_LOGE("GIF", "Refusing to load invalid gif_dsc");
            return animation_ui;  // Return container without GIF
        }
        lv_gif_set_src(gif, lvgl_get_gif());
        lv_obj_center(gif);
    } else
    {
        ESP_LOGI(TAG, "[ * ] [LVGL] lv_animation_ui: jpg decode %s", animation);
        esp_err_t ret = lvgl_load_content_jpg(CONTENT_TYPE_POPUP, animation);
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "Failed to load animation JPEG %s (error: %s), skipping animation",
                     animation, esp_err_to_name(ret));
            // Return the container without image - graceful degradation
            return animation_ui;
        }

        lv_obj_t *img = lv_img_create(animation_ui);
        lv_img_set_src(img, lvgl_get_content_dsc(CONTENT_TYPE_POPUP));
        lv_obj_center(img);
    }

    return animation_ui;
}

/**
 * @brief Hybrid implementation of lv_battery_draw_ui with proper part specification
 */
lv_obj_t *lv_battery_draw_ui(lv_obj_t *parent)
{
    lv_obj_t *battery = lv_obj_create(parent);                          // Create the battery container
    lv_obj_set_size(battery, 80, 45);                                   // Set size of the battery
    lv_obj_set_style_radius(battery, 4, LV_PART_MAIN);                  // Rounded corners
    lv_obj_set_style_bg_opa(battery, LV_OPA_TRANSP, LV_PART_MAIN);      // Transparent background
    lv_obj_set_style_border_width(battery, 4, LV_PART_MAIN);            // Border width
    lv_obj_set_style_border_color(battery, lv_color_white(), LV_PART_MAIN); // White border
    lv_obj_center(battery);                                             // Center the battery on the parent
    lv_obj_set_scrollbar_mode(battery, LV_SCROLLBAR_MODE_OFF);          // Disable scrollbar
    lv_obj_set_style_pad_all(battery, 0, LV_PART_MAIN);

    int bar_width = 10;                                                 // Width of each battery bar
    int bar_height = 30;                                                // Height of each battery bar
    int gap = 4;                                                        // Gap between battery bars
    int offset_x = 4;                                                   // X offset for the first bar
    int offset_y = 4;                                                   // Y offset for the first bar

    for (int i = 0; i < 5; ++i) {
        bars[i] = lv_obj_create(battery);                               // Create each bar inside the battery container
        lv_obj_set_size(bars[i], bar_width, bar_height);                // Set size of each bar
        lv_obj_set_style_bg_opa(bars[i], LV_OPA_COVER, LV_PART_MAIN);   // Fully opaque
        lv_obj_set_style_border_width(bars[i], 0, LV_PART_MAIN);        // No border
        lv_obj_set_style_pad_all(bars[i], 0, LV_PART_MAIN);             // No padding
        lv_obj_set_style_radius(bars[i], 1, LV_PART_MAIN);              // Slightly rounded corners
        lv_obj_set_pos(bars[i], offset_x + i * (bar_width + gap), offset_y);    // Position each bar with offset and gap
    }

    lv_obj_t *terminal = lv_obj_create(parent);                                 // Create a terminal icon
    lv_obj_set_size(terminal, 8, 20);                                           // Set size of the terminal icon
    lv_obj_set_style_bg_color(terminal, lv_color_white(), LV_PART_MAIN);        // White color
    lv_obj_set_style_bg_opa(terminal, LV_OPA_COVER, LV_PART_MAIN);              // Fully opaque
    lv_obj_set_style_border_width(terminal, 0, LV_PART_MAIN);                   // No border
    lv_obj_align_to(terminal, battery, LV_ALIGN_OUT_RIGHT_MID, 2, 0);           // Align to the right of the battery

    lv_obj_t *bolt = lv_label_create(parent);                                   // Create a lightning bolt icon
    lv_label_set_text(bolt, LV_SYMBOL_CHARGE);                                  // Set the lightning bolt symbol
    lv_obj_set_style_text_color(bolt, lv_color_hex(0x88CCCC), LV_PART_MAIN);    // Light blue color
    lv_obj_align_to(bolt, battery, LV_ALIGN_CENTER, 0, 0);                      // Center it on the battery

    return battery;
}

/**
 * @brief Hybrid implementation of lv_battery_icon_ui - loads and displays battery icon
 */
lv_obj_t *lv_battery_icon_ui(lv_obj_t *parent, int value, bool size_2x)
{
    const char *batt_icon = ICON_BATT_0_JPG;
    switch (value) {
        case BATTERY_LEVEL_0:   batt_icon = ICON_BATT_0_JPG; break;
        case BATTERY_LEVEL_1:   batt_icon = ICON_BATT_1_JPG; break;
        case BATTERY_LEVEL_2:   batt_icon = ICON_BATT_2_JPG; break;
        case BATTERY_LEVEL_3:   batt_icon = ICON_BATT_3_JPG; break;
        case BATTERY_LEVEL_4:   batt_icon = ICON_BATT_4_JPG; break;
        case BATTERY_LEVEL_5:   batt_icon = ICON_BATT_5_JPG; break;
        case BATTERY_LEVEL_6:   batt_icon = ICON_BATT_6_JPG; break;
        default:                batt_icon = ICON_BATT_0_JPG; break;
    }

    ESP_LOGI(TAG, "[LVGL] lv_battery_icon_ui: jpg decode %s", batt_icon);
    esp_err_t ret = lvgl_load_image_from_sdcard(batt_icon);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Failed to load battery icon %s (error: %s), using default",
                 batt_icon, esp_err_to_name(ret));
        return NULL; // Return NULL to indicate failure
    }

    const lv_img_dsc_t *img_dsc = lvgl_get_img();
    ESP_LOGI(TAG, "[DEBUG] Battery charge image size: %dx%d pixels, data size: %u bytes",
             img_dsc->header.w, img_dsc->header.h, (unsigned int)img_dsc->data_size);

    lv_obj_t *img = lv_img_create(parent);
    lv_img_set_src(img, img_dsc);

    if (size_2x) {
        lv_img_set_zoom(img, ICON_2MUL);            // 256 == 100 %, 200 ≈ 78 %
    }

    lv_obj_center(img);

    return img;
}

// Animated screens   ////////////////////////////////////////////////////////////////////
// Revised - done
void lv_boot_animation(int use_animation)
{
    const char *boot_resource;
    if (use_animation == USE_ANIM_GIF) { boot_resource = ANIM_POWER_ON; }
    else { boot_resource = ICON_POWER_ON; }

    ESP_LOGI(TAG, "[ * ] [LVGL] lv_boot_screen: loading static %s [%d]", boot_resource, use_animation);
    lv_bkg_color = LV_CUSTOM_BLACK;
    lv_crc_color = LV_CUSTOM_WHITE;
    lv_fnt_color = LV_CUSTOM_BLACK;
    lv_dot_color = LV_CUSTOM_BLACK;

    if (use_animation == USE_ANIM_GIF) {
        lv_obj_t *local_ui = lv_animation_ui(boot_resource, use_animation);
    } else {
        lv_obj_t *ui = lv_clean_ui();
        ESP_LOGI(TAG, "[ * ] [LVGL] Reusing boot JPG buffer (no reload)");
        esp_err_t ret = lvgl_load_content_jpg(CONTENT_TYPE_POPUP, boot_resource);
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "Failed to load animation JPEG %s (error: %s), skipping animation",
                     boot_resource, esp_err_to_name(ret));
            // Return the container without image - graceful degradation
        }

        lv_obj_t *img = lv_img_create(ui);
        lv_img_set_src(img, lvgl_get_content_dsc(CONTENT_TYPE_POPUP));
        lv_obj_center(img);
    }
}

// Revised - done
void lv_alarm_animation(int use_animation)
{
    ESP_LOGI(TAG, "[ * ] [LVGL] lv_alarm_screen: loading static %s", s3_current_alarm->cover);
    lv_bkg_color = LV_CUSTOM_BLACK;
    lv_crc_color = LV_CUSTOM_WHITE;
    lv_fnt_color = LV_CUSTOM_BLACK;
    lv_dot_color = LV_CUSTOM_BLACK;

    lv_obj_t *local_ui = lv_animation_ui(s3_current_alarm->cover, use_animation);
}

/*
// Revised - alternative
void lv_alarm_animation_lottie(void)
{
    lvgl_free_previous_buffer();
    ESP_LOGW(TAG, "Loading Lottie animation [ALARM_SCREEN]");

    // Reset monitoring variables
    lottie_render_count = 0;
    lottie_corruption_detected = false;
    current_lottie_obj = NULL;

    // Clean up any existing watchdog
    if (lottie_watchdog_timer) {
        lv_timer_del(lottie_watchdog_timer);
        lottie_watchdog_timer = NULL;
    }

    // Check if resource is valid
    if (!s3_recover.resource) {
        ESP_LOGE(TAG, "s3_recover.resource is NULL!");
        return;
    }

    ESP_LOGI(TAG, "[ * ] [LVGL] lv_alarm_screen: loads from spiff %s", s3_recover.resource);
    lv_bkg_color = LV_CUSTOM_BLACK;
    lv_crc_color = LV_CUSTOM_WHITE;
    lv_fnt_color = LV_CUSTOM_BLACK;
    lv_dot_color = LV_CUSTOM_BLACK;

    lv_obj_t *local_ui = lv_clean_ui();

    // Only log hex buffer if resource is valid
    if (s3_recover.resource && strlen(s3_recover.resource) > 0) {
        ESP_LOG_BUFFER_HEX(TAG, s3_recover.resource, strlen(s3_recover.resource));
    }

    // Use 30x30 size - now trying PSRAM for canvas buffer
    lv_obj_t *lottie = lvgl_load_lottie_from_sdcard(s3_recover.resource, 30, 30);
    if (!lottie) {
        ESP_LOGE(TAG, "Failed to load Lottie animation from %s", s3_recover.resource ? s3_recover.resource : "NULL");

        // Create a fallback static screen
        lv_obj_t *label = lv_label_create(local_ui);
        lv_label_set_text(label, "Charging...");
        lv_obj_set_style_text_color(label, lv_color_hex(lv_fnt_color), 0);
        lv_obj_center(label);
        return;
    }

    // Center the small animation object inside the local UI
    lv_obj_set_parent(lottie, local_ui);  // Move under local_ui for correct layering
    lv_obj_center(lottie);

    // Set up monitoring
    current_lottie_obj = lottie;

    // Create watchdog timer to monitor Lottie health
    lottie_watchdog_timer = lv_timer_create(lottie_watchdog_cb, 500, NULL);  // Check every 500ms
    if (lottie_watchdog_timer) {
        ESP_LOGI(TAG, "Lottie watchdog timer created");
    } else {
        ESP_LOGW(TAG, "Failed to create Lottie watchdog timer");
    }

    ESP_LOGI(TAG, "Lottie animation setup complete with monitoring");
}
*/

// Revised - done
void lv_shutdown_animation(int use_animation)
{
    ESP_LOGI(TAG, "[ * ] [LVGL] lv_shutdown_screen: loading static %s", s3_recover.resource);
    lv_bkg_color = LV_CUSTOM_BLACK;
    lv_crc_color = LV_CUSTOM_WHITE;
    lv_fnt_color = LV_CUSTOM_BLACK;
    lv_dot_color = LV_CUSTOM_BLACK;

    lv_obj_t *local_ui = lv_animation_ui(s3_recover.resource, use_animation);
}

void lv_volume_screen(int use_animation)
{
    ESP_LOGI(TAG, "[ * ] [LVGL] lv_volume_screen: loading static %s", s3_recover.resource);
    lv_bkg_color = LV_CUSTOM_BLACK;
    lv_crc_color = LV_CUSTOM_WHITE;
    lv_fnt_color = LV_CUSTOM_BLACK;
    lv_dot_color = LV_CUSTOM_BLACK;
    lv_obj_t *local_ui = lv_animation_ui(s3_recover.resource, use_animation);

    s3_menu_idx = IDX_VOLUME;
    ui_add_dots_badge(USE_MENU);
}

void lv_wifi_sync_mai_screen(int use_animation)
{
    ESP_LOGI(TAG, "[ * ] [LVGL] lv_wifi_sync_mai_screen: loading static %s", s3_recover.resource);
    lv_bkg_color = LV_CUSTOM_BLACK;
    lv_crc_color = LV_CUSTOM_WHITE;
    lv_fnt_color = LV_CUSTOM_BLACK;
    lv_dot_color = LV_CUSTOM_BLACK;
    lv_obj_t *local_ui = lv_animation_ui(s3_recover.resource, use_animation);

    s3_menu_idx = IDX_WIFI;
    ui_add_dots_badge(USE_MENU);
}

void lv_wifi_sync_err_screen(int use_animation)
{
    ESP_LOGI(TAG, "[ * ] [LVGL] lv_wifi_sync_err_screen: loading static %s", s3_recover.resource);
    lv_bkg_color = LV_CUSTOM_BLACK;
    lv_crc_color = LV_CUSTOM_WHITE;
    lv_fnt_color = LV_CUSTOM_BLACK;
    lv_dot_color = LV_CUSTOM_BLACK;
    lv_obj_t *local_ui = lv_animation_ui(s3_recover.resource, use_animation);
}

void lv_wifi_sync_suc_screen(int use_animation)
{
    ESP_LOGI(TAG, "[ * ] [LVGL] lv_wifi_sync_suc_screen: loading static %s", s3_recover.resource);
    lv_bkg_color = LV_CUSTOM_BLACK;
    lv_crc_color = LV_CUSTOM_WHITE;
    lv_fnt_color = LV_CUSTOM_BLACK;
    lv_dot_color = LV_CUSTOM_BLACK;
    lv_obj_t *local_ui = lv_animation_ui(s3_recover.resource, use_animation);
}

void lv_wifi_sync_n_screen(int use_animation)
{
    ESP_LOGI(TAG, "[ * ] [LVGL] lv_wifi_sync_n_screen: loading static %s", s3_recover.resource);
    lv_bkg_color = LV_CUSTOM_BLACK;
    lv_crc_color = LV_CUSTOM_WHITE;
    lv_fnt_color = LV_CUSTOM_BLACK;
    lv_dot_color = LV_CUSTOM_BLACK;
    lv_obj_t *local_ui = lv_animation_ui(s3_recover.resource, use_animation);
}

void lv_wifi_err_screen(int use_animation)
{
    ESP_LOGI(TAG, "[ * ] [LVGL] lv_wifi_err_screen: loading static %s", s3_recover.resource);
    lv_bkg_color = LV_CUSTOM_BLACK;
    lv_crc_color = LV_CUSTOM_WHITE;
    lv_fnt_color = LV_CUSTOM_BLACK;
    lv_dot_color = LV_CUSTOM_BLACK;
    lv_obj_t *local_ui = lv_animation_ui(s3_recover.resource, use_animation);
}
// Revised - done
void lv_volume_up_animation(int use_animation)
{
    static const char *volume_resource;
    if (s3_volume_level == VOLUME_LEVEL_1) { volume_resource = use_animation == USE_ANIM_GIF ? ANIM_VOLUME_1 : ICON_VOLUME_1; }
    else if (s3_volume_level == VOLUME_LEVEL_2) { volume_resource = use_animation == USE_ANIM_GIF ? ANIM_VOLUME_2 : ICON_VOLUME_2; }
    else if (s3_volume_level == VOLUME_LEVEL_3) { volume_resource = use_animation == USE_ANIM_GIF ? ANIM_VOLUME_3 : ICON_VOLUME_3; }
    else if (s3_volume_level == VOLUME_LEVEL_4) { volume_resource = use_animation == USE_ANIM_GIF ? ANIM_VOLUME_4 : ICON_VOLUME_4; }
    else if (s3_volume_level == VOLUME_LEVEL_5) { volume_resource = use_animation == USE_ANIM_GIF ? ANIM_VOLUME_5 : ICON_VOLUME_5; }
    else if (s3_volume_level == VOLUME_LEVEL_6) { volume_resource = use_animation == USE_ANIM_GIF ? ANIM_VOLUME_6 : ICON_VOLUME_6; }

    ESP_LOGI(TAG, "[ * ] [LVGL] lv_volume_up_animation: loading static %s [%d]", volume_resource, s3_volume_level);
    lv_bkg_color = LV_CUSTOM_BLACK;
    lv_crc_color = LV_CUSTOM_WHITE;
    lv_fnt_color = LV_CUSTOM_BLACK;
    lv_dot_color = LV_CUSTOM_BLACK;

    lv_obj_t *local_ui = lv_animation_ui(volume_resource, use_animation);
}

void lv_volume_down_animation(int use_animation)
{
    static const char *volume_resource;
    if (s3_volume_level == VOLUME_LEVEL_1) { volume_resource = use_animation == USE_ANIM_GIF ? ANIM_VOLUME_1 : ICON_VOLUME_1; }
    else if (s3_volume_level == VOLUME_LEVEL_2) { volume_resource = use_animation == USE_ANIM_GIF ? ANIM_VOLUME_2 : ICON_VOLUME_2; }
    else if (s3_volume_level == VOLUME_LEVEL_3) { volume_resource = use_animation == USE_ANIM_GIF ? ANIM_VOLUME_3 : ICON_VOLUME_3; }
    else if (s3_volume_level == VOLUME_LEVEL_4) { volume_resource = use_animation == USE_ANIM_GIF ? ANIM_VOLUME_4 : ICON_VOLUME_4; }
    else if (s3_volume_level == VOLUME_LEVEL_5) { volume_resource = use_animation == USE_ANIM_GIF ? ANIM_VOLUME_5 : ICON_VOLUME_5; }
    else if (s3_volume_level == VOLUME_LEVEL_6) { volume_resource = use_animation == USE_ANIM_GIF ? ANIM_VOLUME_6 : ICON_VOLUME_6; }

    ESP_LOGI(TAG, "[ * ] [LVGL] lv_volume_down_animation: loading static %s [%d]", volume_resource, s3_volume_level);
    lv_bkg_color = LV_CUSTOM_BLACK;
    lv_crc_color = LV_CUSTOM_WHITE;
    lv_fnt_color = LV_CUSTOM_BLACK;
    lv_dot_color = LV_CUSTOM_BLACK;

    lv_obj_t *local_ui = lv_animation_ui(volume_resource, use_animation);
}

// Revised - done
void lv_bright_up_animation(int use_animation)
{
    // Display current brightness level (no logic changes here)
    ESP_LOGI(TAG, "Bright up animation: displaying brightness level %d", s3_brightness_level);

    static const char *brightnesss_resource;
    if (s3_brightness_level == BRIGHTNESS_LEVEL_1) { brightnesss_resource = use_animation == USE_ANIM_GIF ? ANIM_BRIGHT_1 : ICON_BRIGHT_1; }
    else if (s3_brightness_level == BRIGHTNESS_LEVEL_2) { brightnesss_resource = use_animation == USE_ANIM_GIF ? ANIM_BRIGHT_2 : ICON_BRIGHT_2; }
    else if (s3_brightness_level == BRIGHTNESS_LEVEL_3) { brightnesss_resource = use_animation == USE_ANIM_GIF ? ANIM_BRIGHT_3 : ICON_BRIGHT_3; }

    ESP_LOGI(TAG, "[ * ] [LVGL] lv_display_screen: loading static %s", brightnesss_resource);
    lv_bkg_color = LV_CUSTOM_BLACK;
    lv_crc_color = LV_CUSTOM_WHITE;
    lv_fnt_color = LV_CUSTOM_BLACK;
    lv_dot_color = LV_CUSTOM_BLACK;

    lv_obj_t *local_ui = lv_animation_ui(brightnesss_resource, use_animation);
}

void lv_bright_down_animation(int use_animation)
{
    // Display current brightness level (no logic changes here)
    ESP_LOGI(TAG, "Bright down animation: displaying brightness level %d", s3_brightness_level);

    static const char *brightnesss_resource;
    if (s3_brightness_level == BRIGHTNESS_LEVEL_1) { brightnesss_resource = use_animation == USE_ANIM_GIF ? ANIM_BRIGHT_1 : ICON_BRIGHT_1; }
    else if (s3_brightness_level == BRIGHTNESS_LEVEL_2) { brightnesss_resource = use_animation == USE_ANIM_GIF ? ANIM_BRIGHT_2 : ICON_BRIGHT_2; }
    else if (s3_brightness_level == BRIGHTNESS_LEVEL_3) { brightnesss_resource = use_animation == USE_ANIM_GIF ? ANIM_BRIGHT_3 : ICON_BRIGHT_3; }

    ESP_LOGI(TAG, "[ * ] [LVGL] lv_display_screen: loading static %s", brightnesss_resource);
    lv_bkg_color = LV_CUSTOM_BLACK;
    lv_crc_color = LV_CUSTOM_WHITE;
    lv_fnt_color = LV_CUSTOM_BLACK;
    lv_dot_color = LV_CUSTOM_BLACK;

    lv_obj_t *local_ui = lv_animation_ui(brightnesss_resource, use_animation);
}

// Version to review
void lv_ota_screen(int use_animation)
{
    ESP_LOGI(TAG, "[ * ] [LVGL] lv_ota_screen: loading static %s", s3_recover.resource);
    lv_bkg_color = LV_CUSTOM_BLACK;
    lv_crc_color = LV_CUSTOM_WHITE;
    lv_fnt_color = LV_CUSTOM_BLACK;
    lv_dot_color = LV_CUSTOM_BLACK;

    lv_obj_t *local_ui = lv_animation_ui(s3_recover.resource, use_animation);
}

// Normal screens   ///////////////////////////////////////////////////////////////////////
// Revised - OK
void lv_power_screen(int use_animation)
{
    int batt = NULL;
    char *msg = NULL;
    char *icon = NULL;
    char *anim = NULL;
    int batt_value = 0;
    char bat_msg[8];

    int msg_type = get_current_screen();
    ESP_LOGI(TAG, "[LVGL] lv_power_screen: %i", msg_type);

    lv_bkg_color = LV_CUSTOM_GRAY;
    lv_crc_color = LV_CUSTOM_BLACK;
    lv_fnt_color = LV_CUSTOM_YELLOW;
    lv_dot_color = LV_CUSTOM_BLACK;

    switch (msg_type)
    {
    case POWER_LOW_SCREEN:
        msg = "LowPower Popup";
        batt = BATTERY_LEVEL_0;
        icon = ICON_BATT_LOW;
        anim = ANIM_BATT_LOW;
        break;
    case POWER_FULL_SCREEN:
        msg = "Battery charged Popup";
        batt = BATTERY_LEVEL_6;
        icon = ICON_BATT_FULL;
        anim = ANIM_BATT_FULL;
        break;
    default:  // Or CHARGING_SCREEN
        msg = "Charging Popup";
        batt = BATTERY_LEVEL_2;
        icon = ICON_BATT_CHRG;
        anim = ANIM_BATT_CHRG;
        break;
    }
    if (s3_battery_percent > 5 && s3_battery_percent <= 10)
        batt_value = 10;
    else if (s3_battery_percent > 1 && s3_battery_percent <= 5)
        batt_value = 5;
    else
    batt_value = s3_battery_percent; // should 1 or 0

    lv_snprintf(bat_msg, sizeof(bat_msg), "%d", batt_value);

    switch (use_animation) {
        case USE_ANIM_JPG:
            ESP_LOGI(TAG, "[LVGL] lv_power_screen: DUCKLING_STATIC");

            lv_obj_t *battery1 = lv_animation_ui(icon, use_animation);

            if (msg_type == POWER_LOW_SCREEN)
            {
                lv_obj_t *val_lbl = lv_label_create(battery1);
                lv_label_set_text           (val_lbl, bat_msg);
                lv_obj_set_style_text_font  (val_lbl, &cherry_bomb_48, 0);
                lv_obj_set_style_text_color (val_lbl, lv_color_hex(lv_fnt_color), 0);
                lv_obj_align                (val_lbl, LV_ALIGN_CENTER, 0, -10);
            }
            break;
        case USE_ANIM_GIF:
            ESP_LOGI(TAG, "[LVGL] lv_power_screen: DUCKLING_GIF_ANIMATION");

            lv_obj_t *battery2 = lv_animation_ui(anim, use_animation);
            break;
        default:
            ESP_LOGI(TAG, "[LVGL] lv_power_screen: BATTERY_ICON");
            lv_obj_t *local_ui = lv_base_ui(NO_TRANSPARENCY);
            lv_obj_t *battery3  = lv_battery_icon_ui(local_ui, batt, BATTERY_LARGE);

            lv_obj_t *label = lv_label_create(local_ui);
            lv_label_set_text(label, msg);
            lv_obj_set_style_text_color(label, lv_color_hex(lv_fnt_color), 0);
            lv_obj_align_to(label, battery3, LV_ALIGN_OUT_BOTTOM_MID, 0, 20);
            break;
    }
}

// Placeholder
void lv_poweroff_screen(void)
{
    ESP_LOGI(TAG, "[ * ] [LVGL] lv_poweroff_screen: Static");
    lv_bkg_color = LV_CUSTOM_GRAY;
    lv_crc_color = LV_CUSTOM_BLACK;
    lv_fnt_color = LV_CUSTOM_WHITE;
    lv_dot_color = LV_CUSTOM_WHITE;

    lv_obj_t *local_ui = lv_base_ui(NO_TRANSPARENCY);

    lv_obj_t *label = lv_label_create(local_ui);
    lv_label_set_text(label, "Power off");
    lv_obj_set_style_text_color(label, lv_color_hex(lv_fnt_color), 0);
    lv_obj_center(label);
}

// Placeholder
void lv_off_screen(void)
{
    ESP_LOGI(TAG, "[ * ] [LVGL] lv_off_screen: Static");
    lv_bkg_color = LV_CUSTOM_BLACK;
    lv_crc_color = LV_CUSTOM_BLACK;
    lv_dot_color = LV_CUSTOM_BLACK;
    lv_fnt_color = LV_CUSTOM_GRAY;

    lv_obj_t *local_ui = lv_clean_ui();
    last_displayed_home_album = NULL;
    last_displayed_album = NULL;
}

// Version to review
void lv_clock_screen(void)
{
    ESP_LOGI(TAG, "[ * ] lv_clock_screen");

    const int time_space = 35; // Space between hour and minutes labels
    /* ── 1. fetch time ───────────────────────────────────── */
    char hh[3], mm[3], ap[3];
    get_current_internal_time(hh, mm, ap);
    bool is_pm = (strcmp(ap, "PM") == 0);

    // FOR DEMO (use only for all numbers exibition):
    // uint8_t hour_val = esp_random() % 12; // 0-11
    // uint8_t min_val  = esp_random() % 60; // 0-59
    // bool    is_pm    = (esp_random() & 1) == 1;
    // snprintf(hh, sizeof(hh), "%02u", hour_val);
    // snprintf(mm, sizeof(mm), "%02u", min_val);

    /* ── 2. palette (unchanged) ──────────────────────────────────────────── */
    lv_bkg_color = LV_CUSTOM_GRAY;
    lv_crc_color = LV_CUSTOM_BLACK;
    lv_fnt_color = LV_CUSTOM_YELLOW;
    lv_dot_color = LV_CUSTOM_BLACK;

    /* ── 3. root container — full screen ──────────────────────────────── */
    lv_obj_t *root = lv_clean_ui();
    lv_obj_set_size(root, LV_PCT(100), LV_PCT(100));

    /* Helper: vertical offset calculations for proper alignment        */
    const int BIG_LH   = lv_font_get_line_height(&cherry_bomb_90);
    const int SMALL_LH = lv_font_get_line_height(&cherry_bomb_24);
    const int AM_OFFSET = BIG_LH - SMALL_LH;     /* align AM to bottom of hour */
    const int PM_OFFSET = 0;                     /* align PM to top of minutes */

    /* ============= HOUR (centered on screen) ============================ */
    lv_obj_t *lbl_h = lv_label_create(root);
    lv_label_set_text(lbl_h, hh);
    lv_obj_set_style_text_font(lbl_h, &cherry_bomb_90, 0);
    lv_obj_set_style_text_color(lbl_h, lv_color_hex(lv_fnt_color), 0);
    lv_obj_align(lbl_h, LV_ALIGN_CENTER, 0, -time_space);  // Above center with more space

    /* ============= AM (right aligned to bottom of hour) ================= */
    lv_obj_t *lbl_am = lv_label_create(root);
    lv_label_set_text(lbl_am, "am");
    lv_obj_set_style_text_font(lbl_am, &cherry_bomb_24, 0);
    lv_obj_set_style_text_color(lbl_am,
        is_pm ? lv_color_hex(lv_dot_color) : lv_color_hex(lv_fnt_color), 0);
    lv_obj_align_to(lbl_am, lbl_h, LV_ALIGN_OUT_RIGHT_TOP, 10, AM_OFFSET);

    /* ============= MINUTES (centered on screen) ========================= */
    lv_obj_t *lbl_m = lv_label_create(root);
    lv_label_set_text(lbl_m, mm);
    lv_obj_set_style_text_font(lbl_m, &cherry_bomb_90, 0);
    lv_obj_set_style_text_color(lbl_m, lv_color_hex(lv_fnt_color), 0);
    lv_obj_align(lbl_m, LV_ALIGN_CENTER, 0, time_space);   // Below center with more space

    /* ============= PM (right aligned to top of minutes) ================= */
    lv_obj_t *lbl_pm = lv_label_create(root);
    lv_label_set_text(lbl_pm, "pm");
    lv_obj_set_style_text_font(lbl_pm, &cherry_bomb_24, 0);
    lv_obj_set_style_text_color(lbl_pm,
        is_pm ? lv_color_hex(lv_fnt_color) : lv_color_hex(lv_dot_color), 0);
    lv_obj_align_to(lbl_pm, lbl_m, LV_ALIGN_OUT_RIGHT_TOP, 10, PM_OFFSET);

    /* ── 4. battery badge when charging ─────────────────────────────────── */
    ui_add_batt_badge(USE_ANIM_PNG);
}

// Revised - half term
void lv_home_screen(bool renew)
{

    lv_bkg_color = LV_CUSTOM_GRAY;
    lv_crc_color = LV_CUSTOM_BLACK;
    lv_fnt_color = LV_CUSTOM_WHITE;
    lv_dot_color = LV_CUSTOM_WHITE;

    ESP_LOGI(TAG, "[ * ] [LVGL] lv_home_screen: Album cover %s", s3_current_album ? s3_current_album->home_cover : "NULL");
    lv_obj_t *cover_ui = NULL;

    // Home screen does not have number badge.
    lv_obj_t *number = find_badge(CONTENT_TYPE_PLAYER_BADGE, BADGE_SUBTYPE_NUMBER);
    if(number) {
		lv_obj_del(number);
    }

    if (s3_current_album)
    {
        bool album_changed = (renew == true ) ? true : ((last_displayed_home_album == NULL ) ? true : strcmp(last_displayed_home_album->home_cover, s3_current_album->home_cover) != 0 );
        if (album_changed) {
            ESP_LOGI(TAG, "[LVGL] lv_home_screen: FULL RECREATION - album changed");
            esp_err_t ret = lvgl_load_image_from_sdcard(s3_current_album->home_cover);
            ESP_LOGI(TAG, "[ * ] [LVGL] lv_home_screen: jpg decode %s", s3_current_album->home_cover);

            if (ret != ESP_OK) {
                ESP_LOGW(TAG, "Failed to load album cover %s (error: %s), using default",
                         s3_current_album->home_cover, esp_err_to_name(ret));
                // Continue with default UI - don't crash
            } else {
                cover_ui = lv_base_ui(USE_TRANSPARENCY);
                lv_obj_t *img = lv_img_create(cover_ui);
                lv_img_set_src(img, lvgl_get_img());
                lv_obj_center(img);
            }
        } 
        ESP_LOGI(TAG, "[ * ] [LVGL] lv_home_screen: Update badge");
        // TOP - BADGES
        ui_add_top_badge();

        ui_add_dots_badge(USE_ALBUM);
        ui_add_lang_badge(LANG_HOME);

        last_displayed_home_album = s3_current_album;

    }
    else
    {
        ESP_LOGI(TAG, "[ * ] [LVGL] lv_home_screen: NO ALBUM AVAILABLE");

        cover_ui = lv_base_ui(NO_TRANSPARENCY);
        lv_obj_t *label = lv_label_create(cover_ui);
        lv_label_set_text(label, "NO ALBUM AVAILABLE");
        lv_obj_set_style_text_color(label, lv_color_hex(lv_fnt_color), 0);
        lv_obj_center(label);
    }


}

// Revised - with selective redraw logic
void lv_player_screen(bool renew)
{
    //lvgl_free_previous_buffer();

    // Set color scheme (always needed)
    lv_bkg_color = LV_CUSTOM_GRAY;
    lv_crc_color = LV_CUSTOM_BLACK;
    lv_fnt_color = LV_CUSTOM_WHITE;
    lv_dot_color = LV_CUSTOM_WHITE;

    // Check if we can optimize. s3_player_update is true that means previous screen is not play screen.
    bool coming_from_play_screen = (renew != true) && (pause_overlay_drawn != true);
    // This play cover changed, redraw all.
    // Other status track number, lang are referred to s3_current_album. And update in _xxx_badage.
    bool album_changed = (last_displayed_album == NULL ) ? true : strcmp(last_displayed_album->play_cover, s3_current_album->play_cover) != 0;

    // Force full redraw if player update flag is set (e.g., track changed in LANGUAGE_ALL mode)
    bool force_full_redraw = s3_player_update;

    if (coming_from_play_screen && !album_changed && !force_full_redraw) {
        // OPTIMIZATION PATH: Just update badges, keep existing screen
        ESP_LOGI(TAG, "[LVGL] lv_player_screen: OPTIMIZED - updating badges only");
        // Update bottom badges based on current state
        // Note: Gradient overlay already exists from last full recreation, no need to recreate
        if (s3_current_album && !s3_pause_state) {
            // Show number and language badges when playing
            // Remove conflicting bottom badges (pause vs number vs language)
            // Delete all PLAYER_BADGE type (includes pause, number)
            delete_badges_by_type(CONTENT_TYPE_PLAYER_BADGE);
            // Update top badges (battery, BT)
            ui_add_top_badge();
            ui_add_number_badge();

            if(is_on_blankee() == false)
            {
                ui_add_lang_badge(false);
            }
        }
    } else {
        // FULL RECREATION PATH: Original behavior
        pause_overlay_drawn = false; // Reset pause overlay flag
        if (coming_from_play_screen && album_changed) {
            ESP_LOGI(TAG, "[LVGL] lv_player_screen: FULL RECREATION - album changed");
        } else {
            ESP_LOGI(TAG, "[LVGL] lv_player_screen: FULL RECREATION - entering from %d", s3_last_screen);
        }

        ESP_LOGI(TAG, "[ * ] [LVGL] lv_player_screen: Album cover %s",
                 s3_current_album ? s3_current_album->play_cover : "NULL");
        lv_obj_t *cover_ui = NULL;

        if (s3_current_album)
        {
            esp_err_t ret = lvgl_load_image_from_sdcard(s3_current_album->play_cover);
            ESP_LOGI(TAG, "[ * ] [LVGL] lv_player_screen: jpg decode %s", s3_current_album->play_cover);

            if (ret != ESP_OK) {
                ESP_LOGW(TAG, "Failed to load play cover %s (error: %s), using default",
                         s3_current_album->play_cover, esp_err_to_name(ret));
                // Continue with default UI - don't crash
            } else {
                cover_ui = lv_base_ui(USE_TRANSPARENCY);  // ← This cleans the screen!
                lv_obj_t *img = lv_img_create(cover_ui);
                lv_img_set_src(img, lvgl_get_img());
                lv_obj_center(img);
            }
        }
        else
        {
            ESP_LOGI(TAG, "[ * ] [LVGL] lv_player_screen: NO ALBUM AVAILABLE");

            cover_ui = lv_base_ui(NO_TRANSPARENCY);
            lv_obj_t *label = lv_label_create(cover_ui);
            lv_label_set_text(label, "NO ALBUM AVAILABLE");
            lv_obj_set_style_text_color(label, lv_color_hex(lv_fnt_color), 0);
            lv_obj_center(label);
        }

        // Add all badges
        ui_add_top_badge();

        if (s3_current_album) {
            // Only shows number and language badges when playing
            ui_add_number_badge();

            if(is_on_blankee() == false)
            {
            ui_add_lang_badge(false);
            }
        }

        // Update last displayed album after successful creation
        last_displayed_album = s3_current_album;
    }

    // Clear update flag after successful redraw
    s3_player_update = false;
    ESP_LOGI(TAG, "[LVGL] lv_player_screen: Full redraw completed, flag cleared");

    // Dump badge status for debugging
    // dump_badge_status("lv_player_screen");
}

// MENU
void lv_bt_screen(void)
{
    // Dynamically determine BT icon based on current status
    const char *bt_resource = s3_recover.resource;
    if (s3_pairing_status == BT_PAIRED) {
        bt_resource = ICON_BT_CONNECT;  // Show connected icon when paired
    } else {
        bt_resource = ICON_BT;  // Show default Bluetooth icon when not paired
    }
    
    ESP_LOGI(TAG, "[ * ] [LVGL] lv_bluetooth_screen: jpg decode %s", bt_resource);

    lv_bkg_color = LV_CUSTOM_GRAY;
    lv_crc_color = LV_CUSTOM_BLACK;
    lv_fnt_color = LV_CUSTOM_WHITE;
    lv_dot_color = LV_CUSTOM_WHITE;

    lv_obj_t *menu_ui = lv_menu_ui(bt_resource);

    // s3_menu_idx = IDX_BLUETOOTH;  // Bluetooth menu hidden
    // ui_add_dots_badge(USE_MENU);  // No dots needed as Bluetooth is hidden
}

// MENU
void lv_wifi_search_screen(void)
{
	ESP_LOGI(TAG, "[ * ] [LVGL] lv_wifi_search_screen");

    lv_bkg_color = LV_CUSTOM_GRAY;
    lv_crc_color = LV_CUSTOM_BLACK;
    lv_fnt_color = LV_CUSTOM_WHITE;
    lv_dot_color = LV_CUSTOM_WHITE;

	char *wifi_status = NULL;
    wifi_status = ICON_WIFI_SYNC0;
	start_wifi_connecting();
    lv_obj_t *menu_ui = lv_menu_ui(wifi_status);
}

// MENU
void lv_display_screen(void)
{
    ESP_LOGI(TAG, "[ * ] [LVGL] lv_display_screen: jpg decode %s", s3_recover.resource);

    lv_bkg_color = LV_CUSTOM_GRAY;
    lv_crc_color = LV_CUSTOM_BLACK;
    lv_fnt_color = LV_CUSTOM_WHITE;
    lv_dot_color = LV_CUSTOM_WHITE;

    lv_obj_t *menu_ui = lv_menu_ui(s3_recover.resource);

    s3_menu_idx = IDX_BRIGHTNESS;
    ui_add_dots_badge(USE_MENU);
}

// MENU
void lv_nfc_screen(void)
{
    ESP_LOGI(TAG, "[ * ] [LVGL] lv_nfc_screen: jpg decode %s", s3_recover.resource);

    lv_bkg_color = LV_CUSTOM_GRAY;
    lv_crc_color = LV_CUSTOM_BLACK;
    lv_fnt_color = LV_CUSTOM_WHITE;
    lv_dot_color = LV_CUSTOM_WHITE;

    lv_obj_t *menu_ui = lv_menu_ui(s3_recover.resource);

    s3_menu_idx = IDX_NFC;
    ui_add_dots_badge(USE_MENU);
}

// Version to review
void lv_bt_pair_screen(void)
{
    const char *bt_status = NULL;

    switch (s3_pairing_status)
    {
    case BT_SCAN:
        bt_status = ICON_BT_SEARCH;
        break;

    case BT_PAIRED:
        bt_status = ICON_BT_CONNECT;
        break;

    case BT_UNPAIRED:
    default:
        bt_status = ICON_BT;  // Show plain BT icon when unpaired
        break;
    }

    lv_obj_t *menu_ui = lv_menu_ui(bt_status);
}

// Version to review
void lv_wifi_connect_screen(void)
{
    ESP_LOGI(TAG, "[ * ] [LVGL] lv_wifi_connect_screen jpg decode %s", s3_recover.resource);
    lv_bkg_color = LV_CUSTOM_GRAY;
    lv_crc_color = LV_CUSTOM_BLACK;
    lv_fnt_color = LV_CUSTOM_WHITE;
    lv_dot_color = LV_CUSTOM_WHITE;
    lv_obj_t *menu_ui = lv_menu_ui(s3_recover.resource);
}

// Version to review
void lv_ble_pair_screen(void)
{
    char text[64];
    ESP_LOGI(TAG, "[ * ] [LVGL] lv_ble_pairing_screen");
    lv_bkg_color = LV_CUSTOM_WHITE;                                                         // set the background color
    lv_crc_color = LV_CUSTOM_BLACK;                                                         // set the circle color
    lv_fnt_color = LV_CUSTOM_WHITE;
    lv_dot_color = LV_CUSTOM_WHITE;

    lv_obj_t *local_ui = lv_base_ui(NO_TRANSPARENCY);                                                 // recover the main element

    char sn[32] = {0};
    char sn_final_numbers[5] = {0};

    read_serial_number(sn);
    if (strlen(sn) > 0) {
        strncpy(sn_final_numbers, &sn[strlen(sn) - 4], 4);
        sn_final_numbers[4] = '\0';
        snprintf(text, 29, "Pixsee_%s", sn_final_numbers);
    } else {
        snprintf(text, 29, "Pixsee_XXXX");
    }

    lv_obj_t *label = lv_label_create(local_ui);
    lv_label_set_text(label, text);
    lv_obj_set_style_text_color(label, lv_color_hex(lv_fnt_color), 0);
    lv_obj_center(label);
}

// Version to review
void lv_wifi_unknown_screen(void)
{
    ESP_LOGI(TAG, "[ * ] [LVGL] lv_wifi_unknown_screen jpg decode %s", s3_recover.resource);
    lv_bkg_color = LV_CUSTOM_GRAY;
    lv_crc_color = LV_CUSTOM_BLACK;
    lv_fnt_color = LV_CUSTOM_WHITE;
    lv_dot_color = LV_CUSTOM_WHITE;
    lv_obj_t *menu_ui = lv_menu_ui(s3_recover.resource);

    s3_menu_idx = IDX_WIFI;
    ui_add_dots_badge(USE_MENU);
}

// Version to review
void lv_wifi_disconnect_screen(void)
{
    ESP_LOGI(TAG, "[ * ] [LVGL] lv_wifi_disconnect_screen jpg decode %s", s3_recover.resource);
    lv_bkg_color = LV_CUSTOM_GRAY;
    lv_crc_color = LV_CUSTOM_BLACK;
    lv_fnt_color = LV_CUSTOM_WHITE;
    lv_dot_color = LV_CUSTOM_WHITE;
    lv_obj_t *menu_ui = lv_menu_ui(s3_recover.resource);

	s3_menu_idx = IDX_WIFI;
    ui_add_dots_badge(USE_MENU);
}


// Version to review
void lv_data_sync_screen(void)
{
    lv_bkg_color = LV_CUSTOM_GRAY;
    lv_crc_color = LV_CUSTOM_BLACK;
    lv_fnt_color = LV_CUSTOM_WHITE;
    lv_dot_color = LV_CUSTOM_WHITE;

    char tmpResource[64]={0};

    // Check if wait screen should be displayed (button pressed during sync)
    if (s3_data_sync_show_wait) {
        strcpy(tmpResource, ICON_WIFI_SYNC_WAIT);
        ESP_LOGI(TAG, "[ * ] [LVGL] lv_data_sync_screen: showing wait screen");
    } else {
        // Display different images based on sync stage
        // Stage 0: Preparation, Stage 1: WiFi, Stage 2: Resource, Stage 3: Account
        switch(s3_sync_stage) {
            case 0:  // Preparation stage
                strcpy(tmpResource, ICON_WIFI_SYNC0);
                break;
            case 1:  // WiFi connection stage
                strcpy(tmpResource, ICON_WIFI_SYNC1);
                break;
            case 2:  // Resource update stage
                strcpy(tmpResource, ICON_WIFI_SYNC2);
                break;
            case 3:  // Account content sync stage
                strcpy(tmpResource, ICON_WIFI_SYNC3);
                break;
            default:
                // Fallback to original logic if stage is invalid
                if (s3_show_default_syncUp == true || strlen(s3_WiFiSyncKidIcon) == 0 ){
                    strcpy(tmpResource,ICON_WIFI_SYNC);
                } else {
                    sprintf(tmpResource,ICON_KID_SYNC_N, s3_WiFiSyncKidIcon);
                }
                break;
        }
    }

	ESP_LOGI(TAG, "[ * ] [LVGL] lv_data_sync_screen: stage=%d, loading %s", s3_sync_stage, tmpResource);

	lv_obj_t *local_ui = lv_animation_ui(tmpResource, USE_ANIM_JPG);

    // Only show GIF animation for normal sync stages, not for wait screen
    if (!s3_data_sync_show_wait) {
        // Overlay GIF animation at position (169, 127) for data_sync screens
        // IMPORTANT: Load GIF after JPG to ensure it's on top and buffer is fresh
        const char *gif_path = "/sdcard/animation_gif/wifi/data_sync.gif";

        // Reset static GIF object pointer since screen was cleared by lv_animation_ui
        data_sync_gif_obj = NULL;

        ESP_LOGI(TAG, "[ * ] [LVGL] lv_data_sync_screen: loading GIF %s", gif_path);
        esp_err_t ret = lvgl_load_gif_from_sdcard(gif_path);
        if (ret == ESP_OK) {
            data_sync_gif_obj = lv_gif_create(local_ui);
            if (data_sync_gif_obj && lvgl_validate_gif_dsc(lvgl_get_gif())) {
                lv_gif_set_src(data_sync_gif_obj, lvgl_get_gif());
                lv_obj_set_pos(data_sync_gif_obj, 169, 127);
                lv_obj_move_foreground(data_sync_gif_obj);

                // Force LVGL to update display to ensure GIF starts playing
                lv_refr_now(NULL);

                ESP_LOGI(TAG, "[ * ] [LVGL] lv_data_sync_screen: GIF animation overlayed and started at (169, 127)");
            } else {
                ESP_LOGW(TAG, "[ * ] [LVGL] lv_data_sync_screen: Invalid GIF descriptor or failed to create object");
                data_sync_gif_obj = NULL;
            }
        } else {
            ESP_LOGW(TAG, "[ * ] [LVGL] lv_data_sync_screen: Failed to load GIF %s (error: %s)",
                     gif_path, esp_err_to_name(ret));
            data_sync_gif_obj = NULL;
        }
    } else {
        // Clear GIF object when showing wait screen
        data_sync_gif_obj = NULL;
    }

    // Update last sync stage
    last_sync_stage = s3_sync_stage;
}

void lv_wifi_synced_screen(void)
{
    lv_bkg_color = LV_CUSTOM_GRAY;
    lv_crc_color = LV_CUSTOM_BLACK;
    lv_fnt_color = LV_CUSTOM_WHITE;
    lv_dot_color = LV_CUSTOM_WHITE;

    char tmpResource[64]={0};
    if (strlen(s3_WiFiSyncKidIcon) == 0 ){
        strcpy(tmpResource,ICON_WIFI_SYNCED);
    } else {
        sprintf(tmpResource,ICON_KID_SYNCED_N, s3_WiFiSyncKidIcon);
    }
    ESP_LOGI(TAG, "[ * ] [LVGL] lv_wifi_synced_screen: loading static %s", tmpResource);

    lv_obj_t *local_ui = lv_animation_ui(tmpResource, USE_ANIM_JPG);
}

// power on -> kid -> home
void lv_kid_screen(void)
{
    lv_bkg_color = LV_CUSTOM_GRAY;
    lv_crc_color = LV_CUSTOM_BLACK;
    lv_fnt_color = LV_CUSTOM_WHITE;
    lv_dot_color = LV_CUSTOM_WHITE;

    char tmpResource[64]={0};
    if (strlen(s3_WiFiSyncKidIcon) == 0 ){

    } else {
        sprintf(tmpResource,ICON_KID_N, s3_WiFiSyncKidIcon);
        lv_obj_t *local_ui = lv_animation_ui(tmpResource, USE_ANIM_JPG);
    }
    ESP_LOGI(TAG, "[ * ] [LVGL] lv_kid_screen: loading static %s", tmpResource);
    set_current_screen(POWER_ON_KID_SCREEN, HOME_SCREEN);
}

void lv_nfc_wifi_search_screen(void)
{
    ESP_LOGI(TAG, "[ * ] [LVGL] lv_wifi_search_screen");

    lv_bkg_color = LV_CUSTOM_GRAY;
    lv_crc_color = LV_CUSTOM_BLACK;
    lv_fnt_color = LV_CUSTOM_WHITE;
    lv_dot_color = LV_CUSTOM_WHITE;

    char *wifi_status = NULL;
    wifi_status = ICON_WIFI_SECH;
    lv_obj_t *menu_ui = lv_menu_ui(wifi_status);
}

void lv_nfc_wifi_disc_screen(void)
{
    ESP_LOGI(TAG, "[ * ] [LVGL] lv_wifi_disconnect_screen jpg decode %s", s3_recover.resource);
    lv_bkg_color = LV_CUSTOM_GRAY;
    lv_crc_color = LV_CUSTOM_BLACK;
    lv_fnt_color = LV_CUSTOM_WHITE;
    lv_dot_color = LV_CUSTOM_WHITE;
    lv_obj_t *menu_ui = lv_menu_ui(s3_recover.resource);

    s3_menu_idx = IDX_WIFI;
    ui_add_dots_badge(USE_MENU);
}

// Version to review
void lv_wifi_plug_in_screen(void)
{
    ESP_LOGI(TAG, "[ * ] [LVGL] lv_data_sync_scree jpg decode %s", s3_recover.resource);
    lv_bkg_color = LV_CUSTOM_GRAY;
    lv_crc_color = LV_CUSTOM_BLACK;
    lv_fnt_color = LV_CUSTOM_WHITE;
    lv_dot_color = LV_CUSTOM_WHITE;

    lv_obj_t *menu_ui = lv_menu_ui(s3_recover.resource);
}

// Version to review
void lv_nfc_settings_screen(void)
{
    lvgl_free_previous_buffer();

    if (s3_selected_language == NO_LANGUAGE) { ui_init_language(); }

    ESP_LOGI(TAG, "[ * ] [LVGL] lv_nfc_settings_screen: jpg decode %s", s3_language_resource);
    esp_err_t ret = lvgl_load_image_from_sdcard(s3_language_resource);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Failed to load language resource %s (error: %s), using default",
                 s3_language_resource, esp_err_to_name(ret));
        // Continue with default UI - don't crash
        return;
    }

    lv_bkg_color = LV_CUSTOM_GRAY;
    lv_crc_color = LV_CUSTOM_BLACK;
    lv_fnt_color = LV_CUSTOM_WHITE;
    lv_dot_color = LV_CUSTOM_WHITE;

    lv_obj_t *local_ui = lv_base_ui(NO_TRANSPARENCY);

    lv_obj_t *img = lv_img_create(local_ui);
    lv_img_set_src(img, lvgl_get_img());
    lv_obj_center(img);
}

/**
 * @brief NFC activation screen - displayed when blank NFC is detected
 */
void lv_nfc_activation_screen(void)
{
    lvgl_free_previous_buffer();

    ESP_LOGI(TAG, "[ * ] [LVGL] lv_nfc_activation_screen: jpg decode %s", ICON_NFC_ACTIVATE);
    esp_err_t ret = lvgl_load_image_from_sdcard(ICON_NFC_ACTIVATE);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Failed to load NFC activation icon %s (error: %s), using default",
                 ICON_NFC_ACTIVATE, esp_err_to_name(ret));
        return;
    }

    lv_bkg_color = LV_CUSTOM_BLACK;
    lv_crc_color = LV_CUSTOM_WHITE;
    lv_fnt_color = LV_CUSTOM_WHITE;
    lv_dot_color = LV_CUSTOM_WHITE;

    lv_obj_t *local_ui = lv_base_ui(NO_TRANSPARENCY);

    lv_obj_t *img = lv_img_create(local_ui);
    lv_img_set_src(img, lvgl_get_img());
    lv_obj_center(img);
}

/**
 * @brief NFC no content screen - displayed when NFC scan finds no content
 */
void lv_nfc_no_content_screen(void)
{
    lvgl_free_previous_buffer();

    ESP_LOGI(TAG, "[ * ] [LVGL] lv_nfc_no_content_screen: jpg decode %s", ICON_NFC_NO_CONTENT);
    esp_err_t ret = lvgl_load_image_from_sdcard(ICON_NFC_NO_CONTENT);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Failed to load NFC no content icon %s (error: %s), using default",
                 ICON_NFC_NO_CONTENT, esp_err_to_name(ret));
        return;
    }

    lv_bkg_color = LV_CUSTOM_BLACK;
    lv_crc_color = LV_CUSTOM_WHITE;
    lv_fnt_color = LV_CUSTOM_WHITE;
    lv_dot_color = LV_CUSTOM_WHITE;

    lv_obj_t *local_ui = lv_base_ui(NO_TRANSPARENCY);

    lv_obj_t *img = lv_img_create(local_ui);
    lv_img_set_src(img, lvgl_get_img());
    lv_obj_center(img);
}

void lv_power_plug_in_screen(void)
{
    ESP_LOGI(TAG, "[ * ] [LVGL] lv_power_plug_in_screen jpg decode %s", s3_recover.resource);
    lv_bkg_color = LV_CUSTOM_GRAY;
    lv_crc_color = LV_CUSTOM_BLACK;
    lv_fnt_color = LV_CUSTOM_WHITE;
    lv_dot_color = LV_CUSTOM_WHITE;

    lv_obj_t *menu_ui = lv_menu_ui(s3_recover.resource);
}

/**
 * @brief Unified NFC content screen - displays appropriate content based on s3_nfc_content_type
 * 
 * NFC Logic Flow (after removing automatic download check):
 * 1) NFC is_nfc_available → proceed to play_screen and play
 * 2) !is_nfc_available → show NFC_CONT_GO_ACTIVE, then return to previous screen
 * 3) blank NFC detected → show NFC_CONT_NO_CONTENT, then return to previous screen
 * 4) NFC_CONT_NOT_AVAIL → placeholder, never shown
 *
 * This screen consolidates the old NFC_ACTIVATION_SCREEN and NFC_NO_CONTENT_SCREEN
 */
void lv_nfc_content_screen(void)
{
    ESP_LOGI(TAG, "[ * ] [LVGL] lv_nfc_content_screen: content type %d", s3_nfc_content_type);
    
    const char *content_icon = NULL;
    const char *log_message = "";
    
    switch (s3_nfc_content_type) {
            
        case NFC_CONT_GO_ACTIVE:
            // Show when NFC tag needs to be activated (!is_nfc_available)
            // Replaces old NFC_ACTIVATION_SCREEN
            content_icon = ICON_NFC_GO_ACT;
            log_message = "NFC requires activation (not available for this account)";
            break;
            
        case NFC_CONT_NO_CONTENT:
            // Show when blankee NFC tag is detected (SKU: "none")
            content_icon = ICON_NFC_NO_CON;
            log_message = "Blank NFC detected (no content)";
            break;
        case NFC_CONT_NOT_DOWNLOADED:
            // Show when blankee NFC tag is detected (SKU: "none")
            content_icon = ICON_NFC_NO_DOW;
            log_message = "Blank NFC detected (not downloaded)";
            break;
        case NFC_CONT_DEFAULT:
        default:
            // This should never be shown - it's just a placeholder
            ESP_LOGW(TAG, "NFC Content: Invalid/unused content type %d, using fallback", s3_nfc_content_type);
            content_icon = ICON_NFC_OTHER;
            log_message = "Invalid NFC content type (fallback)";
            break;
    }
    
    ESP_LOGI(TAG, "NFC Content Screen: %s", log_message);
    
    lvgl_free_previous_buffer();
    ESP_LOGI(TAG, "[ * ] [LVGL] lv_nfc_content_screen: jpg decode %s", content_icon);
    esp_err_t ret = lvgl_load_image_from_sdcard(content_icon);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Failed to load content icon %s (error: %s), using default",
                 content_icon, esp_err_to_name(ret));
        return;
    }

    lv_bkg_color = LV_CUSTOM_BLACK;
    lv_crc_color = LV_CUSTOM_WHITE;
    lv_fnt_color = LV_CUSTOM_WHITE;
    lv_dot_color = LV_CUSTOM_WHITE;

    lv_obj_t *local_ui = lv_base_ui(NO_TRANSPARENCY);

    lv_obj_t *img = lv_img_create(local_ui);
    lv_img_set_src(img, lvgl_get_img());
    lv_obj_center(img);
}

// Version to review
void lv_display_icon_screen(void)
{    
    ESP_LOGI(TAG, "[ * ] [LVGL] lv_display_icon_screen: brightness level synced to %d", s3_brightness_level);
    
    lv_bkg_color = LV_CUSTOM_BLACK;
    lv_crc_color = LV_CUSTOM_WHITE;
    lv_fnt_color = LV_CUSTOM_BLACK;
    lv_dot_color = LV_CUSTOM_BLACK;

    const char *brightnesss_resource;
    // Map brightness level values (0,1,2) to brightness icons
    if (s3_brightness_level == BRIGHTNESS_LEVEL_1) { brightnesss_resource = ICON_BRIGHT_1; }
    else if (s3_brightness_level == BRIGHTNESS_LEVEL_2) { brightnesss_resource = ICON_BRIGHT_2; }
    else if (s3_brightness_level == BRIGHTNESS_LEVEL_3) { brightnesss_resource = ICON_BRIGHT_3; }
    else {
        ESP_LOGW(TAG, "Invalid brightness level %d, defaulting to medium", s3_brightness_level);
        brightnesss_resource = ICON_BRIGHT_2;
    }

    lv_obj_t *local_ui = lv_animation_ui(brightnesss_resource, USE_ANIM_JPG);
}

void lv_facReset_screen(void)
{
	ESP_LOGI(TAG, "[ * ] [LVGL] lv_facReset_screen: Static");
	lv_bkg_color = LV_CUSTOM_WHITE;
	lv_crc_color = LV_CUSTOM_BLACK;
	lv_fnt_color = LV_CUSTOM_WHITE;
	lv_dot_color = LV_CUSTOM_WHITE;

	lv_obj_t *local_ui = lv_animation_ui(ICON_POWER_FAC, USE_ANIM_JPG);
}

void lv_facReset_accInv_screen(void) {
    ESP_LOGI(TAG, "[ * ] [LVGL] lv_facReset_accInv_screen: Static");
    lv_bkg_color = LV_CUSTOM_WHITE;
    lv_crc_color = LV_CUSTOM_BLACK;
    lv_fnt_color = LV_CUSTOM_WHITE;
    lv_dot_color = LV_CUSTOM_WHITE;

    lv_obj_t *local_ui = lv_animation_ui(ICON_POWER_FAC_ACC_INV, USE_ANIM_JPG);
}

void lv_countdown_screen(void)
{
    ESP_LOGI(TAG, "[ * ] [LVGL] lv_countdown_screen: Static");

    lv_bkg_color = LV_CUSTOM_BLACK;
    lv_crc_color = LV_CUSTOM_BLACK;
    lv_fnt_color = LV_CUSTOM_YELLOW;
    lv_dot_color = LV_CUSTOM_WHITE;

    s3_countdown_value = COUNTDOWN_START_VALUE;

    // Clean up old timer/label if present
    if (s3_countdown_timer) {
        lv_timer_del(s3_countdown_timer);
        s3_countdown_timer = NULL;
    }
    s3_countdown_label = NULL;

    lv_obj_t *local_ui = lv_base_ui(NO_TRANSPARENCY);

    s3_countdown_label = lv_label_create(local_ui);
    lv_obj_set_style_text_font(s3_countdown_label, &cherry_bomb_90, 0);
    lv_obj_set_style_text_color(s3_countdown_label, lv_color_hex(lv_fnt_color), 0);
    lv_obj_center(s3_countdown_label);

    // Start value (macro)
    char buf[4];
    snprintf(buf, sizeof(buf), "%d", s3_countdown_value);
    lv_label_set_text(s3_countdown_label, buf);

    // Start timer
    s3_countdown_timer = lv_timer_create(countdown_timer_cb, COUNTDOWN_TRANSITION, NULL);
}

void lv_dummy_screen(void)
{
    char text[64];
    ESP_LOGI(TAG, "[ * ] [LVGL] lv_dummy_screen: Static");
    lv_bkg_color = LV_CUSTOM_WHITE;                                                         // set the background color
    lv_crc_color = LV_CUSTOM_BLACK;                                                         // set the circle color
    lv_fnt_color = LV_CUSTOM_WHITE;
    lv_dot_color = LV_CUSTOM_WHITE;

    lv_obj_t *local_ui = lv_base_ui(NO_TRANSPARENCY);                                                 // recover the main element

    lv_obj_t *label = lv_label_create(local_ui);                                            // create a label
    snprintf(text, sizeof(text), "Dummy: %s", s3_recover.name ? s3_recover.name : "NULL");
    lv_label_set_text(label, text);                                                         // write the msg
    lv_obj_set_style_text_color(label, lv_color_hex(lv_fnt_color), 0);                      // tet text color to blue
    lv_obj_center(label);                                                                   // center the label on the screen
}

void lv_animation_dummy_screen(void)
{
    ESP_LOGI(TAG, "[ * ] [LVGL] lv_rlottie_builtin_screen: LVGL native animations");

    // Check if LVGL is initialized
    if (lv_disp_get_default() == NULL) {
        ESP_LOGE(TAG, "LVGL display not initialized! Cannot start animations.");
        return;
    }

    // Get active screen and clean it
    lv_obj_t *scr = lv_scr_act();
    if (scr == NULL) {
        ESP_LOGE(TAG, "Failed to get active screen!");
        return;
    }
    lv_obj_clean(scr);
    // clear_static_lv_objects();

    // Set dark background
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x222222), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);

    // Create title label
    lv_obj_t *title = lv_label_create(scr);
    lv_label_set_text(title, "LVGL Animation Demo");
    lv_obj_set_style_text_color(title, lv_color_hex(lv_fnt_color), 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 10);

    // Create a container to hold the animations
    lv_obj_t *cont = lv_obj_create(scr);
    lv_obj_set_size(cont, 240, 240);
    lv_obj_set_style_bg_color(cont, lv_color_hex(0x333333), 0);
    lv_obj_set_style_bg_opa(cont, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(cont, 2, 0);
    lv_obj_set_style_border_color(cont, lv_color_hex(0x555555), 0);
    lv_obj_center(cont);

    // Create 4 objects for different animation types

    // 1. Bouncing square (position animation)
    animated_objects[0] = lv_obj_create(cont);
    lv_obj_set_size(animated_objects[0], 50, 50);
    lv_obj_set_style_bg_color(animated_objects[0], lv_color_hex(0xFF5555), 0);
    lv_obj_set_style_radius(animated_objects[0], 5, 0);
    lv_obj_align(animated_objects[0], LV_ALIGN_TOP_LEFT, 20, 20);

    // 2. Size pulsing effect
    animated_objects[1] = lv_obj_create(cont);
    lv_obj_set_size(animated_objects[1], 50, 50);
    lv_obj_set_style_bg_color(animated_objects[1], lv_color_hex(0x55FF55), 0);
    lv_obj_set_style_radius(animated_objects[1], LV_RADIUS_CIRCLE, 0);
    lv_obj_align(animated_objects[1], LV_ALIGN_TOP_RIGHT, -20, 20);

    // 3. Color transition
    animated_objects[2] = lv_obj_create(cont);
    lv_obj_set_size(animated_objects[2], 50, 50);
    lv_obj_set_style_bg_color(animated_objects[2], lv_color_hex(0xFF0000), 0);
    lv_obj_set_style_radius(animated_objects[2], 10, 0);
    lv_obj_align(animated_objects[2], LV_ALIGN_BOTTOM_LEFT, 20, -20);

    // 4. Opacity fade
    animated_objects[3] = lv_obj_create(cont);
    lv_obj_set_size(animated_objects[3], 50, 50);
    lv_obj_set_style_bg_color(animated_objects[3], lv_color_hex(0x5555FF), 0);
    lv_obj_set_style_radius(animated_objects[3], 0, 0);  // Square
    lv_obj_align(animated_objects[3], LV_ALIGN_BOTTOM_RIGHT, -20, -20);

    // Create descriptive labels for each animation
    lv_obj_t *label1 = lv_label_create(cont);
    lv_label_set_text(label1, "Position");
    lv_obj_set_style_text_color(label1, lv_color_hex(lv_fnt_color), 0);
    lv_obj_align_to(label1, animated_objects[0], LV_ALIGN_OUT_BOTTOM_MID, 0, 5);

    lv_obj_t *label2 = lv_label_create(cont);
    lv_label_set_text(label2, "Size");
    lv_obj_set_style_text_color(label2, lv_color_hex(lv_fnt_color), 0);
    lv_obj_align_to(label2, animated_objects[1], LV_ALIGN_OUT_BOTTOM_MID, 0, 5);

    lv_obj_t *label3 = lv_label_create(cont);
    lv_label_set_text(label3, "Color");
    lv_obj_set_style_text_color(label3, lv_color_hex(lv_fnt_color), 0);
    lv_obj_align_to(label3, animated_objects[2], LV_ALIGN_OUT_TOP_MID, 0, -5);

    lv_obj_t *label4 = lv_label_create(cont);
    lv_label_set_text(label4, "Opacity");
    lv_obj_set_style_text_color(label4, lv_color_hex(lv_fnt_color), 0);
    lv_obj_align_to(label4, animated_objects[3], LV_ALIGN_OUT_TOP_MID, 0, -5);

    // Create the animations

    // 1. Bouncing animation (up and down)
    lv_anim_t bounce_anim;
    lv_anim_init(&bounce_anim);
    lv_anim_set_var(&bounce_anim, animated_objects[0]);
    lv_anim_set_exec_cb(&bounce_anim, bounce_anim_y_cb);
    lv_anim_set_values(&bounce_anim, 20, 170);  // Start and end positions
    lv_anim_set_time(&bounce_anim, 1000);      // Animation duration in ms
    lv_anim_set_playback_time(&bounce_anim, 1000); // Return animation duration
    lv_anim_set_repeat_count(&bounce_anim, LV_ANIM_REPEAT_INFINITE);
    lv_anim_set_path_cb(&bounce_anim, lv_anim_path_ease_in_out);
    if (lv_anim_start(&bounce_anim) != &bounce_anim) {
        ESP_LOGW(TAG, "Failed to start bounce animation!");
    }

    // 2. Size pulsing animation
    lv_anim_t size_anim;
    lv_anim_init(&size_anim);
    lv_anim_set_var(&size_anim, animated_objects[1]);
    lv_anim_set_exec_cb(&size_anim, size_anim_cb);
    lv_anim_set_values(&size_anim, 20, 80);     // Min and max size
    lv_anim_set_time(&size_anim, 800);          // Animation duration
    lv_anim_set_playback_time(&size_anim, 800); // Return animation duration
    lv_anim_set_repeat_count(&size_anim, LV_ANIM_REPEAT_INFINITE);
    lv_anim_set_path_cb(&size_anim, lv_anim_path_overshoot);
    if (lv_anim_start(&size_anim) != &size_anim) {
        ESP_LOGW(TAG, "Failed to start size animation!");
    }

    // 3. Color transition (red to green and back)
    lv_anim_t color_anim;
    lv_anim_init(&color_anim);
    lv_anim_set_var(&color_anim, animated_objects[2]);
    lv_anim_set_exec_cb(&color_anim, color_anim_cb);
    lv_anim_set_values(&color_anim, 0, 255);    // Color mix values
    lv_anim_set_time(&color_anim, 1500);        // Animation duration
    lv_anim_set_playback_time(&color_anim, 1500); // Return animation duration
    lv_anim_set_repeat_count(&color_anim, LV_ANIM_REPEAT_INFINITE);
    lv_anim_set_path_cb(&color_anim, lv_anim_path_linear);
    if (lv_anim_start(&color_anim) != &color_anim) {
        ESP_LOGW(TAG, "Failed to start color animation!");
    }

    // 4. Opacity fade (fade in and out)
    lv_anim_t opacity_anim;
    lv_anim_init(&opacity_anim);
    lv_anim_set_var(&opacity_anim, animated_objects[3]);
    lv_anim_set_exec_cb(&opacity_anim, opacity_anim_cb);
    lv_anim_set_values(&opacity_anim, 255, 50); // From opaque to transparent
    lv_anim_set_time(&opacity_anim, 1200);      // Animation duration
    lv_anim_set_playback_time(&opacity_anim, 1200); // Return animation duration
    lv_anim_set_repeat_count(&opacity_anim, LV_ANIM_REPEAT_INFINITE);
    lv_anim_set_path_cb(&opacity_anim, lv_anim_path_ease_out);
    if (lv_anim_start(&opacity_anim) != &opacity_anim) {
        ESP_LOGW(TAG, "Failed to start opacity animation!");
    }

    // Create a timer to ensure animations are updated
    lv_timer_t *anim_timer = lv_timer_create(anim_timer_cb, 20, NULL);  // 20ms timer for ~50fps
    if (anim_timer == NULL) {
        ESP_LOGW(TAG, "Failed to create animation timer - animations may not update properly");
    } else {
        ESP_LOGI(TAG, "Animation timer created successfully");
    }

    // Make sure LVGL display driver is refreshing
    lv_disp_t *disp = lv_disp_get_default();
    if (disp != NULL) {
        lv_disp_trig_activity(disp);  // Signal that there's activity to the display
        ESP_LOGI(TAG, "Display refreshed to ensure animations play");
    }

    ESP_LOGI(TAG, "Animations started successfully");
    ESP_LOGI(TAG, "Bounce animation: %p", (void*)&bounce_anim);
    ESP_LOGI(TAG, "Size animation: %p", (void*)&size_anim);
    ESP_LOGI(TAG, "Color animation: %p", (void*)&color_anim);
    ESP_LOGI(TAG, "Opacity animation: %p", (void*)&opacity_anim);
}

// Screen control functions
lv_timer_t *init_screen_manager(bool use_carroucel)
{
    // Initialize the GUI mutex first
    gui_mutex_init();

    // Initialize JPEG cache system
    jpeg_cache_init();

    // Initialize PNG cache system
    png_cache_init();

    gui_lock();
    s3_carroucel = use_carroucel;
    screen_timer = lv_timer_create(screen_timer_cb, INSTANT_TRANSITION, NULL);
    lv_timer_pause(screen_timer);
    ui_init_language();
    gui_unlock();

    return screen_timer;
}

void refresh_screen_display(void)
{
    // Allow ALARM_SCREEN refresh even during boot (alarm can trigger before HOME_SCREEN is reached)
    // Skip boot check for ALARM_SCREEN to handle early boot alarms and overlapping alarms
    if (s3_current_screen != ALARM_SCREEN) {
        if (!s3_boot_completed || s3_shutdown_started) {
            ESP_LOGI(TAG, "[SCREEN_REFRESH] Boot/Shutdown active, skipping refresh");
            return;
        }
    }

    // Skip refresh if screen is dimmed to save CPU and power
    if (is_screen_dimmed()) {
        ESP_LOGD(TAG, "[SCREEN_REFRESH] Screen dimmed, skipping refresh to save power");
        return;
    }

    ESP_LOGI(TAG, "[SCREEN_REFRESH] Refreshing screen display");
    
    // Protect screen update pending flag with mutex to prevent race conditions
    gui_lock();
    if (!screen_update_pending) {
        screen_update_pending = true;
        lv_async_call(update_screen_display, NULL);
    }
    gui_unlock();
}

void update_screen_display(void)
{
    gui_lock();

    // Clear pending flag at start so next refresh can be queued
    screen_update_pending = false;
    bool renew_screen = true;

    // BLOCK SCREEN TRANSITION DURING SHUTDOWN
    // But allow transitions to SHUTDOWN_SCREEN and POWER_OFF_SCREEN for proper shutdown sequence
    if (s3_shutdown_started &&
        s3_current_screen != COUNTDOWN_SCREEN &&
        s3_current_screen != SHUTDOWN_SCREEN &&
        s3_current_screen != POWER_OFF_SCREEN) {
        ESP_LOGW(TAG, "[SCREEN_TRANSITION]: Ignored - Shutdown in progress (current: %d)", s3_current_screen);
        gui_unlock();
        return;
    }

    // presumably s3_current and s3_next are different
    s3_recover = s3_screen_resources[s3_current_screen];
 

 
    if ( s3_current_screen == PLAY_SCREEN ) {
        if (s3_last_screen != PLAY_SCREEN) {
            s3_player_update = true;  // Force full redraw of player screen
            s3_pause_state = false; // Reset pause state when entering PLAY_SCREEN
            s3_lang_state = true; // Reset language state when entering PLAY_SCREEN
            ESP_LOGI(TAG, "[SCREEN_UPDATE] Transition to PLAY_SCREEN, forcing full redraw");

        } else {
            renew_screen = false;
        }
    } else {
        ESP_LOGI(TAG, "[LVGL] lv_player_screen: Update flag set, so next time we perform a  full redraw");
        s3_player_update = true;
    }

    if (s3_current_screen == HOME_SCREEN) {
        if ( s3_current_album != NULL) {
            // Reset language badge state when entering HOME_SCREEN to ensure it displays
            s3_lang_state = true;
            ESP_LOGI(TAG, "[SCREEN_UPDATE] Entering HOME_SCREEN, enabling language badge");
        }
        if (s3_last_screen == HOME_SCREEN) {
            renew_screen = false;
            ESP_LOGI(TAG, "[SCREEN_UPDATE] HOME_SCREEN: Previous screen was the same, partial update only");
        } else {
            // First time entering HOME_SCREEN from boot - register restoration callback
            // The callback will restore the album AFTER this HOME screen is fully rendered
            set_last_transition_callback(restore_last_played_album_callback);
            ESP_LOGI(TAG, "[SCREEN_UPDATE] Registered album restoration callback for HOME_SCREEN");
        }
        // Special handling for HOME_SCREEN: dynamically set Bluetooth icon based on connection status
        if (s3_pairing_status == BT_PAIRED) {
            s3_recover.resource = ICON_BT_CONNECT;  // Show connected icon when paired
        } else {
            s3_recover.resource = ICON_BT;  // Show default Bluetooth icon when not paired
        }
    }

    ESP_LOGI(TAG, "[%d/%d] Show screen [%s]", s3_current_screen, SCREENS_QTD, s3_recover.name);
    ESP_LOGI(TAG, "[SCREEN_UPDATE][%d/%d] Show screen [%s]", s3_current_screen, SCREENS_QTD, s3_recover.name);
    dev_ctrl_update_values(s3_current_screen, NO_UPDATE, NO_UPDATE);

 
    if(s3_recover.base_type == BASE_ANCHOR)
    {
        ESP_LOGI(TAG, "[SCREEN_UPDATE] Set Base screen as [%d], previous screen [%d]", s3_current_screen, s3_previous_screen);
        s3_previous_screen = s3_current_screen;
    }
    // for tracking last screen, so we can determine renew_screen in player/home screen
    // s3_previous_screen  
    s3_last_screen = s3_current_screen; 


    // Update display to next screen
    switch(s3_current_screen)
    {
        case BOOT_SCREEN:                lv_boot_animation(USE_ANIM_JPG);        break;
        case POWER_LOW_SCREEN:           lv_power_screen(USE_ANIM_JPG);          break;
        case STANDBY_SCREEN:             lv_off_screen();                        break;
        case POWER_OFF_SCREEN:           lv_off_screen();                        break;
        case SHUTDOWN_SCREEN:            lv_shutdown_animation(USE_ANIM_JPG);    break;
        case HOME_SCREEN:                lv_home_screen(renew_screen);           break;
        case PLAY_SCREEN:                lv_player_screen(renew_screen);         break;
        case PAUSE_SCREEN:               lv_pause_screen();                      break;
        case VOLUME_UP_SCREEN:           lv_volume_up_animation(USE_ANIM_JPG);   break;
        case VOLUME_DOWN_SCREEN:         lv_volume_down_animation(USE_ANIM_JPG); break;
        case CLOCK_SCREEN:               lv_clock_screen();                      break;
        case ALARM_SCREEN:               lv_alarm_animation(USE_ANIM_JPG);       break;
        case DISPLAY_SCREEN:             lv_display_screen();                    break;
        case DISPLAY_SETTINGS_SCREEN:    lv_display_icon_screen();               break;
        case BRIGHTNESS_UP_SCREEN:       lv_bright_up_animation(USE_ANIM_JPG);   break;
        case BRIGHTNESS_DOWN_SCREEN:     lv_bright_down_animation(USE_ANIM_JPG); break;
        case BLUETOOTH_SCREEN:           lv_bt_screen();                         break;
        case BLUETOOTH_SCAN_SCREEN:      lv_bt_pair_screen();                    break;
        case WIFI_SEARCH_SCREEN:         lv_wifi_search_screen();                break;
        case BLE_PAIRING_SCREEN:         lv_bt_pair_screen();                    break;
		case WIFI_UNKNOWN_SCREEN:        lv_wifi_unknown_screen();               break;
		case WIFI_DISCONNECT_SCREEN:     lv_wifi_disconnect_screen();            break;
        case DATA_SYNC_SCREEN:           lv_data_sync_screen();                  break;
        case OTA_SCREEN:                 lv_ota_screen(USE_ANIM_JPG);            break;
        case WIFI_PLUG_IN_SCREEN:        lv_wifi_plug_in_screen();               break;
        case WIFI_SYNCED_SCREEN:         lv_wifi_synced_screen();                break;
        case NFC_SCREEN:                 lv_nfc_screen();                        break;
        case NFC_LANGUAGE_SCREEN:        lv_nfc_settings_screen();               break;
        case NFC_ACTIVATION_SCREEN:      lv_nfc_activation_screen();             break;
        case NFC_CONTENT_SCREEN:         lv_nfc_content_screen();                break;
        case POWER_CHARGE_SCREEN:        lv_power_screen(USE_ANIM_JPG);          break;
        case POWER_FULL_SCREEN:          lv_power_screen(USE_ANIM_JPG);          break;
		case FAC_RESET_SCREEN:			 lv_facReset_screen();					 break;
        case COUNTDOWN_SCREEN:			 lv_countdown_screen();					 break;
        case POWER_ON_KID_SCREEN:        lv_kid_screen();                        break;
        case NFC_WIFI_SEARCH_SCREEN:     lv_nfc_wifi_search_screen();            break;
        case NFC_WIFI_DISCONNECT_SCREEN: lv_nfc_wifi_disc_screen();              break;
        case NFC_NO_CONTENT_SCREEN:      lv_nfc_no_content_screen();             break;
        case POWER_LOW_PLUG_IN_SCREEN:   lv_power_plug_in_screen();  			 break;
        case VOLUME_SCREEN:              lv_volume_screen(USE_ANIM_JPG);         break;
        case WIFI_SYNC_MAI_SCREEN:       lv_wifi_sync_mai_screen(USE_ANIM_JPG);  break;
        case WIFI_SYNC_ERR_SCREEN:       lv_wifi_sync_err_screen(USE_ANIM_JPG);  break;
        case WIFI_SYNC_SUC_SCREEN:       lv_wifi_sync_suc_screen(USE_ANIM_JPG);  break;
        case WIFI_SYNC_N_SCREEN:         lv_wifi_sync_n_screen(USE_ANIM_JPG);    break;
        case WIFI_ERR_SCREEN:            lv_wifi_err_screen(USE_ANIM_JPG);       break;
        case ACC_INV_FAC_RESET_SCREEN:	 lv_facReset_accInv_screen();			  break;

        default:                         lv_dummy_screen();                       break;
    }

    // Mark boot as completed when HOME_SCREEN is displayed (not just transitioning away from boot)
    if (s3_current_screen == HOME_SCREEN && !s3_boot_completed) {
        s3_boot_completed = true;
        ESP_LOGI(TAG, "[BOOT_COMPLETED] Home screen reached, battery/charger events now enabled");
    }

    if (s3_current_screen == POWER_LOW_PLUG_IN_SCREEN && s3_preLowBattery_screen != NULL_SCREEN){
        s3_next_screen = s3_preLowBattery_screen;
        s3_preLowBattery_screen = NULL_SCREEN;
    }

    // Flag is now cleared within lv_player_screen() itself after successful redraw

    if (s3_next_screen == NULL_SCREEN)
    {
        ESP_LOGI(TAG, "[SCREEN_UPDATE] MANUAL: [%d]->[x][%ds]", s3_current_screen, s3_recover.duration_ms);

        // Execute post-transition callback only after the final transition
        if (pending_callback) {
            ESP_LOGI(TAG, "[SCREEN_UPDATE] Executing post-transition callback after final screen transition");
            post_transition_cb_t cb = pending_callback;

            // Clear callback before executing to prevent re-entry
            pending_callback = NULL;

            // Force LVGL to complete all rendering to display before callback
            // This ensures screen is fully flushed to SPI before audio starts
            lv_refr_now(NULL);

            // Execute callback after unlocking GUI to prevent deadlocks
            gui_unlock();
            if (cb != NULL) {
                cb();
            } else {
                ESP_LOGE(TAG, "[SCREEN_UPDATE] post-transition callback not set");
            }
            gui_lock();
        }

        lv_timer_pause(screen_timer);
    } else
    {
        ESP_LOGI(TAG, "[SCREEN_UPDATE] AUTO: [%d]->[%d][%dms]", s3_current_screen, s3_next_screen, s3_recover.duration_ms);
        
        // Execute single transition callback after the FIRST transition (when screen is displayed)
        if (pending_single_callback) {
            ESP_LOGI(TAG, "[SCREEN_UPDATE] Executing single-transition callback for screen [%d]", s3_current_screen);
            post_transition_cb_t cb = pending_single_callback;
            
            // Clear callback before executing to prevent re-entry
            pending_single_callback = NULL;
            
            // Execute callback after unlocking GUI to prevent deadlocks
            gui_unlock();
            cb();
            gui_lock();
        }

        s3_current_screen = s3_next_screen;
        s3_next_screen = NULL_SCREEN;
        lv_timer_set_period(screen_timer, s3_recover.duration_ms);
        lv_timer_reset(screen_timer);
        lv_timer_resume(screen_timer);
    }

    gui_unlock();
}

void set_current_screen(s3_screens_t current_screen, s3_screens_t next_screen)
{
    gui_lock();

    ESP_LOGI(TAG, "[SCREEN_TRANSITION]: New screen [%d]", current_screen);

    if (current_screen < 0 || current_screen >= SCREENS_QTD) {
        ESP_LOGI(TAG, "Invalid screen option");
        gui_unlock();
        return;
    }

    s3_current_screen = current_screen;
    s3_next_screen = next_screen;
	ESP_LOGD(TAG, "[set_current_screen] s3_previous_screen[%d] s3_current_screen [%d] s3_next_screen[%d]", s3_previous_screen, s3_current_screen, s3_next_screen);
    lv_timer_set_period(screen_timer, INSTANT_TRANSITION);
    lv_timer_reset(screen_timer);
    lv_timer_resume(screen_timer);

    gui_unlock();
}

void set_last_transition_callback(post_transition_cb_t callback)
{
    gui_lock();
    ESP_LOGI(TAG, "set_last_transition_callback: Setting callback for next transition %p", callback);
    pending_callback = callback;
    gui_unlock();
}

void set_first_transition_callback(post_transition_cb_t callback)
{
    gui_lock();
    ESP_LOGI(TAG, "set_first_transition_callback: Setting callback for single transition");
    pending_single_callback = callback;
    gui_unlock();
}

int get_current_screen(void)
{
    gui_lock();
    int screen = s3_current_screen;
    gui_unlock();
    return screen;
}

int get_previous_screen(void)
{
    gui_lock();
    // s3_previous_screen already tracks the last BASE_ANCHOR screen visited
    // This serves as our base screen for navigation
    int screen = s3_previous_screen;
    gui_unlock();
    return screen;
}

void deinit_screen_manager(void)
{
    if (screen_timer) {
        lv_timer_del(screen_timer);
        screen_timer = NULL;
        ESP_LOGW(TAG, "Screen manager timer deleted.");
    }
}

void lvgl_tick_inc_locked(uint32_t inc_ms)
{
    gui_lock();
    lv_tick_inc(inc_ms);
    gui_unlock();
}

void lvgl_process_step(uint32_t delay_ms)
{
    gui_lock();
    lv_timer_handler();
    lv_tick_inc(delay_ms);
    gui_unlock();
}

static void ui_add_top_badge(void)
{
	bool is_charging = (s3_charger_status == BATTERY_CHARGE);
	bool bt_connected = (s3_pairing_status == BT_PAIRED);

	// Find existing badges
	lv_obj_t *existing_batt = find_badge(CONTENT_TYPE_BATTERY_BADGE, BADGE_SUBTYPE_TOP_BATTERY);
	lv_obj_t *existing_bt = find_badge(CONTENT_TYPE_BT_BADGE, BADGE_SUBTYPE_TOP_BT);

	// Handle battery badge
	if (is_charging) {
		if (lvgl_load_content_png(CONTENT_TYPE_BATTERY_BADGE, ICON_BATT_CHARGING_PNG) == ESP_OK) {
			const lv_img_dsc_t *batt_dsc = lvgl_get_content_dsc(CONTENT_TYPE_BATTERY_BADGE);
			if (batt_dsc && batt_dsc->data) {
				lv_obj_t *img_batt = existing_batt;

				if (!img_batt) {
					// Create new battery badge
					img_batt = lv_img_create(lv_scr_act());

					// Set user data: type=BATTERY_BADGE, subtype=TOP_BATTERY, status=CHARGING
					uint32_t user_data = make_badge_user_data(
						CONTENT_TYPE_BATTERY_BADGE,
						BADGE_SUBTYPE_TOP_BATTERY,
						BATT_STATUS_CHARGING
					);
					lv_obj_set_user_data(img_batt, (void*)(uintptr_t)user_data);
				}

				lv_img_set_src(img_batt, batt_dsc);
				lv_obj_set_pos(img_batt, bt_connected ? 82 : 98, 6);
				lv_obj_move_foreground(img_batt);
			} else {
				ESP_LOGE(TAG, "ui_add_top_badge: battery descriptor invalid");
			}
		} else {
			ESP_LOGE(TAG, "ui_add_top_badge: cannot load %s", ICON_BATT_CHARGING_PNG);
		}
	} else if (existing_batt) {
		// Not charging but badge exists - remove it
		lv_obj_del(existing_batt);
	}

	// Handle BT badge
	if (bt_connected) {
		if (lvgl_load_content_png(CONTENT_TYPE_BT_BADGE, ICON_MINI_CONN) == ESP_OK) {
			const lv_img_dsc_t *bt_dsc = lvgl_get_content_dsc(CONTENT_TYPE_BT_BADGE);
			if (bt_dsc && bt_dsc->data) {
				lv_obj_t *img_bt = existing_bt;

				if (!img_bt) {
					// Create new BT badge
					img_bt = lv_img_create(lv_scr_act());

					// Set user data: type=BT_BADGE, subtype=TOP_BT, status=CONNECTED
					uint32_t user_data = make_badge_user_data(
						CONTENT_TYPE_BT_BADGE,
						BADGE_SUBTYPE_TOP_BT,
						BT_STATUS_CONNECTED
					);
					lv_obj_set_user_data(img_bt, (void*)(uintptr_t)user_data);
				}

				lv_img_set_src(img_bt, bt_dsc);
				lv_obj_set_pos(img_bt, is_charging ? 114 : 98, 6);
				lv_obj_move_foreground(img_bt);
			} else {
				ESP_LOGE(TAG, "ui_add_top_badge: bt descriptor invalid");
			}
		} else {
			ESP_LOGE(TAG, "ui_add_top_badge: cannot load %s", ICON_MINI_CONN);
		}
	} else if (existing_bt) {
		// Not connected but badge exists - remove it
		lv_obj_del(existing_bt);
	}

	if (!is_charging && !bt_connected) {
		ESP_LOGD(TAG, "ui_add_top_badge: nothing to show (not charging, bt disconnected)");
	}
}
