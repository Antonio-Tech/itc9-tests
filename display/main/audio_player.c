/**
 * @file audio_player.c
 * @author Igor Oliveira
 * @date 2025-06-04
 * @brief Audio player interface for playing system sound effects
 *
 * This module provides functions to play MP3 audio effects
 * from the SD card filesystem.
 * It implements a thread-safe audio playback system with mutex protection.
 */

#include "audio_player.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "lvgl.h"
#include "audio_pipeline.h"
#include "audio_element.h"
#include "audio_common.h"
#include "ringbuf.h"
#include "i2s_stream.h"
#include "mp3_decoder.h"
#include "board.h"
#include "esp_audio.h"
#include <sys/stat.h>
#include "fatfs_stream.h"
#include <dirent.h>
#include <sys/types.h>
#include <string.h>
#include "xor_decrypt_filter.h"

#include "s3_nvs_item.h"

#include "a2dp_stream.h"  // ESP-ADF A2DP stream for audio pipeline
#include "s3_bluetooth.h"
#include "s3_album_mgr.h"
#include "s3_definitions.h"
#include "s3_sync_account_contents.h"
#include "s3_tracking.h"
#include "cJSON.h"
#include <unistd.h>
#include "audio_event_iface.h"
#include "lv_screen_mgr.h"
#include "s3_nfc_handler.h"
#include "esp_timer.h"
#include "alc5616.h"
#include "backlight.h"
#include "app_state_machine.h"
#include "WiFi.h"  // For WiFi auto-disconnect when audio starts
#include "app_timeout.h"  // For standby timer control

// enumeration to decide the audio route
// audio_sink_t changed and moved to s3_definitions.h

static const char *TAG = "AUDIO_PLAYER";
SemaphoreHandle_t audio_mutex = NULL;
static SemaphoreHandle_t track_mutex = NULL;  // Protects track list and shuffle state

// Audio state machine to prevent pause/resume race conditions
typedef enum {
    AUDIO_STATE_STOPPED,    // No audio playing
    AUDIO_STATE_PLAYING,    // Audio actively playing
    AUDIO_STATE_PAUSING,    // Transition: pause command sent, waiting for pipeline to drain
    AUDIO_STATE_PAUSED,     // Audio paused (stable state)
    AUDIO_STATE_RESUMING    // Transition: resume command sent, waiting for pipeline to start
} audio_state_t;

// Forward declarations
static audio_board_handle_t board_handle = NULL;
static audio_state_t audio_state = AUDIO_STATE_STOPPED;
static int current_volume           = 75;
static bool is_alarm_on_blankee     = false;
static bool alarm_should_repeat     = false;  // Track if alarm should auto-repeat until dismissed or 10min timeout
bool is_powered_on           = false;
char** s3_current_track_list = NULL;
static int volume_backup_on_entry = -1;  // Backup volume when entering volume screen
// Global variables s3_current_idx_track, s3_current_size_track, s3_active_sink,
// s3_volume_level, s3_playback_mode, and s3_auto_play_mode are defined in
// s3_definitions.c and declared in s3_definitions.h

// Fast pause optimization - position is preserved by fatfs_stream pause behavior

// Simple shuffle system - just shuffle the current album's track indices
static size_t *current_shuffle_order = NULL;     // Current album's shuffled track order
static size_t shuffle_position = 0;              // Current position in shuffle
static size_t shuffle_count = 0;                 // Number of tracks in current shuffle

// Audio pipeline handles
static audio_pipeline_handle_t active_pipeline = NULL;
static audio_element_handle_t fatfs_reader = NULL;
static audio_element_handle_t xor_filter = NULL;
static audio_element_handle_t mp3_decoder = NULL;
// Keep track of the current pipeline
static audio_element_handle_t current_sink_element = NULL;

// Sound effect quick playback state
static bool sound_effect_playing = false;
static char *saved_track_uri = NULL;  // Save current track for restoration
static bool was_playing_before_effect = false;  // Save previous playback state
static bool suppress_auto_play_once = false;    // Skip one auto-advance after manual stop/effect

// Mute timer for ALC5616 codec (I2S sink only)
static esp_timer_handle_t codec_mute_timer = NULL;
static bool codec_is_muted = true;  // Start muted

// Persistent I2S element (created once, reused across playbacks)
static audio_element_handle_t persistent_i2s_writer = NULL;
static bool i2s_element_initialized = false;

// Track which type of audio was last played to distinguish tracks from system sounds
static audio_type_t current_audio_type = AUDIO_TYPE_NONE;

// Playback tracking for #15141
typedef struct {
    char* contentId;           // From GetContentId(filename)
    time_t start_time;         // When playback started
    time_t total_pause_time;   // Accumulated pause duration
    time_t pause_start_time;   // When current pause started (0 if not paused)
    bool is_full_play;         // true if completed naturally, false if stopped manually
    bool is_tracking;          // Active tracking flag
} playback_tracking_t;

static playback_tracking_t current_tracking = {0};

// Audio event system disabled: Events are disabled at element level to prevent queue overflow.
// We use periodic check (audio_pipeline_periodic_check) instead of ESP-ADF event callbacks.
// All audio elements have their event callbacks set to NULL to prevent "no space in external queue" warnings.
// static audio_event_iface_handle_t evt = NULL;

// Helper functions for audio state machine
static inline bool is_state_playing(void) {
    return (audio_state == AUDIO_STATE_PLAYING || audio_state == AUDIO_STATE_PAUSING || audio_state == AUDIO_STATE_RESUMING);
}

static inline bool is_state_paused(void) {
    return audio_state == AUDIO_STATE_PAUSED;
}

static inline bool is_state_stopped(void) {
    return audio_state == AUDIO_STATE_STOPPED;
}

// Exported for state machine to prevent pause/resume during transitions
bool is_state_stable(void) {
    return (audio_state == AUDIO_STATE_PLAYING || audio_state == AUDIO_STATE_PAUSED || audio_state == AUDIO_STATE_STOPPED);
}

// Forward declarations
static void stop_active_pipeline_internal(void);
static void cleanup_simple_shuffle(void);
bool init_persistent_i2s_element(void);
void cleanup_persistent_i2s_element(void);
static void codec_mute_timer_callback(void *arg);
static esp_err_t init_codec_mute_timer(void);
static void codec_unmute_for_i2s_playback(void);
static void codec_start_mute_timer(void);
static void codec_stop_mute_timer(void);
// Playback tracking functions
static const char* extract_filename(const char* full_path);
static time_t get_actual_playback_duration(void);
static void start_playback_tracking(const char* file_path);
static void pause_playback_tracking(void);
static void resume_playback_tracking(void);
static void finish_playback_tracking(bool completed_naturally);
static void save_tracking_record_if_active(void);
static void cleanup_playback_tracking(void);
int track_name_compare(const void *a, const void *b);

/**
 * @brief Get list of filenames from account_file.json for a specific SKURC SKU
 * @param sku The SKURC SKU to look for (e.g., "SKURC-045CA493C12A81")
 * @param filename_count Output parameter - number of filenames found
 * @return Array of filename strings, or NULL on error. Caller must free array and strings.
 */
static char** get_skurc_filenames_from_account(const char *sku, int *filename_count) {
    if (!sku || !filename_count) {
        return NULL;
    }
    
    *filename_count = 0;
    
    // Read account_file.json from /sdcard/tmp/
    // Get file size using stat
    struct stat st;
    if (stat("/sdcard/tmp/account_file.json", &st) != 0) {
        ESP_LOGD(TAG, "Could not stat /sdcard/tmp/account_file.json for SKURC lookup");
        return NULL;
    }

    long file_size = st.st_size;
    if (file_size <= 0 || file_size > 1024*1024) { // Max 1MB safety check
        return NULL;
    }

    FILE *fp = fopen("/sdcard/tmp/account_file.json", "r");
    if (!fp) {
        ESP_LOGD(TAG, "Could not open /sdcard/tmp/account_file.json for SKURC lookup");
        return NULL;
    }
    
    // Read file
    char *json_string = malloc(file_size + 1);
    if (!json_string) {
        fclose(fp);
        return NULL;
    }
    
    size_t read_size = fread(json_string, 1, file_size, fp);
    fclose(fp);
    json_string[read_size] = '\0';
    
    // Parse JSON
    cJSON *root = cJSON_Parse(json_string);
    free(json_string);
    
    if (!root) {
        return NULL;
    }
    
    char **filenames = NULL;
    int count = 0;
    
    // Navigate: result -> NFCs -> find matching SKU -> contents -> filenames
    cJSON *result = cJSON_GetObjectItem(root, "result");
    cJSON *nfcs = result ? cJSON_GetObjectItem(result, "NFCs") : NULL;
    
    if (cJSON_IsArray(nfcs)) {
        int nfc_count = cJSON_GetArraySize(nfcs);
        for (int i = 0; i < nfc_count && !filenames; i++) {
            cJSON *nfc = cJSON_GetArrayItem(nfcs, i);
            cJSON *skus = cJSON_IsObject(nfc) ? cJSON_GetObjectItem(nfc, "skus") : NULL;
            
            if (cJSON_IsArray(skus)) {
                int sku_count = cJSON_GetArraySize(skus);
                for (int j = 0; j < sku_count && !filenames; j++) {
                    cJSON *sku_obj = cJSON_GetArrayItem(skus, j);
                    cJSON *skuId = cJSON_IsObject(sku_obj) ? cJSON_GetObjectItem(sku_obj, "skuId") : NULL;
                    
                    if (cJSON_IsString(skuId) && strcmp(skuId->valuestring, sku) == 0) {
                        // Found our SKURC SKU
                        cJSON *contents = cJSON_GetObjectItem(sku_obj, "contents");
                        if (cJSON_IsArray(contents)) {
                            int content_count = cJSON_GetArraySize(contents);
                            if (content_count > 0) {
                                filenames = malloc(content_count * sizeof(char*));
                                if (filenames) {
                                    for (int k = 0; k < content_count; k++) {
                                        cJSON *content = cJSON_GetArrayItem(contents, k);
                                        cJSON *filename = cJSON_IsObject(content) ? cJSON_GetObjectItem(content, "filename") : NULL;

                                        if (cJSON_IsString(filename)) {
                                            filenames[count] = strdup_spiram(filename->valuestring);
                                            if (filenames[count]) {
                                                count++;
                                            }
                                        }
                                    }
                                }
                            }
                        }
                    }
                }
            }
        }
    }
    
    cJSON_Delete(root);
    
    if (count > 0) {
        *filename_count = count;
        ESP_LOGI(TAG, "Found %d SKURC filenames for SKU '%s' from account data", count, sku);
        return filenames;
    }
    
    // Cleanup on failure
    if (filenames) {
        for (int i = 0; i < count; i++) {
            free(filenames[i]);
        }
        free(filenames);
    }
    
    return NULL;
}

// Audio handlers
// Global variable s3_current_alarm is defined in s3_definitions.c and declared in s3_definitions.h

/**
 * UNUSED FUNCTION - Can be removed since we use periodic check instead
 * 
 * @brief Audio event handler for automatic pipeline cleanup
 * @param evt Audio event interface handle
 * @param event Audio event message
 * @param event_data Event data
 * @param timeout_ms Timeout in milliseconds
 * @return esp_err_t ESP_OK on success
 */
/*
static esp_err_t audio_event_handler(audio_event_iface_msg_t *event, void *context)
{
    // Check if this is the MP3 decoder finishing
    if (event->source == (void *)mp3_decoder && event->cmd == AEL_MSG_CMD_REPORT_STATUS) {
        audio_element_state_t el_state = audio_element_get_state(mp3_decoder);
        if (el_state == AEL_STATE_FINISHED) {
            ESP_LOGI(TAG, "MP3 decoder finished - audio completed naturally");
            
            // Use a short timeout to avoid blocking the event handler too long
            if (xSemaphoreTake(audio_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
                stop_active_pipeline_internal();
                ESP_LOGI(TAG, "Pipeline automatically cleaned up after MP3 finished");
                xSemaphoreGive(audio_mutex);
                
                // Trigger auto-play logic (all the AUTO_PLAY_* handling is already in one_step_track)
                if (s3_auto_play_mode != AUTO_PLAY_OFF) {
                    ESP_LOGI(TAG, "Auto-play enabled - advancing to next track");
                    one_step_track(true);  // This handles all AUTO_PLAY_* modes correctly
                    // Ensure playback starts for the new track
                    ESP_LOGI(TAG, "Starting playback of next track");
                    audio_start_playing();
                }
            } else {
                ESP_LOGW(TAG, "Could not acquire mutex for automatic cleanup");
            }
        }
    }
    
    return ESP_OK;
}
*/

/** @brief Ensure the audio system is ready. */
static bool ensure_audio_system_ready(void)
{
    ESP_LOGI(TAG, "ensure_audio_system_ready()");

    if (!is_powered_on) {
        ESP_LOGW(TAG, "Audio system not powered on - attempting to power on");
        audio_power_on();

        // Wait a while for the system boot
        vTaskDelay(pdMS_TO_TICKS(100));

        if (!is_powered_on) {
            ESP_LOGE(TAG, "Failed to power on audio system");
            return false;
        }
    }
    return true;
}

esp_err_t audio_player_init(void)
{
    ESP_LOGI(TAG, "audio_player_init()");

    if (audio_mutex == NULL) {
        audio_mutex = xSemaphoreCreateMutex();
        if (audio_mutex == NULL) {
            ESP_LOGE(TAG, "Failed to create audio mutex");
            return ESP_FAIL;
        } else {
            ESP_LOGI(TAG, "Audio mutex initialized");
        }
    }

    if (track_mutex == NULL) {
        track_mutex = xSemaphoreCreateMutex();
        if (track_mutex == NULL) {
            ESP_LOGE(TAG, "Failed to create track mutex");
            return ESP_FAIL;
        } else {
            ESP_LOGI(TAG, "Track mutex initialized");
        }
    }

    vTaskDelay(pdMS_TO_TICKS(100));
    audio_power_on();

    // Initialize persistent I2S element
    if (!init_persistent_i2s_element()) {
        ESP_LOGE(TAG, "Failed to initialize persistent I2S element");
        return ESP_FAIL;
    }

    // Initialize codec mute timer
    if (init_codec_mute_timer() != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize codec mute timer");
        return ESP_FAIL;
    }

    // Start setting up albums (obs.: lang is not needed anymore in album manager)
    cleanup_simple_shuffle();
    audio_update_album_data();

    return ESP_OK;
}

/**
 * @brief Clean up audio player and persistent elements
 */
void audio_player_cleanup(void)
{
    ESP_LOGI(TAG, "audio_player_cleanup()");

    // Stop any active pipeline first
    if (audio_mutex && xSemaphoreTake(audio_mutex, pdMS_TO_TICKS(1000)) == pdTRUE) {
        if (active_pipeline) {
            stop_active_pipeline_internal();
        }
        xSemaphoreGive(audio_mutex);
    }

    // Clean up persistent I2S element
    cleanup_persistent_i2s_element();

    // Clean up codec mute timer
    if (codec_mute_timer) {
        esp_timer_stop(codec_mute_timer);
        esp_timer_delete(codec_mute_timer);
        codec_mute_timer = NULL;
        ESP_LOGI(TAG, "Codec mute timer cleaned up");
    }

    // Clean up tracking state (#15141)
    cleanup_playback_tracking();

    // Clean up mutexes
    if (track_mutex) {
        vSemaphoreDelete(track_mutex);
        track_mutex = NULL;
    }
    if (audio_mutex) {
        vSemaphoreDelete(audio_mutex);
        audio_mutex = NULL;
    }

    ESP_LOGI(TAG, "Audio player cleanup complete");
}

/**
 * @brief Timer callback to mute ALC5616 codec after 5 seconds
 */
static void codec_mute_timer_callback(void *arg)
{
    if (!codec_is_muted) {
        ESP_LOGI(TAG, "Auto-muting ALC5616 codec after 5 seconds");
        alc5616_codec_set_voice_mute(true);
        codec_is_muted = true;
    }
}

/**
 * @brief Initialize codec mute timer
 */
