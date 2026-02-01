#include "pch.h"

#include <string>
#include <sstream>
#include <iomanip>
#include <usp10.h>
#include <vector>

#include "PALHooks.h"
#include "SharedConstants.h"
#include "Util/Logger.h"

#pragma comment(lib, "usp10.lib")

#define ENLARGE_FONT 1
#define LEGACY_KERNING 0

using namespace std;

/*
SoftPal uses GDI methods only to create the font and to compute the metrics and bitmap of one character at a time.
Then on its end (and beyond our control here), it adds a black outline around the bitmap and prints it onscreen itself.
It uses the GDI-returned gmCellIncX to advance its internal "pen position".
Since GDI never sees more than one letter per function call, its native kerning support doesn't do anything.

Example call sequence from SoftPal:

GdiProportionalizer::CreateFontAHook()
GdiProportionalizer::CreateFontWHook()
GdiProportionalizer::CreateFontIndirectWHook()
GdiProportionalizer::SelectObjectHook()
GdiProportionalizer::GetGlyphOutlineAHook() char: W, 0x57
GdiProportionalizer::GetGlyphOutlineAHook() char: W, 0x57
GdiProportionalizer::GetGlyphOutlineAHook() char: W, 0x57
GdiProportionalizer::GetGlyphOutlineAHook() char: h, 0x68
GdiProportionalizer::GetGlyphOutlineAHook() char: h, 0x68
GdiProportionalizer::GetGlyphOutlineAHook() char: h, 0x68
GdiProportionalizer::GetGlyphOutlineAHook() char: a, 0x61
GdiProportionalizer::GetGlyphOutlineAHook() char: a, 0x61
GdiProportionalizer::GetGlyphOutlineAHook() char: a, 0x61
GdiProportionalizer::GetGlyphOutlineAHook() char: t, 0x74
GdiProportionalizer::GetGlyphOutlineAHook() char: t, 0x74
GdiProportionalizer::GetGlyphOutlineAHook() char: t, 0x74
GdiProportionalizer::GetGlyphOutlineAHook() char: ?, 0x3f
GdiProportionalizer::GetGlyphOutlineAHook() char: ?, 0x3f
GdiProportionalizer::GetGlyphOutlineAHook() char: ?, 0x3f
GdiProportionalizer::SelectObjectHook()
GdiProportionalizer::DeleteObjectHook()
*/

#if LEGACY_KERNING
static std::unordered_map<uint32_t, int> kernAmounts;
#endif
static int currentTextOffset = 0;
static int totalAdvOut = 0;

// Check if text contains full-width Japanese characters (Hiragana, Katakana, CJK)
static bool ContainsJapaneseCharacters(const wchar_t* text)
{
    if (text == nullptr)
        return false;

    for (const wchar_t* p = text; *p != L'\0'; ++p)
    {
        wchar_t ch = *p;
        // Hiragana: U+3040-U+309F
        // Katakana: U+30A0-U+30FF
        // CJK Unified Ideographs: U+4E00-U+9FFF
        // CJK Extension A: U+3400-U+4DBF
        if ((ch >= 0x3040 && ch <= 0x309F) ||  // Hiragana
            (ch >= 0x30A0 && ch <= 0x30FF) ||  // Katakana
            (ch >= 0x4E00 && ch <= 0x9FFF) ||  // CJK Unified Ideographs
            (ch >= 0x3400 && ch <= 0x4DBF))    // CJK Extension A
        {
            return true;
        }
    }
    return false;
}

void GdiProportionalizer::Init()
{
    proxy_log(LogCategory::TEXT, "GdiProportionalizer::Init()");

    Proportionalizer::Init();
    ImportHooker::Hook(
        {
            { "EnumFontsA", EnumFontsAHook },
            { "EnumFontFamiliesExA", EnumFontFamiliesExAHook },
            { "CreateFontA", CreateFontAHook },
            { "CreateFontIndirectA", CreateFontIndirectAHook },
            { "CreateFontW", CreateFontWHook },
            { "CreateFontIndirectW", CreateFontIndirectWHook },
            { "SelectObject", SelectObjectHook },
            { "DeleteObject", DeleteObjectHook },
            { "GetTextExtentPointA", GetTextExtentPointAHook },
            { "GetTextExtentPoint32A", GetTextExtentPoint32AHook },
            { "TextOutA", TextOutAHook },
            { "GetGlyphOutlineA", GetGlyphOutlineAHook }
        }
    );
}

