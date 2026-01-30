#include "pch.h"

#include <d3d9.h>
#include <windows.h>

#include "SharedConstants.h"
#include "BorderlessState.h"

#pragma comment(lib, "d3d9.lib")

static FILE* g_logFile = nullptr;
static void dbg_log(const char* format, ...)
{
    if (!RuntimeConfig::DebugLogging())
        return;
    if (!g_logFile)
        g_logFile = _fsopen("./D3D9hooks.log", "w", _SH_DENYNO);
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

namespace D3D9Hooks
{
    // Original function pointers
    static IDirect3D9* (WINAPI* oDirect3DCreate9)(UINT SDKVersion) = nullptr;

    // Vtable indices for IDirect3D9
    constexpr int VTABLE_IDirect3D9_CreateDevice = 16;

    // Vtable indices for IDirect3DDevice9
    constexpr int VTABLE_IDirect3DDevice9_Reset = 16;
    constexpr int VTABLE_IDirect3DDevice9_Present = 17;
    constexpr int VTABLE_IDirect3DDevice9_SetRenderTarget = 37;
    constexpr int VTABLE_IDirect3DDevice9_EndScene = 42;
    constexpr int VTABLE_IDirect3DDevice9_SetViewport = 47;

    // Original vtable function pointers
    static HRESULT(WINAPI* oCreateDevice)(IDirect3D9* pThis, UINT Adapter, D3DDEVTYPE DeviceType,
        HWND hFocusWindow, DWORD BehaviorFlags, D3DPRESENT_PARAMETERS* pPresentationParameters,
        IDirect3DDevice9** ppReturnedDeviceInterface) = nullptr;

    static HRESULT(WINAPI* oReset)(IDirect3DDevice9* pThis, D3DPRESENT_PARAMETERS* pPresentationParameters) = nullptr;
    static HRESULT(WINAPI* oPresent)(IDirect3DDevice9* pThis, const RECT* pSourceRect, const RECT* pDestRect,
        HWND hDestWindowOverride, const RGNDATA* pDirtyRegion) = nullptr;
    static HRESULT(WINAPI* oSetRenderTarget)(IDirect3DDevice9* pThis, DWORD RenderTargetIndex, IDirect3DSurface9* pRenderTarget) = nullptr;
    static HRESULT(WINAPI* oEndScene)(IDirect3DDevice9* pThis) = nullptr;
    static HRESULT(WINAPI* oSetViewport)(IDirect3DDevice9* pThis, const D3DVIEWPORT9* pViewport) = nullptr;

    static int presentLogCount = 0;
    static int endSceneLogCount = 0;
    static int setRenderTargetLogCount = 0;
    static int setViewportLogCount = 0;

    // Test render target for verifying SetRenderTarget approach
    static IDirect3DSurface9* g_pTestRenderTarget = nullptr;
    static IDirect3DSurface9* g_pOriginalBackBuffer = nullptr;
    static bool g_testRenderTargetActive = false;

    static void LogSurfaceInfo(const char* label, IDirect3DSurface9* pSurface)
    {
        if (!pSurface)
        {
            dbg_log("  %s: null", label);
            return;
        }
        D3DSURFACE_DESC desc;
        if (SUCCEEDED(pSurface->GetDesc(&desc)))
        {
            dbg_log("  %s: 0x%p (%dx%d, fmt=%d, usage=%d, pool=%d)",
                label, pSurface, desc.Width, desc.Height, desc.Format, desc.Usage, desc.Pool);
        }
        else
        {
            dbg_log("  %s: 0x%p (failed to get desc)", label, pSurface);
        }
    }

    static void LogPresentParameters(const char* context, D3DPRESENT_PARAMETERS* pp)
    {
        if (!pp)
        {
            dbg_log("%s: PresentParameters is NULL", context);
            return;
        }

        dbg_log("%s: BackBuffer=%dx%d, Format=%d, Count=%d, Windowed=%d, SwapEffect=%d",
            context,
            pp->BackBufferWidth, pp->BackBufferHeight,
            pp->BackBufferFormat, pp->BackBufferCount,
            pp->Windowed, pp->SwapEffect);
        dbg_log("  FullScreen_RefreshRateInHz=%d, PresentationInterval=0x%x",
            pp->FullScreen_RefreshRateInHz, pp->PresentationInterval);
        dbg_log("  hDeviceWindow=0x%p, EnableAutoDepthStencil=%d",
            pp->hDeviceWindow, pp->EnableAutoDepthStencil);
    }

    static void HookDeviceVtable(IDirect3DDevice9* pDevice);

    HRESULT WINAPI CreateDevice_Hook(IDirect3D9* pThis, UINT Adapter, D3DDEVTYPE DeviceType,
        HWND hFocusWindow, DWORD BehaviorFlags, D3DPRESENT_PARAMETERS* pPresentationParameters,
        IDirect3DDevice9** ppReturnedDeviceInterface)
    {
        dbg_log("IDirect3D9::CreateDevice called");
        dbg_log("  Adapter=%d, DeviceType=%d, hFocusWindow=0x%p, BehaviorFlags=0x%x",
            Adapter, DeviceType, hFocusWindow, BehaviorFlags);
        LogPresentParameters("  Before CreateDevice", pPresentationParameters);

        HRESULT hr = oCreateDevice(pThis, Adapter, DeviceType, hFocusWindow, BehaviorFlags,
            pPresentationParameters, ppReturnedDeviceInterface);

        dbg_log("  CreateDevice returned 0x%x", hr);

        if (SUCCEEDED(hr) && ppReturnedDeviceInterface && *ppReturnedDeviceInterface)
        {
            dbg_log("  Device created successfully, hooking device vtable...");
            HookDeviceVtable(*ppReturnedDeviceInterface);
        }

        return hr;
    }

    // Helper function to set up borderless window after Reset
    static void SetupBorderlessWindow(HWND hWnd)
    {
        if (!hWnd)
            return;

        dbg_log("  [Borderless] Setting up borderless window...");

        // Get native screen resolution
        BorderlessState::GetNativeResolution();
        BorderlessState::CalculateScaling();

        dbg_log("  [Borderless] Screen: %dx%d, Scaled: %dx%d, Offset: (%d,%d)",
            BorderlessState::g_screenWidth, BorderlessState::g_screenHeight,
            BorderlessState::g_scaledWidth, BorderlessState::g_scaledHeight,
            BorderlessState::g_offsetX, BorderlessState::g_offsetY);

        // Set borderless style: WS_POPUP | WS_VISIBLE
        LONG style = WS_POPUP | WS_VISIBLE;
        SetWindowLongA(hWnd, GWL_STYLE, style);

        // Remove extended styles that might cause issues
        SetWindowLongA(hWnd, GWL_EXSTYLE, 0);

        // Position window to cover entire screen
        SetWindowPos(hWnd, HWND_TOP, 0, 0,
            BorderlessState::g_screenWidth, BorderlessState::g_screenHeight,
            SWP_FRAMECHANGED | SWP_SHOWWINDOW);

        dbg_log("  [Borderless] Window set to %dx%d at (0,0)",
            BorderlessState::g_screenWidth, BorderlessState::g_screenHeight);
    }

    HRESULT WINAPI Reset_Hook(IDirect3DDevice9* pThis, D3DPRESENT_PARAMETERS* pPresentationParameters)
    {
        dbg_log("IDirect3DDevice9::Reset called");
        LogPresentParameters("  Before Reset", pPresentationParameters);

        // Release old test render target before Reset (required by D3D9)
        if (g_pTestRenderTarget)
        {
            g_pTestRenderTarget->Release();
            g_pTestRenderTarget = nullptr;
        }
        if (g_pOriginalBackBuffer)
        {
            g_pOriginalBackBuffer->Release();
            g_pOriginalBackBuffer = nullptr;
        }
        g_testRenderTargetActive = false;

        // Detect fullscreen request and convert to borderless windowed
        bool requestingFullscreen = (pPresentationParameters && !pPresentationParameters->Windowed);
        bool requestingWindowed = (pPresentationParameters && pPresentationParameters->Windowed && BorderlessState::g_borderlessActive);

        if (requestingFullscreen)
        {
            dbg_log("  [Borderless] Intercepting fullscreen request");

            // Store the original game resolution
            BorderlessState::g_gameWidth = pPresentationParameters->BackBufferWidth;
            BorderlessState::g_gameHeight = pPresentationParameters->BackBufferHeight;
            dbg_log("  [Borderless] Game resolution: %dx%d",
                BorderlessState::g_gameWidth, BorderlessState::g_gameHeight);

            // Get native screen resolution
            BorderlessState::GetNativeResolution();
            dbg_log("  [Borderless] Native resolution: %dx%d",
                BorderlessState::g_screenWidth, BorderlessState::g_screenHeight);

            // Modify presentation parameters for borderless windowed
            pPresentationParameters->Windowed = TRUE;
            pPresentationParameters->BackBufferWidth = BorderlessState::g_screenWidth;
            pPresentationParameters->BackBufferHeight = BorderlessState::g_screenHeight;
            pPresentationParameters->FullScreen_RefreshRateInHz = 0;  // Required for windowed mode

            // Mark borderless mode as active BEFORE calling Reset
            // This will block ChangeDisplaySettingsExA and window manipulation
            BorderlessState::g_borderlessActive = true;

            LogPresentParameters("  Modified for borderless", pPresentationParameters);
        }
        else if (requestingWindowed)
        {
            dbg_log("  [Borderless] Game requesting windowed mode, deactivating borderless");
            BorderlessState::g_borderlessActive = false;
            // Reset log counters so we can see what happens after returning to windowed
            presentLogCount = 0;
            endSceneLogCount = 0;
            setViewportLogCount = 0;

            // Restore window to normal windowed style and size BEFORE Reset
            // This is needed because Reset with BackBuffer=0 uses window client size
            HWND hWnd = BorderlessState::g_mainGameWindow;
            if (hWnd)
            {
                dbg_log("  [Borderless] Restoring window to windowed mode...");
                // Restore overlapped window style
                SetWindowLongA(hWnd, GWL_STYLE, WS_OVERLAPPEDWINDOW | WS_VISIBLE);
                // Restore window size to game resolution (with frame adjustment)
                RECT rect = { 0, 0, BorderlessState::g_gameWidth, BorderlessState::g_gameHeight };
                AdjustWindowRect(&rect, WS_OVERLAPPEDWINDOW, FALSE);
                int width = rect.right - rect.left;
                int height = rect.bottom - rect.top;
                // Center on screen
                int x = (BorderlessState::g_screenWidth - width) / 2;
                int y = (BorderlessState::g_screenHeight - height) / 2;
                SetWindowPos(hWnd, HWND_NOTOPMOST, x, y, width, height, SWP_FRAMECHANGED | SWP_SHOWWINDOW);
                dbg_log("  [Borderless] Window restored to %dx%d at (%d,%d)", width, height, x, y);
            }
        }

        HRESULT hr = oReset(pThis, pPresentationParameters);

        dbg_log("  Reset returned 0x%x", hr);

        // After successful Reset with borderless mode, set up the window
        if (SUCCEEDED(hr) && requestingFullscreen)
        {
            // Set up borderless window
            HWND hWnd = BorderlessState::g_mainGameWindow;
            if (!hWnd && pPresentationParameters)
                hWnd = pPresentationParameters->hDeviceWindow;

            SetupBorderlessWindow(hWnd);
        }

        // After successful Reset, set up render target redirection for scaling
        if (SUCCEEDED(hr) && BorderlessState::g_borderlessActive)
        {
            dbg_log("  [Borderless] Setting up render target for scaling...");

            // Get backbuffer reference
            pThis->GetRenderTarget(0, &g_pOriginalBackBuffer);
            LogSurfaceInfo("[Borderless] Backbuffer", g_pOriginalBackBuffer);

            // Create render target at game resolution for game to render to
            HRESULT hrCreate = pThis->CreateRenderTarget(
                BorderlessState::g_gameWidth, BorderlessState::g_gameHeight,
                D3DFMT_X8R8G8B8,
                D3DMULTISAMPLE_NONE,
                0,
                FALSE,
                &g_pTestRenderTarget,
                nullptr
            );

            if (SUCCEEDED(hrCreate))
            {
                LogSurfaceInfo("[Borderless] Game render target", g_pTestRenderTarget);

                // Redirect rendering to our surface
                HRESULT hrSet = oSetRenderTarget(pThis, 0, g_pTestRenderTarget);
                if (SUCCEEDED(hrSet))
                {
                    g_testRenderTargetActive = true;
                    dbg_log("  [Borderless] Render target redirection active");
                }
                else
                {
                    dbg_log("  [Borderless] Failed to set render target, hr=0x%x", hrSet);
                }
            }
            else
            {
                dbg_log("  [Borderless] Failed to create game render target, hr=0x%x", hrCreate);
            }

            // Reset log counters
            presentLogCount = 0;
            endSceneLogCount = 0;
        }

        return hr;
    }

    HRESULT WINAPI Present_Hook(IDirect3DDevice9* pThis, const RECT* pSourceRect, const RECT* pDestRect,
        HWND hDestWindowOverride, const RGNDATA* pDirtyRegion)
    {
        if (RuntimeConfig::DebugLogging() && presentLogCount < 20)
        {
            dbg_log("IDirect3DDevice9::Present: src=%s, dst=%s, hwnd=0x%p",
                pSourceRect ? "set" : "null",
                pDestRect ? "set" : "null",
                hDestWindowOverride);
            if (pSourceRect)
                dbg_log("  SourceRect: (%d,%d)-(%d,%d)", pSourceRect->left, pSourceRect->top, pSourceRect->right, pSourceRect->bottom);
            if (pDestRect)
                dbg_log("  DestRect: (%d,%d)-(%d,%d)", pDestRect->left, pDestRect->top, pDestRect->right, pDestRect->bottom);
            presentLogCount++;
        }

        // Handle borderless scaling
        if (BorderlessState::g_borderlessActive && g_testRenderTargetActive && g_pOriginalBackBuffer && g_pTestRenderTarget)
        {
            if (RuntimeConfig::DebugLogging() && presentLogCount <= 5)
            {
                dbg_log("  [Borderless] Performing scaling...");
            }

            // 1. Restore backbuffer as render target
            oSetRenderTarget(pThis, 0, g_pOriginalBackBuffer);

            // 2. Clear backbuffer to black (for pillarboxes)
            pThis->Clear(0, nullptr, D3DCLEAR_TARGET, D3DCOLOR_XRGB(0, 0, 0), 1.0f, 0);

            // 3. Copy and scale game render target to backbuffer with pillarboxing
            RECT srcRect = { 0, 0, BorderlessState::g_gameWidth, BorderlessState::g_gameHeight };
            RECT dstRect = {
                BorderlessState::g_offsetX,
                BorderlessState::g_offsetY,
                BorderlessState::g_offsetX + BorderlessState::g_scaledWidth,
                BorderlessState::g_offsetY + BorderlessState::g_scaledHeight
            };

            // Use StretchRect for scaling (D3DTEXF_LINEAR for bilinear filtering)
            HRESULT hrStretch = pThis->StretchRect(
                g_pTestRenderTarget,
                &srcRect,
                g_pOriginalBackBuffer,
                &dstRect,
                D3DTEXF_LINEAR
            );

            if (RuntimeConfig::DebugLogging() && presentLogCount <= 5)
            {
                dbg_log("  [Borderless] StretchRect: src=(%d,%d-%d,%d) dst=(%d,%d-%d,%d) hr=0x%x",
                    srcRect.left, srcRect.top, srcRect.right, srcRect.bottom,
                    dstRect.left, dstRect.top, dstRect.right, dstRect.bottom,
                    hrStretch);
            }

            // 4. Call original Present
            HRESULT hr = oPresent(pThis, pSourceRect, pDestRect, hDestWindowOverride, pDirtyRegion);

            // 5. Re-set our render target for the next frame
            oSetRenderTarget(pThis, 0, g_pTestRenderTarget);

            if (RuntimeConfig::DebugLogging() && presentLogCount <= 5)
            {
                dbg_log("  [Borderless] Present returned 0x%x, RT redirected for next frame", hr);
            }

            return hr;
        }

        // Normal path (not borderless)
        return oPresent(pThis, pSourceRect, pDestRect, hDestWindowOverride, pDirtyRegion);
    }

    HRESULT WINAPI EndScene_Hook(IDirect3DDevice9* pThis)
    {
        if (RuntimeConfig::DebugLogging() && endSceneLogCount < 5)
        {
            dbg_log("IDirect3DDevice9::EndScene called");
            endSceneLogCount++;
        }

        return oEndScene(pThis);
    }

    HRESULT WINAPI SetRenderTarget_Hook(IDirect3DDevice9* pThis, DWORD RenderTargetIndex, IDirect3DSurface9* pRenderTarget)
    {
        if (RuntimeConfig::DebugLogging() && setRenderTargetLogCount < 50)
        {
            D3DSURFACE_DESC desc;
            const char* surfaceInfo = "null";
            char surfaceBuf[128] = {0};
            if (pRenderTarget && SUCCEEDED(pRenderTarget->GetDesc(&desc)))
            {
                sprintf_s(surfaceBuf, "%dx%d fmt=%d", desc.Width, desc.Height, desc.Format);
                surfaceInfo = surfaceBuf;
            }
            dbg_log("IDirect3DDevice9::SetRenderTarget: index=%d, surface=0x%p (%s)",
                RenderTargetIndex, pRenderTarget, surfaceInfo);
            setRenderTargetLogCount++;
        }

        return oSetRenderTarget(pThis, RenderTargetIndex, pRenderTarget);
    }

    HRESULT WINAPI SetViewport_Hook(IDirect3DDevice9* pThis, const D3DVIEWPORT9* pViewport)
    {
        if (RuntimeConfig::DebugLogging() && setViewportLogCount < 50)
        {
            if (pViewport)
            {
                dbg_log("IDirect3DDevice9::SetViewport: X=%d, Y=%d, Width=%d, Height=%d, MinZ=%.2f, MaxZ=%.2f",
                    pViewport->X, pViewport->Y, pViewport->Width, pViewport->Height,
                    pViewport->MinZ, pViewport->MaxZ);
            }
            else
            {
                dbg_log("IDirect3DDevice9::SetViewport: pViewport=null");
            }
            setViewportLogCount++;
        }

        return oSetViewport(pThis, pViewport);
    }

    static void PatchVtable(void** vtable, int index, void* hookFunc, void** originalFunc)
    {
        DWORD oldProtect;
        if (VirtualProtect(&vtable[index], sizeof(void*), PAGE_EXECUTE_READWRITE, &oldProtect))
        {
            *originalFunc = vtable[index];
            vtable[index] = hookFunc;
            VirtualProtect(&vtable[index], sizeof(void*), oldProtect, &oldProtect);
            dbg_log("  Patched vtable[%d]: 0x%p -> 0x%p", index, *originalFunc, hookFunc);
        }
        else
        {
            dbg_log("  Failed to patch vtable[%d], VirtualProtect failed", index);
        }
    }

    static void HookD3D9Vtable(IDirect3D9* pD3D9)
    {
        dbg_log("Hooking IDirect3D9 vtable...");

        void** vtable = *(void***)pD3D9;
        dbg_log("  IDirect3D9 vtable at 0x%p", vtable);

        PatchVtable(vtable, VTABLE_IDirect3D9_CreateDevice, (void*)CreateDevice_Hook, (void**)&oCreateDevice);
    }

    static void HookDeviceVtable(IDirect3DDevice9* pDevice)
    {
        dbg_log("Hooking IDirect3DDevice9 vtable...");

        void** vtable = *(void***)pDevice;
        dbg_log("  IDirect3DDevice9 vtable at 0x%p", vtable);

        PatchVtable(vtable, VTABLE_IDirect3DDevice9_Reset, (void*)Reset_Hook, (void**)&oReset);
        PatchVtable(vtable, VTABLE_IDirect3DDevice9_Present, (void*)Present_Hook, (void**)&oPresent);
        PatchVtable(vtable, VTABLE_IDirect3DDevice9_SetRenderTarget, (void*)SetRenderTarget_Hook, (void**)&oSetRenderTarget);
        PatchVtable(vtable, VTABLE_IDirect3DDevice9_EndScene, (void*)EndScene_Hook, (void**)&oEndScene);
        PatchVtable(vtable, VTABLE_IDirect3DDevice9_SetViewport, (void*)SetViewport_Hook, (void**)&oSetViewport);
    }

    IDirect3D9* WINAPI Direct3DCreate9_Hook(UINT SDKVersion)
    {
        dbg_log("Direct3DCreate9 called with SDKVersion=%d", SDKVersion);

        IDirect3D9* pD3D9 = oDirect3DCreate9(SDKVersion);

        if (pD3D9)
        {
            dbg_log("Direct3DCreate9 returned IDirect3D9 at 0x%p", pD3D9);
            HookD3D9Vtable(pD3D9);
        }
        else
        {
            dbg_log("Direct3DCreate9 returned NULL");
        }

        return pD3D9;
    }

    bool Install()
    {
        dbg_log("D3D9Hooks::Install() called");

        HMODULE hD3D9 = GetModuleHandleA("d3d9.dll");
        if (!hD3D9)
        {
            // d3d9.dll not loaded yet, we'll need to hook LoadLibraryA to catch it
            // For now, try loading it
            hD3D9 = LoadLibraryA("d3d9.dll");
            if (!hD3D9)
            {
                dbg_log("D3D9Hooks::Install: Failed to get/load d3d9.dll");
                return false;
            }
        }

        oDirect3DCreate9 = (decltype(oDirect3DCreate9))GetProcAddress(hD3D9, "Direct3DCreate9");
        if (!oDirect3DCreate9)
        {
            dbg_log("D3D9Hooks::Install: Failed to find Direct3DCreate9");
            return false;
        }

        dbg_log("D3D9Hooks::Install: Found Direct3DCreate9 at 0x%p", oDirect3DCreate9);

        DetourTransactionBegin();
        DetourUpdateThread(GetCurrentThread());
        DetourAttach(&(PVOID&)oDirect3DCreate9, Direct3DCreate9_Hook);
        LONG error = DetourTransactionCommit();

        if (error == NO_ERROR)
        {
            dbg_log("D3D9Hooks::Install: Hook installed successfully");
            return true;
        }

        dbg_log("D3D9Hooks::Install: Failed to install hook, Detours error: %d", error);
        return false;
    }
}
