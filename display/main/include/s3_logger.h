#ifndef S3_LOGGER_H
#define S3_LOGGER_H

// Set default logger type (can be -DUSE_S3_LOGGER=1 in your CMake or Makefile)
#ifndef USE_S3_LOGGER
#define USE_S3_LOGGER 0  // Set to 1 to enable S3 logger, 0 to disable
#endif

#include <stdio.h>
#include <stddef.h>
#include "esp_err.h"

#if USE_S3_LOGGER
// Initialize logging (call once, at startup)
esp_err_t s3_logger_init(const char *path);
// Close logging and cleanup mutex (call before SD card unmount/shutdown)
void s3_logger_close(void);
// Manually flush the log buffer to SD card
void s3_logger_flush_buffer(void);
// Test function to get vprintf call count
uint32_t s3_logger_get_call_count(void);

// SD card mutex management (used internally, delegates to s3_definitions)
void s3_logger_init_mutex(void);
void s3_logger_deinit_mutex(void);

// Thread-safe SD card file functions:
FILE   *s3_fopen(const char *path, const char *mode);
size_t  s3_fread(void *ptr, size_t size, size_t count, FILE *stream);
size_t  s3_fwrite(const void *ptr, size_t size, size_t count, FILE *stream);
int     s3_fclose(FILE *stream);
int     s3_remove(const char *path);
int     s3_rename(const char *oldpath, const char *newpath);
int     s3_fseek(FILE *stream, long offset, int whence);
#else
// Map your s3_* functions directly to standard ones (no logging task/queue/mutex)
static inline esp_err_t s3_logger_init(const char *path) { 
    (void)path; // Suppress unused parameter warning
    return ESP_ERR_NOT_SUPPORTED; // Indicates logging is disabled
}
static inline void s3_logger_close(void) {}
static inline void s3_logger_flush_buffer(void) {}

#define s3_fopen  fopen
#define s3_fread  fread
#define s3_fwrite fwrite
#define s3_fclose fclose
#define s3_remove remove
#define s3_rename rename
#define s3_fseek  fseek
#endif

// SD Card functions (available regardless of USE_S3_LOGGER)
void print_sdcard_contents(void);
void cache_sdcard_contents(void);

#endif // S3_LOGGER_H