int GdiProportionalizer::EnumFontsAHook(HDC hdc, LPCSTR lpLogfont, FONTENUMPROCA lpProc, LPARAM lParam)
{
    proxy_log(LogCategory::TEXT, "GdiProportionalizer::EnumFontsAHook()");

    EnumFontsContext context;
    context.OriginalProc = lpProc;
    context.OriginalContext = lParam;
    context.Extended = false;
    return EnumFontsW(hdc, lpLogfont != nullptr ? SjisTunnelEncoding::Decode(lpLogfont).c_str() : nullptr, &EnumFontsProc, (LPARAM)&context);
}

int GdiProportionalizer::EnumFontFamiliesExAHook(HDC hdc, LPLOGFONTA lpLogfont, FONTENUMPROCA lpProc, LPARAM lParam, DWORD dwFlags)
{
    proxy_log(LogCategory::TEXT, "GdiProportionalizer::EnumFontFamiliesExAHook()");

    LOGFONTW logFontW = ConvertLogFontAToW(*lpLogfont);
    EnumFontsContext context;
    context.OriginalProc = lpProc;
    context.OriginalContext = lParam;
    context.Extended = true;
    return EnumFontFamiliesExW(hdc, &logFontW, &EnumFontsProc, (LPARAM)&context, dwFlags);
}

int GdiProportionalizer::EnumFontsProc(const LOGFONTW* lplf, const TEXTMETRICW* lptm, DWORD dwType, LPARAM lpData)
{
    proxy_log(LogCategory::TEXT, "GdiProportionalizer::EnumFontsProc()");

    EnumFontsContext* pContext = (EnumFontsContext*)lpData;
    ENUMLOGFONTEXDVA logFontExA;
    logFontExA.elfEnumLogfontEx.elfLogFont = ConvertLogFontWToA(*lplf);
    if (pContext->Extended)
    {
        ENUMLOGFONTEXDVW* pLogFontExW = (ENUMLOGFONTEXDVW*)lplf;
        strcpy_s((char*)logFontExA.elfEnumLogfontEx.elfFullName, sizeof(logFontExA.elfEnumLogfontEx.elfFullName), SjisTunnelEncoding::Encode(pLogFontExW->elfEnumLogfontEx.elfFullName).c_str());
        strcpy_s((char*)logFontExA.elfEnumLogfontEx.elfScript,   sizeof(logFontExA.elfEnumLogfontEx.elfScript),   SjisTunnelEncoding::Encode(pLogFontExW->elfEnumLogfontEx.elfScript).c_str());
        strcpy_s((char*)logFontExA.elfEnumLogfontEx.elfStyle,    sizeof(logFontExA.elfEnumLogfontEx.elfStyle),    SjisTunnelEncoding::Encode(pLogFontExW->elfEnumLogfontEx.elfStyle).c_str());
        logFontExA.elfDesignVector = pLogFontExW->elfDesignVector;
    }

    TEXTMETRICA textMetricA = ConvertTextMetricWToA(*lptm);
    return pContext->OriginalProc(&logFontExA.elfEnumLogfontEx.elfLogFont, &textMetricA, dwType, pContext->OriginalContext);
}

HFONT GdiProportionalizer::CreateFontAHook(int cHeight, int cWidth, int cEscapement, int cOrientation, int cWeight,
    DWORD bItalic, DWORD bUnderline, DWORD bStrikeOut, DWORD iCharSet, DWORD iOutPrecision, DWORD iClipPrecision,
    DWORD iQuality, DWORD iPitchAndFamily, LPCSTR pszFaceName)
{
    proxy_log(LogCategory::TEXT, "GdiProportionalizer::CreateFontAHook()");

    return CreateFontWHook(
        cHeight,
        cWidth,
        cEscapement,
        cOrientation,
        cWeight,
        bItalic,
        bUnderline,
        bStrikeOut,
        iCharSet,
        iOutPrecision,
        iClipPrecision,
        iQuality,
        iPitchAndFamily,
        StringUtil::ToWString(pszFaceName).c_str()
    );
}

HFONT GdiProportionalizer::CreateFontIndirectAHook(LOGFONTA* pFontInfo)
{
    proxy_log(LogCategory::TEXT, "GdiProportionalizer::CreateFontIndirectAHook()");

    return CreateFontWHook(
        pFontInfo->lfHeight,
        pFontInfo->lfWidth,
        pFontInfo->lfEscapement,
        pFontInfo->lfOrientation,
        pFontInfo->lfWeight,
        pFontInfo->lfItalic,
        pFontInfo->lfUnderline,
        pFontInfo->lfStrikeOut,
        pFontInfo->lfCharSet,
        pFontInfo->lfOutPrecision,
        pFontInfo->lfClipPrecision,
        pFontInfo->lfQuality,
        pFontInfo->lfPitchAndFamily,
        StringUtil::ToWString(pFontInfo->lfFaceName).c_str()
    );
}

