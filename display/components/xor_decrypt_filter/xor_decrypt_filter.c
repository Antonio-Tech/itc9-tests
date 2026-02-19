#include "xor_decrypt_filter.h"
#include "esp_log.h"
#include <string.h> // for strlen, memcpy
#include <stdlib.h> // for calloc, free
#include "audio_element.h" // for AEL_IO_ABORT constant
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "XOR_DECRYPT_FILTER";

// Define the hardcoded XOR decryption key here.
const char XOR_HARDCODED_KEY[] = "a6cf4c1ef7f43251e673e8578a481a26";
const size_t XOR_HARDCODED_KEY_LEN = sizeof(XOR_HARDCODED_KEY) - 1; // -1 to exclude the null terminator

// Define a buffer size for processing. This affects memory usage and performance.
//#define XOR_FILTER_BUFFER_SIZE (8192)
#define XOR_FILTER_BUFFER_SIZE (4096)

/**
 * @brief Structure to store private data for the XOR decrypt filter.
 * We now include a pointer for our pre-allocated buffer.
 */
typedef struct {
    size_t current_offset; /*!< Current offset in the XOR key stream, to maintain state. */
    char   *buffer;        /*!< Pointer to the pre-allocated buffer for processing. */
} xor_filter_priv_data_t;

/**
 * @brief Audio element 'open' callback function.
 * Here we allocate the buffer that will be used throughout the element's lifecycle.
 */
static esp_err_t _xor_decrypt_open(audio_element_handle_t self) {
    ESP_LOGI(TAG, "XOR Decrypt Filter Opening...");
    xor_filter_priv_data_t *priv_data = (xor_filter_priv_data_t *)audio_element_getdata(self);
    if (!priv_data) {
        ESP_LOGE(TAG, "Private data is NULL in open function.");
        return ESP_FAIL;
    }

    // Allocate the buffer once when the element is opened.
    if (priv_data->buffer == NULL) {
        priv_data->buffer = (char *)malloc(XOR_FILTER_BUFFER_SIZE);
        if (priv_data->buffer == NULL) {
            ESP_LOGE(TAG, "Failed to allocate buffer in open function");
            return ESP_FAIL;
        }
    }

    // Reset the key stream offset whenever a new stream is opened.
    priv_data->current_offset = 0;
    ESP_LOGI(TAG, "XOR Decrypt Filter Opened, buffer allocated, offset reset to %u", (unsigned int)priv_data->current_offset);
    return ESP_OK;
}

/**
 * @brief Audio element 'close' callback function.
 * We free the buffer here, as it's the counterpart to the 'open' function.
 */
static esp_err_t _xor_decrypt_close(audio_element_handle_t self) {
    ESP_LOGI(TAG, "XOR Decrypt Filter Closing...");
    xor_filter_priv_data_t *priv_data = (xor_filter_priv_data_t *)audio_element_getdata(self);
    if (priv_data && priv_data->buffer) {
        free(priv_data->buffer);
        priv_data->buffer = NULL; // Set to NULL to prevent double-free
    }
    // Also reset offset
    if (priv_data) {
        priv_data->current_offset = 0;
    }
    ESP_LOGI(TAG, "XOR Decrypt Filter Closed, buffer freed.");
    return ESP_OK;
}

/**
 * @brief Audio element 'process' callback function for the filter.
 * Now it uses the pre-allocated buffer from private data.
 */
