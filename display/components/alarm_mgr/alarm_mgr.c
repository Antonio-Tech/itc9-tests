#include <s3_logger.h>
#include <string.h>
#include "esp_event.h"
#include "esp_log.h"
#include "alarm_mgr.h"
#include "esp_peripherals.h"
#include "s3_definitions.h"
#include "audio_player.h"
#include "lv_screen_mgr.h"
#include "sntp_syncer.h"
#include "backlight.h"
#include "cJSON.h"
#include "app_timeout.h"
#include "esp_timer.h"
#include "nfc-service.h"
#include "esp_sleep.h"
#include "app_state_machine.h"
#include "esp_heap_caps.h"

#define ALARM_OP_1 "72b8c6cf92b14f2b337b340b3de41bea.mp3"
#define ALARM_OP_2 "1e9ad1c4eb31b48cfe972c82c08ed3fe.mp3"
#define ALARM_OP_3 "2fb394f12ebb31d7465c7bfd4c887717.mp3"
#define ALARM_OP_4 "9877e8fcd124390043e52a40233247ed.mp3"
#define ALARM_OP_5 "64af17d829764c745a69a568e17d3d5e.mp3"
#define ALARM_OP_6 "67b154078a22c4b1ff809ec3cc172291.mp3"
#define ALARM_OP_7 "64b778eb21ce859edd57d4a5140d3db3.mp3"

#define ALARM_US_SCALE (1 * 1000 * 1000)
#define ALARM_TIMEOUT_SECONDS (600)  // 10 minutes for alarm auto-dismiss
#define ALARM_LIST_FILE_PATH ("/sdcard/tmp/alarms_list.json")

static const char *TAG = "ALARM_MANAGER";

extern esp_err_t g_init_sdcard;

extern int64_t last_alarm;

static int count = 0;
static uint8_t alarms_epochs_idx = 0;
static esp_timer_handle_t s_alarm_timer = NULL;
static esp_timer_handle_t s_alarm_timout_timer = NULL;
static alarm_epoch_t alarms_epochs[ALARM_LIST_LEN] = {0};
static RTC_DATA_ATTR s3_alarms_t power_off_alarm_opt = ALARM_1;

esp_err_t init_alarm_timeout_timer(void);

// SCHEDULE RELATED ==================================================================
int day_of_week_to_int(const char *day)
{
    if (strcmp(day, "Sunday") == 0) return 0;
    if (strcmp(day, "Monday") == 0) return 1;
    if (strcmp(day, "Tuesday") == 0) return 2;
    if (strcmp(day, "Wednesday") == 0) return 3;
    if (strcmp(day, "Thursday") == 0) return 4;
    if (strcmp(day, "Friday") == 0) return 5;
    if (strcmp(day, "Saturday") == 0) return 6;
    return -1;
}

int generate_schedule_epochs(const schedule_t *schedule, time_t base_epoch, time_t *out_epochs, size_t max_out) {
    struct tm base_tm;
    localtime_r(&base_epoch, &base_tm);

    int hour, minute;
    sscanf(schedule->time_str, "%d:%d", &hour, &minute);

    if (strcmp(schedule->period, "PM") == 0 && hour != 12) hour += 12;
    else if (strcmp(schedule->period, "AM") == 0 && hour == 12) hour = 0;

    int count = 0;
    for (int i = 0; i < schedule->days_count && count < max_out; ++i) {
        int target_wday = day_of_week_to_int(schedule->days[i]);
        if (target_wday == -1) continue;

        struct tm next_tm = base_tm;
        next_tm.tm_hour = hour;
        next_tm.tm_min = minute;
        next_tm.tm_sec = 0;

        int delta_days = (target_wday - base_tm.tm_wday + 7) % 7;
        if (delta_days == 0 && mktime(&next_tm) <= base_epoch) {
            delta_days = 7;
        }

        next_tm.tm_mday += delta_days;
        time_t next_time = mktime(&next_tm);
        if (count < max_out) {
            out_epochs[count++] = next_time;
        }
    }
    return count;
}