static esp_err_t init_codec_mute_timer(void)
{
    if (codec_mute_timer != NULL) {
        return ESP_OK;  // Already initialized
    }

    esp_timer_create_args_t timer_args = {
        .callback = codec_mute_timer_callback,
        .arg = NULL,
        .name = "codec_mute_timer"
    };

    esp_err_t ret = esp_timer_create(&timer_args, &codec_mute_timer);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create codec mute timer: %s", esp_err_to_name(ret));
    }
    return ret;
}

/**
 * @brief Unmute codec for I2S playback
 */
static void codec_unmute_for_i2s_playback(void)
{
    if (codec_is_muted  && i2s_element_initialized && persistent_i2s_writer) {
        ESP_LOGI(TAG, "Unmuting ALC5616 codec for I2S playback");
        alc5616_codec_set_voice_mute(false);
        codec_is_muted = false;
    }
}

/**
 * @brief Start 5-second mute timer
 */
static void codec_start_mute_timer(void)
{
    if (codec_mute_timer != NULL) {
        esp_timer_stop(codec_mute_timer);  // Stop any existing timer
        esp_err_t ret = esp_timer_start_once(codec_mute_timer, 5000000);  // 5 seconds in microseconds
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to start codec mute timer: %s", esp_err_to_name(ret));
        } else {
            ESP_LOGD(TAG, "Started 5-second codec mute timer");
        }
    }
}

/**
 * @brief Stop mute timer (when playback starts)
 */
static void codec_stop_mute_timer(void)
{
    if (codec_mute_timer != NULL) {
        esp_timer_stop(codec_mute_timer);
        ESP_LOGD(TAG, "Stopped codec mute timer");
    }
}

/**
 * @brief Initialize persistent I2S element (created once, reused across playbacks)
 */
bool init_persistent_i2s_element(void)
{
    if (i2s_element_initialized && persistent_i2s_writer) {
        ESP_LOGD(TAG, "Persistent I2S element already initialized");
        return true;
    }

    ESP_LOGI(TAG, "Creating persistent I2S writer element");

    i2s_stream_cfg_t i2s_cfg = I2S_STREAM_CFG_DEFAULT();
    i2s_cfg.type = AUDIO_STREAM_WRITER;
    i2s_cfg.out_rb_size = 20 * 1024;        // 20KB ringbuffer = ~106ms @ 48kHz (improved underrun protection)
    i2s_cfg.chan_cfg.dma_desc_num = 6;      // 6 descriptors for smooth playback
    i2s_cfg.chan_cfg.dma_frame_num = 624;   // 624 frames = ~78ms @ 48kHz

    persistent_i2s_writer = i2s_stream_init(&i2s_cfg);
    if (persistent_i2s_writer == NULL) {
        ESP_LOGE(TAG, "Failed to initialize persistent I2S writer");
        i2s_element_initialized = false;
        return false;
    }
    
    // Disable event generation for persistent I2S writer
    audio_element_set_event_callback(persistent_i2s_writer, NULL, NULL);
    ESP_LOGD(TAG, "Disabled events for persistent_i2s_writer");

    i2s_element_initialized = true;
    ESP_LOGI(TAG, "Persistent I2S writer element created successfully");
    return true;
}

/**
 * @brief Clean up persistent I2S element
 */
void cleanup_persistent_i2s_element(void)
{
    if (persistent_i2s_writer) {
        ESP_LOGI(TAG, "Cleaning up persistent I2S writer element");
        
        // Mute codec BEFORE deinitializing I2S element to prevent audio pop
        if (s3_active_sink == AUDIO_SINK_I2S && !codec_is_muted) {
            ESP_LOGI(TAG, "Muting ALC5616 codec before I2S cleanup to prevent audio pop");
            alc5616_codec_set_voice_mute(true);
            codec_is_muted = true;
            // Allow mute command to take effect
            vTaskDelay(pdMS_TO_TICKS(50));
        }
        
        audio_element_deinit(persistent_i2s_writer);
        persistent_i2s_writer = NULL;
        i2s_element_initialized = false;
        ESP_LOGI(TAG, "Persistent I2S element cleaned up");
    }
}

/**
 * @brief Clean up simple shuffle system
 */
static void cleanup_simple_shuffle(void) {
    if (current_shuffle_order) {
        free(current_shuffle_order);
        current_shuffle_order = NULL;
    }
    shuffle_count = 0;
    shuffle_position = 0;
}

/**
 * @brief check state of the audio pipeline
 */
static bool is_pipeline_stopped()
{
    ESP_LOGI(TAG, "is_pipeline_stopped()");

    if (active_pipeline == NULL || current_sink_element == NULL) return true;

    audio_element_state_t state = audio_element_get_state(current_sink_element);
    ESP_LOGD(TAG, "Pipeline state: %d", state);
    return (state == AEL_STATE_INIT || state == AEL_STATE_FINISHED || state == AEL_STATE_STOPPED);
}

/**
 * @brief Wait for pipeline to completely stop with timeout
 */
