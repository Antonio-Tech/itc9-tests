#include "s3_album_mgr.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/timers.h"
#include <dirent.h>
#include <sys/stat.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <errno.h>
#include <time.h>
#include "s3_definitions.h"
#include "audio_player.h"
#include "s3_nvs_item.h"
#include "lv_screen_mgr.h"
#include "s3_sync_account_contents.h"
#include "s3_logger.h"
#include "esp_heap_caps.h"
#include "cJSON.h"

#define TAG            "S3_ALBUM_MGR"

/* === RESOURCE SHARING SYSTEM === */

/**
 * @brief Resource sharing map - defines which SKU's resources to use
 *
 * For albums that share the same artwork, this table maps each SKU
 * to the SKU whose resource files should be used.
 */
typedef struct {
    const char *sku;           // The album's SKU
    const char *resource_sku;  // The SKU whose resources to use
} sku_resource_map_t;

static const sku_resource_map_t resource_sharing_map[] = {
    // Album pairs sharing resources (second in pair uses first's resources)
    {"SKU-00002", "SKU-00001"},
    {"SKU-00004", "SKU-00003"},
    {"SKU-00006", "SKU-00005"},
    {"SKU-00008", "SKU-00007"},  // SKU-00008 uses SKU-00007's covers
    {"SKU-00010", "SKU-00009"},  // SKU-00010 uses SKU-00009's covers
    {"SKU-00014", "SKU-00013"},  // SKU-00014 uses SKU-00013's covers
    {"SKU-00020", "SKU-00019"},
    {"SKU-00022", "SKU-00021"},
    {"SKU-00024", "SKU-00023"},  // SKU-00024 uses SKU-00023's covers
    {"SKU-00026", "SKU-00025"},
   {"SKU-00028", "SKU-00027"},
   {"SKU-00030", "SKU-00029"},
   {"SKU-00032", "SKU-00031"},
   {"SKU-00034", "SKU-00033"},
   {"SKU-00044", "SKU-00043"},
    {NULL, NULL}  // Sentinel
};

/**
 * @brief Get the SKU to use for resource paths (covers, animations)
 *
 * @param sku The album's actual SKU
 * @return The SKU whose resources should be used (may be different for shared resources)
 */
static const char* get_resource_sku(const char *sku) {
    if (!sku) return sku;

    // Check if this SKU shares resources with another
    for (int i = 0; resource_sharing_map[i].sku != NULL; i++) {
        if (strcmp(sku, resource_sharing_map[i].sku) == 0) {
            ESP_LOGI(TAG, "[RESOURCE_SHARING] %s will use %s's resources",
                     sku, resource_sharing_map[i].resource_sku);
            return resource_sharing_map[i].resource_sku;
        }
    }

    // No mapping found, use own resources
    return sku;
}

/* === HELPER FUNCTIONS === */

/**
 * @brief Convert language ID to string for logging
 * @param language_id The language ID (LANGUAGE_ENGLISH, LANGUAGE_CHINESE, NO_LANGUAGE)
 * @return String representation of the language
 */
static const char* get_language_string(int language_id) {
    switch (language_id) {
        case LANGUAGE_ENGLISH: return "en-us";
        case LANGUAGE_CHINESE: return "zh-tw";
        case LANGUAGE_ALL: return "all";
        case NO_LANGUAGE: return "none";
        default: return "unknown";
    }
}

/* === LOCAL ALBUM MANAGEMENT === */
// Global album variables moved from s3_definitions.c
// Note: blankee functionality merged into NFC albums - no separate blankee variables needed

// Dynamic album management - comprehensive replacement for static s3_albums array
size_t s3_dynamic_albums_count = 0;
s3_album_handler_t *s3_dynamic_albums = NULL;

size_t s3_nfc_albums_count = 0;
s3_album_handler_t *s3_nfc_albums = NULL;

/* === INTERNAL ALBUM HELPER FUNCTIONS === */
// Internal helper functions for album array management - moved from s3_definitions.c
static void append_to_dynamic_array(const s3_album_handler_t *album) {
    s3_album_handler_t *temp = realloc(s3_dynamic_albums, (s3_dynamic_albums_count + 1) * sizeof(s3_album_handler_t));
    if (temp == NULL) {
        ESP_LOGE(TAG, "dynamic album realloc failed!");
        return;
    }
    s3_dynamic_albums = temp;
    memcpy(&s3_dynamic_albums[s3_dynamic_albums_count], album, sizeof(s3_album_handler_t));
    s3_dynamic_albums_count++;
}

static void append_to_nfc_array(const s3_album_handler_t *album) {
    s3_album_handler_t *temp = realloc(s3_nfc_albums, (s3_nfc_albums_count + 1) * sizeof(s3_album_handler_t));
    if (temp == NULL) {
        ESP_LOGE(TAG, "NFC album realloc failed!");
        return;
    }
    s3_nfc_albums = temp;
    memcpy(&s3_nfc_albums[s3_nfc_albums_count], album, sizeof(s3_album_handler_t));
    s3_nfc_albums_count++;
}

// Internal clear functions for each album type
static void clear_dynamic_albums(void) {
    free(s3_dynamic_albums);
    s3_dynamic_albums = NULL;
    s3_dynamic_albums_count = 0;
}

static void clear_nfc_albums(void) {
    free(s3_nfc_albums);
    s3_nfc_albums = NULL;
    s3_nfc_albums_count = 0;
}

static void clear_blankee_albums(void) {
    // Note: blankee functionality merged into NFC albums - this is now a no-op
    ESP_LOGD(TAG, "clear_blankee_albums: blankee functionality merged into NFC albums");
}

static void clear_all_separated_albums(void) {
    clear_dynamic_albums();
    clear_nfc_albums();
    // Note: blankee functionality merged into NFC albums
}

static esp_err_t add_nfc_album(const char *sku, const char *language, int content_count) {
    if (!sku || !language) return ESP_ERR_INVALID_ARG;
    
    // Parse language first
    int lang_id = NO_LANGUAGE;
    if (strcmp(language, "zh-tw") == 0) {
        lang_id = LANGUAGE_CHINESE;
    } else if (strcmp(language, "en-us") == 0) {
        lang_id = LANGUAGE_ENGLISH;
    } else if (strcmp(language, "all") == 0) {
        lang_id = LANGUAGE_ALL;
    }
    
    // Use content count from cloud data, fallback to -1 if invalid
    int files_available = content_count > 0 ? content_count : -1;
    
    // Create NFC album structure
    s3_album_handler_t album = {
        .id = ALBUM_DEFAULT,  // Generic ID for NFC albums
        .files_available = files_available,   // Use actual count from cloud data
        .language = lang_id,
        .album_type = ALBUM_SKU,       // Use album type macro for NFC SKU albums
        .is_downloaded = IS_DOWNLOADED,
        .is_available_player = true,   // NFC albums should be playable when downloaded
        .is_available_nfc = true  // Always true for NFC albums
    };
    
    // Use snprintf to safely copy strings into fixed-size arrays
    snprintf(album.name, sizeof(album.name), "%s", sku); // Use SKU as name by default
    snprintf(album.sku, sizeof(album.sku), "%s", sku);
    snprintf(album.path, sizeof(album.path), "/sdcard/content/full/%s/", sku);

    // Use resource mapping for shared covers/animations
    const char *resource_sku = get_resource_sku(sku);
    snprintf(album.play_cover, sizeof(album.play_cover), "/sdcard/cover/device/%s_D.jpg", resource_sku);
    snprintf(album.home_cover, sizeof(album.home_cover), "/sdcard/cover/device/%s.jpg", resource_sku);
    snprintf(album.anim, sizeof(album.anim), "/sdcard/animation_gif/album_cover/%s.gif", resource_sku);
    
    append_to_dynamic_array(&album);  // Add to main dynamic array, not separate NFC array
    ESP_LOGD(TAG, "Added NFC album: %s", sku);
    return ESP_OK;
}

static esp_err_t add_blankee_album(const char *sku, const char *title, const char *language, int content_count) {
    if (!sku || !title || !language)
        return ESP_ERR_INVALID_ARG;

    // Parse language first
    int lang_id = NO_LANGUAGE;
    if (strcmp(language, "zh-tw") == 0) {
        lang_id = LANGUAGE_CHINESE;
    } else if (strcmp(language, "en-us") == 0) {
        lang_id = LANGUAGE_ENGLISH;
    } else if (strcmp(language, "all") == 0) {
        lang_id = LANGUAGE_ALL;
    }

    // Use content count from cloud data, fallback to -1 if invalid
    int files_available = content_count > 0 ? content_count : -1;

    // Create blankee album structure (stored in NFC array)
    s3_album_handler_t album = {
            .id = ALBUM_DEFAULT, // Generic ID for blankee albums
            .files_available = files_available, // Use actual count from cloud data
            .language = lang_id,
            .album_type = ALBUM_SKURC, // Use album type macro for SKURC albums
            .is_downloaded = IS_DOWNLOADED,
            .is_available_player = IS_PLAY_ENABLED,
            .is_available_nfc = true // Always true for blankee albums (NFC-based)
    };

    // Use snprintf to safely copy strings into fixed-size arrays
    snprintf(album.name, sizeof(album.name), "%s", title);
    snprintf(album.sku, sizeof(album.sku), "%s", sku);
    snprintf(album.path, sizeof(album.path), BLANKEE_CONTENT_PATH_TEMPLATE, sku);
    // Blankee albums use the same cover for both home and player, and have no animation
    // Try SKU-specific cover first - fallback logic will be handled during download validation
    if (strlen(sku) > strlen("SKU-00029")) {
        strcpy(album.play_cover, BLANKEE_COVER_FALLBACK);
        strcpy(album.home_cover, BLANKEE_COVER_FALLBACK);
    } else {
        snprintf(album.play_cover, sizeof(album.play_cover), BLANKEE_COVER_PATH_TEMPLATE, sku);
        snprintf(album.home_cover, sizeof(album.home_cover), BLANKEE_COVER_PATH_TEMPLATE, sku);
    }
    snprintf(album.anim, sizeof(album.anim), "%s", BLANKEE_NO_ANIMATION);

    // Add to main dynamic array (blankee functionality merged with all albums)
    append_to_dynamic_array(&album);
    ESP_LOGD(TAG, "Added blankee album to dynamic array: %s (%s)", sku, title);
    return ESP_OK;
}