int compare_alarm_epochs(const void *a, const void *b)
{
    const alarm_epoch_t *ae = (const alarm_epoch_t *)a;
    const alarm_epoch_t *be = (const alarm_epoch_t *)b;
    return (ae->epoch > be->epoch) - (ae->epoch < be->epoch);
}

int parse_json_and_generate_epochs(const char *json_text, time_t base_epoch, alarm_epoch_t *epochs_out, size_t max_epochs) {
    cJSON *root = cJSON_Parse(json_text);
    if (!root) return -1;

    cJSON *alarms_array = cJSON_GetObjectItem(root, "alarms");
    if (!cJSON_IsArray(alarms_array)) {
        cJSON_Delete(root);
        return -1;
    }

    int total_epochs = 0;

    cJSON *alarm;
    cJSON_ArrayForEach(alarm, alarms_array) {
        cJSON *time_str = cJSON_GetObjectItem(alarm, "time");
        cJSON *period = cJSON_GetObjectItem(alarm, "period");
        cJSON *days_array = cJSON_GetObjectItem(alarm, "days");
        cJSON *media = cJSON_GetObjectItem(alarm, "filename");
        cJSON *is_active = cJSON_GetObjectItem(alarm, "isActive");

        if (!cJSON_IsString(time_str) || !cJSON_IsString(period) || !cJSON_IsArray(days_array))
            continue;

        if ((is_active != NULL) && (!cJSON_IsTrue(is_active)))
            continue;

        int days_count = cJSON_GetArraySize(days_array);
        const char **days = heap_caps_malloc(sizeof(char *) * days_count, MALLOC_CAP_SPIRAM);
        if (!days) continue;

        for (int i = 0; i < days_count; ++i) {
            cJSON *day = cJSON_GetArrayItem(days_array, i);
            days[i] = strdup_spiram(day->valuestring);
        }

        schedule_t sched = {
            .time_str = strdup_spiram(time_str->valuestring),
            .period = strdup_spiram(period->valuestring),
            .days = days,
            .days_count = days_count,
            .media = media ? strdup_spiram(media->valuestring) : NULL,
        };

        time_t temp_epochs[7];
        int n = generate_schedule_epochs(&sched, base_epoch, temp_epochs, 7);

        for (int i = 0; i < n && total_epochs < max_epochs; ++i) {
            epochs_out[total_epochs].epoch = temp_epochs[i];
            epochs_out[total_epochs].media = sched.media ? strdup_spiram(sched.media) : NULL;
            total_epochs++;
        }

        free((void *)sched.time_str);
        free((void *)sched.period);
        if (sched.media) free((void *)sched.media);
        for (int i = 0; i < sched.days_count; ++i) {
            free((void *)sched.days[i]);
        }
        free(sched.days);
    }

    cJSON_Delete(root);
    return total_epochs;
}

char *wrap_partial_json(const char *json_text) {
    if (json_text[0] != '{') {
        size_t len = strlen(json_text) + 3;
        char *wrapped = heap_caps_malloc(len, MALLOC_CAP_SPIRAM);
        if (!wrapped) return NULL;
        snprintf(wrapped, len, "{%s}", json_text);
        return wrapped;
    } else {
        return strdup_spiram(json_text);
    }
}

