#include "s3_tracking.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

// Internal state for the tracking module
static TrackingRecord *records = NULL;
static int record_count = 0;
static int record_capacity = 0;

// Initial capacity for the dynamic array
#define INITIAL_CAPACITY 10

#include <time.h>
#include <sys/stat.h>
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "cJSON.h"
#include "s3_definitions.h"

extern esp_err_t g_init_sdcard;
extern char s3_babyId[32];
#define TAG "s3_tracking"

static void save_records_to_sdcard()
{
    if (record_count == 0) {
        return; // No records to save
    }

    ESP_LOGI(TAG, "Saving %d records to SD card", record_count);
    // Create /sdcard/tmp directory if it doesn't exist
    struct stat st = {0};
    if (stat("/sdcard/tmp", &st) == -1) {
        mkdir("/sdcard/tmp", 0700);
    }

    // We will write all records for a given day to the same file.
    // The filename is based on the date of the first record.
    char filepath[64];
    struct tm timeinfo;
    time_t first_record_time = (time_t)records[0].start;
    localtime_r(&first_record_time, &timeinfo);
    char date_str[9];
    strftime(date_str, sizeof(date_str), "%Y%m%d", &timeinfo);
    snprintf(filepath, sizeof(filepath), "/sdcard/tmp/tracking_%s.bin", date_str);

    FILE *f = fopen(filepath, "ab"); // Append Binary
    if (f == NULL) {
        ESP_LOGE(TAG, "Failed to open file for writing: %s", filepath);
        return; // Can't save any records if file fails to open
    }

    for (int i = 0; i < record_count; i++) {
        // 1. Write contentId length
        uint16_t len = strlen(records[i].contentId);
        if (fwrite(&len, sizeof(uint16_t), 1, f) != 1) {
            ESP_LOGE(TAG, "Failed to write contentId length");
            break; // Stop if write fails
        }

        // 2. Write contentId string
        if (fwrite(records[i].contentId, sizeof(char), len, f) != len) {
            ESP_LOGE(TAG, "Failed to write contentId string");
            break; // Stop if write fails
        }

        // 3. Write the rest of the record
        if (fwrite(&records[i].start, sizeof(time_t), 1, f) != 1) {
            ESP_LOGE(TAG, "Failed to write start time");
            break; // Stop if write fails
        }
        if (fwrite(&records[i].end, sizeof(time_t), 1, f) != 1) {
            ESP_LOGE(TAG, "Failed to write end time");
            break; // Stop if write fails
        }
        if (fwrite(&records[i].isFullPlay, sizeof(int), 1, f) != 1) {
            ESP_LOGE(TAG, "Failed to write isFullPlay flag");
            break; // Stop if write fails
        }
    }

    fclose(f);

    ESP_LOGI(TAG, "Finished saving records.");
    // Clean up records after saving
    s3_tracking_cleanup();
}

void s3_tracking_save_now(void)
{
    if ( g_init_sdcard != ESP_OK ) {
        ESP_LOGW(TAG, "SD card not initialized, cannot save tracking records.");
        return;
    }
    ESP_LOGI(TAG, "Forcing save of tracking records before sleep.");
    save_records_to_sdcard();
}

int s3_tracking_add_record(const char *contentId, time_t start, time_t end, int isFullPlay)
{
    if (start < 1735689600) {
        ESP_LOGE(TAG, "start_time < 2025/01/01 00:08:00 (TWD)");
        return -1;
    }

    if (contentId && strncmp(contentId, "SORC", 4) == 0) {
        ESP_LOGE(TAG, "contentId starts with 'SORC'");
        return -1;
    }

    if (strlen(s3_babyId) == 0) {
        ESP_LOGE(TAG, "s3_babyId == NULL'");
        return -1;
    }

    // If the list is full, resize the dynamic array
    if (record_count >= record_capacity) {
        int new_capacity = (record_capacity == 0) ? INITIAL_CAPACITY : record_capacity + INITIAL_CAPACITY;
        TrackingRecord *new_records = heap_caps_realloc(records, new_capacity * sizeof(TrackingRecord), MALLOC_CAP_SPIRAM);

        if (new_records == NULL) {
            // Memory allocation failed
            ESP_LOGE(TAG, "Failed to reallocate memory for tracking records");
            return -1;
        }
        records = new_records;
        record_capacity = new_capacity;
    }

    // Duplicate the contentId string to prevent issues with caller's memory management
    char *contentId_copy = strdup_spiram(contentId);
    if (contentId_copy == NULL) {
        ESP_LOGE(TAG, "Failed to duplicate contentId string");
        return -1;
    }

    // Add the new record to the list
    records[record_count].contentId = contentId_copy;
    records[record_count].start = start;
    records[record_count].end = end;
    records[record_count].isFullPlay = isFullPlay;
    record_count++;

    return 0;
}

const TrackingRecord* s3_tracking_get_records(int *count) {
    if (count != NULL) {
        *count = record_count;
    }
    return records;
}

void s3_tracking_cleanup(void) {
    if (records != NULL) {
        // Free each duplicated contentId string
        for (int i = 0; i < record_count; i++) {
            // The const_cast is necessary here to free the allocated string.
            free((void*)records[i].contentId);
        }
        // Free the main records array
        free(records);
    }

    // Reset the state
    records = NULL;
    record_count = 0;
    record_capacity = 0;
}