/* === PUBLIC API FUNCTIONS === */

// Get size functions for album arrays
size_t get_dynamic_albums_size(void) { return s3_dynamic_albums_count; }
size_t get_nfc_albums_size(void) { return s3_nfc_albums_count; }
size_t get_kidspack_albums_size(void) { return s3_dynamic_albums_count; } // Legacy compatibility alias

// Dynamic album public API functions
s3_album_handler_t* get_dynamic_album_by_index(size_t index) {
    if (index >= s3_dynamic_albums_count) return NULL;
    return &s3_dynamic_albums[index];
}

s3_album_handler_t* find_dynamic_album_by_sku(const char *sku) {
    if (sku == NULL) return NULL;
    for (size_t i = 0; i < s3_dynamic_albums_count; i++) {
        if (strcmp(s3_dynamic_albums[i].sku, sku) == 0) {
            return &s3_dynamic_albums[i];
        }
    }
    return NULL;
}

// Legacy compatibility functions
s3_album_handler_t* get_kidspack_album_by_index(size_t index) {
    return get_dynamic_album_by_index(index);
}

s3_album_handler_t* find_kidspack_album_by_sku(const char *sku) {
    return find_dynamic_album_by_sku(sku);
}

// NFC album public API functions
s3_album_handler_t* find_nfc_album_by_sku(const char *sku) {
    if (sku == NULL) return NULL;
    for (size_t i = 0; i < s3_nfc_albums_count; i++) {
        if (strcmp(s3_nfc_albums[i].sku, sku) == 0) {
            return &s3_nfc_albums[i];
        }
    }
    return NULL;
}

// Universal search function
s3_album_handler_t* find_album_by_sku_universal(const char *sku) {
    s3_album_handler_t *album = NULL;
    
    // Try dynamic albums first (comprehensive list)
    album = find_dynamic_album_by_sku(sku);
    if (album != NULL) return album;
    
    // Try NFC albums (separate NFC-only albums)
    album = find_nfc_album_by_sku(sku);
    if (album != NULL) return album;
    
    // Note: blankee functionality merged into NFC albums, already searched above
    
    return NULL;
}

// Helper function to get album type name based on type and availability flags
static const char* get_album_type_name(const s3_album_handler_t *album) {
    if (!album) return "unknown";
    
    switch(album->album_type) {
        case ALBUM_DEFAULT: 
            return "default";
        case ALBUM_SKU:
            // For ALBUM_SKU, check if it's NFC ONLY (not available for player) or kidspack
            if (album->is_available_nfc && !album->is_available_player) {
                return "nfc";  // NFC ONLY
            } else {
                return "kidspack";  // kidspack ONLY or both kidspack AND nfc
            }
        case ALBUM_SKURC:
        case ALBUM_ISR:
            return "blankee";
        default:
            return "unknown";
    }
}


typedef struct {
    s3_album_handler_t **vec;
    size_t               size;
    size_t               cap;
} album_vec_t;

static album_vec_t          available_albums        = {0};             // Single list for available albums
static size_t               cur_idx                 = 0;                // Current index in available albums
// Global variables s3_current_album, s3_current_idx, s3_current_size, s3_current_idx_track,
// and s3_current_size_track are defined in s3_definitions.c and declared in s3_definitions.h
static SemaphoreHandle_t    album_mutex             = NULL;             // Mutex for thread safety

/* Auto-rescan timer removed - dynamic build now happens only on sync events */

/* ---------- helpers internos ---------- */
static esp_err_t vec_push(album_vec_t *v, s3_album_handler_t *h)
{
    if (v->size == v->cap) {
        size_t ncap = v->cap ? v->cap * 2 : 8;
        void  *tmp  = realloc(v->vec, ncap * sizeof(*v->vec));
        if (!tmp)  return ESP_ERR_NO_MEM;
        v->vec = tmp;
        v->cap = ncap;
    }
    v->vec[v->size++] = h;
    return ESP_OK;
}

static void vec_clear(album_vec_t *v) { v->size = 0; }

static void update_current_nolock(void)
{
    if (available_albums.size == 0) {
        ESP_LOGW(TAG, "[EMERGENCY_FALLBACK] No available albums found - attempting emergency recovery");
        
        // Emergency fallback: try to enable any downloaded albums to prevent "NO ALBUM AVAILABLE"
        bool emergency_album_found = false;
        size_t kidspack_size = get_kidspack_albums_size();
        
        // First, check if any albums are actually downloaded but not marked as available
        for (size_t i = 0; i < kidspack_size && !emergency_album_found; i++) {
            s3_album_handler_t *album = get_kidspack_album_by_index(i);
            if (album && album->is_downloaded) {
                ESP_LOGW(TAG, "[EMERGENCY_FALLBACK] Found downloaded album: %s (Type: %s, Player: %s, NFC: %s)", 
                         album->sku, get_album_type_name(album),
                         album->is_available_player ? "YES" : "NO",
                         album->is_available_nfc ? "YES" : "NO");
                
                // Try to add this album to available list regardless of availability flags
                esp_err_t result = vec_push(&available_albums, album);
                if (result == ESP_OK) {
                    ESP_LOGW(TAG, "[EMERGENCY_FALLBACK] Added album %s to available list", album->sku);
                    emergency_album_found = true;
                } else {
                    ESP_LOGE(TAG, "[EMERGENCY_FALLBACK] Failed to add album %s to available list", album->sku);
                }
            }
        }
        
        // If still no albums found, try to force-enable default albums
        if (!emergency_album_found) {
            ESP_LOGE(TAG, "[EMERGENCY_FALLBACK] No downloaded albums found - attempting to enable default albums");
            
            // Try to find and enable default albums
            for (size_t i = 0; i < kidspack_size && !emergency_album_found; i++) {
                s3_album_handler_t *album = get_kidspack_album_by_index(i);
                if (album && (album->id == DEFAULT_ALBUM_EN || album->id == DEFAULT_ALBUM_CH)) {
                    ESP_LOGW(TAG, "[EMERGENCY_FALLBACK] Found default album: %s (Downloaded: %s)", 
                             album->sku, album->is_downloaded ? "YES" : "NO");
                    
                    // Force enable this default album even if not downloaded (better than NO ALBUM)
                    album->is_available_player = true;
                    esp_err_t result = vec_push(&available_albums, album);
                    if (result == ESP_OK) {
                        ESP_LOGW(TAG, "[EMERGENCY_FALLBACK] Force-enabled default album %s", album->sku);
                        emergency_album_found = true;
                    } else {
                        ESP_LOGE(TAG, "[EMERGENCY_FALLBACK] Failed to add default album %s", album->sku);
                    }
                }
            }
        }
        
        if (!emergency_album_found) {
            ESP_LOGE(TAG, "[EMERGENCY_FALLBACK] CRITICAL: No albums available - will show 'NO ALBUM AVAILABLE'");
            s3_current_album = NULL;
            cur_idx = 0;
            s3_current_size = 0;
            s3_current_idx = 0;
            return;
        } else {
            ESP_LOGW(TAG, "[EMERGENCY_FALLBACK] Emergency recovery successful - %u albums now available", (unsigned int)available_albums.size);
        }
    }
    
    /* Try to preserve the current album during rescan */
    if (s3_current_album != NULL) {
        // First, try to find by pointer comparison (fastest)
        for (size_t i = 0; i < available_albums.size; i++) {
            if (available_albums.vec[i] == s3_current_album) {
                cur_idx = i;
                s3_current_album = available_albums.vec[cur_idx];
                s3_current_size  = available_albums.size;
                s3_current_idx   = cur_idx;
                ESP_LOGI(TAG, "Preserved current album %s at new index %u during rescan (pointer match)",
                         s3_current_album->sku, (unsigned int)cur_idx);
                return;
            }
        }

        // If pointer comparison failed, try to find by SKU (more reliable after list rebuild)
        if (s3_current_album->sku && strlen(s3_current_album->sku) > 0) {
            for (size_t i = 0; i < available_albums.size; i++) {
                if (available_albums.vec[i] &&
                    available_albums.vec[i]->sku &&
                    strcmp(available_albums.vec[i]->sku, s3_current_album->sku) == 0) {
                    cur_idx = i;
                    s3_current_album = available_albums.vec[cur_idx];
                    s3_current_size  = available_albums.size;
                    s3_current_idx   = cur_idx;
                    ESP_LOGI(TAG, "Preserved current album %s at new index %u during rescan (SKU match)",
                             s3_current_album->sku, (unsigned int)cur_idx);
                    return;
                }
            }
        }

        /* If not found, log and fallback to first available */
        ESP_LOGW(TAG, "Current album %s no longer available after rescan, switching to first available", 
                 s3_current_album->sku ? s3_current_album->sku : "unknown");
    }
    
    /* Fallback: reset to first album if necessary */
    // CRITICAL FIX: Always reset to index 0 when current album is not found
    // Don't use old cur_idx as it may point to wrong album after list rebuild
    cur_idx = 0;
    if (available_albums.size > 0) {
        s3_current_album = available_albums.vec[cur_idx];
        s3_current_size  = available_albums.size;
        s3_current_idx   = cur_idx;
        ESP_LOGI(TAG, "Reset to first available album %s at index 0",
                 s3_current_album->sku ? s3_current_album->sku : "unknown");
    } else {
        s3_current_album = NULL;
        s3_current_size  = 0;
        s3_current_idx   = 0;
        ESP_LOGW(TAG, "No available albums found, resetting current album to NULL");
    }
}


