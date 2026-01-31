#pragma once

#include <string>

// Runtime configuration loaded from VNTranslationToolsConstants.json
// Call RuntimeConfig::Load() early in initialization before accessing any values.
class RuntimeConfig {
public:
    // Loads configuration from VNTranslationToolsConstants.json
    // Shows MessageBox and exits on error (file not found or parse error)
    static void Load();

    // Accessors (call only after Load())
    static bool DebugLogging();
    static bool EnableFontSubstitution();
    static bool PillarboxedFullscreen();
    static bool ClipMouseCursorInPillarboxedFullscreen();
    static bool DirectX11Upscaling();
    static const std::wstring& CustomFontName();
    static const std::wstring& CustomFontFilename();
    static const std::wstring& MonospaceFontFilename();
    static int FontHeightIncrease();
    static int FontYSpacingBetweenLines();
    static int FontYTopPosDecrease();
    static bool ProportionalFontBold();
    static int ProportionalLineWidth();
    static int MaxLineWidth();
    static int NumLinesWarnThreshold();

private:
    static inline bool _loaded = false;
    static inline bool _debugLogging;
    static inline bool _enableFontSubstitution;
    static inline bool _pillarboxedFullscreen;
    static inline bool _clipMouseCursorInBorderlessFullscreen;
    static inline bool _directX11Upscaling;
    static inline std::wstring _customFontName;
    static inline std::wstring _customFontFilename;
    static inline std::wstring _monospaceFontFilename;
    static inline int _fontHeightIncrease;
    static inline int _fontYSpacingBetweenLines;
    static inline int _fontYTopPosDecrease;
    static inline bool _proportionalFontBold;
    static inline int _proportionalLineWidth;
    static inline int _maxLineWidth;
    static inline int _numLinesWarnThreshold;
};
