#include "lv_decoders.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_jpeg_dec.h"
#include "esp_jpeg_common.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdlib.h>
#include "s3_logger.h"
#include "s3_sync_account_contents.h"
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>

#if ESP_IDF_VERSION_MAJOR >= 5
#include "esp_private/esp_cache_private.h"
#endif

#include "esp_cache.h"

static const char *TAG = "IMAGE_DECODER";

// Legacy descriptors and buffers (for backward compatibility)
static lv_img_dsc_t jpg_dsc = {.path = NULL};
static lv_img_dsc_t gif_dsc = {.path = NULL};
static lv_img_dsc_t icon_dsc = {.path = NULL};     /* small icons (badges, language glyphs) */
static lv_img_dsc_t png_dsc = {.path = NULL};      /* PNG with transparency support */
static uint8_t *jpg_buf = NULL;
static uint8_t *gif_buf = NULL;
static uint8_t *icon_buf  = NULL; /* small JPEG */
static uint8_t *png_buf = NULL;   /* PNG buffer */
// static uint8_t *lottie_data = NULL;
// static uint8_t *canvas_fb = NULL;

// Flags to track if buffers are borrowed from cache (true) or owned (false)
static bool jpg_buf_is_cached = false;
static bool gif_buf_is_cached = false;
static bool icon_buf_is_cached = false;
static bool png_buf_is_cached = false;

// Content-specific descriptors and buffers
static lv_img_dsc_t content_dsc[CONTENT_TYPE_MAX] = {[0 ... CONTENT_TYPE_MAX-1] = {.path = NULL}};
static uint8_t *content_buf[CONTENT_TYPE_MAX] = {NULL};
static bool content_buf_is_cached[CONTENT_TYPE_MAX] = {false};

// JPEG Cache for 240x240 images (20 slots)
#define JPEG_CACHE_SLOTS 20
#define JPEG_CACHE_TARGET_SIZE 240  // Only cache 240x240 images

typedef struct {
    char *path;              // File path string (dynamically allocated)
    uint8_t *buffer;         // Decoded RGB565 image buffer
    uint32_t timestamp;      // Last access time (monotonic counter for LRU)
    uint16_t width;          // Image width
    uint16_t height;         // Image height
    size_t buffer_size;      // Actual buffer size in bytes
    bool valid;              // true if slot contains valid data
} jpeg_cache_entry_t;

static jpeg_cache_entry_t jpeg_cache[JPEG_CACHE_SLOTS];
static uint32_t cache_timestamp_counter = 0;
static uint32_t cache_hits = 0;
static uint32_t cache_misses = 0;

// PNG Cache for any size images (file size based filtering)
#define PNG_CACHE_SLOTS 30
#define PNG_CACHE_MAX_FILE_SIZE (100 * 1024)  // Only cache PNGs < 100KB

typedef struct {
    char *path;              // File path string (dynamically allocated)
    uint8_t *buffer;         // Raw PNG file data
    uint32_t timestamp;      // Last access time (monotonic counter for LRU)
    uint16_t width;          // PNG width
    uint16_t height;         // PNG height
    size_t file_size;        // PNG file size in bytes
    bool valid;              // true if slot contains valid data
} png_cache_entry_t;

static png_cache_entry_t png_cache[PNG_CACHE_SLOTS];
static uint32_t png_cache_timestamp_counter = 0;
static uint32_t png_cache_hits = 0;
static uint32_t png_cache_misses = 0;
static size_t png_cache_total_bytes = 0;

/**
 * @brief Initialize the JPEG cache system
 */
void jpeg_cache_init(void)
{
    for (int i = 0; i < JPEG_CACHE_SLOTS; i++) {
        jpeg_cache[i].path = NULL;
        jpeg_cache[i].buffer = NULL;
        jpeg_cache[i].timestamp = 0;
        jpeg_cache[i].width = 0;
        jpeg_cache[i].height = 0;
        jpeg_cache[i].buffer_size = 0;
        jpeg_cache[i].valid = false;
    }
    cache_timestamp_counter = 0;
    cache_hits = 0;
    cache_misses = 0;
    ESP_LOGI(TAG, "JPEG cache initialized: %d slots for %dx%d images",
             JPEG_CACHE_SLOTS, JPEG_CACHE_TARGET_SIZE, JPEG_CACHE_TARGET_SIZE);
}

/**
 * @brief Find a JPEG in the cache by path
 * @param path File path to search for
 * @return Pointer to cache entry if found, NULL otherwise
 */
static jpeg_cache_entry_t *jpeg_cache_find(const char *path)
{
    if (!path) return NULL;

    for (int i = 0; i < JPEG_CACHE_SLOTS; i++) {
        if (jpeg_cache[i].valid &&
            jpeg_cache[i].path &&
            strcmp(jpeg_cache[i].path, path) == 0) {
            // Update timestamp on access (LRU)
            jpeg_cache[i].timestamp = cache_timestamp_counter++;
            cache_hits++;
            ESP_LOGI(TAG, "Cache HIT [%d]: %s (hits=%u, misses=%u, ratio=%.1f%%)",
                     i, path, cache_hits, cache_misses,
                     (100.0f * cache_hits) / (cache_hits + cache_misses));
            return &jpeg_cache[i];
        }
    }

    cache_misses++;
    ESP_LOGI(TAG, "Cache MISS: %s (hits=%u, misses=%u, ratio=%.1f%%)",
             path, cache_hits, cache_misses,
             (100.0f * cache_hits) / (cache_hits + cache_misses));
    return NULL;
}

/**
 * @brief Find the oldest (least recently used) cache slot
 * @return Slot index of the LRU entry
 */
static int jpeg_cache_find_oldest_slot(void)
{
    int oldest_idx = 0;
    uint32_t oldest_timestamp = UINT32_MAX;

    // First, prefer invalid (empty) slots
    for (int i = 0; i < JPEG_CACHE_SLOTS; i++) {
        if (!jpeg_cache[i].valid) {
            return i;
        }
    }

    // If all slots are valid, find the one with smallest timestamp
    for (int i = 0; i < JPEG_CACHE_SLOTS; i++) {
        if (jpeg_cache[i].timestamp < oldest_timestamp) {
            oldest_timestamp = jpeg_cache[i].timestamp;
            oldest_idx = i;
        }
    }

    return oldest_idx;
}

/**
 * @brief Evict (remove) a cache entry
 * @param slot_index Index of the slot to evict
 */
static void jpeg_cache_evict_slot(int slot_index)
{
    if (slot_index < 0 || slot_index >= JPEG_CACHE_SLOTS) {
        return;
    }

    jpeg_cache_entry_t *entry = &jpeg_cache[slot_index];

    if (entry->path) {
        ESP_LOGI(TAG, "Cache EVICT [%d]: %s (%ux%u, %u bytes)",
                 slot_index, entry->path, entry->width, entry->height, (unsigned int)entry->buffer_size);
        free(entry->path);
        entry->path = NULL;
    }

    if (entry->buffer) {
        heap_caps_free(entry->buffer);
        entry->buffer = NULL;
    }

    entry->timestamp = 0;
    entry->width = 0;
    entry->height = 0;
    entry->buffer_size = 0;
    entry->valid = false;
}