static bool s3_album_is_available(const s3_album_handler_t *album)
{
    if (!album) {
        ESP_LOGW(TAG, "[ALBUM_CHECK] NULL album pointer");
        return false;
    }
    
    ESP_LOGI(TAG, "[ALBUM_CHECK] Validating album: SKU=%s, Name=%s, Type=%s", 
             album->sku ? album->sku : "NULL",
             album->name ? album->name : "NULL", 
             get_album_type_name(album));
    
    // Check if album path exists
    if (!album->path) {
        ESP_LOGW(TAG, "[ALBUM_CHECK] FAILED - Album path is NULL for album %s", album->name ? album->name : "unknown");
        return false;
    }
    
    ESP_LOGI(TAG, "[ALBUM_CHECK] Checking directory: %s", album->path);
    struct stat st;
    if (stat(album->path, &st) != 0) {
        ESP_LOGW(TAG, "[ALBUM_CHECK] FAILED - Album directory missing: %s", album->path);
        return false;
    }
    ESP_LOGI(TAG, "[ALBUM_CHECK] Directory exists: %s", album->path);

    // COMMENTED OUT: Resource (jpg/gif/png) checks - only validate MP3 files
    // Check if cover images exist (optional files)
    // if (album->home_cover) {
    //     ESP_LOGI(TAG, "[ALBUM_CHECK] Checking home cover: %s", album->home_cover);
    //     if (stat(album->home_cover, &st) != 0) {
    //         ESP_LOGW(TAG, "[ALBUM_CHECK] FAILED - Home cover missing: %s", album->home_cover);
    //         return false;
    //     }
    //     ESP_LOGI(TAG, "[ALBUM_CHECK] Home cover exists: %s", album->home_cover);
    // }
    //
    // if (album->play_cover) {
    //     ESP_LOGI(TAG, "[ALBUM_CHECK] Checking play cover: %s", album->play_cover);
    //     if (stat(album->play_cover, &st) != 0) {
    //         ESP_LOGW(TAG, "[ALBUM_CHECK] FAILED - Player cover missing: %s", album->play_cover);
    //         return false;
    //     }
    //     ESP_LOGI(TAG, "[ALBUM_CHECK] Play cover exists: %s", album->play_cover);
    // }

    // Count MP3 files in the directory
    ESP_LOGI(TAG, "[ALBUM_CHECK] Counting MP3 files in: %s", album->path);
    DIR *dir = opendir(album->path);
    if (!dir) {
        ESP_LOGW(TAG, "[ALBUM_CHECK] FAILED - Cannot open album directory: %s", album->path);
        return false;
    }

    int mp3_count = 0;
    bool is_skurc_album = (album->sku && strncmp(album->sku, "SKURC-", 6) == 0);
    
    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        if (entry->d_type == DT_REG && strcasestr(entry->d_name, ".mp3")) {
            if (is_skurc_album) {
                // For SKURC albums, validate MP3 header to exclude MP4/AAC files
                char full_path[300];
                snprintf(full_path, sizeof(full_path), "%s%s", album->path, entry->d_name);
                if (is_real_mp3_file(full_path)) {
                    mp3_count++;
                    ESP_LOGI(TAG, "[ALBUM_CHECK] Valid MP3 file: %s", entry->d_name);
                } else {
                    ESP_LOGW(TAG, "[ALBUM_CHECK] Skipping invalid MP3 file: %s (not a real MP3)", entry->d_name);
                }
            } else {
                // For regular albums, count all .mp3 files
                mp3_count++;
                ESP_LOGD(TAG, "[ALBUM_CHECK] Found MP3 file: %s", entry->d_name);
            }
        }
    }
    closedir(dir);
    
    ESP_LOGI(TAG, "[ALBUM_CHECK] Found %d valid MP3 files, expected %d", mp3_count, album->files_available);

    // Check if we have the expected number of files (if specified)
    // Special handling for SKURC albums: validate against account file contents
    if (album->files_available > 0 && mp3_count != album->files_available) {
        // For SKURC albums, just check if any MP3 files exist (account determines playlist)
        if (album->sku && strncmp(album->sku, "SKURC-", 6) == 0) {
            if (mp3_count > 0) {
                ESP_LOGI(TAG, "[ALBUM_CHECK] SKURC album %s: %d MP3 files found (expected: %d), considering available", 
                         album->name ? album->name : "unknown", mp3_count, album->files_available);
                // MP3 files exist, album is available despite count mismatch
            } else {
                ESP_LOGW(TAG, "[ALBUM_CHECK] SKURC album %s: no MP3 files found in directory", 
                         album->name ? album->name : "unknown");
                return false;
            }
        } else {
            // Non-SKURC albums still use strict file count validation
            ESP_LOGW(TAG, "[ALBUM_CHECK] FAILED - Album %s: found %d MP3s, expected %d", 
                     album->name ? album->name : "unknown", mp3_count, album->files_available);
            return false;
        }
    }

    ESP_LOGI(TAG, "[ALBUM_CHECK] SUCCESS - Album %s is physically available (%d MP3 files)", 
             album->name ? album->name : "unknown", mp3_count);
    return true;
}

/* ---------- API pública ---------- */
int s3_albums_find_by_sku(const char *sku)
{
    if (!sku) {
        return NO_SKU;
    }
    
    size_t kidspack_size = get_kidspack_albums_size();
    for (size_t g = 0; g < kidspack_size; ++g) {
        s3_album_handler_t *album = get_kidspack_album_by_index(g);
        if (album && album->sku && strcmp(album->sku, sku) == 0 &&
            album->is_downloaded) {
            return (int)g;
        }
    }
    return NO_SKU;
}

size_t s3_albums_find_by_sku_lang(const char *sku)
{
    if (!sku) {
        return SIZE_MAX;
    }
    
    extern int s3_selected_language; // Defined in s3_definitions.c
    
    size_t best_match = SIZE_MAX;
    size_t fallback_match = SIZE_MAX;
    
    ESP_LOGI(TAG, "Looking for SKU %s with language preference: %d", sku, s3_selected_language);
    
    xSemaphoreTake(album_mutex, portMAX_DELAY);
    
    // Search through all kidspack albums for matching SKU
    size_t kidspack_size = get_kidspack_albums_size();
    for (size_t g = 0; g < kidspack_size; ++g) {
        s3_album_handler_t *album = get_kidspack_album_by_index(g);
        if (!album || !album->sku || strcmp(album->sku, sku) != 0) {
            continue; // SKU doesn't match
        }
        
        // Check if album is downloaded and NFC available
        bool downloaded = album->is_downloaded;
        bool nfc_available = album->is_available_nfc;
        
        if (!downloaded || !nfc_available) {
            ESP_LOGD(TAG, "SKU %s album %u not available (downloaded: %s, NFC: %s)",
                     sku, (unsigned int)g, downloaded ? "yes" : "no", nfc_available ? "yes" : "no");
            continue;
        }
        
        ESP_LOGI(TAG, "Found available album %u: %s (SKU: %s, Language: %d)",
                 (unsigned int)g, album->name, album->sku, album->language);
        
        // Check language preference
        if (album->language == s3_selected_language) {
            best_match = g;
            ESP_LOGI(TAG, "Perfect language match found at global index %u", (unsigned int)g);
            break; // Perfect match found
        } else if (fallback_match == SIZE_MAX) {
            // Keep first valid album as fallback
            fallback_match = g;
            ESP_LOGI(TAG, "Fallback album set to global index %u", (unsigned int)g);
        }
    }
    
    xSemaphoreGive(album_mutex);
    
    // Return the best option
    if (best_match != SIZE_MAX) {
        ESP_LOGI(TAG, "Using language-matched album at global index %u", (unsigned int)best_match);
        return best_match;
    } else if (fallback_match != SIZE_MAX) {
        ESP_LOGW(TAG, "No language match found, using fallback album at global index %u", (unsigned int)fallback_match);
        return fallback_match;
    } else {
        ESP_LOGE(TAG, "No available album found for SKU %s", sku);
        return SIZE_MAX;
    }
}

esp_err_t s3_albums_init(void)
{
    album_mutex = xSemaphoreCreateMutex();
    if (!album_mutex) return ESP_ERR_NO_MEM;

    ESP_LOGI(TAG, "Set album play mode before loading albums.");
    set_playback_mode(PLAYBACK_MODE_SEQUENTIAL);
    set_auto_play_mode(AUTO_PLAY_ALL);

    ESP_LOGI(TAG, "Perform initial dynamic build once at startup.");
    s3_albums_dynamic_build();
    
    // NOTE: Album restoration is now deferred until after HOME screen renders
    // This prevents the boot sound from interfering with album selection
    // See: restore_last_played_album_callback() in lv_screen_mgr.c
    
    s3_albums_check_availability(ALL_SKU);          // For debug
    /* Automatic rescan timer removed - dynamic builds now triggered only by sync events */
    
    return ESP_OK;
}

/* 
 * Legacy s3_albums_scan_sd function removed - 
 * functionality integrated into s3_albums_dynamic_build()
 */

const s3_album_handler_t *s3_albums_get_current(void) { return s3_current_album; }

/**
 * @brief Save the currently playing album SKU to SD card file for persistence across reboots
 * @param sku SKU of the album to save (usually from s3_current_album->sku)
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t s3_albums_save_last_played(const char *sku)
{
    if (!sku || strlen(sku) == 0) {
        ESP_LOGW(TAG, "[LAST_ALBUM] Cannot save: NULL or empty SKU");
        return ESP_ERR_INVALID_ARG;
    }
    
    ESP_LOGI(TAG, "[LAST_ALBUM] Saving to SD card: %s", sku);
    
    // Save to SD card file: /sdcard/tmp/last_played_album.txt
    FILE *file = fopen("/sdcard/tmp/last_played_album.txt", "w");
    if (!file) {
        ESP_LOGE(TAG, "[LAST_ALBUM] Failed to open file: %s", strerror(errno));
        return ESP_ERR_NOT_FOUND;
    }
    
    // Write SKU to file
    if (fprintf(file, "%s", sku) < 0) {
        ESP_LOGE(TAG, "[LAST_ALBUM] Failed to write: %s", strerror(errno));
        fclose(file);
        return ESP_FAIL;
    }
    
    fclose(file);
    ESP_LOGI(TAG, "[LAST_ALBUM] Saved: %s", sku);
    return ESP_OK;
}

/**
 * @brief Restore the last played album from SD card file
 *        Returns the album index if found and available, without switching to it
 *        This is called AFTER HOME screen renders to trigger album switch via n_step_album()
 * @return Album index (>=0) on success, or -1 if no saved album or not available
 * @note The caller should use n_step_album() with the returned index to trigger screen redraw
 */
