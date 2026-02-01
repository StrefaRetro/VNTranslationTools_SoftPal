#include "pch.h"

#include <d3d9.h>
#include <windows.h>

#include "SharedConstants.h"
#include "PillarboxedState.h"
#include "Util/Logger.h"

#pragma comment(lib, "d3d9.lib")

#define dbg_log(...) proxy_log(LogCategory::DX9, __VA_ARGS__)

namespace DX9Hooks
{
    // Original function pointers
    static IDirect3D9* (WINAPI* oDirect3DCreate9)(UINT SDKVersion) = nullptr;

    // Vtable indices for IDirect3D9
    constexpr int VTABLE_IDirect3D9_CreateDevice = 16;

    // Vtable indices for IDirect3DDevice9
    constexpr int VTABLE_IDirect3DDevice9_Reset = 16;
    constexpr int VTABLE_IDirect3DDevice9_Present = 17;
    constexpr int VTABLE_IDirect3DDevice9_GetBackBuffer = 18;
    constexpr int VTABLE_IDirect3DDevice9_StretchRect = 34;
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
    static HRESULT(WINAPI* oGetBackBuffer)(IDirect3DDevice9* pThis, UINT iSwapChain, UINT iBackBuffer,
        D3DBACKBUFFER_TYPE Type, IDirect3DSurface9** ppBackBuffer) = nullptr;
    static HRESULT(WINAPI* oStretchRect)(IDirect3DDevice9* pThis, IDirect3DSurface9* pSourceSurface, const RECT* pSourceRect,
        IDirect3DSurface9* pDestSurface, const RECT* pDestRect, D3DTEXTUREFILTERTYPE Filter) = nullptr;
    static HRESULT(WINAPI* oSetRenderTarget)(IDirect3DDevice9* pThis, DWORD RenderTargetIndex, IDirect3DSurface9* pRenderTarget) = nullptr;
    static HRESULT(WINAPI* oEndScene)(IDirect3DDevice9* pThis) = nullptr;
    static HRESULT(WINAPI* oSetViewport)(IDirect3DDevice9* pThis, const D3DVIEWPORT9* pViewport) = nullptr;

    static int presentLogCount = 0;
    static int endSceneLogCount = 0;
    static int setRenderTargetLogCount = 0;
    static int setViewportLogCount = 0;
    static int stretchRectLogCount = 0;
    static int getBackBufferLogCount = 0;