/**
 * @brief Invalidate (clear) a specific image from the cache by path
 * @param path File path to invalidate
 */
void jpeg_cache_invalidate(const char *path) {
    if (!path)
        return;

    for (int i = 0; i < JPEG_CACHE_SLOTS; i++) {
        if (jpeg_cache[i].valid && jpeg_cache[i].path && strcmp(jpeg_cache[i].path, path) == 0) {
            ESP_LOGI(TAG, "Cache INVALIDATE [%d]: %s", i, path);
            jpeg_cache_evict_slot(i);
            return;
        }
    }
    ESP_LOGD(TAG, "Cache invalidate: %s not found in cache", path);
}

/**
 * @brief Add a decoded JPEG to the cache
 * @param path File path
 * @param buffer Decoded image buffer (ownership transferred to cache)
 * @param width Image width
 * @param height Image height
 * @param size Buffer size in bytes
 */
static void jpeg_cache_add(const char *path, uint8_t *buffer, uint16_t width, uint16_t height, size_t size)
{
    if (!path || !buffer) {
        return;
    }

    // Only cache 240x240 images
    if (width != JPEG_CACHE_TARGET_SIZE || height != JPEG_CACHE_TARGET_SIZE) {
        ESP_LOGD(TAG, "Not caching %s: size %ux%u != %dx%d",
                 path, width, height, JPEG_CACHE_TARGET_SIZE, JPEG_CACHE_TARGET_SIZE);
        return;
    }

    // Find a slot (prefer empty, otherwise evict oldest)
    int slot_idx = jpeg_cache_find_oldest_slot();

    // Evict if needed
    if (jpeg_cache[slot_idx].valid) {
        jpeg_cache_evict_slot(slot_idx);
    }

    // Add new entry
    jpeg_cache_entry_t *entry = &jpeg_cache[slot_idx];
    entry->path = strdup_spiram(path);
    entry->buffer = buffer;
    entry->width = width;
    entry->height = height;
    entry->buffer_size = size;
    entry->timestamp = cache_timestamp_counter++;
    entry->valid = true;

    ESP_LOGI(TAG, "Cache ADD [%d]: %s (%ux%u, %u bytes) @ %p",
             slot_idx, path, width, height, (unsigned int)size, buffer);
}

/**
 * @brief Initialize the PNG cache system
 */
void png_cache_init(void)
{
    for (int i = 0; i < PNG_CACHE_SLOTS; i++) {
        png_cache[i].path = NULL;
        png_cache[i].buffer = NULL;
        png_cache[i].timestamp = 0;
        png_cache[i].width = 0;
        png_cache[i].height = 0;
        png_cache[i].file_size = 0;
        png_cache[i].valid = false;
    }
    png_cache_timestamp_counter = 0;
    png_cache_hits = 0;
    png_cache_misses = 0;
    png_cache_total_bytes = 0;
    ESP_LOGI(TAG, "PNG cache initialized: %d slots (max %d KB per file)",
             PNG_CACHE_SLOTS, PNG_CACHE_MAX_FILE_SIZE / 1024);
}

/**
 * @brief Find a PNG in the cache by path
 * @param path File path to search for
 * @return Pointer to cache entry if found, NULL otherwise
 */
static png_cache_entry_t *png_cache_find(const char *path)
{
    if (!path) return NULL;

    for (int i = 0; i < PNG_CACHE_SLOTS; i++) {
        if (png_cache[i].valid &&
            png_cache[i].path &&
            strcmp(png_cache[i].path, path) == 0) {
            // Update timestamp on access (LRU)
            png_cache[i].timestamp = png_cache_timestamp_counter++;
            png_cache_hits++;
            ESP_LOGI(TAG, "PNG Cache HIT [%d]: %s (%ux%u, %u bytes) (hits=%u, misses=%u, ratio=%.1f%%)",
                     i, path, png_cache[i].width, png_cache[i].height, (unsigned int)png_cache[i].file_size,
                     png_cache_hits, png_cache_misses,
                     (100.0f * png_cache_hits) / (png_cache_hits + png_cache_misses));
            return &png_cache[i];
        }
    }

    png_cache_misses++;
    ESP_LOGI(TAG, "PNG Cache MISS: %s (hits=%u, misses=%u)", path, png_cache_hits, png_cache_misses);
    return NULL;
}

/**
 * @brief Find the oldest (least recently used) PNG cache slot
 * @return Slot index of the LRU entry
 */
static int png_cache_find_oldest_slot(void)
{
    int oldest_idx = 0;
    uint32_t oldest_timestamp = UINT32_MAX;

    // First, prefer invalid (empty) slots
    for (int i = 0; i < PNG_CACHE_SLOTS; i++) {
        if (!png_cache[i].valid) {
            return i;
        }
    }

    // If all slots are valid, find the one with smallest timestamp
    for (int i = 0; i < PNG_CACHE_SLOTS; i++) {
        if (png_cache[i].timestamp < oldest_timestamp) {
            oldest_timestamp = png_cache[i].timestamp;
            oldest_idx = i;
        }
    }

    return oldest_idx;
}

/**
 * @brief Evict (remove) a PNG cache entry
 * @param slot_index Index of the slot to evict
 */
static void png_cache_evict_slot(int slot_index)
{
    if (slot_index < 0 || slot_index >= PNG_CACHE_SLOTS) {
        return;
    }

    png_cache_entry_t *entry = &png_cache[slot_index];

    if (entry->path) {
        ESP_LOGI(TAG, "PNG Cache EVICT [%d]: %s (%ux%u, %u bytes)",
                 slot_index, entry->path, entry->width, entry->height, (unsigned int)entry->file_size);
        free(entry->path);
        entry->path = NULL;
    }

    if (entry->buffer) {
        heap_caps_free(entry->buffer);
        entry->buffer = NULL;
    }

    entry->timestamp = 0;
    entry->width = 0;
    entry->height = 0;
    entry->file_size = 0;
    entry->valid = false;
}

/**
 * @brief Invalidate (clear) a specific PNG from the cache by path
 * @param path File path to invalidate
 */
void png_cache_invalidate(const char *path) {
    if (!path)
        return;

    for (int i = 0; i < PNG_CACHE_SLOTS; i++) {
        if (png_cache[i].valid && png_cache[i].path && strcmp(png_cache[i].path, path) == 0) {
            ESP_LOGI(TAG, "PNG Cache INVALIDATE [%d]: %s", i, path);
            png_cache_total_bytes -= png_cache[i].file_size;
            png_cache_evict_slot(i);
            return;
        }
    }
    ESP_LOGD(TAG, "PNG cache invalidate: %s not found in cache", path);
}

/**
 * @brief Add a PNG to the cache
 * @param path File path
 * @param buffer Raw PNG file data (ownership transferred to cache)
 * @param width PNG width
 * @param height PNG height
 * @param file_size PNG file size in bytes
 */