int s3_albums_restore_last_played(void)
{
    char last_sku[S3_LAST_ALBUM_SKU_LENGTH] = {0};
    
    // Read last played album SKU from SD card file
    FILE *file = fopen("/sdcard/tmp/last_played_album.txt", "r");
    if (!file) {
        ESP_LOGI(TAG, "[LAST_ALBUM] No file found");
        return -1;
    }
    
    // Read SKU from file
    if (fscanf(file, "%31s", last_sku) != 1 || strlen(last_sku) == 0) {
        ESP_LOGI(TAG, "[LAST_ALBUM] Failed to read or empty SKU");
        fclose(file);
        return -1;
    }
    fclose(file);
    
    ESP_LOGI(TAG, "[LAST_ALBUM] Loading from SD card: %s", last_sku);
    
    // Take mutex to safely search available albums
    if (xSemaphoreTake(album_mutex, pdMS_TO_TICKS(1000)) != pdTRUE) {
        ESP_LOGW(TAG, "[LAST_ALBUM] Mutex timeout");
        return -1;
    }
    
    // Search for the album in the available (home-accessible) albums list
    int found_index = -1;
    for (size_t i = 0; i < available_albums.size; i++) {
        s3_album_handler_t *album = available_albums.vec[i];
        if (album && album->sku && strcmp(album->sku, last_sku) == 0) {
            found_index = (int)i;
            ESP_LOGI(TAG, "[LAST_ALBUM] Found: %s (index %u/%u)", 
                     album->name, (unsigned int)(i + 1), (unsigned int)available_albums.size);
            break;
        }
    }
    
    xSemaphoreGive(album_mutex);
    
    if (found_index < 0) {
        ESP_LOGW(TAG, "[LAST_ALBUM] Not available: %s", last_sku);
        return -1;
    }
    
    return found_index;
}

bool s3_albums_next(void)
{
    bool ok = false; 
    xSemaphoreTake(album_mutex, portMAX_DELAY);
    if (available_albums.size) {
        cur_idx = (cur_idx + 1) % available_albums.size;
        s3_current_album = available_albums.vec[cur_idx]; 
        s3_current_idx = cur_idx;
        ok = true;
    }
    xSemaphoreGive(album_mutex); 
    return ok;
}

bool s3_albums_prev(void)
{
    bool ok = false; 
    xSemaphoreTake(album_mutex, portMAX_DELAY);
    if (available_albums.size) {
        cur_idx = (cur_idx ? cur_idx - 1 : available_albums.size - 1);
        s3_current_album = available_albums.vec[cur_idx]; 
        s3_current_idx = cur_idx;
        ok = true;
    }
    xSemaphoreGive(album_mutex); 
    return ok;
}

size_t s3_albums_get_size(void)
{
    size_t s; 
    xSemaphoreTake(album_mutex, portMAX_DELAY);
    s = available_albums.size;
    xSemaphoreGive(album_mutex); 
    return s;
}

const s3_album_handler_t *s3_albums_get(size_t idx)
{
    const s3_album_handler_t *r = NULL;
    xSemaphoreTake(album_mutex, portMAX_DELAY);
    if (idx < available_albums.size) r = available_albums.vec[idx];
    xSemaphoreGive(album_mutex); 
    return r;
}

size_t s3_albums_get_current_idx(void)  { return s3_current_idx; }

size_t s3_albums_get_current_size(void) { return s3_current_size; }

void s3_albums_update_available(const char **available_skus, size_t count)
{
    if (!available_skus) {
        ESP_LOGW(TAG, "s3_albums_update_available: NULL SKU list provided");
        return;
    }

    xSemaphoreTake(album_mutex, portMAX_DELAY);
    
    // First, set all kidspack albums as unavailable for player use
    size_t kidspack_size = get_kidspack_albums_size();
    for (size_t i = 0; i < kidspack_size; i++) {
        s3_album_handler_t *album = get_kidspack_album_by_index(i);
        if (album) {
            album->is_available_player = false;
        }
    }
    
    // Then, mark the provided SKUs as available
    for (size_t j = 0; j < count; j++) {
        if (!available_skus[j]) continue; // Skip NULL SKUs
        
        for (size_t i = 0; i < kidspack_size; i++) {
            s3_album_handler_t *album = get_kidspack_album_by_index(i);
            if (album && album->sku && strcmp(album->sku, available_skus[j]) == 0) {
                album->is_available_player = true;
                ESP_LOGD(TAG, "[S3_ALBUM_MGR] SKU %s marked as available for player", available_skus[j]);
                break; // Found the SKU, no need to continue searching
            }
        }
    }
    
    xSemaphoreGive(album_mutex);
    ESP_LOGI(TAG, "Updated availability for %u SKUs", (unsigned int)count);
    
    // Use dynamic build instead of legacy scan_sd
    s3_albums_dynamic_build();
}

/* 
 * s3_albums_is_downloaded function removed - redundant
 * Use s3_albums_check_availability for comprehensive availability checks
 */

/* 
 * s3_albums_album_is_available and s3_albums_nfc_is_available functions removed - redundant
 * Use s3_albums_check_availability for comprehensive availability checks
 */

void s3_albums_availability_cmd()
{
    ESP_LOGW(TAG, "s3_albums_check_availability: Wrapper to use in CLI.");
    s3_albums_check_availability(ALL_SKU);
}

bool s3_albums_check_availability(const char *sku)
{
    if (!sku) {
        ESP_LOGW(TAG, "s3_albums_check_availability: NULL SKU provided");
        return false;
    }
    
    xSemaphoreTake(album_mutex, portMAX_DELAY);
    
    // Handle "all" case - print downloaded, player and NFC availability status
    if (strcmp(sku, "all") == 0) {
        ESP_LOGI(TAG, "=== [S3_ALBUM_MGR] ===");
        
        // Check kidspack albums
        size_t kidspack_size = get_kidspack_albums_size();
        for (size_t i = 0; i < kidspack_size; i++) {
            s3_album_handler_t *album = get_kidspack_album_by_index(i);
            if (album && album->sku) {
                bool downloaded = album->is_downloaded;
                bool album_available = album->is_available_player;
                bool nfc_available = album->is_available_nfc;
                ESP_LOGI(TAG, "SKU %-*s (%-5s)(%-9s): Downloaded=%s | Player=%s | NFC=%s", 
                         SKU_LEN,  // Use full SKU_LEN for proper alignment
                         album->sku, 
                         get_language_string(album->language),
                         get_album_type_name(album),
                         downloaded ? "YES" : "NO ",
                         album_available ? "YES" : "NO ", 
                         nfc_available ? "YES" : "NO ");
            }
        }
        
        xSemaphoreGive(album_mutex);
        return true; // "all" case always returns true
    }
    
    // Handle specific SKU case - search kidspack first, then NFC albums
    bool result = false;
    s3_album_handler_t *album = find_kidspack_album_by_sku(sku);
    if (album) {
        bool player_available = album->is_available_player;
        bool nfc_available = album->is_available_nfc;
        result = player_available || nfc_available;
        ESP_LOGI(TAG, "SKU %s (%s)(%s) availability: Player=%s | NFC=%s | Combined=%s", 
                 album->sku, 
                 get_language_string(album->language),
                 get_album_type_name(album),
                 player_available ? "YES" : "NO", 
                 nfc_available ? "YES" : "NO",
                 result ? "YES" : "NO");
    } else {
        // Also check NFC albums (includes blankee)
        album = find_nfc_album_by_sku(sku);
        if (album) {
            bool player_available = album->is_available_player;
            bool nfc_available = album->is_available_nfc;
            result = player_available || nfc_available;
            ESP_LOGI(TAG, "SKU %s (%s)(NFC) availability: Player=%s | NFC=%s | Combined=%s", 
                     album->sku, 
                     get_language_string(album->language),
                     player_available ? "YES" : "NO", 
                     nfc_available ? "YES" : "NO",
                     result ? "YES" : "NO");
        }
    }
    
    xSemaphoreGive(album_mutex);
    return result;
}

/* ---------- Factory Reset Support ---------- */
bool s3_album_mgr_factory_reset_status(void)
{
    int oob_status = 1; // Default to non-factory-reset state
    esp_err_t ret = s3_nvs_get(NVS_S3_DEVICE_OOB, &oob_status);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Failed to read OOB status from NVS, assuming factory-reset");
        return true; // Assume factory reset if read fails
    }
    ESP_LOGW(TAG, "OOB status [%d]. 0 = factory reset", oob_status);

    return (oob_status == OOB_FACTORY_RESET);
}

