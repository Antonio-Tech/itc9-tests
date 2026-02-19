//
// Created by Shane_Hwang on 2025/7/11.
//

#include "cjson_psram_hooks.h"
#include "esp_heap_caps.h"
#include "cJSON.h"

static void *psram_malloc(size_t size)
{
	return heap_caps_malloc(size, MALLOC_CAP_SPIRAM);
}

static void psram_free(void *ptr)
{
	heap_caps_free(ptr);
}

void cjson_init_with_psram(void)
{
	cJSON_Hooks hooks = {
		.malloc_fn = psram_malloc,
		.free_fn = psram_free
	};
	cJSON_InitHooks(&hooks);
}