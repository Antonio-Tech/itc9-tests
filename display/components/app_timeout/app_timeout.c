#include "app_timeout.h"
#include "esp_timer.h"
#include "app_state_machine.h" // For events and the handler
#include "esp_log.h"
#include "audio_player.h"
#include "power_management.h"
#include "WiFi.h"

static esp_timer_handle_t g_deepsleep_timer_handle;
static evt_state_machine_cb_t s_evt_state_machine_cb = NULL;

// To test change to 2 minutes, for example (2 * 60 * 1000 * 1000)
// Deep Sleep timeout (from standby): 10 minutes
#define DEEP_SLEEP_TIMEOUT_US (10 * 60 * 1000 * 1000)

// To test change to 1 minutes, for example (1 * 60 * 1000 * 1000)
// Standby inactivity timeout: 2 minutes
#define STANDBY_TIMEOUT_US (2 * 60 * 1000 * 1000)

static const char *TAG = "app_timeout";
static esp_timer_handle_t g_standby_timer_handle = NULL;

void setup_state_handle_cb(evt_state_machine_cb_t *evt_state_machine_cb)
{
    s_evt_state_machine_cb = evt_state_machine_cb;
}

// Timer callback that sends the event to the state machine
static void standby_timer_callback(void *arg) {
    ESP_LOGW(TAG, "Standby timer expired. Posting event.");
    // Correctly call the event handler with the unified event name
    if (s_evt_state_machine_cb) {
        s_evt_state_machine_cb(EVENT_ENTER_STANDBY);
    } else {
        ESP_LOGE(TAG, "!!! Callback is NULL !!! ");
    }
}

// Initialize the inactivity timer
esp_err_t app_timeout_init(void) {
    // if timer already exists, do not create it again
    if (g_standby_timer_handle != NULL) {
        ESP_LOGI(TAG, "Timer already initialized, skipping creation.");
        return ESP_OK;
    }

    ESP_LOGW(TAG, "Initializing inactivity timer.");
    const esp_timer_create_args_t standby_timer_args = {
        .callback = &standby_timer_callback,
        .name = "standby-timer"
    };
    return esp_timer_create(&standby_timer_args, &g_standby_timer_handle);
}

// Reset the inactivity timer - called on any user interaction
esp_err_t app_timeout_reset(void) {
    ESP_LOGI(TAG, "app_timeout_reset reset timeout.");
    if (g_standby_timer_handle) {
        esp_timer_stop(g_standby_timer_handle);
        esp_timer_start_once(g_standby_timer_handle, STANDBY_TIMEOUT_US);
    } else{
        ESP_LOGW(TAG, "Timer not initialized, initializing now.");
        app_timeout_init();
        if (g_standby_timer_handle) {
            esp_timer_start_once(g_standby_timer_handle, STANDBY_TIMEOUT_US);
        }
    }
    return ESP_OK;
}

// Stop the inactivity timer (called when entering standby)
esp_err_t app_timeout_stop(void) {
    ESP_LOGW(TAG, "app_timeout_stop");
    if (g_standby_timer_handle) {
        esp_timer_stop(g_standby_timer_handle);
        esp_timer_delete(g_standby_timer_handle);
        g_standby_timer_handle = NULL;
    }
    return ESP_OK;
}

void app_timeout_restart(void)
{
    // just ensure timer exists, then reset it
    // app_timeout_init() will check if it already exists, and will not create it again
    app_timeout_init();
    app_timeout_reset();
}

// Moved to .h file
// extern bool global_poweroff; // Global flag to indicate power off
static void deep_sleep_timer_callback(void *arg)
{
    ESP_LOGW(TAG, "Deep sleep timer expired. Requesting shutdown.");
    // Do NOT start audio or heavy work in timer context; set flag only
    global_poweroff = POWER_MODE_SHUTDOWN; // Trigger shutdown sequence in main context
}

void app_timeout_deepsleep_init(void)
{
    const esp_timer_create_args_t deep_sleep_timer_args = {
        .callback = &deep_sleep_timer_callback,
        .name = "deepsleep-timer"
    };
    esp_timer_create(&deep_sleep_timer_args, &g_deepsleep_timer_handle);
}

void app_timeout_deepsleep_start(void)
{
    ESP_LOGI(TAG, "Starting deep sleep timer (10 minutes).");
    if (g_deepsleep_timer_handle) {
        esp_timer_start_once(g_deepsleep_timer_handle, DEEP_SLEEP_TIMEOUT_US);
    }
}

void app_timeout_deepsleep_stop(void)
{
    ESP_LOGI(TAG, "Stopping deep sleep timer.");
    if (g_deepsleep_timer_handle && esp_timer_is_active(g_deepsleep_timer_handle)) {
        esp_timer_stop(g_deepsleep_timer_handle);
    }
}