static int _xor_decrypt_process(audio_element_handle_t self) {
    // Retrieve private data
    xor_filter_priv_data_t *priv_data = (xor_filter_priv_data_t *)audio_element_getdata(self);

    // Use the pre-allocated buffer.
    char *buffer = priv_data->buffer;
    
    // Read data from the upstream element into our buffer.
    int r_size = audio_element_input(self, buffer, XOR_FILTER_BUFFER_SIZE);
    int w_size = 0;

    if (r_size > 0) {
        // Data was read, now perform XOR decryption with 4-byte alignment optimization.
        uint8_t *data = (uint8_t *)buffer;
        int processed = 0;
        
        // Process 4 bytes at a time for better performance
        while (processed + 4 <= r_size) {
            uint32_t *data32 = (uint32_t *)(data + processed);
            uint32_t key32 = 0;
            
            // Build 4-byte XOR key
            for (int j = 0; j < 4; j++) {
                key32 |= ((uint32_t)XOR_HARDCODED_KEY[(priv_data->current_offset + j) % XOR_HARDCODED_KEY_LEN]) << (j * 8);
            }
            
            // XOR 4 bytes at once
            *data32 ^= key32;
            
            processed += 4;
            priv_data->current_offset += 4;
        }
        
        // Handle remaining bytes (less than 4)
        for (int i = processed; i < r_size; i++) {
            data[i] = data[i] ^ XOR_HARDCODED_KEY[priv_data->current_offset % XOR_HARDCODED_KEY_LEN];
            priv_data->current_offset++;
        }
        
        // Write the decrypted data to the downstream element.
        w_size = audio_element_output(self, buffer, r_size);
        if (w_size < 0) {
            if (w_size == AEL_IO_ABORT) {
                ESP_LOGD(TAG, "Output aborted during pipeline shutdown: %d", w_size);
            } else {
                ESP_LOGE(TAG, "Error writing to output: %d", w_size);
            }
        }
    } else {
        // No data read. This could be end-of-stream (AEL_IO_DONE) or other states.
        // We pass the return code along to the pipeline.
        ESP_LOGW(TAG, "No data read from input: %d", r_size);
        w_size = r_size;
    }
    
    return w_size;
}

/**
 * @brief Audio element 'destroy' callback function.
 * Free all allocated memory as a final cleanup.
 */
static esp_err_t _xor_decrypt_destroy(audio_element_handle_t self) {
    ESP_LOGI(TAG, "XOR Decrypt Filter Destroyed");
    xor_filter_priv_data_t *priv_data = (xor_filter_priv_data_t *)audio_element_getdata(self);
    if (priv_data) {
        // As a safeguard, free the buffer if it hasn't been freed in 'close'.
        if (priv_data->buffer) {
            free(priv_data->buffer);
        }
        // Free the private data structure itself.
        free(priv_data);
    }
    return ESP_OK;
}

/**
 * @brief Initializes and creates an XOR decrypt audio element.
 */
audio_element_handle_t xor_decrypt_filter_init(const xor_decrypt_cfg_t *config) {
    audio_element_handle_t el;
    xor_filter_priv_data_t *priv_data;

    if (config == NULL) {
        ESP_LOGE(TAG, "Invalid config for XOR decrypt filter. Config pointer is NULL.");
        return NULL;
    }

    // Allocate memory for the private data structure.
    priv_data = (xor_filter_priv_data_t *)calloc(1, sizeof(xor_filter_priv_data_t));
    if (priv_data == NULL) {
        ESP_LOGE(TAG, "Failed to allocate private data for XOR decrypt filter");
        return NULL;
    }
    // Note: calloc initializes memory to zero, so priv_data->buffer is already NULL.

    // Set basic audio element configuration.
    audio_element_cfg_t cfg = DEFAULT_AUDIO_ELEMENT_CONFIG();
    cfg.tag = "XOR";
    cfg.open = _xor_decrypt_open;
    cfg.close = _xor_decrypt_close;
    cfg.destroy = _xor_decrypt_destroy;
    cfg.process = _xor_decrypt_process;
    cfg.read = NULL;
    cfg.write = NULL;
    cfg.task_stack = config->task_stack;
    cfg.task_prio = config->task_prio;
    cfg.task_core = config->task_core;
    cfg.stack_in_ext = true;

    el = audio_element_init(&cfg);
    if (el == NULL) {
        ESP_LOGE(TAG, "Failed to create XOR decrypt audio element");
        free(priv_data);
        return NULL;
    }

    audio_element_setdata(el, priv_data);
    
    // The output ringbuffer size is still needed.
    audio_element_set_output_ringbuf_size(el, config->out_rb_size);

    ESP_LOGI(TAG, "XOR Decrypt Filter Initialized Successfully");
    return el;
}