static void png_cache_add(const char *path, uint8_t *buffer, uint16_t width, uint16_t height, size_t file_size)
{
    if (!path || !buffer) {
        return;
    }

    // Only cache if file size is within limit
    if (file_size > PNG_CACHE_MAX_FILE_SIZE) {
        ESP_LOGD(TAG, "Not caching %s: file size %u > max %d",
                 path, (unsigned int)file_size, PNG_CACHE_MAX_FILE_SIZE);
        return;
    }

    // Find a slot (prefer empty, otherwise evict oldest)
    int slot_idx = png_cache_find_oldest_slot();

    // Evict if needed (update total_bytes)
    if (png_cache[slot_idx].valid) {
        png_cache_total_bytes -= png_cache[slot_idx].file_size;
        png_cache_evict_slot(slot_idx);
    }

    // Add new entry
    png_cache_entry_t *entry = &png_cache[slot_idx];
    entry->path = strdup_spiram(path);
    entry->buffer = buffer;
    entry->width = width;
    entry->height = height;
    entry->file_size = file_size;
    entry->timestamp = png_cache_timestamp_counter++;
    entry->valid = true;

    png_cache_total_bytes += file_size;

    ESP_LOGI(TAG, "PNG Cache ADD [%d]: %s (%ux%u, %u bytes) @ %p (total cache: %u KB)",
             slot_idx, path, width, height, (unsigned int)file_size, buffer,
             (unsigned int)(png_cache_total_bytes / 1024));
}

/**
 * @brief Create fallback image data for different content types
 * @param content_type The type of content that failed to load
 * @param out_buf Pointer to store the generated fallback image buffer
 * @param out_dsc Pointer to store the image descriptor
 * @return ESP_OK on success, ESP_FAIL on failure
 */
static esp_err_t create_fallback_image(content_type_t content_type, uint8_t **out_buf, lv_img_dsc_t *out_dsc)
{
    uint16_t width, height;
    bool is_badge = false;

    // Determine size and type based on content type
    switch (content_type) {
        case CONTENT_TYPE_LANGUAGE_BADGE:
        case CONTENT_TYPE_BATTERY_BADGE:
        case CONTENT_TYPE_PLAYER_BADGE:
        case CONTENT_TYPE_BT_BADGE:
            // Small badge fallback
            width = 24;
            height = 24;
            is_badge = true;
            break;

        case CONTENT_TYPE_COVER:
            // Medium cover fallback
            width = 80;
            height = 80;
            break;

        case CONTENT_TYPE_POPUP:
        case CONTENT_TYPE_MENU:
        default:
            // Large screen fallback
            width = 120;
            height = 120;
            break;
    }

    // Calculate buffer size for RGB565 format
    size_t buffer_size = width * height * 2;  // 2 bytes per pixel for RGB565

    // Allocate buffer
    *out_buf = heap_caps_malloc(buffer_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!*out_buf) {
        ESP_LOGE(TAG, "Failed to allocate fallback image buffer");
        return ESP_FAIL;
    }

    uint16_t *pixel_data = (uint16_t *)*out_buf;

    if (is_badge) {
        // Badge fallback: white background with red X
        uint16_t white = 0xFFFF;  // RGB565 white (same after swap)
        uint16_t red = 0x00F8;    // RGB565 red with LV_COLOR_16_SWAP
        uint16_t black = 0x0000;  // RGB565 black (same after swap)

        // Fill with white background
        for (int y = 0; y < height; y++) {
            for (int x = 0; x < width; x++) {
                pixel_data[y * width + x] = white;
            }
        }

        // Draw red X in the center
        int center_x = width / 2;
        int center_y = height / 2;
        int x_size = width / 3;  // Size of the X

        for (int i = -x_size/2; i <= x_size/2; i++) {
            // Main diagonal
            if (center_x + i >= 0 && center_x + i < width && center_y + i >= 0 && center_y + i < height) {
                pixel_data[(center_y + i) * width + (center_x + i)] = red;
            }
            // Anti-diagonal
            if (center_x + i >= 0 && center_x + i < width && center_y - i >= 0 && center_y - i < height) {
                pixel_data[(center_y - i) * width + (center_x + i)] = red;
            }
        }

        // Add black border
        for (int x = 0; x < width; x++) {
            pixel_data[0 * width + x] = black;                    // Top border
            pixel_data[(height-1) * width + x] = black;           // Bottom border
        }
        for (int y = 0; y < height; y++) {
            pixel_data[y * width + 0] = black;                    // Left border
            pixel_data[y * width + (width-1)] = black;            // Right border
        }

    } else {
        // Full screen fallback: white background with red X (matches badge pattern)
        uint16_t red = 0x00F8;    // RGB565 red with LV_COLOR_16_SWAP
        uint16_t white = 0xFFFF;  // RGB565 white (same after swap)

        // Fill with white background
        for (int y = 0; y < height; y++) {
            for (int x = 0; x < width; x++) {
                pixel_data[y * width + x] = white;
            }
        }

        // Draw large red X
        int center_x = width / 2;
        int center_y = height / 2;
        int x_size = width * 2 / 3;  // Larger X for full screen

        for (int i = -x_size/2; i <= x_size/2; i++) {
            // Main diagonal (thicker line)
            for (int thickness = -2; thickness <= 2; thickness++) {
                int px = center_x + i;
                int py = center_y + i + thickness;
                if (px >= 0 && px < width && py >= 0 && py < height) {
                    pixel_data[py * width + px] = red;
                }
            }
            // Anti-diagonal (thicker line)
            for (int thickness = -2; thickness <= 2; thickness++) {
                int px = center_x + i;
                int py = center_y - i + thickness;
                if (px >= 0 && px < width && py >= 0 && py < height) {
                    pixel_data[py * width + px] = red;
                }
            }
        }
    }

    // Set up image descriptor
    out_dsc->header.cf = LV_IMG_CF_TRUE_COLOR;  // RGB565
    out_dsc->header.w = width;
    out_dsc->header.h = height;
    out_dsc->data_size = buffer_size;
    out_dsc->data = *out_buf;

    ESP_LOGI(TAG, "Created fallback image for %s: %dx%d, %s style",
             content_type_names[content_type], width, height, is_badge ? "badge" : "full-screen");

    return ESP_OK;
}

