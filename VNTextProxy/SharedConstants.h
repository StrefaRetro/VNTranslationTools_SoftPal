#pragma once

// Shared constants between C# (VNTextPatch) and C++ (VNTextProxy)
// This file is the single source of truth.
// C# constants are auto-generated from this file via SharedConstants.tt

// Expected original game values
#define JAPANESE_FONT_NAME L"MS Gothic"
#define GAME_DEFAULT_FONT_HEIGHT 21
#define GAME_DEFAULT_SPACING_BETWEEN_LINES 8
#define GAME_DEFAULT_MAX_LINE_WIDTH 528

// Our font changes
#define CUSTOM_FONT_NAME L"Nunito ExtraBold"
#define CUSTOM_FONT_FILENAME L"Nunito ExtraBold.ttf"
#define FONT_HEIGHT_INCREASE 6
#define FONT_Y_SPACING_BETWEEN_LINES 7
#define FONT_Y_TOP_POS_DECREASE 4
#define PROPORTIONAL_FONT_BOLD 0
#define PROPORTIONAL_LINE_WIDTH 506
#define MAX_LINE_WIDTH 580

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
#define MAP_SJIS_6 'ｲ'
#define MAP_UNICODE_6 u'é'
