//
// Created by Shane_Hwang on 2025/6/23.
//

#ifndef S3_SYNC_ACCOUNT_CONTENTS
#define S3_SYNC_ACCOUNT_CONTENTS

#define SDCARD_CONTENT_PATH "/sdcard/content/full/%s/"
#define SDCARD_CONTENT_FULLNAME "/sdcard/content/full/%s/%s"

#define SDCARD_ALARMS_PATH "/sdcard/alarms/"
#define SDCARD_ALARMS_FULLNAME "/sdcard/alarms/%s"

#define SDCARD_COVER_PATH "/sdcard/cover/device/"

#define MAX_DOWNLOAD_ATTEMPTS 3

typedef struct  {
    char *skuId;
    char *language;     // Language from JSON (e.g., "en-us", "zh-tw")
    int contentCount;
    unsigned int expiresAt; // timestamp (e.g. 1774145903, 2026年03月22日10點18分23秒 (+08:00 CST))
}s3_babyPack_t;

// Alarm
typedef enum {
    Days_Monday = 0,
    Days_Tuesday,
    Days_Wednesday,
    Days_Thursday,
    Days_Friday,
    Days_Saturday,
    Days_Sunday,
    Days_Size,
} s3_days_enum;
static const char *s3_days_array[Days_Size] = {"Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday", "Sunday"};

typedef struct  {
    char *time;
    char *period;
    char days[Days_Size];
    char *filename;
}s3_alarm_t;

// NFC
typedef  struct  {
    char *skuId;
    char *language;
    int contentCount;  // Number of content files for this SKU
    unsigned int expiresAt; // timestamp (e.g. 1774145903, 2026年03月22日10點18分23秒 (+08:00 CST))
} s3_nfc_skus_t;

typedef struct  {
    char *sn;
    char *linked;
    int skusCount;
    s3_nfc_skus_t *skus;
}s3_nfc_t;

esp_err_t parser_account_contents(int justPaserContent);

void get_babyPacks(const s3_babyPack_t **babyPack, int *conunt);

void get_alarms(const s3_alarm_t **alarm, int *conunt);

void get_nfcs(const s3_nfc_t **nfc, int *conunt);

// Macros for haveNFC return values
// Normal NFC: linked field is empty ("") - enables NFC menu
// Blankee NFC: linked field has UUID - does not enable NFC menu
#define HAS_NFC_ENABLED     1  // At least one normal (empty linked) NFC is available
#define HAS_NFC_DISABLED    0  // No normal NFCs available (only blankee or none)

int haveNFC(void);

esp_err_t parser_account_kids(char *kid);

esp_err_t parser_fw_contents_without_mp3(void);

void read_resource_version_or_default(char *out_buf, size_t buf_size);

void write_resource_version_to_file(char *version_str);

esp_err_t sync_resource_without_mp3(char *url,int cnt);

// Connection reuse cleanup function
void cleanup_sync_connection_reuse(void);

// Pure network speed test function - downloads data without storing to isolate bottlenecks
esp_err_t test_pure_download_speed(char *url, int test_duration_seconds);

// Filename to ContentId mapping functions
const char* GetContentId(const char* filename);
void free_filename_contentid_map(void);

// SPIRAM-aware string duplication to avoid internal RAM exhaustion
char* strdup_spiram(const char* str);

#endif // S3_SYNC_CONTENTS_H