static bool wait_for_pipeline_stop(uint32_t timeout_ms)
{
    ESP_LOGI(TAG, "wait_for_pipeline_stop(timeout=%d)", timeout_ms);
    
    if (active_pipeline == NULL) return true;
    
    uint32_t start_time = xTaskGetTickCount();
    uint32_t timeout_ticks = pdMS_TO_TICKS(timeout_ms);
    
    while (!is_pipeline_stopped()) {
        if ((xTaskGetTickCount() - start_time) >= timeout_ticks) {
            ESP_LOGW(TAG, "Pipeline stop timeout after %d ms", timeout_ms);
            return false;
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    
    ESP_LOGI(TAG, "Pipeline stopped successfully");
    return true;
}

/**
 * @brief Safely stop and cleanup any active audio pipeline
 * @note This version doesn't attempt to take the mutex, must be called from a function that already holds it
 */
static void stop_active_pipeline_internal(void)
{
    ESP_LOGI(TAG, "stop_active_pipeline_internal()");

    if (active_pipeline == NULL) {
        audio_state = AUDIO_STATE_STOPPED;
        return;
    }

    // Save tracking record for manual stops (#15141)
    save_tracking_record_if_active();

    // Codec mute handled by timer system after pipeline stops

    // If the active sink was A2DP, tell the BT stack to stop streaming
    if (s3_active_sink == AUDIO_SINK_A2DP) {
        bt_a2dp_stop_media();
        // Give time for L2CAP layer to flush buffers - reduced to prevent audio event queue overflow
        vTaskDelay(pdMS_TO_TICKS(500));  // Reduced from 1500ms - balance between L2CAP flush and event queue health
    }

    ESP_LOGI(TAG, "Terminating pipeline...");
    audio_pipeline_stop(active_pipeline);
    audio_pipeline_wait_for_stop(active_pipeline);
    audio_pipeline_terminate(active_pipeline);
    
    // Wait for element tasks to reach stopped state before deinit
    // This prevents crash when deinit destroys event groups while tasks are still exiting
    uint16_t wait_ms = 0;
    const uint16_t max_wait_ms = 200;  // 200ms max wait
    while (wait_ms < max_wait_ms) {
        // Check individual element states since there's no audio_pipeline_get_state()
        bool all_stopped = true;
        if (fatfs_reader) {
            audio_element_state_t state = audio_element_get_state(fatfs_reader);
            if (state != AEL_STATE_STOPPED && state != AEL_STATE_INIT) {
                all_stopped = false;
            }
        }
        if (mp3_decoder && all_stopped) {
            audio_element_state_t state = audio_element_get_state(mp3_decoder);
            if (state != AEL_STATE_STOPPED && state != AEL_STATE_INIT) {
                all_stopped = false;
            }
        }
        if (current_sink_element && current_sink_element != persistent_i2s_writer && all_stopped) {
            audio_element_state_t state = audio_element_get_state(current_sink_element);
            if (state != AEL_STATE_STOPPED && state != AEL_STATE_INIT) {
                all_stopped = false;
            }
        }
        
        if (all_stopped) {
            ESP_LOGI(TAG, "All elements stopped after %d ms", wait_ms);
            break;
        }
        
        vTaskDelay(pdMS_TO_TICKS(10));
        wait_ms += 10;
    }

    // DMA memory leak fix: audio_pipeline_deinit() handles everything
    // It calls audio_pipeline_unlink() (frees ringbuffers) and audio_element_deinit() (frees elements)
    // We only need to unregister the persistent I2S element to prevent it from being destroyed

    // Unregister persistent I2S element BEFORE deinit to preserve it
    if (current_sink_element == persistent_i2s_writer) {
        audio_pipeline_unregister(active_pipeline, persistent_i2s_writer);
        ESP_LOGI(TAG, "Unregistered persistent I2S element before pipeline deinit");
    }

    // De-initialize the pipeline (this unlinks ringbuffers and deinits all REGISTERED elements)
    audio_pipeline_deinit(active_pipeline);

    // No need to manually deinit elements - audio_pipeline_deinit() already did it
    // Exception: If we unregistered persistent I2S, we don't want it deinitialized anyway

    // Reset the handles
    active_pipeline = NULL;
    fatfs_reader = NULL;
    xor_filter = NULL;
    mp3_decoder = NULL;
    current_sink_element = NULL;
    audio_state = AUDIO_STATE_STOPPED;  // Reset state machine when stopping

    // After playback stops, reset standby timer so screen doesn't immediately go black
    app_timeout_reset();

    // For I2S sink only: start mute timer after playback stops
    if (s3_active_sink == AUDIO_SINK_I2S) {
        codec_start_mute_timer();  // Start 5-second mute timer
    }

    // Clean up sound effect state when stopping pipeline
    if (sound_effect_playing) {
        sound_effect_playing = false;
        was_playing_before_effect = false;
        if (saved_track_uri) {
            free(saved_track_uri);
            saved_track_uri = NULL;
        }
    }
    
    // Notify Bluetooth that audio stopped - triggers deferred A2DP connection if pending
    // This prevents crash when BT connects while audio is playing (race condition)
    extern void bt_notify_audio_stopped(void);
    bt_notify_audio_stopped();
}

/**
 * @brief Thread-safe wrapper for stopping pipeline
 */
static void stop_active_pipeline(void)
{
    ESP_LOGI(TAG, "stop_active_pipeline()");

    if (xSemaphoreTake(audio_mutex, pdMS_TO_TICKS(200)) == pdTRUE) {
        stop_active_pipeline_internal();
        xSemaphoreGive(audio_mutex);
    } else {
        ESP_LOGE(TAG, "Failed to acquire mutex for pipeline stop");
    }
}

/** @brief Check if audio is currently playing (using state machine). */
bool is_audio_playing(void) {
    return is_state_playing();
}

/**
 * @brief Initialize audio pipeline components
 * @param sink_type The audio sink type (I2S or A2DP)
 * @param use_encryption Whether to include XOR decryption filter in pipeline
 * @return true if initialization succeeded, false otherwise
 */
static bool init_audio_pipeline(audio_sink_t sink_type, bool use_encryption)
{
    ESP_LOGI(TAG, "init_audio_pipeline(sink_type=%d)", sink_type);

    if (active_pipeline) {
        stop_active_pipeline_internal();
    }

    audio_pipeline_cfg_t pipeline_cfg = DEFAULT_AUDIO_PIPELINE_CONFIG();
    // Increase buffer for A2DP
    if (sink_type == AUDIO_SINK_A2DP) {
        pipeline_cfg.rb_size = 64 * 1024; // Duplicate to A2DP
    } else {
        pipeline_cfg.rb_size = 64 * 1024; // Keep for I2S
    }
    active_pipeline = audio_pipeline_init(&pipeline_cfg);

    fatfs_stream_cfg_t fs_cfg = FATFS_STREAM_CFG_DEFAULT();

    fs_cfg.out_rb_size = 8 * 1024;  // 8K size is double of xor_filter's input 4K
    if (sink_type == AUDIO_SINK_A2DP) {
        fs_cfg.task_prio = 14;          // Below LVGL (18) to prevent UI starvation
        fs_cfg.task_core = 1;           // Core 1 - separate from A2DP/BT
    }
    fs_cfg.buf_sz = 2048;
    fs_cfg.type = AUDIO_STREAM_READER;

    fatfs_reader = fatfs_stream_init(&fs_cfg);
    if (fatfs_reader == NULL) {
        ESP_LOGE(TAG, "Failed to initialize fatfs reader");
        stop_active_pipeline();
        return false;
    }
    
    // Disable event generation for fatfs reader
    audio_element_set_event_callback(fatfs_reader, NULL, NULL);
    ESP_LOGD(TAG, "Disabled events for fatfs_reader");

#ifdef CONFIG_USE_ENCRYPTION
    if (use_encryption) {
        // Initialize XOR filter only when encryption is needed
        xor_decrypt_cfg_t xor_cfg = DEFAULT_XOR_DECRYPT_CONFIG();
        xor_cfg.out_rb_size = 12 * 1024; // Optimized 6KB - compromise between 4KB default and 16KB
        if (sink_type == AUDIO_SINK_A2DP) {
            xor_cfg.task_core = 1;       // Core 1 - separate from A2DP/BT
        }
        xor_filter = xor_decrypt_filter_init(&xor_cfg);
        if (xor_filter == NULL) {
            ESP_LOGE(TAG, "Failed to initialize xor_filter");
            stop_active_pipeline();
            return false;
        }

        // Disable event generation for XOR filter
        audio_element_set_event_callback(xor_filter, NULL, NULL);
        ESP_LOGD(TAG, "Disabled events for xor_filter");
        
        ESP_LOGI(TAG, "XOR decryption filter initialized");
    } else {
        xor_filter = NULL; // No encryption filter needed
        ESP_LOGI(TAG, "Skipping XOR decryption filter - playing unencrypted content");
    }
#endif

    // Initialize MP3 decoder

    mp3_decoder_cfg_t mp3_cfg = DEFAULT_MP3_DECODER_CONFIG();

    mp3_cfg.out_rb_size = 32 * 1024; // Reduced from 32KB to 16KB - prevents L2CAP congestion (Bug #2)
    // Optimized ring buffer sizes - balanced for throughput vs BT congestion
    if (sink_type == AUDIO_SINK_A2DP) {
        mp3_cfg.task_prio = 15;          // Below LVGL (18) but above file reader (14)
        mp3_cfg.task_core = 1;           // Core 1 - separate from A2DP/BT for DSP processing
    }

    /* Previous high-memory settings (for reference):
    if (sink_type == AUDIO_SINK_A2DP) {
        mp3_cfg.out_rb_size = 32 * 1024; // 32KB to A2DP - optimized for fast cycles
        mp3_cfg.task_prio = 19;          // High priority - swapped with File Reader
        mp3_cfg.task_core = 1;           // Core 1 - separate from A2DP/BT for DSP processing
    } else {
        mp3_cfg.out_rb_size = 16 * 1024; // 16KB to I2S
    }
    */
    mp3_decoder = mp3_decoder_init(&mp3_cfg);
    if (mp3_decoder == NULL) {
        ESP_LOGE(TAG, "Failed to initialize mp3 decoder");
        stop_active_pipeline();
        return false;
    }
    
    // Disable event generation for MP3 decoder
    audio_element_set_event_callback(mp3_decoder, NULL, NULL);
    ESP_LOGD(TAG, "Disabled events for mp3_decoder");

    ESP_LOGI(TAG, "Creating sink element for: %s", sink_type == AUDIO_SINK_A2DP ? "A2DP" : "I2S");
    if (sink_type == AUDIO_SINK_A2DP) {
        a2dp_stream_config_t a2dp_config = { .type = AUDIO_STREAM_WRITER };
        current_sink_element = a2dp_stream_init(&a2dp_config);

        // Allow A2DP stream to stabilize before pipeline setup to prevent L2CAP congestion
        if (current_sink_element != NULL) {
            // Disable event generation for A2DP sink
            audio_element_set_event_callback(current_sink_element, NULL, NULL);
            ESP_LOGD(TAG, "Disabled events for a2dp_sink");
            
            vTaskDelay(pdMS_TO_TICKS(100));  // Allow A2DP initialization and BLE coordination
            ESP_LOGI(TAG, "A2DP stream initialized with stabilization delay");
        }
    } else {
        // Use persistent I2S element instead of creating new one
        if (!i2s_element_initialized || !persistent_i2s_writer) {
            ESP_LOGE(TAG, "Persistent I2S element not initialized");
            if (!init_persistent_i2s_element()) {
                ESP_LOGE(TAG, "Failed to initialize persistent I2S element");
                stop_active_pipeline_internal();
                return ESP_FAIL;
            }
            codec_unmute_for_i2s_playback();
        }
        current_sink_element = persistent_i2s_writer;
        ESP_LOGI(TAG, "Reusing persistent I2S writer element");
    }

    if (active_pipeline == NULL || fatfs_reader == NULL || mp3_decoder == NULL || current_sink_element == NULL) {
        ESP_LOGE(TAG, "Failed to initialize one or more pipeline elements");
        stop_active_pipeline_internal();
        return false;
    }

    audio_pipeline_register(active_pipeline, fatfs_reader, "file");
#ifdef CONFIG_USE_ENCRYPTION
    if (use_encryption) {
        audio_pipeline_register(active_pipeline, xor_filter, "XOR");
    }
#endif
    audio_pipeline_register(active_pipeline, mp3_decoder, "mp3");
    audio_pipeline_register(active_pipeline, current_sink_element, "output");

#ifdef CONFIG_USE_ENCRYPTION
    if (use_encryption) {
        const char *link_tag[4] = {"file", "XOR", "mp3", "output"};
        if (audio_pipeline_link(active_pipeline, link_tag, 4) != ESP_OK) {
            ESP_LOGE(TAG, "Failed to link pipeline elements with encryption");
            stop_active_pipeline();
            return false;
        }
    } else {
        const char *link_tag[3] = {"file", "mp3", "output"};
        if (audio_pipeline_link(active_pipeline, link_tag, 3) != ESP_OK) {
            ESP_LOGE(TAG, "Failed to link pipeline elements without encryption");
            stop_active_pipeline_internal();
            return false;
        }
    }
#else
    const char *link_tag[3] = {"file", "mp3", "output"};
    if (audio_pipeline_link(active_pipeline, link_tag, 3) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to link pipeline elements");
        stop_active_pipeline_internal();
        return false;
    }
#endif

    ESP_LOGI(TAG, "Pipeline initialized with automatic cleanup via main loop check");
    
    return true;
}

/**
 * @brief The new master function to play any media to a specified sink
 */
static bool audio_play_internal(const char *path, audio_sink_t sink_pref)
{
    if (path == NULL || *path == '\0') {
        ESP_LOGE(TAG, "audio_play_internal: NULL or empty path");
        return false;
    }

    audio_sink_t sink = (sink_pref == AUDIO_SINK_AUTO) ? (bt_is_a2dp_connected() ? AUDIO_SINK_A2DP : AUDIO_SINK_I2S): sink_pref;

    ESP_LOGI(TAG, "audio_play_internal(path=\"%s\", sink=%d)", path, sink);
    
    // Auto-disconnect WiFi to free DMA RAM for audio playback
    if (is_wifi_connected()) {
        ESP_LOGW(TAG, "WiFi connected detected - auto-disconnecting to free DMA RAM for audio");
        memory_status();  // Show memory status before WiFi disconnect
        disconnect_wifi_with_cleanup();
        // power off or reboot, release WiFi DMA?
        if (global_poweroff != POWER_MODE_NORMAL) {
            esp_wifi_deinit();
        }
        ESP_LOGI(TAG, "WiFi disconnected - DMA RAM freed for audio playback");
        memory_status();  // Show memory status after WiFi disconnect
    }

    /* 3. Take the mutex so only one playback request is processed at once */
    if (xSemaphoreTake(audio_mutex, pdMS_TO_TICKS(2000)) != pdTRUE) {
        ESP_LOGW(TAG, "audio_play_internal: timed-out waiting for mutex");
        return false;
    }

    bool success = false;                    /* final return value */

    do { /* single-iteration loop lets us break on any error */

        /* 4. Make sure the file exists ---------------------------------- */
        struct stat st;
        if (stat(path, &st) != 0) {
            ESP_LOGE(TAG, "audio_play_internal: file does not exist: %s", path);
            break;
        }

        /* 5. Stop anything that was already playing --------------------- */
        stop_active_pipeline_internal();
        
        /* 5.1. Wait for pipeline to completely stop -------------------- */
        if (!wait_for_pipeline_stop(1000)) {
            ESP_LOGW(TAG, "Previous pipeline did not stop cleanly");
            // Force cleanup anyway
            active_pipeline = NULL;
            fatfs_reader = NULL;
            xor_filter = NULL;
            mp3_decoder = NULL;
            current_sink_element = NULL;
            audio_state = AUDIO_STATE_STOPPED;
        }

        /* 6. Power-up codec/board if needed ----------------------------- */
        if (!ensure_audio_system_ready()) {
            ESP_LOGE(TAG, "audio_play_internal: audio HW not ready");
            break;
        }

        /* For I2S sink only: unmute codec and stop mute timer ------- */
        if (sink == AUDIO_SINK_I2S) {
            codec_stop_mute_timer();    // Stop mute timer if running
            codec_unmute_for_i2s_playback();  // Unmute codec before playback
            vTaskDelay(pdMS_TO_TICKS(300)); // Give some time before pipeline start
        }


        /* 7. Build a brand-new pipeline for the chosen sink ------------- */
        // Determine if we need encryption based on file path and current album SKU
        bool use_encryption = true; // Default to encrypted
        bool is_root_sdcard_file = (strncmp(path, "/sdcard/", 8) == 0 && strchr(path + 8, '/') == NULL);

        if (is_root_sdcard_file) {
            ESP_LOGI(TAG, "Root /sdcard/ file detected (%s) - disabling encryption for non-encrypted content", path);
            use_encryption = false;
        } else if (s3_current_album && s3_current_album->sku) {
            // Disable encryption ONLY for SKURC/ISR recording content
            if (strncmp(s3_current_album->sku, "SKURC-", 6) == 0 || strncmp(s3_current_album->sku, "ISR-", 4) == 0) {
                ESP_LOGI(TAG, "Recording album detected (%s) - disabling encryption for unencrypted content", s3_current_album->sku);
                use_encryption = is_alarm_on_blankee;
            } else {
                ESP_LOGI(TAG, "Regular album detected (%s) - using encryption for encrypted content", s3_current_album->sku);
            }
        }
        
        if (!init_audio_pipeline(sink, use_encryption)) {
            ESP_LOGE(TAG, "audio_play_internal: pipeline init failed");
            break;
        }
        s3_active_sink = sink;

        /* 7.1. For I2S sink only: stop mute timer (unmute happens AFTER buffering) */
        if (sink == AUDIO_SINK_I2S) {
            codec_stop_mute_timer();    // Stop mute timer if running
            // Note: Unmuting is deferred until after buffering to prevent initial pops/clicks
        }

        /* 8. Point file-reader to the MP3 and start the pipeline -------- */
        audio_element_set_uri(fatfs_reader, path);
        if (audio_pipeline_run(active_pipeline) != ESP_OK) {
            ESP_LOGE(TAG, "audio_play_internal: pipeline run failed");
            stop_active_pipeline_internal(); /* clean up on failure         */
            break;
        }

        /* 9. Enhanced buffer pre-fill strategy to eliminate initial chopping */
        // Get the mp3 decoder's output ringbuffer to check fill level
        ringbuf_handle_t decoder_out_rb = audio_element_get_output_ringbuf(mp3_decoder);
        if (decoder_out_rb) {
            int rb_size = rb_get_size(decoder_out_rb);
            // More aggressive buffering: 75% for I2S (more susceptible to underruns), 60% for A2DP
            int target_fill = (sink == AUDIO_SINK_I2S) ? (rb_size * 3 / 4) : (rb_size * 3 / 5);
            uint16_t wait_count = 0;
            const uint16_t max_wait_ms = 1200;   // Slightly longer timeout for better buffering

            ESP_LOGI(TAG, "Waiting for decoder buffer to fill (target: %d/%d bytes, sink=%s)", 
                     target_fill, rb_size, (sink == AUDIO_SINK_I2S) ? "I2S" : "A2DP");

            while (wait_count < max_wait_ms) {
                int filled = rb_bytes_filled(decoder_out_rb);
                if (filled >= target_fill) {
                    ESP_LOGI(TAG, "Decoder buffer ready: %d bytes filled in %d ms", filled, wait_count);
                    break;
                }
                vTaskDelay(pdMS_TO_TICKS(20));  // Check every 20ms - faster response to prevent underruns
                wait_count += 20;
            }

            if (wait_count >= max_wait_ms) {
                ESP_LOGW(TAG, "Decoder buffer fill timeout - starting anyway (filled: %d/%d)",
                         rb_bytes_filled(decoder_out_rb), rb_size);
            }
            
            // Additional I2S-specific buffering: Allow DMA buffers to pre-fill
            if (sink == AUDIO_SINK_I2S) {
                ESP_LOGI(TAG, "I2S sink: additional DMA buffer pre-fill delay");
                vTaskDelay(pdMS_TO_TICKS(100));  // Extra 100ms for I2S DMA buffers to fill
            }
        } else {
            // Fallback to fixed delay if we can't get ringbuffer
            ESP_LOGW(TAG, "Cannot get decoder ringbuffer - using fixed delay");
            if (sink == AUDIO_SINK_I2S) {
                vTaskDelay(pdMS_TO_TICKS(400));  // Longer delay for I2S to ensure smooth start
            } else {
                vTaskDelay(pdMS_TO_TICKS(250));  // A2DP has more internal buffering
            }
        }

        /* 9.1. Start media streaming based on sink type */
        if (s3_active_sink == AUDIO_SINK_A2DP) {
            bt_a2dp_start_media();
        } else if (s3_active_sink == AUDIO_SINK_I2S) {
            // Unmute codec AFTER buffers have filled
            // I2S DMA buffers will naturally fill from decoder output while we waited above
            codec_unmute_for_i2s_playback();
            ESP_LOGI(TAG, "Codec unmuted after buffer pre-fill");
        }

        /* 10. Success! ---------------------------------------------------- */
        audio_state = AUDIO_STATE_PLAYING;
        ESP_LOGI(TAG, "audio_play_internal: playback started");

        /* 10.1. Track audio type as TRACK (album playback) -------------- */
        current_audio_type = AUDIO_TYPE_TRACK;

        /* 10.2. Start playback tracking (#15141) ------------------------ */
        start_playback_tracking(path);

        success = true;

    } while (0);

    xSemaphoreGive(audio_mutex);
    return success;
}

void reset_albums_from_nfc(void)
{
    // Save current album index before resetting
    size_t saved_album_idx = s3_current_idx;

    ESP_LOGI(TAG,"s3_current_album: %p", s3_current_album);
    ESP_LOGI(TAG,"s3_current_album->is_available_nfc: %d", s3_current_album ? s3_current_album->is_available_nfc : false);
    if ( s3_current_album != NULL && s3_current_album->is_available_nfc)
    {
        ESP_LOGI(TAG, "Resetting albums from NFC data");

        if (xSemaphoreTake(audio_mutex, pdMS_TO_TICKS(1000)) == pdTRUE) {
            // Stop any active playback
            //
            if (is_state_playing()) {
                stop_active_pipeline_internal();
            }

            // Clear current album data
            cleanup_simple_shuffle();
            audio_update_album_data();

            // Restore to the previously playing album if it's still valid
            if (saved_album_idx < s3_albums_get_size()) {
                ESP_LOGI(TAG, "Restoring to previously playing album index: %u", (unsigned int)saved_album_idx);

                // Update global pointers directly since we already have the mutex
                s3_current_idx = saved_album_idx;
                s3_current_album = s3_albums_get(s3_current_idx);

                if (s3_current_album && s3_current_album->path) {
                    // Build playlist immediately to have track count
                    build_playlist();

                    // Reset track index to 0 for the restored album
                    s3_current_idx_track = 0;

                    ESP_LOGI(TAG, "Album restored to [%u/%u] → %s",
                             (unsigned int)(s3_current_idx + 1), (unsigned int)s3_current_size, s3_current_album->name);
                } else {
                    ESP_LOGE(TAG, "Failed to restore album - selected album has NULL fields");
                }
            } else {
                ESP_LOGI(TAG, "Previously playing album index %u is no longer valid, staying at current album", (unsigned int)saved_album_idx);
            }

            xSemaphoreGive(audio_mutex);
        } else {
            ESP_LOGE(TAG, "Failed to acquire mutex in reset_albums_from_sd.");
        }
    }
}

/**
 * @brief Play MP3 file from SD card. Merged from play_mp3, play_music and play_music_a2dp.
 * @param path Path to the MP3 file
 */
bool audio_play(const char *path, audio_sink_t sink_pref)
{
    // sink_pref is verified inside internal
    return audio_play_internal(path, sink_pref);
}







/**
 * @brief Play boot sound
 */
void play_audio_boot()
{
    if (!is_powered_on) {
        ESP_LOGW(TAG, "Audio system not powered on, cannot play boot sound");
        return;
    }
    if (is_state_playing()) {
        ESP_LOGW(TAG, "Audio is already playing, stopping current playback before boot sound");
        play_stop();
        // Give some time for cleanup
        vTaskDelay(pdMS_TO_TICKS(200));
    }
    ESP_LOGW(TAG, "Playing audio boot sound");
    current_audio_type = AUDIO_TYPE_EFFECT;
    play_auto_mode(BOOT_SOUND);
}

/**
 * @brief Play shutdown sound
 */
void play_audio_shutdown()
{
    if (!is_powered_on) {
        ESP_LOGW(TAG, "Audio system not powered on, cannot play shutdown sound");
        return;
    }
    if (is_state_playing()) {
        ESP_LOGW(TAG, "Audio is already playing, stopping current playback before shutdown sound");
        play_stop();
    }
    ESP_LOGW(TAG, "Playing audio shutdown sound");
    current_audio_type = AUDIO_TYPE_EFFECT;
    play_auto_mode(SHUTDOWN_SOUND);
}

/**
 * @brief Play volume sound
 */
void play_audio_volume()
{
    if (!is_powered_on) {
        ESP_LOGW(TAG, "Audio system not powered on, cannot play volume sound");
        return;
    }
    if (is_state_playing()) {
        ESP_LOGW(TAG, "Audio is already playing, stopping current playback before volume sound");
        play_stop();
    }
    ESP_LOGW(TAG, "Playing audio volume sound");
    current_audio_type = AUDIO_TYPE_EFFECT;
    play_auto_mode(VOLUME_SOUND);
}

// Global variable to track if we stopped audio for alarm (should resume after)
static bool audio_was_playing_before_alarm = false;

// Global variable to track if audio was paused due to BT disconnection (should resume when BT reconnects)
static bool audio_was_paused_due_bt_disconnect = false;

/**
 * @brief Play alarm sound
 */
void play_audio_alarm()
{
    if (!is_powered_on) {
        ESP_LOGW(TAG, "Audio system not powered on, cannot play alarm sound");
        return;
    }
    
    // Track if TRACK was playing before stopping it for alarm
    // Resume only if audio was playing AND it was a track (album)
    audio_was_playing_before_alarm = is_state_playing() && (current_audio_type == AUDIO_TYPE_TRACK);
    
    if (is_state_playing()) {
        if (audio_was_playing_before_alarm) {
            ESP_LOGW(TAG, "Track was playing, stopping current playback before alarm sound (will resume after alarm)");
        } else {
            ESP_LOGW(TAG, "Sound effect was playing, stopping before alarm sound (will NOT resume)");
        }
        is_alarm_on_blankee = true;
        play_stop();
        
        // Wait for pipeline to fully stop to prevent crash
        uint16_t wait_count = 0;
        const uint16_t max_wait_ms = 3000;  // Increased to 3 seconds (was 2) to handle queue congestion
        while (audio_state != AUDIO_STATE_STOPPED && wait_count < max_wait_ms) {
            vTaskDelay(pdMS_TO_TICKS(50));
            wait_count += 50;
        }
        
        if (audio_state != AUDIO_STATE_STOPPED) {
            ESP_LOGW(TAG, "Pipeline did not stop cleanly after %d ms, forcing cleanup", max_wait_ms);
        } else {
            ESP_LOGI(TAG, "Pipeline stopped successfully after %d ms", wait_count);
        }
        
        // Extra delay to let internal state fully settle, especially important when queue was congested
        vTaskDelay(pdMS_TO_TICKS(300));
    }
    ESP_LOGW(TAG, "Playing audio alarm sound %s (will repeat until dismissed or 10min timeout)", s3_current_alarm->name);
    alarm_should_repeat = true;  // Enable repeat for 10 minutes (set before play to prevent race)
    play_auto_mode(s3_current_alarm->audio);

    // IMPORTANT: Set these AFTER play_auto_mode() because audio_play_internal() overwrites them
    current_audio_type = AUDIO_TYPE_ALARM;
    ESP_LOGI(TAG, "Alarm audio type and flags set after playback started");
}

void resume_audio_to_now_playing(void)
{
    play_album();
    audio_was_playing_before_alarm = false;
}

/**
 * @brief Resume audio playback if it was playing before alarm started
 */
void resume_audio_after_alarm()
{
    // Only resume if audio was playing AND it was a track (not a system sound)
    if (audio_was_playing_before_alarm) {
        ESP_LOGI(TAG, "Resuming audio playback after alarm (was playing a track before alarm)");
        
        // Resume the current album/track
        play_album();
    } else {
        ESP_LOGI(TAG, "Not resuming audio after alarm (flag=%d, type=%d)", 
                 audio_was_playing_before_alarm, current_audio_type);
        // Note: only clear audio_type if it was NONE (user stopped it)
    }
    audio_was_playing_before_alarm = false;
}

/**
 * @brief Stop alarm repeat behavior (called when alarm is dismissed)
 */
void stop_alarm_repeat(void)
{
    ESP_LOGI(TAG, "Stopping alarm repeat - alarm dismissed");
    alarm_should_repeat = false;
    is_alarm_on_blankee = false;
}

/**
 * @brief Get the current audio type being played
 * @return Current audio type (TRACK, ALARM, BOOT, SHUTDOWN, VOLUME, EFFECT)
 * @note Use this to determine if audio was a track before alarm interrupted it
 */
audio_type_t get_current_audio_type(void)
{
    return current_audio_type;
}

/**
 * @brief Automatically pause audio when A2DP connection is lost
 * Called by BT manager on abrupt disconnect - pauses pipeline directly
 */
void pause_audio_for_bt_disconnect()
{
    ESP_LOGI(TAG, "pause_audio_for_bt_disconnect() - A2DP connection lost during playback");
    
    // Only pause if currently playing A2DP audio
    if (s3_active_sink == AUDIO_SINK_A2DP && is_audio_playing()) {
        ESP_LOGI(TAG, "A2DP audio was playing - pausing pipeline directly (no screen transition)");
        
        // Track that this pause was due to BT disconnect
        audio_was_paused_due_bt_disconnect = true;
        
        // Pause pipeline directly - state machine will show BLUETOOTH_SCAN_SCREEN via BT_STATUS_RECONNECTING
        play_pause();
        
        ESP_LOGI(TAG, "Audio paused - BT manager will trigger BLUETOOTH_SCAN_SCREEN");
    } else {
        ESP_LOGI(TAG, "Not pausing: sink=%d, playing=%d", s3_active_sink, is_audio_playing());
    }
}

/**
 * @brief Automatically resume audio when A2DP connection is restored
 * This function resumes audio only if it was previously paused due to BT disconnection
 */
void resume_audio_after_bt_reconnect()
{
    ESP_LOGI(TAG, "resume_audio_after_bt_reconnect() - A2DP connection restored");
    
    // Only resume if audio was paused due to BT disconnect, we're back on A2DP, and audio is paused
    // Note: Don't check screen - state machine handles screen transition after this call
    if (audio_was_paused_due_bt_disconnect && s3_active_sink == AUDIO_SINK_A2DP && is_audio_paused()) {
        ESP_LOGI(TAG, "Resuming audio that was paused due to A2DP disconnect");
        
        // Clear the BT disconnect pause flag
        audio_was_paused_due_bt_disconnect = false;
        
        // Resume the audio from where it was paused
        play_resume();
        
        ESP_LOGI(TAG, "Audio resumed successfully after A2DP reconnection");
    } else {
        ESP_LOGI(TAG, "Not resuming: bt_disconnect_flag=%d, sink=%d, is_paused=%d", 
                 audio_was_paused_due_bt_disconnect, s3_active_sink, is_audio_paused());
        
        // Clear the flag anyway to prevent stale state
        audio_was_paused_due_bt_disconnect = false;
    }
}

/**
 * @brief Clear the BT disconnect pause flag to prevent stale state
 * This should be called when leaving PLAY_SCREEN or when audio state changes
 */
void clear_bt_disconnect_pause_flag(void)
{
    ESP_LOGI(TAG, "clear_bt_disconnect_pause_flag() - clearing stale BT disconnect state");
    audio_was_paused_due_bt_disconnect = false;
}

/**
 * @brief Get current file position for pause/resume across sink changes
 * @return Current byte position in file, or -1 if no active playback
 */
int audio_get_file_position(void)
{
    if (!fatfs_reader || !active_pipeline) {
        ESP_LOGW(TAG, "No active pipeline to get file position from");
        return -1;
    }
    
    audio_element_info_t info = AUDIO_ELEMENT_INFO_DEFAULT();
    if (audio_element_getinfo(fatfs_reader, &info) != ESP_OK) {
        ESP_LOGW(TAG, "Failed to get element info");
        return -1;
    }
    
    int pos = (int)info.byte_pos;
    ESP_LOGI(TAG, "Current file position: %d bytes", pos);
    return pos;
}

/**
 * @brief Resume playback from a saved file position (for sink switching)
 * @param position Byte position in file to resume from
 * @note This rebuilds the pipeline with current sink and seeks to position
 */
void audio_play_from_position(int position)
{
    ESP_LOGI(TAG, "audio_play_from_position(position=%d)", position);
    
    if (position < 0) {
        ESP_LOGW(TAG, "Invalid position %d, playing from beginning", position);
        play_album();
        return;
    }
    
    // Play the current track
    play_album();
    
    // Wait for pipeline to start
    vTaskDelay(pdMS_TO_TICKS(200));
    
    // Seek to saved position
    if (fatfs_reader && active_pipeline) {
        ESP_LOGI(TAG, "Seeking to saved position: %d bytes", position);
        audio_element_set_byte_pos(fatfs_reader, position);
        ESP_LOGI(TAG, "Playback resumed from saved position");
    } else {
        ESP_LOGW(TAG, "Failed to seek - no active pipeline after play");
    }
}

/**
 * @brief Stop current playback immediately
 */
void play_stop(void) {

    ESP_LOGI(TAG, "play_stop()");

    suppress_auto_play_once = true; // Prevent auto-play increment after manual stop

    if (xSemaphoreTake(audio_mutex, pdMS_TO_TICKS(1000)) == pdTRUE) {
        // Stop if playing OR paused (any active audio state)
        if (is_state_playing() || is_state_paused()) {
            ESP_LOGI(TAG, "Stopping playback... (current state: %d)", audio_state);
            stop_active_pipeline_internal();
            // State is already set to STOPPED by stop_active_pipeline_internal()
            // Give pipeline time to properly stop
            vTaskDelay(pdMS_TO_TICKS(100));
        }
        
        // When user explicitly stops audio, reset audio type to NONE to prevent unintended resume
        // This handles the case where user stops track, navigates away, then alarm triggers
        current_audio_type = AUDIO_TYPE_NONE;
        ESP_LOGI(TAG, "Reset audio type to NONE and cleared alarm resume flag (user stopped playback)");
        
        xSemaphoreGive(audio_mutex);
    } else {
        ESP_LOGE(TAG, "Failed to acquire mutex in play_stop.");
    }
}

/**
 * @brief Pause the currently playing audio (with state machine protection)
 */
void play_pause(void) {
    ESP_LOGI(TAG, "play_pause()");

    if (xSemaphoreTake(audio_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {  // Reduced timeout for faster pause
        // Only allow pause from stable PLAYING state
        if (audio_state == AUDIO_STATE_PLAYING && active_pipeline) {
            ESP_LOGI(TAG, "Pausing playback... (state: PLAYING → PAUSING)");

            // Transition to PAUSING state BEFORE sending pipeline command
            // This prevents resume from being accepted while buffers drain
            audio_state = AUDIO_STATE_PAUSING;

            // OPTIMIZED FAST PAUSE STRATEGY: Mute I2S, let A2DP drain naturally
            // Step 1: Mute I2S immediately, let A2DP buffers drain naturally
            //if (s3_active_sink == AUDIO_SINK_I2S) {
            //    //alc5616_codec_set_voice_mute(true);
            //    //codec_is_muted = true;
            //    ESP_LOGI(TAG, "I2S codec muted for pause");
            //} else if (s3_active_sink == AUDIO_SINK_A2DP) {
            //    // INSTANT A2DP MUTE: Stop media immediately for instant audio cutoff
            //    bt_a2dp_stop_media();
            //    ESP_LOGI(TAG, "A2DP media stopped immediately (prevents audio bleeding)");
            //}

            // Step 2: OPTIMIZED PAUSE - Pause only file reader (prevents underflow spam)
            // This stops data flow without causing A2DP underflow warnings
            //if (fatfs_reader) {
            //    audio_element_pause(fatfs_reader);
            //    ESP_LOGI(TAG, "File reader paused (position preserved - prevents underflow)");
            //    
            //    // CRITICAL: Wait for A2DP to acknowledge pause to prevent race condition
            //    // Without this delay, rapid pause→resume causes delayed pause command to arrive after resume
            //    if (s3_active_sink == AUDIO_SINK_A2DP) {
            //        vTaskDelay(pdMS_TO_TICKS(250));  // Allow A2DP pause command to propagate through BT stack
            //        ESP_LOGI(TAG, "A2DP pause synchronization delay completed");
            //    }
            //}
            // Note: Don't pause MP3 decoder or stop pipeline - causes A2DP underflow spam

            audio_pipeline_pause(active_pipeline);


            // Record pause start time for tracking (#15141)
            pause_playback_tracking();

            // Transition to stable PAUSED state immediately
            audio_state = AUDIO_STATE_PAUSED;

            ESP_LOGI(TAG, "Playback paused (state: PAUSING → PAUSED)");
        } else if (audio_state == AUDIO_STATE_PAUSING) {
            ESP_LOGW(TAG, "Pause already in progress, ignoring");
        } else if (audio_state == AUDIO_STATE_PAUSED) {
            ESP_LOGW(TAG, "Audio is already paused");
        } else if (audio_state == AUDIO_STATE_RESUMING) {
            ESP_LOGW(TAG, "Resume in progress, cannot pause yet");
        } else {
            ESP_LOGW(TAG, "No audio currently playing to pause (state: %d)", audio_state);
        }
        xSemaphoreGive(audio_mutex);
    } else {
        ESP_LOGE(TAG, "Failed to acquire mutex in play_pause.");
    }
}

/**
 * @brief Resume the currently paused audio (with state machine protection)
 */
void play_resume(void) {
	ESP_LOGI(TAG, "play_resume()");

	if (xSemaphoreTake(audio_mutex, pdMS_TO_TICKS(1000)) == pdTRUE) {
		// Only allow resume from stable PAUSED state
		if (audio_state == AUDIO_STATE_PAUSED && active_pipeline) {
			ESP_LOGI(TAG, "Resuming playback... (state: PAUSED → RESUMING)");

			// Transition to RESUMING state BEFORE sending pipeline command
			// This prevents pause from being accepted while pipeline restarts
			audio_state = AUDIO_STATE_RESUMING;

			// For A2DP, check if still connected before resuming
			if (s3_active_sink == AUDIO_SINK_A2DP) {
				if (!bt_is_a2dp_connected()) {
					ESP_LOGW(TAG, "A2DP disconnected during pause, cannot resume BT stream - stopping playback");
					stop_active_pipeline_internal();
					audio_state = AUDIO_STATE_STOPPED;
					xSemaphoreGive(audio_mutex);
					return;
				}
			}

			// OPTIMIZED FAST RESUME STRATEGY: Resume file reader with minimal buffer wait
			// Step 1: Resume file reader with immediate response (no buffer pre-fill wait)
			//if (fatfs_reader) {
			//	audio_element_resume(fatfs_reader, 0.0f, pdMS_TO_TICKS(100));
			//	ESP_LOGI(TAG, "File reader resumed (FAST resume - position preserved)");
			//}

            audio_pipeline_resume(active_pipeline);


			// Step 2: Unmute I2S output, let A2DP resume naturally
			if (s3_active_sink == AUDIO_SINK_I2S) {
				alc5616_codec_set_voice_mute(false);
				codec_is_muted = false;
				ESP_LOGI(TAG, "I2S codec unmuted after buffer pre-fill");
			} else if (s3_active_sink == AUDIO_SINK_A2DP) {
				// RESTART A2DP: Since we stopped it on pause, restart it on resume
				bt_a2dp_start_media();
				ESP_LOGI(TAG, "A2DP media restarted (matches immediate stop on pause)");
			}

			// Accumulate pause duration for tracking (#15141)
			resume_playback_tracking();

			// Ensure dimmer is off when playback resumes
			stop_dimmer();

			// Transition to stable PLAYING state
			audio_state = AUDIO_STATE_PLAYING;

			ESP_LOGI(TAG, "Playback resumed (state: RESUMING → PLAYING)");
		} else if (audio_state == AUDIO_STATE_RESUMING) {
			ESP_LOGW(TAG, "Resume already in progress, ignoring");
		} else if (audio_state == AUDIO_STATE_PLAYING) {
			ESP_LOGW(TAG, "Audio is already playing");
		} else if (audio_state == AUDIO_STATE_PAUSING) {
			ESP_LOGW(TAG, "Pause in progress, cannot resume yet");
		} else {
			ESP_LOGW(TAG, "No paused audio to resume (state: %d)", audio_state);
		}
		xSemaphoreGive(audio_mutex);
	} else {
		ESP_LOGE(TAG, "Failed to acquire mutex in play_resume.");
	}
}

/**
 * @brief Check if audio is currently paused
 * @return true if audio is paused AND there's an active pipeline, false otherwise
 * @note This function ensures we're in a valid playback context (not just paused state without audio)
 */
bool is_audio_paused(void) {
    return is_state_paused() && active_pipeline != NULL;
}

/**
 * @brief Check if audio is currently stopped
 * @return true if audio is stopped, false otherwise
 * @note This function ensures we're in a valid playback context (not just stopped state without audio)
 */
bool is_audio_stopped(void) {
    return is_state_stopped();
}

/**
 * @brief Get current hardware volume level (1-6)
 * @return Current volume level (1-6), or -1 if audio system not ready
 */
int get_current_volume_level(void)
{
    ESP_LOGI(TAG, "get_current_volume_level()");
    
    if (!board_handle || !board_handle->audio_hal) {
        ESP_LOGW(TAG, "Audio system not ready for volume query");
        return -1;
    }
    
    audio_hal_volume_level_t hw_level = audio_hal_volume_get_level(board_handle->audio_hal);
    
    ESP_LOGI(TAG, "Hardware volume level: %d", hw_level);
    return (int)hw_level; // Direct mapping since both use 1-5
}

/**
 * @brief Set hardware volume level directly (1-6)
 * @param level Target volume level (1-6)
 * @return true if successful, false if audio system not ready or invalid level
 */
bool set_volume_level(int level)
{
    ESP_LOGI(TAG, "set_volume_level(%d)", level);

    if (level < 1 || level > 6) {
        ESP_LOGW(TAG, "Invalid volume level: %d (must be 1-6)", level);
        return false;
    }

    if (!board_handle || !board_handle->audio_hal) {
        ESP_LOGW(TAG, "Audio system not ready for volume control");
        return false;
    }

    audio_hal_volume_set_level(board_handle->audio_hal, level);
    s3_volume_level = level;  // Keep UI in sync
    ESP_LOGI(TAG, "Volume level set to: %d", level);

    return true;
}

/**
 * @brief Sync UI volume level with hardware volume level
 * Call this at boot to ensure UI shows correct volume
 */
void sync_volume_with_hardware(void)
{
    ESP_LOGI(TAG, "sync_volume_with_hardware()");

    int hw_volume = get_current_volume_level();
    if (hw_volume > 0) {
        // Update the global UI volume variable (declared in s3_definitions.h)
        s3_volume_level = hw_volume;
        ESP_LOGI(TAG, "Volume synced: s3_volume_level = %d", s3_volume_level);
    } else {
        ESP_LOGW(TAG, "Could not sync volume - hardware not ready");
    }
}

/**
 * @brief Increase volume by one level (1-6)
 */
void increase_volume(void)
{
    ESP_LOGI(TAG, "increase_volume()");

    if (board_handle && board_handle->audio_hal) {
        esp_err_t ret = audio_hal_volume_increase(board_handle->audio_hal);
        if (ret == ESP_OK) {
            audio_hal_volume_level_t level = audio_hal_volume_get_level(board_handle->audio_hal);
            ESP_LOGI(TAG, "Volume increased to level %d", level);
            
            // Sync the UI volume level with hardware volume
            s3_volume_level = (int)level;
            ESP_LOGI(TAG, "UI volume synced to %d", s3_volume_level);
        } else {
            ESP_LOGE(TAG, "Failed to increase volume: %s", esp_err_to_name(ret));
        }
    } else {
        ESP_LOGW(TAG, "Audio system not ready for volume control");
    }
}

/**
 * @brief Decrease volume by one level (1-6)
 */
void decrease_volume(void)
{
    ESP_LOGI(TAG, "decrease_volume()");

    if (board_handle && board_handle->audio_hal) {
        esp_err_t ret = audio_hal_volume_decrease(board_handle->audio_hal);
        if (ret == ESP_OK) {
            audio_hal_volume_level_t level = audio_hal_volume_get_level(board_handle->audio_hal);
            ESP_LOGI(TAG, "Volume decreased to level %d", level);
            
            // Sync the UI volume level with hardware volume
            s3_volume_level = (int)level;
            ESP_LOGI(TAG, "UI volume synced to %d", s3_volume_level);
        } else {
            ESP_LOGE(TAG, "Failed to decrease volume: %s", esp_err_to_name(ret));
        }
    } else {
        ESP_LOGW(TAG, "Audio system not ready for volume control");
    }
}

void volume_screen_enter(void)
{
    volume_backup_on_entry = s3_volume_level;
    ESP_LOGI(TAG, "volume_screen_enter() - backed up volume level: %d", volume_backup_on_entry);
}

void volume_confirm_and_save(void)
{
    ESP_LOGI(TAG, "volume_confirm_and_save() - flushing volume to NVS flash");
    s3_nvs_flush(); // Write HAL's dirty cache to NVS flash immediately
    volume_backup_on_entry = -1; // Clear backup after save
}

void volume_cancel_and_restore(void)
{
    ESP_LOGI(TAG, "volume_cancel_and_restore() - restoring backed up volume");

    if (volume_backup_on_entry > 0 && volume_backup_on_entry <= VOLUME_LEVEL_6) {
        // Restore hardware volume to backup value
        if (board_handle && board_handle->audio_hal) {
            s3_volume_level = volume_backup_on_entry;
            audio_hal_volume_set_level(board_handle->audio_hal, s3_volume_level);
            ESP_LOGI(TAG, "Volume restored to backed up level: %d", volume_backup_on_entry);
        } else {
            ESP_LOGW(TAG, "Audio system not ready, volume not restored");
        }
    } else {
        ESP_LOGW(TAG, "No valid backup volume to restore (backup=%d)", volume_backup_on_entry);
    }

    volume_backup_on_entry = -1; // Clear backup
}

/**
 * @brief Power on audio system
 */
void audio_power_on(void)
{
    ESP_LOGI(TAG, "power_on()");

    if (is_powered_on) {
        ESP_LOGW(TAG, "Audio system already powered on");
        return;
    }

    ESP_LOGI(TAG, "Powering on audio system...");
    board_handle = audio_board_init();

    if (board_handle->audio_hal == NULL) {
        ESP_LOGE(TAG, "Audio HAL not initialized");
        audio_board_deinit(board_handle);
        board_handle = NULL;
        return;
    }

    if (board_handle == NULL) {
        ESP_LOGE(TAG, "Failed to initialize audio board");
        return;
    }

    esp_err_t ret = audio_hal_ctrl_codec(board_handle->audio_hal,
                                      AUDIO_HAL_CODEC_MODE_DECODE,
                                      AUDIO_HAL_CTRL_START);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start codec: %s", esp_err_to_name(ret));
        audio_board_deinit(board_handle);
        board_handle = NULL;
        return;
    }

    ret = audio_hal_volume_init_from_nvs(board_handle->audio_hal);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize volume system: %s", esp_err_to_name(ret));
    }

    is_powered_on = true;
    ESP_LOGI(TAG, "Audio system powered on successfully");
}

/**
 * @brief Power off audio system
 */
void audio_power_off(void)
{
    ESP_LOGI(TAG, "shutdown()");

    if (!is_powered_on) {
        ESP_LOGW(TAG, "Audio system already powered off");
        return;
    }

    ESP_LOGI(TAG, "Powering off audio system");
    stop_active_pipeline();

    // UNUSED: Clean up event listener - can be removed since evt is not used
    /*
    if (evt) {
        audio_event_iface_destroy(evt);
        evt = NULL;
    }
    */

    if (board_handle) {
        audio_hal_ctrl_codec(board_handle->audio_hal,
                           AUDIO_HAL_CODEC_MODE_DECODE,
                           AUDIO_HAL_CTRL_STOP);
        audio_free(board_handle->audio_hal);
        board_sd_power(false);
        tca8418e_nfc_irq_mode(0);
        board_handle = NULL;
    }

    is_powered_on = false;
}

void play_music_task(void *pvParameters)
{
    ESP_LOGI(TAG, "play_music_task(param=%p)", pvParameters);

    char* sku_code = (char*)pvParameters;
    if (sku_code != NULL) {
        // This function is deprecated - NFC handling is now done directly in nfc_worker_task
        ESP_LOGW(TAG, "play_music_task is deprecated and should not be called");
        free(sku_code); // Free the allocated parameter
    } else {
        ESP_LOGE(TAG, "play_music_task: No SKU provided - cannot play album");
    }
    vTaskDelete(NULL);
}

/**
 * @brief Internal: Create simple shuffle for current album (assumes track_mutex held)
 */
static void create_current_shuffle_internal_nolock(void) {
    if (s3_current_size_track == 0) {
        ESP_LOGW(TAG, "No tracks to shuffle");
        return;
    }
    
    // Free existing shuffle
    if (current_shuffle_order) {
        free(current_shuffle_order);
    }
    
    // Allocate new shuffle array
    current_shuffle_order = (size_t*)malloc(s3_current_size_track * sizeof(size_t));
    if (!current_shuffle_order) {
        ESP_LOGE(TAG, "Failed to allocate shuffle array");
        return;
    }
    
    // Initialize with sequential indices (0, 1, 2, ...)
    for (size_t i = 0; i < s3_current_size_track; i++) {
        current_shuffle_order[i] = i;
    }
    
    // Simple Fisher-Yates shuffle
    uint32_t seed = (uint32_t)esp_log_timestamp();
    srand(seed);
    ESP_LOGI(TAG, "Creating shuffle with seed: %lu for %u tracks", seed, (unsigned int)s3_current_size_track);
    
    for (size_t i = s3_current_size_track - 1; i > 0; i--) {
        size_t j = rand() % (i + 1);
        size_t temp = current_shuffle_order[i];
        current_shuffle_order[i] = current_shuffle_order[j];
        current_shuffle_order[j] = temp;
    }
    
    shuffle_count = s3_current_size_track;
    shuffle_position = 0;
    
    ESP_LOGI(TAG, "Shuffle created: [0]=%u [1]=%u [2]=%u",
             (unsigned int)current_shuffle_order[0],
             (unsigned int)(shuffle_count > 1 ? current_shuffle_order[1] : 999),
             (unsigned int)(shuffle_count > 2 ? current_shuffle_order[2] : 999));
}

/**
 * @brief Public: Create simple shuffle for current album (takes track_mutex)
 */
static void create_current_shuffle(void) {
    if (xSemaphoreTake(track_mutex, pdMS_TO_TICKS(500)) != pdTRUE) {
        ESP_LOGE(TAG, "Failed to acquire track_mutex in create_current_shuffle");
        return;
    }

    create_current_shuffle_internal_nolock();

    xSemaphoreGive(track_mutex);
}


/**
 * @brief Check if a file is a real MP3 by reading its header
 * @param filepath Full path to the file to check
 * @return true if file has valid MP3 header, false otherwise
 */
bool is_real_mp3_file(const char *filepath) {
    FILE *file = fopen(filepath, "rb");
    if (!file) {
        return false;
    }
    
    // Read first 512 bytes to analyze file format
    unsigned char buffer[512];
    size_t read_bytes = fread(buffer, 1, sizeof(buffer), file);
    fclose(file);
    
    if (read_bytes < 8) {
        return false;
    }
    
    // First, explicitly check for MP4/AAC signatures (these should be rejected)
    // MP4/AAC files typically start with "ftyp" at offset 4
    if (buffer[4] == 'f' && buffer[5] == 't' && buffer[6] == 'y' && buffer[7] == 'p') {
        return false;
    }
    
    // Check for other MP4 container signatures
    if ((buffer[4] == 'm' && buffer[5] == 'o' && buffer[6] == 'o' && buffer[7] == 'v') ||
        (buffer[4] == 'm' && buffer[5] == 'd' && buffer[6] == 'a' && buffer[7] == 't')) {
        return false;
    }
    
    // Check for ADTS AAC signature (0xFF 0xFx where x >= 9)
    if (buffer[0] == 0xFF && (buffer[1] & 0xF6) == 0xF0) {
        return false; // This is likely AAC, not MP3
    }
    
    // Now check for valid MP3 signatures
    
    // Check for ID3v2 tag at beginning ("ID3")
    if (read_bytes >= 3 && buffer[0] == 'I' && buffer[1] == 'D' && buffer[2] == '3') {
        return true; // ID3v2 tag indicates MP3 format
    }
    
    // Search for valid MP3 sync word with strict validation
    for (size_t i = 0; i < read_bytes - 3; i++) {
        if (buffer[i] == 0xFF && (buffer[i+1] & 0xE0) == 0xE0) {
            // Strict validation of MP3 frame header
            unsigned char b1 = buffer[i+1];
            unsigned char b2 = buffer[i+2];
            
            // Check MPEG version (bits 4-3 in second byte)
            unsigned char version = (b1 >> 3) & 0x03;
            if (version == 0x01) continue; // Reserved
            
            // Check layer (bits 2-1 in second byte)
            unsigned char layer = (b1 >> 1) & 0x03;
            if (layer == 0x00) continue; // Reserved
            
            // Check bitrate (bits 7-4 in third byte)
            unsigned char bitrate = (b2 >> 4) & 0x0F;
            if (bitrate == 0x00 || bitrate == 0x0F) continue; // Free/Invalid bitrate
            
            // Check sampling frequency (bits 3-2 in third byte)
            unsigned char sampling = (b2 >> 2) & 0x03;
            if (sampling == 0x03) continue; // Reserved
            
            return true; // Valid MP3 frame found
        }
    }
    
    return false; // No valid MP3 signatures found
}

/**
 * @brief Internal: Scan directory for MP3 files and build playlist (assumes track_mutex held)
 */
static void build_playlist_internal_nolock()
{
    // CRITICAL: Log which album we're building playlist for
    if (!s3_current_album) {
        ESP_LOGE(TAG, "build_playlist() called with NULL s3_current_album!");
        return;
    }

    ESP_LOGI(TAG, "build_playlist() for album: SKU=%s, path=%s",
             s3_current_album->sku ? s3_current_album->sku : "NULL",
             s3_current_album->path ? s3_current_album->path : "NULL");

    DIR *dir = NULL;
    struct dirent* entry;
    bool is_skurc_album = false;

    // Free previous list if exists
    if (s3_current_track_list != NULL) {
        for (int i = 0; i < s3_current_size_track; i++) {
            free(s3_current_track_list[i]);
        }
        free(s3_current_track_list);
        s3_current_track_list = NULL;
        s3_current_size_track = 0;
    }

    // Check if this is a SKURC album
    if (s3_current_album && s3_current_album->sku &&
        strncmp(s3_current_album->sku, "SKURC-", 6) == 0) {
        is_skurc_album = true;
        ESP_LOGI(TAG, "Building playlist for SKURC album: %s", s3_current_album->sku);
    }

    if (is_skurc_album) {
        // For SKURC albums, get filenames from account data instead of scanning directory
        int account_filename_count = 0;
        char **account_filenames = get_skurc_filenames_from_account(s3_current_album->sku, &account_filename_count);
        
        if (account_filenames && account_filename_count > 0) {
            ESP_LOGI(TAG, "Using account data for SKURC playlist: %d files listed", account_filename_count);
            
            // Count valid files that exist on disk and are real MP3s
            for (int i = 0; i < account_filename_count; i++) {
                char full_path[256];
                snprintf(full_path, sizeof(full_path), "%s%s", s3_current_album->path, account_filenames[i]);
                
                // Check if file exists and is a real MP3
                if (access(full_path, F_OK) == 0 && is_real_mp3_file(full_path)) {
                    s3_current_size_track++;
                }
            }
            
            ESP_LOGI(TAG, "SKURC account-based scan: %d valid files found", s3_current_size_track);
        } else {
            ESP_LOGW(TAG, "Could not get SKURC filenames from account, falling back to directory scan");
            // Fall back to directory scan
            is_skurc_album = false;
        }
        
        // Cleanup account filenames (will re-read them in second pass)
        if (account_filenames) {
            for (int i = 0; i < account_filename_count; i++) {
                free(account_filenames[i]);
            }
            free(account_filenames);
        }
    }

    if (!is_skurc_album) {
        // Regular album: scan directory
        ESP_LOGI(TAG, "Opening directory for playlist: %s", s3_current_album->path);
        dir = opendir(s3_current_album->path);
        if (dir == NULL) {
            ESP_LOGE(TAG, "Failed to open directory: %s", s3_current_album->path);
            return;
        }

        // First pass: count MP3 files
        while ((entry = readdir(dir)) != NULL) {
            if (entry->d_type == DT_REG && strstr(entry->d_name, ".mp3")) {
                s3_current_size_track++;
            }
        }
        rewinddir(dir);
    }

    if (s3_current_size_track == 0) {
        ESP_LOGW(TAG, "No MP3 files found in directory");
        closedir(dir);
        return;
    }

    // Allocate memory for track list
    s3_current_track_list = (char**)malloc(s3_current_size_track * sizeof(char*));
    if (s3_current_track_list == NULL) {
        ESP_LOGE(TAG, "Memory allocation failed");
        closedir(dir);
        s3_current_size_track = 0;
        return;
    }

    // Second pass: store filenames
    int index = 0;
    
    if (is_skurc_album) {
        // For SKURC albums, use account data to build playlist
        int account_filename_count = 0;
        char **account_filenames = get_skurc_filenames_from_account(s3_current_album->sku, &account_filename_count);
        
        if (account_filenames && account_filename_count > 0) {
            ESP_LOGI(TAG, "Building SKURC playlist from account data");
            
            for (int i = 0; i < account_filename_count && index < s3_current_size_track; i++) {
                char full_path[256];
                snprintf(full_path, sizeof(full_path), "%s%s", s3_current_album->path, account_filenames[i]);
                
                // Only include files that exist and are valid MP3s
                if (access(full_path, F_OK) == 0 && is_real_mp3_file(full_path)) {
                    s3_current_track_list[index] = strdup_spiram(full_path);
                    if (s3_current_track_list[index] != NULL) {
                        ESP_LOGI(TAG, "Added SKURC track: %s", account_filenames[i]);
                        index++;
                    } else {
                        ESP_LOGE(TAG, "Failed to allocate memory for track path");
                    }
                }
            }
            
            // Cleanup account filenames
            for (int i = 0; i < account_filename_count; i++) {
                free(account_filenames[i]);
            }
            free(account_filenames);
        }
    } else {
        // Regular albums: read from directory
        while ((entry = readdir(dir)) != NULL && index < s3_current_size_track) {
            if (entry->d_type == DT_REG && strstr(entry->d_name, ".mp3")) {
                char full_path[256];
                snprintf(full_path, sizeof(full_path), "%s%s", s3_current_album->path, entry->d_name);

                s3_current_track_list[index] = strdup_spiram(full_path);
                if (s3_current_track_list[index] == NULL) {
                    ESP_LOGE(TAG, "Failed to allocate memory for track path");
                    continue;
                }
                index++;
            }
        }
    }

    if (dir) {
        closedir(dir);
    }
    
    // Sort the track list alphabetically to ensure consistent ordering
    if (s3_current_size_track > 1) {
        // Use qsort with a comparison function for strings
        extern int track_name_compare(const void *a, const void *b);
        qsort(s3_current_track_list, s3_current_size_track, sizeof(char*), track_name_compare);
        ESP_LOGI(TAG, "Track list sorted alphabetically");
    }
    
    ESP_LOGI(TAG, "Playlist built with %d tracks", s3_current_size_track);

    // Track index is already set by switch_album_internal() before build_playlist() is called
    // No need to call get_current_track_index() here
}

/**
 * @brief Public: Scan directory for MP3 files and build playlist (takes track_mutex)
 */
void build_playlist(void)
{
    if (xSemaphoreTake(track_mutex, pdMS_TO_TICKS(2000)) != pdTRUE) {
        ESP_LOGE(TAG, "Failed to acquire track_mutex in build_playlist");
        return;
    }

    build_playlist_internal_nolock();

    xSemaphoreGive(track_mutex);
}

/**
 * @brief Internal: Build playlist with tracks from multiple SKUs (assumes track_mutex held)
 * Used when LANGUAGE_ALL is selected for NFC content
 */
static void build_playlist_all_languages_internal_nolock(char** skus, int sku_count)
{
    ESP_LOGI(TAG, "build_playlist_all_languages() for %d SKUs", sku_count);

    // Free previous list if exists
    if (s3_current_track_list != NULL) {
        for (int i = 0; i < s3_current_size_track; i++) {
            free(s3_current_track_list[i]);
        }
        free(s3_current_track_list);
        s3_current_track_list = NULL;
        s3_current_size_track = 0;
    }

    // Find all albums for all provided SKUs
    s3_album_handler_t* albums[20] = {0}; // Support up to 20 albums total
    int albums_count = 0;
    size_t dynamic_count = get_dynamic_albums_size();

    // Collect all matching albums from all SKUs
    for (int sku_idx = 0; sku_idx < sku_count; sku_idx++) {
        const char* sku = skus[sku_idx];
        ESP_LOGI(TAG, "Processing SKU: %s", sku);

        for (size_t i = 0; i < dynamic_count && albums_count < 20; i++) {
            s3_album_handler_t *album = get_dynamic_album_by_index(i);
            if (album && album->sku && strcmp(album->sku, sku) == 0 &&
                album->is_downloaded && album->is_available_nfc) {

                albums[albums_count] = album;
                albums_count++;
                ESP_LOGI(TAG, "Found album: %s (language: %d, path: %s)", album->name, album->language, album->path);
            }
        }
    }

    // Count total MP3 files from all albums
    int total_tracks = 0;
    for (int album_idx = 0; album_idx < albums_count; album_idx++) {
        s3_album_handler_t* album = albums[album_idx];
        if (album && album->path) {
            DIR* dir = opendir(album->path);
            if (dir) {
                struct dirent* entry;
                while ((entry = readdir(dir)) != NULL) {
                    if (entry->d_type == DT_REG && strstr(entry->d_name, ".mp3")) {
                        total_tracks++;
                    }
                }
                closedir(dir);
            }
        }
    }

    if (total_tracks == 0) {
        ESP_LOGW(TAG, "No MP3 files found in any of the %d SKUs", sku_count);
        return;
    }

    // Allocate memory for combined track list
    s3_current_track_list = (char**)malloc(total_tracks * sizeof(char*));
    if (s3_current_track_list == NULL) {
        ESP_LOGE(TAG, "Memory allocation failed for combined playlist");
        return;
    }

    // Collect all tracks from all albums
    int track_index = 0;
    for (int album_idx = 0; album_idx < albums_count; album_idx++) {
        s3_album_handler_t* album = albums[album_idx];
        if (album && album->path) {
            DIR* dir = opendir(album->path);
            if (dir) {
                struct dirent* entry;
                while ((entry = readdir(dir)) != NULL && track_index < total_tracks) {
                    if (entry->d_type == DT_REG && strstr(entry->d_name, ".mp3")) {
                        char full_path[256];
                        snprintf(full_path, sizeof(full_path), "%s%s", album->path, entry->d_name);

                        s3_current_track_list[track_index] = strdup_spiram(full_path);
                        if (s3_current_track_list[track_index] == NULL) {
                            ESP_LOGE(TAG, "Failed to allocate memory for track path");
                            continue;
                        }
                        track_index++;
                    }
                }
                closedir(dir);
            }
        }
    }

    s3_current_size_track = track_index;

    // Sort the combined track list alphabetically
    if (s3_current_size_track > 1) {
        extern int track_name_compare(const void *a, const void *b);
        qsort(s3_current_track_list, s3_current_size_track, sizeof(char*), track_name_compare);
        ESP_LOGI(TAG, "Combined track list sorted alphabetically");
    }

    ESP_LOGI(TAG, "Combined playlist built with %d tracks from %d albums (%d SKUs)", s3_current_size_track, albums_count, sku_count);
}

/**
 * @brief Public: Build playlist with tracks from multiple SKUs (takes track_mutex)
 */
void build_playlist_all_languages(char** skus, int sku_count)
{
    if (xSemaphoreTake(track_mutex, pdMS_TO_TICKS(2000)) != pdTRUE) {
        ESP_LOGE(TAG, "Failed to acquire track_mutex in build_playlist_all_languages");
        return;
    }

    build_playlist_all_languages_internal_nolock(skus, sku_count);

    xSemaphoreGive(track_mutex);
}

/**
 * @brief Simple comparison function for sorting track file paths
 */
int track_name_compare(const void *a, const void *b) {
    return strcmp(*(const char**)a, *(const char**)b);
}

/**
 * @brief Play current track in playlist with gapless transition support
 */
void audio_start_playing()
{
    audio_start_playing_with_transition(false);
}

/**
 * @brief Play current track in playlist - enhanced version with transition control
 * @param is_track_transition true if this is a track change (enables gapless transition), false for new playback
 */
void audio_start_playing_with_transition(bool is_track_transition)
{
    ESP_LOGI(TAG, "audio_start_playing_with_transition(transition=%s)", is_track_transition ? "true" : "false");

    // Reset suppression so natural completions can auto-advance
    suppress_auto_play_once = false;

    // CRITICAL: Verify that playlist matches current album before using it
    // If s3_current_album changed, we need to rebuild the playlist
    // Quick check without lock (race condition is benign - worst case we build twice)
    bool need_rebuild = false;
    if (s3_current_track_list == NULL) {
        ESP_LOGW(TAG, "Track list not ready - building playlist");
        need_rebuild = true;
    } else if (s3_current_album && s3_current_album->path) {
        // Check if first track in playlist matches current album path
        // This is a quick sanity check - if path doesn't match, playlist is stale
        if (s3_current_size_track > 0 && s3_current_track_list[0]) {
            if (strstr(s3_current_track_list[0], s3_current_album->path) == NULL) {
                ESP_LOGW(TAG, "Playlist path mismatch - rebuilding for current album %s",
                         s3_current_album->sku ? s3_current_album->sku : "unknown");
                need_rebuild = true;
            }
        }
    }

    if (need_rebuild) {
        build_playlist();  // This will take track_mutex internally
    }

    // LOCK ORDERING: audio_mutex FIRST, then track_mutex
    // Take audio_mutex first to check pipeline state and prevent conflicts
    if (xSemaphoreTake(audio_mutex, pdMS_TO_TICKS(2000)) != pdTRUE) {
        ESP_LOGE(TAG, "Failed to acquire audio_mutex in audio_start_playing_with_transition (timeout 2s)");
        return;
    }

    // Now take track_mutex to safely access track list
    // Use longer timeout in case build_playlist() is running on slow SD card
    if (xSemaphoreTake(track_mutex, pdMS_TO_TICKS(3000)) != pdTRUE) {
        ESP_LOGE(TAG, "Failed to acquire track_mutex in audio_start_playing_with_transition (timeout 3s)");
        xSemaphoreGive(audio_mutex);
        return;
    }

    // Double-check playlist is ready (could have been cleared by another thread)
    if (s3_current_track_list == NULL) {
        ESP_LOGE(TAG, "Track list not ready after build attempt - cannot play");
        xSemaphoreGive(track_mutex);
        xSemaphoreGive(audio_mutex);
        return;
    }

    if (s3_current_size_track == 0) {
        ESP_LOGE(TAG, "No tracks available in playlist");
        xSemaphoreGive(track_mutex);
        xSemaphoreGive(audio_mutex);
        return;
    }

    // Create shuffle if we're in shuffle mode and don't have one yet
    if (s3_playback_mode == PLAYBACK_MODE_SHUFFLE && !current_shuffle_order) {
        ESP_LOGI(TAG, "Creating shuffle for playback");
        create_current_shuffle_internal_nolock();  // We hold track_mutex

        // Set track index to first shuffled track
        if (current_shuffle_order && shuffle_count > 0) {
            s3_current_idx_track = current_shuffle_order[0];
            shuffle_position = 0;
            ESP_LOGI(TAG, "Shuffle created - starting with track %u", (unsigned int)(s3_current_idx_track + 1));
        }
    }

    /* make sure the index we are about to use is inside range */
    if (s3_current_idx_track >= s3_current_size_track)
        s3_current_idx_track = 0;

    // Copy track path while holding track_mutex (prevent use-after-free)
    if (s3_current_idx_track >= s3_current_size_track) {
        ESP_LOGE(TAG, "Track index %u out of range (0-%u)!",
                 (unsigned int)s3_current_idx_track, (unsigned int)s3_current_size_track);
        xSemaphoreGive(track_mutex);
        xSemaphoreGive(audio_mutex);
        return;
    }

    char *next_track = strdup(s3_current_track_list[s3_current_idx_track]);
    if (!next_track) {
        ESP_LOGE(TAG, "Failed to allocate memory for track path");
        xSemaphoreGive(track_mutex);
        xSemaphoreGive(audio_mutex);
        return;
    }

    // CRITICAL: Log which album this track belongs to
    ESP_LOGI(TAG, "Playing track idx %d / %d from album %s: %s",
             s3_current_idx_track + 1, s3_current_size_track,
             s3_current_album && s3_current_album->sku ? s3_current_album->sku : "unknown",
             next_track);

    // Release track_mutex early (we have the path copied)
    xSemaphoreGive(track_mutex);

    // Now continue with audio operations (still holding audio_mutex)
    if (is_track_transition && active_pipeline && is_state_playing()) {
        // Gapless transition: reuse existing pipeline instead of stopping/starting
        ESP_LOGI(TAG, "Gapless track transition: switching to new track without pipeline restart");

        // For A2DP, restart media stream
        if (s3_active_sink == AUDIO_SINK_A2DP) {
            bt_a2dp_stop_media();
        }

        // Reset pipeline elements but keep pipeline running
        audio_pipeline_stop(active_pipeline);
        audio_pipeline_wait_for_stop(active_pipeline);
        audio_pipeline_reset_ringbuffer(active_pipeline);
        audio_pipeline_reset_elements(active_pipeline);

        // Set new track and restart pipeline
        audio_element_set_uri(fatfs_reader, next_track);
        if (audio_pipeline_run(active_pipeline) == ESP_OK) {
            ESP_LOGI(TAG, "Gapless transition successful");

            // For A2DP, restart media stream
            if (s3_active_sink == AUDIO_SINK_A2DP) {
                bt_a2dp_start_media();
            }

            // Update tracking
            start_playback_tracking(next_track);

            free(next_track);  // Free the copied path
            xSemaphoreGive(audio_mutex);
            return;
        } else {
            ESP_LOGW(TAG, "Gapless transition failed - falling back to normal transition");
        }
    }

    // Release audio_mutex BEFORE calling play_auto_mode to prevent nested lock
    xSemaphoreGive(audio_mutex);

    // Normal transition or fallback: full pipeline restart
    // play_auto_mode will internally take audio_mutex if needed
    play_auto_mode(next_track);

    // Cleanup
    free(next_track);
}


// Set playback mode
void set_playback_mode(playback_mode_t mode) {
    ESP_LOGI(TAG, "set_playback_mode(%d) - mode definition only", mode);
    s3_playback_mode = mode;
    
    // Note: Actual shuffle initialization happens later in switch_album_internal() 
    // when s3_current_album is properly set and SD card content is available
    if (mode == PLAYBACK_MODE_SHUFFLE) {
        ESP_LOGI(TAG, "Shuffle mode defined - initialization will happen when album is loaded");
    } else {
        ESP_LOGI(TAG, "Sequential mode set");
    }
}

// Get playback mode
playback_mode_t get_playback_mode(void) {
    return s3_playback_mode;
}

// Set auto-play mode
void set_auto_play_mode(auto_play_mode_t mode) {
    s3_auto_play_mode = mode;
}

// Get auto-play mode
auto_play_mode_t get_auto_play_mode(void) {
    return s3_auto_play_mode;
}

// Helper function to detect language from track path
static int detect_track_language(const char* track_path) {
    if (!track_path) {
        return LANGUAGE_CHINESE;  // Default fallback
    }

    // Check for English markers (case-insensitive)
    if (strstr(track_path, "-EN") || strstr(track_path, "_EN") ||
        strstr(track_path, "-en") || strstr(track_path, "_en")) {
        return LANGUAGE_ENGLISH;
    }

    // Check for Chinese markers (case-insensitive, support both ZH and CH)
    if (strstr(track_path, "-ZH") || strstr(track_path, "_ZH") ||
        strstr(track_path, "-zh") || strstr(track_path, "_zh") ||
        strstr(track_path, "-CH") || strstr(track_path, "_CH") ||
        strstr(track_path, "-ch") || strstr(track_path, "_ch")) {
        return LANGUAGE_CHINESE;
    }

    // Default to Chinese if no marker found
    return LANGUAGE_CHINESE;
}

// Get current track position for UI display
size_t get_current_track_display_position(void) {
    // Take track_mutex to safely access track list
    if (xSemaphoreTake(track_mutex, pdMS_TO_TICKS(100)) != pdTRUE) {
        ESP_LOGW(TAG, "Failed to acquire track_mutex in get_current_track_display_position, returning default");
        return 1;  // Return safe default
    }

    size_t display_pos = s3_current_idx_track + 1;

    // Check if user selected LANGUAGE_ALL (system-wide setting)
    // When LANGUAGE_ALL is selected, the playlist combines multiple language versions
    // but s3_current_album still points to a single-language album object
    extern int s3_selected_language;  // Declared in s3_definitions.c

    if (s3_selected_language == LANGUAGE_ALL &&
        s3_current_track_list && s3_current_size_track > 0) {
        // LANGUAGE_ALL mode: count tracks within the same language group

        // Detect current track's language
        const char* current_track_path = s3_current_track_list[s3_current_idx_track];
        int current_lang = detect_track_language(current_track_path);

        // Count tracks before current one that have the same language
        size_t same_lang_position = 1;  // Start from 1 (1-based counting)
        for (size_t i = 0; i < s3_current_idx_track; i++) {
            int track_lang = detect_track_language(s3_current_track_list[i]);
            if (track_lang == current_lang) {
                same_lang_position++;
            }
        }

        display_pos = same_lang_position;

        ESP_LOGI(TAG, "get_current_track_display_position: LANGUAGE_ALL mode - track %u/%u (language-grouped position %u, language=%s)",
                 (unsigned int)(s3_current_idx_track + 1),
                 (unsigned int)s3_current_size_track,
                 (unsigned int)display_pos,
                 current_lang == LANGUAGE_ENGLISH ? "EN" : "ZH");
    } else {
        // Single language mode - use simple position
        const char *mode_str = "UNKNOWN";
        if (s3_playback_mode == PLAYBACK_MODE_SHUFFLE) {
            mode_str = "SHUFFLE";
        } else if (s3_playback_mode == PLAYBACK_MODE_SEQUENTIAL) {
            mode_str = "SEQUENTIAL";
        }

        ESP_LOGI(TAG, "get_current_track_display_position: Single language - track %u (mode=%s)",
                 (unsigned int)display_pos, mode_str);
    }

    xSemaphoreGive(track_mutex);

    return display_pos;
}

/**
 * @brief Internal: Navigate to next/previous track (assumes track_mutex held)
 */
static void one_step_track_shuffle_internal_nolock(bool next) {
    ESP_LOGI(TAG, "one_step_track_shuffle(%s)", next ? "next" : "previous");

    // Critical: Playlist should already be built! If not, something is wrong
    if (s3_current_track_list == NULL || s3_current_size_track == 0) {
        ESP_LOGE(TAG, "Track list not ready - cannot navigate");
        return;
    }

    if (s3_playback_mode == PLAYBACK_MODE_SHUFFLE) {
        // Use simple shuffle navigation
        if (current_shuffle_order && shuffle_count > 0) {
            // Navigate through the shuffle order
            if (next) {
                shuffle_position = (shuffle_position + 1) % shuffle_count;
            } else {
                shuffle_position = (shuffle_position == 0) ? shuffle_count - 1 : shuffle_position - 1;
            }
            
            s3_current_idx_track = current_shuffle_order[shuffle_position];
            ESP_LOGI(TAG, "Shuffle: position %u/%u -> track %u",
                     (unsigned int)(shuffle_position + 1), (unsigned int)shuffle_count, (unsigned int)(s3_current_idx_track + 1));
        } else {
            ESP_LOGW(TAG, "Shuffle not initialized - falling back to sequential");
            // Fallback to sequential navigation
            if (next) {
                s3_current_idx_track = (s3_current_idx_track + 1) % s3_current_size_track;
            } else {
                s3_current_idx_track = (s3_current_idx_track == 0) ? s3_current_size_track - 1 : s3_current_idx_track - 1;
            }
        }
    } else {
        // Sequential mode
        if (next) {
            s3_current_idx_track = (s3_current_idx_track + 1) % s3_current_size_track;
        } else {
            s3_current_idx_track = (s3_current_idx_track == 0) ? s3_current_size_track - 1 : s3_current_idx_track - 1;
        }
        ESP_LOGI(TAG, "Sequential: track %u/%u", (unsigned int)(s3_current_idx_track + 1), (unsigned int)s3_current_size_track);
    }
}

/**
 * @brief Public: Navigate to next/previous track (takes track_mutex)
 */
void one_step_track_shuffle(bool next) {
    if (xSemaphoreTake(track_mutex, pdMS_TO_TICKS(500)) != pdTRUE) {
        ESP_LOGE(TAG, "Failed to acquire track_mutex in one_step_track_shuffle");
        return;
    }

    one_step_track_shuffle_internal_nolock(next);

    xSemaphoreGive(track_mutex);
}

void one_step_track(bool next)
{
    // Take track_mutex to safely access and modify track indices
    if (xSemaphoreTake(track_mutex, pdMS_TO_TICKS(500)) != pdTRUE) {
        ESP_LOGE(TAG, "Failed to acquire track_mutex in one_step_track");
        return;
    }

    // Check if we're at the end of playlist before stepping
    bool was_at_end = false;
    bool was_at_begin = false;
    bool is_special_album_transition = false;

    if (s3_playback_mode == PLAYBACK_MODE_SHUFFLE) {
        // In shuffle mode, check if we're at the end of the shuffle playlist
        if (current_shuffle_order && shuffle_count > 0) {
            was_at_end = (shuffle_position == shuffle_count - 1) && next;
        }
    } else {
        // In sequential mode, check if we're at the last track
        was_at_end = (s3_current_idx_track == s3_current_size_track - 1) && next;
        was_at_begin = (s3_current_idx_track == 0) && !next;
    }    

    if (s3_auto_play_mode != AUTO_PLAY_OFF && (was_at_end || was_at_begin)) {
        ESP_LOGI(TAG, "Album cycle finished - auto-play mode: %d", s3_auto_play_mode);

        if (s3_auto_play_mode == AUTO_PLAY_ALL) {
            ESP_LOGI(TAG, "Auto-playing next album...");

            // CRITICAL: Release track_mutex BEFORE calling album switch functions
            // to prevent deadlock (switch_album_internal will acquire it)
            xSemaphoreGive(track_mutex);

            if(next && was_at_end)
            {
                is_special_album_transition = true;
                if(s3_current_idx == (s3_current_size - 1))
                {
                    n_step_album(0);
                    s3_current_idx_track = 0;
                }
                else
                {
                    one_step_album(next); // Next album
                    s3_current_idx_track = 0;
                }
            }
            else if(!next && was_at_begin)
            {
                is_special_album_transition = true;
                if(s3_current_idx == 0)
                {
                    n_step_album(s3_current_size - 1);
                    s3_current_idx_track = (s3_current_size_track - 1);
                }
                else
                {
                    n_step_album(s3_current_idx - 1);
                    s3_current_idx_track = (s3_current_size_track - 1);
                }
            }

            // Automatically start playing the new album
            ESP_LOGI(TAG, "Starting playback of new track");

            // Album switch completed, mutex already released above
            // Do NOT continue to normal track stepping since we switched albums
            return;
        } else if (s3_auto_play_mode == AUTO_PLAY_FOLDER) {
#if 0
            ESP_LOGI(TAG, "Folder repeat - creating new shuffle cycle...");
            if (s3_playback_mode == PLAYBACK_MODE_SHUFFLE) {
                // Create new simple shuffle for folder repeat
                create_current_shuffle();
                if (current_shuffle_order && shuffle_count > 0) {
                    s3_current_idx_track = current_shuffle_order[0];
                    shuffle_position = 0;
                    ESP_LOGI(TAG, "New shuffle cycle started - playing track %u", (unsigned int)(s3_current_idx_track + 1));
                }
            }
            // In sequential mode, we already wrapped to track 0
#endif
        }
    }
    one_step_track_shuffle_internal_nolock(next);

    xSemaphoreGive(track_mutex);
}

// next track and prevoous track wappers
void audio_play_next_album_track(void)
{
    one_step_track(VALUE_UP);
}

void audio_play_previous_album_track(void)
{
    one_step_track(VALUE_DOWN);
}

void audio_update_album_data(void)
{
    s3_current_idx  = 0;
    s3_current_size  = s3_albums_get_size();

    // Get the first album in the available list
    s3_current_album = s3_albums_get(s3_current_idx);

    // Take track_mutex to safely clear track list
    if (xSemaphoreTake(track_mutex, pdMS_TO_TICKS(1000)) != pdTRUE) {
        ESP_LOGE(TAG, "Failed to acquire track_mutex in audio_update_album_data");
        return;
    }

    // Clear the track list cache since we changed album
    if (s3_current_track_list) {
        for (int i = 0; i < s3_current_size_track; i++) {
            free(s3_current_track_list[i]);
        }
        free(s3_current_track_list);
        s3_current_track_list = NULL;
        s3_current_size_track = 0;
        s3_current_idx_track = 0;
    }

    xSemaphoreGive(track_mutex);
}

static void switch_album_internal(size_t idx)
{
    if (idx >= s3_current_size) {
        ESP_LOGW(TAG, "Album index %u out of range (0-%u)",
                 (unsigned int)idx, (unsigned int)(s3_current_size - 1));
        return;
    }

    /* ---- mutex -------------------------------------------------------- */
    // LOCK ORDERING: audio_mutex FIRST, then track_mutex
    if (xSemaphoreTake(audio_mutex, pdMS_TO_TICKS(1500)) != pdTRUE) {
        ESP_LOGW(TAG, "Audio busy, cannot switch album");
        return;
    }

    /* ---- stop current playback --------------------------------------- */
    if (is_state_playing()) {
        stop_active_pipeline_internal();     /* no nested mutex take */
        audio_state = AUDIO_STATE_STOPPED;
    }

    /* ---- update global pointers -------------------------------------- */
    s3_current_idx   = idx;
    s3_current_album = s3_albums_get(s3_current_idx);

    if (!s3_current_album || !s3_current_album->path) {
        ESP_LOGE(TAG, "Selected album has NULL fields at index %u", (unsigned int)idx);
        xSemaphoreGive(audio_mutex);
        return;
    }
    
    // CRITICAL: Log album switch with full details
    ESP_LOGI(TAG, "Switching to album index %u: SKU=%s, path=%s",
             (unsigned int)idx,
             s3_current_album->sku ? s3_current_album->sku : "NULL",
             s3_current_album->path ? s3_current_album->path : "NULL");

    // Save album selection immediately - this persists it even if playback doesn't start
    if (s3_current_album->sku && strlen(s3_current_album->sku) > 0) {
        ESP_LOGI(TAG, "[LAST_ALBUM] Saving on switch: %s", s3_current_album->sku);
        s3_albums_save_last_played(s3_current_album->sku);
    }

    // Now take track_mutex (LOCK ORDERING: audio → track)
    // Use longer timeout because build_playlist() might be scanning slow SD card
    if (xSemaphoreTake(track_mutex, pdMS_TO_TICKS(3000)) != pdTRUE) {
        ESP_LOGE(TAG, "Failed to acquire track_mutex in switch_album_internal (timeout after 3s)");
        xSemaphoreGive(audio_mutex);
        return;
    }

    /* ---- flush old playlist cache ------------------------------------ */
    if (s3_current_track_list) {
        for (int i = 0; i < s3_current_size_track; ++i) free(s3_current_track_list[i]);
        free(s3_current_track_list);
        s3_current_track_list = NULL;
        s3_current_size_track = 0;
    }

    // Initialize track index - always start from track 0 initially
    s3_current_idx_track = 0;
    ESP_LOGI(TAG, "Album switched - initial track index set to 0");

    // Clean up any existing shuffle
    cleanup_simple_shuffle();

    // Build playlist immediately to have track count for shuffle
    build_playlist_internal_nolock();  // Call internal version (we hold track_mutex)

    // Create shuffle immediately if in shuffle mode
    if (s3_playback_mode == PLAYBACK_MODE_SHUFFLE && s3_current_size_track > 0) {
        ESP_LOGI(TAG, "Creating shuffle immediately after album switch");
        create_current_shuffle_internal_nolock();  // Call internal version (we hold track_mutex)

        // Set track index to first shuffled track
        if (current_shuffle_order && shuffle_count > 0) {
            s3_current_idx_track = current_shuffle_order[0];
            shuffle_position = 0;
            ESP_LOGI(TAG, "Shuffle created - track index set to %u (shuffle pos 0)", (unsigned int)(s3_current_idx_track + 1));
        }
    } else {
        ESP_LOGI(TAG, "Sequential mode - track index remains 0");
    }

    ESP_LOGI(TAG, "Album [%u/%u] → %s",
             (unsigned int)(idx + 1), (unsigned int)s3_current_size, s3_current_album->name);

    xSemaphoreGive(track_mutex);
    xSemaphoreGive(audio_mutex);

    // audio_start_playing();      /* builds playlist on demand */
}

void play_album()
{
    if (!is_powered_on) {
        ESP_LOGW(TAG, "Audio system not powered on, cannot play album");
        return;
    }

    // Check if we need to stop current playback
    if (is_state_playing()) {
        ESP_LOGW(TAG, "Audio is already playing, stopping current playback before album");
        play_stop();
        // Give more time for proper cleanup
        vTaskDelay(pdMS_TO_TICKS(500));
    }

    sys_memory_status(TAG, "Inside play_album()");

    if (s3_current_album->is_downloaded) {
        ESP_LOGI("ALBUM_CHECK", "Playing album: %s", s3_current_album->name);
        
        // Save last played album to SD card for persistence
        if (s3_current_album->sku && strlen(s3_current_album->sku) > 0) {
            ESP_LOGI(TAG, "[LAST_ALBUM] Saving on play: %s", s3_current_album->sku);
            s3_albums_save_last_played(s3_current_album->sku);
        }
        
        audio_start_playing();
    } else {
        ESP_LOGW("ALBUM_CHECK", "Album [%s] not available!", s3_current_album->name);
    }
}

void one_step_album(bool next)
{
    if (s3_current_size == 0) {
        ESP_LOGW(TAG, "No downloaded albums!");
        return;
    }

    size_t new_idx = next
                     ? (s3_current_idx + 1) % s3_current_size
                     : (s3_current_idx == 0
                        ? s3_current_size - 1
                        : s3_current_idx - 1);

    switch_album_internal(new_idx);
}

void n_step_album(size_t global_idx)
{
    ESP_LOGI(TAG, "n_step_album(global_idx=%u)", (unsigned int)global_idx);

    // Get the total number of available albums from album manager
    size_t album_count = s3_albums_get_size();
    if (global_idx >= album_count) {
        ESP_LOGE(TAG, "Global album index %u out of range (0-%u)!", (unsigned int)global_idx, (unsigned int)(album_count - 1));
        return;
    }

    // Get the target album directly from album manager
    const s3_album_handler_t *target_album = s3_albums_get(global_idx);
    if (!target_album) {
        ESP_LOGE(TAG, "Album at index [%u] not found!", (unsigned int)global_idx);
        return;
    }

    if (!target_album->is_downloaded) {
        ESP_LOGE(TAG, "Album [%u] '%s' is not downloaded!", (unsigned int)global_idx, target_album->name);
        return;
    }

    // Since we're using the album manager, the global_idx IS the correct index
    // No translation needed - just use it directly
    if (global_idx == s3_current_idx) {
        ESP_LOGI(TAG, "Album %s already current", s3_current_album->name);
        return;
    }

    ESP_LOGI(TAG, "Switching to album [%u/%u]: %s", (unsigned int)(global_idx + 1), (unsigned int)album_count, target_album->name);
    switch_album_internal(global_idx);
}

void update_alarm(s3_alarms_t alarm_id)
{
    s3_current_alarm = NULL;

    if (alarm_id >= 0 && alarm_id <= ALARMS_QTD) {
        s3_current_alarm = &s3_alarms[alarm_id];
        ESP_LOGI("ALARM_UPDATE", "Alarm updated: %s", s3_current_alarm->name);
        return;
    }

    ESP_LOGW("ALARM_UPDATE", "Alarm with ID %d not found", alarm_id);
}

bool audio_player_is_running(void)
{
    ESP_LOGI(TAG, "audio_player_is_running()");
    if (active_pipeline == NULL || mp3_decoder == NULL) {
        return false;
    }
    audio_element_state_t state = audio_element_get_state(mp3_decoder);
    return (state == AEL_STATE_RUNNING);
}

/**
 * @brief Restore previous playback state after sound effect completes
 */
static void restore_previous_playback_state(void)
{
    ESP_LOGI(TAG, "restore_previous_playback_state()");
    
    if (!was_playing_before_effect || !saved_track_uri) {
        ESP_LOGI(TAG, "No previous playback to restore");
        sound_effect_playing = false;
        was_playing_before_effect = false;
        return;
    }
    
    ESP_LOGI(TAG, "Restoring previous track: %s", saved_track_uri);
    
    // Reset pipeline and restore original track
    audio_pipeline_stop(active_pipeline);
    audio_pipeline_wait_for_stop(active_pipeline);
    audio_pipeline_reset_ringbuffer(active_pipeline);
    audio_pipeline_reset_elements(active_pipeline);
    
    // Restore original track
    audio_element_set_uri(fatfs_reader, saved_track_uri);
    if (audio_pipeline_run(active_pipeline) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to restore original playback");
        stop_active_pipeline_internal();
        return;
    }
    
    // Buffer the restored track
    vTaskDelay(pdMS_TO_TICKS(200));

    // For A2DP, restart media stream only if still connected
    if (s3_active_sink == AUDIO_SINK_A2DP) {
        if (bt_is_a2dp_connected()) {
            bt_a2dp_start_media();
        } else {
            ESP_LOGW(TAG, "A2DP disconnected, cannot restore BT playback - stopping");
            stop_active_pipeline_internal();
            sound_effect_playing = false;
            was_playing_before_effect = false;
            free(saved_track_uri);
            saved_track_uri = NULL;
            return;
        }
    }

    // Ensure audio is unmuted for restored playback
    if (s3_active_sink == AUDIO_SINK_I2S) {
        codec_stop_mute_timer();    // Stop mute timer if running
        codec_unmute_for_i2s_playback();  // Unmute codec for restored playback
    }
    
    // Clean up saved state
    free(saved_track_uri);
    saved_track_uri = NULL;
    sound_effect_playing = false;
    was_playing_before_effect = false;
    
    ESP_LOGI(TAG, "Previous playback restored successfully");
}

/**
 * @brief Periodic check for audio pipeline completion - called from main loop
 * This function checks if audio has finished playing and automatically cleans up the pipeline
 */
void audio_pipeline_periodic_check(void)
{
    static TickType_t last_check_time = 0;
    static int slow_check_counter = 0;
    
    // Monitor periodic check performance
    TickType_t current_time = xTaskGetTickCount();
    if (last_check_time > 0) {
        TickType_t time_diff = current_time - last_check_time;
        if (time_diff > pdMS_TO_TICKS(200)) {  // More than 200ms since last check
            slow_check_counter++;
            if (slow_check_counter % 10 == 1) {  // Log every 10th slow check
                ESP_LOGW(TAG, "Periodic check delayed: %lums (slow checks: %d)", 
                         time_diff * portTICK_PERIOD_MS, slow_check_counter);
            }
        }
    }
    last_check_time = current_time;
    
    // Only check if we have an active pipeline and audio is actively playing (not paused/stopped)
    if (!active_pipeline || audio_state != AUDIO_STATE_PLAYING || !mp3_decoder) {
        return;
    }
    
    // Check the MP3 decoder state (most reliable indicator of completion)
    audio_element_state_t mp3_state = audio_element_get_state(mp3_decoder);
    
    if (mp3_state == AEL_STATE_FINISHED) {
        ESP_LOGI(TAG, "Audio finished naturally - automatic cleanup triggered");
        
        // Try to acquire mutex with very short timeout to avoid blocking main loop
        if (xSemaphoreTake(audio_mutex, pdMS_TO_TICKS(1)) == pdTRUE) {
            
            // Check if this was a sound effect that finished
            if (sound_effect_playing) {
                ESP_LOGI(TAG, "Sound effect completed - checking for restoration");
                restore_previous_playback_state();
                xSemaphoreGive(audio_mutex);
                return; // Don't proceed with normal auto-play logic
            }

            // Record natural completion for tracking (#15141)
            finish_playback_tracking(true);

            stop_active_pipeline_internal();
            ESP_LOGI(TAG, "Pipeline automatically cleaned up after audio finished");
            xSemaphoreGive(audio_mutex);

            // Trigger alarm repeat if alarm is active and should repeat (10min loop)
            if (current_audio_type == AUDIO_TYPE_ALARM && alarm_should_repeat) {
                ESP_LOGI(TAG, "Alarm audio finished - restarting for repeat (10min loop)");
                // Restart alarm audio (outside mutex, just like auto-play)
                play_auto_mode(s3_current_alarm->audio);

                // Re-set the alarm flags after playback starts (audio_play_internal overwrites them)
                current_audio_type = AUDIO_TYPE_ALARM;
                is_alarm_on_blankee = true;
                alarm_should_repeat = true;
                return; // Don't proceed with auto-play logic
            }

            bool skip_auto_play = suppress_auto_play_once;
            if (skip_auto_play) {
                ESP_LOGI(TAG, "Auto-play skipped once due to manual stop/effect");
                suppress_auto_play_once = false;
            }

            // Trigger auto-play logic if enabled and user is on PLAY_SCREEN
            if (!skip_auto_play && s3_auto_play_mode != AUTO_PLAY_OFF) {
                s3_screens_t current_screen = (s3_screens_t)get_current_screen();
                if ((current_screen == PLAY_SCREEN) || is_screen_dimmed()) {
                    ESP_LOGI(TAG, "Auto-play enabled on PLAY_SCREEN - advancing to next track");
                    ESP_LOGI(TAG, "Starting playback of next track");
                    app_state_handle_event(EVENT_BTN_B_SHORT);
                } else {
                    ESP_LOGI(TAG, "Auto-play enabled but not on PLAY_SCREEN (current: %d) - skipping auto-advance", current_screen);
                }
            }
        }
        // If we can't get the mutex immediately, that's ok - we'll try again next time
    }
}

/**
 * @brief Trigger shuffle reshuffle when entering PLAY_SCREEN
 * This function ensures that shuffle is properly initialized and display is updated
 */
void trigger_shuffle_reshuffle(void) {
    ESP_LOGI(TAG, "trigger_shuffle_reshuffle() called");
    
    if (s3_playback_mode != PLAYBACK_MODE_SHUFFLE) {
        ESP_LOGD(TAG, "Not in shuffle mode, no reshuffle needed");
        return;
    }
    
    if (!s3_current_album || s3_current_idx >= s3_current_size || s3_current_size_track == 0) {
        ESP_LOGW(TAG, "Invalid album state for shuffle reshuffle");
        return;
    }
    
    // Force recreation of simple shuffle to ensure fresh randomization
    ESP_LOGI(TAG, "Recreating simple shuffle for fresh randomization");
    create_current_shuffle();
    
    // Reset to first shuffled track
    if (current_shuffle_order && shuffle_count > 0) {
        shuffle_position = 0;
        s3_current_idx_track = current_shuffle_order[0];
        ESP_LOGI(TAG, "Reshuffle complete - first track: %u", (unsigned int)(s3_current_idx_track + 1));
        
        // Refresh the screen to update track number display
        ESP_LOGI(TAG, "Refreshing screen to update track display after reshuffle");
        refresh_screen_display();
    } else {
        ESP_LOGW(TAG, "Failed to create shuffle");
    }
}

/**
 * @brief Path 1: Play sound effect when nothing is currently playing (optimized for speed)
 */
static bool audio_play_sound_effect_while_stopped(const char *path)
{
    ESP_LOGI(TAG, "audio_play_sound_effect_while_stopped(path=\"%s\")", path);
    
    // Check file exists
    struct stat st;
    if (stat(path, &st) != 0) {
        ESP_LOGE(TAG, "Sound effect file does not exist: %s", path);
        return false;
    }
    
    // Ensure audio system is ready
    if (!ensure_audio_system_ready()) {
        ESP_LOGE(TAG, "Audio system not ready for sound effect");
        return false;
    }
    
    // Build a lightweight pipeline for I2S only (sound effects don't need A2DP)
    // Sound effects are always encrypted, so use encryption = true
    if (!init_audio_pipeline(AUDIO_SINK_I2S, true)) {
        ESP_LOGE(TAG, "Failed to initialize pipeline for sound effect");
        return false;
    }
    s3_active_sink = AUDIO_SINK_I2S;

    // For I2S sound effects: unmute codec and stop mute timer
    codec_stop_mute_timer();    // Stop mute timer if running
    codec_unmute_for_i2s_playback();  // Unmute codec before playback

    // Set URI and start pipeline
    audio_element_set_uri(fatfs_reader, path);
    if (audio_pipeline_run(active_pipeline) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start sound effect pipeline");
        stop_active_pipeline_internal();
        return false;
    }
    
    // Minimal buffering: 30ms instead of 50ms since we unmuted early
    vTaskDelay(pdMS_TO_TICKS(30));

    // Update state
    sound_effect_playing = true;
    audio_state = AUDIO_STATE_PLAYING;
    was_playing_before_effect = false;

    ESP_LOGI(TAG, "Sound effect started (optimized path - not playing)");
    return true;
}

/**
 * @brief Path 2: Play sound effect when music is currently playing (preserve current playback)
 */
static bool audio_play_sound_effect_while_playing(const char *path)
{
    ESP_LOGI(TAG, "audio_play_sound_effect_while_playing(path=\"%s\")", path);
    
    // Check file exists
    struct stat st;
    if (stat(path, &st) != 0) {
        ESP_LOGE(TAG, "Sound effect file does not exist: %s", path);
        return false;
    }
    
    // Save current track URI for restoration
    if (fatfs_reader) {
        char *current_uri = audio_element_get_uri(fatfs_reader);
        if (current_uri) {
            if (saved_track_uri) {
                free(saved_track_uri);
            }
            saved_track_uri = strdup_spiram(current_uri);
            ESP_LOGI(TAG, "Saved current track URI: %s", saved_track_uri);
        }
    }
    
    if (!active_pipeline) {
        ESP_LOGE(TAG, "No active pipeline to switch - falling back to stopped mode");
        return audio_play_sound_effect_while_stopped(path);
    }
    
    // Use pipeline switching technique as suggested
    ESP_LOGI(TAG, "Switching pipeline to sound effect...");
    
    // Stop current pipeline but don't destroy it
    audio_pipeline_stop(active_pipeline);
    audio_pipeline_wait_for_stop(active_pipeline);
    audio_pipeline_reset_ringbuffer(active_pipeline);
    audio_pipeline_reset_elements(active_pipeline);
    
    // Switch to sound effect
    audio_element_set_uri(fatfs_reader, path);
    if (audio_pipeline_run(active_pipeline) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to run pipeline with sound effect");
        // Try to restore original track
        if (saved_track_uri) {
            audio_element_set_uri(fatfs_reader, saved_track_uri);
            audio_pipeline_run(active_pipeline);
        }
        return false;
    }
    
    // Shorter buffer delay since pipeline infrastructure is already in place
    vTaskDelay(pdMS_TO_TICKS(100));

    // For A2DP, restart media stream only if still connected
    if (s3_active_sink == AUDIO_SINK_A2DP) {
        if (bt_is_a2dp_connected()) {
            bt_a2dp_start_media();
        } else {
            ESP_LOGW(TAG, "A2DP disconnected, cannot play sound effect via BT - stopping");
            stop_active_pipeline_internal();
            sound_effect_playing = false;
            if (saved_track_uri) {
                free(saved_track_uri);
                saved_track_uri = NULL;
            }
            return false;
        }
    }

    // Keep existing unmute behavior since we're switching content, not starting fresh
    if (s3_active_sink == AUDIO_SINK_I2S) {
        ESP_LOGI(TAG, "Using timer system for pipeline switch");
        codec_stop_mute_timer();    // Stop mute timer if running
        codec_unmute_for_i2s_playback();  // Ensure unmuted for switch
    }
    
    // Update state
    sound_effect_playing = true;
    was_playing_before_effect = true;  // Mark that we need to restore playback
    // audio_state remains in playing state since we're still playing something

    ESP_LOGI(TAG, "Sound effect started (optimized path - while playing)");
    return true;
}

/**
 * @brief Main entry point for quick sound effect playback
 * @param path Path to the sound effect file
 * @return true if sound effect playback started successfully, false otherwise
 */
bool audio_play_sound_effect_quick(const char *path)
{
    if (path == NULL || *path == '\0') {
        ESP_LOGE(TAG, "audio_play_sound_effect_quick: NULL or empty path");
        return false;
    }
    
    ESP_LOGI(TAG, "audio_play_sound_effect_quick(path=\"%s\")", path);
    
    suppress_auto_play_once = true; // Skip auto-play when detection sound finishes

    // Take mutex with shorter timeout since this is for quick sound effects
    if (xSemaphoreTake(audio_mutex, pdMS_TO_TICKS(500)) != pdTRUE) {
        ESP_LOGW(TAG, "audio_play_sound_effect_quick: timeout waiting for mutex");
        return false;
    }
    
    bool success = audio_play_sound_effect_while_stopped(path);
    
    xSemaphoreGive(audio_mutex);
    
    if (success) {
        ESP_LOGI(TAG, "Sound effect quick playback started successfully");
    } else {
        ESP_LOGE(TAG, "Sound effect quick playback failed");
    }

    return success;
}

// ============================================================================
// PLAYBACK TRACKING IMPLEMENTATION (#15141)
// ============================================================================

/**
 * @brief Extract filename from full path
 */
static const char* extract_filename(const char* full_path) {
    const char* filename = strrchr(full_path, '/');
    return filename ? filename + 1 : full_path;
}

/**
 * @brief Calculate actual playback duration (excluding pause time)
 */
static time_t get_actual_playback_duration(void) {
    time_t now = time(NULL);
    time_t total_time = now - current_tracking.start_time;
    return total_time - current_tracking.total_pause_time;
}

/**
 * @brief Start tracking playback session
 */
static void start_playback_tracking(const char* file_path) {
    if (sound_effect_playing) return; // Skip sound effects

    // Extract filename and get contentId
    const char* filename = extract_filename(file_path);
    const char* contentId = GetContentId(filename);

    if (!contentId) {
        ESP_LOGD(TAG, "No contentId found for filename: %s - skipping tracking", filename);
        // Reset any existing tracking state
        if (current_tracking.contentId) {
            free(current_tracking.contentId);
            current_tracking.contentId = NULL;
        }
        current_tracking.is_tracking = false;
        return; // Don't track files without contentId
    }

    // Initialize tracking only when contentId is available
    if (current_tracking.contentId) free(current_tracking.contentId);
    current_tracking.contentId = strdup_spiram(contentId);
    current_tracking.start_time = time(NULL);
    current_tracking.total_pause_time = 0;
    current_tracking.pause_start_time = 0;
    current_tracking.is_full_play = false;
    current_tracking.is_tracking = true;

    ESP_LOGI(TAG, "Started tracking playback: %s -> %s", filename, contentId);
}

/**
 * @brief Record pause start time
 */
static void pause_playback_tracking(void) {
    if (current_tracking.is_tracking && current_tracking.contentId &&
        current_tracking.pause_start_time == 0) {
        current_tracking.pause_start_time = time(NULL);
        ESP_LOGD(TAG, "Tracking paused for: %s", current_tracking.contentId);
    }
}

/**
 * @brief Accumulate pause duration on resume
 */
static void resume_playback_tracking(void) {
    if (current_tracking.is_tracking && current_tracking.contentId &&
        current_tracking.pause_start_time > 0) {
        time_t now = time(NULL);
        current_tracking.total_pause_time += (now - current_tracking.pause_start_time);
        current_tracking.pause_start_time = 0;
        ESP_LOGD(TAG, "Tracking resumed for: %s, total pause time: %ld seconds",
                 current_tracking.contentId, current_tracking.total_pause_time);
    }
}

/**
 * @brief Finish tracking and record session
 */
static void finish_playback_tracking(bool completed_naturally) {
    if (!current_tracking.is_tracking || !current_tracking.contentId) {
        // Clean up any partial state
        if (current_tracking.contentId) {
            free(current_tracking.contentId);
            current_tracking.contentId = NULL;
        }
        current_tracking.is_tracking = false;
        return;
    }

    // Handle pause state during completion
    if (current_tracking.pause_start_time > 0) {
        time_t now = time(NULL);
        current_tracking.total_pause_time += (now - current_tracking.pause_start_time);
    }

    time_t actual_duration = get_actual_playback_duration();
    time_t end_time = current_tracking.start_time + actual_duration;

    // Only record if playback was substantial (>5 seconds)
    if (actual_duration >= 5) {
        int ret = s3_tracking_add_record(
            current_tracking.contentId,
            current_tracking.start_time,
            end_time,
            completed_naturally ? 1 : 0
        );

        ESP_LOGI(TAG, "Recorded tracking: %s, duration=%lds, full_play=%d, result=%d",
                 current_tracking.contentId, actual_duration, completed_naturally, ret);
    } else {
        ESP_LOGD(TAG, "Skipping tracking record for %s - too short (%ld seconds)",
                 current_tracking.contentId, actual_duration);
    }

    // Reset tracking state
    free(current_tracking.contentId);
    current_tracking.contentId = NULL;
    current_tracking.is_tracking = false;
}

/**
 * @brief Save tracking record if active (for manual stops)
 */
static void save_tracking_record_if_active(void) {
    if (current_tracking.is_tracking) {
        finish_playback_tracking(false); // Manual stop = not full play
    }
}

/**
 * @brief Clean up tracking state (for system shutdown)
 */
static void cleanup_playback_tracking(void) {
    if (current_tracking.contentId) {
        ESP_LOGD(TAG, "Cleaning up tracking state for: %s", current_tracking.contentId);
        free(current_tracking.contentId);
        current_tracking.contentId = NULL;
    }
    current_tracking.is_tracking = false;
}

