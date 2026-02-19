/**
 * @file s3_nfc_handler.c
 * @brief Simplified NFC tag handling using cached album flags
 *
 * This module handles NFC tag detection and album selection using high-quality
 * cached flags maintained by s3_album_mgr:
 * - is_downloaded: Set by comprehensive filesystem validation (files, covers, MP3 count)
 * - is_available_nfc: Set by parsing cloud NFC data (only authorized SKUs)
 *
 * All validation quality is preserved in the album manager's flag-setting logic.
 * NFC handler now has simple, fast flag-based decisions.
 */

#include "s3_nfc_handler.h"
#include "esp_log.h"
#include "esp_err.h"
#include <string.h>
#include <stdlib.h>
#include <time.h>
#include "s3_definitions.h"
#include "s3_sync_account_contents.h"
#include "s3_album_mgr.h"
#include "audio_player.h"
#include "lv_screen_mgr.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "nfc-service.h"
#include "WiFi.h"

#define NFC_UID_LEN (7)
#define NFC_QUEUE_SIZE (1)

// NFC activation data storage for callback
static char nfc_activation_sku[32] = {0};
static uint8_t nfc_activation_uid[7] = {0};

static QueueHandle_t nfc_event_queue = NULL;
static TaskHandle_t nfc_worker_task_handle = NULL;
static uint8_t nfc_processing = false;  // Flag to prevent duplicate NFC tag processing
static volatile bool nfc_worker_should_exit = false;  // Flag to signal worker task to exit

static int last_uid_len = 0;
static uint32_t last_detect_time = 0;
static bool m_is_on_blankee = false;

// Forward declarations
static void uid_to_string(const uint8_t* uid, char* uid_str);
static bool validate_enfc_nfc_uid(const uint8_t* uid);
static const char* check_special_album_by_uid(const uint8_t* uid);
static bool is_sku_expired(const char* sku);
static bool check_nfc_uid_in_account(const uint8_t* uid);
static bool check_nfc_has_content(const char* sku_code, const uint8_t* uid, bool is_blankee);
static bool check_nfc_content_downloaded(const char* sku_code, bool is_blankee);

static const char *TAG = "NFC_HANDLER";

/**
 * @brief Check if a SKU has expired by checking baby pack and NFC data
 * @param sku The SKU to check
 * @return true if expired, false otherwise
 */
static bool is_sku_expired(const char* sku)
{
    if (!sku) {
        return false;
    }

    time_t now = time(NULL);
    if (now == (time_t)-1) {
        ESP_LOGW(TAG, "Failed to get current time, skipping expiration check");
        return false;
    }

    // Check baby packs
    const s3_babyPack_t *babyPacks = NULL;
    int babyPackCount = 0;
    get_babyPacks(&babyPacks, &babyPackCount);

    if (babyPacks && babyPackCount > 0) {
        for (int i = 0; i < babyPackCount; i++) {
            if (babyPacks[i].skuId && strcmp(babyPacks[i].skuId, sku) == 0) {
                if (babyPacks[i].expiresAt != 0 && babyPacks[i].expiresAt < (unsigned int)now) {
                    ESP_LOGE(TAG, "Baby pack SKU %s expired (expiresAt: %u, now: %u)",
                             sku, babyPacks[i].expiresAt, (unsigned int)now);
                    return true;
                }
                // Found matching SKU and it's not expired
                return false;
            }
        }
    }

    // Check NFC data
    const s3_nfc_t *nfcs = NULL;
    int nfcCount = 0;
    get_nfcs(&nfcs, &nfcCount);

    if (nfcs && nfcCount > 0) {
        for (int i = 0; i < nfcCount; i++) {
            if (nfcs[i].skus && nfcs[i].skusCount > 0) {
                for (int j = 0; j < nfcs[i].skusCount; j++) {
                    if (nfcs[i].skus[j].skuId && strcmp(nfcs[i].skus[j].skuId, sku) == 0) {
                        if (nfcs[i].skus[j].expiresAt != 0 && nfcs[i].skus[j].expiresAt < (unsigned int)now) {
                            ESP_LOGE(TAG, "NFC SKU %s expired (expiresAt: %u, now: %u)",
                                     sku, nfcs[i].skus[j].expiresAt, (unsigned int)now);
                            return true;
                        }
                        // Found matching SKU and it's not expired
                        return false;
                    }
                }
            }
        }
    }

    // SKU not found in baby packs or NFC data, assume not expired
    ESP_LOGD(TAG, "SKU %s not found in baby packs or NFC data, assuming not expired", sku);
    return false;
}

bool is_on_blankee(void)
{
    return m_is_on_blankee;
}

void rst_is_on_blankee_flg(void)
{
    m_is_on_blankee = false;
}

/**
 * @brief Convert NFC UID to string format for comparison
 * @param uid The 7-byte UID array
 * @param uid_str Buffer to store the UID string (must be at least 15 chars)
 */
static void uid_to_string(const uint8_t* uid, char* uid_str)
{
    snprintf(uid_str, 15, "%02X%02X%02X%02X%02X%02X%02X",
             uid[0], uid[1], uid[2], uid[3], uid[4], uid[5], uid[6]);
}

/**
 * @brief Validate enfc NFC by checking UID match with account NFCs
 * @param uid The NFC tag UID
 * @return true if UID matches any account NFC sn, false otherwise
 */
static bool validate_enfc_nfc_uid(const uint8_t* uid)
{
    const s3_nfc_t *nfcs = NULL;
    int nfc_count = 0;

    get_nfcs(&nfcs, &nfc_count);
    if (nfcs == NULL || nfc_count == 0) {
        ESP_LOGW(TAG, "No NFCs found in account data for enfc validation");
        return false;
    }

    char uid_str[15];
    uid_to_string(uid, uid_str);
    ESP_LOGI(TAG, "Validating enfc NFC UID: %s", uid_str);

    for (int i = 0; i < nfc_count; i++) {
        if (nfcs[i].sn != NULL && strcmp(nfcs[i].sn, uid_str) == 0) {
            ESP_LOGI(TAG, "enfc NFC UID %s matches account sn", uid_str);
            return true;
        }
    }

    ESP_LOGW(TAG, "enfc NFC UID %s not found in account NFCs", uid_str);
    return false;
}

