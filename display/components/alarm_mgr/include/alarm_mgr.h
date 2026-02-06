#ifndef ALARM_MANAGER_H
#define ALARM_MANAGER_H

#include <time.h>
#include "esp_err.h"

#define ALARM_LIST_LEN (51)

typedef struct {
    const char *id;
    const char *time_str;
    const char *period;
    const char **days;
    int days_count;
    const char *media;
    int is_active;
} schedule_t;

typedef struct {
    time_t epoch;
    char *media;
} alarm_epoch_t;

typedef enum {
    TIMER_SOURCE_ESP_TIMER,
    TIMER_SOURCE_DEEP_SLEEP,
} alarm_timer_src_t;

void stop_alarm_timer(void);
void set_alarm_interval(uint64_t new_interval_in_seconds);
void register_alarms(const char *full_json_text);

void start_alarm_list(const char *full_json_text, alarm_timer_src_t alarm_timer_src);
esp_err_t get_alarm_setting(alarm_timer_src_t alarm_timer_src);
esp_err_t clear_alarm_file_content(void);

esp_err_t init_alarm_timer(void);
void stop_alarm_timeout_timer(void);

void alarm_from_deep_sleep(void);

#endif
