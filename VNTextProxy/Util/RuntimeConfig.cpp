#include "pch.h"
#include "RuntimeConfig.h"
#include "SharedConstants.h"
#include "external/json.hpp"
#include <fstream>
#include <sstream>

using json = nlohmann::json;

static std::wstring Utf8ToWstring(const std::string& utf8Str)
{
    if (utf8Str.empty())
        return std::wstring();

    int sizeNeeded = MultiByteToWideChar(CP_UTF8, 0, utf8Str.c_str(), (int)utf8Str.size(), nullptr, 0);
    std::wstring wstr(sizeNeeded, 0);
    MultiByteToWideChar(CP_UTF8, 0, utf8Str.c_str(), (int)utf8Str.size(), &wstr[0], sizeNeeded);
    return wstr;
}

static void ShowErrorAndExit(const std::wstring& message)
{
    MessageBoxW(nullptr, message.c_str(), L"VNTranslationTools Configuration Error", MB_OK | MB_ICONERROR);
    ExitProcess(1);
}

void RuntimeConfig::Load()
{
    if (_loaded)
        return;

    const char* configFileName = RUNTIME_CONFIG_FILENAME;

    std::ifstream file(configFileName);
    if (!file.is_open())
    {
        std::wstringstream ss;
        ss << L"Configuration file not found: " << Utf8ToWstring(configFileName) << L"\n\n";
        ss << L"Please ensure VNTranslationToolsConstants.json is in the game directory.";
        ShowErrorAndExit(ss.str());
    }

    json config;
    try
    {
        file >> config;
    }
    catch (const json::parse_error& e)
    {
        std::wstringstream ss;
        ss << L"Failed to parse configuration file: " << Utf8ToWstring(configFileName) << L"\n\n";
        ss << L"Error: " << Utf8ToWstring(e.what());
        ShowErrorAndExit(ss.str());
    }

    try
    {
        _debugLogging = config.value("debugLogging", true);
        _enableFontSubstitution = config.value("enableFontSubstitution", true);
        _customFontName = Utf8ToWstring(config.at("customFontName").get<std::string>());
        _customFontFilename = Utf8ToWstring(config.at("customFontFilename").get<std::string>());
        _monospaceFontFilename = Utf8ToWstring(config.at("monospaceFontFilename").get<std::string>());
        _fontHeightIncrease = config.at("fontHeightIncrease").get<int>();
        _fontYSpacingBetweenLines = config.at("fontYSpacingBetweenLines").get<int>();
        _fontYTopPosDecrease = config.at("fontYTopPosDecrease").get<int>();
        _proportionalFontBold = config.at("proportionalFontBold").get<bool>();
        _proportionalLineWidth = config.at("proportionalLineWidth").get<int>();
        _maxLineWidth = config.at("maxLineWidth").get<int>();
        _numLinesWarnThreshold = config.at("numLinesWarnThreshold").get<int>();
        _borderlessFullscreen = config.value("borderlessFullscreen", true);
        _clipMouseCursorInBorderlessFullscreen = config.value("clipMouseCursorInBorderlessFullscreen", true);
        _directX11Upscaling = config.value("directX11Upscaling", true);
    }
    catch (const json::exception& e)
    {
        std::wstringstream ss;
        ss << L"Missing or invalid configuration value in: " << Utf8ToWstring(configFileName) << L"\n\n";
        ss << L"Error: " << Utf8ToWstring(e.what());
        ShowErrorAndExit(ss.str());
    }

    _loaded = true;

    // Debug: Write loaded values to winmm_dll_log.txt to confirm config was read
    FILE* debugLog2 = nullptr;
    if (fopen_s(&debugLog2, "winmm_dll_log.txt", "at") == 0 && debugLog2) {
        fprintf(debugLog2, "RuntimeConfig::Load() SUCCESS - Config loaded:\n");
        fprintf(debugLog2, "  debugLogging: %s\n", _debugLogging ? "true" : "false");
        fprintf(debugLog2, "  enableFontSubstitution: %s\n", _enableFontSubstitution ? "true" : "false");
        fprintf(debugLog2, "  customFontName: %ls\n", _customFontName.c_str());
        fprintf(debugLog2, "  customFontFilename: %ls\n", _customFontFilename.c_str());
        fprintf(debugLog2, "  monospaceFontFilename: %ls\n", _monospaceFontFilename.c_str());
        fprintf(debugLog2, "  fontHeightIncrease: %d\n", _fontHeightIncrease);
        fprintf(debugLog2, "  fontYSpacingBetweenLines: %d\n", _fontYSpacingBetweenLines);
        fprintf(debugLog2, "  fontYTopPosDecrease: %d\n", _fontYTopPosDecrease);
        fprintf(debugLog2, "  proportionalFontBold: %s\n", _proportionalFontBold ? "true" : "false");
        fprintf(debugLog2, "  proportionalLineWidth: %d\n", _proportionalLineWidth);
        fprintf(debugLog2, "  maxLineWidth: %d\n", _maxLineWidth);
        fprintf(debugLog2, "  numLinesWarnThreshold: %d\n", _numLinesWarnThreshold);
        fprintf(debugLog2, "  borderlessFullscreen: %s\n", _borderlessFullscreen ? "true" : "false");
        fprintf(debugLog2, "  clipMouseCursorInBorderlessFullscreen: %s\n", _clipMouseCursorInBorderlessFullscreen ? "true" : "false");
        fprintf(debugLog2, "  directX11Upscaling: %s\n", _directX11Upscaling ? "true" : "false");
        fclose(debugLog2);
    }
}

bool RuntimeConfig::DebugLogging() { return _debugLogging; }
bool RuntimeConfig::EnableFontSubstitution() { return _enableFontSubstitution; }
bool RuntimeConfig::BorderlessFullscreen() { return _borderlessFullscreen; }
bool RuntimeConfig::ClipMouseCursorInBorderlessFullscreen() { return _clipMouseCursorInBorderlessFullscreen; }
bool RuntimeConfig::DirectX11Upscaling() { return _directX11Upscaling; }
const std::wstring& RuntimeConfig::CustomFontName() { return _customFontName; }
const std::wstring& RuntimeConfig::CustomFontFilename() { return _customFontFilename; }
const std::wstring& RuntimeConfig::MonospaceFontFilename() { return _monospaceFontFilename; }
int RuntimeConfig::FontHeightIncrease() { return _fontHeightIncrease; }
int RuntimeConfig::FontYSpacingBetweenLines() { return _fontYSpacingBetweenLines; }
int RuntimeConfig::FontYTopPosDecrease() { return _fontYTopPosDecrease; }
bool RuntimeConfig::ProportionalFontBold() { return _proportionalFontBold; }
int RuntimeConfig::ProportionalLineWidth() { return _proportionalLineWidth; }
int RuntimeConfig::MaxLineWidth() { return _maxLineWidth; }
int RuntimeConfig::NumLinesWarnThreshold() { return _numLinesWarnThreshold; }