static esp_err_t load_jpeg_into_buffer(const char       *path, uint8_t **out_buf, lv_img_dsc_t *out_dsc, bool *is_cached)
{
    // Check if path matches existing resource
    if (out_dsc->path && strcmp(out_dsc->path, path) == 0) {
        ESP_LOGI(TAG, "JPEG %s already loaded, reusing buffer @ %p", path, *out_buf);
        return ESP_OK;
    }

    // Try to find in cache first (ONLY if is_cached tracking is enabled)
    // If is_cached is NULL, caller doesn't want cached buffers (manages own memory)
    jpeg_cache_entry_t *cached = NULL;
    if (is_cached) {
        cached = jpeg_cache_find(path);
    }

    if (cached) {
        // Cache hit - reuse the cached buffer
        // Free old path and buffer if different (only if owned, not cached)
        if (out_dsc->path) {
            free(out_dsc->path);
            out_dsc->path = NULL;
        }
        if (*out_buf && is_cached && !(*is_cached)) {
            // Only free if we own the buffer (not borrowed from cache)
            heap_caps_free(*out_buf);
            *out_buf = NULL;
        }

        // Point to cached buffer (no ownership transfer)
        *out_buf = cached->buffer;

#if LVGL_VERSION_MAJOR == 8
        out_dsc->header.cf = LV_IMG_CF_TRUE_COLOR;
        out_dsc->header.w  = cached->width;
        out_dsc->header.h  = cached->height;
#endif
        out_dsc->data_size = cached->buffer_size;
        out_dsc->data      = cached->buffer;
        out_dsc->path      = strdup_spiram(path);

        // Mark as cached (borrowed from cache)
        if (is_cached) {
            *is_cached = true;
        }

        return ESP_OK;
    }

    // Cache miss - need to load and decode
    // Free old path and buffer if different (only if owned, not cached)
    if (out_dsc->path) {
        ESP_LOGI(TAG, "Freeing old resource: %s", out_dsc->path);
        free(out_dsc->path);
        out_dsc->path = NULL;
    }
    if (*out_buf && is_cached && !(*is_cached)) {
        // Only free if we own the buffer (not borrowed from cache)
        heap_caps_free(*out_buf);
        *out_buf = NULL;
    }

    // Load new resource
    struct stat st;
    if (stat(path, &st) != 0) {
        ESP_LOGE(TAG, "stat %s failed", path);
        return ESP_ERR_NOT_FOUND;
    }

    size_t in_len = st.st_size;

    FILE *f = s3_fopen(path, "rb");
    if (!f) { ESP_LOGE(TAG, "open %s failed", path); return ESP_ERR_NOT_FOUND; }

    uint8_t *jpg_data = malloc(in_len);
    if (!jpg_data) { s3_fclose(f); return ESP_ERR_NO_MEM; }

    s3_fread(jpg_data, 1, in_len, f);
    s3_fclose(f);

    jpeg_dec_config_t cfg = DEFAULT_JPEG_DEC_CONFIG();
#if LV_COLOR_16_SWAP
    cfg.output_type = JPEG_RAW_TYPE_RGB565_BE;
#else
    cfg.output_type = JPEG_RAW_TYPE_RGB565_LE;
#endif
    jpeg_dec_handle_t h = jpeg_dec_open(&cfg);
    if (!h) { free(jpg_data); return ESP_FAIL; }

    jpeg_dec_io_t io = { .inbuf = jpg_data, .inbuf_len = in_len };
    jpeg_dec_header_info_t info = {0};
    if (jpeg_dec_parse_header(h, &io, &info) < 0) {
        jpeg_dec_close(h); free(jpg_data); return ESP_FAIL;
    }

    size_t out_len = info.width * info.height * 2;          /* RGB565 */

    *out_buf = heap_caps_aligned_alloc(16, out_len,
                                       MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!*out_buf)
        *out_buf = heap_caps_aligned_alloc(16, out_len, MALLOC_CAP_8BIT);
    if (!*out_buf) {
        jpeg_dec_close(h); free(jpg_data); return ESP_ERR_NO_MEM;
    }

    io.outbuf = *out_buf;
    if (jpeg_dec_process(h, &io) < 0) {
        jpeg_dec_close(h); free(jpg_data); heap_caps_free(*out_buf); *out_buf = NULL;
        return ESP_FAIL;
    }
    jpeg_dec_close(h);
    free(jpg_data);

#if LVGL_VERSION_MAJOR == 8
    out_dsc->header.cf = LV_IMG_CF_TRUE_COLOR;
    out_dsc->header.w  = info.width;
    out_dsc->header.h  = info.height;
#endif
    out_dsc->data_size = out_len;
    out_dsc->data      = *out_buf;

    // Save the path for future reuse checking
    out_dsc->path = strdup_spiram(path);

    ESP_LOGI(TAG, "JPEG %s → %ux%u (%u B) @ %p", path, info.width, info.height,
             (unsigned int)out_len, *out_buf);

    // Try to add to cache if it's 240x240 AND caller supports cache borrowing (is_cached != NULL)
    // Only legacy buffers (jpg_buf, icon_buf) support borrowing from cache
    // Content buffers manage their own memory and shouldn't be cached to avoid duplication
    if (is_cached && info.width == JPEG_CACHE_TARGET_SIZE && info.height == JPEG_CACHE_TARGET_SIZE) {
        // For legacy buffers: add current buffer to cache (transfer ownership to cache)
        // The next time this image is requested, cache will provide it
        jpeg_cache_add(path, *out_buf, info.width, info.height, out_len);

        // Mark that this buffer is now owned by cache (don't free on cleanup)
        *is_cached = true;

        ESP_LOGI(TAG, "Added to cache [%s]: buffer @ %p now managed by cache", path, *out_buf);
    } else if (is_cached) {
        // Mark as owned (not cached) for non-240x240 images
        *is_cached = false;
    }

    return ESP_OK;
}

esp_err_t lvgl_load_image_from_sdcard(const char *path)
{
    return load_jpeg_into_buffer(path, &jpg_buf, &jpg_dsc, &jpg_buf_is_cached);
}

esp_err_t lvgl_load_icon_from_sdcard(const char *path)
{
    return load_jpeg_into_buffer(path, &icon_buf, &icon_dsc, &icon_buf_is_cached);
}

const lv_img_dsc_t *lvgl_get_icon(void)
{
    return &icon_dsc;
}

const lv_img_dsc_t *lvgl_get_img(void)
{
    return &jpg_dsc;
}

esp_err_t lvgl_load_gif_from_sdcard(const char *path)
{
    // Check if path matches existing resource
    if (gif_dsc.path && strcmp(gif_dsc.path, path) == 0) {
        ESP_LOGI(TAG, "GIF %s already loaded, reusing buffer @ %p", path, gif_buf);
        return ESP_OK;
    }

    // Free old path and buffer if different
    if (gif_dsc.path) {
        ESP_LOGI(TAG, "Freeing old GIF resource: %s", gif_dsc.path);
        free(gif_dsc.path);
        gif_dsc.path = NULL;
    }
    if (gif_buf) {
        heap_caps_free(gif_buf);
        gif_buf = NULL;
    }

    struct stat st;
    if (stat(path, &st) != 0 || st.st_size <= 0) {
        ESP_LOGE(TAG, "Failed to stat %s or invalid file size", path);
        return ESP_FAIL;
    }

    long sz = st.st_size;

    FILE *f = s3_fopen(path, "rb");
    if (!f) {
        ESP_LOGE(TAG, "Failed to open %s", path);
        return ESP_FAIL;
    }

    uint8_t *data = heap_caps_aligned_alloc(16, sz, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!data) {
        data = heap_caps_aligned_alloc(16, sz, MALLOC_CAP_8BIT);
        if (!data) {
            s3_fclose(f);
            ESP_LOGE(TAG, "Memory allocation failed");
            return ESP_ERR_NO_MEM;
        }
    }

    if (s3_fread(data, 1, sz, f) != sz) {
        s3_fclose(f);
        heap_caps_free(data);
        ESP_LOGE(TAG, "File read failed");
        return ESP_FAIL;
    }
    s3_fclose(f);

    gif_buf = data;

    gif_dsc.header.cf = LV_IMG_CF_RAW;
    gif_dsc.header.w = 0; // LVGL will parse
    gif_dsc.header.h = 0;
    gif_dsc.data_size = sz;
    gif_dsc.data = gif_buf;

    // Save the path for future reuse checking
    gif_dsc.path = strdup_spiram(path);

    ESP_LOGI(TAG, "GIF loaded: %u bytes, data at: %p", (unsigned int)sz, gif_buf);

    return ESP_OK;
}

