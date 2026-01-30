#include "pch.h"

#include "PALHooks.h"
#include <sstream>

void* OriginalEntryPoint;

static void ShowErrorAndExit(const std::wstring& message)
{
    MessageBoxW(nullptr, message.c_str(), L"VNTranslationTools Error", MB_OK | MB_ICONERROR);
    ExitProcess(1);
}

static void CheckRequiredDataFiles()
{
    const wchar_t* requiredFiles[] = {
        L"data\\script.src",
        L"data\\TEXT.DAT"
    };

    for (const wchar_t* filePath : requiredFiles)
    {
        if (GetFileAttributesW(filePath) == INVALID_FILE_ATTRIBUTES)
        {
            std::wstringstream ss;
            ss << L"Required data file not found: " << filePath << L"\n\n";
            ss << L"Please ensure you have run VNTextPatch to insert the translated script.";
            ShowErrorAndExit(ss.str());
        }
    }
}

void Initialize();

__declspec(naked) void EntryPointHook()
{
    __asm
    {
        call Initialize
        jmp OriginalEntryPoint
    }
}

void Initialize()
{
    if (OriginalEntryPoint != nullptr)
        DetourDetach(&OriginalEntryPoint, &EntryPointHook);

    // Uncomment for games that only work in a Japanese locale
    // (and include LoaderDll.dll and LocaleEmulator.dll from https://github.com/xupefei/Locale-Emulator/releases)
    /*
    if (GetACP() != 932)
    {
        if (LocaleEmulator::Relaunch())
            ExitProcess(0);
    }
    //*/

    SetCurrentDirectoryW(Path::GetModuleFolderPath(nullptr).c_str());
    RuntimeConfig::Load();
    CheckRequiredDataFiles();

    CompilerHelper::Init();
    Win32AToWAdapter::Init();
    SjisTunnelEncoding::PatchGameLookupTable();

    GdiProportionalizer::Init();
//    D2DProportionalizer::Init();

    EnginePatches::Init();

    PALGrabCurrentText::Install();
    PALVideoFix::Install();
}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD ul_reason_for_call, LPVOID lpReserved)
{
    switch (ul_reason_for_call)
    {
    case DLL_PROCESS_ATTACH:
        Proxy::Init(hModule);

#if _DEBUG
        Initialize();
#else
        OriginalEntryPoint = DetourGetEntryPoint(nullptr);
        DetourTransactionBegin();
        DetourAttach(&OriginalEntryPoint, EntryPointHook);
        DetourTransactionCommit();
#endif
        break;
    	
    case DLL_PROCESS_DETACH:
        break;
    }
    return TRUE;
}