/**
 * @brief Step 1: Check if the account owns this NFC (by UID for Blankee, by SKU for Normal NFC)
 * @param uid The NFC tag UID
 * @param sku_code The SKU code from NFC tag
 * @param is_blankee Whether this is a Blankee NFC
 * @return true if account owns this NFC, false otherwise
 */
static bool check_nfc_uid_in_account(const uint8_t* uid)
{
    if (!uid) {
        return false;
    }

    const s3_nfc_t *nfcs = NULL;
    int nfc_count = 0;
    get_nfcs(&nfcs, &nfc_count);

    if (nfcs == NULL || nfc_count == 0) {
        ESP_LOGW(TAG, "No NFCs found in account data");
        return false;
    }

    char uid_str[15];
    uid_to_string(uid, uid_str);
    ESP_LOGI(TAG, "Step 1: Checking if UID %s is in account", uid_str);

    for (int i = 0; i < nfc_count; i++) {
        if (nfcs[i].sn != NULL && strcmp(nfcs[i].sn, uid_str) == 0) {
            ESP_LOGI(TAG, "UID %s found in account NFCs", uid_str);
            return true;
        }
    }

    ESP_LOGW(TAG, "UID %s not found in account NFCs", uid_str);
    return false;
}

/**
 * @brief Step 2: Check if the NFC has content (SKU exists and not expired)
 * @param sku_code The SKU code from NFC tag (can be comma-separated for Normal NFC)
 * @param uid The NFC tag UID
 * @param is_blankee Whether this is a Blankee NFC
 * @return true if NFC has content, false otherwise
 */
static bool check_nfc_has_content(const char* sku_code, const uint8_t* uid, bool is_blankee)
{
    if (!sku_code || strlen(sku_code) == 0) {
        ESP_LOGW(TAG, "Step 2: No SKU code provided");
        return false;
    }

    ESP_LOGI(TAG, "Step 2: Checking if NFC has content for SKU: %s", sku_code);

    // For Blankee: Check if UID has this SKU in account data
    if (is_blankee && uid) {
        // Check if SKU is expired
        if (is_sku_expired(sku_code)) {
            ESP_LOGE(TAG, "Step 2: SKU %s is expired", sku_code);
            return false;
        }

        const s3_nfc_t *nfcs = NULL;
        int nfc_count = 0;
        get_nfcs(&nfcs, &nfc_count);

        if (nfcs && nfc_count > 0) {
            char uid_str[15];
            uid_to_string(uid, uid_str);

            for (int i = 0; i < nfc_count; i++) {
                if (nfcs[i].sn != NULL && strcmp(nfcs[i].sn, uid_str) == 0) {
                    // Found matching UID, check if it has this SKU
                    if (nfcs[i].skus && nfcs[i].skusCount > 0) {
                        for (int j = 0; j < nfcs[i].skusCount; j++) {
                            if (nfcs[i].skus[j].skuId && strcmp(nfcs[i].skus[j].skuId, sku_code) == 0) {
                                // Check if this SKU is expired
                                if (nfcs[i].skus[j].expiresAt != 0) {
                                    time_t now = time(NULL);
                                    if (now != (time_t)-1 && nfcs[i].skus[j].expiresAt < (unsigned int)now) {
                                        ESP_LOGE(TAG, "Step 2: SKU %s is expired (expiresAt: %u, now: %u)",
                                                 sku_code, nfcs[i].skus[j].expiresAt, (unsigned int)now);
                                        return false;
                                    }
                                }
                                ESP_LOGI(TAG, "Step 2: Found valid SKU %s for UID %s in account", sku_code, uid_str);
                                return true;
                            }
                        }
                    }
                    ESP_LOGW(TAG, "Step 2: UID %s found but SKU %s not in its skus array", uid_str, sku_code);
                    return false;
                }
            }
        }
        ESP_LOGW(TAG, "Step 2: UID not found in account NFCs");
        return false;
    }

    // For Normal NFC: Check if any SKU in the comma-separated list exists and is not expired
    char* sku_check = strdup(sku_code);
    if (!sku_check) {
        ESP_LOGE(TAG, "Step 2: Failed to allocate memory for SKU check");
        return false;
    }

    char* token = strtok(sku_check, ",");
    bool has_content = false;

    while (token != NULL) {
        // Trim whitespace
        while (*token == ' ') token++;
        char* end = token + strlen(token) - 1;
        while (end > token && *end == ' ') *end-- = '\0';

        // Check if SKU is expired
        if (is_sku_expired(token)) {
            ESP_LOGD(TAG, "Step 2: SKU %s is expired, checking next", token);
            token = strtok(NULL, ",");
            continue;
        }

        // Check if SKU exists in dynamic albums
        size_t dynamic_count = get_dynamic_albums_size();
        for (size_t i = 0; i < dynamic_count; i++) {
            s3_album_handler_t *album = get_dynamic_album_by_index(i);
            if (album && album->sku && strcmp(album->sku, token) == 0) {
                ESP_LOGI(TAG, "Step 2: Found valid SKU %s in dynamic albums", token);
                has_content = true;
                break;
            }
        }

        if (has_content) {
            break;
        }

        // Also check baby packs
        const s3_babyPack_t *babyPacks = NULL;
        int babyPackCount = 0;
        get_babyPacks(&babyPacks, &babyPackCount);

        if (babyPacks && babyPackCount > 0) {
            for (int i = 0; i < babyPackCount; i++) {
                if (babyPacks[i].skuId && strcmp(babyPacks[i].skuId, token) == 0) {
                    ESP_LOGI(TAG, "Step 2: Found valid SKU %s in baby packs", token);
                    has_content = true;
                    break;
                }
            }
        }

        if (has_content) {
            break;
        }

        token = strtok(NULL, ",");
    }

    free(sku_check);

    if (!has_content) {
        ESP_LOGW(TAG, "Step 2: No valid content found for SKU list: %s", sku_code);
    }

    return has_content;
}

/**
 * @brief Step 3: Check if the NFC content is downloaded
 * @param sku_code The SKU code from NFC tag (can be comma-separated for Normal NFC)
 * @param is_blankee Whether this is a Blankee NFC
 * @return true if content is downloaded, false otherwise
 */
