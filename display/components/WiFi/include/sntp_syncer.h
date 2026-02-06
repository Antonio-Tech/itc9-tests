#ifndef SNTP_SYNCER_H
#define SNTP_SYNCER_H

#include "esp_err.h"
#include <time.h>
#include <esp_err.h>
// #define TIMEZONE_STR_LEN (10)

void init_sntp(const char *timezone);
void deinit_sntp(void);
void sync_time_from_sntp(void);
void set_timezone(const char *timezone_str);
void get_system_epoch(time_t *now);
void get_current_time(time_t *now, struct tm *timeinfo);
esp_err_t wait_for_time_sync(void);

#endif
