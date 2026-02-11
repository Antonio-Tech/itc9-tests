#ifndef S3_TRACKING_H
#define S3_TRACKING_H

#include <time.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Structure to hold a single tracking record.
 */
typedef struct {
    const char *contentId;  /*!< The unique identifier for the content */
    time_t start;           /*!< The playback start time */
    time_t end;             /*!< The playback end time */
    int isFullPlay;         /*!< Flag indicating if the content was played completely */
} TrackingRecord;

/**
 * @brief Adds a new tracking record to the dynamic list.
 *
 * This function creates a copy of the contentId string.
 *
 * @param contentId The unique identifier for the content.
 * @param start The playback start time.
 * @param end The playback end time.
 * @param isFullPlay Flag indicating if the content was played completely (1 for true, 0 for false).
 * @return 0 on success, -1 on memory allocation failure.
 */
int s3_tracking_add_record(const char *contentId, time_t start, time_t end, int isFullPlay);

/**
 * @brief Retrieves the list of all tracking records.
 *
 * @param[out] count Pointer to an integer that will be filled with the number of records.
 * @return A pointer to the array of TrackingRecord structures. The caller should not modify this.
 */
const TrackingRecord* s3_tracking_get_records(int *count);

/**
 * @brief Cleans up all allocated resources for the tracking module.
 *
 * This function frees the memory used by the records list and all the copied contentId strings.
 */
void s3_tracking_cleanup(void);

/**
 * @brief Loads tracking records from a file into a dynamically allocated array.
 *
 * The caller is responsible for freeing the allocated memory by calling
 * s3_tracking_free_loaded_records().
 *
 * @param filepath The path to the file to load.
 * @param[out] out_records A pointer to a TrackingRecord pointer that will be allocated.
 * @param[out] out_count A pointer to an integer that will be filled with the number of loaded records.
 * @return 0 on success, -1 on failure.
 */
int s3_tracking_load_records_from_file(const char *filepath, TrackingRecord **out_records, int *out_count);

/**
 * @brief Frees the memory allocated by s3_tracking_load_records_from_file().
 *
 * @param loaded_records The array of records to free.
 * @param count The number of records in the array.
 */
void s3_tracking_free_loaded_records(TrackingRecord *loaded_records, int count);

/**
 * @brief Immediately saves any pending tracking records to the SD card.
 *
 * This function is intended to be called before the device enters a state
 * where in-memory data might be lost, such as deep sleep.
 */
void s3_tracking_save_now(void);

/*
 *  upload format : https://redmine.pixseecare.com:8081/issues/15143#note-4
 */

char *make_json_tracking_messages(TrackingRecord *records, int record_count);

#ifdef __cplusplus
}
#endif

#endif // S3_TRACKING_H
