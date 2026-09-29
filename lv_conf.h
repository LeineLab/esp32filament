/**
 * lv_conf.h — LVGL 8.3 configuration for ESP32 filament station
 * Place in project root alongside platformio.ini
 */
#ifndef LV_CONF_H
#define LV_CONF_H

#include <stdint.h>

/*====================
   COLOR SETTINGS
 *====================*/
#define LV_COLOR_DEPTH     16
#define LV_COLOR_16_SWAP   0
#define LV_COLOR_SCREEN_TRANSP 0
#define LV_COLOR_MIX_ROUND_OFS 0

/*=========================
   MEMORY SETTINGS
 *=========================*/
#define LV_MEM_CUSTOM 0
#define LV_MEM_SIZE (48U * 1024U)  /* 48 KB for LVGL heap */
#define LV_MEM_POOL_INCLUDE <stdlib.h>
#define LV_MEM_POOL_ALLOC   malloc
#define LV_MEM_POOL_FREE    free

/*====================
   HAL SETTINGS
 *====================*/
#define LV_DISP_DEF_REFR_PERIOD  10   /* ms */
#define LV_INDEV_DEF_READ_PERIOD 30   /* ms */
#define LV_DISP_ROT_MAX_BUF (10 * 1024)

/*=======================
   FEATURE CONFIGURATION
 *=======================*/
#define LV_SPRINTF_CUSTOM 0
#define LV_USE_ASSERT_NULL          1
#define LV_USE_ASSERT_MALLOC        1
#define LV_USE_ASSERT_STYLE         0
#define LV_USE_ASSERT_MEM_INTEGRITY 0
#define LV_USE_ASSERT_OBJ           0

/*==================
 * TICK INTERFACE
 *==================*/
#define LV_TICK_CUSTOM     1
#if LV_TICK_CUSTOM
    #define LV_TICK_CUSTOM_INCLUDE  "Arduino.h"
    #define LV_TICK_CUSTOM_SYS_TIME_EXPR (millis())
#endif

/*====================
 * LOGGING
 *====================*/
#define LV_USE_LOG      0   /* Disable log to save memory */
#if LV_USE_LOG
    #define LV_LOG_LEVEL    LV_LOG_LEVEL_WARN
    #define LV_LOG_PRINTF   1
#endif

/*================
 * ASSERTS
 *================*/
#define LV_USE_PERF_MONITOR 0
#define LV_USE_MEM_MONITOR  0
#define LV_USE_REFR_DEBUG   0

/*==================
 * COMPILER SETTINGS
 *==================*/
#define LV_BIG_ENDIAN_SYSTEM    0
#define LV_ATTRIBUTE_MEM_FAST
#define LV_ATTRIBUTE_FAST_MEM
#define LV_ATTRIBUTE_DMA
#define LV_EXPORT_CONST_INT(int_value) struct _silence_gcc_warning
#define LV_USE_LARGE_COORD  0

/*=================
 * FONT USAGE
 *=================*/
#define LV_FONT_MONTSERRAT_8  0
#define LV_FONT_MONTSERRAT_10 0
#define LV_FONT_MONTSERRAT_12 1
#define LV_FONT_MONTSERRAT_14 1
#define LV_FONT_MONTSERRAT_16 1
#define LV_FONT_MONTSERRAT_18 0
#define LV_FONT_MONTSERRAT_20 1
#define LV_FONT_MONTSERRAT_22 0
#define LV_FONT_MONTSERRAT_24 1
#define LV_FONT_MONTSERRAT_26 0
#define LV_FONT_MONTSERRAT_28 0
#define LV_FONT_MONTSERRAT_30 0
#define LV_FONT_MONTSERRAT_32 0
#define LV_FONT_MONTSERRAT_34 0
#define LV_FONT_MONTSERRAT_36 0
#define LV_FONT_MONTSERRAT_38 0
#define LV_FONT_MONTSERRAT_40 0
#define LV_FONT_MONTSERRAT_42 0
#define LV_FONT_MONTSERRAT_44 0
#define LV_FONT_MONTSERRAT_46 0
#define LV_FONT_MONTSERRAT_48 0

#define LV_FONT_MONTSERRAT_12_SUBPX      0
#define LV_FONT_MONTSERRAT_28_COMPRESSED 0
#define LV_FONT_DEJAVU_16_PERSIAN_HEBREW 0
#define LV_FONT_SIMSUN_16_CJK            0
#define LV_FONT_UNSCII_8                 0
#define LV_FONT_UNSCII_16                0

/* When LVGL_EXT_FONTS is defined (via platformio.ini, after running
 * scripts/gen_fonts.sh), use custom extended-Latin Montserrat fonts from
 * src/fonts/ — LVGL's own built-in lv_font_montserrat_* fonts only cover
 * 0x20-0x7F plus degree/bullet (confirmed against the shipped font source),
 * i.e. no umlauts at all. Falls back to the built-in ASCII-only fonts if the
 * custom ones haven't been generated yet. */
#ifdef LVGL_EXT_FONTS
  #define LV_FONT_CUSTOM_DECLARE \
    extern const lv_font_t montserrat_ext_14; \
    extern const lv_font_t montserrat_ext_16; \
    extern const lv_font_t montserrat_ext_20;
  #define LV_FONT_DEFAULT &montserrat_ext_14