char *extract_alarms_json_text(const char *full_json_text) {
    char *wrapped_json = wrap_partial_json(full_json_text);
    if (!wrapped_json) {
        ESP_LOGE(TAG, "Error on packing partial JSON.");
        return NULL;
    }

    cJSON *root = cJSON_Parse(wrapped_json);
    free(wrapped_json);
    if (!root) {
        ESP_LOGE(TAG, "Error on parsing JSON.");
        return NULL;
    }

    cJSON *alarms = cJSON_GetObjectItem(root, "alarms");

    if (!cJSON_IsArray(alarms)) {
        cJSON *result = cJSON_GetObjectItem(root, "result");
        if (cJSON_IsObject(result)) {
            alarms = cJSON_GetObjectItem(result, "alarms");
        }
    }

    if (!cJSON_IsArray(alarms)) {
        cJSON_Delete(root);
        ESP_LOGE(TAG, "'alarms' key not found or is not an array.");
        return NULL;
    }

    cJSON *wrapped = cJSON_CreateObject();
    if (!wrapped) {
        cJSON_Delete(root);
        return NULL;
    }

    cJSON_AddItemToObject(wrapped, "alarms", cJSON_Duplicate(alarms, 1));
    char *output = cJSON_PrintUnformatted(wrapped);

    cJSON_Delete(wrapped);
    cJSON_Delete(root);
    return output; // use free()
}

int save_alarms_to_file(const char *alarms_json_text, const char *filename) {
    if (!alarms_json_text || !filename) {
        ESP_LOGE(TAG, "Ivalid parameters to save file.");
        return -1;
    }

    FILE *f = s3_fopen(filename, "w");
    if (!f) {
        ESP_LOGE(TAG, "Fail on openning file to write.");
        return -1;
    }

    fprintf(f, "%s", alarms_json_text);
    s3_fclose(f);
    return 0;
}

esp_err_t clear_alarm_file_content(void)
{
    FILE *file = s3_fopen(ALARM_LIST_FILE_PATH, "w");
    if (file == NULL)
    {
        ESP_LOGE(TAG, "Fail on openning file to read");
        return ESP_FAIL;
    }
    fclose(file);
    ESP_LOGI(TAG, "Alarms reseted");
    return ESP_OK;
}

s3_alarms_t get_alarm_option(const char *alarm_media)
{
    s3_alarms_t alarm_opt = ALARM_1;

    if (strcmp(ALARM_OP_1, alarm_media) == 0)
    {
        alarm_opt = ALARM_1;
    }
    else if (strcmp(ALARM_OP_2, alarm_media) == 0)
    {
        alarm_opt = ALARM_2;
    }
    else if (strcmp(ALARM_OP_3, alarm_media) == 0)
    {
        alarm_opt = ALARM_3;
    }
    else if (strcmp(ALARM_OP_4, alarm_media) == 0)
    {
        alarm_opt = ALARM_4;
    }
    else if (strcmp(ALARM_OP_5, alarm_media) == 0)
    {
        alarm_opt = ALARM_5;
    }
    else if (strcmp(ALARM_OP_6, alarm_media) == 0)
    {
        alarm_opt = ALARM_6;
    }
    else if (strcmp(ALARM_OP_7, alarm_media) == 0)
    {
        alarm_opt = ALARM_7;
    }
    else
    {
        ESP_LOGW(TAG, "Unknown media - selecting (ALARM_%d) as default", ((uint8_t)alarm_opt + 1));
        alarm_opt = ALARM_1;
    }

    ESP_LOGI(TAG, "Alarm option selected: ALARM_%d", (alarm_opt + 1));
    return alarm_opt;
}