bool s3_albums_dynamic_build(void)
{
    int album_count;
    s3_babyPack_t *albums;
    int nfc_count;
    const s3_nfc_t *nfcs;
    static char skus[MAX_ALBUMS_BUFFER][SKU_LEN];  // Use macro for maximum albums buffer
    const char *new_available_skus[MAX_ALBUMS_BUFFER];  // Use macro for maximum albums buffer
    bool changes_detected = false;

    ESP_LOGI(TAG, "=== [S3_ALBUMS_DYNAMIC_BUILD] Starting unified album build and scan ===");
    
    xSemaphoreTake(album_mutex, portMAX_DELAY);
    
    // STEP 1: Build dynamic albums from cloud data
    // CRITICAL FIX: Always populate/update dynamic albums from parsed data
    // This ensures cloud albums are loaded even when default albums already exist
    size_t current_dynamic_count = get_dynamic_albums_size();
    ESP_LOGI(TAG, "Current dynamic albums count: %u, loading fresh cloud data...", (unsigned int)current_dynamic_count);
    
    esp_err_t populate_result = s3_album_mgr_load_from_cloud();
    if (populate_result == ESP_OK) {
        ESP_LOGI(TAG, "Successfully populated %u dynamic albums", (unsigned int)get_dynamic_albums_size());
    } else {
        ESP_LOGW(TAG, "Failed to populate dynamic albums from parsed data");
    }
    
    // STEP 2: Parse SKUs from album_data JSON for player availability
    get_babyPacks(&albums, &album_count);
    
    // Check bounds and warn if approaching or exceeding limit
    if (album_count >= MAX_ALBUMS_BUFFER) {
        ESP_LOGE(TAG, "CRITICAL: Album count (%d) exceeds MAX_ALBUMS_BUFFER (%d)! Data will be truncated!", 
                 album_count, MAX_ALBUMS_BUFFER);
        album_count = MAX_ALBUMS_BUFFER - 1; // Prevent buffer overflow
    } else if (album_count >= (MAX_ALBUMS_BUFFER * 3 / 4)) {
        ESP_LOGW(TAG, "WARNING: Album count (%d) is approaching MAX_ALBUMS_BUFFER limit (%d)", 
                 album_count, MAX_ALBUMS_BUFFER);
    }
    
    for (int i = 0; i < album_count; i++) {
        ESP_LOGI(TAG, "=== [get_babyPacks sku: %d/%d] %s ===", i, album_count, albums[i].skuId);
        new_available_skus[i] = albums[i].skuId;
    }

    // STEP 3: Parse NFC data for NFC availability
    get_nfcs(&nfcs, &nfc_count);
    ESP_LOGI(TAG, "=== [NFC AVAILABILITY CHECK] Found %d NFC entries ===", nfc_count);
    
    // Reset all NFC availability flags first for dynamic albums
    size_t dynamic_albums_count = get_dynamic_albums_size();
    for (size_t i = 0; i < dynamic_albums_count; i++) {
        s3_album_handler_t *album = get_dynamic_album_by_index(i);
        if (album) {
            bool old_value = album->is_available_nfc;
            album->is_available_nfc = false;
            if (old_value != false) {
                changes_detected = true;
                ESP_LOGI(TAG, "NFC availability changed for %s: true -> false", album->sku);
            }
        }
    }
    
    // Update NFC availability based on NFC data
    for (int i = 0; i < nfc_count; i++) {
        ESP_LOGI(TAG, "=== [NFC Entry %d/%d] sn:%s, linked:%s, skusCount:%d ===", 
                 i, nfc_count, nfcs[i].sn, nfcs[i].linked, nfcs[i].skusCount);
        
        // Check if this is a blankee/recording entry (has linked field)
        if (nfcs[i].linked && strlen(nfcs[i].linked) > 0) {
            // Blankee/recording entry: only process the linked SKU
            ESP_LOGI(TAG, "==== [BLANKEE NFC] Processing linked SKU only: %s ====", nfcs[i].linked);
            
            s3_album_handler_t *album = find_dynamic_album_by_sku(nfcs[i].linked);
            if (album) {
                bool old_value = album->is_available_nfc;
                album->is_available_nfc = true;
                if (old_value != true) {
                    changes_detected = true;
                    ESP_LOGI(TAG, "NFC availability changed for %s: false -> true (linked)", album->sku);
                }
            } else {
                ESP_LOGD(TAG, "Linked NFC SKU %s not found in dynamic albums - may be created later during album loading", nfcs[i].linked);
            }
        } else {
            // Regular NFC entry: process all SKUs in the array
            ESP_LOGI(TAG, "==== [REGULAR NFC] Processing all %d SKUs ====", nfcs[i].skusCount);
            
            for (int j = 0; j < nfcs[i].skusCount; j++) {
                ESP_LOGI(TAG, "===== [NFC SKU %d/%d] skuId:%s, language:%s =====", 
                         j, nfcs[i].skusCount, nfcs[i].skus[j].skuId, nfcs[i].skus[j].language);
                
                // Find matching album and update NFC availability in dynamic albums
                s3_album_handler_t *album = find_dynamic_album_by_sku(nfcs[i].skus[j].skuId);
                if (album) {
                    bool old_value = album->is_available_nfc;
                    album->is_available_nfc = true;
                    if (old_value != true) {
                        changes_detected = true;
                        ESP_LOGI(TAG, "NFC availability changed for %s: false -> true", album->sku);
                    }
                } else {
                    ESP_LOGD(TAG, "NFC SKU %s not found in dynamic albums - may be created later during album loading", nfcs[i].skus[j].skuId);
                }
            }
        }
    }

    size_t used_count = (album_count > 0) ? album_count : 0;

    // STEP 4: Check OOB status to determine default album availability
    bool is_factory_reset = s3_album_mgr_factory_reset_status();
    ESP_LOGI(TAG, "OOB status check: is_factory_reset=%s", is_factory_reset ? "true" : "false");
    
    // Determine if default albums should be enabled:
    // 1. If in OOB mode (factory reset): enable defaults
    // 2. If cloud has 0 albums: enable defaults (regardless of OOB status)
    // 3. If not in OOB mode AND cloud has albums: disable defaults (use cloud albums only)
    bool should_enable_defaults = is_factory_reset || (album_count == 0);
    
    if (should_enable_defaults) {
        ESP_LOGI(TAG, "Enabling default albums - Reason: %s", 
                 is_factory_reset ? "OOB mode (factory reset)" : "No albums from cloud");
        
        // Add default albums to the available list
        if (album_count == 0) {
            // Case: No cloud albums, use defaults only
            used_count = 0;
            for (size_t i = 0; i < dynamic_albums_count && used_count < 2; ++i) {
                s3_album_handler_t *album = get_dynamic_album_by_index(i);
                if (album && (album->id == DEFAULT_ALBUM_EN || album->id == DEFAULT_ALBUM_CH)) {
                    new_available_skus[used_count++] = album->sku;
                }
            }
        } else {
            // Case: OOB mode with cloud albums - add defaults to existing cloud albums
            for (size_t i = 0; i < dynamic_albums_count && used_count < MAX_ALBUMS_BUFFER; ++i) {
                s3_album_handler_t *album = get_dynamic_album_by_index(i);
                if (album && (album->id == DEFAULT_ALBUM_EN || album->id == DEFAULT_ALBUM_CH)) {
                    // Check if this default album is not already in the list
                    bool already_in_list = false;
                    for (size_t j = 0; j < used_count; j++) {
                        if (strcmp(album->sku, new_available_skus[j]) == 0) {
                            already_in_list = true;
                            break;
                        }
                    }
                    if (!already_in_list) {
                        new_available_skus[used_count++] = album->sku;
                    }
                }
            }
        }
        ESP_LOGI(TAG, "Total albums to enable: %u (includes defaults)", (unsigned int)used_count);
    } else {
        ESP_LOGI(TAG, "Default albums disabled - using cloud albums only (%d albums)", album_count);
        // used_count already set to album_count above
    }

    // STEP 5: Update player availability flags
    ESP_LOGI(TAG, "Updating player availability for %u SKUs", (unsigned int)used_count);
    
    // First, set all kidspack albums as unavailable for player use
    size_t kidspack_size = get_kidspack_albums_size();
    for (size_t i = 0; i < kidspack_size; i++) {
        s3_album_handler_t *album = get_kidspack_album_by_index(i);
        if (album) {
            album->is_available_player = false;
        }
    }
    
    // Then, mark the provided SKUs as available
    for (size_t j = 0; j < used_count; j++) {
        if (!new_available_skus[j]) continue; // Skip NULL SKUs
        
        for (size_t i = 0; i < kidspack_size; i++) {
            s3_album_handler_t *album = get_kidspack_album_by_index(i);
            if (album && album->sku && strcmp(album->sku, new_available_skus[j]) == 0) {
                album->is_available_player = true;
                ESP_LOGD(TAG, "[S3_ALBUM_MGR] SKU %s marked as available for player", new_available_skus[j]);
                break; // Found the SKU, no need to continue searching
            }
        }
    }
    
    // STEP 6: Build available albums list based on flags already set by s3_album_mgr_scan_sd_downloads()
    // Note: is_downloaded flags were already validated in s3_album_mgr_load_from_cloud() -> s3_album_mgr_scan_sd_downloads()
    ESP_LOGI(TAG, "=== [BUILD_AVAILABLE_LIST] Building available albums list - Factory reset mode: %s ===", is_factory_reset ? "ON" : "OFF");

    // Clear the available albums list
    vec_clear(&available_albums);

    // Build available albums list based on already-validated flags
    for (size_t i = 0; i < kidspack_size; ++i) {
        s3_album_handler_t *album = get_kidspack_album_by_index(i);
        if (!album) continue;

        bool downloaded = album->is_downloaded;  // Use already-validated flag from SD scan

        // In factory reset mode, only add default albums to navigation playlist
        if (is_factory_reset) {
            if (album->id != DEFAULT_ALBUM_EN &&
                album->id != DEFAULT_ALBUM_CH) {
                continue;
            }
            // In factory reset mode, add default albums if downloaded
            if (downloaded) {
                vec_push(&available_albums, album);
            }
        } else {
            // In normal mode, add albums that are both downloaded and cloud-available
            bool cloud_available = album->is_available_player;
            if (downloaded && cloud_available) {
                vec_push(&available_albums, album);
            }
        }
    }

    // STEP 7: Update current album selection
    update_current_nolock();
    
    xSemaphoreGive(album_mutex);
    
    ESP_LOGI(TAG, "=== [S3_ALBUMS_DYNAMIC_BUILD] Completed: Available albums=[%u] (Factory reset: %s) ===", 
             (unsigned)available_albums.size, is_factory_reset ? "ON" : "OFF");
    
    // Refresh the UI to show updated album list
    refresh_screen_display();
    ESP_LOGI(TAG, "Screen refreshed after unified album build and scan");
    
    // Return true if any changes were detected (either player or NFC)
    return changes_detected;
}

/* === MOVED FUNCTIONS FROM S3_DEFINITIONS.C === */

// read_json_file function moved from s3_definitions.c (already defined above)

// Note: Helper functions for global album arrays moved to s3_definitions.c
// Local functions in s3_album_mgr.c should use the global API functions

/* === ALBUM MANAGEMENT FUNCTIONS === */
/* Note: Functions that need global variable access are defined in s3_definitions.c */


/* Unified Album Management */
void s3_album_mgr_initialize(void) {
    // Clear all existing albums
    s3_album_mgr_clear_all();
    
    // Initialize default dynamic albums
    s3_album_mgr_add_defaults();
    
    ESP_LOGI(TAG, "Album manager initialized: %u dynamic, %u NFC (includes blankee)",
             (unsigned int)s3_dynamic_albums_count, (unsigned int)s3_nfc_albums_count);
}

void s3_album_mgr_clear_all(void) {
    s3_album_mgr_clear_dynamic_albums();
    clear_nfc_albums();
    // Note: blankee functionality merged into NFC albums
}