HFONT GdiProportionalizer::CreateFontWHook(int cHeight, int cWidth, int cEscapement, int cOrientation, int cWeight,
    DWORD bItalic, DWORD bUnderline, DWORD bStrikeOut, DWORD iCharSet, DWORD iOutPrecision, DWORD iClipPrecision,
    DWORD iQuality, DWORD iPitchAndFamily, LPCWSTR pszFaceName)
{
    proxy_log(LogCategory::TEXT, "GdiProportionalizer::CreateFontWHook()");

    LOGFONTW fontInfo;
    fontInfo.lfHeight = cHeight;
    fontInfo.lfWidth = cWidth;
    fontInfo.lfEscapement = cEscapement;
    fontInfo.lfOrientation = cOrientation;
    fontInfo.lfWeight = cWeight;
    fontInfo.lfItalic = (BYTE) bItalic;
    fontInfo.lfUnderline = (BYTE) bUnderline;
    fontInfo.lfStrikeOut = (BYTE) bStrikeOut;
    fontInfo.lfCharSet = (BYTE) iCharSet;
    fontInfo.lfOutPrecision = (BYTE) iOutPrecision;
    fontInfo.lfClipPrecision = (BYTE) iClipPrecision;
    fontInfo.lfQuality = (BYTE) iQuality;
    fontInfo.lfPitchAndFamily = (BYTE) iPitchAndFamily;
    wcscpy_s(fontInfo.lfFaceName, pszFaceName);
    return CreateFontIndirectWHook(&fontInfo);
}

std::string WideToUTF8(const wchar_t* wstr)
{
    int size = WideCharToMultiByte(
        CP_UTF8, 0, wstr, -1, nullptr, 0, nullptr, nullptr);

    std::string result(size - 1, '\0');
    WideCharToMultiByte(
        CP_UTF8, 0, wstr, -1, result.data(), size, nullptr, nullptr);

    return result;
}

HFONT GdiProportionalizer::CreateFontIndirectWHook(LOGFONTW* pFontInfo)
{
    if (CustomFontName.empty())
    {
        LastFontName = pFontInfo->lfFaceName;
        proxy_log(LogCategory::TEXT, "GdiProportionalizer::CreateFontIndirectWHook(): engineRequestedFaceName: %s, height: %d",
            WideToUTF8(pFontInfo->lfFaceName).c_str(), pFontInfo->lfHeight);

        return FontManager.FetchFont(*pFontInfo)->GetGdiHandle();
    }

    LONG height = pFontInfo->lfHeight; // normally 21
#if ENLARGE_FONT
    height += RuntimeConfig::FontHeightIncrease();
#endif

    LastFontName = CustomFontName;

    proxy_log(LogCategory::TEXT, "GdiProportionalizer::CreateFontIndirectWHook(): CustomFontName: %ls, height: %d",
        CustomFontName.c_str(), height);

    return FontManager.FetchFont(CustomFontName, height, Bold, Italic, Underline)->GetGdiHandle();
}

HGDIOBJ GdiProportionalizer::SelectObjectHook(HDC hdc, HGDIOBJ obj)
{
    const unsigned char* currentText = PALGrabCurrentText::get();
    proxy_log(LogCategory::TEXT, "GdiProportionalizer::SelectObjectHook(): currentText: %s", currentText);

    // Check if this is a font we manage and if text contains Japanese characters
    Font* pFont = FontManager.GetFont(static_cast<HFONT>(obj));
    if (pFont != nullptr && !CustomFontName.empty() && currentText != nullptr)
    {
        wstring wideText = SjisTunnelEncoding::Decode(reinterpret_cast<const char*>(currentText));
        if (ContainsJapaneseCharacters(wideText.c_str()))
        {
            // Use MS Gothic at game default size for Japanese text
            pFont = FontManager.FetchFont(JAPANESE_FONT_NAME, GAME_DEFAULT_FONT_HEIGHT, false, false, false);
            obj = pFont->GetGdiHandle();
            proxy_log(LogCategory::TEXT, "GdiProportionalizer::SelectObjectHook(): Switching to Japanese font for text: %s", currentText);
        }
    }

    if (pFont != nullptr)
        CurrentFonts[hdc] = pFont;

    HGDIOBJ ret = SelectObject(hdc, obj);

#if LEGACY_KERNING
    DWORD count = GetKerningPairsW(hdc, 0, nullptr);
    if (count == 0) {
        proxy_log(LogCategory::TEXT, "A: 0 kerning pairs");
        // No pairs or error; optionally check GetLastError()
        return ret;
    }

    // Allocate and fetch the pairs
    std::vector<KERNINGPAIR> pairs(count);
    DWORD got = GetKerningPairsW(hdc, count, pairs.data());
    proxy_log(LogCategory::TEXT, "B: %d kerning pairs", got);
    if (got == 0) {
        // Failed; optionally check GetLastError()
        return ret;
    }

    // Store as key = wFirst | (wSecond << 16), value = iKernAmount
    kernAmounts.reserve(kernAmounts.size() + got);
    for (DWORD i = 0; i < got; ++i) {
        const KERNINGPAIR& kp = pairs[i];
        uint32_t key = static_cast<uint32_t>(kp.wFirst)
            | (static_cast<uint32_t>(kp.wSecond) << 16);
        kernAmounts[key] = static_cast<int>(kp.iKernAmount);
    }
#endif

    return ret;
}