    // Render target for game rendering
    static IDirect3DSurface9* g_pGameRenderTarget = nullptr;
    static IDirect3DSurface9* g_pOriginalBackBuffer = nullptr;
    static bool g_renderTargetActive = false;

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
            dbg_log("  Pure DX9 mode");
        }

        return hr;
    }

    // Helper function to set up pillarboxed window after Reset
    static void SetupPillarboxedWindow(HWND hWnd)
    {
        if (!hWnd)
            return;

        dbg_log("  [Pillarboxed] Setting up pillarboxed window...");

        // Get native screen resolution
        PillarboxedState::GetNativeResolution();
        PillarboxedState::CalculateScaling();

        dbg_log("  [Pillarboxed] Screen: %dx%d, Scaled: %dx%d, Offset: (%d,%d)",
            PillarboxedState::g_screenWidth, PillarboxedState::g_screenHeight,
            PillarboxedState::g_scaledWidth, PillarboxedState::g_scaledHeight,
            PillarboxedState::g_offsetX, PillarboxedState::g_offsetY);

        // Set pillarboxed style: WS_POPUP | WS_VISIBLE
        LONG style = WS_POPUP | WS_VISIBLE;
        SetWindowLongA(hWnd, GWL_STYLE, style);

        // Remove extended styles that might cause issues
        SetWindowLongA(hWnd, GWL_EXSTYLE, 0);

        // Position window to cover entire screen
        SetWindowPos(hWnd, HWND_TOP, 0, 0,
            PillarboxedState::g_screenWidth, PillarboxedState::g_screenHeight,
            SWP_FRAMECHANGED | SWP_SHOWWINDOW);

        dbg_log("  [Pillarboxed] Window set to %dx%d at (0,0)",
            PillarboxedState::g_screenWidth, PillarboxedState::g_screenHeight);
    }

    HRESULT WINAPI Reset_Hook(IDirect3DDevice9* pThis, D3DPRESENT_PARAMETERS* pPresentationParameters)
    {
        dbg_log("IDirect3DDevice9::Reset called");
        LogPresentParameters("  Before Reset", pPresentationParameters);

        // Release old render target before Reset (required by D3D9)
        if (g_pGameRenderTarget)
        {
            g_pGameRenderTarget->Release();
            g_pGameRenderTarget = nullptr;
        }
        if (g_pOriginalBackBuffer)
        {
            g_pOriginalBackBuffer->Release();
            g_pOriginalBackBuffer = nullptr;
        }
        g_renderTargetActive = false;

        // Detect fullscreen request and convert to pillarboxed windowed
        bool requestingFullscreen = (pPresentationParameters && !pPresentationParameters->Windowed);
        bool requestingWindowed = (pPresentationParameters && pPresentationParameters->Windowed && PillarboxedState::g_pillarboxedActive);

        if (requestingFullscreen)
        {
            dbg_log("  [Pillarboxed] Intercepting fullscreen request");
            dbg_log("  [Pillarboxed] Game resolution: %dx%d",
                PillarboxedState::g_gameWidth, PillarboxedState::g_gameHeight);

            // Get native screen resolution
            PillarboxedState::GetNativeResolution();
            PillarboxedState::CalculateScaling();
            dbg_log("  [Pillarboxed] Native resolution: %dx%d",
                PillarboxedState::g_screenWidth, PillarboxedState::g_screenHeight);

            pPresentationParameters->Windowed = TRUE;
            pPresentationParameters->FullScreen_RefreshRateInHz = 0;

            // Pure DX9 mode: Set backbuffer to SCREEN resolution
            // We'll use StretchRect to scale from game RT to screen-sized backbuffer
            pPresentationParameters->BackBufferWidth = PillarboxedState::g_screenWidth;
            pPresentationParameters->BackBufferHeight = PillarboxedState::g_screenHeight;
            dbg_log("  [Pillarboxed] Setting backbuffer to screen resolution %dx%d",
                PillarboxedState::g_screenWidth, PillarboxedState::g_screenHeight);

            // Mark pillarboxed mode as active BEFORE calling Reset
            PillarboxedState::g_pillarboxedActive = true;

            LogPresentParameters("  Modified for pillarboxed", pPresentationParameters);
        }
        else if (requestingWindowed)
        {
            dbg_log("  [Pillarboxed] Game requesting windowed mode, deactivating pillarboxed");
            PillarboxedState::g_pillarboxedActive = false;

            // Reset log counters
            presentLogCount = 0;
            endSceneLogCount = 0;
            setViewportLogCount = 0;
            setRenderTargetLogCount = 0;
            stretchRectLogCount = 0;
            getBackBufferLogCount = 0;

            // Restore window to normal windowed style and size
            HWND hWnd = PillarboxedState::g_mainGameWindow;
            if (hWnd)
            {
                dbg_log("  [Pillarboxed] Restoring window to windowed mode...");
                SetWindowLongA(hWnd, GWL_STYLE, WS_OVERLAPPEDWINDOW | WS_VISIBLE);
                RECT rect = { 0, 0, PillarboxedState::g_gameWidth, PillarboxedState::g_gameHeight };
                AdjustWindowRect(&rect, WS_OVERLAPPEDWINDOW, FALSE);
                int width = rect.right - rect.left;
                int height = rect.bottom - rect.top;
                int x = (PillarboxedState::g_screenWidth - width) / 2;
                int y = (PillarboxedState::g_screenHeight - height) / 2;
                SetWindowPos(hWnd, HWND_NOTOPMOST, x, y, width, height, SWP_FRAMECHANGED | SWP_SHOWWINDOW);
                dbg_log("  [Pillarboxed] Window restored to %dx%d at (%d,%d)", width, height, x, y);
            }

            if (pPresentationParameters->BackBufferWidth == 0 || pPresentationParameters->BackBufferHeight == 0)
            {
                pPresentationParameters->BackBufferWidth = PillarboxedState::g_gameWidth;
                pPresentationParameters->BackBufferHeight = PillarboxedState::g_gameHeight;
            }
        }

        HRESULT hr = oReset(pThis, pPresentationParameters);

        dbg_log("  Reset returned 0x%x", hr);

        // After successful Reset with pillarboxed mode, set up the window
        if (SUCCEEDED(hr) && requestingFullscreen)
        {
            HWND hWnd = PillarboxedState::g_mainGameWindow;
            if (!hWnd && pPresentationParameters)
                hWnd = pPresentationParameters->hDeviceWindow;

            SetupPillarboxedWindow(hWnd);

            presentLogCount = 0;
            endSceneLogCount = 0;
        }

        // Set up render target redirection for scaling
        if (SUCCEEDED(hr))
        {
            UINT gameWidth = PillarboxedState::g_gameWidth;
            UINT gameHeight = PillarboxedState::g_gameHeight;

            if (gameWidth == 0 || gameHeight == 0)
            {
                gameWidth = pPresentationParameters ? pPresentationParameters->BackBufferWidth : 800;
                gameHeight = pPresentationParameters ? pPresentationParameters->BackBufferHeight : 600;
            }

            dbg_log("  [RT] Setting up render target redirection at %dx%d", gameWidth, gameHeight);

            // Get backbuffer reference
            if (g_pOriginalBackBuffer)
            {
                g_pOriginalBackBuffer->Release();
                g_pOriginalBackBuffer = nullptr;
            }
            pThis->GetRenderTarget(0, &g_pOriginalBackBuffer);
            LogSurfaceInfo("[RT] Original backbuffer", g_pOriginalBackBuffer);

            // Create render target at game resolution
            if (g_pGameRenderTarget)
            {
                g_pGameRenderTarget->Release();
                g_pGameRenderTarget = nullptr;
            }
            HRESULT hrCreate = pThis->CreateRenderTarget(
                gameWidth, gameHeight,
                D3DFMT_X8R8G8B8,
                D3DMULTISAMPLE_NONE,
                0,
                FALSE,
                &g_pGameRenderTarget,
                nullptr
            );

            if (SUCCEEDED(hrCreate))
            {
                LogSurfaceInfo("[RT] Game render target", g_pGameRenderTarget);
                HRESULT hrSet = oSetRenderTarget(pThis, 0, g_pGameRenderTarget);
                if (SUCCEEDED(hrSet))
                {
                    g_renderTargetActive = true;
                    dbg_log("  [RT] Render target redirection active");
                }
            }
            else
            {
                dbg_log("  [RT] Failed to create game render target, hr=0x%x", hrCreate);
            }
        }

        return hr;
    }

    HRESULT WINAPI Present_Hook(IDirect3DDevice9* pThis, const RECT* pSourceRect, const RECT* pDestRect,
        HWND hDestWindowOverride, const RGNDATA* pDirtyRegion)
    {
        if (RuntimeConfig::DebugLogging() && presentLogCount < 20)
        {
            dbg_log("IDirect3DDevice9::Present: src=%s, dst=%s, hwnd=0x%p, pillarboxed=%d",
                pSourceRect ? "set" : "null",
                pDestRect ? "set" : "null",
                hDestWindowOverride,
                PillarboxedState::g_pillarboxedActive ? 1 : 0);
            presentLogCount++;
        }

        // DX9 scaling path: use StretchRect for scaling with pillarboxing
        if (g_renderTargetActive && g_pGameRenderTarget && g_pOriginalBackBuffer)
        {
            // Switch to backbuffer for our scaling operation
            oSetRenderTarget(pThis, 0, g_pOriginalBackBuffer);

            // Clear backbuffer to black for pillarboxing
            pThis->ColorFill(g_pOriginalBackBuffer, nullptr, D3DCOLOR_XRGB(0, 0, 0));

            if (PillarboxedState::g_pillarboxedActive)
            {
                // Pillarboxed mode: scale with aspect ratio preservation
                RECT srcRect = { 0, 0, (LONG)PillarboxedState::g_gameWidth, (LONG)PillarboxedState::g_gameHeight };
                RECT dstRect = {
                    (LONG)PillarboxedState::g_offsetX,
                    (LONG)PillarboxedState::g_offsetY,
                    (LONG)(PillarboxedState::g_offsetX + PillarboxedState::g_scaledWidth),
                    (LONG)(PillarboxedState::g_offsetY + PillarboxedState::g_scaledHeight)
                };

                if (RuntimeConfig::DebugLogging() && presentLogCount <= 10)
                {
                    dbg_log("  [DX9] StretchRect: %dx%d -> %dx%d at offset (%d,%d)",
                        PillarboxedState::g_gameWidth, PillarboxedState::g_gameHeight,
                        PillarboxedState::g_scaledWidth, PillarboxedState::g_scaledHeight,
                        PillarboxedState::g_offsetX, PillarboxedState::g_offsetY);
                }

                HRESULT hr = oStretchRect(pThis, g_pGameRenderTarget, &srcRect, g_pOriginalBackBuffer, &dstRect, D3DTEXF_LINEAR);
                if (FAILED(hr) && RuntimeConfig::DebugLogging() && presentLogCount <= 10)
                {
                    dbg_log("  [DX9] StretchRect failed, hr=0x%x", hr);
                }
            }
            else
            {
                // Windowed mode: 1:1 copy
                HRESULT hr = oStretchRect(pThis, g_pGameRenderTarget, nullptr, g_pOriginalBackBuffer, nullptr, D3DTEXF_POINT);
                if (RuntimeConfig::DebugLogging() && presentLogCount <= 10)
                {
                    dbg_log("  [DX9] 1:1 copy, hr=0x%x", hr);
                }
            }

            // Present via D3D9
            HRESULT hrPresent = oPresent(pThis, nullptr, nullptr, hDestWindowOverride, pDirtyRegion);

            // Re-set render target for next frame
            oSetRenderTarget(pThis, 0, g_pGameRenderTarget);

            return hrPresent;
        }

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
        // Capture the true game resolution from viewport (unaffected by DPI virtualization)
        if (pViewport)
            PillarboxedState::SetGameResolution(pViewport->Width, pViewport->Height);

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

    HRESULT WINAPI StretchRect_Hook(IDirect3DDevice9* pThis, IDirect3DSurface9* pSourceSurface, const RECT* pSourceRect,
        IDirect3DSurface9* pDestSurface, const RECT* pDestRect, D3DTEXTUREFILTERTYPE Filter)
    {
        if (RuntimeConfig::DebugLogging() && stretchRectLogCount < 50)
        {
            D3DSURFACE_DESC srcDesc = {}, dstDesc = {};
            if (pSourceSurface) pSourceSurface->GetDesc(&srcDesc);
            if (pDestSurface) pDestSurface->GetDesc(&dstDesc);

            dbg_log("IDirect3DDevice9::StretchRect: src=%dx%d, dst=%dx%d, filter=%d",
                srcDesc.Width, srcDesc.Height, dstDesc.Width, dstDesc.Height, Filter);
            stretchRectLogCount++;
        }

        return oStretchRect(pThis, pSourceSurface, pSourceRect, pDestSurface, pDestRect, Filter);
    }

    HRESULT WINAPI GetBackBuffer_Hook(IDirect3DDevice9* pThis, UINT iSwapChain, UINT iBackBuffer,
        D3DBACKBUFFER_TYPE Type, IDirect3DSurface9** ppBackBuffer)
    {
        HRESULT hr = oGetBackBuffer(pThis, iSwapChain, iBackBuffer, Type, ppBackBuffer);

        if (RuntimeConfig::DebugLogging() && getBackBufferLogCount < 20)
        {
            if (SUCCEEDED(hr) && ppBackBuffer && *ppBackBuffer)
            {
                D3DSURFACE_DESC desc;
                (*ppBackBuffer)->GetDesc(&desc);
                dbg_log("IDirect3DDevice9::GetBackBuffer: chain=%d, idx=%d -> %dx%d (fmt=%d)",
                    iSwapChain, iBackBuffer, desc.Width, desc.Height, desc.Format);
            }
            getBackBufferLogCount++;
        }

        return hr;
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
        PatchVtable(vtable, VTABLE_IDirect3DDevice9_GetBackBuffer, (void*)GetBackBuffer_Hook, (void**)&oGetBackBuffer);
        PatchVtable(vtable, VTABLE_IDirect3DDevice9_StretchRect, (void*)StretchRect_Hook, (void**)&oStretchRect);
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
        dbg_log("DX9Hooks::Install() called");

        HMODULE hD3D9 = GetModuleHandleA("d3d9.dll");
        if (!hD3D9)
        {
            hD3D9 = LoadLibraryA("d3d9.dll");
            if (!hD3D9)
            {
                dbg_log("DX9Hooks::Install: Failed to get/load d3d9.dll");
                return false;
            }
        }

        oDirect3DCreate9 = (decltype(oDirect3DCreate9))GetProcAddress(hD3D9, "Direct3DCreate9");
        if (!oDirect3DCreate9)
        {
            dbg_log("DX9Hooks::Install: Failed to find Direct3DCreate9");
            return false;
        }

        dbg_log("DX9Hooks::Install: Found Direct3DCreate9 at 0x%p", oDirect3DCreate9);

        DetourTransactionBegin();
        DetourUpdateThread(GetCurrentThread());
        DetourAttach(&(PVOID&)oDirect3DCreate9, Direct3DCreate9_Hook);
        LONG error = DetourTransactionCommit();

        if (error == NO_ERROR)
        {
            dbg_log("DX9Hooks::Install: Hook installed successfully");
            return true;
        }

        dbg_log("DX9Hooks::Install: Failed to install hook, Detours error: %d", error);
        return false;
    }
}