void s3_album_mgr_print_all(void) {
    s3_album_mgr_print_dynamic_albums();
    ESP_LOGD(TAG,"NFC Albums (includes blankee): %u", (unsigned int)s3_nfc_albums_count);
}

esp_err_t s3_album_mgr_get_from_account(void) {
    // TODO: Implement JSON parsing from account file
    ESP_LOGW(TAG, "Account JSON parsing not yet implemented");
    return ESP_ERR_NOT_SUPPORTED;
}

void s3_album_mgr_clear_dynamic_albums(void) {
    free(s3_dynamic_albums);
    s3_dynamic_albums = NULL;
    s3_dynamic_albums_count = 0;
}

void s3_album_mgr_print_dynamic_albums(void) {
    ESP_LOGD(TAG,"===== DYNAMIC ALBUM LIST (%u) =====", (unsigned int)s3_dynamic_albums_count);
    for (size_t i = 0; i < s3_dynamic_albums_count; i++) {
        ESP_LOGD(TAG, "[%u] ID=%d - Name=%s - SKU=%s - Language=%d", (unsigned int)i,
               s3_dynamic_albums[i].id, s3_dynamic_albums[i].name, 
               s3_dynamic_albums[i].sku, s3_dynamic_albums[i].language);
    }
    ESP_LOGD(TAG,"================================\n");
}

void s3_album_mgr_create_dynamic_album(const char *sku, const char *language, const char *name, int files_available) {
    // Parse language first
    // Handle empty string or NULL language - treat as NO_LANGUAGE for music albums
    int lang_id = NO_LANGUAGE;
    if (language && strlen(language) > 0) {
        if (strcmp(language, "zh-tw") == 0) {
            lang_id = LANGUAGE_CHINESE;
        } else if (strcmp(language, "en-us") == 0) {
            lang_id = LANGUAGE_ENGLISH;
        } else if (strcmp(language, "all") == 0) {
            lang_id = LANGUAGE_ALL;
        }
        // If language is not empty but doesn't match any known value, keep as NO_LANGUAGE
    }
    // If language is NULL or empty string, lang_id remains NO_LANGUAGE

    // Create album with dynamic file count from cloud data
    s3_album_handler_t album = {
        .id = ALBUM_DEFAULT,  // Generic ID for dynamic albums
        .files_available = files_available > 0 ? files_available : FILES_AVAILABLE_UNKNOWN,   // Use provided count or macro for unknown
        .language = lang_id,
        .album_type = ALBUM_SKU,       // Use album type macro for SKU albums
        .is_downloaded = IS_DOWNLOADED,
        .is_available_player = true,   // Albums ARE playable (will be controlled by cloud availability)
        .is_available_nfc = false      // Will be set separately if NFC is available
    };

    // Use snprintf to safely copy strings into fixed-size arrays
    snprintf(album.name, sizeof(album.name), "%s", name ? name : sku);
    snprintf(album.sku, sizeof(album.sku), "%s", sku);
    snprintf(album.path, sizeof(album.path), "/sdcard/content/full/%s/", sku);

    // Use resource mapping for shared covers/animations
    const char *resource_sku = get_resource_sku(sku);
    snprintf(album.play_cover, sizeof(album.play_cover), "/sdcard/cover/device/%s_D.jpg", resource_sku);
    snprintf(album.home_cover, sizeof(album.home_cover), "/sdcard/cover/device/%s.jpg", resource_sku);
    snprintf(album.anim, sizeof(album.anim), "/sdcard/animation_gif/album_cover/%s.gif", resource_sku);

    append_to_dynamic_array(&album);
    
    ESP_LOGI(TAG, "Created dynamic album: %s (language: %s->%d, files_available: %d)", 
            sku, language, lang_id, files_available);
}

s3_album_handler_t* s3_album_mgr_find_album_by_sku_universal(const char *sku) {
    s3_album_handler_t *album = NULL;
    
    // Try dynamic albums first (comprehensive list)
    album = find_dynamic_album_by_sku(sku);
    if (album != NULL) return album;
    
    // Try NFC albums (separate NFC-only albums)
    album = find_nfc_album_by_sku(sku);
    if (album != NULL) return album;
    
    // Note: blankee functionality merged into NFC albums, already searched above
    
    return NULL;
}

/* Album Initialization Functions */
esp_err_t s3_album_mgr_add_defaults(void) {
    ESP_LOGI(TAG, "[S3_DYNAMIC_ALBUMS] Initializing the default albums");
    
    // Ensure the array is cleared - these MUST be the first entries
    s3_album_mgr_clear_dynamic_albums();
    
    // Try to read from default albums JSON file
    char *json_data = read_file_to_spiram(DEFAULT_ALBUM_CONTENT_PATH);
    if (json_data) {
        ESP_LOGI(TAG, "Loading default albums from JSON: %s", DEFAULT_ALBUM_CONTENT_PATH);
        
        cJSON *json = cJSON_Parse(json_data);
        if (json) {
            cJSON *albums_array = cJSON_GetObjectItemCaseSensitive(json, "albums");
            if (cJSON_IsArray(albums_array)) {
                int album_count = cJSON_GetArraySize(albums_array);
                ESP_LOGI(TAG, "Found %d default albums in JSON", album_count);
                
                for (int i = 0; i < album_count; i++) {
                    cJSON *album_obj = cJSON_GetArrayItem(albums_array, i);
                    if (cJSON_IsObject(album_obj)) {
                        cJSON *sku = cJSON_GetObjectItemCaseSensitive(album_obj, "sku");
                        cJSON *name = cJSON_GetObjectItemCaseSensitive(album_obj, "name");
                        cJSON *language = cJSON_GetObjectItemCaseSensitive(album_obj, "language");
                        cJSON *files_count = cJSON_GetObjectItemCaseSensitive(album_obj, "files_count");
                        cJSON *path = cJSON_GetObjectItemCaseSensitive(album_obj, "path");
                        cJSON *play_cover = cJSON_GetObjectItemCaseSensitive(album_obj, "play_cover");
                        cJSON *home_cover = cJSON_GetObjectItemCaseSensitive(album_obj, "home_cover");
                        cJSON *animation = cJSON_GetObjectItemCaseSensitive(album_obj, "animation");
                        
                        if (cJSON_IsString(sku) && cJSON_IsString(language) && cJSON_IsString(name)) {
                            // Parse language
                            int lang_id = NO_LANGUAGE;
                            if (strcmp(language->valuestring, "zh-tw") == 0) {
                                lang_id = LANGUAGE_CHINESE;
                            } else if (strcmp(language->valuestring, "en-us") == 0) {
                                lang_id = LANGUAGE_ENGLISH;
                            } else if (strcmp(language->valuestring, "all") == 0) {
                                lang_id = LANGUAGE_ALL;
                            }
                            
                            // Create album structure with default availability using macros
                            s3_album_handler_t album = {
                                .id = ALBUM_DEFAULT,
                                .files_available = cJSON_IsNumber(files_count) ? files_count->valueint : 3,
                                .language = lang_id,
                                .album_type = ALBUM_DEFAULT,         // Use album type macro
                                .is_downloaded = IS_DOWNLOADED,      // Use macro
                                .is_available_player = IS_PLAY_ENABLED, // Use macro - FALSE by default
                                .is_available_nfc = IS_NFC_ENABLED       // Use macro - FALSE by default
                            };
                            
                            // Set proper album ID based on SKU
                            if (strcmp(sku->valuestring, "SKU-00013") == 0) album.id = DEFAULT_ALBUM_CH;
                            else if (strcmp(sku->valuestring, "SKU-00014") == 0) album.id = DEFAULT_ALBUM_EN;
                            
                            // Copy strings safely
                            snprintf(album.name, sizeof(album.name), "%s", name->valuestring);
                            snprintf(album.sku, sizeof(album.sku), "%s", sku->valuestring);
                            // Handle paths with fallback to defaults
                            if (cJSON_IsString(path)) {
                                snprintf(album.path, sizeof(album.path), "%s", path->valuestring);
                            } else {
                                snprintf(album.path, sizeof(album.path), "/sdcard/content/full/%s/", sku->valuestring);
                            }
                            
                            // Use resource mapping for shared covers/animations
                            const char *resource_sku = get_resource_sku(sku->valuestring);

                            if (cJSON_IsString(play_cover)) {
                                snprintf(album.play_cover, sizeof(album.play_cover), "%s", play_cover->valuestring);
                            } else {
                                snprintf(album.play_cover, sizeof(album.play_cover), "/sdcard/cover/device/%s_D.jpg", resource_sku);
                            }

                            if (cJSON_IsString(home_cover)) {
                                snprintf(album.home_cover, sizeof(album.home_cover), "%s", home_cover->valuestring);
                            } else {
                                snprintf(album.home_cover, sizeof(album.home_cover), "/sdcard/cover/device/%s.jpg", resource_sku);
                            }

                            if (cJSON_IsString(animation)) {
                                snprintf(album.anim, sizeof(album.anim), "%s", animation->valuestring);
                            } else {
                                snprintf(album.anim, sizeof(album.anim), "/sdcard/animation_gif/album_cover/%s.gif", resource_sku);
                            }
                            
                            append_to_dynamic_array(&album);
                            ESP_LOGI(TAG, "Added default album: %s (%s) at position %u",
                                    album.sku, album.name, (unsigned int)(s3_dynamic_albums_count - 1));
                        }
                    }
                }
            } else {
                ESP_LOGW(TAG, "No 'albums' array found in default JSON");
            }
            cJSON_Delete(json);
        } else {
            ESP_LOGE(TAG, "Failed to parse default albums JSON");
        }
        free(json_data);
    } else {
        ESP_LOGW(TAG, "Could not read default albums JSON file, using hardcoded defaults");
    }
    
    // Fallback: If no albums were loaded from JSON, add hardcoded defaults
    if (s3_dynamic_albums_count == 0) {
        ESP_LOGI(TAG, "Using hardcoded default albums as fallback");
        
        // Create hardcoded Chinese default album
        s3_album_handler_t album_ch = {
            .id = DEFAULT_ALBUM_CH,
            .files_available = 3,
            .language = LANGUAGE_CHINESE,
            .album_type = ALBUM_DEFAULT,           // Use album type macro
            .is_downloaded = IS_DOWNLOADED,        // Use macro
            .is_available_player = IS_PLAY_ENABLED, // Use macro - FALSE by default
            .is_available_nfc = IS_NFC_ENABLED     // Use macro - FALSE by default
        };
        snprintf(album_ch.name, sizeof(album_ch.name), "Default Chinese Album");
        snprintf(album_ch.sku, sizeof(album_ch.sku), "DEFAULT-CH");
        snprintf(album_ch.path, sizeof(album_ch.path), "/sdcard/content/full/SKU-00013/");

        // Use resource mapping for shared covers/animations
        const char *resource_sku_ch = get_resource_sku("SKU-00013");
        snprintf(album_ch.play_cover, sizeof(album_ch.play_cover), "/sdcard/cover/device/%s_D.jpg", resource_sku_ch);
        snprintf(album_ch.home_cover, sizeof(album_ch.home_cover), "/sdcard/cover/device/%s.jpg", resource_sku_ch);
        snprintf(album_ch.anim, sizeof(album_ch.anim), "/sdcard/animation_gif/album_cover/%s.gif", resource_sku_ch);
        append_to_dynamic_array(&album_ch);

        // Create hardcoded English default album
        s3_album_handler_t album_en = {
            .id = DEFAULT_ALBUM_EN,
            .files_available = 3,
            .language = LANGUAGE_ENGLISH,
            .album_type = ALBUM_DEFAULT,           // Use album type macro
            .is_downloaded = IS_DOWNLOADED,        // Use macro
            .is_available_player = IS_PLAY_ENABLED, // Use macro - FALSE by default
            .is_available_nfc = IS_NFC_ENABLED     // Use macro - FALSE by default
        };
        snprintf(album_en.name, sizeof(album_en.name), "Default English Album");
        snprintf(album_en.sku, sizeof(album_en.sku), "DEFAULT-EN");
        snprintf(album_en.path, sizeof(album_en.path), "/sdcard/content/full/SKU-00014/");

        // Use resource mapping for shared covers/animations
        const char *resource_sku_en = get_resource_sku("SKU-00014");
        snprintf(album_en.play_cover, sizeof(album_en.play_cover), "/sdcard/cover/device/%s_D.jpg", resource_sku_en);
        snprintf(album_en.home_cover, sizeof(album_en.home_cover), "/sdcard/cover/device/%s.jpg", resource_sku_en);
        snprintf(album_en.anim, sizeof(album_en.anim), "/sdcard/animation_gif/album_cover/%s.gif", resource_sku_en);
        append_to_dynamic_array(&album_en);
        
        ESP_LOGI(TAG, "Hardcoded default albums created with availability flags using macros (all FALSE by default)");
    }
    
    ESP_LOGI(TAG, "Default albums initialized: %u albums at positions 0-%u",
             (unsigned int)s3_dynamic_albums_count, (unsigned int)(s3_dynamic_albums_count - 1));
    return ESP_OK;
}