static bool check_nfc_content_downloaded(const char* sku_code, bool is_blankee)
{
    if (!sku_code || strlen(sku_code) == 0) {
        return false;
    }

    ESP_LOGI(TAG, "Step 3: Checking if content is downloaded for SKU: %s", sku_code);

    if (is_blankee) {
        // For Blankee: Check single SKU
        size_t dynamic_count = get_dynamic_albums_size();
        for (size_t i = 0; i < dynamic_count; i++) {
            s3_album_handler_t *album = get_dynamic_album_by_index(i);
            if (album && album->sku && strcmp(album->sku, sku_code) == 0) {
                ESP_LOGI(TAG, "Step 3: Found album for SKU %s, is_downloaded: %d", sku_code, album->is_downloaded);
                return album->is_downloaded;
            }
        }
        ESP_LOGW(TAG, "Step 3: Album for SKU %s not found in dynamic albums", sku_code);
        return false;
    } else {
        // For Normal NFC: Check if at least one SKU in the list is downloaded
        char* sku_check = strdup(sku_code);
        if (!sku_check) {
            ESP_LOGE(TAG, "Step 3: Failed to allocate memory for SKU check");
            return false;
        }

        char* token = strtok(sku_check, ",");
        bool has_downloaded = false;

        while (token != NULL) {
            // Trim whitespace
            while (*token == ' ') token++;
            char* end = token + strlen(token) - 1;
            while (end > token && *end == ' ') *end-- = '\0';

            // Find the album in dynamic albums
            size_t dynamic_count = get_dynamic_albums_size();
            for (size_t i = 0; i < dynamic_count; i++) {
                s3_album_handler_t *album = get_dynamic_album_by_index(i);
                if (album && album->sku && strcmp(album->sku, token) == 0) {
                    if (album->is_downloaded) {
                        ESP_LOGI(TAG, "Step 3: Found downloaded album for SKU %s", token);
                        has_downloaded = true;
                        break;
                    }
                }
            }

            if (has_downloaded) {
                break;
            }

            token = strtok(NULL, ",");
        }

        free(sku_check);

        if (!has_downloaded) {
            ESP_LOGW(TAG, "Step 3: No downloaded content found for SKU list: %s", sku_code);
        }

        return has_downloaded;
    }
}

// Special return values for check_special_album_by_uid
#define BLANKEE_NOT_FOUND       NULL        // UID not found in account -> activation required
#define BLANKEE_NO_CONTENT      "none"      // UID found but no SKUs -> no content

/**
 * @brief Check if UID has associated special albums (SKURC) in account data
 * @param uid The NFC tag UID
 * @return Album name if UID has SKURC albums, "none" if UID found but no content, NULL if UID not found
 */
static const char* check_special_album_by_uid(const uint8_t* uid)
{
    const s3_nfc_t *nfcs = NULL;
    int nfc_count = 0;

    get_nfcs(&nfcs, &nfc_count);
    if (nfcs == NULL || nfc_count == 0) {
        ESP_LOGW(TAG, "No NFCs found in account data for special album check");
        return BLANKEE_NOT_FOUND;
    }

    char uid_str[15];
    uid_to_string(uid, uid_str);
    ESP_LOGI(TAG, "Checking for special albums with UID: %s", uid_str);

    // Search for matching UID in account NFCs
    for (int i = 0; i < nfc_count; i++) {
        if (nfcs[i].sn != NULL && strcmp(nfcs[i].sn, uid_str) == 0) {
            ESP_LOGI(TAG, "Found matching UID %s in account NFCs (skusCount: %d)", uid_str, nfcs[i].skusCount);

            // Check if this NFC has any SKUs at all
            if (nfcs[i].skusCount == 0) {
                ESP_LOGI(TAG, "UID %s found but skus array is empty - no content available", uid_str);
                return BLANKEE_NO_CONTENT;
            }

            // Check if this NFC has SKURC albums
            for (int j = 0; j < nfcs[i].skusCount; j++) {
                if (nfcs[i].skus[j].skuId != NULL) {
                    // Check if SKU starts with "SKURC-" (custom recording)
                    if (strncmp(nfcs[i].skus[j].skuId, "SKURC-", 6) == 0) {
                        ESP_LOGI(TAG, "Found special album: %s for UID %s", nfcs[i].skus[j].skuId, uid_str);
                        // Return the SKU ID directly - we'll use it to find the album later
                        return nfcs[i].skus[j].skuId;
                    }
                }
            }

            ESP_LOGI(TAG, "UID %s found but no SKURC albums associated (has %d non-SKURC SKUs)", uid_str, nfcs[i].skusCount);
            return BLANKEE_NO_CONTENT;
        }
    }

    ESP_LOGI(TAG, "UID %s not found in account NFCs", uid_str);
    return BLANKEE_NOT_FOUND;
}

// All NFC authorization validation is now handled by album manager's
// is_available_nfc flag, which is set with the same quality as the
// original is_sku_in_account_nfcs() function by parsing cloud NFC data.

// REMOVED: select_language_preferred_sku function
// This function has been replaced by using s3_albums_find_by_sku_lang() in a simple loop.
// Multiple SKU handling is now done directly in handle_nfc_input() using the album manager.

/**
 * @brief Handle NFC input and set appropriate playlist directory
 * @param sku_code The SKU code detected from NFC (e.g., "SKU-00007", "SKU-00009,SKU-00010", or "enfc,SKU-00001,SKU-000002")
 * @param uid The NFC tag UID (7 bytes)
 * @return nfc_result_t indicating the required action
 */
