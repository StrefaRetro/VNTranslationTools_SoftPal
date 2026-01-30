#include "pch.h"

#include <string>
#include <windows.h>
#include <dshow.h>
#include <control.h>

#include "SharedConstants.h"
#include "BorderlessState.h"

#pragma comment(lib, "strmiids.lib")

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

// DirectShow video scaling for borderless mode
// Hooks IVideoWindow::SetWindowPosition to scale video to match our borderless scaling
namespace DirectShowVideoScale
{
    static const GUID LOCAL_CLSID_FilterGraph = { 0xe436ebb3, 0x524f, 0x11ce, { 0x9f, 0x53, 0x00, 0x20, 0xaf, 0x0b, 0xa7, 0x70 } };

    // Original function pointers
    static HRESULT(WINAPI* oCoCreateInstance)(REFCLSID, LPUNKNOWN, DWORD, REFIID, LPVOID*) = nullptr;
    static HRESULT(STDMETHODCALLTYPE* oGB_QueryInterface)(IGraphBuilder*, REFIID, void**) = nullptr;
    static HRESULT(STDMETHODCALLTYPE* oVW_SetWindowPosition)(IVideoWindow*, long, long, long, long) = nullptr;

    // Track hooked interfaces to avoid double-hooking
    static IGraphBuilder* g_pGraphBuilder = nullptr;
    static IVideoWindow* g_pVideoWindow = nullptr;

    static void PatchVtable(void** vtable, int index, void* hookFunc, void** originalFunc)
    {
        DWORD oldProtect;
        if (VirtualProtect(&vtable[index], sizeof(void*), PAGE_EXECUTE_READWRITE, &oldProtect))
        {
            *originalFunc = vtable[index];
            vtable[index] = hookFunc;
            VirtualProtect(&vtable[index], sizeof(void*), oldProtect, &oldProtect);
        }
    }

    // IVideoWindow::SetWindowPosition hook - scales video position/size in borderless mode
    static HRESULT STDMETHODCALLTYPE VW_SetWindowPosition_Hook(IVideoWindow* pThis, long Left, long Top, long Width, long Height)
    {
        dbg_log("IVideoWindow::SetWindowPosition: %d,%d %dx%d", Left, Top, Width, Height);

        if (BorderlessState::g_borderlessActive)
        {
            long scaledLeft = BorderlessState::g_offsetX;
            long scaledTop = BorderlessState::g_offsetY;
            long scaledWidth = BorderlessState::g_scaledWidth;
            long scaledHeight = BorderlessState::g_scaledHeight;
            dbg_log("  [Borderless] Scaled to: %d,%d %dx%d", scaledLeft, scaledTop, scaledWidth, scaledHeight);
            return oVW_SetWindowPosition(pThis, scaledLeft, scaledTop, scaledWidth, scaledHeight);
        }

        return oVW_SetWindowPosition(pThis, Left, Top, Width, Height);
    }

    static void HookVideoWindow(IVideoWindow* pVW)
    {
        if (g_pVideoWindow) return;
        g_pVideoWindow = pVW;

        void** vtable = *(void***)pVW;
        // IVideoWindow vtable index 39 = SetWindowPosition
        PatchVtable(vtable, 39, (void*)VW_SetWindowPosition_Hook, (void**)&oVW_SetWindowPosition);
        dbg_log("DirectShowVideoScale: Hooked IVideoWindow::SetWindowPosition");
    }

    // IGraphBuilder::QueryInterface hook - catches IVideoWindow requests
    static HRESULT STDMETHODCALLTYPE GB_QueryInterface_Hook(IGraphBuilder* pThis, REFIID riid, void** ppvObject)
    {
        HRESULT hr = oGB_QueryInterface(pThis, riid, ppvObject);

        if (SUCCEEDED(hr) && ppvObject && *ppvObject && riid == IID_IVideoWindow)
        {
            HookVideoWindow((IVideoWindow*)*ppvObject);
        }

        return hr;
    }

    static void HookGraphBuilder(IGraphBuilder* pGB)
    {
        if (g_pGraphBuilder) return;
        g_pGraphBuilder = pGB;

        void** vtable = *(void***)pGB;
        // IUnknown vtable index 0 = QueryInterface
        PatchVtable(vtable, 0, (void*)GB_QueryInterface_Hook, (void**)&oGB_QueryInterface);
        dbg_log("DirectShowVideoScale: Hooked IGraphBuilder::QueryInterface");
    }

    // CoCreateInstance hook - catches FilterGraph creation
    static HRESULT WINAPI CoCreateInstance_Hook(REFCLSID rclsid, LPUNKNOWN pUnkOuter, DWORD dwClsContext, REFIID riid, LPVOID* ppv)
    {
        HRESULT hr = oCoCreateInstance(rclsid, pUnkOuter, dwClsContext, riid, ppv);

        if (SUCCEEDED(hr) && ppv && *ppv && rclsid == LOCAL_CLSID_FilterGraph)
        {
            dbg_log("DirectShowVideoScale: FilterGraph created");
            IGraphBuilder* pGB = nullptr;
            if (SUCCEEDED(((IUnknown*)*ppv)->QueryInterface(IID_IGraphBuilder, (void**)&pGB)))
            {
                HookGraphBuilder(pGB);
                pGB->Release();
            }
        }

        return hr;
    }

    bool Install()
    {
        HMODULE hOle32 = GetModuleHandleA("ole32.dll");
        if (!hOle32) hOle32 = LoadLibraryA("ole32.dll");
        if (!hOle32) return false;

        oCoCreateInstance = (decltype(oCoCreateInstance))GetProcAddress(hOle32, "CoCreateInstance");
        if (!oCoCreateInstance) return false;

        DetourTransactionBegin();
        DetourUpdateThread(GetCurrentThread());
        DetourAttach(&(PVOID&)oCoCreateInstance, CoCreateInstance_Hook);
        LONG error = DetourTransactionCommit();

        if (error == NO_ERROR)
        {
            dbg_log("DirectShowVideoScale: Installed");
            return true;
        }
        return false;
    }
}