esp_err_t s3_album_mgr_load_from_cloud(void) {
    // Get data using the provided getter functions
    const s3_babyPack_t *babyPacks = NULL;
    int babyPackCount = 0;
    const s3_nfc_t *nfcs = NULL;
    int nfcCount = 0;
    
    get_babyPacks(&babyPacks, &babyPackCount);
    get_nfcs(&nfcs, &nfcCount);
    
    ESP_LOGI(TAG, "[S3_DYNAMIC_ALBUMS] Adding albums from cloud data: %d babyPacks, %d NFC entries", babyPackCount, nfcCount);
    
    // Clear existing albums before populating
    s3_album_mgr_clear_dynamic_albums();
    
    // STEP 1: ALWAYS add default albums first (positions 0 and 1)
    ESP_LOGI(TAG, "Adding default albums at positions 0-1");
    s3_album_mgr_add_defaults(); // This uses the renamed function
    
    time_t now = time(NULL);

    // STEP 2: Add kidspack albums if available
    if (babyPacks && babyPackCount > 0) {
        ESP_LOGI(TAG, "Adding %d kidspack albums from cloud data", babyPackCount);
        
        for (int i = 0; i < babyPackCount; i++) {
            if (babyPacks[i].skuId) {
                if (babyPacks[i].expiresAt != 0 && babyPacks[i].expiresAt < now) {
                    ESP_LOGE(TAG, "Skipping expired album: %s (expiresAt: %u, now: %u)", babyPacks[i].skuId, babyPacks[i].expiresAt, (unsigned int)now);
                    continue;
                }
                // Use the actual language from JSON, preserve empty string to allow NO_LANGUAGE
                // Only fallback to English if language is NULL (not present in JSON)
                // Empty string "" should be preserved to indicate NO_LANGUAGE for music albums
                const char *language = babyPacks[i].language ? babyPacks[i].language : "en-us";
                
                // Use the actual contentCount from cloud data, or -1 if not available
                int files_available = babyPacks[i].contentCount > 0 ? babyPacks[i].contentCount : -1;
                
                // Add debug logging to track file counts and language
                ESP_LOGI(TAG, "[CLOUD_DATA] BabyPack %s: language=%s, contentCount=%d, using files_available=%d", 
                        babyPacks[i].skuId, language, babyPacks[i].contentCount, files_available);
                
                // Create album with actual language and file count from cloud data
                s3_album_mgr_create_dynamic_album(babyPacks[i].skuId, language, babyPacks[i].skuId, files_available);
                
                ESP_LOGI(TAG, "Added kidspack album: %s (language: %s, files_available: %d)", 
                        babyPacks[i].skuId, language, files_available);
            }
        }
    } else {
        ESP_LOGI(TAG, "No kidspack data - only default albums available");
    }
    
    // STEP 3: Process NFC albums from parsed NFC data if available
    if (nfcs && nfcCount > 0) {
        ESP_LOGI(TAG, "Processing %d NFC entries for album creation", nfcCount);

        for (int i = 0; i < nfcCount; i++) {
            ESP_LOGI(TAG, "[NFC Entry %d] sn:%s, linked:%s, skusCount:%d", i, nfcs[i].sn, nfcs[i].linked ? nfcs[i].linked : "(empty)", nfcs[i].skusCount);

            // Check if this is a blankee/recording entry (has linked field)
            if (nfcs[i].linked && strlen(nfcs[i].linked) > 0) {
                // Blankee/recording entry: only process the linked SKU
                ESP_LOGI(TAG, "[BLANKEE NFC] Processing linked SKU only: %s", nfcs[i].linked);

                // Find the linked SKU in the skus array
                bool linked_sku_found = false;
                for (int j = 0; j < nfcs[i].skusCount; j++) {
                    if (nfcs[i].skus[j].skuId) {
                        linked_sku_found = true;

                        if (nfcs[i].skus[j].expiresAt != 0 && nfcs[i].skus[j].expiresAt < now) {
                            ESP_LOGE(TAG, "Skipping expired linked NFC album: %s, (expiresAt: %u, now: %u)", nfcs[i].skus[j].skuId, nfcs[i].skus[j].expiresAt,
                                     (unsigned int) now);
                            continue;
                        }

                        // Check if this SKU already exists in dynamic albums
                        if (!find_dynamic_album_by_sku(nfcs[i].skus[j].skuId)) {
                            // Get content count from NFC data
                            int content_count = nfcs[i].skus[j].contentCount;

                            ESP_LOGI(TAG, "[NFC_DATA] Linked SKU %s: contentCount=%d from cloud", nfcs[i].skus[j].skuId, content_count);

                            // All linked entries are blankees (regardless of SKU prefix)
                            add_blankee_album(nfcs[i].skus[j].skuId, nfcs[i].skus[j].skuId, nfcs[i].skus[j].language, content_count);
                            ESP_LOGI(TAG, "Added blankee album (linked): %s (%s, files: %d)", nfcs[i].skus[j].skuId, nfcs[i].skus[j].language, content_count);
                        } else {
                            // Enable NFC functionality for existing dynamic album
                            s3_album_handler_t *album = find_dynamic_album_by_sku(nfcs[i].skus[j].skuId);
                            if (album) {
                                album->is_available_nfc = true;
                                ESP_LOGI(TAG, "Enabled NFC for existing album (linked): %s", album->sku);
                            }
                        }
                    }
                }

                if (!linked_sku_found) {
                    ESP_LOGW(TAG, "Linked SKU %s not found in skus array for NFC entry %s", nfcs[i].linked, nfcs[i].sn);
                }
            } else {
                // Regular NFC entry: process all SKUs in the array
                ESP_LOGI(TAG, "[REGULAR NFC] Processing all %d SKUs", nfcs[i].skusCount);

                for (int j = 0; j < nfcs[i].skusCount; j++) {
                    if (nfcs[i].skus[j].skuId && nfcs[i].skus[j].language) {
                        if (nfcs[i].skus[j].expiresAt != 0 && nfcs[i].skus[j].expiresAt < now) {
                            ESP_LOGE(TAG, "Skipping expired NFC album: %s, (expiresAt: %u, now: %u)", nfcs[i].skus[j].skuId, nfcs[i].skus[j].expiresAt,
                                     (unsigned int) now);
                            continue;
                        }
                        // Check if this SKU already exists in dynamic albums
                        if (!find_dynamic_album_by_sku(nfcs[i].skus[j].skuId)) {
                            // Get content count from NFC data
                            int content_count = nfcs[i].skus[j].contentCount;

                            ESP_LOGI(TAG, "[NFC_DATA] SKU %s: contentCount=%d from cloud", nfcs[i].skus[j].skuId, content_count);

                            // Check if this is a SKURC or ISR album (blankee albums)
                            if (strncmp(nfcs[i].skus[j].skuId, "SKURC-", 6) == 0 || strncmp(nfcs[i].skus[j].skuId, "ISR-", 4) == 0) {
                                // Add as blankee album with content count
                                add_blankee_album(nfcs[i].skus[j].skuId, nfcs[i].skus[j].skuId, nfcs[i].skus[j].language, content_count);
                                ESP_LOGI(TAG, "Added blankee album: %s (%s, files: %d)", nfcs[i].skus[j].skuId, nfcs[i].skus[j].language, content_count);
                            } else {
                                // Add as regular NFC album if not in dynamic albums with content count
                                add_nfc_album(nfcs[i].skus[j].skuId, nfcs[i].skus[j].language, content_count);
                                ESP_LOGI(TAG, "Added NFC album: %s (%s, files: %d)", nfcs[i].skus[j].skuId, nfcs[i].skus[j].language, content_count);
                            }
                        } else {
                            // Enable NFC functionality for existing dynamic album
                            s3_album_handler_t *album = find_dynamic_album_by_sku(nfcs[i].skus[j].skuId);
                            if (album) {
                                album->is_available_nfc = true;
                                ESP_LOGI(TAG, "Enabled NFC for existing album: %s", album->sku);
                            }
                        }
                    }
                }
            }
        }
    }
    
    ESP_LOGI(TAG, "[S3_DYNAMIC_ALBUMS] Album creation completed: %u total albums", (unsigned int)s3_dynamic_albums_count);
    
    // After creating all albums, scan SD card to update is_downloaded flags
    ESP_LOGI(TAG, "Scanning SD card to update album availability flags...");
    s3_album_mgr_scan_sd_downloads();
    
    return ESP_OK;
}

