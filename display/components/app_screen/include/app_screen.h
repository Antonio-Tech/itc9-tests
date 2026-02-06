/**
 * @file app_screen.h
 * @author Igor Oliveira
 * @date 2025-06-12
 * @brief Screen management and transition functions
 *
 * Defines the interface for all application screen transitions and UI states.
 * Provides function declarations for switching between different application screens
 * and managing the visual state of the device.
 */

#ifndef APP_SCREEN_H
#define APP_SCREEN_H

#include "lv_screen_mgr.h"

/**
 * @brief Transition to device boot screen
 * 
 * Initial screen shown during device startup sequence.
 */
void boot_screen(void);

/**
 * @brief Transition to main home screen
 * 
 * Central navigation hub with access to all main features.
 */
void home_screen(void);

/**
 * @brief Transition to media playback screen
 * 
 * Controls and displays current media playback status.
 */
void play_screen(void);

/**
 * @brief Transition to volume control screen
 * 
 * Interface for adjusting audio output levels.
 */
void volume_screen(void);

/**
 * @brief Transition to clock display screen
 * 
 * Shows current time and date information.
 */
void clock_screen(void);

/**
 * @brief Transition to alarm management screen
 * 
 * Interface for setting and managing alarm schedules.
 */
void alarm_screen(void);

/**
 * @brief Transition to display settings screen
 * 
 * Controls for brightness, timeout, and other visual parameters.
 */
void display_screen(void);

/**
 * @brief Transition to Bluetooth main screen
 * 
 * Shows Bluetooth connection status and basic controls.
 */
void bluetooth_screen(void);

/**
 * @brief Transition to Bluetooth settings screen
 * 
 * Advanced Bluetooth configuration and device management.
 */
void bluetooth_settings_screen(void);

/**
 * @brief Transition to WiFi settings screen
 * 
 * Network configuration and access point selection.
 */
void wifi_settings_screen(void);

/**
 * @brief Transition to WiFi connection screen
 * 
 * Interface for establishing new network connections.
 */
void wifi_connect_screen(void);

/**
 * @brief Transition to WiFi pairing screen
 * 
 * Handles credential input and authentication process.
 */
void wifi_pairing_screen(void);

/**
 * @brief Transition to data synchronization screen
 * 
 * Manages cloud sync and data transfer operations.
 */
void data_sync_screen(void);

/**
 * @brief Transition to OTA update screen
 * 
 * Handles firmware update process and progress display.
 */
void ota_screen(void);

/**
 * @brief Transition to NFC operations screen
 * 
 * Interface for NFC tag reading/writing operations.
 */
void nfc_screen(void);

/**
 * @brief Transition to low power warning screen
 * 
 * Shows battery status and power saving options.
 */
void power_low_screen(void);

/**
 * @brief Transition to standby mode screen
 * 
 * Reduced functionality mode for power conservation.
 */
void standby_screen(void);

/**
 * @brief Transition to shutdown confirmation screen
 * 
 * Final screen before device powers off.
 */
void shutdown_screen(void);

/**
 * @brief Transition to network status screen
 * 
 * Shows detailed network connection information.
 */
void network_screen(void);

/**
 * @brief Transition to NFC language selection
 * 
 * Interface for choosing language via NFC tags.
 */
void nfc_language_screen(void);

/**
 * @brief Enter idle mode
 * 
 * Minimal power state while maintaining basic functionality.
 */
void idle_mode(void);

#endif // APP_SCREEN_H