const lv_img_dsc_t *lvgl_get_gif(void)
{
    return &gif_dsc;
}

// Simple PNG header parser to extract dimensions
static esp_err_t parse_png_header(const char *path, uint32_t *width, uint32_t *height)
{
    FILE *f = s3_fopen(path, "rb");
    if (!f) {
        return ESP_FAIL;
    }

    uint8_t header[24]; // PNG signature (8) + IHDR length (4) + IHDR type (4) + width (4) + height (4)
    if (s3_fread(header, 1, 24, f) != 24) {
        s3_fclose(f);
        return ESP_FAIL;
    }
    s3_fclose(f);

    // Check PNG signature
    uint8_t png_sig[8] = {0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A};
    if (memcmp(header, png_sig, 8) != 0) {
        return ESP_FAIL;
    }

    // Extract width and height from IHDR chunk (big endian)
    *width = (header[16] << 24) | (header[17] << 16) | (header[18] << 8) | header[19];
    *height = (header[20] << 24) | (header[21] << 16) | (header[22] << 8) | header[23];

    return ESP_OK;
}

static esp_err_t load_png_into_buffer(const char *path, uint8_t **out_buf, lv_img_dsc_t *out_dsc, bool *is_cached)
{
    // Check if path matches existing resource
    if (out_dsc->path && strcmp(out_dsc->path, path) == 0) {
        ESP_LOGI(TAG, "PNG %s already loaded, reusing buffer @ %p", path, *out_buf);
        return ESP_OK;
    }

    // Try to find in cache first (ONLY if is_cached tracking is enabled)
    // If is_cached is NULL, caller doesn't want cached buffers (manages own memory)
    png_cache_entry_t *cached = NULL;
    if (is_cached) {
        cached = png_cache_find(path);
    }

    if (cached) {
        // Cache hit - reuse the cached buffer
        // Free old path and buffer if different (only if owned, not cached)
        if (out_dsc->path) {
            free(out_dsc->path);
            out_dsc->path = NULL;
        }
        if (*out_buf && is_cached && !(*is_cached)) {
            // Only free if we own the buffer (not borrowed from cache)
            heap_caps_free(*out_buf);
            *out_buf = NULL;
        }

        // Point to cached buffer (no ownership transfer)
        *out_buf = cached->buffer;

#if LVGL_VERSION_MAJOR == 8
        out_dsc->header.cf = LV_IMG_CF_UNKNOWN;  // Let LVGL decode PNG
        out_dsc->header.w  = cached->width;
        out_dsc->header.h  = cached->height;
#endif
        out_dsc->data_size = cached->file_size;
        out_dsc->data      = cached->buffer;
        out_dsc->path      = strdup_spiram(path);

        // Mark as cached (borrowed from cache)
        if (is_cached) {
            *is_cached = true;
        }

        return ESP_OK;
    }

    // Cache miss - need to load PNG file
    // Free old path and buffer if different (only if owned, not cached)
    if (out_dsc->path) {
        ESP_LOGI(TAG, "Freeing old PNG resource: %s", out_dsc->path);
        free(out_dsc->path);
        out_dsc->path = NULL;
    }
    if (*out_buf && is_cached && !(*is_cached)) {
        // Only free if we own the buffer (not borrowed from cache)
        heap_caps_free(*out_buf);
        *out_buf = NULL;
    }

    // Get file size using stat
    struct stat st;
    if (stat(path, &st) != 0 || st.st_size <= 0) {
        ESP_LOGE(TAG, "Failed to stat PNG file or invalid size: %s", path);
        return ESP_ERR_NOT_FOUND;
    }

    size_t file_size = st.st_size;

    // Parse PNG header to get dimensions
    uint32_t width = 0, height = 0;
    if (parse_png_header(path, &width, &height) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to parse PNG header: %s", path);
        return ESP_ERR_NOT_FOUND;
    }

    // Load PNG file
    FILE *f = s3_fopen(path, "rb");
    if (!f) {
        ESP_LOGE(TAG, "Failed to open PNG file: %s", path);
        return ESP_ERR_NOT_FOUND;
    }

    // Allocate memory for PNG data (try PSRAM first, then internal)
    *out_buf = heap_caps_aligned_alloc(16, file_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!*out_buf) {
        *out_buf = heap_caps_aligned_alloc(16, file_size, MALLOC_CAP_8BIT);
        if (!*out_buf) {
            s3_fclose(f);
            ESP_LOGE(TAG, "Failed to allocate memory for PNG data: %u bytes", (unsigned int)file_size);
            return ESP_ERR_NO_MEM;
        }
    }

    // Read PNG data into buffer
    if (s3_fread(*out_buf, 1, file_size, f) != file_size) {
        s3_fclose(f);
        heap_caps_free(*out_buf);
        *out_buf = NULL;
        ESP_LOGE(TAG, "Failed to read PNG data from file");
        return ESP_FAIL;
    }
    s3_fclose(f);

    // Set up PNG descriptor
#if LVGL_VERSION_MAJOR == 8
    out_dsc->header.cf = LV_IMG_CF_UNKNOWN;  // Let LVGL detect and decode PNG
    out_dsc->header.w = width;
    out_dsc->header.h = height;
#endif
    out_dsc->data_size = file_size;
    out_dsc->data      = *out_buf;

    // Save the path for future reuse checking
    out_dsc->path = strdup_spiram(path);

    ESP_LOGI(TAG, "PNG %s → %ux%u (file: %u bytes) @ %p", path, width, height, (unsigned int)file_size, *out_buf);

    // Try to add to cache if file size is within limit AND caller supports cache borrowing
    if (is_cached && file_size <= PNG_CACHE_MAX_FILE_SIZE) {
        // Transfer ownership to cache
        png_cache_add(path, *out_buf, width, height, file_size);

        // Mark that this buffer is now owned by cache (don't free on cleanup)
        *is_cached = true;

        ESP_LOGI(TAG, "Added to PNG cache [%s]: buffer @ %p now managed by cache", path, *out_buf);
    } else if (is_cached) {
        // Mark as owned (not cached) for oversized PNGs
        *is_cached = false;
    }

    return ESP_OK;
}

esp_err_t lvgl_load_png_from_sdcard(const char *path)
{
    return load_png_into_buffer(path, &png_buf, &png_dsc, &png_buf_is_cached);
}

const lv_img_dsc_t *lvgl_get_png(void)
{
    return &png_dsc;
}