nfc_result_t handle_nfc_input(const char* sku_code, const uint8_t* uid)
{
    ESP_LOGI(TAG, "=== NFC FLOW: handle_nfc_input(sku_code=%s) ===", sku_code);

    // Parse NFC format
    bool is_blankee = false;
    const char* actual_sku_code = sku_code;

    // Case 1: Normal SKU format ("SKU*") - easiest to detect
    if (sku_code && strlen(sku_code) > 0 &&
        (strncmp(sku_code, "SKU", 3) == 0 || strncmp(sku_code, "SKURC", 5) == 0)) {
        ESP_LOGI(TAG, "Case 1: Normal NFC format - processing with language matching: %s", sku_code);
        is_blankee = false;
    }
    // Case 2: Blankee format ("enfc,SKU*") - strip prefix and process
    else if (sku_code && strncmp(sku_code, "enfc,", 5) == 0) {
        const char* remaining_skus = sku_code + 5;

        // Check if there's actually a SKU after "enfc,"
        if (remaining_skus && strlen(remaining_skus) > 0) {
            ESP_LOGI(TAG, "Case 2: Blankee format - processing single SKU: %s", remaining_skus);
            actual_sku_code = remaining_skus;  // Strip "enfc," prefix and continue processing
            is_blankee = true;
        } else {
            ESP_LOGI(TAG, "Case 3: Invalid format (enfc,<empty>) - NO_CONTENT");
            return NFC_RESULT_NO_CONTENT;
        }
    }
    // Case 3: Everything else - NO_CONTENT
    else {
        ESP_LOGI(TAG, "Case 3: Invalid format (%s) - NO_CONTENT", sku_code ? sku_code : "NULL");
        return NFC_RESULT_NO_CONTENT;
    }

    m_is_on_blankee = is_blankee;

    // ========== NEW 3-STEP CHECK FLOW ==========

    // STEP 1: Check if account owns this NFC
    if (is_blankee) {
        // For Blankee: Check UID in account
        if (!check_nfc_uid_in_account(uid)) {
            ESP_LOGE(TAG, "Step 1 FAILED: UID not in account - returning ACTIVATION_REQUIRED");
            return NFC_RESULT_ACTIVATION_REQUIRED;
        }
    } else {
        // For Normal NFC: Check if any SKU in the list is in account
        // Parse comma-separated SKU list
        char* sku_check = strdup(sku_code);
        if (sku_check) {
            char* token = strtok(sku_check, ",");
            bool found_in_account = false;

            while (token != NULL) {
                // Trim whitespace
                while (*token == ' ') token++;
                char* end = token + strlen(token) - 1;
                while (end > token && *end == ' ') *end-- = '\0';

                // Check if SKU is in dynamic albums or baby packs
                size_t dynamic_count = get_dynamic_albums_size();
                for (size_t i = 0; i < dynamic_count; i++) {
                    s3_album_handler_t *album = get_dynamic_album_by_index(i);
                    if (album && album->sku && strcmp(album->sku, token) == 0) {
                        found_in_account = true;
                        break;
                    }
                }

                if (!found_in_account) {
                    const s3_babyPack_t *babyPacks = NULL;
                    int babyPackCount = 0;
                    get_babyPacks(&babyPacks, &babyPackCount);

                    if (babyPacks && babyPackCount > 0) {
                        for (int i = 0; i < babyPackCount; i++) {
                            if (babyPacks[i].skuId && strcmp(babyPacks[i].skuId, token) == 0) {
                                found_in_account = true;
                                break;
                            }
                        }
                    }
                }

                if (found_in_account) {
                    break;
                }

                token = strtok(NULL, ",");
            }

            free(sku_check);

            if (!found_in_account) {
                ESP_LOGE(TAG, "Step 1 FAILED: No SKU in account - returning ACTIVATION_REQUIRED");
                return NFC_RESULT_ACTIVATION_REQUIRED;
            }
        }
    }

    // STEP 2: Check if NFC has content
    if (!check_nfc_has_content(actual_sku_code, uid, is_blankee)) {
        ESP_LOGE(TAG, "Step 2 FAILED: NFC has no content - returning NO_CONTENT");
        return NFC_RESULT_NO_CONTENT;
    }

    // STEP 3: Check if content is downloaded
    if (!check_nfc_content_downloaded(actual_sku_code, is_blankee)) {
        ESP_LOGE(TAG, "Step 3 FAILED: Content not downloaded - returning NOT_DOWNLOADED");
        return NFC_RESULT_NOT_DOWNLOADED;
    }

    // All checks passed - proceed to find and play album
    ESP_LOGI(TAG, "All 3 steps passed - finding album for playback");

    int selected_album_index = NO_ALBUM_FOUND;
    char* all_skus[10] = {0}; // For "All" language support
    int all_skus_count = 0;

    if (is_blankee) {
        // Blankee path: Simple single SKU lookup (no language matching needed)
        ESP_LOGI(TAG, "Finding blankee album for SKU: %s", actual_sku_code);

        // Find the album by SKU in dynamic albums
        size_t dynamic_count = get_dynamic_albums_size();
        for (size_t i = 0; i < dynamic_count; i++) {
            s3_album_handler_t *candidate = get_dynamic_album_by_index(i);
            if (candidate && candidate->sku && strcmp(candidate->sku, actual_sku_code) == 0) {
                if (candidate->is_downloaded) {
                    ESP_LOGI(TAG, "Found playable blankee album: %s at index %u (language: %d)",
                             candidate->name, (unsigned int)i, candidate->language);
                    selected_album_index = (int)i;
                    break;
                }
            }
        }
    } else {
        // Normal NFC path: Language matching with comma-separated SKU list
        ESP_LOGI(TAG, "Finding normal NFC album for SKU list: %s", sku_code);

        // Parse comma-separated SKU list and find best language match
        char* sku_copy = strdup(sku_code);
        if (!sku_copy) {
            ESP_LOGE(TAG, "******************** CRITICAL HEAP MEMORY FAILURE ********************");
            ESP_LOGE(TAG, "** FAILED TO ALLOCATE MEMORY FOR SKU LIST - HEAP FRAGMENTATION?? **");
            ESP_LOGE(TAG, "******************** INVESTIGATE IMMEDIATELY ********************");
            return NFC_RESULT_NO_MEMORY;
        }

        char* token = strtok(sku_copy, ",");

        // For language-based selection with multiple SKUs
        int best_language_match = NO_ALBUM_FOUND;
        int fallback_match = NO_ALBUM_FOUND;

        ESP_LOGI(TAG, "Current system language: %d", s3_selected_language);

        while (token != NULL) {
            // Trim whitespace
            while (*token == ' ') token++;
            char* end = token + strlen(token) - 1;
            while (end > token && *end == ' ') *end-- = '\0';

            ESP_LOGI(TAG, "Checking SKU: '%s'", token);

            // Find the album by SKU in dynamic albums
            int album_index = NO_ALBUM_FOUND;
            size_t dynamic_count = get_dynamic_albums_size();
            for (size_t i = 0; i < dynamic_count; i++) {
                s3_album_handler_t *candidate = get_dynamic_album_by_index(i);
                if (candidate && candidate->sku && strcmp(candidate->sku, token) == 0) {
                    album_index = (int)i;
                    break;
                }
            }

            if (album_index != NO_ALBUM_FOUND) {
                s3_album_handler_t *album = get_dynamic_album_by_index(album_index);
                if (!album) {
                    ESP_LOGW(TAG, "Album at index %u not found", (unsigned int)album_index);
                    token = strtok(NULL, ",");
                    continue;
                }

                // All checks passed, so album must be downloaded
                if (album->is_downloaded) {
                    ESP_LOGI(TAG, "Found playable album for SKU: %s at global index %u (language: %d)",
                             token, (unsigned int)album_index, album->language);

                    // Check language preference - prioritize perfect language match
                    if (s3_selected_language == LANGUAGE_ALL) {
                        // For "All" language, collect all available SKUs for combined playlist
                        ESP_LOGI(TAG, "[DYNAMIC_LANG] LANGUAGE_ALL: Found album %s (language: %d)", token, album->language);

                        // Add this SKU to the list if not already added and we have space
                        bool already_added = false;
                        for (int i = 0; i < all_skus_count; i++) {
                            if (strcmp(all_skus[i], token) == 0) {
                                already_added = true;
                                break;
                            }
                        }

                        if (!already_added && all_skus_count < 10) {
                            all_skus[all_skus_count] = strdup(token);
                            all_skus_count++;
                            ESP_LOGI(TAG, "[DYNAMIC_LANG] LANGUAGE_ALL: Added SKU %s to combined playlist (count: %d)", token, all_skus_count);
                        }

                        // Use first album as base reference
                        if (best_language_match == NO_ALBUM_FOUND) {
                            best_language_match = album_index;
                            ESP_LOGI(TAG, "[DYNAMIC_LANG] LANGUAGE_ALL: Using album %s as base reference", token);
                        }
                    } else if (album->language == s3_selected_language) {
                        ESP_LOGI(TAG, "[DYNAMIC_LANG] Perfect language match found: %s (language: %d)", token, album->language);
                        best_language_match = album_index;
                        // Found perfect match, no need to check others
                        break;
                    } else if (fallback_match == NO_ALBUM_FOUND) {
                        // Keep first valid album as fallback
                        fallback_match = album_index;
                        ESP_LOGI(TAG, "[DYNAMIC_LANG] Fallback album set to: %s (language: %d)", token, album->language);
                    }
                }
            }

            token = strtok(NULL, ",");
        }

        // Select the best album based on language preference
        if (best_language_match != NO_ALBUM_FOUND) {
            selected_album_index = best_language_match;
            ESP_LOGI(TAG, "Using language-matched album at global index %d", selected_album_index);
        } else if (fallback_match != NO_ALBUM_FOUND) {
            selected_album_index = fallback_match;
            ESP_LOGW(TAG, "No language match found, using fallback album at global index %d", selected_album_index);
        }

        free(sku_copy);
    }

    // Find and prepare album for playback
    if (selected_album_index != NO_ALBUM_FOUND) {
        s3_album_handler_t* selected_album = get_dynamic_album_by_index((size_t)selected_album_index);
        if (!selected_album) {
            ESP_LOGE(TAG, "Selected album at index %d not found", selected_album_index);
            // Cleanup allocated SKU strings
            for (int i = 0; i < all_skus_count; i++) {
                free(all_skus[i]);
            }
            return NFC_RESULT_ACTIVATION_REQUIRED;
        }

        ESP_LOGI(TAG, "Preparing album for playback: %s (global index %d)", selected_album->name, selected_album_index);

        // IMPORTANT: Switch to the selected album BEFORE calling any audio functions
        // This ensures s3_current_album is correct when audio_play_internal() checks for encryption
        play_stop();
        s3_current_album = selected_album;
        s3_current_idx = 0;  // Reset album index
        s3_current_idx_track = 0;  // Reset to first track
        s3_current_size = 1; // Single album context

        ESP_LOGI(TAG, "NFC: Set s3_current_album to %s (SKU: %s)", selected_album->name, selected_album->sku);

        // Build playlist: use combined playlist for "All" language, regular for specific language
        if (s3_selected_language == LANGUAGE_ALL && all_skus_count > 0) {
            ESP_LOGI(TAG, "Building combined playlist for LANGUAGE_ALL with %d SKUs", all_skus_count);
            build_playlist_all_languages(all_skus, all_skus_count);
        } else {
            build_playlist();
        }

        // Cleanup allocated SKU strings
        for (int i = 0; i < all_skus_count; i++) {
            free(all_skus[i]);
        }

        return NFC_RESULT_PLAY_NOW;
    } else {
        // This shouldn't happen if all checks passed, but handle gracefully
        ESP_LOGE(TAG, "Album not found after all checks passed - this shouldn't happen");

        // Cleanup allocated SKU strings
        for (int i = 0; i < all_skus_count; i++) {
            free(all_skus[i]);
        }

        return NFC_RESULT_ACTIVATION_REQUIRED;
    }
}

