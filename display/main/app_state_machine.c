/**
 * @file app_state_machine.c
 * @author Igor Oliveira
 * @date 2025-06-09
 * @brief State machine logic for application UI and behavior
 *
 * Implements the state machine transitions triggered by button presses,
 * timeouts, alarm events, and NFC detection. Provides a centralized control
 * layer for managing screen flow and high-level application state.
 */

#include "app_state_machine.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "app_screen.h"
#include "backlight.h"
#include "lv_screen_mgr.h"
#include "nfc-service.h"
#include "audio_player.h"
#include "s3_bluetooth.h"
#include "esp_bt.h"
#include "WiFi.h"
#include "esp_bt_main.h"
#include "alarm_mgr.h"
#include "s3_alarm_mgr.h"
#include "sntp_syncer.h"
#include "WiFi.h"
#include "clock.h"
#include "power_management.h"
#include "lvgl.h"
#include <string.h>
#include "s3_https_cloud.h"
// bt_manager.h functionality now in s3_bluetooth.h
#include "s3_nfc_handler.h"
#include "audio_thread.h"
#include "app_timeout.h"
#include "s3_album_mgr.h"

static const char *TAG = "APP_STATE";

static QueueHandle_t nfc_event_queue;
static audio_thread_t nfc_worker_thread = NULL;
static uint8_t last_uid[NFC_UID_LEN] = {0};
static int last_uid_len = 0;
static uint32_t last_detect_time = 0;
bool gWiFi_SYNC_USER_INTERRUPT = false;

// Debouncing for pause/resume to prevent pipeline corruption from fast clicks
static uint32_t last_pause_resume_time = 0;
#define PAUSE_RESUME_COOLDOWN_MS 500

// Timer for data sync wait screen (1 second display)
static esp_timer_handle_t data_sync_wait_timer = NULL;

// system_transition_in_progress is now defined in s3_definitions.c

// Callback to clear system busy flag after screen refresh completes
static void clear_transition_flag(void) {
    system_transition_in_progress = false;
    ESP_LOGI(TAG, "System transition complete - buttons re-enabled");
}

// Timer callback to restore normal data sync screen after wait screen
static void data_sync_wait_timer_callback(void* arg) {
    ESP_LOGI(TAG, "Data sync wait timer expired - restoring normal sync screen");
    s3_data_sync_show_wait = false;

    // Stop and delete the timer
    if (data_sync_wait_timer) {
        esp_timer_stop(data_sync_wait_timer);
        esp_timer_delete(data_sync_wait_timer);
        data_sync_wait_timer = NULL;
    }

    // Refresh the screen to show the normal sync stage image
    set_current_screen(DATA_SYNC_SCREEN, NULL_SCREEN);
}

// NFC sync data storage for callbacks
static char nfc_sync_sku[32] = {0};
static uint8_t nfc_sync_uid[7] = {0};
static int nfc_sync_context = NFC_SYNC_CONTEXT_CONTENT_UPDATE;
static bool nfc_sync_attempted = false; // Track if sync was attempted for current tag

static void handle_home_screen(AppEvent event);
static void handle_bluetooth_screen(AppEvent event);

static s3_screens_t current_state = NULL_SCREEN;
static s3_screens_t next_state = NULL_SCREEN;
static int system_vol = 1;      // 0 - 4
static int system_bright = 2;   // 0 - 2
static int system_alarm = 0;    // 0 - 6
static char *history_resource;
extern s3_screens_t s3_preLowBattery_screen;
// Global variable wifi_pairing_defer_timer is defined in s3_definitions.c and declared in s3_definitions.h

// Track the screen before entering VOLUME_SCREEN for proper return behavior
static s3_screens_t volume_screen_entry_source = NULL_SCREEN;

// Track the screen before entering FAC_RESET_SCREEN for proper return behavior
static s3_screens_t fac_reset_screen_entry_source = NULL_SCREEN;

/**
 * @brief Centralized NFC state management based on screen type
 * @param screen The screen to set NFC state for
 */
static void manage_nfc_state(s3_screens_t screen)
{
    switch (screen) {
        case HOME_SCREEN:
            ESP_LOGI(TAG, "NFC RESUME for screen: %d", screen);
            rst_is_on_blankee_flg();
            
            // Only check for audio pipeline cleanup when transitioning to HOME
            // This prevents resource conflicts after returning from PLAY screen
            int wait_attempts = 0;
            const int max_wait_attempts = 30; // 300ms max wait (10ms * 30)
            
            while (audio_player_is_running() && wait_attempts < max_wait_attempts) {
                ESP_LOGI(TAG, "NFC resume: Waiting for audio pipeline cleanup... (attempt %d/%d)", wait_attempts + 1, max_wait_attempts);
                vTaskDelay(pdMS_TO_TICKS(10));
                wait_attempts++;
            }
            
            if (wait_attempts > 0) {
                ESP_LOGI(TAG, "NFC resume: Audio pipeline cleanup completed after %d attempts", wait_attempts);
            }
            
            nfc_resume();
            ESP_LOGI(TAG, "NFC resumed for HOME screen after audio check");
            break;
            
        case PLAY_SCREEN:
        case PAUSE_SCREEN:
        case STANDBY_SCREEN:
        case CLOCK_SCREEN:
            ESP_LOGI(TAG, "NFC RESUME for screen: %d", screen);
            nfc_resume();
            ESP_LOGI(TAG, "NFC resumed for screen: %d", screen);
            break;

        default:
            ESP_LOGI(TAG, "NFC PAUSE for screen: %d", screen);
            nfc_pause();
            break;
    }
}


/**
 * @brief Unified dimmer management on any user interaction
 * @details Called once per event to handle dimmer timer and backlight restoration:
 *          - Restores backlight if dimmed
 *          - Resets dimmer timer efficiently
 *          - Centralized management like app_timeout_reset()
 */
static void manage_dimmer_on_user_input(void)
{
    if (is_screen_dimmed()) {
        ESP_LOGI(TAG, "Screen is dimmed, restoring backlight on user input");
        undimmed_backlight();
    }
}

static void restart_dimmer_timer_if_paused(void)
{
    if (get_current_screen() == PLAY_SCREEN) {
        restart_dimmer_timer();
    }
}

// NFC sync functions moved to s3_nfc_handler.c



// NFC worker task is implemented in s3_nfc_handler.c

// This callback handles events reported by the BT Manager
static void on_bt_status_changed(bt_manager_status_t status) {
    ESP_LOGI(TAG, "Callback received: BT Manager reported new status: %d", status);
    
    switch (status) {
        case BT_STATUS_OFF:
            s3_pairing_status = BT_UNPAIRED;
            // If we're on a BT scan screen, return to BLUETOOTH_SCREEN
            if (get_current_screen() == BLUETOOTH_SCAN_SCREEN) {
                ESP_LOGI(TAG, "BT went IDLE from scanning, returning to BLUETOOTH_SCREEN");
                set_current_screen(BLUETOOTH_SCREEN, NULL_SCREEN);
            } else if (get_current_screen() == BLUETOOTH_SCREEN) {
                ESP_LOGI(TAG, "BT went IDLE on BLUETOOTH_SCREEN, refreshing display to show disconnected state");
                refresh_screen_display();
            }
            break;

        case BT_STATUS_SCANNING:
            s3_pairing_status = BT_SCAN;
            set_current_screen(BLUETOOTH_SCAN_SCREEN, NULL_SCREEN);
            break;

        case BT_STATUS_CONNECTED:
            s3_pairing_status = BT_PAIRED;
            
            // Clear global reconnect flag
            gBTReconnectInProgress = false;
            
            // Check if we came from BLUETOOTH_SCAN_SCREEN (reconnect scenario)
            if (get_current_screen() == BLUETOOTH_SCAN_SCREEN) {
                // Check if audio was paused due to BT disconnect (flag set by pause_audio_for_bt_disconnect)
                if (is_audio_paused()) {
                    ESP_LOGI(TAG, "BT reconnected successfully - resuming audio on A2DP and returning to PLAY_SCREEN");
                    resume_audio_after_bt_reconnect();
                    set_current_screen(PLAY_SCREEN, NULL_SCREEN);
                } else {
                    // No audio was playing, return to previous screen (HOME or other)
                    s3_screens_t prev = (s3_screens_t)get_previous_screen();
                    ESP_LOGI(TAG, "BT reconnected - returning to previous screen %d", prev);
                    set_current_screen(prev != NULL_SCREEN ? prev : HOME_SCREEN, NULL_SCREEN);
                }
            } else if (get_current_screen() == PLAY_SCREEN) {
                ESP_LOGI(TAG, "BT connected while on PLAY_SCREEN - resuming audio and staying on PLAY_SCREEN");
                resume_audio_after_bt_reconnect();
                refresh_screen_display(); // Update BT icon
            } else if (get_current_screen() == HOME_SCREEN) {
                ESP_LOGI(TAG, "BT connected while on HOME_SCREEN - staying and refreshing BT icon");
                refresh_screen_display(); // Update BT icon to show connected
            } else {
                // Shows "Connected" screen, then transitions to the BT menu (only when in setup/menus)
                set_current_screen(BLUETOOTH_SCAN_SCREEN, BLUETOOTH_SCREEN); 
            }
            break;

        case BT_STATUS_FAILED: // Failed after all retries
            s3_pairing_status = BT_UNPAIRED; // Show unpaired icon (NOT timeout)
            
            // Clear global reconnect flag
            gBTReconnectInProgress = false;
            
            // Check if we came from BLUETOOTH_SCAN_SCREEN (reconnect scenario)
            if (get_current_screen() == BLUETOOTH_SCAN_SCREEN) {
                // Check if audio was paused due to BT disconnect
                if (is_audio_paused()) {
                    ESP_LOGI(TAG, "BT reconnection failed - resuming audio on I2S from beginning and returning to PLAY_SCREEN");
                    
                    // Clear the BT disconnect flag since reconnection failed
                    clear_bt_disconnect_pause_flag();
                    
                    // Stop the A2DP pipeline
                    play_stop();
                    
                    // Switch sink to I2S
                    s3_active_sink = AUDIO_SINK_I2S;
                    
                    // Give time for cleanup
                    vTaskDelay(pdMS_TO_TICKS(100));
                    
                    // Restart playback on I2S from beginning (cannot preserve position across sink change)
                    play_album();
                    
                    ESP_LOGI(TAG, "Audio restarted on I2S after BT reconnection failed");
                    
                    // Return to PLAY_SCREEN
                    set_current_screen(PLAY_SCREEN, NULL_SCREEN);
                } else {
                    // No audio was playing, return to previous screen (HOME or other)
                    s3_screens_t prev = (s3_screens_t)get_previous_screen();
                    ESP_LOGI(TAG, "BT reconnection failed - returning to previous screen %d", prev);
                    set_current_screen(prev != NULL_SCREEN ? prev : HOME_SCREEN, NULL_SCREEN);
                }
            } else if (get_current_screen() == HOME_SCREEN) {
                ESP_LOGI(TAG, "BT reconnection failed on HOME_SCREEN - staying and showing unpaired badge");
                refresh_screen_display(); // Update BT badge to show unpaired
            } else if (get_current_screen() == PLAY_SCREEN || get_current_screen() == PAUSE_SCREEN) {
                ESP_LOGI(TAG, "BT reconnection failed on PLAY/PAUSE_SCREEN - switching to I2S");
                if (is_audio_paused()) {
                    // Switch to I2S and restart playback
                    clear_bt_disconnect_pause_flag();
                    play_stop();
                    s3_active_sink = AUDIO_SINK_I2S;
                    vTaskDelay(pdMS_TO_TICKS(100));
                    play_album();
                    
                    // Transition to PLAY_SCREEN if on PAUSE_SCREEN
                    if (get_current_screen() == PAUSE_SCREEN) {
                        set_current_screen(PLAY_SCREEN, NULL_SCREEN);
                    }
                } else {
                    // Not paused, just switch sink for next play
                    s3_active_sink = AUDIO_SINK_I2S;
                }
                refresh_screen_display(); // Update BT badge to show unpaired
            } else {
                // Only go to bluetooth menu if we're in other screens/menus
                play_stop();
                stop_dimmer();
                ESP_LOGW(TAG, "Bluetooth connection failed after all retries, returning to setup mode BT");
                // Reset pairing status to UNPAIRED so next connection shows correct icon
                s3_pairing_status = BT_UNPAIRED;
                set_current_screen(BLUETOOTH_SCREEN, NULL_SCREEN);
            }
            break;

        case BT_STATUS_RECONNECTING:
            s3_pairing_status = BT_SCAN; // Show searching icon during reconnect
            
            // Set global flag to block alarms during reconnect
            gBTReconnectInProgress = true;
            
            // If playing audio on PLAY_SCREEN, pause it first before showing scan screen
            if (get_current_screen() == PLAY_SCREEN && is_audio_playing()) {
                ESP_LOGI(TAG, "BT reconnecting - pausing audio before showing scan screen");
                play_pause(); // Pause pipeline directly, no screen transition to PAUSE_SCREEN
            }
            
            // Always transition to BLUETOOTH_SCAN_SCREEN for all screens during reconnect
            ESP_LOGI(TAG, "BT reconnecting - showing BLUETOOTH_SCAN_SCREEN (previous screen was %d)", get_current_screen());
            set_current_screen(BLUETOOTH_SCAN_SCREEN, NULL_SCREEN);
            break;

        default:
            ESP_LOGW(TAG, "Unknown BT status: %d", status);
            break;
    }
}

