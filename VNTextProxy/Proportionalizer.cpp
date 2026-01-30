#include "pch.h"
#include "SharedConstants.h"

using namespace std;

void Proportionalizer::Init()
{
    LastFontName = CustomFontName = LoadCustomFont();
    MonospaceFontName = LoadMonospaceFont();
}

int Proportionalizer::MeasureStringWidth(const wstring& str, int fontSize)
{
    Font* pFont = FontManager.FetchFont(LastFontName, fontSize, false, false, false);
    return pFont->MeasureStringWidth(str);
}

bool Proportionalizer::AdaptRenderArgs(const wchar_t* pText, int length, int fontSize, int& x, int& y)
{
    static int startX = 0;
    static int lastMonospaceX = 0;
    static int lastMonospaceY = 0;
    static int lastProportionalX = 0;
    static wchar_t lastChar = L'\0';
    static int nextProportionalX = 0;

    if (length != 1)
        return true;

    wchar_t currentChar = pText[0];
    if (HandleFormattingCode(currentChar))
        return false;

    Font* pFont = FontManager.FetchFont(LastFontName, fontSize, Bold, Italic, Underline);

    if (x == 0 || x < lastMonospaceX - 4 || abs(y - lastMonospaceY) > 4)
    {
        // To the left of previously rendered text or different Y -> reset
        startX = x;
        lastMonospaceX = x;
        lastMonospaceY = y;
        lastProportionalX = x;

        lastChar = currentChar;
        nextProportionalX = x + pFont->MeasureCharWidth(currentChar);
    }
    else if (x <= lastMonospaceX + 4)
    {
        // Close to previously rendered text (e.g. shadow) -> calculate offset
        int offset = x - lastMonospaceX;
        x = lastProportionalX + offset;
    }
    else
    {
        // Far to the right of previously rendered text -> next char
        lastMonospaceX = x;

        x = nextProportionalX + pFont->GetKernAmount(lastChar, currentChar);
        lastProportionalX = x;
        lastChar = currentChar;
        nextProportionalX = x + pFont->MeasureCharWidth(currentChar);
    }

    LastLineEnd = nextProportionalX;
    return true;
}

bool Proportionalizer::HandleFormattingCode(wchar_t c)
{
    switch (c)
    {
    case L'龠':
        Bold = !Bold;
        return true;

    case L'籥':
        Italic = !Italic;
        return true;

    case L'鑰':
        Underline = !Underline;
        return true;
    }

    return false;
}

wstring Proportionalizer::LoadCustomFont()
{
    CustomFontFilePath = FindCustomFontFile();
    if (CustomFontFilePath.empty())
        return L"";

    int numFonts = AddFontResourceExW(CustomFontFilePath.c_str(), FR_PRIVATE, nullptr);
    if (numFonts == 0)
        return L"";

    wstring fontFileName = CustomFontFilePath;
    fontFileName.erase(0, fontFileName.rfind(L'\\') + 1);
    fontFileName.erase(fontFileName.find(L'.'), -1);
    return fontFileName;
}

wstring Proportionalizer::LoadMonospaceFont()
{
    // Build path: same folder as exe + MONOSPACE_FONT_FILENAME
    wchar_t folderPath[MAX_PATH];
    GetModuleFileName(GetModuleHandle(nullptr), folderPath, sizeof(folderPath) / sizeof(wchar_t));
    wchar_t* pLastSlash = wcsrchr(folderPath, L'\\');
    if (pLastSlash != nullptr)
        *pLastSlash = L'\0';

    wstring fontPath = wstring(folderPath) + L"\\" + RuntimeConfig::MonospaceFontFilename();
    if (GetFileAttributesW(fontPath.c_str()) == INVALID_FILE_ATTRIBUTES)
        return L"";

    int numFonts = AddFontResourceExW(fontPath.c_str(), FR_PRIVATE, nullptr);
    if (numFonts == 0)
        return L"";

    // Extract font name from filename (remove path and extension)
    wstring fontFileName = fontPath;
    fontFileName.erase(0, fontFileName.rfind(L'\\') + 1);
    fontFileName.erase(fontFileName.find(L'.'), -1);
    return fontFileName;
}

wstring Proportionalizer::FindCustomFontFile()
{
    wchar_t folderPath[MAX_PATH];
    GetModuleFileName(GetModuleHandle(nullptr), folderPath, sizeof(folderPath) / sizeof(wchar_t));
    wchar_t* pLastSlash = wcsrchr(folderPath, L'\\');
    if (pLastSlash != nullptr)
        *pLastSlash = L'\0';

    wstring fontPath = wstring(folderPath) + L"\\" + RuntimeConfig::CustomFontFilename();
    if (GetFileAttributesW(fontPath.c_str()) != INVALID_FILE_ATTRIBUTES)
        return fontPath;

    wchar_t winDir[MAX_PATH];
    (void) GetWindowsDirectoryW(winDir, MAX_PATH);
    return std::wstring(winDir) + L"\\Fonts\\Arial.ttf";
}