void alarm_cb(void)
{
    ESP_LOGI(TAG, "Alarm cb");
    if(is_screen_dimmed() == true)
    {
        undimmed_backlight();
        restart_dimmer_timer();
    }

    stop_alarm_timer();
    update_alarm(get_alarm_option(alarms_epochs[alarms_epochs_idx].media));

    // Check if sync is in progress - disable alarms during sync to prevent crashes
    if (gSyncInProgress) {
        ESP_LOGW(TAG, "Sync in progress - skipping alarm to prevent audio system conflicts");
        // Reschedule the alarm for 1 minute later to try again after sync
        time_t reschedule_time = 60; // 1 minute
        set_alarm_interval(reschedule_time);
        // Timer continues running - no need to restart
        return;
    }

    // Check if BT reconnection is in progress - disable alarms during reconnect
    if (gBTReconnectInProgress) {
        ESP_LOGW(TAG, "BT reconnection in progress - skipping alarm to prevent audio system conflicts");
        // Reschedule the alarm for 1 minute later to try again after reconnect
        time_t reschedule_time = 60; // 1 minute
        set_alarm_interval(reschedule_time);
        // Timer continues running - no need to restart
        return;
    }

    backlight_on();

    // Stop old alarm timeout timer before creating new one (handles overlapping alarms)
    stop_alarm_timeout_timer();
    init_alarm_timeout_timer();

    app_timeout_stop();            // Stop standby timer during alarm
    app_timeout_deepsleep_stop();  // Stop deep sleep timer during alarm
    nfc_pause();

    set_current_screen(ALARM_SCREEN, NULL_SCREEN);
    set_last_transition_callback(play_audio_alarm);

    get_alarm_setting(TIMER_SOURCE_ESP_TIMER);
}

void alarm_from_deep_sleep(void)
{
    ESP_LOGI(TAG, "Alarm from deep sleep cb");

    update_alarm(power_off_alarm_opt);
    init_alarm_timeout_timer();
    app_timeout_stop();            // Stop standby timer during alarm
    app_timeout_deepsleep_stop();  // Stop deep sleep timer during alarm

    set_current_screen(ALARM_SCREEN, NULL_SCREEN);
    play_audio_alarm();

    get_alarm_setting(TIMER_SOURCE_ESP_TIMER);
}

void register_alarms(const char *full_json_text)
{
    time_t now;
    get_system_epoch(&now);

    char *alarms_json = extract_alarms_json_text(full_json_text);
    if (!alarms_json)
    {
        ESP_LOGE(TAG, "Error on extracting alarms from JSON");
        return;
    }

    count = parse_json_and_generate_epochs(alarms_json, now, alarms_epochs, ALARM_LIST_LEN);
    if (save_alarms_to_file(alarms_json, ALARM_LIST_FILE_PATH) == 0) {
        ESP_LOGI(TAG, "Success on saving [%s] file.", ALARM_LIST_FILE_PATH);
    }
    free(alarms_json);    
}

void start_alarm_list(const char *full_json_text, alarm_timer_src_t alarm_timer_src)
{
    time_t now;
    get_system_epoch(&now);

    char *alarms_json = extract_alarms_json_text(full_json_text);
    if (!alarms_json)
    {
        ESP_LOGE(TAG, "Error on extracting alarms from JSON");
        return;
    }

    count = parse_json_and_generate_epochs(alarms_json, now, alarms_epochs, ALARM_LIST_LEN);
    if (save_alarms_to_file(alarms_json, ALARM_LIST_FILE_PATH) == 0) {
        ESP_LOGI(TAG, "Success on saving [%s] file.", ALARM_LIST_FILE_PATH);
    }
    free(alarms_json);

    if (count < 0) {
        ESP_LOGE(TAG, "Failed to parse JSON or generate alarms.");
        return;
    }

    qsort(alarms_epochs, count, sizeof(alarm_epoch_t), compare_alarm_epochs);

    for (int i = 0; i < count; ++i) {
        alarms_epochs[i].epoch = (alarms_epochs[i].epoch);
        struct tm *tm_info = localtime(&alarms_epochs[i].epoch);
        char buffer[64];
        strftime(buffer, sizeof(buffer), "%Y-%m-%d %H:%M:%S [%p]", tm_info);
        ESP_DRAM_LOGD(TAG, "Alarm %d -> %s --- Media: %s --- UTC time: %s ---- epoch: %llu", i + 1, buffer, alarms_epochs[i].media ? alarms_epochs[i].media : "(null)", asctime(tm_info), alarms_epochs[i].epoch);
    }
    for(int i = 0; i < count; ++i)
    {
        if(alarms_epochs[i].epoch > now)
        {
            alarms_epochs_idx = i;
            ESP_DRAM_LOGD(TAG, "now: %llu --- diff: %llu", now, (alarms_epochs[alarms_epochs_idx].epoch - now));

            if(alarm_timer_src == TIMER_SOURCE_DEEP_SLEEP)
            {
                last_alarm = alarms_epochs[alarms_epochs_idx].epoch;
                esp_sleep_enable_timer_wakeup((alarms_epochs[alarms_epochs_idx].epoch - now) * ALARM_US_SCALE);
                power_off_alarm_opt = get_alarm_option(alarms_epochs[alarms_epochs_idx].media);
            }
            else
            {
                set_alarm_interval(alarms_epochs[alarms_epochs_idx].epoch - now);
            }
            break;
        }
    }
}