BOOL GdiProportionalizer::DeleteObjectHook(HGDIOBJ obj)
{
    proxy_log(LogCategory::TEXT, "GdiProportionalizer::DeleteObjectHook()");

    currentTextOffset = 0;
    totalAdvOut = 0;
    Italic = false;
    Bold = false;
    Monospace = false;

    Font* pFont = FontManager.GetFont(static_cast<HFONT>(obj));
    if (pFont != nullptr)
        return false;

    return DeleteObject(obj);
}

BOOL GdiProportionalizer::GetTextExtentPointAHook(HDC hdc, LPCSTR lpString, int c, LPSIZE lpsz)
{
    proxy_log(LogCategory::TEXT, "GdiProportionalizer::GetTextExtentPointAHook()");

    wstring str = SjisTunnelEncoding::Decode(lpString, c);
    return GetTextExtentPointW(hdc, str.c_str(), str.size(), lpsz);
}

BOOL GdiProportionalizer::GetTextExtentPoint32AHook(HDC hdc, LPCSTR lpString, int c, LPSIZE psizl)
{
    proxy_log(LogCategory::TEXT, "GdiProportionalizer::GetTextExtentPoint32AHook()");

    wstring str = SjisTunnelEncoding::Decode(lpString, c);
    return GetTextExtentPoint32W(hdc, str.c_str(), str.size(), psizl);
}

BOOL GdiProportionalizer::TextOutAHook(HDC dc, int x, int y, LPCSTR pString, int count)
{
    proxy_log(LogCategory::TEXT, "GdiProportionalizer::TextOutAHook()");

    wstring text = SjisTunnelEncoding::Decode(pString, count);
    Font* pFont = CurrentFonts[dc];
    if (pFont == nullptr)
    {
        pFont = FontManager.FetchFont(L"System", 12, false, false, false);
        SelectObjectHook(dc, pFont->GetGdiHandle());
    }

    if (!AdaptRenderArgs(text.c_str(), text.size(), pFont->GetHeight(), x, y))
        return false;

    if (!CustomFontName.empty() && (pFont->IsBold() != Bold || pFont->IsItalic() != Italic || pFont->IsUnderline() != Underline))
    {
        pFont = FontManager.FetchFont(CustomFontName, pFont->GetHeight(), Bold, Italic, Underline);
        SelectObjectHook(dc, pFont->GetGdiHandle());
    }

    return TextOutW(dc, x, y, text.data(), text.size());
}

std::string FuFormatToString(UINT fuFormat)
{
    std::ostringstream oss;

    // --- Main formats (mutually exclusive) ---
    switch (fuFormat & 0xFF) { // low byte stores the main mode
    case GGO_METRICS:       oss << "GGO_METRICS"; break;
    case GGO_BITMAP:        oss << "GGO_BITMAP"; break;
    case GGO_NATIVE:        oss << "GGO_NATIVE"; break;
    case GGO_BEZIER:        oss << "GGO_BEZIER"; break;
    case GGO_GRAY2_BITMAP:  oss << "GGO_GRAY2_BITMAP"; break;
    case GGO_GRAY4_BITMAP:  oss << "GGO_GRAY4_BITMAP"; break;
    case GGO_GRAY8_BITMAP:  oss << "GGO_GRAY8_BITMAP"; break;
    default:                oss << "UNKNOWN_FORMAT(" << (fuFormat & 0xFF) << ")"; break;
    }

    // --- Option flags (may combine) ---
    if (fuFormat & GGO_GLYPH_INDEX)
        oss << " | GGO_GLYPH_INDEX";

    if (fuFormat & GGO_UNHINTED)
        oss << " | GGO_UNHINTED";

    return oss.str();
}