// NFC SYNC INFRASTRUCTURE =====================================

// Static variables for NFC sync callback context
static char nfc_sync_sku[64] = {0};
static uint8_t nfc_sync_uid[7] = {0};
static int nfc_sync_context = 0;
static bool nfc_sync_attempted = false;

/**
 * @brief Unified NFC sync callback - handles post-sync logic based on context
 * NOTE: This function is kept for backward compatibility but sync is now disabled (#if 0)
 * All checks are done directly in handle_nfc_input() without sync
 */
static void nfc_sync_task_cb(void) {
    const char* context_name = (nfc_sync_context == NFC_SYNC_CONTEXT_CONTENT_UPDATE) ? "CONTENT_UPDATE" : "ACTIVATION_CHECK";
    ESP_LOGI(TAG, "[NFC_%s] Post-sync callback - re-checking NFC result for SKU: %s", context_name, nfc_sync_sku);

    // Re-run handle_nfc_input to check result (sync is disabled, so this just re-checks)
    nfc_result_t nfc_result = handle_nfc_input(nfc_sync_sku, nfc_sync_uid);

    switch (nfc_result) {
        case NFC_RESULT_EXPIRED:
            ESP_LOGI(TAG, "[NFC_%s] Album expired - showing no content screen", context_name);
            s3_nfc_content_type = NFC_CONT_NO_CONTENT;
            {
                s3_screens_t previous = get_previous_screen();
                set_current_screen(NFC_CONTENT_SCREEN, previous);
            }
            break;

        case NFC_RESULT_PLAY_NOW:
            ESP_LOGI(TAG, "[NFC_%s] Ready to play - triggering EVENT_NFC_DETECTED", context_name);
            app_state_handle_event(EVENT_NFC_DETECTED);
            break;

        case NFC_RESULT_NOT_DOWNLOADED:
            ESP_LOGI(TAG, "[NFC_%s] Not downloaded - showing not downloaded screen", context_name);
            s3_nfc_content_type = NFC_CONT_NOT_DOWNLOADED;
            {
                s3_screens_t previous = get_previous_screen();
                set_current_screen(NFC_CONTENT_SCREEN, previous);
            }
            break;

        case NFC_RESULT_ACTIVATION_REQUIRED:
            ESP_LOGI(TAG, "[NFC_%s] Requires activation - showing activation screen", context_name);
            s3_nfc_content_type = NFC_CONT_GO_ACTIVE;
            {
                s3_screens_t previous = get_previous_screen();
                set_current_screen(NFC_CONTENT_SCREEN, previous);
            }
            break;

        case NFC_RESULT_NO_CONTENT:
            ESP_LOGI(TAG, "[NFC_%s] No content - showing no content screen", context_name);
            s3_nfc_content_type = NFC_CONT_NO_CONTENT;
            {
                s3_screens_t previous = get_previous_screen();
                set_current_screen(NFC_CONTENT_SCREEN, previous);
            }
            break;

        default:
            ESP_LOGE(TAG, "[NFC_%s] Unknown result: %d - showing activation screen", context_name, nfc_result);
            s3_nfc_content_type = NFC_CONT_GO_ACTIVE;
            {
                s3_screens_t previous = get_previous_screen();
                set_current_screen(NFC_CONTENT_SCREEN, previous);
            }
            break;
    }

    // Reset sync tracking
    nfc_sync_attempted = false;
    nfc_processing = false;
    nfc_resume();
}

