#ifndef XOR_DECRYPT_FILTER_H
#define XOR_DECRYPT_FILTER_H

#include "audio_element.h" // Includes basic definitions for ESP-ADF audio elements
#include "audio_common.h"  // Includes common ESP-ADF definitions
#include <stdint.h>        // Ensures uint8_t is defined
#include <stddef.h>        // Ensures size_t is defined

#ifdef __cplusplus
extern "C" {
#endif

// Hardcoded XOR decryption key (as a string)
// This is your original hexadecimal string "a6cf4c1ef7f43251e673e8578a481a26"
// Its ASCII byte values will be used for XOR operations.
extern const char XOR_HARDCODED_KEY[];
extern const size_t XOR_HARDCODED_KEY_LEN; // The length of the key string

/**
 * @brief Configuration structure for the XOR decrypt filter.
 * This structure no longer contains the key, as the key is hardcoded.
 */
typedef struct {
    int     buf_size;     /*!< Size of the processing buffer. If 0, default size will be used. */
    int     out_rb_size;  /*!< Size of the output ring buffer. If 0, default size will be used. */
    int     task_stack;   /*!< Stack size for the filter task. */
    int     task_prio;    /*!< Priority for the filter task. */
    int     task_core;    /*!< CPU core for the filter task to run on. */
    bool    stack_in_ext; /*!< Whether to allocate the task stack in external memory. */
} xor_decrypt_cfg_t;

/**
 * @brief Default configuration for the XOR decrypt filter.
 * Provides a convenient macro to initialize the `xor_decrypt_cfg_t` structure.
 */
#define DEFAULT_XOR_DECRYPT_CONFIG() {                              \
    .buf_size = 512,                                                \
    .out_rb_size = 8 * 1024,                                        \
    .task_stack = 4 * 1024,                                         \
    .task_prio  = 5,                                                \
    .task_core  = 0,                                                \
    .stack_in_ext  = true,                                        \
}

/**
 * @brief Initializes and creates an XOR decrypt audio element.
 *
 * @param config Pointer to the `xor_decrypt_cfg_t` structure.
 * @return `audio_element_handle_t` on success, or NULL on failure.
 */
audio_element_handle_t xor_decrypt_filter_init(const xor_decrypt_cfg_t *config);

#ifdef __cplusplus
}
#endif

#endif // XOR_DECRYPT_FILTER_H
