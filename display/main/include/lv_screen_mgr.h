//
// Created by Antonio_Pereira on 2025/5/20.
//

#ifndef LV_SCREEN_MGR_H
#define LV_SCREEN_MGR_H

// #include "lvgl.h"
#include "s3_definitions.h"

// Post-transition callback type
typedef void (*post_transition_cb_t)(void);

// Display management
lv_timer_t * init_screen_manager(bool use_carroucel);
void deinit_screen_manager(void);
void set_current_screen(s3_screens_t current_screen, s3_screens_t next_screen);
void set_last_transition_callback(post_transition_cb_t callback);
void set_first_transition_callback(post_transition_cb_t callback);
int get_current_screen(void);
int get_previous_screen(void);
void ui_change_language(void);
void enable_player_update(void);
void enable_lang_badge_update(void);
void enable_pause_update(void);
void enable_resume_update(void);

// Memory management
void *lv_malloc(size_t s);
void lv_free(void *p);
void *lv_realloc(void *p, size_t s);

int ui_get_language(void);
void ui_save_language(void);
void lv_wifi_helper(void);
void refresh_screen_display(void);  // Refresh the screen display
void lvgl_process_step(uint32_t delay_ms);
void lvgl_tick_inc_locked(uint32_t inc_ms);
#endif /* LV_SCREEN_MGR_H */