std::string GlyphMetricsToString(const GLYPHMETRICS* gm)
{
    if (!gm) return "NULL";

    std::ostringstream oss;
    oss << "GLYPHMETRICS { "
        << "gmBlackBoxX=" << gm->gmBlackBoxX << ", "
        << "gmBlackBoxY=" << gm->gmBlackBoxY << ", "
        << "gmptGlyphOrigin=("
        << gm->gmptGlyphOrigin.x << ", "
        << gm->gmptGlyphOrigin.y << "), "
        << "gmCellIncX=" << gm->gmCellIncX << ", "
        << "gmCellIncY=" << gm->gmCellIncY
        << " }";

    return oss.str();
}

static const unsigned char* sjis_next_char(const unsigned char* p) {
    if (!p || *p == '\0') return p; // Null or end of string

    unsigned char c = *p;

    // Double-byte lead byte ranges:
    // 0x81–0x9F or 0xE0–0xEF
    if ((c >= 0x81 && c <= 0x9F) || (c >= 0xE0 && c <= 0xEF)) {
        // Ensure next byte exists before advancing
        if (*(p + 1) != '\0')
            return p + 2;
        else
            return p + 1; // Avoid reading past string end
    }

    // Otherwise, single-byte character (ASCII or half-width kana)
    return p + 1;
}

static UINT SJISCharToUnicode(const string& sjisStr, bool mapPipeToSpace) {

    wstring wstr = SjisTunnelEncoding::Decode(sjisStr);

    UINT ch = wstr[0];

    // Workarounds for characters that don't appear correctly if plumbed through via normal SJIS character codes.
    // This code is intended to reverse the replacements into the half-width katakana range in SoftpalScript.WritePatched().
    // Note that this code relies on /source-charset:utf-8 /execution-charset:.932
    switch (sjisStr[0]) {
    case MAP_SJIS_1:
        ch = MAP_UNICODE_1; break;
    case MAP_SJIS_2:
        ch = MAP_UNICODE_2; break;
    case MAP_SJIS_3:
        ch = MAP_UNICODE_3; break;
    case MAP_SJIS_4:
        ch = MAP_UNICODE_4; break;
    case MAP_SJIS_5:
        ch = MAP_UNICODE_5; break;
    case MAP_SJIS_6:
        ch = MAP_UNICODE_6; break;
    case MAP_SJIS_7:
        ch = MAP_UNICODE_7; break;
    case MAP_SJIS_8:
        ch = MAP_UNICODE_8; break;
    }

    if (mapPipeToSpace && ch == MAP_SPACE_CHARACTER) {
        ch = ' ';
    }

    return ch;
}

static int GetStringAdvance(HDC hdc, SCRIPT_CACHE* psc, const wchar_t* text, int count)
{
    SCRIPT_ITEM items[4];
    int numItems = 0;
    if (FAILED(ScriptItemize(text, count, _countof(items), nullptr, nullptr, items, &numItems)))
        return 0;

    int runLen = (numItems >= 1) ? (count - items[0].iCharPos) : count;

    std::vector<WORD> glyphs(count * 3);
    std::vector<WORD> logClust(count);
    std::vector<SCRIPT_VISATTR> visAttr(glyphs.size());
    int numGlyphs = 0;

    HRESULT hr = ScriptShape(hdc, psc, text, count, (int)glyphs.size(),
        &items[0].a, glyphs.data(), logClust.data(),
        visAttr.data(), &numGlyphs);
    if (FAILED(hr) || numGlyphs <= 0) return 0;

    std::vector<int> advances(numGlyphs);
    std::vector<GOFFSET> offsets(numGlyphs);
    ABC abc = {};

    hr = ScriptPlace(hdc, psc, glyphs.data(), numGlyphs,
        visAttr.data(), &items[0].a,
        advances.data(), offsets.data(), &abc);
    if (FAILED(hr)) return 0;

    int total = 0;
    for (int i = 0; i < numGlyphs; ++i) total += advances[i];
    return total;
}

static int GetKerningAdjustment(HDC hdc, SCRIPT_CACHE* psc, wchar_t c1, wchar_t c2) {
    // 1. Setup the pair string
    wchar_t pair[2] = { c1, c2 };

    proxy_log(LogCategory::TEXT, "GdiProportionalizer::GetKerningAdjustment A: c1: %lc, c2: %lc", c1, c2);

    int wPair = GetStringAdvance(hdc, psc, pair, 2);
    int w1 = GetStringAdvance(hdc, psc, &c1, 1);
    int w2 = GetStringAdvance(hdc, psc, &c2, 1);

    proxy_log(LogCategory::TEXT, "GdiProportionalizer::GetKerningAdjustment B: wPair: %d, w1: %d, w2: %d", wPair, w1, w2);

    return wPair - (w1 + w2);
}

