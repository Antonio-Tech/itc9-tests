#ifndef S3_NFC_HANDLER_H
#define S3_NFC_HANDLER_H

#include <stdint.h>
#include <stdbool.h>
#include "s3_definitions.h"

/**
 * @brief NFC processing result states
 */
typedef enum {
    NFC_RESULT_ACTIVATION_REQUIRED, /**< NFC tag requires activation (blank NFC, invalid UID, etc.) */
    NFC_RESULT_PLAY_NOW,            /**< Album found, downloaded and NFC-available - start playback */
    NFC_RESULT_NO_CONTENT,          /**< NFC tag has no content ("none" SKU) - show no content screen */
    NFC_RESULT_NOT_DOWNLOADED,      /**< Album found and authorized but not downloaded - content unavailable */
    NFC_RESULT_NO_MEMORY,           /**< Critical heap allocation failure */
    NFC_RESULT_EXPIRED              /**< Album found but expired - show no content screen */
} nfc_result_t;

bool is_on_blankee(void);
void rst_is_on_blankee_flg(void);

/**
 * @brief Handle NFC input and set appropriate playlist directory
 * @param sku_code The SKU code detected from NFC (e.g., "SKU-00007" or "enfc,SKU-00001,SKU-000002")
 * @param uid The NFC tag UID (7 bytes)
 * @return nfc_result_t indicating the required action
 */
nfc_result_t handle_nfc_input(const char* sku_code, const uint8_t* uid);

void start_nfc(void);
void stop_nfc(void);

/**
 * @brief Start NFC sync task with specified context
 * @param sku_detected The SKU that was detected
 * @param uid The NFC tag UID (7 bytes)
 * @param context The sync context (CONTENT_UPDATE or ACTIVATION_CHECK)
 */
void start_nfc_sync_task(const char* sku_detected, const uint8_t* uid, int context);

#endif // S3_NFC_HANDLER_H
