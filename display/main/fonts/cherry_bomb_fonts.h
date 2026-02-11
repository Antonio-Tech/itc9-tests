/**
 * @file cherry_bomb_fonts.h
 * @brief Cherry Bomb One custom fonts for LVGL
 * 
 * This file contains the declarations for Cherry Bomb One font in various sizes.
 * Used specifically for clock display with numbers 0-9, colon (:), and AM/PM indicators.
 */

#ifndef CHERRY_BOMB_FONTS_H
#define CHERRY_BOMB_FONTS_H

#ifdef __cplusplus
extern "C" {
#endif

#include "lvgl.h"

/* Font declarations */
extern const lv_font_t cherry_bomb_16;  // For testing - full ASCII range (size 16px)
extern const lv_font_t cherry_bomb_24;  // For AM/PM indicators (size 24px)
extern const lv_font_t cherry_bomb_48;  // For digits and colon (size 48px) 
extern const lv_font_t cherry_bomb_72;  // For digits and colon (size 72px) 
extern const lv_font_t cherry_bomb_90;  // For larger digits and colon (size 90px)

/* Font size defines for easy reference */
#define CHERRY_BOMB_FONT_TEST   (&cherry_bomb_16)  // For testing
#define CHERRY_BOMB_FONT_SMALL  (&cherry_bomb_24)  // For Clock AM/PM
#define CHERRY_BOMB_FONT_SMALL  (&cherry_bomb_48)  // For Player track list
#define CHERRY_BOMB_FONT_MEDIUM (&cherry_bomb_72)  // For Clock digits/colon
#define CHERRY_BOMB_FONT_LARGE  (&cherry_bomb_90)  // For Countdown digits/colon

#ifdef __cplusplus
} /*extern "C"*/
#endif

#endif /* CHERRY_BOMB_FONTS_H */