// Helper: Check for control codes at current position and update font state flags
// Returns true if any font-affecting control code was found
bool GdiProportionalizer::ProcessControlCode(const unsigned char* pos)
{
    bool fontChanged = false;

    if (!strncmp((const char*)pos, "<i>", 3) && Italic != true) {
        Italic = true;
        fontChanged = true;
    }
    if (!strncmp((const char*)pos, "</i>", 4) && Italic != false) {
        Italic = false;
        fontChanged = true;
    }
    if (!strncmp((const char*)pos, "<b>", 3) && Bold != true) {
        Bold = true;
        fontChanged = true;
    }
    if (!strncmp((const char*)pos, "</b>", 4) && Bold != false) {
        Bold = false;
        fontChanged = true;
    }
    if (!strncmp((const char*)pos, "<monospace>", 11) && Monospace != true) {
        Monospace = true;
        fontChanged = true;
    }
    if (!strncmp((const char*)pos, "</monospace>", 12) && Monospace != false) {
        Monospace = false;
        fontChanged = true;
    }

    return fontChanged;
}

// Helper: Apply current font state to HDC
void GdiProportionalizer::ApplyFontState(HDC hdc)
{
    Font* pFont = CurrentFonts[hdc];
    if (pFont != nullptr)
    {
        if (Monospace && !MonospaceFontName.empty()) {
            pFont = FontManager.FetchFont(MonospaceFontName, pFont->GetHeight(), Bold, Italic, Underline);
        } else if (!CustomFontName.empty()) {
            pFont = FontManager.FetchFont(CustomFontName, pFont->GetHeight(), Bold, Italic, Underline);
        }
        SelectObject(hdc, pFont->GetGdiHandle());
    }
}

