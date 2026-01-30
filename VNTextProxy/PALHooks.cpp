#include "pch.h"

#include <string>
#include <windows.h>

#include "SharedConstants.h"

static FILE* g_logFile = nullptr;
static void dbg_log(const char* format, ...)
{
    if (!RuntimeConfig::DebugLogging())
        return;
    if (!g_logFile)
        g_logFile = _fsopen("./PALhooks.log", "w", _SH_DENYNO);
    if (g_logFile)
    {
        va_list args;
        va_start(args, format);
        vfprintf(g_logFile, format, args);
        fprintf(g_logFile, "\n");
        va_end(args);
        fflush(g_logFile);
    }
}

namespace PALGrabCurrentText
{
    static void* (__cdecl* oPalTaskGetSubTaskData)() = nullptr;

    const unsigned char* get()
    {
        return (const unsigned char*)oPalTaskGetSubTaskData() + 0x204;
    }

    bool Install()
    {
        dbg_log("PalGrabCurrentText::Install start");

        LoadLibraryA("./dll/ogg.dll");
        LoadLibraryA("./dll/vorbis.dll");
        LoadLibraryA("./dll/vorbisfile.dll");
        HMODULE hMod = LoadLibraryA("./dll/PAL.dll");
        if (!hMod)
            return false;

        oPalTaskGetSubTaskData = (decltype(oPalTaskGetSubTaskData))GetProcAddress(hMod, "PalTaskGetSubTaskData");
        if (!oPalTaskGetSubTaskData)
            return false;

        dbg_log("PalGrabCurrentText::Install completed");

        return true;
    }
}

namespace PALVideoFix
{
    namespace
    {
        // Used by the game to switch between display modes (0:Windowed, 1:Fullscreen),
        // where wParam is the display mode to switch to.
        constexpr UINT MSG_TOGGLE_DISPLAY_MODE = WM_USER + 2;

        constexpr const char* TARGET_DLL_NAME = "./dll/PAL.dll";
        constexpr const char* TARGET_FUNCTION_NAME = "PalVideoPlay";
        constexpr uintptr_t GAME_MANAGER_POINTER_OFFSET = 0x30989F8;

#pragma pack(push, 1)
        struct GameManager
        {
            void* pGameDevice;
            BYTE gap4[4];
            HWND hWnd;
            BOOL isRunning;
            BYTE gap10[12];
            DWORD dword1C;
            BYTE gap20[8];
            DWORD dword28;
            BYTE gap2C[260];
            HANDLE hThread2;
            HANDLE hThread1;
            HANDLE hEvent2;
            HANDLE hEvent1;
            DWORD threadId2;
            DWORD threadId1;
            BYTE gap148[116];
            DWORD dword1BC;
            DWORD dword1C0;
            WORD word1C4;
            WORD word1C6;
            WORD word1C8;
            BYTE gap1CA[162];
            DWORD defferedWindowMode; // 0 for Windowed, 1 for Fullscreen
        };
#pragma pack(pop)

        static GameManager* g_pGameMgr = nullptr;
        static int(__cdecl* oPalVideoPlay)(const char* fileName) = nullptr;

        int __cdecl PalVideoPlay_Hook(const char* fileName)
        {
            static bool isInitialized = false;
            if (!isInitialized)
            {
                dbg_log("PalVideoPlay_Hook: First run, performing initialization...");
                HMODULE hMod = GetModuleHandleA(TARGET_DLL_NAME);
                if (hMod)
                {
                    uintptr_t moduleBase = (uintptr_t)hMod;
                    g_pGameMgr = *(GameManager**)(moduleBase + GAME_MANAGER_POINTER_OFFSET);
                    dbg_log("PalVideoPlay_Hook: Module base=0x%p, g_pGameMgr=0x%p, g_pGameMgr->hWnd=0x%p", hMod, g_pGameMgr, g_pGameMgr->hWnd);
                }
                else
                {
                    dbg_log("PalVideoPlay_Hook: ERROR - Could not get module handle for '%s'", TARGET_DLL_NAME);
                }
                isInitialized = true;
            }

            dbg_log("PalVideoPlay_Hook: Playing '%s'", fileName);

            int result = oPalVideoPlay(fileName);

            if (g_pGameMgr && g_pGameMgr->defferedWindowMode != 0)
            {
                dbg_log("PalVideoPlay_Hook: Fullscreen mode detected. Posting messages to reset display.");
                PostMessageA(g_pGameMgr->hWnd, MSG_TOGGLE_DISPLAY_MODE, 0, 0);
                PostMessageA(g_pGameMgr->hWnd, MSG_TOGGLE_DISPLAY_MODE, 1, 0);
            }
            else
            {
                dbg_log("PalVideoPlay_Hook: Windowed mode detected. No action needed.");
            }

            return result;
        }
    }

    bool Install()
    {
        dbg_log("VideoFix::Install() called.");

        LoadLibraryA("./dll/ogg.dll");
        LoadLibraryA("./dll/vorbis.dll");
        LoadLibraryA("./dll/vorbisfile.dll");
        HMODULE hMod = LoadLibraryA(TARGET_DLL_NAME);
        if (!hMod)
        {
            dbg_log("VideoFix::Install: Failed to load '%s'.", TARGET_DLL_NAME);
            return false;
        }

        oPalVideoPlay = (decltype(oPalVideoPlay))GetProcAddress(hMod, TARGET_FUNCTION_NAME);
        if (!oPalVideoPlay)
        {
            dbg_log("VideoFix::Install: Failed to find function '%s' in '%s'.", TARGET_FUNCTION_NAME, TARGET_DLL_NAME);
            return false;
        }

        dbg_log("VideoFix::Install: Found '%s' at address 0x%p.", TARGET_FUNCTION_NAME, oPalVideoPlay);

        DetourTransactionBegin();
        DetourUpdateThread(GetCurrentThread());
        DetourAttach(&(PVOID&)oPalVideoPlay, PalVideoPlay_Hook);
        LONG error = DetourTransactionCommit();

        if (error == NO_ERROR)
        {
            dbg_log("VideoFix::Install: Hook for '%s' installed successfully.", TARGET_FUNCTION_NAME);
            return true;
        }

        dbg_log("VideoFix::Install: Failed to install hook, Detours error: %d", error);
        oPalVideoPlay = nullptr;
        return false;
    }
}