/**
 * @brief Start NFC sync task with specified context
 * @param sku_detected The SKU that was detected
 * @param uid The NFC tag UID (7 bytes)
 * @param context The sync context (CONTENT_UPDATE or ACTIVATION_CHECK)
 */
void start_nfc_sync_task(const char* sku_detected, const uint8_t* uid, int context) {
    // Store SKU, UID and context in static variables for callback access
    strncpy(nfc_sync_sku, sku_detected ? sku_detected : "", sizeof(nfc_sync_sku) - 1);
    nfc_sync_sku[sizeof(nfc_sync_sku) - 1] = '\0';

    if (uid) {
        memcpy(nfc_sync_uid, uid, 7);
    } else {
        memset(nfc_sync_uid, 0, 7);  // Use zero UID if none provided
    }

    nfc_sync_context = context;

    // Mark that sync was attempted for this tag (for activation check context)
    if (context == NFC_SYNC_CONTEXT_ACTIVATION_CHECK) {
        nfc_sync_attempted = true;
    }

    // Use existing start_nfc_sync infrastructure
    nfc_sync_param_t *param = malloc(sizeof(nfc_sync_param_t));
    if (param == NULL) {
        ESP_LOGE(TAG, "Failed to allocate memory for NFC sync params");
        if (context == NFC_SYNC_CONTEXT_ACTIVATION_CHECK) {
            nfc_sync_attempted = false;
        }
        return;
    }

    param->callback = nfc_sync_task_cb;
    param->is_from_cli = false;  // This function is called from tag reading, not CLI

    const char* context_name = (context == NFC_SYNC_CONTEXT_CONTENT_UPDATE) ? "CONTENT_UPDATE" : "ACTIVATION_CHECK";
#if 0 // 1 USE_NFC_SYNC / 0 NOT_USE_NFC_SYNC
    // If sync needed uncomment these lines, else keep comented
    ESP_LOGI(TAG, "Starting NFC sync (%s) for SKU: %s", context_name, sku_detected ? sku_detected : "none");
    start_nfc_sync(param);
#else
    // If sync not needed call post-sync callback directly (no sync needed per new spec)
    nfc_sync_task_cb();
#endif
}

// STATE MACHINE RELATED ==========================================

// NFC tag detection callback
void printTag(NfcTagData *tag)
{
    static uint8_t last_uid[NFC_UID_LEN] = {0};
    int last_uid_len = 0;

    ESP_LOGI(TAG, "NFC deteced.");
    if (tag == NULL || nfc_event_queue == NULL || nfc_processing == true)
    {
        ESP_LOGI(TAG, "tag == NULL || nfc_event_queue == NULL || nfc_processing, return.");
        return;
    }
    else
    {
        // Set nfc_processing flag to prevent new NFC processing until current processing completes
        nfc_processing = true;
        // Pause NFC IRQ to reduce hardware loading during processing
        nfc_pause();
        // Check for repeated tag scan within 3 seconds
        uint32_t now = esp_log_timestamp();
        if (last_uid_len == NFC_UID_LEN &&
        memcmp(last_uid, tag->uid, NFC_UID_LEN) == 0 &&
        now - last_detect_time < 2000)
        {
            ESP_LOGI(TAG, "Same tag within 2s, skipping.");
            // Reset nfc_processing flag since we're not processing this duplicate
            nfc_processing = false;
            // Resume NFC IRQ since we're not processing this duplicate
            nfc_resume();
            return;
        }

        // Store current UID and timestamp
        memcpy(last_uid, tag->uid, NFC_UID_LEN);
        last_uid_len = NFC_UID_LEN;
        last_detect_time = now;

        ESP_LOGI(TAG, "Sending data to queue.");
    }

    NfcTagData copy;
    memcpy(&copy, tag, sizeof(NfcTagData));
    BaseType_t ok = xQueueSend(nfc_event_queue, &copy, 0);

    if (ok != pdTRUE)
    {
        ESP_LOGW(TAG, "Failed to send NFC tag to queue (queue full?)");
    }
}

/**
 * @brief Centralized NFC state management based on screen type
 * @param screen The screen to set NFC state for
 */