DWORD GdiProportionalizer::GetGlyphOutlineAHook(HDC hdc, UINT uChar, UINT fuFormat, LPGLYPHMETRICS lpgm, DWORD cjBuffer, LPVOID pvBuffer, MAT2* lpmat2)
{
    string sjisStr;
    while (uChar != 0)
    {
        sjisStr.insert(0, 1, (char)uChar);
        uChar >>= 8;
    }
    UINT ch = SJISCharToUnicode(sjisStr, false);

    // Process control codes at the very beginning of text (before first character is rendered)
    if (currentTextOffset == 0) {
        const unsigned char* textString = PALGrabCurrentText::get();
        const unsigned char* scanPos = textString;
        bool fontChanged = false;

        while (*scanPos == '<') {
            fontChanged |= ProcessControlCode(scanPos);
            // Skip past this control code, updating currentTextOffset
            const unsigned char* prevPos;
            do {
                int charLen = sjis_next_char(scanPos) - scanPos;
                currentTextOffset += charLen;
                prevPos = scanPos;
                scanPos += charLen;
            } while (*prevPos != '>' && *scanPos != '\0');
        }

        if (fontChanged) {
            ApplyFontState(hdc);
        }
    }

    DWORD ret = GetGlyphOutlineW(hdc, ch, fuFormat, lpgm, cjBuffer, pvBuffer, lpmat2);

    // Workaround to make '|' behave as if it were a space.
    // This code is intended to reverse the ' ' -> '|' replacement in SoftpalScript.WritePatched().
    // (We can't use space characters in TEXT.DAT because SoftPal hardcodes an advance distance for them.)
    if (ch == MAP_SPACE_CHARACTER) {
        ch = ' ';

        // Wipe all pixels from the bitmap GetGlyphOutlineW created
        // (This substitution needs to be after the GetGlyphOutlineW call to avoid a crash.)
        if (pvBuffer && cjBuffer >= ret) {
            memset(pvBuffer, 0, ret);
        }
    }

    const unsigned char* textString = PALGrabCurrentText::get();
    const unsigned char* currentChar = textString + currentTextOffset;
    int sjisCharLength = sjis_next_char(currentChar) - currentChar;

    // TODO: support skipping control codes here (for the rare situation where control code is between two characters with nonzero kerning relationship)
    const unsigned char* nextChar = currentChar + sjisCharLength;
    int sjisNextCharLength = sjis_next_char(nextChar) - nextChar;
    string nextCharStr;
    while (sjisNextCharLength > 0) {
        nextCharStr.insert(0, 1, *nextChar);
        nextChar++;
        sjisNextCharLength--;
    }
    UINT nextCharUnicode = SJISCharToUnicode(nextCharStr, true);

#if LEGACY_KERNING
    uint32_t kernKey = static_cast<uint32_t>(ch) | (static_cast<uint32_t>(nextCharUnicode) << 16);
    int kern = kernAmounts[kernKey];
#else
    SCRIPT_CACHE sc = NULL; // Must be initialized to NULL
    int kern = GetKerningAdjustment(hdc, &sc, ch, nextCharUnicode);
    ScriptFreeCache(&sc);
#endif

    ABCFLOAT abc;
    GetCharABCWidthsFloatW(hdc, ch, ch, &abc);
    double advanceF = abc.abcfA + abc.abcfB + abc.abcfC + kern;
    int advOut = (int)floor(advanceF + 0.5);
    

    proxy_log(LogCategory::TEXT, "GdiProportionalizer::GetGlyphOutlineAHook() codepage: %d, fuFormat: %s, sjisChar: %s, currentText char: %c, Unicode 0x%x, nextChar: %c, pvBuffer: %d, cjBuffer: %d, metricsResult: %s, advOut: %d, "
        "totalAdvOut: %d, a: %f, b: %f, c: %f, kern: %d",
        GetACP(),
        FuFormatToString(fuFormat).c_str(),
        reinterpret_cast<const char*>(sjisStr.c_str()),
        *currentChar,
        ch,
        (char) nextCharUnicode,
        pvBuffer != NULL,
        cjBuffer,
        GlyphMetricsToString(lpgm).c_str(),
        advOut, totalAdvOut, abc.abcfA, abc.abcfB, abc.abcfC, kern);

    if (pvBuffer) {
        bool fontChanged = false;
        currentTextOffset += sjisCharLength;
        currentChar += sjisCharLength;
        while (*currentChar == '<') {
            proxy_log(LogCategory::TEXT, "GdiProportionalizer control code currentChar: %s", currentChar);

            fontChanged |= ProcessControlCode(currentChar);

            const unsigned char* previousChar;
            do {
                sjisCharLength = sjis_next_char(currentChar) - currentChar;
                currentTextOffset += sjisCharLength;
                previousChar = currentChar;
                currentChar += sjisCharLength;
            } while (*previousChar != '>' && *currentChar != '\0');
        }


        if (ch == MAP_UNICODE_7 || ch == MAP_UNICODE_8) {
            // Just finished rendering music note or heart - restore normal font
            fontChanged = true;
        }

        totalAdvOut += advOut;

        if (fontChanged) {
            proxy_log(LogCategory::TEXT, "GdiProportionalizer font properties changed: Bold: %d, Italic: %d, Underline: %d", Bold, Italic, Underline);
            Font* pFont = CurrentFonts[hdc];
            if (pFont != nullptr)
            {
                if (Monospace && !MonospaceFontName.empty()) {
                    pFont = FontManager.FetchFont(MonospaceFontName, pFont->GetHeight(), Bold, Italic, Underline);
                } else if (!CustomFontName.empty()) {
                    pFont = FontManager.FetchFont(CustomFontName, pFont->GetHeight(), Bold, Italic, Underline);
                }
                SelectObjectHook(hdc, pFont->GetGdiHandle());
            }
        }

        // Check if next character is music note - switch to a symbol font for that character
        int nextSjisCharLength = sjis_next_char(currentChar) - currentChar;
        string nextSjisStr((const char*)currentChar, nextSjisCharLength);
        UINT nextCh = SJISCharToUnicode(nextSjisStr, false);
        if (nextCh == MAP_UNICODE_7 || nextCh == MAP_UNICODE_8) {
            Font* pFont = CurrentFonts[hdc];
            if (pFont != nullptr) {
                Font* segoeUI = FontManager.FetchFont(L"Segoe UI Symbol", pFont->GetHeight(), false, false, false);
                SelectObject(hdc, segoeUI->GetGdiHandle());
            }
        }
    }

    // SoftPal systematically adds 2 extra pixels of spacing after every character, beyond what the font specifies.
    // This is not very noticeable with Japanese characters, but it's extremely noticeable with a proportional Latin font.
    // Cancel out that behavior here.
    advOut = max(0, advOut - 2);

    lpgm->gmCellIncX = advOut;

#if ENLARGE_FONT
    // Text is too low in the textbox for enlarged fonts
    lpgm->gmptGlyphOrigin.y += RuntimeConfig::FontYTopPosDecrease();
#endif

    return ret;
}