#if 0 // #ifndef NO_LOTTIE
lv_obj_t *lvgl_load_lottie_from_sdcard(const char *path, uint16_t w, uint16_t h)
{
    ESP_LOGI(TAG, "Loading Lottie from: %s (%dx%d)", path, w, h);
    
    // Log memory before starting
    size_t free_heap = heap_caps_get_free_size(MALLOC_CAP_8BIT);
    size_t free_internal = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    ESP_LOGI(TAG, "Free heap: %u bytes, Internal: %u bytes", (unsigned int)free_heap, (unsigned int)free_internal);
    
    if (!path) {
        ESP_LOGE(TAG, "Invalid path: NULL");
        return NULL;
    }

    // Calculate required canvas size
    size_t canvas_size = w * h * 4;  // RGBA
    ESP_LOGI(TAG, "Canvas buffer size needed: %u bytes", (unsigned int)canvas_size);
    
    // Check if we have enough internal RAM (need some headroom)
    const size_t MIN_INTERNAL_RAM_RESERVE = 10 * 1024; // 10KB headroom
    if (free_internal < (canvas_size + MIN_INTERNAL_RAM_RESERVE)) {
        ESP_LOGW(TAG, "Insufficient internal RAM for Lottie canvas. Need %u, have %u",
                 (unsigned int)(canvas_size + MIN_INTERNAL_RAM_RESERVE), (unsigned int)free_internal);
        
        // Try smaller sizes until we find one that fits
        uint16_t new_w, new_h;
        for (int scale = 2; scale <= 4; scale++) {
            new_w = w / scale;
            new_h = h / scale;
            size_t new_canvas_size = new_w * new_h * 4;
            
            ESP_LOGI(TAG, "Trying smaller size: %dx%d (%u bytes)", new_w, new_h, (unsigned int)new_canvas_size);
            
            if (free_internal >= (new_canvas_size + MIN_INTERNAL_RAM_RESERVE)) {
                ESP_LOGI(TAG, "Using reduced size: %dx%d instead of %dx%d", new_w, new_h, w, h);
                w = new_w;
                h = new_h;
                canvas_size = new_canvas_size;
                break;
            }
        }
        
        // If still no luck, give up
        if (free_internal < (canvas_size + MIN_INTERNAL_RAM_RESERVE)) {
            ESP_LOGE(TAG, "Cannot fit Lottie canvas even with reduced size");
            return NULL;
        }
    }

    const size_t MAX_LOTTIE_FILE_SIZE = 64 * 1024;

    struct stat st;
    if (stat(path, &st) != 0) {
        ESP_LOGE(TAG, "Failed to stat Lottie file: %s", path);
        return NULL;
    }

    size_t size = st.st_size;
    ESP_LOGI(TAG, "Lottie file size: %u bytes", (unsigned int)size);

    if (size == 0 || size > MAX_LOTTIE_FILE_SIZE) {
        ESP_LOGE(TAG, "Invalid Lottie file size: %u bytes", (unsigned int)size);
        return NULL;
    }
    
    // Use properly aligned external RAM (PSRAM) for canvas buffer
    // Ensure 32-byte alignment for ThorVG performance and cache coherency
    canvas_fb = heap_caps_aligned_alloc(32, canvas_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!canvas_fb) {
        ESP_LOGW(TAG, "PSRAM allocation failed, trying internal RAM for canvas buffer");
        canvas_fb = heap_caps_aligned_alloc(32, canvas_size, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    }
    
    if (!canvas_fb) {
        ESP_LOGE(TAG, "Failed to allocate canvas buffer: %u bytes", (unsigned int)canvas_size);
        return NULL;
    }
    
    // Check which memory type was used
    if (heap_caps_get_allocated_size(canvas_fb) && 
        heap_caps_check_integrity(MALLOC_CAP_SPIRAM, true) &&
        ((uintptr_t)canvas_fb >= 0x3F800000)) {
        ESP_LOGI(TAG, "Canvas buffer allocated in PSRAM at %p (size: %u)", canvas_fb, (unsigned int)canvas_size);
    } else {
        ESP_LOGI(TAG, "Canvas buffer allocated in internal RAM at %p (size: %u)", canvas_fb, (unsigned int)canvas_size);
    }
    
    // Ensure buffer is aligned for ThorVG
    if ((uintptr_t)canvas_fb % 4 != 0) {
        ESP_LOGW(TAG, "Canvas buffer is not 4-byte aligned: %p", canvas_fb);
    }
    
    // Allocate Lottie data - can use PSRAM
    lottie_data = heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!lottie_data) {
        lottie_data = heap_caps_malloc(size, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    }

    if (!lottie_data) {
        ESP_LOGE(TAG, "Failed to allocate Lottie data: %u bytes", (unsigned int)size);
        heap_caps_free(canvas_fb);
        canvas_fb = NULL;
        return NULL;
    }
    ESP_LOGI(TAG, "Lottie data allocated at %p", lottie_data);

    // Read file again
    f = s3_fopen(path, "rb");
    if (!f) {
        ESP_LOGE(TAG, "Failed to reopen Lottie file");
        heap_caps_free(canvas_fb);
        heap_caps_free(lottie_data);
        canvas_fb = NULL;
        lottie_data = NULL;
        return NULL;
    }
    
    size_t read_size = s3_fread(lottie_data, 1, size, f);
    s3_fclose(f);

    if (read_size != size) {
        ESP_LOGE(TAG, "Failed to read complete file. Read %u of %u bytes", (unsigned int)read_size, (unsigned int)size);
        heap_caps_free(canvas_fb);
        heap_caps_free(lottie_data);
        canvas_fb = NULL;
        lottie_data = NULL;
        return NULL;
    }

    ESP_LOGI(TAG, "Creating Lottie object...");
    
    // Initialize ThorVG engine with C-style error handling
    lv_obj_t *lottie = lv_lottie_create(lv_scr_act());
    if (!lottie) {
        ESP_LOGE(TAG, "Failed to create Lottie object - lv_lottie_create returned NULL");
        heap_caps_free(canvas_fb);
        heap_caps_free(lottie_data);
        canvas_fb = NULL;
        lottie_data = NULL;
        return NULL;
    }
    
    // Add a small delay to let ThorVG initialize its threading
    vTaskDelay(pdMS_TO_TICKS(50));
    
    // Validate the created object
    if (!lv_obj_is_valid(lottie)) {
        ESP_LOGE(TAG, "Lottie object creation succeeded but object is invalid");
        lv_obj_del(lottie);  // Clean up the invalid object
        heap_caps_free(canvas_fb);
        heap_caps_free(lottie_data);
        canvas_fb = NULL;
        lottie_data = NULL;
        return NULL;
    }
    
    ESP_LOGI(TAG, "Lottie object created and validated successfully");

    ESP_LOGI(TAG, "Setting Lottie buffer (%dx%d)...", w, h);
    
    // Clear canvas buffer to prevent garbage data
    memset(canvas_fb, 0, canvas_size);
    
    // Add memory barrier to ensure buffer clearing is complete before use
    __asm__ __volatile__("memw" ::: "memory");
    
    ESP_LOGI(TAG, "Canvas buffer cleared");
    
    lv_lottie_set_buffer(lottie, w, h, canvas_fb);
    
    ESP_LOGI(TAG, "Setting Lottie source data...");
    
    // Let LVGL/ThorVG handle PSRAM access patterns internally
    
    lv_lottie_set_src_data(lottie, lottie_data, size);
    
    ESP_LOGI(TAG, "Configuring Lottie object...");
    lv_obj_set_size(lottie, w, h);
    
    // Add safety checks before centering
    if (lv_obj_is_valid(lottie)) {
        lv_obj_center(lottie);
        ESP_LOGI(TAG, "Lottie object centered");
    } else {
        ESP_LOGE(TAG, "Lottie object is invalid after configuration");
        // Cleanup
        heap_caps_free(canvas_fb);
        heap_caps_free(lottie_data);
        canvas_fb = NULL;
        lottie_data = NULL;
        return NULL;
    }

    // Validate animation with more checks
    lv_anim_t *anim = lv_lottie_get_anim(lottie);
    if (!anim) {
        ESP_LOGW(TAG, "Lottie animation is NULL - might be invalid");
    } else {
        ESP_LOGI(TAG, "Lottie animation created successfully");
        // Set conservative animation parameters to reduce memory pressure
        lv_anim_set_time(anim, 2000);  // Slower animation (2 seconds)
        lv_anim_set_repeat_count(anim, LV_ANIM_REPEAT_INFINITE);
        ESP_LOGI(TAG, "Animation parameters set: 2s loop, infinite repeat");
    }

    // Log final memory state
    size_t final_heap = heap_caps_get_free_size(MALLOC_CAP_8BIT);
    size_t final_internal = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    ESP_LOGI(TAG, "Final memory - Total: %u, Internal: %u", (unsigned int)final_heap, (unsigned int)final_internal);

    return lottie;
}
#endif // #ifndef NO_LOTTIE

/**
 * @brief Register an already-decoded buffer as content (avoids re-reading from SD card)
 * @param content_type The type of content to register
 * @param path The original file path (for cache matching)
 * @param decoded_buf Pre-decoded RGB565 buffer (ownership transferred to this function)
 * @param width Image width in pixels
 * @param height Image height in pixels
 * @return ESP_OK on success
 */
esp_err_t lvgl_set_content_buffer(content_type_t content_type,
                                   const char *path,
                                   uint8_t *decoded_buf,
                                   uint16_t width,
                                   uint16_t height)
{
    if (content_type >= CONTENT_TYPE_MAX) {
        ESP_LOGE(TAG, "Invalid content type: %d", content_type);
        return ESP_ERR_INVALID_ARG;
    }

    if (!decoded_buf || !path) {
        ESP_LOGE(TAG, "Invalid arguments: decoded_buf=%p, path=%p", decoded_buf, path);
        return ESP_ERR_INVALID_ARG;
    }

    // Free any existing buffer for this content type
    if (content_buf[content_type]) {
        ESP_LOGI(TAG, "Freeing existing [%s] buffer @ %p",
                 content_type_names[content_type], content_buf[content_type]);
        heap_caps_free(content_buf[content_type]);
        content_buf[content_type] = NULL;
    }

    // Free any existing path
    if (content_dsc[content_type].path) {
        free(content_dsc[content_type].path);
        content_dsc[content_type].path = NULL;
    }

    // Take ownership of the buffer
    content_buf[content_type] = decoded_buf;

    // Set up image descriptor
    size_t buffer_size = width * height * 2;  // RGB565 = 2 bytes per pixel
#if LVGL_VERSION_MAJOR == 8
    content_dsc[content_type].header.cf = LV_IMG_CF_TRUE_COLOR;
    content_dsc[content_type].header.w = width;
    content_dsc[content_type].header.h = height;
#endif
    content_dsc[content_type].data_size = buffer_size;
    content_dsc[content_type].data = decoded_buf;
    content_dsc[content_type].path = strdup_spiram(path);

    ESP_LOGI(TAG, "Registered pre-decoded [%s] buffer: %ux%u (%u B) @ %p from %s",
             content_type_names[content_type], width, height, (unsigned int)buffer_size, decoded_buf, path);

    return ESP_OK;
}

// Content-specific JPEG loader
esp_err_t lvgl_load_content_jpg(content_type_t content_type, const char *path)
{
    if (content_type >= CONTENT_TYPE_MAX) {
        ESP_LOGE(TAG, "Invalid content type: %d", content_type);
        return ESP_ERR_INVALID_ARG;
    }

    ESP_LOGI(TAG, "Loading JPEG content [%s] from: %s", content_type_names[content_type], path);

    // Try to load the actual image first (with cache support)
    esp_err_t result = load_jpeg_into_buffer(path, &content_buf[content_type], &content_dsc[content_type], &content_buf_is_cached[content_type]);

    if (result != ESP_OK) {
        ESP_LOGW(TAG, "Failed to load JPEG [%s] from %s, creating fallback",
                 content_type_names[content_type], path);

        // Free any partially allocated buffer and path
        if (content_dsc[content_type].path) {
            free(content_dsc[content_type].path);
            content_dsc[content_type].path = NULL;
        }
        if (content_buf[content_type]) {
            free(content_buf[content_type]);
            content_buf[content_type] = NULL;
        }
        if (result == ESP_ERR_NOT_FOUND) {
            ESP_LOGE(TAG, "Error loading JPEG [%s]: %s", content_type_names[content_type], esp_err_to_name(result));
            // Remove resource verion, so we can reload during data sync
            write_resource_version_to_file("1.0.0");
        }

        // Create fallback image
        result = create_fallback_image(content_type, &content_buf[content_type], &content_dsc[content_type]);
        if (result == ESP_OK) {
            // Save the path even for fallback image
            content_dsc[content_type].path = strdup_spiram(path);
            ESP_LOGI(TAG, "Successfully created fallback for [%s]", content_type_names[content_type]);
        } else {
            ESP_LOGE(TAG, "Failed to create fallback for [%s]", content_type_names[content_type]);
        }
    }

    return result;
}

// Content-specific PNG loader
esp_err_t lvgl_load_content_png(content_type_t content_type, const char *path)
{
    if (content_type >= CONTENT_TYPE_MAX) {
        ESP_LOGE(TAG, "Invalid content type: %d", content_type);
        return ESP_ERR_INVALID_ARG;
    }

    ESP_LOGI(TAG, "Loading PNG content [%s] from: %s", content_type_names[content_type], path);

    // Try to load the actual image first (with cache support)
    esp_err_t result = load_png_into_buffer(path, &content_buf[content_type], &content_dsc[content_type], &content_buf_is_cached[content_type]);

    if (result != ESP_OK) {
        ESP_LOGW(TAG, "Failed to load PNG [%s] from %s, creating fallback",
                 content_type_names[content_type], path);

        // Free any partially allocated buffer and path
        if (content_dsc[content_type].path) {
            free(content_dsc[content_type].path);
            content_dsc[content_type].path = NULL;
        }
        if (content_buf[content_type]) {
            free(content_buf[content_type]);
            content_buf[content_type] = NULL;
        }
        if (result == ESP_ERR_NOT_FOUND) {
            ESP_LOGE(TAG, "Error loading JPEG [%s]: %s", content_type_names[content_type], esp_err_to_name(result));
            // Remove resource verion, so we can reload during data sync
            write_resource_version_to_file("1.0.0");
        }

        // Create fallback image
        result = create_fallback_image(content_type, &content_buf[content_type], &content_dsc[content_type]);
        if (result == ESP_OK) {
            // Save the path even for fallback image
            content_dsc[content_type].path = strdup_spiram(path);
            ESP_LOGI(TAG, "Successfully created fallback for [%s]", content_type_names[content_type]);
        } else {
            ESP_LOGE(TAG, "Failed to create fallback for [%s]", content_type_names[content_type]);
        }
    }

    return result;
}

// Content-specific GIF loader
esp_err_t lvgl_load_content_gif(content_type_t content_type, const char *path)
{
    if (content_type >= CONTENT_TYPE_MAX) {
        ESP_LOGE(TAG, "Invalid content type: %d", content_type);
        return ESP_ERR_INVALID_ARG;
    }

    ESP_LOGI(TAG, "Loading GIF content [%s] from: %s", content_type_names[content_type], path);

    // Check if path matches existing resource
    if (content_dsc[content_type].path && strcmp(content_dsc[content_type].path, path) == 0) {
        ESP_LOGI(TAG, "GIF [%s] %s already loaded, reusing buffer @ %p",
                 content_type_names[content_type], path, content_buf[content_type]);
        return ESP_OK;
    }

    // Free old path and buffer if different
    if (content_dsc[content_type].path) {
        ESP_LOGI(TAG, "Freeing old GIF resource: %s", content_dsc[content_type].path);
        free(content_dsc[content_type].path);
        content_dsc[content_type].path = NULL;
    }
    if (content_buf[content_type]) {
        heap_caps_free(content_buf[content_type]);
        content_buf[content_type] = NULL;
    }

    struct stat st;
    if (stat(path, &st) != 0 || st.st_size <= 0) {
        ESP_LOGE(TAG, "Failed to stat %s or invalid file size", path);
        return ESP_FAIL;
    }

    long sz = st.st_size;

    FILE *f = s3_fopen(path, "rb");
    if (!f) {
        ESP_LOGE(TAG, "Failed to open %s", path);
        return ESP_FAIL;
    }

    uint8_t *data = heap_caps_aligned_alloc(16, sz, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!data) {
        data = heap_caps_aligned_alloc(16, sz, MALLOC_CAP_8BIT);
        if (!data) {
            s3_fclose(f);
            ESP_LOGE(TAG, "Memory allocation failed");
            return ESP_ERR_NO_MEM;
        }
    }

    if (s3_fread(data, 1, sz, f) != sz) {
        s3_fclose(f);
        heap_caps_free(data);
        ESP_LOGE(TAG, "File read failed");
        return ESP_FAIL;
    }
    s3_fclose(f);

    content_buf[content_type] = data;

    content_dsc[content_type].header.cf = LV_IMG_CF_RAW;
    content_dsc[content_type].header.w = 0; // LVGL will parse
    content_dsc[content_type].header.h = 0;
    content_dsc[content_type].data_size = sz;
    content_dsc[content_type].data = content_buf[content_type];

    // Save the path for future reuse checking
    content_dsc[content_type].path = strdup_spiram(path);

    ESP_LOGI(TAG, "GIF [%s] loaded: %u bytes, data at: %p",
             content_type_names[content_type], (unsigned int)sz, content_buf[content_type]);
    return ESP_OK;
}

// Content-specific descriptor getter
const lv_img_dsc_t *lvgl_get_content_dsc(content_type_t content_type)
{
    if (content_type >= CONTENT_TYPE_MAX) {
        ESP_LOGE(TAG, "Invalid content type: %d", content_type);
        return NULL;
    }
    
    if (!content_dsc[content_type].data) {
        ESP_LOGW(TAG, "No data loaded for content type [%s]", content_type_names[content_type]);
        return NULL;
    }
    
    return &content_dsc[content_type];
}

void lvgl_free_previous_buffer(void)
{
    // Free legacy buffers (only if owned, not borrowed from cache)
    if (jpg_buf && !jpg_buf_is_cached)  { heap_caps_free(jpg_buf);  jpg_buf  = NULL; }
    if (icon_buf && !icon_buf_is_cached) { heap_caps_free(icon_buf); icon_buf = NULL; }
    if (gif_buf && !gif_buf_is_cached)  { heap_caps_free(gif_buf);  gif_buf  = NULL; }
    if (png_buf && !png_buf_is_cached)  { heap_caps_free(png_buf);  png_buf  = NULL; }
    // if (lottie_data) { heap_caps_free(lottie_data); lottie_data = NULL; }
    // if (canvas_fb)   { heap_caps_free(canvas_fb);   canvas_fb  = NULL; }

    // Reset buffers to NULL even if cached (just don't free them)
    jpg_buf = NULL;
    icon_buf = NULL;
    gif_buf = NULL;
    png_buf = NULL;

    // Reset cached flags
    jpg_buf_is_cached = false;
    icon_buf_is_cached = false;
    gif_buf_is_cached = false;
    png_buf_is_cached = false;

    // Free legacy paths
    if (jpg_dsc.path)  { free(jpg_dsc.path);  jpg_dsc.path  = NULL; }
    if (icon_dsc.path) { free(icon_dsc.path); icon_dsc.path = NULL; }
    if (gif_dsc.path)  { free(gif_dsc.path);  gif_dsc.path  = NULL; }
    if (png_dsc.path)  { free(png_dsc.path);  png_dsc.path  = NULL; }

    memset(&jpg_dsc,  0, sizeof(jpg_dsc));
    memset(&icon_dsc, 0, sizeof(icon_dsc));
    memset(&gif_dsc,  0, sizeof(gif_dsc));
    memset(&png_dsc,  0, sizeof(png_dsc));

    // Free content-specific buffers and paths (only if owned, not cached)
    for (int i = 0; i < CONTENT_TYPE_MAX; i++) {
        if (content_buf[i] && !content_buf_is_cached[i]) {
            heap_caps_free(content_buf[i]);
        }
        content_buf[i] = NULL;
        content_buf_is_cached[i] = false;

        if (content_dsc[i].path) {
            free(content_dsc[i].path);
            content_dsc[i].path = NULL;
        }
        memset(&content_dsc[i], 0, sizeof(content_dsc[i]));
    }
}

bool lvgl_validate_gif_dsc(const lv_img_dsc_t *gif_dsc) {
    if (!gif_dsc || !gif_dsc->data || gif_dsc->data_size < 13) {
        ESP_LOGE("GIF_VALID", "Invalid gif_dsc: null or too small");
        return false;
    }

    const uint8_t *data = (const uint8_t *)gif_dsc->data;

    if (!(memcmp(data, "GIF89a", 6) == 0 || memcmp(data, "GIF87a", 6) == 0)) {
        ESP_LOGE("GIF_VALID", "Invalid GIF signature");
        return false;
    }

    uint16_t w = data[6] | (data[7] << 8);
    uint16_t h = data[8] | (data[9] << 8);
    if (w == 0 || h == 0) {
        ESP_LOGE("GIF_VALID", "Invalid dimensions in GIF header: width=%u height=%u", w, h);
        return false;
    }

    uint8_t packed = data[10];
    bool global_color_table_flag = packed & 0x80;
    uint8_t gct_size_value = packed & 0x07;

    if (global_color_table_flag) {
        size_t gct_size = 3 * (1 << (gct_size_value + 1));
        size_t required_header = 13 + gct_size;
        if (gif_dsc->data_size < required_header) {
            ESP_LOGE("GIF_VALID", "Data too small for global color table (needs %u bytes)", (unsigned int)required_header);
            return false;
        }
    }

    ESP_LOGI("GIF_VALID", "Valid GIF: %ux%u %s GCT (size = %d colors)",
             w, h,
             global_color_table_flag ? "with" : "without",
             global_color_table_flag ? (1 << (gct_size_value + 1)) : 0);

    return true;
}