void manage_nfc_state(s3_screens_t screen)
{
    switch (screen) {
        case HOME_SCREEN:
        case PLAY_SCREEN:
        case STANDBY_SCREEN:
        case CLOCK_SCREEN:
            ESP_LOGI(TAG, "NFC RESUME for screen: %d", screen);
            nfc_processing = false; // Reset nfc_processing flag when entering NFC-enabled screens
            nfc_resume();
            break;

        default:
            ESP_LOGI(TAG, "NFC PAUSE for screen: %d", screen);
            nfc_pause();
            break;
    }
}

/**
 * @brief Callback executed after NFC_CONTENT_SCREEN timeout when content type is NFC_CONT_UPDATING
 * Checks if album became available after potential cloud download, otherwise restores normal album navigation
 */
static void nfc_content_updating_timeout_cb(void) {
    ESP_LOGI(TAG, "[NFC_CALLBACK] NFC content updating timeout callback - checking if album is now available");

    // Check if album is still NULL after potential cloud download
    if (s3_current_album == NULL) {
        ESP_LOGI(TAG, "Album still not available after cloud sync - showing no content screen");
        audio_update_album_data(); // Restore normal album navigation so home screen shows available albums

        // Show no content screen for 3 seconds, then return to previous screen
        s3_nfc_content_type = NFC_CONT_NO_CONTENT;
        s3_screens_t previous = get_previous_screen();
        set_current_screen(NFC_CONTENT_SCREEN, previous);
    } else {
        // Album became available - check if it's NFC-enabled and trigger playback
        if (s3_current_album->is_available_nfc) {
            ESP_LOGI(TAG, "Album now available and NFC-enabled: %s, triggering playback", s3_current_album->sku);
            app_state_handle_event(EVENT_NFC_DETECTED);
        } else {
            ESP_LOGI(TAG, "Album available but requires activation: %s - showing activation screen", s3_current_album->sku);
            audio_update_album_data(); // Restore normal album navigation

            // Show activation required screen for 3 seconds, then return to HOME_SCREEN
            s3_nfc_content_type = NFC_CONT_GO_ACTIVE;
            set_current_screen(NFC_CONTENT_SCREEN, HOME_SCREEN);
        }
    }
}

/**
 * @brief Task that processes queued NFC tag data
 */
static void nfc_worker_task(void *pv)
{
    NfcTagData tag;
    ESP_LOGI(TAG, "NFC task started");

    nfc_event_queue = xQueueCreate(NFC_QUEUE_SIZE, sizeof(NfcTagData));
    ESP_LOGI("STACK", "nfc_worker_task started");
    UBaseType_t watermark = uxTaskGetStackHighWaterMark(NULL);
    ESP_LOGI("STACK", "Stack watermark: %u words", watermark);

    while (!nfc_worker_should_exit)
    {
        // Use timeout instead of portMAX_DELAY to allow checking exit flag
        if (xQueueReceive(nfc_event_queue, &tag, pdMS_TO_TICKS(100)))
        {
            play_stop();

            // nfc_sync flag is already set from printTag(), process the event
            // Select appropriate SKU based on system language

            // NFC detect sound volume logic:
            // - If device volume is 1,2,3 -> play at volume 3
            // - If device volume is 4,5,6 -> play at that volume
            int original_volume = get_current_volume_level();
            int nfc_sound_volume = original_volume;

            if (original_volume >= 1 && original_volume <= 3) {
                nfc_sound_volume = 3;
            }
            // For volumes 4,5,6 keep the same volume

            // Temporarily set volume for NFC detect sound if needed
            if (nfc_sound_volume != original_volume) {
                if (set_volume_level(nfc_sound_volume)) {
                    ESP_LOGI("NFC", "Temporarily set volume to %d for NFC detect sound (original: %d)",
                             nfc_sound_volume, original_volume);
                } else {
                    ESP_LOGW("NFC", "Failed to set volume for NFC detect sound");
                }
            }

            if (audio_play_sound_effect_quick("/sdcard/sound/PIX-WE-04-Detected.mp3")) {
                ESP_LOGI("NFC", "NFC detection sound started successfully");
            } else {
                ESP_LOGW("NFC", "Failed to play NFC detection sound");
            }

            // Wait a bit for shutdown sound to play
            do {
                vTaskDelay(pdMS_TO_TICKS(100));
            } while (is_audio_playing());

            // Restore original volume if it was changed
            if (nfc_sound_volume != original_volume) {
                if (set_volume_level(original_volume)) {
                    ESP_LOGI("NFC", "Restored volume to %d after NFC detect sound", original_volume);
                } else {
                    ESP_LOGW("NFC", "Failed to restore volume after NFC detect sound");
                }
            }

            ESP_LOGI(TAG, "NFC Tag Detected:");
            ESP_LOGI(TAG, "UID: %02x%02x%02x%02x%02x%02x%02x",
                     tag.uid[0], tag.uid[1], tag.uid[2],
                     tag.uid[3], tag.uid[4], tag.uid[5], tag.uid[6]);
            ESP_LOGI(TAG, "Album: %s", tag.albums);

            // Use handle_nfc_input to process the raw NFC data
            // This function handles enfc, comma-separated SKUs, and single SKUs properly
            nfc_result_t nfc_result = handle_nfc_input(tag.albums, tag.uid);

            // Handle NFC results based on proper enum values
            switch (nfc_result) {
                case NFC_RESULT_NO_CONTENT:
                    ESP_LOGI(TAG, "NFC has no content (SKU: none) - showing no content screen");
                    // MIGRATED: Use unified NFC_CONTENT_SCREEN instead of separate NFC_NO_CONTENT_SCREEN
                    // Set content type to indicate no content available
                    s3_nfc_content_type = NFC_CONT_NO_CONTENT;
                    s3_screens_t previous = get_previous_screen();
                    set_current_screen(NFC_CONTENT_SCREEN, previous);
                    nfc_processing = false; // Clear processing flag to allow new NFC processing
                    nfc_resume(); // Resume NFC IRQ
                    break;

                case NFC_RESULT_NO_MEMORY:
                    ESP_LOGI(TAG, "NFC requires activation - showing activation screen");
                    // NFC tag requires activation - no need for cloud sync, just show activation screen
                    // and return to HOME_SCREEN after 3 seconds
                    s3_nfc_content_type = NFC_CONT_GO_ACTIVE;
                    set_current_screen(NFC_CONTENT_SCREEN, HOME_SCREEN);
                    nfc_processing = false; // Clear processing flag to allow new NFC processing
                    nfc_resume(); // Resume NFC IRQ
                    break;

                case NFC_RESULT_EXPIRED:
                    ESP_LOGI(TAG, "NFC album expired - showing no content screen");
                    s3_nfc_content_type = NFC_CONT_NO_CONTENT;
                    {
                        s3_screens_t previous = get_previous_screen();
                        set_current_screen(NFC_CONTENT_SCREEN, previous);
                    }
                    nfc_processing = false; // Clear processing flag to allow new NFC processing
                    nfc_resume(); // Resume NFC IRQ
                    break;

                case NFC_RESULT_PLAY_NOW:
                    ESP_LOGI(TAG, "NFC album ready for playback - triggering playback");
                    // Check if an album was actually selected (s3_current_album should be set by handle_nfc_input)
                    if (s3_current_album != NULL) {
                        ESP_LOGI(TAG, "NFC album selected successfully, triggering playback");
                        app_state_handle_event(EVENT_NFC_DETECTED);
                    } else {
                        ESP_LOGW(TAG, "NFC result was PLAY_NOW but no album selected - this shouldn't happen");
                        // Fallback to activation required
                        s3_nfc_content_type = NFC_CONT_GO_ACTIVE;
                        set_current_screen(NFC_CONTENT_SCREEN, HOME_SCREEN);
                    }
                    nfc_processing = false; // Clear processing flag to allow new NFC processing
                    nfc_resume(); // Resume NFC IRQ
                    break;

                case NFC_RESULT_ACTIVATION_REQUIRED:
                    ESP_LOGI(TAG, "NFC not in account - showing activation screen");
                    s3_nfc_content_type = NFC_CONT_GO_ACTIVE;
                    {
                        s3_screens_t previous = get_previous_screen();
                        set_current_screen(NFC_CONTENT_SCREEN, previous);
                    }
                    nfc_processing = false; // Clear processing flag to allow new NFC processing
                    nfc_resume(); // Resume NFC IRQ
                    break;

                case NFC_RESULT_NOT_DOWNLOADED:
                    ESP_LOGI(TAG, "NFC content not downloaded - showing not downloaded screen");
                    s3_nfc_content_type = NFC_CONT_NOT_DOWNLOADED;
                    {
                        s3_screens_t previous = get_previous_screen();
                        set_current_screen(NFC_CONTENT_SCREEN, previous);
                    }
                    nfc_processing = false; // Clear processing flag to allow new NFC processing
                    nfc_resume(); // Resume NFC IRQ
                    break;

                default:
                    ESP_LOGE(TAG, "Unknown NFC result: %d", nfc_result);
                    // Fallback to activation required
                    s3_nfc_content_type = NFC_CONT_GO_ACTIVE;
                    set_current_screen(NFC_CONTENT_SCREEN, HOME_SCREEN);
                    nfc_processing = false;
                    nfc_resume();
                    break;
            }
        }
    }

    // Task is exiting - clean up resources
    ESP_LOGI(TAG, "NFC worker task exiting gracefully");

    if (nfc_event_queue != NULL) {
        vQueueDelete(nfc_event_queue);
        nfc_event_queue = NULL;
        ESP_LOGI(TAG, "NFC event queue deleted");
    }

    nfc_worker_task_handle = NULL;
    vTaskDelete(NULL);  // Delete self
}

