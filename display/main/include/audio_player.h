/**
 * @file audio_player.h
 * @author Igor Oliveira
 * @date 2025-06-04
 * @brief Header for system audio effect player
 *
 * This header provides function declarations to trigger playback of
 * system sound effects stored in SD Card, such as boot, shutdown, alarm,
 * and sample sounds. These functions are used by higher-level application
 * components to provide audible system feedback.
 */


#ifndef AUDIO_PLAYER_H
#define AUDIO_PLAYER_H

#include "audio_element.h"
#include "audio_pipeline.h"
#include "audio_hal.h"
#include "s3_definitions.h"

extern SemaphoreHandle_t audio_mutex;
extern bool is_powered_on;

/**
 * @brief Audio type enumeration to track which type of audio was last played
 */
typedef enum {
    AUDIO_TYPE_NONE,        // No audio playing or user stopped it
    AUDIO_TYPE_TRACK,       // Album track (resume after alarm)
    AUDIO_TYPE_ALARM,       // Alarm sound
    AUDIO_TYPE_EFFECT       // Any other system sound (boot, shutdown, volume, etc.)
} audio_type_t;


/**
 * @brief Initialize the audio player system
 * @return ESP_OK on success, ESP_FAIL on failure
 * @note This function initializes the audio mutex and sets up the audio board.
 * It also powers on the audio system and prepares it for playback.
 */
esp_err_t audio_player_init(void);

/**
 * @brief Clean up audio player and persistent elements
 * @note This function should be called at system shutdown to properly clean up
 * persistent elements and release all resources.
 */
void audio_player_cleanup(void);

/**
 * @brief Initialize persistent I2S element (for emergency memory management)
 * @return true if initialization succeeded, false otherwise
 * @note This function is exposed for emergency WiFi initialization cleanup
 */
bool init_persistent_i2s_element(void);

/**
 * @brief Clean up persistent I2S element (for emergency memory management)
 * @note This function is exposed for emergency WiFi initialization cleanup
 */
void cleanup_persistent_i2s_element(void);

/**
 * @brief Audio system power on function (for emergency memory management)
 * @note This function is exposed for emergency WiFi initialization cleanup
 */
void audio_power_on(void);

/**
 * @brief Audio system power off function (for emergency memory management)
 * @note This function is exposed for emergency WiFi initialization cleanup
 */
void audio_power_off(void);

/**
 * @brief Immediately stops any ongoing audio playbook
 */
void play_stop(void);

/**
 * @brief Pause the currently playing audio
 */
void play_pause(void);

/**
 * @brief Resume the currently paused audio
 */
void play_resume(void);

/**
 * @brief Check if audio is currently paused with an active pipeline
 * @return true if audio is paused AND there's an active pipeline, false otherwise
 * @note This ensures we're in a valid playback context (not just paused state without audio loaded)
 */
bool is_audio_paused(void);

/**
 * @brief Check if audio is currently stopped with an active pipeline
 * @return true if audio is stopped AND there's an active pipeline, false otherwise
 * @note This ensures we're in a valid playback context (not just paused state without audio loaded)
 */
bool is_audio_stopped(void);

/**
 * @brief Checks whether an audio file is currently playing.
 *
 * Useful to prevent overlapping playback or to synchronize audio events.
 *
 * @return true if audio is playing, false otherwise.
 */
extern bool is_audio_playing(void);

/**
 * @brief Check if audio state is stable (not transitioning)
 *
 * @return true if audio is in a stable state (PLAYING, PAUSED, or STOPPED), false during transitions (PAUSING, RESUMING)
 * @note Use this to prevent operations during state transitions that could corrupt the audio pipeline
 */
extern bool is_state_stable(void);

/**
 * @brief Get the current audio type being played
 * @return Current audio type (TRACK, ALARM, BOOT, SHUTDOWN, VOLUME, EFFECT)
 * @note Use this to determine if audio was a track before alarm interrupted it
 */
audio_type_t get_current_audio_type(void);

/**
 * @brief Play the boot sound from SD Card.
 *
 * This function plays the 'boot.mp3' audio file stored in the SD Card filesystem.
 * It is intended to be called during system initialization.
 */
void play_audio_boot(void);

/**
 * @brief Play the shutdown sound from SD Card.
 *
 * This function plays the 'shutdown.mp3' audio file stored in the SD Card filesystem.
 * It is intended to be called during system shutdown or power-off sequence.
 */
void play_audio_shutdown(void);

/**
 * @brief Play the volume sound from SD Card.
 *
 * This function plays the 'volume.mp3' audio file stored in the SD Card filesystem.
 * It is intended to be called during system shutdown or power-off sequence.
 */
void play_audio_volume(void);

/**
 * @brief Play the alarm sound from SD Card.
 *
 * This function plays the 'alarm.mp3' audio file stored in the SD Card filesystem.
 * It is intended to be used for alerts or warnings.
 */
void play_audio_alarm(void);

/**
 * @brief Resume audio playback if it was playing before alarm started
 */
void resume_audio_after_alarm(void);

/**
 * @brief Stop alarm repeat behavior (called when alarm is dismissed)
 */
void stop_alarm_repeat(void);

/**
 * @brief Automatically pause audio when A2DP connection is lost
 * This function pauses audio during BT disconnection and tracks the state for auto-resume
 */
void pause_audio_for_bt_disconnect(void);

/**
 * @brief Automatically resume audio when A2DP connection is restored
 * This function resumes audio only if it was paused due to BT disconnection
 */
void resume_audio_after_bt_reconnect(void);

