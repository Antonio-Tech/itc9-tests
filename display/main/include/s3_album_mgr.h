#ifndef S3_ALBUM_MGR_H
#define S3_ALBUM_MGR_H

#include <stddef.h>
#include <stdbool.h>
#include "esp_err.h"
#include "s3_definitions.h"

/* === PUBLIC ALBUM MANAGER API === */

/* Album Manager Initialization */
esp_err_t s3_albums_init(void);
void s3_album_mgr_initialize(void);
bool s3_albums_dynamic_build(void);

/* Album Navigation (works with availability-based filtering) */
const s3_album_handler_t *s3_albums_get_current(void);
bool s3_albums_next(void);
bool s3_albums_prev(void);
size_t s3_albums_get_current_idx(void);
size_t s3_albums_get_current_size(void);

/* Direct Access Functions */
size_t s3_albums_get_size(void);
const s3_album_handler_t *s3_albums_get(size_t idx);

/* SKU Search Functions */
int s3_albums_find_by_sku(const char *sku);
size_t s3_albums_find_by_sku_lang(const char *sku);

/* Album Availability Status Functions */
bool s3_albums_check_availability(const char *sku);
void s3_albums_availability_cmd(); // Wrapper for CLI use

/* Cloud Data Management */
/* Renamed from s3_check_album_data to s3_albums_dynamic_build (already declared above) */
void s3_albums_update_available(const char **available_skus, size_t count);

/* Factory Reset Support */
bool s3_album_mgr_factory_reset_status(void);

/* Last Played Album Persistence */
esp_err_t s3_albums_save_last_played(const char *sku);
int s3_albums_restore_last_played(void);  // Returns album index (>=0) on success, or -1 if not found/error

/* === INTERNAL/ADVANCED API (use carefully) === */
/* These are exposed for specialized use cases - most code should use the public API above */

/* Album Search - for NFC and blankee album lookup */
s3_album_handler_t *s3_album_mgr_find_album_by_sku_universal(const char *sku);
s3_album_handler_t *s3_album_mgr_find_blankee_album_by_sku(const char *sku);
s3_album_handler_t *s3_album_mgr_find_blankee_album_by_uid(const uint8_t* uid);

/* === LEGACY COMPATIBILITY (only essential functions kept) === */
/* Functions still used by existing modules - minimal set kept for compatibility */

/* Dynamic album access (used by s3_check_album_data) */
size_t get_dynamic_albums_size(void);
s3_album_handler_t *get_dynamic_album_by_index(size_t index);
s3_album_handler_t *find_dynamic_album_by_sku(const char *sku);
esp_err_t get_album_list_from_cloud(void);

/* Kidspack access (used throughout system for global album access) */
size_t get_kidspack_albums_size(void);
s3_album_handler_t *get_kidspack_album_by_index(size_t index);
s3_album_handler_t *find_kidspack_album_by_sku(const char *sku);

/* NFC/Blankee functions (used by NFC module) */
s3_album_handler_t *find_nfc_album_by_sku(const char *sku);
s3_album_handler_t *find_blankee_album_by_sku(const char *sku);
s3_album_handler_t *find_blankee_album_by_uid(const uint8_t* uid);

/* Album Management Functions */
esp_err_t s3_album_mgr_add_defaults(void);
esp_err_t s3_album_mgr_load_from_cloud(void);
esp_err_t s3_album_mgr_enable_default_albums_for_factory_reset(void);
void s3_album_mgr_scan_sd_downloads(void);
void s3_album_mgr_clear_all(void);
void s3_album_mgr_print_all(void);
esp_err_t s3_album_mgr_get_from_account(void);
void s3_album_mgr_clear_dynamic_albums(void);
void s3_album_mgr_print_dynamic_albums(void);
void s3_album_mgr_create_dynamic_album(const char *sku, const char *language, const char *name, int files_available);

/* Legacy Functions (moved from s3_definitions.h) */
esp_err_t add_dynamic_albums(const char *sku, const char *language);
esp_err_t add_static_albums(void);  // Renamed from add_default_albums for consistency
void print_dynamic_albums(void);
void create_dynamic_album(const char *sku, const char *language, const char *name, int files_available);
void initialize_dynamic_albums(void);
esp_err_t add_kidspack_album(const char *sku, const char *language);
esp_err_t add_default_kidspack_albums(void);
void clear_kidspack_albums(void);
void print_kidspack_albums(void);
s3_album_handler_t* find_album_by_sku_universal(const char *sku);
void initialize_separated_albums(void);
void print_all_separated_albums(void);
esp_err_t get_albums_from_user_account(void);
esp_err_t enable_default_albums_for_factory_reset(void);
void scan_sd_and_update_downloaded_flags(void);
size_t get_album_list_size(void);
void print_albums(void);
void clear_albums(void);

#endif // S3_ALBUM_MGR_H