esp_err_t s3_album_mgr_enable_default_albums_for_factory_reset(void) {
    ESP_LOGI(TAG, "Factory reset mode - enabling default albums for player");
    
    // Find and enable the first two albums (should be defaults at positions 0 and 1)
    for (size_t i = 0; i < s3_dynamic_albums_count && i < 2; i++) {
        if (strncmp(s3_dynamic_albums[i].sku, "DEFAULT-", 8) == 0) {
            s3_dynamic_albums[i].is_available_player = true;
            ESP_LOGI(TAG, "Enabled default album for player: %s", s3_dynamic_albums[i].sku);
        }
    }
    
    return ESP_OK;
}


void s3_album_mgr_scan_sd_downloads(void) {
    ESP_LOGI(TAG, "Scanning SD card to update is_downloaded flags for %u albums", (unsigned int)s3_dynamic_albums_count);
    
    for (size_t i = 0; i < s3_dynamic_albums_count; i++) {
        s3_album_handler_t *album = &s3_dynamic_albums[i];
        if (!album || !album->path) {
            ESP_LOGW(TAG, "Skipping album with NULL path at index %u", (unsigned int)i);
            continue;
        }
        
        ESP_LOGD(TAG, "\n=== [S3_DYNAMIC_ALBUMS] Checking album %s ===", album->sku);
        ESP_LOGD(TAG, "Path: %s", album->path);
        ESP_LOGD(TAG, "Home cover: %s", album->home_cover);
        ESP_LOGD(TAG, "Play cover: %s", album->play_cover);
        ESP_LOGD(TAG, "Animation: %s", album->anim);
        
        // Check components for comprehensive download verification
        struct stat st;
        bool path_exists = (stat(album->path, &st) == 0 && S_ISDIR(st.st_mode));

        // COMMENTED OUT: Resource (jpg/gif/png) checks - only validate MP3 files
        // For blankee albums, implement fallback logic for covers
        // bool home_cover_exists, play_cover_exists;
        // if (album->album_type == ALBUM_SKURC || album->album_type == ALBUM_ISR) {
        //     // Check home cover: try SKU-specific first, fallback to generic if needed
        //     home_cover_exists = album->home_cover[0] ? (stat(album->home_cover, &st) == 0) : false;
        //     if (!home_cover_exists) {
        //         // Try fallback cover
        //         bool fallback_exists = (stat(BLANKEE_COVER_FALLBACK, &st) == 0);
        //         if (fallback_exists) {
        //             ESP_LOGI(TAG, "  Using fallback home cover for blankee album %s: %s", album->sku, BLANKEE_COVER_FALLBACK);
        //             // Update album structure to use fallback cover
        //             strncpy(album->home_cover, BLANKEE_COVER_FALLBACK, sizeof(album->home_cover) - 1);
        //             album->home_cover[sizeof(album->home_cover) - 1] = '\0';
        //             home_cover_exists = true; // Mark as OK since fallback is available
        //         } else {
        //             ESP_LOGW(TAG, "  No cover available for blankee album %s (neither SKU-specific nor fallback)", album->sku);
        //             home_cover_exists = false; // Mark as NOT OK - no cover available
        //         }
        //     }
        //
        //     // Check play cover: try SKU-specific first, fallback to generic if needed
        //     play_cover_exists = album->play_cover[0] ? (stat(album->play_cover, &st) == 0) : false;
        //     if (!play_cover_exists) {
        //         // Try fallback cover
        //         bool fallback_exists = (stat(BLANKEE_COVER_FALLBACK, &st) == 0);
        //         if (fallback_exists) {
        //             ESP_LOGI(TAG, "  Using fallback play cover for blankee album %s: %s", album->sku, BLANKEE_COVER_FALLBACK);
        //             // Update album structure to use fallback cover
        //             strncpy(album->play_cover, BLANKEE_COVER_FALLBACK, sizeof(album->play_cover) - 1);
        //             album->play_cover[sizeof(album->play_cover) - 1] = '\0';
        //             play_cover_exists = true; // Mark as OK since fallback is available
        //         } else {
        //             ESP_LOGW(TAG, "  No cover available for blankee album %s (neither SKU-specific nor fallback)", album->sku);
        //             play_cover_exists = false; // Mark as NOT OK - no cover available
        //         }
        //     }
        // } else {
        //     // Regular albums - no fallback logic, strict validation
        //     home_cover_exists = album->home_cover[0] ? (stat(album->home_cover, &st) == 0) : true;
        //     play_cover_exists = album->play_cover[0] ? (stat(album->play_cover, &st) == 0) : true;
        // }
        //
        // bool anim_exists = album->anim[0] ? (stat(album->anim, &st) == 0) : true; // Animation optional for blankee

        bool had_downloaded = album->is_downloaded;
        int mp3_count = 0;

        ESP_LOGI(TAG, "[DOWNLOAD_VALIDATION] %s:", album->sku);
        ESP_LOGI(TAG, "  Directory: %s (%s)", album->path, path_exists ? "EXISTS" : "MISSING");
        // ESP_LOGI(TAG, "  Home cover: %s (%s)", album->home_cover, home_cover_exists ? "EXISTS" : "MISSING");
        // ESP_LOGI(TAG, "  Play cover: %s (%s)", album->play_cover, play_cover_exists ? "EXISTS" : "MISSING");
        // ESP_LOGI(TAG, "  Animation: %s (%s)", album->anim, anim_exists ? "EXISTS" : "MISSING");
        
        if (path_exists) {
            // Directory exists, check if it has MP3 files
            DIR *dir = opendir(album->path);
            if (dir) {
                struct dirent *entry;
                
                ESP_LOGD(TAG, "Directory %s opened successfully, checking for MP3 files...", album->path);
                
                while ((entry = readdir(dir)) != NULL) {
                    if (entry->d_type == DT_REG) {
                        // Check if it's an MP3 file (case insensitive)
                        size_t name_len = strlen(entry->d_name);
                        if (name_len > 4) {
                            const char *ext = &entry->d_name[name_len - 4];
                            if (strcasecmp(ext, ".mp3") == 0) {
                                mp3_count++;
                                ESP_LOGD(TAG, "Found MP3 file: %s", entry->d_name);
                            }
                        }
                    }
                }
                closedir(dir);
                
                ESP_LOGI(TAG, "  MP3 files: %d found (expected: %d)", mp3_count, album->files_available);

                // Album is considered downloaded if:
                // 1. Directory exists and has MP3 files
                // 2. SKIP: Required covers check (commented out)
                // 3. SKIP: Animation check (commented out)
                // 4. If files_available is -1, skip file count validation
                // 5. For SKURC albums, check sync list instead of strict file count
                bool has_required_mp3s;
                if (album->files_available == -1) {
                    // Skip file count validation when count is unknown (-1)
                    has_required_mp3s = (mp3_count > 0);
                    ESP_LOGI(TAG, "  File count validation skipped (unknown count: -1), MP3s: %d", mp3_count);
                } else if (album->sku && strncmp(album->sku, "SKURC-", 6) == 0) {
                    // SKURC albums: just check if any MP3 files exist (account file determines playlist)
                    if (mp3_count > 0) {
                        has_required_mp3s = true;
                        ESP_LOGI(TAG, "  SKURC album: %d MP3 files found (expected: %d), considering downloaded",
                                 mp3_count, album->files_available);
                    } else {
                        has_required_mp3s = false;
                        ESP_LOGW(TAG, "  SKURC album: no MP3 files found in directory");
                    }
                } else {
                    // Normal file count validation for regular albums
                    has_required_mp3s = (mp3_count > 0) &&
                        (album->files_available <= 0 || mp3_count >= album->files_available);
                }

                // SIMPLIFIED: Only require MP3 files (no cover/animation checks)
                album->is_downloaded = has_required_mp3s;
                ESP_LOGI(TAG, "  Album result: %s (MP3s: %s)",
                         album->is_downloaded ? "DOWNLOADED" : "NOT_DOWNLOADED",
                         has_required_mp3s ? "OK" : "MISSING");
                
                if (had_downloaded != album->is_downloaded) {
                    ESP_LOGI(TAG, "  Status changed: %s -> %s", 
                             had_downloaded ? "DOWNLOADED" : "NOT_DOWNLOADED", 
                             album->is_downloaded ? "DOWNLOADED" : "NOT_DOWNLOADED");
                }
            } else {
                // Directory exists but can't be opened
                album->is_downloaded = false;
                ESP_LOGW(TAG, "  Directory %s exists but cannot be opened: %s", album->path, strerror(errno));
                ESP_LOGI(TAG, "  Result: NOT_DOWNLOADED (directory access error)");
            }
        } else {
            // Directory doesn't exist
            album->is_downloaded = false;
            ESP_LOGI(TAG, "  Result: NOT_DOWNLOADED (directory missing)");
        }
        
        ESP_LOGD(TAG, "=== End check for %s ===\n", album->sku);
    }
    
    ESP_LOGI(TAG, "SD card scan completed for album is_downloaded flags");
}

/* === ALBUM MANAGEMENT FUNCTIONS === */
// All album management functions now implemented directly in this file