#else
  #define LV_FONT_CUSTOM_DECLARE
  #define LV_FONT_DEFAULT &lv_font_montserrat_14
#endif

#define LV_FONT_FMT_TXT_LARGE   0
#define LV_USE_FONT_SUBPX       0
#if LV_USE_FONT_SUBPX
    #define LV_FONT_SUBPX_BGR   0
#endif
#define LV_USE_FONT_COMPRESSED  0

/*===================
 * WIDGET USAGE
 *===================*/
#define LV_USE_ARC          1   /* required by LV_USE_SPINNER */
#define LV_USE_BAR          1
#define LV_USE_BTN          1
#define LV_USE_BTNMATRIX    0
#define LV_USE_CANVAS       0
#define LV_USE_CHECKBOX     0
#define LV_USE_DROPDOWN     0
#define LV_USE_IMG          0
#define LV_USE_LABEL        1
#if LV_USE_LABEL
    #define LV_LABEL_TEXT_SELECTION 0
    #define LV_LABEL_LONG_TXT_HINT  0
#endif
#define LV_USE_LINE         0
#define LV_USE_ROLLER       0
#if LV_USE_ROLLER
    #define LV_ROLLER_INF_PAGES     7
#endif
#define LV_USE_SLIDER       0
#define LV_USE_SWITCH       0
#define LV_USE_TEXTAREA     0
#if LV_USE_TEXTAREA != 0
    #define LV_TEXTAREA_DEF_PWD_SHOW_TIME 1500
#endif
#define LV_USE_TABLE        0

/*==================
 * EXTRA COMPONENTS
 *==================*/
#define LV_USE_ANIMIMG      0
#define LV_USE_CALENDAR     0
#define LV_USE_CHART        0
#define LV_USE_COLORWHEEL   0
#define LV_USE_IMGBTN       0
#define LV_USE_KEYBOARD     0
#define LV_USE_LED          0
#define LV_USE_LIST         0
#define LV_USE_MENU         0
#define LV_USE_METER        0
#define LV_USE_MSGBOX       0   /* not used — custom popups via lv_obj_create */
#define LV_USE_SPAN         0
#if LV_USE_SPAN
    #define LV_SPAN_SNIPPET_STACK_SIZE 64
#endif
#define LV_USE_SPINBOX      0
#define LV_USE_SPINNER      1
#define LV_USE_TABVIEW      0
#define LV_USE_TILEVIEW     0
#define LV_USE_WIN          0

/*=====================
 * THEMES
 *=====================*/
#define LV_USE_THEME_DEFAULT    1
#if LV_USE_THEME_DEFAULT
    #define LV_THEME_DEFAULT_DARK     0
    #define LV_THEME_DEFAULT_GROW     1
    #define LV_THEME_DEFAULT_TRANSITION_TIME 80
#endif
#define LV_USE_THEME_BASIC  1
#define LV_USE_THEME_MONO   0

/*=====================
 * LAYOUT
 *=====================*/
#define LV_USE_FLEX     1
#define LV_USE_GRID     0

/*==================
 * DPI/ROTATION
 *==================*/
#define LV_HOR_RES_MAX  320
#define LV_VER_RES_MAX  240

#define LV_DPI_DEF 130

/*===================
 * DRAWING
 *===================*/
#define LV_DRAW_COMPLEX 1
#if LV_DRAW_COMPLEX != 0
    #define LV_SHADOW_CACHE_SIZE    0
    #define LV_CIRCLE_CACHE_SIZE    4
#endif
#define LV_IMG_CACHE_DEF_SIZE   0
#define LV_GRADIENT_MAX_STOPS   2
#define LV_GRAD_CACHE_DEF_SIZE  0
#define LV_DITHER_GRADIENT  0
#define LV_DISP_ROT_MAX_BUF (10 * 1024)

/*===================
 * GPU
 *===================*/
#define LV_USE_GPU_STM32_DMA2D  0
#define LV_USE_GPU_SWM341_DMA   0
#define LV_USE_GPU_NXP_PXP      0
#define LV_USE_GPU_NXP_VG_LITE  0
#define LV_USE_GPU_SDL          0
#define LV_USE_EXTERNAL_RENDERER 0

/*===================
 * FILE SYSTEM
 *===================*/
#define LV_USE_FS_STDIO     0
#define LV_USE_FS_POSIX     0
#define LV_USE_FS_WIN32     0
#define LV_USE_FS_FATFS     0

/*===================
 * PNG DECODER
 *===================*/
#define LV_USE_PNG  0
#define LV_USE_BMP  0
#define LV_USE_SJPG 0
#define LV_USE_GIF  0
#define LV_USE_QRCODE  0

/*===================
 * MISC
 *===================*/
#define LV_USE_SNAPSHOT     0
#define LV_USE_MONKEY       0
#define LV_USE_GRIDNAV      0
#define LV_USE_FRAGMENT     0
#define LV_USE_IMGFONT      0
#define LV_USE_MSG          0
#define LV_USE_IME_PINYIN   0

#endif /* LV_CONF_H */
