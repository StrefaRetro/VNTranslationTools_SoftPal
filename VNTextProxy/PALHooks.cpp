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
