#pragma once

// Compile-time shared constants between C# (VNTextPatch) and C++ (VNTextProxy)
//
// Runtime-configurable settings are in VNTranslationToolsConstants.json
// and are loaded at runtime by RuntimeConfig.

#define RUNTIME_CONFIG_FILENAME "VNTranslationToolsConstants.json"

// Expected original game values
#define JAPANESE_FONT_NAME L"MS Gothic"
#define GAME_DEFAULT_FONT_HEIGHT 21
#define GAME_DEFAULT_SPACING_BETWEEN_LINES 8
#define GAME_DEFAULT_MAX_LINE_WIDTH 528

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

