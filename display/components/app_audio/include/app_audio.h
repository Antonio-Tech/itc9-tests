#ifndef APP_AUDIO_H
#define APP_AUDIO_H

#include "esp_err.h"

esp_err_t app_audio_init(void);
esp_err_t app_audio_deinit(void);
esp_err_t app_audio_play(const char *file);
esp_err_t app_audio_stop(void);

#endif // APP_AUDIO_H