int s3_tracking_load_records_from_file(const char *filepath, TrackingRecord **out_records, int *out_count)
{
    FILE *f = fopen(filepath, "rb"); // Read Binary
    if (f == NULL) {
        ESP_LOGE(TAG, "File not found or failed to open for reading: %s", filepath);
        *out_records = NULL;
        *out_count = 0;
        return -1;
    }

    int capacity = 10;
    int count = 0;
    TrackingRecord *records_ptr = malloc(capacity * sizeof(TrackingRecord));
    if (records_ptr == NULL) {
        ESP_LOGE(TAG, "Failed to allocate initial memory for records");
        fclose(f);
        return -1;
    }

    while (1) {
        if (count >= capacity) {
            capacity *= 2;
            TrackingRecord *new_records = heap_caps_realloc(records_ptr, capacity * sizeof(TrackingRecord), MALLOC_CAP_SPIRAM);
            if (new_records == NULL) {
                ESP_LOGE(TAG, "Failed to reallocate memory for records");
                s3_tracking_free_loaded_records(records_ptr, count);
                fclose(f);
                return -1;
            }
            records_ptr = new_records;
        }

        // 1. Read contentId length
        uint16_t len;
        if (fread(&len, sizeof(uint16_t), 1, f) != 1) {
            if (feof(f)) break; // End of file is expected
            ESP_LOGE(TAG, "Failed to read contentId length");
            s3_tracking_free_loaded_records(records_ptr, count);
            fclose(f);
            return -1;
        }

        // 2. Read contentId string
        char *contentId_buf = malloc(len + 1);
        if (contentId_buf == NULL) {
            ESP_LOGE(TAG, "Failed to allocate memory for contentId");
            s3_tracking_free_loaded_records(records_ptr, count);
            fclose(f);
            return -1;
        }
        if (fread(contentId_buf, sizeof(char), len, f) != len) {
            ESP_LOGE(TAG, "Failed to read contentId string");
            free(contentId_buf);
            s3_tracking_free_loaded_records(records_ptr, count);
            fclose(f);
            return -1;
        }
        contentId_buf[len] = '\0'; // Null-terminate the string

        records_ptr[count].contentId = strdup_spiram(contentId_buf);
        free(contentId_buf);
        if (records_ptr[count].contentId == NULL) {
            ESP_LOGE(TAG, "Failed to duplicate contentId string into SPIRAM");
            s3_tracking_free_loaded_records(records_ptr, count);
            fclose(f);
            return -1;
        }

        // 3. Read the rest of the record
        if (fread(&records_ptr[count].start, sizeof(time_t), 1, f) != 1 ||
            fread(&records_ptr[count].end, sizeof(time_t), 1, f) != 1 ||
            fread(&records_ptr[count].isFullPlay, sizeof(int), 1, f) != 1) {
            ESP_LOGE(TAG, "Failed to read record data");
            // Free the contentId we just allocated
            free((void*)records_ptr[count].contentId);
            s3_tracking_free_loaded_records(records_ptr, count);
            fclose(f);
            return -1;
        }

        // uint64_t tStart = records_ptr[count].start;
        // ESP_LOGI(TAG, "Value: %u%010u",
        //          (unsigned)(tStart / 10000000000ULL),  // 高位
        //          (unsigned)(tStart % 10000000000ULL)); // 低位
        //
        // uint64_t tEnd = records_ptr[count].end;
        // ESP_LOGI(TAG, "Value: %u%010u",
        //          (unsigned)(tEnd / 10000000000ULL),  // 高位
        //          (unsigned)(tEnd % 10000000000ULL)); // 低位

        ESP_LOGI(TAG, "Loaded record: %s, %d", records_ptr[count].contentId, records_ptr[count].isFullPlay);
        count++;
    }

    fclose(f);
    *out_records = records_ptr;
    *out_count = count;
    ESP_LOGI(TAG, "Loaded %d records from %s", count, filepath);
    return 0;
}

void s3_tracking_free_loaded_records(TrackingRecord *loaded_records, int count)
{
    if (loaded_records == NULL) {
        return;
    }
    for (int i = 0; i < count; i++) {
        free((void*)loaded_records[i].contentId);
    }
    free(loaded_records);
}

char *make_json_tracking_messages( TrackingRecord *records, int record_count) {
    // root object
    cJSON *root = cJSON_CreateObject();

//    time_t now = time(NULL);
//    struct tm *t = localtime(&now);
    struct tm *t = localtime(&records[0].start);
    char date_str[9];
    strftime(date_str, sizeof(date_str), "%Y%m%d", t);
    cJSON_AddStringToObject(root, "date", date_str);
    cJSON_AddStringToObject(root, "source", "device");

    // records object
    cJSON *records_obj = cJSON_CreateObject();
    cJSON *records_array = cJSON_CreateArray();

    for (int i = 0; i < record_count; i++) {
        cJSON *item = cJSON_CreateObject();
        cJSON_AddStringToObject(item, "contentId", records[i].contentId);
        cJSON_AddNumberToObject(item, "seconds", records[i].end - records[i].start);
        if (records[i].end - records[i].start > 300 || records[i].isFullPlay)
        	cJSON_AddNumberToObject(item, "points", 1);
        else
            cJSON_AddNumberToObject(item, "points", 0);
        cJSON_AddItemToArray(records_array, item);
    }

    cJSON_AddItemToObject(records_obj, s3_babyId, records_array);
    cJSON_AddItemToObject(root, "records", records_obj);

    // 輸出字串（記得要 free）
    char *json_str = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return json_str;
}

//esp_err_t s3_tracking_messages_format(const char *messages) {
//    TrackingRecord records[] = {
//        {"IST-00047", 1757399599, 1757399999, 0}, // 400 points 1
//        {"IST-00047", 1757399599, 1757399999, 1}, // 400 points 1
//        {"IST-00099", 1757399899, 1757399999, 0}, // 100 points 0
//        {"IST-00099", 1757399899, 1757399999, 1}  // 100 points 1
//    };
//
//    char *json_str = make_json_tracking_messages(records, 4);
//    printf("%s\n", json_str);
//    free(json_str);
//    return ESP_OK;
//}

