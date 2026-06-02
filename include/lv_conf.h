/**
 * lv_conf.h — LVGL 8.4 configuration for HAXXcounter
 * Placed in project root so -DLV_CONF_INCLUDE_SIMPLE can find it.
 */

#ifndef LV_CONF_H
#define LV_CONF_H

#include <stdint.h>

/* =========================================================================
   COLOR
   ========================================================================= */
#define LV_COLOR_DEPTH 16

/* Swap RGB565 bytes so Arduino_GFX draw16bitBeRGBBitmap() gets big-endian */
#define LV_COLOR_16_SWAP 1

#define LV_COLOR_SCREEN_TRANSP 0
#define LV_COLOR_MIX_ROUND_OFS 0
#define LV_COLOR_CHROMA_KEY lv_color_hex(0x00ff00)

/* =========================================================================
   MEMORY
   ========================================================================= */
/* Use custom allocator so LVGL internal heap lives in PSRAM */
#define LV_MEM_CUSTOM 1
#if LV_MEM_CUSTOM == 0
    #define LV_MEM_SIZE       (48U * 1024U)
    #define LV_MEM_ADR        0
    #define LV_MEM_POOL_INCLUDE <stdlib.h>
    #define LV_MEM_POOL_ALLOC malloc
#else
    #define LV_MEM_CUSTOM_INCLUDE "esp_heap_caps.h"
    #define LV_MEM_CUSTOM_ALLOC(size) \
        heap_caps_malloc((size), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)
    #define LV_MEM_CUSTOM_FREE(ptr)   free(ptr)
    #define LV_MEM_CUSTOM_REALLOC(ptr, size) \
        heap_caps_realloc((ptr), (size), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)
#endif

#define LV_MEM_BUF_MAX_NUM 16
#define LV_MEMCPY_MEMSET_STD 0

/* =========================================================================
   HAL / TICK
   ========================================================================= */
/* Drive the LVGL tick from Arduino millis() — no need for lv_tick_inc() */
#define LV_TICK_CUSTOM 1
#if LV_TICK_CUSTOM
    #define LV_TICK_CUSTOM_INCLUDE       "Arduino.h"
    #define LV_TICK_CUSTOM_SYS_TIME_EXPR (millis())
#endif

#define LV_DEFAULT_REFR_PERIOD   33   /* ~30 fps */
#define LV_INDEV_DEF_READ_PERIOD 30

/* =========================================================================
   DRAWING
   ========================================================================= */
#define LV_DRAW_COMPLEX 1
#if LV_DRAW_COMPLEX
    #define LV_SHADOW_CACHE_SIZE    0
    #define LV_CIRCLE_CACHE_SIZE    4
#endif

#define LV_LAYER_SIMPLE_BUF_SIZE          (24U * 1024U)
#define LV_LAYER_SIMPLE_FALLBACK_BUF_SIZE (3U * 1024U)

#define LV_IMG_CACHE_DEF_SIZE 0
#define LV_GRADIENT_MAX_STOPS 2
#define LV_GRAD_CACHE_DEF_SIZE 0
#define LV_DITHER_GRADIENT 0
#define LV_DISP_ROT_MAX_BUF (10U * 1024U)

/* =========================================================================
   GPU
   ========================================================================= */
#define LV_USE_GPU_STM32_DMA2D 0
#define LV_USE_GPU_SWM341_DMA  0
#define LV_USE_GPU_NXP_PXP     0
#define LV_USE_GPU_NXP_VG_LITE 0
#define LV_USE_GPU_SDL         0

/* =========================================================================
   LOGGING
   ========================================================================= */
#define LV_USE_LOG 0
#if LV_USE_LOG
    #define LV_LOG_LEVEL LV_LOG_LEVEL_WARN
    #define LV_LOG_PRINTF 0
    #define LV_LOG_USE_TIMESTAMP 1
    #define LV_LOG_USE_FILE_LINE 1
#endif

/* =========================================================================
   ASSERTS
   ========================================================================= */
#define LV_USE_ASSERT_NULL          1
#define LV_USE_ASSERT_MALLOC        1
#define LV_USE_ASSERT_STYLE         0
#define LV_USE_ASSERT_MEM_INTEGRITY 0
#define LV_USE_ASSERT_OBJ           0

#define LV_ASSERT_HANDLER_INCLUDE <stdint.h>
#define LV_ASSERT_HANDLER while(1);

/* =========================================================================
   DEBUG
   ========================================================================= */
#define LV_USE_PERF_MONITOR 0
#if LV_USE_PERF_MONITOR
    #define LV_USE_PERF_MONITOR_POS LV_ALIGN_BOTTOM_RIGHT
#endif
#define LV_USE_MEM_MONITOR 0
#if LV_USE_MEM_MONITOR
    #define LV_USE_MEM_MONITOR_POS LV_ALIGN_BOTTOM_LEFT
#endif
#define LV_USE_REFR_DEBUG 0

/* =========================================================================
   TEXT
   ========================================================================= */
#define LV_TXT_ENC LV_TXT_ENC_UTF8
#define LV_TXT_BREAK_CHARS " ,.;:-_"
#define LV_TXT_LINE_BREAK_LONG_LEN          0
#define LV_TXT_LINE_BREAK_LONG_PRE_MIN_LEN  3
#define LV_TXT_LINE_BREAK_LONG_POST_MIN_LEN 3
#define LV_TXT_COLOR_CMD "#"
#define LV_USE_BIDI                0
#define LV_USE_ARABIC_PERSIAN_CHARS 0

/* =========================================================================
   WIDGETS — only enable what we use
   ========================================================================= */
