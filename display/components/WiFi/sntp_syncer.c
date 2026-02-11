
#include <time.h>
#include <string.h>

#include "sntp_syncer.h"
#include "sntp.h"
#include "esp_sntp.h"

#include "esp_log.h"
#include "esp_netif_sntp.h"
#include "s3_definitions.h"

#define SNTP_SYNC_TIMEOUT (5 * 1000)
#define STRFTIME_LEN (64)
#define MAX_RETRIES (30)

static const char *TAG = "SNTP";
static char timezone[TIMEZONE_STR_SIZE] = "";

static void apply_timezone(void)
{
    if (strlen(timezone) != 0)
    {
        setenv("TZ", timezone, 1);
        tzset();
    }
    else
    {
        setenv("TZ", "UTC", 0);
        tzset();
    }
}

void print_time(void)
{
    time_t now;
    struct tm timeinfo;
    char strftime_buf[STRFTIME_LEN];

    apply_timezone();
    time(&now);

    localtime_r(&now, &timeinfo);
    strftime(strftime_buf, sizeof(strftime_buf), "%c", &timeinfo);
    ESP_LOGI(TAG, "Current time: %s", strftime_buf);
}

void get_current_time(time_t *now, struct tm *timeinfo)
{
    apply_timezone();
    time(now);
    localtime_r(now, timeinfo);
}

void get_system_epoch(time_t *now)
{
    time(now);
}

void set_timezone(const char *timezone_str)
{
    strncpy(timezone, timezone_str, TIMEZONE_STR_SIZE - 1);
    timezone[TIMEZONE_STR_SIZE - 1] = '\0';
}

void sync_time_from_sntp(void)
{
    esp_netif_sntp_start();
    esp_err_t error = esp_netif_sntp_sync_wait(pdMS_TO_TICKS(SNTP_SYNC_TIMEOUT));
    if (error == ESP_OK)
    {
        ESP_LOGI(TAG, "Time synchronized");
        print_time();
    }
    else if(error == ESP_ERR_TIMEOUT)
    {
        ESP_LOGE(TAG, "Timeout on SNTP sync");
    }
    else if(error == ESP_ERR_NOT_FINISHED)
    {
        ESP_LOGE(TAG, "Error on time syncing");
    }
}

void sntp_sync_cb(struct timeval *tv)
{
    ESP_LOGI(TAG, "Time synchronized");
    print_time();
}

void init_sntp(const char *timezone)
{
    esp_sntp_config_t config = ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
    config.start = true;
    config.sync_cb = &sntp_sync_cb;
    config.server_from_dhcp = false;

    esp_netif_sntp_init(&config);
    set_timezone(timezone);
}

void deinit_sntp(void)
{
    esp_netif_sntp_deinit();
}

esp_err_t wait_for_time_sync(void)
{
    int retries = 0;
    sntp_sync_status_t sync_status = sntp_get_sync_status();

    const int retry_count = 15;
    while (esp_netif_sntp_sync_wait(2000 / portTICK_PERIOD_MS) == ESP_ERR_TIMEOUT && ++retries < retry_count) {
        ESP_LOGI(TAG, "Waiting for system time to be set... (%d/%d)", retries, retry_count);
    }

    if (retries == retry_count)
        return ESP_ERR_TIMEOUT;
    return ESP_OK;
}