esp_err_t get_alarm_setting(alarm_timer_src_t alarm_timer_src)
{
    if ( g_init_sdcard != ESP_OK ) {
        ESP_LOGW(TAG, "SD card not initialized, cannot save tracking records.");
        return ESP_FAIL;
    }

    const char *json_str = read_file_to_spiram(ALARM_LIST_FILE_PATH);
    if(!json_str)
    {
        free(json_str);
        return ESP_FAIL;
    }
    start_alarm_list(json_str, alarm_timer_src);
    if(strcmp(json_str, "Invalid JSON content.") != 0)
    {
        free(json_str);
    }

    return ESP_OK;
}

// ALARM TIMER RELATED ==================================================================
void stop_alarm_timer(void)
{
    esp_timer_stop(s_alarm_timer);
    ESP_DRAM_LOGD(TAG, "Alarm timer stoped");
}

void set_alarm_interval(uint64_t new_interval_in_seconds)
{
    esp_err_t err = esp_timer_start_periodic(s_alarm_timer, (new_interval_in_seconds * ALARM_US_SCALE)); 
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed to start timer - err: %s", esp_err_to_name(err));
    }
}

esp_err_t init_alarm_timer(void)
{
    const esp_timer_create_args_t timer_args = {
        .callback = &alarm_cb,
        .arg = NULL,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "alarm_sec_timer"
    };

    if (esp_timer_create(&timer_args, &s_alarm_timer) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create esp_timer");
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "Alarm started");
    return ESP_OK;
}

// ALARM TIMEOUT TIMER RELATED ==================================================================
void alarm_timeout_cb(void)
{
    ESP_LOGI(TAG, "Alarm timeout cb - auto-dismissing alarm");
    app_state_handle_event(EVENT_ALARM_AUTO_DISMISS);
}

void stop_alarm_timeout_timer(void)
{
    if (s_alarm_timout_timer)
    {
        esp_timer_stop(s_alarm_timout_timer);
        esp_timer_delete(s_alarm_timout_timer);
        s_alarm_timout_timer = NULL;
    }
    ESP_DRAM_LOGD(TAG, "Alarm timeout timer stoped");
}

esp_err_t init_alarm_timeout_timer(void)
{
    const esp_timer_create_args_t timer_args = {
        .callback = &alarm_timeout_cb,
        .arg = NULL,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "tm_alarm_timer"
    };

    if (esp_timer_create(&timer_args, &s_alarm_timout_timer) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create esp_timer");
        return ESP_FAIL;
    }

    // Use esp_timer_start_once() instead of esp_timer_start_periodic()
    // This timer should fire only ONCE per alarm to auto-dismiss after ALARM_TIMEOUT_SECONDS
    // Using periodic would cause it to fire every 10s indefinitely even after alarm dismissal
    esp_err_t err = esp_timer_start_once(s_alarm_timout_timer, (ALARM_TIMEOUT_SECONDS * ALARM_US_SCALE)); 
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed to start timer - err: %s", esp_err_to_name(err));
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Alarm timeout started (one-shot, %d seconds / %.1f minutes)", ALARM_TIMEOUT_SECONDS, ALARM_TIMEOUT_SECONDS / 60.0);
    return ESP_OK;
}