#define LV_USE_ARC       0
#define LV_USE_BAR       0
#define LV_USE_BTN       0
#define LV_USE_BTNMATRIX 0
#define LV_USE_CANVAS    0
#define LV_USE_CHECKBOX  0
#define LV_USE_DROPDOWN  0
#define LV_USE_IMG       1   /* background image */
#define LV_USE_LABEL     1
#if LV_USE_LABEL
    #define LV_LABEL_TEXT_SELECTION  0
    #define LV_LABEL_WAIT_CHAR_COUNT 3
    /* LV_LABEL_DEF_SCROLL_SPEED is a per-widget fallback in lv_label.c — do not redefine here */
#endif
#define LV_USE_LINE      0
#define LV_USE_ROLLER    0
#define LV_USE_SLIDER    0
#define LV_USE_SWITCH    0
#define LV_USE_TEXTAREA  0
#define LV_USE_TABLE     0

/* Extra widgets — all off */
#define LV_USE_ANIMIMG   0
#define LV_USE_CALENDAR  0
#define LV_USE_CHART     0
#define LV_USE_COLORWHEEL 0
#define LV_USE_IMGBTN    0
#define LV_USE_KEYBOARD  0
#define LV_USE_LED       0
#define LV_USE_LIST      0
#define LV_USE_MENU      0
#define LV_USE_METER     0
#define LV_USE_MSGBOX    0
#define LV_USE_SPAN      0
#define LV_USE_SPINBOX   0
#define LV_USE_SPINNER   0
#define LV_USE_TABVIEW   0
#define LV_USE_TILEVIEW  0
#define LV_USE_WIN       0

/* =========================================================================
   THEMES
   ========================================================================= */
#define LV_USE_THEME_DEFAULT 1
#if LV_USE_THEME_DEFAULT
    #define LV_THEME_DEFAULT_DARK        1
    #define LV_THEME_DEFAULT_GROW        0
    #define LV_THEME_DEFAULT_TRANSITION_TIME 80
#endif
#define LV_USE_THEME_SIMPLE  0
#define LV_USE_THEME_MONO    0

/* =========================================================================
   LAYOUTS
   ========================================================================= */
#define LV_USE_FLEX 1
#define LV_USE_GRID 0

/* =========================================================================
   FONTS
   ========================================================================= */
/* Montserrat — enable only what we use */
#define LV_FONT_MONTSERRAT_8   0
#define LV_FONT_MONTSERRAT_10  0
#define LV_FONT_MONTSERRAT_12  0
#define LV_FONT_MONTSERRAT_14  0
#define LV_FONT_MONTSERRAT_16  0
#define LV_FONT_MONTSERRAT_18  0
#define LV_FONT_MONTSERRAT_20  0
#define LV_FONT_MONTSERRAT_22  1   /* header / footer labels */
#define LV_FONT_MONTSERRAT_24  0
#define LV_FONT_MONTSERRAT_26  0
#define LV_FONT_MONTSERRAT_28  0
#define LV_FONT_MONTSERRAT_30  0
#define LV_FONT_MONTSERRAT_32  0
#define LV_FONT_MONTSERRAT_34  0
#define LV_FONT_MONTSERRAT_36  0
#define LV_FONT_MONTSERRAT_38  0
#define LV_FONT_MONTSERRAT_40  0
#define LV_FONT_MONTSERRAT_42  0
#define LV_FONT_MONTSERRAT_44  0
#define LV_FONT_MONTSERRAT_46  0
#define LV_FONT_MONTSERRAT_48  1   /* main counter digit */

#define LV_FONT_MONTSERRAT_12_SUBPX      0
#define LV_FONT_MONTSERRAT_28_COMPRESSED 0
#define LV_FONT_DEJAVU_16_PERSIAN_HEBREW 0
#define LV_FONT_SIMSUN_16_CJK            0
#define LV_FONT_UNSCII_8                 0
#define LV_FONT_UNSCII_16                0
#define LV_FONT_CUSTOM_DECLARE

#define LV_FONT_DEFAULT &lv_font_montserrat_22

#define LV_FONT_FMT_TXT_LARGE   1   /* required for glyphs > ~200px */
#define LV_USE_FONT_SUBPX       0
#define LV_USE_FONT_COMPRESSED  0

/* =========================================================================
   ANIMATION
   ========================================================================= */
#define LV_USE_ANIMATION 1

/* =========================================================================
   FILE SYSTEM — not used
   ========================================================================= */
#define LV_USE_FS_STDIO  0
#define LV_USE_FS_POSIX  0
#define LV_USE_FS_WIN32  0
#define LV_USE_FS_FATFS  0

/* =========================================================================
   IMAGE DECODERS — not used
   ========================================================================= */
#define LV_USE_PNG  0
#define LV_USE_BMP  0
#define LV_USE_SJPG 0
#define LV_USE_GIF  0

/* =========================================================================
   MISC
   ========================================================================= */
#define LV_USE_QRCODE   0
#define LV_USE_RLOTTIE  0
#define LV_USE_FFMPEG   0
#define LV_USE_FREETYPE 0

#define LV_USE_SNAPSHOT 0
#define LV_USE_MONKEY   0
#define LV_USE_GRIDNAV  0
#define LV_USE_FRAGMENT 0
#define LV_USE_IMGFONT  0
#define LV_USE_MSG      0
#define LV_USE_IME_PINYIN 0

#define LV_USE_USER_DATA    1
#define LV_USE_LARGE_COORD  0
#define LV_ENABLE_GC        0

/* =========================================================================
   DEMOS — not used
   ========================================================================= */
#define LV_USE_DEMO_WIDGETS            0
#define LV_USE_DEMO_KEYPAD_AND_ENCODER 0
#define LV_USE_DEMO_BENCHMARK          0
#define LV_USE_DEMO_STRESS             0
#define LV_USE_DEMO_MUSIC              0

#endif /* LV_CONF_H */