void app_state_init(void) {
    lv_timer_t *screen_manager = init_screen_manager(false);
    // nfc_enable();
    
    if(is_wakeup_from_alarm() == true)
    {
        alarm_from_deep_sleep();
    }
    else
    {
        current_state = BOOT_SCREEN;
        next_state = POWER_ON_KID_SCREEN;
        set_current_screen(current_state, next_state);
        play_audio_boot();
        get_alarm_setting(TIMER_SOURCE_ESP_TIMER);
    }
 
    // BT manager initialization deferred until user accesses BT menu
    // bt_manager_init(on_bt_status_changed);  // This will be called when user first accesses BT

    setup_state_handle_cb(&app_state_handle_event);
    app_timeout_init();
    app_timeout_deepsleep_init();
    app_timeout_restart();

    ESP_LOGI(TAG, "Initial state: BOOT_SCREEN");
}

/**
 * @brief Handles state transitions based on events.
 * @note The EVENT_BTN_D_SHORT functionality to return to HOME_SCREEN
 * is temporary for testing and should be removed in production.
 */
// Brightness preview management functions
void brightness_preview_up(void)
{
    ESP_LOGI(TAG, "brightness_preview_up()");
 
    if (s3_brightness_level < BRIGHTNESS_LEVEL_3) {
        s3_brightness_level++;
        // Update hardware for immediate preview (temporary change)
        increase_backlight_temp();
        ESP_LOGI(TAG, "Brightness preview increased to level %d (not saved to NVS)", s3_brightness_level);
    } else {
        ESP_LOGW(TAG, "Brightness already at maximum level");
    }
}

void brightness_preview_down(void)
{
    ESP_LOGI(TAG, "brightness_preview_down()");
    
    if (s3_brightness_level > BRIGHTNESS_LEVEL_1) {
        s3_brightness_level--;
        // Update hardware for immediate preview (temporary change)
        decrease_backlight_temp();
        ESP_LOGI(TAG, "Brightness preview decreased to level %d (not saved to NVS)", s3_brightness_level);
    } else {
        ESP_LOGW(TAG, "Brightness already at minimum level");
    }
}

void brightness_confirm_and_save(void)
{
    ESP_LOGI(TAG, "brightness_confirm_and_save() - saving level %d to NVS", s3_brightness_level);
    set_backlight(s3_brightness_level); // This will save to NVS and set hardware
}

void brightness_cancel_and_restore(void)
{
    ESP_LOGI(TAG, "brightness_cancel_and_restore() - restoring from NVS");
    backlight_on(); // This loads from NVS and sets hardware
    s3_brightness_level = get_backlight(); // Sync UI variable with restored value
    ESP_LOGI(TAG, "Brightness restored to NVS value: %d", s3_brightness_level);
}