LOGFONTA GdiProportionalizer::ConvertLogFontWToA(const LOGFONTW& logFontW)
{
    proxy_log(LogCategory::TEXT, "GdiProportionalizer::ConvertLogFontWToA()");

    LOGFONTA logFontA;
    logFontA.lfCharSet = logFontW.lfCharSet;
    logFontA.lfClipPrecision = logFontW.lfClipPrecision;
    logFontA.lfEscapement = logFontW.lfEscapement;
    strcpy_s(logFontA.lfFaceName, SjisTunnelEncoding::Encode(logFontW.lfFaceName).c_str());
    logFontA.lfHeight = logFontW.lfHeight;
    logFontA.lfItalic = logFontW.lfItalic;
    logFontA.lfOrientation = logFontW.lfOrientation;
    logFontA.lfOutPrecision = logFontW.lfOutPrecision;
    logFontA.lfPitchAndFamily = logFontW.lfPitchAndFamily;
    logFontA.lfQuality = logFontW.lfQuality;
    logFontA.lfStrikeOut = logFontW.lfStrikeOut;
    logFontA.lfUnderline = logFontW.lfUnderline;
    logFontA.lfWeight = logFontW.lfWeight;
    logFontA.lfWidth = logFontW.lfWidth;
    return logFontA;
}

LOGFONTW GdiProportionalizer::ConvertLogFontAToW(const LOGFONTA& logFontA)
{
    proxy_log(LogCategory::TEXT, "GdiProportionalizer::ConvertLogFontAToW()");

    LOGFONTW logFontW;
    logFontW.lfCharSet = logFontA.lfCharSet;
    logFontW.lfClipPrecision = logFontA.lfClipPrecision;
    logFontW.lfEscapement = logFontA.lfEscapement;
    wcscpy_s(logFontW.lfFaceName, SjisTunnelEncoding::Decode(logFontA.lfFaceName).c_str());
    logFontW.lfHeight = logFontA.lfHeight;
    logFontW.lfItalic = logFontA.lfItalic;
    logFontW.lfOrientation = logFontA.lfOrientation;
    logFontW.lfOutPrecision = logFontA.lfOutPrecision;
    logFontW.lfPitchAndFamily = logFontA.lfPitchAndFamily;
    logFontW.lfQuality = logFontA.lfQuality;
    logFontW.lfStrikeOut = logFontA.lfStrikeOut;
    logFontW.lfUnderline = logFontA.lfUnderline;
    logFontW.lfWeight = logFontA.lfWeight;
    logFontW.lfWidth = logFontA.lfWidth;
    return logFontW;
}

TEXTMETRICA GdiProportionalizer::ConvertTextMetricWToA(const TEXTMETRICW& textMetricW)
{
    proxy_log(LogCategory::TEXT, "GdiProportionalizer::ConvertTextMetricWToA()");

    TEXTMETRICA textMetricA;
    textMetricA.tmAscent = textMetricW.tmAscent;
    textMetricA.tmAveCharWidth = textMetricW.tmAveCharWidth;
    textMetricA.tmBreakChar = textMetricW.tmBreakChar < 0x100 ? (BYTE)textMetricW.tmBreakChar : '?';
    textMetricA.tmCharSet = textMetricW.tmCharSet;
    textMetricA.tmDefaultChar = textMetricW.tmDefaultChar < 0x100 ? (BYTE)textMetricW.tmDefaultChar : '?';
    textMetricA.tmDescent = textMetricW.tmDescent;
    textMetricA.tmDigitizedAspectX = textMetricW.tmDigitizedAspectX;
    textMetricA.tmDigitizedAspectY = textMetricW.tmDigitizedAspectY;
    textMetricA.tmExternalLeading = textMetricW.tmExternalLeading;
    textMetricA.tmFirstChar = (BYTE)min(textMetricW.tmFirstChar, 0xFF);
    textMetricA.tmHeight = textMetricW.tmHeight;
    textMetricA.tmInternalLeading = textMetricW.tmInternalLeading;
    textMetricA.tmItalic = textMetricW.tmItalic;
    textMetricA.tmLastChar = (BYTE)min(textMetricW.tmLastChar, 0xFF);
    textMetricA.tmMaxCharWidth = textMetricW.tmMaxCharWidth;
    textMetricA.tmOverhang = textMetricW.tmOverhang;
    textMetricA.tmPitchAndFamily = textMetricW.tmPitchAndFamily;
    textMetricA.tmStruckOut = textMetricW.tmStruckOut;
    textMetricA.tmUnderlined = textMetricW.tmUnderlined;
    textMetricA.tmWeight = textMetricW.tmWeight;
    return textMetricA;
}

