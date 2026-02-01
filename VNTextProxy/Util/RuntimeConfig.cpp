#include "pch.h"
#include "RuntimeConfig.h"
#include "SharedConstants.h"
#include "Logger.h"
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
        _pillarboxedFullscreen = config.value("pillarboxedFullscreen", true);
        _clipMouseCursorInPillarboxedFullscreen = config.value("clipMouseCursorInPillarboxedFullscreen", true);
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

    // Debug: Log loaded values to confirm config was read
    proxy_log(LogCategory::TEXT, "RuntimeConfig::Load() SUCCESS - Config loaded:");
    proxy_log(LogCategory::TEXT, "  debugLogging: %s", _debugLogging ? "true" : "false");
    proxy_log(LogCategory::TEXT, "  enableFontSubstitution: %s", _enableFontSubstitution ? "true" : "false");
    proxy_log(LogCategory::TEXT, "  customFontName: %ls", _customFontName.c_str());
    proxy_log(LogCategory::TEXT, "  customFontFilename: %ls", _customFontFilename.c_str());
    proxy_log(LogCategory::TEXT, "  monospaceFontFilename: %ls", _monospaceFontFilename.c_str());
    proxy_log(LogCategory::TEXT, "  fontHeightIncrease: %d", _fontHeightIncrease);
    proxy_log(LogCategory::TEXT, "  fontYSpacingBetweenLines: %d", _fontYSpacingBetweenLines);
    proxy_log(LogCategory::TEXT, "  fontYTopPosDecrease: %d", _fontYTopPosDecrease);
    proxy_log(LogCategory::TEXT, "  proportionalFontBold: %s", _proportionalFontBold ? "true" : "false");
    proxy_log(LogCategory::TEXT, "  proportionalLineWidth: %d", _proportionalLineWidth);
    proxy_log(LogCategory::TEXT, "  maxLineWidth: %d", _maxLineWidth);
    proxy_log(LogCategory::TEXT, "  numLinesWarnThreshold: %d", _numLinesWarnThreshold);
    proxy_log(LogCategory::TEXT, "  pillarboxedFullscreen: %s", _pillarboxedFullscreen ? "true" : "false");
    proxy_log(LogCategory::TEXT, "  clipMouseCursorInPillarboxedFullscreen: %s", _clipMouseCursorInPillarboxedFullscreen ? "true" : "false");
    proxy_log(LogCategory::TEXT, "  directX11Upscaling: %s", _directX11Upscaling ? "true" : "false");
}

bool RuntimeConfig::DebugLogging() { return _debugLogging; }
bool RuntimeConfig::EnableFontSubstitution() { return _enableFontSubstitution; }
bool RuntimeConfig::PillarboxedFullscreen() { return _pillarboxedFullscreen; }
bool RuntimeConfig::ClipMouseCursorInPillarboxedFullscreen() { return _clipMouseCursorInPillarboxedFullscreen; }
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
