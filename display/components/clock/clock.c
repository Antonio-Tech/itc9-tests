#include "clock.h"
#include "esp_timer.h"
#include "esp_log.h"
#include <time.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
static const char *TAG = "CLOCK";

clock_screen_cb_t s_clock_screen_cb = NULL;
static esp_timer_handle_t s_clock_timer = NULL;
static volatile bool s_clock_signal = false;

static int last_minute_recorded = -1;
static time_t now;
static struct tm *tm_info;

static void IRAM_ATTR clock_timer_cb(void *arg)
{
    s_clock_signal = true;
    now = time(NULL);
    tm_info = localtime(&now);

    if (!tm_info) return;

    int curr_minute = tm_info->tm_min;
    if (curr_minute != last_minute_recorded)
    {
        last_minute_recorded = curr_minute;
        s_clock_screen_cb();
    }
}

void setup_clock_update_screen_cb(clock_screen_cb_t clock_screen_cb)
{
    s_clock_screen_cb = clock_screen_cb;
}

bool is_clock_initialized(void)
{
    return (s_clock_timer != NULL);
}

esp_err_t init_clock(void)
{
    const esp_timer_create_args_t timer_args = {
        .callback = &clock_timer_cb,
        .arg = NULL,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "clock_sec_timer"
    };

    if (esp_timer_create(&timer_args, &s_clock_timer) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create esp_timer");
        return ESP_FAIL;
    }
    
    if (esp_timer_start_periodic(s_clock_timer, 1000000) != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed to start timer");
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "Clock started");
    return ESP_OK;
}

esp_err_t deinit_clock(void)
{
    if (s_clock_timer) {
        esp_timer_stop(s_clock_timer);
        esp_timer_delete(s_clock_timer);
        s_clock_timer = NULL;
    }

    s_clock_screen_cb = NULL;
    ESP_LOGI(TAG, "Clock deinitialized");
    return ESP_OK;
}