void stop_nfc_worker(void)
{
    vTaskDelete(nfc_worker_task_handle);
}

/**
 * @brief Initialize the NFC event queue and processing task
 */
void app_init_nfc_worker(void)
{
    // nfc_event_queue = xQueueCreate(NFC_QUEUE_SIZE, sizeof(NfcTagData));
    // DMA optimization: NFC worker task doesn't need DMA-capable memory, use PSRAM (saves 4KB DMA)
    xTaskCreatePinnedToCoreWithCaps(nfc_worker_task, "nfc_worker_task", (4 * 1024), NULL, 3, &nfc_worker_task_handle, 0, MALLOC_CAP_SPIRAM);
}

void start_nfc(void)
{
    // Start nfc - DISABLED for memory testing
    ESP_LOGI(TAG, "NFC worker init ");
    app_init_nfc_worker();              // app_state
    register_nfc_callback(printTag);    // nfc- service
    nfc_enable();                       // nfc- service
}

void stop_nfc(void)
{
    ESP_LOGW(TAG, "Stopping NFC worker and all related components");

    // CRITICAL FIX: Graceful shutdown to prevent race conditions and crashes
    // We must allow the worker task to exit cleanly instead of forcefully deleting it

    // Step 1: Unregister callback first to prevent new events
    unregister_nfc_callback();

    // Step 2: Signal worker task to exit gracefully
    nfc_worker_should_exit = true;

    // Step 3: Wait for worker task to exit on its own (with timeout)
    int wait_count = 0;
    const int max_wait = 50;  // 500ms max wait (10ms * 50)
    while (nfc_worker_task_handle != NULL && wait_count < max_wait) {
        vTaskDelay(pdMS_TO_TICKS(10));
        wait_count++;
    }

    if (nfc_worker_task_handle == NULL) {
        ESP_LOGI(TAG, "NFC worker task exited gracefully after %d ms", wait_count * 10);
    } else {
        ESP_LOGW(TAG, "NFC worker task did not exit gracefully, forcing delete");
        vTaskDelete(nfc_worker_task_handle);
        nfc_worker_task_handle = NULL;

        // Clean up queue if task was forcefully deleted
        if (nfc_event_queue != NULL) {
            vQueueDelete(nfc_event_queue);
            nfc_event_queue = NULL;
        }
    }

    // Step 4: Gracefully shutdown NFC hardware via nfc_disable()
    // This calls StopDefaultTask() which waits for the NFC task to exit properly
    // IMPORTANT: nfc_disable() handles the shutdown of enable_nfc_task_handle internally
    nfc_disable();

    // Step 5: Add delay to ensure all NFC operations complete
    vTaskDelay(pdMS_TO_TICKS(100));

    // Clear any pending processing flags and reset exit flag
    nfc_processing = false;
    nfc_worker_should_exit = false;  // Reset for next time NFC is started

    ESP_LOGI(TAG, "NFC completely stopped - all components shut down");
}