/**
 * @brief Clear the BT disconnect pause flag to prevent stale state
 * This should be called when leaving PLAY_SCREEN or when audio state changes
 */
void clear_bt_disconnect_pause_flag(void);

/**
 * @brief Get current file position for pause/resume across sink changes
 * @return Current byte position in file, or -1 if no active playback
 */
int audio_get_file_position(void);

/**
 * @brief Resume playback from a saved file position (for sink switching)
 * @param position Byte position in file to resume from
 * @note This rebuilds the pipeline with current sink and seeks to position
 */
void audio_play_from_position(int position);



/**
 * @brief Increase the audio volume by one level
 *
 * This function increases the system volume to the next level (1-6).
 * It uses the audio_hal API to set the new volume level.
 */
void increase_volume(void);

/**
 * @brief Decrease the audio volume by one level
 *
 * This function decreases the system volume to the previous level (1-6).
 * It uses the audio_hal API to set the new volume level.
 */
void decrease_volume(void);

/**
 * @brief Get current hardware volume level (1-6)
 * @return Current volume level (1-6), or -1 if audio system not ready
 */
int get_current_volume_level(void);

/**
 * @brief Set hardware volume level directly (1-6)
 * @param level Target volume level (1-6)
 * @return true if successful, false if audio system not ready or invalid level
 */
bool set_volume_level(int level);

/**
 * @brief Sync UI volume level with hardware volume level
 * Call this at boot to ensure UI shows correct volume
 */
void sync_volume_with_hardware(void);

/**
 * @brief Backup current volume level when entering volume screen
 * @note Call this when transitioning to VOLUME_UP/DOWN_SCREEN
 */
void volume_screen_enter(void);

/**
 * @brief Save current volume level to NVS flash immediately
 * @note Flushes HAL's cached volume to NVS
 */
void volume_confirm_and_save(void);

/**
 * @brief Restore backed up volume level and save to NVS
 * @note Cancels any unsaved volume changes
 */
void volume_cancel_and_restore(void);

/**
 * @brief Play next track in playlist
 */
void audio_play_next();

/**
 * @brief Navigate through tracks in the current album
 * @param next true to go to next track, false to go to previous track
 * @note Cycles through tracks - when at last track, next goes to first; when at first track, previous goes to last
 */
void one_step_track(bool next);

bool audio_player_is_running(void);
void audio_pipeline_periodic_check(void);

/**
 * @brief Set the playback mode (sequential or shuffle)
 * @param mode PLAYBACK_MODE_SEQUENTIAL or PLAYBACK_MODE_SHUFFLE
 */
void set_playback_mode(playback_mode_t mode);

/**
 * @brief Get the current playback mode
 * @return Current playback mode
 */
playback_mode_t get_playback_mode(void);

/**
 * @brief Set the auto-play mode
 * @param mode AUTO_PLAY_OFF, AUTO_PLAY_FOLDER, or AUTO_PLAY_ALL
 */
void set_auto_play_mode(auto_play_mode_t mode);

/**
 * @brief Get the current auto-play mode
 * @return Current auto-play mode
 */
auto_play_mode_t get_auto_play_mode(void);

/**
 * @brief Get the current track position for UI display
 * @return Current track number (1-based) from the original playlist
 * @note Returns the original track number regardless of playback mode (sequential or shuffle)
 */
size_t get_current_track_display_position(void);

/**
 * @brief Navigate through tracks with shuffle support
 * @param next true to go to next track, false to go to previous track
 * @note Respects the current playback mode (sequential or shuffle)
 */
void one_step_track_shuffle(bool next);

/**
 * @brief Trigger shuffle reshuffle (call when entering PLAY_SCREEN)
 * @note This marks the shuffle playlist for re-initialization
 */
void trigger_shuffle_reshuffle(void);

/**
 * @brief Play current track in playlist with transition control
 * @param is_track_transition true if this is a track change (enables gapless transition), false for new playback
 */
void audio_start_playing_with_transition(bool is_track_transition);

/**
 * @brief Play sound effect with optimized quick playback
 * @param path Path to the sound effect file
 * @return true if sound effect playback started successfully, false otherwise
 * @note This function uses optimized paths for faster sound effect playback:
 *       - When not playing: Uses direct unmute and minimal buffering
 *       - When playing: Uses pipeline switching to preserve current playback
 */
bool audio_play_sound_effect_quick(const char *path);

/**
 * @brief Scan directory for MP3 files and build playlist
 */
void build_playlist();

/**
 * @brief Build playlist with tracks from multiple SKUs (English and Chinese versions)
 * Used when LANGUAGE_ALL is selected for NFC content
 */
void build_playlist_all_languages(char** skus, int sku_count);

/**
 * @brief Check if a file is a real MP3 by reading its header
 * @param filepath Full path to the file to check
 * @return true if file has valid MP3 header, false otherwise
 */
bool is_real_mp3_file(const char *filepath);


void reset_albums_from_nfc(void);

/* the ONE entry-point — returns true when playback was started */
bool audio_play(const char *path, audio_sink_t sink_pref);

void resume_audio_to_now_playing(void);
void audio_update_album_data(void);

void play_album();
void one_step_album(bool next);
void n_step_album(size_t target_idx);
void update_alarm(s3_alarms_t alarm_id);

void audio_play_next_album_track(void);
void audio_play_previous_album_track(void);
void audio_start_playing();

#define play_auto_mode(path)    audio_play((path), AUDIO_SINK_AUTO)
#define play_local_mode(path)   audio_play((path), AUDIO_SINK_I2S)
#define play_bt_mode(path)      audio_play((path), AUDIO_SINK_A2DP)

#endif // AUDIO_PLAYER_H