void app_state_handle_event(AppEvent event) {
    current_state = get_current_screen();
    ESP_LOGI(TAG, "Received event: %d in state: %d", event, current_state);

    // Safety check: If alarm timeout happens outside ALARM_SCREEN, stop the timer immediately
    // This prevents the timer from continuing to generate events every 10 seconds
    if (event == EVENT_ALARM_AUTO_DISMISS && current_state != ALARM_SCREEN) {
        ESP_LOGW(TAG, "EVENT_ALARM_AUTO_DISMISS received outside ALARM_SCREEN (state=%d) - stopping alarm timeout timer", current_state);
        stop_alarm_timeout_timer();
        return; // Don't process this event further as it's a cleanup operation
    }

    if (event == EVENT_ENTER_STANDBY) {
        // Allow standby when audio is paused, but not when actively playing
        if (audio_player_is_running() && !is_audio_paused()) {
            ESP_LOGW(TAG, "Standby timeout ignored because audio pipeline is actively playing.");
            app_timeout_restart();
            return;
        }
        // Ignore standby when in Bluetooth scan screen
        if (current_state == BLUETOOTH_SCAN_SCREEN) {
            ESP_LOGW(TAG, "Standby timeout ignored because Bluetooth scan is active.");
            app_timeout_restart();
            return;
        }
        ESP_LOGI(TAG, "Inactivity timeout! Entering Standby.");
        set_current_screen(STANDBY_SCREEN, NULL_SCREEN);
        if(is_clock_initialized() == true)
        {
            deinit_clock();
        }

        stop_dimmer();
        backlight_off();
        //nfc_disable();
        nfc_pause();
        app_timeout_stop();
        if(!global_plugged_in)
            app_timeout_deepsleep_start();
        return;
    }

    // Only restart timeout if not in pause state - we want timers to continue in pause state
    if (current_state != PAUSE_SCREEN) {
        app_timeout_restart();
    }
    
    // Check if screen is dimmed before managing dimmer
    bool was_screen_dimmed = is_screen_dimmed();

    // Only manage dimmer for actual user input events (keys and NFC)
    if (event <= EVENT_BTN_MACRO_A_N_B_LONG || event == EVENT_NFC_DETECTED) {
        manage_dimmer_on_user_input();
    }

    switch (current_state) {
        case HOME_SCREEN:
            ESP_LOGI(TAG, "HOME_SCREEN: Audio playing: %s, paused: %s", is_audio_playing() ? "true" : "false", is_audio_paused() ? "true" : "false");
            if (event == EVENT_BTN_A_SHORT) {
                ESP_LOGI(TAG, "HOME_SCREEN → HOME_SCREEN[NEXT ALBUM]");
                current_state = HOME_SCREEN;
                next_state = NULL_SCREEN;

                one_step_album(VALUE_DOWN);
            } else if (event == EVENT_BTN_B_SHORT) {
                ESP_LOGI(TAG, "HOME_SCREEN → HOME_SCREEN[PREVIOUS ALBUM");
                current_state = HOME_SCREEN;
                next_state = NULL_SCREEN;

                one_step_album(VALUE_UP);
            } else if (event == EVENT_BTN_A_LONG) {
            } else if (event == EVENT_BTN_B_LONG) {
            } else if (event == EVENT_BTN_A_CONTINUOUS) {
            } else if (event == EVENT_BTN_B_CONTINUOUS) {
            } else if (event == EVENT_BTN_C_SHORT) {
                ESP_LOGI(TAG, "HOME_SCREEN → PLAY_SCREEN");
                current_state = PLAY_SCREEN;
                next_state = NULL_SCREEN;

                sys_memory_status(TAG, "Before transition");
                set_last_transition_callback((post_transition_cb_t) play_album); // make sure the audio start after cover transition
                sys_memory_status(TAG, "After transition and play callback");

            } else if (event == EVENT_BTN_C_LONG) {
                ESP_LOGI(TAG, "HOME_SCREEN → VOLUME_SCREEN");
                volume_screen_entry_source = HOME_SCREEN;
                current_state = VOLUME_SCREEN;
                next_state = NULL_SCREEN;
            } else if (event == EVENT_BTN_D_SHORT) {
                ESP_LOGI(TAG, "HOME_SCREEN → CLOCK_SCREEN");
                current_state = CLOCK_SCREEN;
                next_state = NULL_SCREEN;

                setup_clock_update_screen_cb(refresh_screen_display);
                init_clock();
            } else if (event == EVENT_BTN_D_LONG) {
                ESP_LOGI(TAG, "HOME_SCREEN → SHUTDOWN");
                global_poweroff = POWER_MODE_SHUTDOWN;
                current_state = SHUTDOWN_SCREEN;
                next_state = POWER_OFF_SCREEN;
            } else if(event == EVENT_BTN_MACRO_B_N_C_LONG) {
                ESP_LOGI(TAG, "HOME_SCREEN → WIFI PAIRING");
                current_state = NULL_SCREEN;
                next_state = NULL_SCREEN;
            } else if(event == EVENT_BTN_MACRO_A_N_D_LONG) {
                ESP_LOGI(TAG, "HOME_SCREEN → FAC_RESET_SCREEN");
                fac_reset_screen_entry_source = HOME_SCREEN;
                current_state = FAC_RESET_SCREEN;
                next_state = NULL_SCREEN;
            } else if (event == EVENT_NFC_DETECTED) {
                ESP_LOGI(TAG, "HOME_SCREEN → PLAY_SCREEN [NFC_DETECTED]");
                current_state = PLAY_SCREEN;
                next_state = NULL_SCREEN;
                
                set_last_transition_callback((post_transition_cb_t) play_album); // Start audio after screen transition
            } else if (event == EVENT_LEAVE_STANDBY) {
                ESP_LOGI(TAG, "HOME_SCREEN → [UPDATE_MINI_ICONS]");
                current_state = HOME_SCREEN;
                next_state = NULL_SCREEN;
            } else {
                ESP_LOGI(TAG, "HOME_SCREEN → [NO_ACTION_DEFINED]");
                current_state = NULL_SCREEN;
                next_state = NULL_SCREEN;
            }
            break;

        case PLAY_SCREEN:
            // Block button events during audio/screen transitions to prevent pipeline corruption
            if (system_transition_in_progress) {
                ESP_LOGW(TAG, "PLAY_SCREEN button event %d ignored - transition in progress", event);
                break;
            }

            if(was_screen_dimmed == true) {
                current_state = NULL_SCREEN;
                next_state = NULL_SCREEN;
                restart_dimmer_timer_if_paused();
                return;
            } else if (event == EVENT_BTN_A_SHORT) {
                // Don't allow track changes when paused
                if (is_audio_paused()) {
                    ESP_LOGW(TAG, "PLAY_SCREEN: Button A ignored (audio is paused)");
                    return;
                }

                ESP_LOGI(TAG, "PLAY_SCREEN → PLAY_SCREEN[PREVIOUS TRACK]");
                current_state = PLAY_SCREEN;
                next_state = NULL_SCREEN;

                play_stop();
                audio_play_previous_album_track();
                enable_lang_badge_update(); // Enable language badge display for new track
                set_last_transition_callback((post_transition_cb_t) audio_start_playing);
            } else if (event == EVENT_BTN_B_SHORT) {
                // Don't allow track changes when paused
                if (is_audio_paused()) {
                    ESP_LOGW(TAG, "PLAY_SCREEN: Button B ignored (audio is paused)");
                    return;
                }

                ESP_LOGI(TAG, "PLAY_SCREEN → PLAY_SCREEN[NEXT TRACK]");
                current_state = PLAY_SCREEN;
                next_state = NULL_SCREEN;

                play_stop();
                audio_play_next_album_track();
                enable_lang_badge_update(); // Enable language badge display for new track
                set_last_transition_callback((post_transition_cb_t) audio_start_playing);
            } else if (event == EVENT_BTN_C_SHORT) {
                ESP_LOGI(TAG, "PLAY_SCREEN → PAUSE");
                current_state = PAUSE_SCREEN;
                next_state = NULL_SCREEN;

                // Block pause/resume during state transitions (PAUSING/RESUMING) to prevent pipeline corruption
                if (!is_state_stable()) {
                    ESP_LOGW(TAG, "Pause/Resume ignored - audio state transition in progress");
                    break;
                }

                // Cooldown check to prevent fast clicks from corrupting pipeline
                //uint32_t current_time = esp_timer_get_time() / 1000; // Convert to milliseconds
                //uint32_t elapsed = current_time - last_pause_resume_time;

                //if (last_pause_resume_time != 0 && elapsed < PAUSE_RESUME_COOLDOWN_MS) {
                //    ESP_LOGW(TAG, "Pause/Resume ignored - too fast! Wait %ums (elapsed: %ums)",
                //             PAUSE_RESUME_COOLDOWN_MS - elapsed, elapsed);
                //    break;
                //}

                // Play/Pause toggle: check paused first, then playing, then stopped
                if (is_audio_playing()) {
                    // Start standby and deep sleep timers when pausing
                    ESP_LOGI(TAG, "Starting standby and deep sleep timers for pause state");
                    app_timeout_restart(); // Start 2-minute standby timer
                    if (!global_plugged_in) {
                        app_timeout_deepsleep_start(); // Start 10-minute deep sleep timer if not plugged in
                    }
                } else {
                    ESP_LOGI(TAG, "Audio is stopped, starting playback...");
                    play_album();
                    // Don't update timestamp - play_album is not a pause/resume toggle
                }
            } else if (event == EVENT_BTN_C_LONG) {
                ESP_LOGI(TAG, "PLAY_SCREEN → VOLUME_SCREEN");
                volume_screen_entry_source = PLAY_SCREEN;
                current_state = VOLUME_SCREEN;
                next_state = NULL_SCREEN;
            } else if ((event == EVENT_BTN_D_SHORT) || (event == EVENT_LEAVE_PLAYING_TO_HOME)) {
                // Don't allow home navigation when paused
                if (is_audio_paused()) {
                    ESP_LOGW(TAG, "PLAY_SCREEN: Button D ignored (audio is paused)");
                    return;
                }

                ESP_LOGI(TAG, "PLAY_SCREEN → HOME_SCREEN");
                current_state = HOME_SCREEN;
                next_state = NULL_SCREEN;

                ESP_LOGI(TAG, "Audio playing status: %s", is_audio_playing() ? "true" : "false");
                ESP_LOGI(TAG, "Audio paused status: %s", is_audio_paused() ? "true" : "false");

                stop_dimmer();
                play_stop();

                // Clear BT disconnect pause flag when leaving PLAY_SCREEN
                clear_bt_disconnect_pause_flag();

                reset_albums_from_nfc(); // also do play_stop() inside
                vTaskDelay(pdMS_TO_TICKS(50));
            } else if (event == EVENT_BTN_D_LONG) {
                ESP_LOGI(TAG, "PLAY_SCREEN → SHUTDOWN");
                global_poweroff = POWER_MODE_SHUTDOWN;
                current_state = SHUTDOWN_SCREEN;
                next_state = POWER_OFF_SCREEN;
            } else if (event == EVENT_BTN_MACRO_A_N_D_LONG) {
                ESP_LOGI(TAG, "PLAY_SCREEN → FAC_RESET_SCREEN");
                fac_reset_screen_entry_source = PLAY_SCREEN;
                current_state = FAC_RESET_SCREEN;
                next_state = NULL_SCREEN;
            } else if (event == EVENT_NFC_DETECTED) {
                ESP_LOGI(TAG, "PLAY_SCREEN → PLAY_SCREEN [NFC_DETECTED]");
                current_state = PLAY_SCREEN;
                next_state = NULL_SCREEN;

                set_last_transition_callback((post_transition_cb_t) play_album); // Start audio after screen transition
            } else if (event == EVENT_LEAVE_STANDBY) {
                ESP_LOGI(TAG, "PLAY_SCREEN → [UPDATE_MINI_ICONS]");
                current_state = PLAY_SCREEN;
                next_state = NULL_SCREEN;
            } else {
                if(is_screen_dimmed() == true)
                    ESP_LOGI(TAG, "PLAY_SCREEN → [DIMMER_OFF]");
                else
                    ESP_LOGI(TAG, "PLAY_SCREEN → [NO_ACTION_DEFINED]");
                
                current_state = NULL_SCREEN;
                next_state = NULL_SCREEN;
                // C and D long also turn dimmer off
            }

            enable_player_update(); // Trigger redraw if we press anything in play screen
            break;

        case PAUSE_SCREEN:
            app_timeout_stop(); // Stop standby timer
            app_timeout_deepsleep_stop(); // Stop deep sleep timer
            next_state = NULL_SCREEN;

            if (event == EVENT_BTN_C_SHORT && is_audio_paused())
            {
                // Resume regardless of exact audio state (handles PAUSING state transition)
                current_state = PLAY_SCREEN;

                ESP_LOGI(TAG, "Resume button pressed, attempting to resume...");
                stop_dimmer();
                enable_resume_update(); // Allow pause icon update
                set_last_transition_callback((post_transition_cb_t) play_resume);

                // Stop standby and deep sleep timers when resuming playback
                ESP_LOGI(TAG, "Stopping standby and deep sleep timers when resuming playback");
            }
            else if(event == EVENT_BTN_C_SHORT && is_audio_stopped()) // if a alarm is trigged on pause screen
            {
                current_state = PLAY_SCREEN;
                set_last_transition_callback((post_transition_cb_t) play_album);
            }
            else if (event == EVENT_BTN_D_SHORT)
            {
                ESP_LOGI(TAG, "PAUSE_SCREEN → HOME_SCREEN");
                current_state = HOME_SCREEN;
                next_state = NULL_SCREEN;

                stop_dimmer();
                play_stop();

                // Clear BT disconnect pause flag when leaving PAUSE_SCREEN
                clear_bt_disconnect_pause_flag();

                reset_albums_from_nfc();
            }
            else if (event == EVENT_BTN_D_LONG)
            {
                ESP_LOGI(TAG, "PAUSE_SCREEN → SHUTDOWN");
                // Stop timers when shutting down from pause state
                global_poweroff = POWER_MODE_SHUTDOWN;
                current_state = SHUTDOWN_SCREEN;
                next_state = POWER_OFF_SCREEN;
            }
            else if (event == EVENT_BTN_MACRO_A_N_D_LONG)
            {
                ESP_LOGI(TAG, "PAUSE_SCREEN → FAC_RESET_SCREEN");
                // Stop timers when entering factory reset from pause state
                fac_reset_screen_entry_source = PAUSE_SCREEN;
                current_state = FAC_RESET_SCREEN;
            }
            else if (event == EVENT_NFC_DETECTED)
            {
                ESP_LOGI(TAG, "PAUSE_SCREEN → PLAY_SCREEN [NFC_DETECTED]");
                current_state = PLAY_SCREEN;
                next_state = NULL_SCREEN;

                // NFC handler has already built the playlist and prepared for playback
                // Set callback to start playing the new album after screen transition
                set_last_transition_callback((post_transition_cb_t) play_album);

                ESP_LOGI(TAG, "NFC detected on pause screen - will resume to PLAY_SCREEN and start new album");
            } else {
                ESP_LOGW(TAG, "Invalid key press on pause screen");
                current_state = NULL_SCREEN;
            }
            break; 

        /* Bug detected: when playing do not detect changes, so play again for while */
        case VOLUME_SCREEN:
            if (event == EVENT_BTN_A_SHORT)
            {
                if (haveNFC()){
                    ESP_LOGI(TAG, "VOLUME_SCREEN → NFC_SCREEN");
                    current_state = NFC_SCREEN;
                } else {
                    ESP_LOGI(TAG, "VOLUME_SCREEN → DISPLAY_SCREEN");
                    current_state = DISPLAY_SCREEN;
                }
                next_state    = NULL_SCREEN;
            }
            else if (event == EVENT_BTN_B_SHORT)
            {
                // Check OOB status: if OOB = 0 (factory reset), skip WIFI and go to DISPLAY
                if (s3_album_mgr_factory_reset_status()) {
                    ESP_LOGI(TAG, "VOLUME_SCREEN → DISPLAY_SCREEN (OOB=0, WIFI hidden)");
                    current_state = DISPLAY_SCREEN;
                } else {
                    ESP_LOGI(TAG, "VOLUME_SCREEN → WIFI_SYNC_MAI_SCREEN");
                    current_state = WIFI_SYNC_MAI_SCREEN;
                }
                next_state = NULL_SCREEN;
            }
            else if (event == EVENT_BTN_C_SHORT)
            {
                volume_screen_enter(); // Backup current volume before entering volume adjustment
                current_state  = VOLUME_UP_SCREEN;
                next_state = NULL_SCREEN;
            }
            else if (event == EVENT_BTN_D_SHORT)
            {
                // Return to the screen that entered VOLUME_SCREEN, default to HOME_SCREEN
                current_state = (volume_screen_entry_source != NULL_SCREEN) ?
                               volume_screen_entry_source : HOME_SCREEN;
                ESP_LOGI(TAG, "VOLUME_SCREEN → %d (return to source)", current_state);
                volume_screen_entry_source = NULL_SCREEN; // Reset after use
                next_state = NULL_SCREEN;
            }
            else if (event == EVENT_BTN_D_LONG)
            {
                ESP_LOGI(TAG, "VOLUME_SCREEN → SHUTDOWN");
                global_poweroff = POWER_MODE_SHUTDOWN;
                current_state = SHUTDOWN_SCREEN;
                next_state = POWER_OFF_SCREEN;
            }
            else if (event == EVENT_BTN_MACRO_A_N_D_LONG)
            {
                ESP_LOGI(TAG, "VOLUME_SCREEN → FAC_RESET_SCREEN");
                fac_reset_screen_entry_source = VOLUME_SCREEN;
                current_state = FAC_RESET_SCREEN;
                next_state = NULL_SCREEN;
            }
            else
            {
                ESP_LOGI(TAG, "VOLUME_SCREEN → [NO_ACTION_DEFINED]");
                current_state = NULL_SCREEN;
                next_state    = NULL_SCREEN;
            }
            break;

        case VOLUME_UP_SCREEN:
            if (event == EVENT_BTN_A_SHORT)
            {
                ESP_LOGI(TAG, "VOLUME_UP_SCREEN → VOLUME_DOWN_SCREEN");
        		decrease_volume();
                // Only play volume sound if not entered from PLAY_SCREEN (audio is playing)
                if (volume_screen_entry_source != PLAY_SCREEN) {
                    set_last_transition_callback((post_transition_cb_t) play_audio_volume);
                }
                current_state = VOLUME_DOWN_SCREEN;
            }
            else if (event == EVENT_BTN_B_SHORT)
            {
                ESP_LOGI(TAG, "VOLUME_UP_SCREEN → VOLUME_UP_SCREEN");
                increase_volume();
                // Only play volume sound if not entered from PLAY_SCREEN (audio is playing)
                if (volume_screen_entry_source != PLAY_SCREEN) {
                    set_last_transition_callback((post_transition_cb_t) play_audio_volume);
                }
                current_state  = VOLUME_UP_SCREEN;
            }
            else if (event == EVENT_BTN_C_SHORT)
            {
                ESP_LOGI(TAG, "VOLUME_UP_SCREEN → VOLUME_SCREEN [CONFIRM]");
                volume_confirm_and_save();
                current_state  = VOLUME_SCREEN;
                next_state = NULL_SCREEN;
            }
            else if (event == EVENT_BTN_D_SHORT)
            {
                // Save and return to the screen that entered VOLUME_SCREEN, default to HOME_SCREEN
                volume_confirm_and_save();
                current_state = (volume_screen_entry_source != NULL_SCREEN) ?
                               volume_screen_entry_source : HOME_SCREEN;
                ESP_LOGI(TAG, "VOLUME_UP_SCREEN → %d [SAVE, return to source]", current_state);
                volume_screen_entry_source = NULL_SCREEN; // Reset after use
            }
            else if (event == EVENT_BTN_D_LONG)
            {
                ESP_LOGI(TAG, "VOLUME_UP_SCREEN → SHUTDOWN");
                global_poweroff = POWER_MODE_SHUTDOWN;
                current_state = SHUTDOWN_SCREEN;
                next_state = POWER_OFF_SCREEN;
            }
            else if (event == EVENT_BTN_MACRO_A_N_D_LONG)
            {
                ESP_LOGI(TAG, "VOLUME_UP_SCREEN → FAC_RESET_SCREEN");
                fac_reset_screen_entry_source = VOLUME_UP_SCREEN;
                current_state = FAC_RESET_SCREEN;
                next_state = NULL_SCREEN;
            } else {
            	ESP_LOGI(TAG, "VOLUME_UP_SCREEN → [NO_ACTION_DEFINED]");
            	current_state = NULL_SCREEN;
            }
            next_state = NULL_SCREEN;
            break;

        case VOLUME_DOWN_SCREEN:
            if (event == EVENT_BTN_A_SHORT)
            {
                ESP_LOGI(TAG, "VOLUME_DOWN_SCREEN → VOLUME_DOWN_SCREEN");
        		decrease_volume();
                // Only play volume sound if not entered from PLAY_SCREEN (audio is playing)
                if (volume_screen_entry_source != PLAY_SCREEN) {
                    set_last_transition_callback((post_transition_cb_t) play_audio_volume);
                }
                current_state = VOLUME_DOWN_SCREEN;
            }
            else if (event == EVENT_BTN_B_SHORT)
            {
                ESP_LOGI(TAG, "VOLUME_DOWN_SCREEN → VOLUME_UP_SCREEN");
                increase_volume();
                // Only play volume sound if not entered from PLAY_SCREEN (audio is playing)
                if (volume_screen_entry_source != PLAY_SCREEN) {
                    set_last_transition_callback((post_transition_cb_t) play_audio_volume);
                }
                current_state  = VOLUME_UP_SCREEN;
            }
            else if (event == EVENT_BTN_C_SHORT)
            {
                ESP_LOGI(TAG, "VOLUME_DOWN_SCREEN → VOLUME_SCREEN [CONFIRM]");
                volume_confirm_and_save();
                current_state  = VOLUME_SCREEN;
                next_state = NULL_SCREEN;
            }
            else if (event == EVENT_BTN_D_SHORT)
            {
                // Save and return to the screen that entered VOLUME_SCREEN, default to HOME_SCREEN
                volume_confirm_and_save();
                current_state = (volume_screen_entry_source != NULL_SCREEN) ?
                               volume_screen_entry_source : HOME_SCREEN;
                ESP_LOGI(TAG, "VOLUME_DOWN_SCREEN → %d [SAVE, return to source]", current_state);
                volume_screen_entry_source = NULL_SCREEN; // Reset after use
            }
            else if (event == EVENT_BTN_D_LONG)
            {
                ESP_LOGI(TAG, "VOLUME_DOWN_SCREEN → SHUTDOWN");
                global_poweroff = POWER_MODE_SHUTDOWN;
                current_state = SHUTDOWN_SCREEN;
                next_state = POWER_OFF_SCREEN;
            }
            else if (event == EVENT_BTN_MACRO_A_N_D_LONG)
            {
                ESP_LOGI(TAG, "VOLUME_DOWN_SCREEN → FAC_RESET_SCREEN");
                fac_reset_screen_entry_source = VOLUME_DOWN_SCREEN;
                current_state = FAC_RESET_SCREEN;
                next_state = NULL_SCREEN;
            } else {
            	ESP_LOGI(TAG, "VOLUME_DOWN_SCREEN → [NO_ACTION_DEFINED]");
            	current_state = NULL_SCREEN;
            }
            next_state = NULL_SCREEN;
            break;

        case BLUETOOTH_SCREEN:
            if (event == EVENT_BTN_A_SHORT)
            {
                current_state  = WIFI_SYNC_MAI_SCREEN;
                next_state = NULL_SCREEN;

            }
            else if (event == EVENT_BTN_B_SHORT)
            {
                ESP_LOGI(TAG, "BLUETOOTH_SCREEN → DISPLAY_SCREEN");
                current_state = DISPLAY_SCREEN;
                next_state    = NULL_SCREEN;
            }
            else if (event == EVENT_BTN_C_SHORT)
            {
                if (bt_is_a2dp_connected()) {
                    // Check if this is a stale connection (connected but audio routing to I2S instead of A2DP)
                    if (s3_active_sink == AUDIO_SINK_I2S) {
                        ESP_LOGW(TAG, "Detected stale BT connection (connected but audio on I2S) - forcing disconnect/reconnect");
                        bt_manager_disconnect();
                        // Wait a moment for disconnect to complete, then reconnect
                        vTaskDelay(pdMS_TO_TICKS(500));
                        
                        // Initialize BT Classic and manager if not already done
                        esp_err_t bt_init_result = s3_bluetooth_init_bt_classic();
                        if (bt_init_result == ESP_OK) {
                            bt_manager_init(on_bt_status_changed);
                            bt_manager_connect();
                            current_state = BLUETOOTH_SCAN_SCREEN;
                        } else {
                            ESP_LOGE(TAG, "Failed to reinitialize BT Classic: %s", esp_err_to_name(bt_init_result));
                            current_state = BLUETOOTH_SCREEN;
                        }
                    } else {
                        ESP_LOGI(TAG, "Bluetooth is properly connected. Requesting disconnect...");
                        bt_manager_disconnect();
                        current_state = BLUETOOTH_SCREEN;
                    }
                } else {
                    ESP_LOGI(TAG, "BLUETOOTH_SCREEN → Starting connection process...");

                    // Initialize BT Classic and manager if not already done (deferred from boot)
                    esp_err_t bt_init_result = s3_bluetooth_init_bt_classic();
                    if (bt_init_result != ESP_OK) {
                        ESP_LOGE(TAG, "Failed to initialize BT Classic: %s", esp_err_to_name(bt_init_result));
                        current_state = BLUETOOTH_SCREEN;  // Stay on menu to show error
                        next_state = NULL_SCREEN;
                        break;
                    }

                    // Initialize BT manager (this sets up the status callback)
                    bt_manager_init(on_bt_status_changed);

                    bt_manager_connect();
                    current_state = BLUETOOTH_SCAN_SCREEN;
                }
                next_state = NULL_SCREEN;
            }
            else if (event == EVENT_BTN_D_SHORT)
            {
                ESP_LOGI(TAG, "BLUETOOTH_SCREEN → HOME_SCREEN");
                current_state = HOME_SCREEN;
                next_state    = NULL_SCREEN;
            }
            else if (event == EVENT_BTN_D_LONG)
            {
                ESP_LOGI(TAG, "BLUETOOTH_SCREEN → SHUTDOWN");
                global_poweroff = POWER_MODE_SHUTDOWN;
                current_state = SHUTDOWN_SCREEN;
                next_state = POWER_OFF_SCREEN;
            }
            else if (event == EVENT_BTN_MACRO_A_N_D_LONG)
            {
                ESP_LOGI(TAG, "BLUETOOTH_SCREEN → FAC_RESET_SCREEN");
                fac_reset_screen_entry_source = BLUETOOTH_SCREEN;
                current_state = FAC_RESET_SCREEN;
                next_state = NULL_SCREEN;
            }
            else
            {
                ESP_LOGI(TAG, "BLUETOOTH_SCREEN → [NO_ACTION_DEFINED]");
                current_state = NULL_SCREEN;
                next_state    = NULL_SCREEN;
            }
            break;

        case BLUETOOTH_SCAN_SCREEN:
            if (event == EVENT_BTN_C_SHORT || event == EVENT_BTN_D_SHORT) {
                ESP_LOGI(TAG, "BLUETOOTH_SCAN_SCREEN → [User canceled]");
                bt_manager_disconnect();
                if (event == EVENT_BTN_D_SHORT) {
                    current_state = HOME_SCREEN;
                } else {
                    current_state = BLUETOOTH_SCREEN;
                }
                next_state = NULL_SCREEN;
            } else if (event == EVENT_BTN_D_LONG) {
                ESP_LOGI(TAG, "BLUETOOTH_SCAN_SCREEN → SHUTDOWN");
                bt_manager_disconnect();
                global_poweroff = POWER_MODE_SHUTDOWN;
                current_state = SHUTDOWN_SCREEN;
                next_state = POWER_OFF_SCREEN;
            }
            else if (event == EVENT_BTN_MACRO_A_N_D_LONG)
            {
                ESP_LOGI(TAG, "BLUETOOTH_SCAN_SCREEN → FAC_RESET_SCREEN");
                bt_manager_disconnect();
                fac_reset_screen_entry_source = BLUETOOTH_SCAN_SCREEN;
                current_state = FAC_RESET_SCREEN;
                next_state = NULL_SCREEN;
            } else {
                ESP_LOGI(TAG, "BLUETOOTH_SCAN_SCREEN → [Ignoring event %d during scan]", event);
            }
            break;

        case WIFI_SEARCH_SCREEN:
            ESP_LOGI(TAG, "WIFI_SEARCH_SCREEN → [NO_ACTION_DEFINED] -> syncUp or disconnect or unknown ");
            app_timeout_stop(); // Stop timers during WiFi search
            app_timeout_deepsleep_stop();
            current_state = NULL_SCREEN;
            next_state = NULL_SCREEN;
            break;

		case BLE_PAIRING_SCREEN:
			if (event == EVENT_BTN_C_SHORT)
			{
				int oob_status = 0;
				read_oob_status(&oob_status);
				if (oob_status == 0)
				{
					ESP_LOGI(TAG, "BLE_PAIRING_SCREEN -> oob==0 → WIFI_UNKNOWN_SCREEN");
					current_state  = WIFI_UNKNOWN_SCREEN;
				}
				else
				{
					ESP_LOGI(TAG, "BLE_PAIRING_SCREEN -> oob==1 → WIFI_DISCONNECT_SCREEN");
					current_state  = WIFI_DISCONNECT_SCREEN;
				}
				next_state = NULL_SCREEN;
			}
			else if (event == EVENT_BTN_D_SHORT)
			{
				ESP_LOGI(TAG, "BLE_PAIRING_SCREEN → HOME_SCREEN");
				current_state = HOME_SCREEN;
				next_state	  = NULL_SCREEN;
                
			}
			else
			{
				ESP_LOGI(TAG, "BLE_PAIRING_SCREEN → [NO_ACTION_DEFINED]");
				current_state = NULL_SCREEN;
				next_state	  = NULL_SCREEN;
			    app_timeout_stop();
			}
			break;

        case WIFI_UNKNOWN_SCREEN:
            if (event == EVENT_BTN_A_SHORT)
            {
                ESP_LOGI(TAG, "WIFI_UNKNOWN_SCREEN → VOLUME_SCREEN");
                current_state = VOLUME_SCREEN;
                next_state    = NULL_SCREEN;
            }
            else if (event == EVENT_BTN_B_SHORT)
            {
                ESP_LOGI(TAG, "WIFI_UNKNOWN_SCREEN → BLUETOOTH_SCREEN");
                current_state = BLUETOOTH_SCREEN;
                next_state    = NULL_SCREEN;
            }
            else if (event == EVENT_BTN_C_LONG)
            {
                ESP_LOGI(TAG, "WIFI_UNKNOWN_SCREEN → BLE_PAIRING_SCREEN");
                current_state = BLE_PAIRING_SCREEN;
                next_state    = NULL_SCREEN;
            }
            else if (event == EVENT_BTN_D_SHORT)
            {
                ESP_LOGI(TAG, "BLE_PAIRING_SCREEN → HOME_SCREEN");
                current_state = HOME_SCREEN;
                next_state    = NULL_SCREEN;
            }
            else
            {
                ESP_LOGI(TAG, "WIFI_UNKNOWN_SCREEN → [NO_ACTION_DEFINED]");
                current_state = NULL_SCREEN;
                next_state    = NULL_SCREEN;
            }
            break;

		case WIFI_DISCONNECT_SCREEN:
			if (event == EVENT_BTN_A_SHORT)
			{
				ESP_LOGI(TAG, "WIFI_DISCONNECT_SCREEN → VOLUME_SCREEN");
				current_state = VOLUME_SCREEN;
				next_state	  = NULL_SCREEN;
			}
			else if (event == EVENT_BTN_B_SHORT)
			{
				ESP_LOGI(TAG, "WIFI_DISCONNECT_SCREEN → BLUETOOTH_SCREEN");
				current_state = BLUETOOTH_SCREEN;
				next_state	  = NULL_SCREEN;
			}
			else if (event == EVENT_BTN_C_SHORT)
			{
				ESP_LOGI(TAG, "WIFI_DISCONNECT_SCREEN → WIFI_SEARCH_SCREEN");
				current_state  = WIFI_SEARCH_SCREEN;
				next_state	   = NULL_SCREEN;
			}
			else if (event == EVENT_BTN_C_LONG)
			{
				ESP_LOGI(TAG, "WIFI_DISCONNECT_SCREEN → BLE_PAIRING_SCREEN");
				current_state = BLE_PAIRING_SCREEN;
				next_state	  = NULL_SCREEN;
			}
			else if (event == EVENT_BTN_D_SHORT)
			{
				ESP_LOGI(TAG, "WIFI_DISCONNECT_SCREEN → HOME_SCREEN");
				current_state = HOME_SCREEN;
				next_state	  = NULL_SCREEN;
			}
			else
			{
			    app_timeout_stop();
				ESP_LOGI(TAG, "WIFI_DISCONNECT_SCREEN → [NO_ACTION_DEFINED]");
				current_state = NULL_SCREEN;
				next_state	  = NULL_SCREEN;
			}
			break;

	case DATA_SYNC_SCREEN:
		{
		    app_timeout_stop();
		    app_timeout_deepsleep_stop(); // Also stop deep sleep timer during sync

		    // Handle button press: show wait screen for 1 second
		    if (event == EVENT_BTN_A_SHORT || event == EVENT_BTN_B_SHORT ||
		        event == EVENT_BTN_C_SHORT || event == EVENT_BTN_D_SHORT) {

		        // If already showing wait screen, ignore additional button presses
		        if (s3_data_sync_show_wait) {
		            ESP_LOGI(TAG, "DATA_SYNC_SCREEN → Button pressed (wait screen already showing)");
		            current_state = NULL_SCREEN;
		            next_state = NULL_SCREEN;
		            break;
		        }

		        ESP_LOGI(TAG, "DATA_SYNC_SCREEN → Button pressed, showing wait screen");

		        // Set flag to show wait screen
		        s3_data_sync_show_wait = true;

		        // Stop any existing timer
		        if (data_sync_wait_timer) {
		            esp_timer_stop(data_sync_wait_timer);
		            esp_timer_delete(data_sync_wait_timer);
		            data_sync_wait_timer = NULL;
		        }

		        // Create and start timer for 1 second
		        esp_timer_create_args_t timer_args = {
		            .callback = data_sync_wait_timer_callback,
		            .name = "data_sync_wait"
		        };

		        esp_err_t ret = esp_timer_create(&timer_args, &data_sync_wait_timer);
		        if (ret != ESP_OK) {
		            ESP_LOGE(TAG, "Failed to create data sync wait timer: %s", esp_err_to_name(ret));
		            s3_data_sync_show_wait = false;
		        } else {
		            ret = esp_timer_start_once(data_sync_wait_timer, 1000000); // 1 second in microseconds
		            if (ret != ESP_OK) {
		                ESP_LOGE(TAG, "Failed to start data sync wait timer: %s", esp_err_to_name(ret));
		                esp_timer_delete(data_sync_wait_timer);
		                data_sync_wait_timer = NULL;
		                s3_data_sync_show_wait = false;
		            } else {
		                // Refresh screen to show wait image
		                set_current_screen(DATA_SYNC_SCREEN, NULL_SCREEN);
		            }
		        }

		        current_state = NULL_SCREEN;
		        next_state = NULL_SCREEN;
		    } else {
		        ESP_LOGI(TAG, "DATA_SYNC_SCREEN → [NO_ACTION_DEFINED]");
		        current_state = NULL_SCREEN;
		        next_state = NULL_SCREEN;
		    }
		}
        // Album manager update moved to sync functions themselves (WiFi.c)
        // This ensures sync operations handle album building properly without redundant calls
	break;

		case NFC_SCREEN:
			if (event == EVENT_BTN_A_SHORT)
			{
                ESP_LOGI(TAG, "NFC_SCREEN → DISPLAY_SCREEN");
                current_state = DISPLAY_SCREEN;
                next_state    = NULL_SCREEN;
			}
			else if (event == EVENT_BTN_B_SHORT)
			{
				ESP_LOGI(TAG, "NFC_SCREEN → VOLUME_SCREEN");
				current_state = VOLUME_SCREEN;
				next_state	  = NULL_SCREEN;
			}
			else if (event == EVENT_BTN_C_SHORT)
			{
				ESP_LOGI(TAG, "NFC_SCREEN → NFC_LANGUAGE_SCREEN");
				current_state = NFC_LANGUAGE_SCREEN;
				next_state	  = NULL_SCREEN;
			}
			else if (event == EVENT_BTN_D_SHORT)
			{
                current_state = (volume_screen_entry_source != NULL_SCREEN) ? volume_screen_entry_source : HOME_SCREEN;
                next_state = NULL_SCREEN;
			    ESP_LOGI(TAG, "NFC_SCREEN → %d [SAVE, return to source]", current_state);
			}
			else if (event == EVENT_BTN_D_LONG)
			{
				ESP_LOGI(TAG, "NFC_SCREEN → SHUTDOWN");
				global_poweroff = POWER_MODE_SHUTDOWN;
				current_state = SHUTDOWN_SCREEN;
				next_state = POWER_OFF_SCREEN;
			}
			else if (event == EVENT_BTN_MACRO_A_N_D_LONG)
			{
				ESP_LOGI(TAG, "NFC_SCREEN → FAC_RESET_SCREEN");
				fac_reset_screen_entry_source = NFC_SCREEN;
				current_state = FAC_RESET_SCREEN;
				next_state = NULL_SCREEN;
			}
			else
			{
				ESP_LOGI(TAG, "NFC_SCREEN → NULL_SCREEN");
				current_state = NULL_SCREEN;
				next_state	  = NULL_SCREEN;
			}
			break;

        case NFC_LANGUAGE_SCREEN:
            if (event == EVENT_BTN_A_SHORT) {
                ESP_LOGI(TAG, "NFC_LANGUAGE_SCREEN → [LANGUAGE PREVIEW]");
                ui_change_language();       // Preview language change (no save)
                current_state = NFC_LANGUAGE_SCREEN;
                next_state = NULL_SCREEN;
            } else if (event == EVENT_BTN_B_SHORT) {
                ESP_LOGI(TAG, "NFC_LANGUAGE_SCREEN → [LANGUAGE PREVIEW]");
                ui_change_language();       // Preview language change (no save)
                current_state = NFC_LANGUAGE_SCREEN;
                next_state = NULL_SCREEN;
            } else if (event == EVENT_BTN_C_SHORT) {
                ESP_LOGI(TAG, "NFC_LANGUAGE_SCREEN → NFC_SCREEN [CONFIRM]");
                ui_save_language();         // Save the selected language to NVS
                current_state = NFC_SCREEN;
                next_state = NULL_SCREEN;
            } else if (event == EVENT_BTN_D_SHORT) {
                ui_save_language();         // Save the selected language to NVS
                current_state = (volume_screen_entry_source != NULL_SCREEN) ? volume_screen_entry_source : HOME_SCREEN;
                next_state = NULL_SCREEN;
                ESP_LOGI(TAG, "NFC_LANGUAGE_SCREEN → %d [SAVE, return to source]", current_state);
            } else if (event == EVENT_BTN_D_LONG) {
                ESP_LOGI(TAG, "NFC_LANGUAGE_SCREEN → SHUTDOWN");
                global_poweroff = POWER_MODE_SHUTDOWN;
                current_state = SHUTDOWN_SCREEN;
                next_state = POWER_OFF_SCREEN;
            }
            else if (event == EVENT_BTN_MACRO_A_N_D_LONG)
            {
                ESP_LOGI(TAG, "NFC_LANGUAGE_SCREEN → FAC_RESET_SCREEN");
                fac_reset_screen_entry_source = NFC_LANGUAGE_SCREEN;
                current_state = FAC_RESET_SCREEN;
                next_state = NULL_SCREEN;
            } else {
                ESP_LOGI(TAG, "NFC_LANGUAGE_SCREEN → [NO_ACTION_DEFINED]");
                current_state = NULL_SCREEN;
                next_state = NULL_SCREEN;
            }
            break;

        case NFC_ACTIVATION_SCREEN:
            if (event == EVENT_BTN_D_SHORT) {
                ESP_LOGI(TAG, "NFC_ACTIVATION_SCREEN → HOME_SCREEN [HOME_KEY]");
                current_state = HOME_SCREEN;
                next_state = NULL_SCREEN;
            } else {
                ESP_LOGI(TAG, "NFC_ACTIVATION_SCREEN → [NO_ACTION_DEFINED]");
                current_state = NULL_SCREEN;
                next_state = NULL_SCREEN;
            }
            break;

        case NFC_CONTENT_SCREEN:
            if (event == EVENT_BTN_D_SHORT) {
                ESP_LOGI(TAG, "NFC_CONTENT_SCREEN → HOME_SCREEN [HOME_KEY]");
                current_state = HOME_SCREEN;
                next_state = NULL_SCREEN;
            } else {
                // Handle timeout: return to previous screen
                s3_screens_t previous = get_previous_screen();
                if (previous != NULL_SCREEN) {
                    ESP_LOGI(TAG, "NFC_CONTENT_SCREEN → [TIMEOUT_AUTO_RETURN] to screen %d", previous);
                    current_state = previous;
                    next_state = NULL_SCREEN;
                } else {
                    ESP_LOGI(TAG, "NFC_CONTENT_SCREEN → [TIMEOUT_AUTO_RETURN] to HOME_SCREEN (no previous)");
                    current_state = HOME_SCREEN;
                    next_state = NULL_SCREEN;
                }
            }
            break;

        case NFC_NO_CONTENT_SCREEN:
            if (event == EVENT_BTN_D_SHORT) {
                ESP_LOGI(TAG, "NFC_NO_CONTENT_SCREEN → HOME_SCREEN [HOME_KEY]");
                current_state = HOME_SCREEN;
                next_state = NULL_SCREEN;
            } else {
                ESP_LOGI(TAG, "NFC_NO_CONTENT_SCREEN → [TIMEOUT_AUTO_RETURN]");
                current_state = NULL_SCREEN;
                next_state = NULL_SCREEN;
            }
            break;

        case DISPLAY_SCREEN:
            if (event == EVENT_BTN_A_SHORT) {
                // Check OOB status: if OOB = 0 (factory reset), skip WIFI and go to VOLUME
                if (s3_album_mgr_factory_reset_status()) {
                    ESP_LOGI(TAG, "DISPLAY_SCREEN → VOLUME_SCREEN (OOB=0, WIFI hidden)");
                    current_state = VOLUME_SCREEN;
                } else {
                    ESP_LOGI(TAG, "DISPLAY_SCREEN → WIFI_SYNC_MAI_SCREEN (Bluetooth hidden)");
                    current_state = WIFI_SYNC_MAI_SCREEN;
                }
                next_state = NULL_SCREEN;
            } else if (event == EVENT_BTN_B_SHORT) {
                if (haveNFC()) {
                    ESP_LOGI(TAG, "DISPLAY_SCREEN → NFC_SCREEN");
                    current_state = NFC_SCREEN;
                } else {
                    ESP_LOGI(TAG, "DISPLAY_SCREEN → VOLUME_SCREEN");
                    current_state = VOLUME_SCREEN;
                }
                next_state = NULL_SCREEN;
            } else if (event == EVENT_BTN_C_SHORT) {
                ESP_LOGI(TAG, "DISPLAY_SCREEN → DISPLAY_SETTINGS_SCREEN");
                current_state = DISPLAY_SETTINGS_SCREEN;
                next_state = NULL_SCREEN;

                s3_brightness_level = get_backlight();
            } else if (event == EVENT_BTN_D_SHORT) {
                current_state = (volume_screen_entry_source != NULL_SCREEN) ? volume_screen_entry_source : HOME_SCREEN;
                ESP_LOGI(TAG, "DISPLAY_SCREEN → %d [SAVE, return to source]", current_state);
                next_state = NULL_SCREEN;
            } else if (event == EVENT_BTN_D_LONG) {
                ESP_LOGI(TAG, "DISPLAY_SCREEN → SHUTDOWN");
                global_poweroff = POWER_MODE_SHUTDOWN;
                current_state = SHUTDOWN_SCREEN;
                next_state = POWER_OFF_SCREEN;
            } else if (event == EVENT_BTN_MACRO_A_N_D_LONG) {
                ESP_LOGI(TAG, "DISPLAY_SCREEN → FAC_RESET_SCREEN");
                fac_reset_screen_entry_source = DISPLAY_SCREEN;
                current_state = FAC_RESET_SCREEN;
                next_state = NULL_SCREEN;
            } else {
                ESP_LOGI(TAG, "DISPLAY_SCREEN → [NO_ACTION_DEFINED]");
                current_state = NULL_SCREEN;
                next_state = NULL_SCREEN;
            }
            break;

        case DISPLAY_SETTINGS_SCREEN:
            if (event == EVENT_BTN_C_SHORT) {
                ESP_LOGI(TAG, "DISPLAY_SETTINGS_SCREEN → DISPLAY_SCREEN [CONFIRM]");
                current_state = DISPLAY_SCREEN;
                next_state = NULL_SCREEN;

                brightness_confirm_and_save(); // Save preview to NVS and set hardware
            } else if (event == EVENT_BTN_D_SHORT) {
                current_state = (volume_screen_entry_source != NULL_SCREEN) ? volume_screen_entry_source : HOME_SCREEN;
                next_state = NULL_SCREEN;
                ESP_LOGI(TAG, "DISPLAY_SETTINGS_SCREEN → %d [SAVE, return to source]", current_state);
                brightness_confirm_and_save(); // Save preview to NVS and set hardware
            } else if (event == EVENT_BTN_A_SHORT) {
                ESP_LOGI(TAG, "DISPLAY_SETTINGS_SCREEN → [BRIGHTNESS PREVIEW DOWN]");
                current_state = BRIGHTNESS_DOWN_SCREEN;
                next_state = DISPLAY_SETTINGS_SCREEN;

                brightness_preview_down(); // Preview change (variable + hardware, no NVS save)
            } else if (event == EVENT_BTN_B_SHORT) {
                ESP_LOGI(TAG, "DISPLAY_SETTINGS_SCREEN → [BRIGHTNESS PREVIEW UP]");
                current_state = BRIGHTNESS_UP_SCREEN;
                next_state = DISPLAY_SETTINGS_SCREEN;

                brightness_preview_up(); // Preview change (variable + hardware, no NVS save)
            } else if (event == EVENT_BTN_D_LONG) {
                ESP_LOGI(TAG, "DISPLAY_SETTINGS_SCREEN → SHUTDOWN");
                global_poweroff = POWER_MODE_SHUTDOWN;
                current_state = SHUTDOWN_SCREEN;
                next_state = POWER_OFF_SCREEN;
            }
            else if (event == EVENT_BTN_MACRO_A_N_D_LONG)
            {
                ESP_LOGI(TAG, "DISPLAY_SETTINGS_SCREEN → FAC_RESET_SCREEN");
                fac_reset_screen_entry_source = DISPLAY_SETTINGS_SCREEN;
                current_state = FAC_RESET_SCREEN;
                next_state = NULL_SCREEN;
            } else {
                ESP_LOGI(TAG, "DISPLAY_SETTINGS_SCREEN → [NO_ACTION_DEFINED]");
                current_state = NULL_SCREEN;
                next_state = NULL_SCREEN;
                // Note: No save_backlight() here - changes are lost
            }
            break;

        case BRIGHTNESS_UP_SCREEN:
            if (event == EVENT_BTN_D_LONG) {
                ESP_LOGI(TAG, "BRIGHTNESS_UP_SCREEN → SHUTDOWN");
                global_poweroff = POWER_MODE_SHUTDOWN;
                current_state = SHUTDOWN_SCREEN;
                next_state = POWER_OFF_SCREEN;
            }
            else if (event == EVENT_BTN_MACRO_A_N_D_LONG)
            {
                ESP_LOGI(TAG, "BRIGHTNESS_UP_SCREEN → FAC_RESET_SCREEN");
                fac_reset_screen_entry_source = BRIGHTNESS_UP_SCREEN;
                current_state = FAC_RESET_SCREEN;
                next_state = NULL_SCREEN;
            } else {
                ESP_LOGI(TAG, "BRIGHTNESS_UP_SCREEN → [NO_ACTION_DEFINED]");
                current_state = NULL_SCREEN;
                next_state = NULL_SCREEN;
            }
            break;

        case BRIGHTNESS_DOWN_SCREEN:
            if (event == EVENT_BTN_D_LONG) {
                ESP_LOGI(TAG, "BRIGHTNESS_DOWN_SCREEN → SHUTDOWN");
                global_poweroff = POWER_MODE_SHUTDOWN;
                current_state = SHUTDOWN_SCREEN;
                next_state = POWER_OFF_SCREEN;
            }
            else if (event == EVENT_BTN_MACRO_A_N_D_LONG)
            {
                ESP_LOGI(TAG, "BRIGHTNESS_DOWN_SCREEN → FAC_RESET_SCREEN");
                fac_reset_screen_entry_source = BRIGHTNESS_DOWN_SCREEN;
                current_state = FAC_RESET_SCREEN;
                next_state = NULL_SCREEN;
            } else {
                ESP_LOGI(TAG, "BRIGHTNESS_DOWN_SCREEN → [NO_ACTION_DEFINED]");
                current_state = NULL_SCREEN;
                next_state = NULL_SCREEN;
            }
            break;

        case CLOCK_SCREEN:
            if (event == EVENT_BTN_D_SHORT) {
                ESP_LOGI(TAG, "CLOCK_SCREEN → HOME_SCREEN");
                current_state = HOME_SCREEN;
                next_state = NULL_SCREEN;
                if(is_clock_initialized() == true)
                {
                    deinit_clock();
                }
            } else if (event == EVENT_NFC_DETECTED) {
                ESP_LOGI(TAG, "CLOCK_SCREEN → PLAY_SCREEN [NFC_DETECTED]");
                current_state = PLAY_SCREEN;
                next_state = NULL_SCREEN;

                if (is_clock_initialized() == true) {
                    deinit_clock();
                }

                set_last_transition_callback((post_transition_cb_t) play_album); // Start audio after screen transition
            } else if (event == EVENT_LEAVE_STANDBY) {
                ESP_LOGI(TAG, "CLOCK_SCREEN → [UPDATE_MINI_ICONS]");
                current_state = CLOCK_SCREEN;
                next_state = NULL_SCREEN;
            } else if (event == EVENT_BTN_C_LONG) {
                ESP_LOGI(TAG, "CLOCK_SCREEN → VOLUME_SCREEN");
                volume_screen_entry_source = CLOCK_SCREEN;
                current_state = VOLUME_SCREEN;
                next_state = NULL_SCREEN;
                if (is_clock_initialized() == true) {
                    deinit_clock();
                }
            } else if (event == EVENT_BTN_D_LONG) {
                ESP_LOGI(TAG, "CLOCK_SCREEN → SHUTDOWN");
                global_poweroff = POWER_MODE_SHUTDOWN;
                current_state = SHUTDOWN_SCREEN;
                next_state = POWER_OFF_SCREEN;
                if (is_clock_initialized() == true) {
                    deinit_clock();
                }
            }
            else if (event == EVENT_BTN_MACRO_A_N_D_LONG)
            {
                ESP_LOGI(TAG, "CLOCK_SCREEN → FAC_RESET_SCREEN");
                fac_reset_screen_entry_source = CLOCK_SCREEN;
                current_state = FAC_RESET_SCREEN;
                next_state = NULL_SCREEN;
                if (is_clock_initialized() == true) {
                    deinit_clock();
                }
            } else {
                ESP_LOGI(TAG, "CLOCK_SCREEN → [NO_ACTION_DEFINED]");
                current_state = NULL_SCREEN;
                next_state = NULL_SCREEN;
            }
            break;

        case ALARM_SCREEN:
            if (event == EVENT_BTN_C_SHORT || event == EVENT_ALARM_AUTO_DISMISS) {
                ESP_LOGI(TAG, "ALARM_SCREEN → PREVIOUS_SCREEN [%s]",
                        (event == EVENT_BTN_C_SHORT) ? "MANUAL DISMISS" : "AUTO DISMISS");
                stop_alarm_timeout_timer();
                stop_alarm_repeat();  // Stop alarm from repeating
                play_stop();

                // Restart standby and deep sleep timers after alarm dismissal
                app_timeout_restart();
                app_timeout_deepsleep_start();

                if(is_wakeup_from_alarm() == true)
                {
                    current_state = HOME_SCREEN;
                    set_wakeup_from_alarm_false();
                }
                else
                {
                    current_state = get_previous_screen();
                }
                next_state = NULL_SCREEN;

                if(current_state == PLAY_SCREEN)
                {
                    set_last_transition_callback(resume_audio_to_now_playing);
                }
                else
                {
                    set_last_transition_callback(resume_audio_after_alarm);
                }
            } else {
                ESP_LOGI(TAG, "ALARM_SCREEN → [NO_ACTION_DEFINED]");
                current_state = NULL_SCREEN;
                next_state = NULL_SCREEN;
            }
            // Note: play_stop() removed - alarm audio already finished and pipeline stopped
            // The resume callback will handle restarting audio if it was playing before
            stop_dimmer();
            break;

        case POWER_LOW_SCREEN:
            if (event == EVENT_BTN_A_SHORT || event == EVENT_BTN_B_SHORT || event == EVENT_BTN_C_SHORT || event == EVENT_BTN_D_SHORT) {
                ESP_LOGI(TAG, "POWER_LOW_SCREEN → [previous status %d]",s3_preLowBattery_screen);
            	current_state = s3_preLowBattery_screen;
            	next_state = NULL_SCREEN;
            }else{
                ESP_LOGI(TAG, "POWER_LOW_SCREEN → [NO_ACTION_DEFINED]");
                current_state = NULL_SCREEN;
            	next_state = NULL_SCREEN;
            }
            break;

        case POWER_FULL_SCREEN:
            if (event == EVENT_BTN_A_SHORT || event == EVENT_BTN_B_SHORT || event == EVENT_BTN_C_SHORT || event == EVENT_BTN_D_SHORT) {
                ESP_LOGI(TAG, "POWER_FULL_SCREEN → PREVIOUS_SCREEN");
            	current_state = current_state = get_previous_screen();
            	next_state = NULL_SCREEN;
            }else{
                ESP_LOGI(TAG, "POWER_FULL_SCREEN → [NO_ACTION_DEFINED]");
                current_state = NULL_SCREEN;
            	next_state = NULL_SCREEN;
            }
            break;

        case POWER_CHARGE_SCREEN:
            if (event == EVENT_BTN_A_SHORT || event == EVENT_BTN_B_SHORT || event == EVENT_BTN_C_SHORT || event == EVENT_BTN_D_SHORT) {
                ESP_LOGI(TAG, "POWER_CHARGE_SCREEN → PREVIOUS_SCREEN");
            	current_state = current_state = get_previous_screen();
            	next_state = NULL_SCREEN;
            }else{
                ESP_LOGI(TAG, "POWER_CHARGE_SCREEN → [NO_ACTION_DEFINED]");
                current_state = NULL_SCREEN;
            	next_state = NULL_SCREEN;
            }
            break;

        case POWER_LOW_PLUG_IN_SCREEN:
            if (event == EVENT_BTN_A_SHORT || event == EVENT_BTN_B_SHORT || event == EVENT_BTN_C_SHORT || event == EVENT_BTN_D_SHORT) {
                ESP_LOGI(TAG, "POWER_LOW_PLUG_IN_SCREEN → [previous status %d]",s3_preLowBattery_screen);
            	current_state = s3_preLowBattery_screen;
            	next_state = NULL_SCREEN;
            }else{
                ESP_LOGI(TAG, "POWER_LOW_PLUG_IN_SCREEN → [NO_ACTION_DEFINED]");
                current_state = NULL_SCREEN;
            	next_state = NULL_SCREEN;
            }
            break;

        case OTA_SCREEN:
            ESP_LOGI(TAG, "OTA_SCREEN → [NO_ACTION_DEFINED]");
            current_state = NULL_SCREEN;
            next_state = NULL_SCREEN;
            break;

        case WIFI_PLUG_IN_SCREEN:
            if (event == EVENT_BTN_C_SHORT) {
                ESP_LOGI(TAG, "WIFI_PLUG_IN_SCREEN → WIFI_DISCONNECT_SCREEN");
                gWiFi_SYNC_USER_INTERRUPT = true;
                for (int i = 0;i < 10;i++){
                    vTaskDelay(500 / portTICK_PERIOD_MS);
                    if ( conn_task_running() == false )
                        break;
                }
                current_state = WIFI_DISCONNECT_SCREEN;
                next_state = NULL_SCREEN;
            } else if (event == EVENT_BTN_D_SHORT) {
                ESP_LOGI(TAG, "WIFI_PLUG_IN_SCREEN → HOME_SCREEN");
                gWiFi_SYNC_USER_INTERRUPT = true;
                for (int i = 0;i < 10;i++){
                    vTaskDelay(500 / portTICK_PERIOD_MS);
                    if ( conn_task_running() == false )
                        break;
                }
                current_state = HOME_SCREEN;
                next_state = NULL_SCREEN;
            } else {
                ESP_LOGI(TAG, "WIFI_PLUG_IN_SCREEN → [NO_ACTION_DEFINED]");
                app_timeout_stop(); // Stop timers during WiFi plug-in screen
                app_timeout_deepsleep_stop();
                current_state = NULL_SCREEN;
                next_state = NULL_SCREEN;
            }
            break;

        case SHUTDOWN_SCREEN:
            if (event == EVENT_BTN_D_LONG) {
                ESP_LOGI(TAG, "SHUTDOWN_SCREEN → HOME_SCREEN");
                current_state = HOME_SCREEN;
                next_state = NULL_SCREEN;
            } else {
                ESP_LOGI(TAG, "SHUTDOWN_SCREEN → [NO_ACTION_DEFINED]");
                current_state = NULL_SCREEN;
                next_state = NULL_SCREEN;
            }
            break;

        case STANDBY_SCREEN:
            ESP_LOGI(TAG, "Waking up from Standby...");
            backlight_on();
            // nfc_enable();
            nfc_resume();
            app_timeout_restart();
            app_timeout_deepsleep_stop();

            if (event == EVENT_BTN_D_SHORT) {
                ESP_LOGI(TAG, "STANDBY_SCREEN → HOME_SCREEN");
                current_state = HOME_SCREEN;
                next_state = NULL_SCREEN;
            } else if (event == EVENT_NFC_DETECTED) {
                ESP_LOGI(TAG, "STANDBY_SCREEN → PLAY_SCREEN [NFC_DETECTED]");
                current_state = PLAY_SCREEN;
                next_state = NULL_SCREEN;
                
                set_last_transition_callback((post_transition_cb_t) play_album); // Start audio after screen transition
            } else { // ALL_KEYS and EVENT_LEAVE_STANDBY WAKE UP
                ESP_LOGI(TAG, "STANDBY_SCREEN → PREVIOUS_SCREEN");
                current_state = get_previous_screen();
                next_state = NULL_SCREEN;
                if(current_state == CLOCK_SCREEN)
                {
                    setup_clock_update_screen_cb(refresh_screen_display);
                    init_clock();
                }
                if(current_state == PLAY_SCREEN)
                {
                }
            }
            break;

        case POWER_OFF_SCREEN:
            if (event == EVENT_BTN_D_LONG) {
                ESP_LOGI(TAG, "BOOT_SCREEN → HOME_SCREEN");
                current_state = BOOT_SCREEN;
                next_state = HOME_SCREEN;
                play_audio_boot();
            } else {
                ESP_LOGI(TAG, "STANDBY_SCREEN → NULL_SCREEN");
                current_state = NULL_SCREEN;
                next_state = NULL_SCREEN;
            }
            break;
        case FAC_RESET_SCREEN:
            if (event == EVENT_BTN_D_SHORT) { // home
                ESP_LOGI(TAG, "FAC_RESET_SCREEN → HOME_SCREEN");
                current_state = HOME_SCREEN;
                next_state = NULL_SCREEN;
            } else if (event == EVENT_BTN_C_SHORT) // play
            {
                ESP_LOGI(TAG, "FAC_RESET_SCREEN -> play → [reboot]");
            //    write_resource_version_to_file("1.0.0");
                delete_sdcard_file_if_exists("/sdcard/tmp/account_file.json");
                delete_sdcard_file_if_exists("/sdcard/tmp/account_file.json.bak");
                //delete_sdcard_file_if_exists("/sdcard/tmp/fw-contents.json");
                clear_alarm_file_content();
                s3_nvs_factoryReset();
                vTaskDelay(pdMS_TO_TICKS(200));
                esp_restart();
                current_state = NULL_SCREEN;
                next_state = NULL_SCREEN;
            }else
            {
                current_state = NULL_SCREEN;
                next_state = NULL_SCREEN;
            }
            break;
        case ACC_INV_FAC_RESET_SCREEN:
            if (event == EVENT_BTN_C_SHORT) // play
            {
                ESP_LOGI(TAG, "ACC_INV_FAC_RESET_SCREEN -> play → [fac reboot]");
                delete_sdcard_file_if_exists("/sdcard/tmp/account_file.json");
                delete_sdcard_file_if_exists("/sdcard/tmp/fw-contents.json");
                clear_alarm_file_content();
                s3_nvs_factoryReset();
                vTaskDelay(pdMS_TO_TICKS(200));
                esp_restart();
                current_state = NULL_SCREEN;
                next_state = NULL_SCREEN;
            } else
            {
                current_state = NULL_SCREEN;
                next_state = NULL_SCREEN;
            }
            break;
        case BOOT_SCREEN:
            ESP_LOGI(TAG, "BOOT_SCREEN → ANIMATION");
            current_state = NULL_SCREEN;
            next_state = NULL_SCREEN;
            break;

        case WIFI_SYNC_MAI_SCREEN:
            if (event == EVENT_BTN_A_SHORT)
            {
                ESP_LOGI(TAG, "WIFI_SYNC_MAI_SCREEN → VOLUME_SCREEN");
                current_state = VOLUME_SCREEN;
                next_state    = NULL_SCREEN;
            }
            else if (event == EVENT_BTN_B_SHORT)
            {
                ESP_LOGI(TAG, "WIFI_SYNC_MAI_SCREEN → DISPLAY_SCREEN (Bluetooth hidden)");
                current_state = DISPLAY_SCREEN;
                next_state    = NULL_SCREEN;
            }
            else if (event == EVENT_BTN_C_SHORT)
            {
                // Start data sync directly without showing wifi_search.jpg
                ESP_LOGI(TAG, "WIFI_SYNC_MAI_SCREEN → Starting data sync directly");
                extern void start_wifi_connecting(void);
                start_wifi_connecting();  // This will set stage 0 and show DATA_SYNC_SCREEN (data_sync0.jpg)
                current_state = NULL_SCREEN;  // Don't change screen here, let start_wifi_connecting handle it
                next_state    = NULL_SCREEN;
            }
            else if (event == EVENT_BTN_D_SHORT)
            {
                ESP_LOGI(TAG, "WIFI_SYNC_MAI_SCREEN → HOME_SCREEN");
                current_state = HOME_SCREEN;
                next_state    = NULL_SCREEN;
            }
            else if (event == EVENT_BTN_D_LONG)
            {
                ESP_LOGI(TAG, "WIFI_SYNC_MAI_SCREEN → SHUTDOWN");
                global_poweroff = POWER_MODE_SHUTDOWN;
                current_state = SHUTDOWN_SCREEN;
                next_state = POWER_OFF_SCREEN;
            }
            else if (event == EVENT_BTN_MACRO_A_N_D_LONG)
            {
                ESP_LOGI(TAG, "WIFI_SYNC_MAI_SCREEN → FAC_RESET_SCREEN");
                fac_reset_screen_entry_source = WIFI_SYNC_MAI_SCREEN;
                current_state = FAC_RESET_SCREEN;
                next_state = NULL_SCREEN;
            }
            else
            {
                ESP_LOGI(TAG, "WIFI_SYNC_MAI_SCREEN → [NO_ACTION_DEFINED]");
                current_state = NULL_SCREEN;
                next_state    = NULL_SCREEN;
            }
            break;

        case WIFI_SYNC_ERR_SCREEN:
            ESP_LOGI(TAG, "WIFI_SYNC_ERR_SCREEN");
            app_timeout_stop(); // Stop timers during error screen display
            app_timeout_deepsleep_stop();
            current_state = NULL_SCREEN;
            next_state = NULL_SCREEN;
            break;

        case WIFI_SYNC_SUC_SCREEN:
            ESP_LOGI(TAG, "WIFI_SYNC_SUC_SCREEN");
            app_timeout_stop(); // Stop timers during success screen display
            app_timeout_deepsleep_stop();
            current_state = NULL_SCREEN;
            next_state = NULL_SCREEN;
            break;

        case WIFI_SYNC_N_SCREEN:
            ESP_LOGI(TAG, "WIFI_SYNC_N_SCREEN");
            app_timeout_stop(); // Stop timers during sync N screen display
            app_timeout_deepsleep_stop();
            current_state = NULL_SCREEN;
            next_state = NULL_SCREEN;
            break;

        case WIFI_ERR_SCREEN:
            ESP_LOGI(TAG, "WIFI_ERR_SCREEN");
            app_timeout_stop(); // Stop timers during WiFi error screen display
            app_timeout_deepsleep_stop();
            current_state = NULL_SCREEN;
            next_state = NULL_SCREEN;
            break;

        default:
            ESP_LOGW(TAG, "Event %d ignored in state %d", event, current_state);
            current_state = NULL_SCREEN;
            next_state = NULL_SCREEN;
            break;
    }
    
    if (current_state != NULL_SCREEN) { 
        set_current_screen(current_state, next_state); 
        manage_nfc_state(current_state);
    }
    else
    {
        refresh_screen_display();
    }
}
