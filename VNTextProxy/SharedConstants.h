#pragma once

// Shared constants between C# (VNTextPatch) and C++ (VNTextProxy)
// This file is the single source of truth.
// C# constants are auto-generated from this file via SharedConstants.tt

#define DEBUG_LOGGING 0

// Expected original game values
#define JAPANESE_FONT_NAME L"MS Gothic"
#define GAME_DEFAULT_FONT_HEIGHT 21
#define GAME_DEFAULT_SPACING_BETWEEN_LINES 8
#define GAME_DEFAULT_MAX_LINE_WIDTH 528

// Our font changes

// Large font that fits nicely within 3 lines
#define CUSTOM_FONT_NAME L"Nunito ExtraBold"
#define CUSTOM_FONT_FILENAME L"Nunito ExtraBold.ttf"
#define MONOSPACE_FONT_FILENAME L"Inconsolata ExtraBold.ttf"
#define FONT_HEIGHT_INCREASE 6
#define FONT_Y_SPACING_BETWEEN_LINES 7
#define FONT_Y_TOP_POS_DECREASE 4
#define BOOL_PROPORTIONAL_FONT_BOLD 0
#define PROPORTIONAL_LINE_WIDTH 545
#define MAX_LINE_WIDTH 570
#define NUM_LINES_WARN_THRESHOLD 4

// Medium font designed to fit within 3 lines
//#define CUSTOM_FONT_NAME L"Nunito ExtraBold"
//#define CUSTOM_FONT_FILENAME L"Nunito ExtraBold.ttf"
//#define FONT_HEIGHT_INCREASE 2
//#define FONT_Y_SPACING_BETWEEN_LINES 7
//#define FONT_Y_TOP_POS_DECREASE 2
//#define BOOL_PROPORTIONAL_FONT_BOLD 0
//#define PROPORTIONAL_LINE_WIDTH 545
//#define MAX_LINE_WIDTH 570
//#define NUM_LINES_WARN_THRESHOLD 4

// Small font designed for 4 lines max
//#define CUSTOM_FONT_NAME L"Nunito ExtraBold"
//#define CUSTOM_FONT_FILENAME L"Nunito ExtraBold.ttf"
//#define FONT_HEIGHT_INCREASE 0
//#define FONT_Y_SPACING_BETWEEN_LINES 1
//#define FONT_Y_TOP_POS_DECREASE -2
//#define BOOL_PROPORTIONAL_FONT_BOLD 0
//#define PROPORTIONAL_LINE_WIDTH 545
//#define MAX_LINE_WIDTH 570
//#define NUM_LINES_WARN_THRESHOLD 5

// SJIS tunnelling doesn't work in softpal, so plumb special characters in ASCII and half-width katakana
#define MAP_SPACE_CHARACTER '|'
#define MAP_SJIS_1 'ｱ'
#define MAP_UNICODE_1 u'%'
#define MAP_SJIS_2 'ｫ' // « in latin1
#define MAP_UNICODE_2 u'“'
#define MAP_SJIS_3 'ｻ' // » in latin1
#define MAP_UNICODE_3 u'”'
#define MAP_SJIS_4 'ｨ'
#define MAP_UNICODE_4 u'‘'
#define MAP_SJIS_5 'ｴ'
#define MAP_UNICODE_5 u'’'
#define MAP_SJIS_6 'ｶ'
#define MAP_UNICODE_6 u'é'
#define MAP_SJIS_7 'ｲ'
#define MAP_UNICODE_7 u'♪'
#define MAP_SJIS_8 'ﾙ'
#define MAP_UNICODE_8 u'♥'
