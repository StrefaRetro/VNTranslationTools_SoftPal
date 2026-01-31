#include "pch.h"

#include <d3d9.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <windows.h>

#include "SharedConstants.h"
#include "BorderlessState.h"
#include "BicubicScaler.h"
#include "CuNNyScaler.h"
#include "PALHooks.h"

#pragma comment(lib, "d3d9.lib")
#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")

static FILE* g_logFile = nullptr;
static void dbg_log(const char* format, ...)
{
    if (!RuntimeConfig::DebugLogging())
        return;
    if (!g_logFile)
        g_logFile = _fsopen("./DXhooks.log", "w", _SH_DENYNO);
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

namespace DX11Hooks
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

    // Test render target for verifying SetRenderTarget approach
    static IDirect3DSurface9* g_pTestRenderTarget = nullptr;
    static IDirect3DSurface9* g_pOriginalBackBuffer = nullptr;
    static bool g_testRenderTargetActive = false;

    // DX11 hybrid rendering state
    static ID3D11Device* g_pD3D11Device = nullptr;
    static ID3D11DeviceContext* g_pD3D11Context = nullptr;
    static IDXGISwapChain1* g_pDXGISwapChain = nullptr;
    static ID3D11Texture2D* g_pD3D11BackBuffer = nullptr;
    static ID3D11Texture2D* g_pD3D11StagingTexture = nullptr;   // CPU-writable staging texture
    static ID3D11Texture2D* g_pD3D11SourceTexture = nullptr;   // GPU texture for shader input
    static ID3D11ShaderResourceView* g_pD3D11SourceSRV = nullptr;
    static ID3D11RenderTargetView* g_pD3D11RTV = nullptr;
    static bool g_dx11Active = false;
    static bool g_dx11ScalerInitialized = false;
    static UINT g_dx11Width = 0;      // Swapchain/screen width
    static UINT g_dx11Height = 0;     // Swapchain/screen height
    static UINT g_dx11GameWidth = 0;  // Staging texture/game width
    static UINT g_dx11GameHeight = 0; // Staging texture/game height

    // Offscreen surface for copying render target data (D3D9)
    static IDirect3DSurface9* g_pD3D9CopySurface = nullptr;

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

    static void CleanupDX11()
    {
        dbg_log("[DX11] Cleaning up DX11 resources...");
        dbg_log("[DX11]   Swapchain=%p, Device=%p, Context=%p", g_pDXGISwapChain, g_pD3D11Device, g_pD3D11Context);
        BicubicScaler::Cleanup();
        CuNNyScaler::Cleanup();
        g_dx11ScalerInitialized = false;
        if (g_pD3D11SourceSRV) { g_pD3D11SourceSRV->Release(); g_pD3D11SourceSRV = nullptr; }
        if (g_pD3D11SourceTexture) { g_pD3D11SourceTexture->Release(); g_pD3D11SourceTexture = nullptr; }
        if (g_pD3D11RTV) { g_pD3D11RTV->Release(); g_pD3D11RTV = nullptr; }
        if (g_pD3D11BackBuffer) { g_pD3D11BackBuffer->Release(); g_pD3D11BackBuffer = nullptr; }
        if (g_pD3D11StagingTexture) { g_pD3D11StagingTexture->Release(); g_pD3D11StagingTexture = nullptr; }
        if (g_pDXGISwapChain) {
            dbg_log("[DX11]   Releasing swapchain...");
            ULONG refCount = g_pDXGISwapChain->Release();
            dbg_log("[DX11]   Swapchain release returned refcount=%lu", refCount);
            g_pDXGISwapChain = nullptr;
        }
        if (g_pD3D11Context) {
            g_pD3D11Context->ClearState();
            g_pD3D11Context->Flush();
            g_pD3D11Context->Release();
            g_pD3D11Context = nullptr;
        }
        if (g_pD3D11Device) { g_pD3D11Device->Release(); g_pD3D11Device = nullptr; }
        g_dx11Active = false;
        dbg_log("[DX11] Cleanup complete");
    }

    // Forward declaration
    static bool InitializeDX11ForHybrid(HWND hWnd, UINT screenWidth, UINT screenHeight, UINT gameWidth, UINT gameHeight);

    // Initialize DX11 with same resolution for swapchain and staging (simple case)
    static bool InitializeDX11(HWND hWnd, UINT width, UINT height)
    {
        return InitializeDX11ForHybrid(hWnd, width, height, width, height);
    }

    // Initialize DX11 with separate screen and game resolutions (for borderless scaling)
    static bool InitializeDX11ForHybrid(HWND hWnd, UINT screenWidth, UINT screenHeight, UINT gameWidth, UINT gameHeight)
    {
        dbg_log("[DX11] Initializing DX11 for HWND=0x%p, screen=%dx%d, game=%dx%d",
            hWnd, screenWidth, screenHeight, gameWidth, gameHeight);

        // Cleanup any existing DX11 state
        CleanupDX11();

        // Create D3D11 device
        D3D_FEATURE_LEVEL featureLevels[] = { D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_10_0 };
        D3D_FEATURE_LEVEL featureLevel;
        UINT createFlags = 0;
#ifdef _DEBUG
        createFlags |= D3D11_CREATE_DEVICE_DEBUG;
#endif

        HRESULT hr = D3D11CreateDevice(
            nullptr,
            D3D_DRIVER_TYPE_HARDWARE,
            nullptr,
            createFlags,
            featureLevels,
            ARRAYSIZE(featureLevels),
            D3D11_SDK_VERSION,
            &g_pD3D11Device,
            &featureLevel,
            &g_pD3D11Context
        );

        if (FAILED(hr))
        {
            dbg_log("[DX11] Failed to create D3D11 device, hr=0x%x", hr);
            return false;
        }
        dbg_log("[DX11] Created D3D11 device, feature level=0x%x", featureLevel);

        // Get DXGI factory
        IDXGIDevice* pDXGIDevice = nullptr;
        hr = g_pD3D11Device->QueryInterface(__uuidof(IDXGIDevice), (void**)&pDXGIDevice);
        if (FAILED(hr))
        {
            dbg_log("[DX11] Failed to get IDXGIDevice, hr=0x%x", hr);
            CleanupDX11();
            return false;
        }

        IDXGIAdapter* pDXGIAdapter = nullptr;
        hr = pDXGIDevice->GetAdapter(&pDXGIAdapter);
        pDXGIDevice->Release();
        if (FAILED(hr))
        {
            dbg_log("[DX11] Failed to get IDXGIAdapter, hr=0x%x", hr);
            CleanupDX11();
            return false;
        }

        IDXGIFactory2* pDXGIFactory = nullptr;
        hr = pDXGIAdapter->GetParent(__uuidof(IDXGIFactory2), (void**)&pDXGIFactory);
        pDXGIAdapter->Release();
        if (FAILED(hr))
        {
            dbg_log("[DX11] Failed to get IDXGIFactory2, hr=0x%x", hr);
            CleanupDX11();
            return false;
        }

        // Log window info before creating swapchain
        RECT clientRect, windowRect;
        GetClientRect(hWnd, &clientRect);
        GetWindowRect(hWnd, &windowRect);
        dbg_log("[DX11] Target HWND=0x%p, client=%dx%d, window=%dx%d",
            hWnd,
            clientRect.right - clientRect.left, clientRect.bottom - clientRect.top,
            windowRect.right - windowRect.left, windowRect.bottom - windowRect.top);

        // Create swapchain at SCREEN resolution
        DXGI_SWAP_CHAIN_DESC1 swapChainDesc = {};
        swapChainDesc.Width = screenWidth;
        swapChainDesc.Height = screenHeight;
        swapChainDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;  // Compatible with D3DFMT_X8R8G8B8
        swapChainDesc.SampleDesc.Count = 1;
        swapChainDesc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        swapChainDesc.BufferCount = 2;
        swapChainDesc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
        swapChainDesc.Scaling = DXGI_SCALING_NONE;  // No automatic scaling

        dbg_log("[DX11] Creating swapchain: %dx%d, format=%d, buffers=%d, swapEffect=%d, scaling=%d",
            swapChainDesc.Width, swapChainDesc.Height, swapChainDesc.Format,
            swapChainDesc.BufferCount, swapChainDesc.SwapEffect, swapChainDesc.Scaling);

        hr = pDXGIFactory->CreateSwapChainForHwnd(
            g_pD3D11Device,
            hWnd,
            &swapChainDesc,
            nullptr,
            nullptr,
            &g_pDXGISwapChain
        );
        pDXGIFactory->Release();

        if (FAILED(hr))
        {
            dbg_log("[DX11] Failed to create swapchain, hr=0x%x", hr);
            CleanupDX11();
            return false;
        }
        dbg_log("[DX11] Created swapchain %dx%d, ptr=0x%p", screenWidth, screenHeight, g_pDXGISwapChain);

        // Get backbuffer and create RTV
        hr = g_pDXGISwapChain->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)&g_pD3D11BackBuffer);
        if (FAILED(hr))
        {
            dbg_log("[DX11] Failed to get backbuffer, hr=0x%x", hr);
            CleanupDX11();
            return false;
        }

        hr = g_pD3D11Device->CreateRenderTargetView(g_pD3D11BackBuffer, nullptr, &g_pD3D11RTV);
        if (FAILED(hr))
        {
            dbg_log("[DX11] Failed to create RTV, hr=0x%x", hr);
            CleanupDX11();
            return false;
        }

        // Create staging texture at GAME resolution for CPU copy from D3D9
        D3D11_TEXTURE2D_DESC stagingDesc = {};
        stagingDesc.Width = gameWidth;
        stagingDesc.Height = gameHeight;
        stagingDesc.MipLevels = 1;
        stagingDesc.ArraySize = 1;
        stagingDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        stagingDesc.SampleDesc.Count = 1;
        stagingDesc.Usage = D3D11_USAGE_STAGING;
        stagingDesc.BindFlags = 0;
        stagingDesc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;

        hr = g_pD3D11Device->CreateTexture2D(&stagingDesc, nullptr, &g_pD3D11StagingTexture);
        if (FAILED(hr))
        {
            dbg_log("[DX11] Failed to create staging texture, hr=0x%x", hr);
            CleanupDX11();
            return false;
        }
        dbg_log("[DX11] Created staging texture %dx%d", gameWidth, gameHeight);

        // Create source texture for shader input (GPU-side, can be bound as SRV)
        D3D11_TEXTURE2D_DESC sourceDesc = {};
        sourceDesc.Width = gameWidth;
        sourceDesc.Height = gameHeight;
        sourceDesc.MipLevels = 1;
        sourceDesc.ArraySize = 1;
        sourceDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        sourceDesc.SampleDesc.Count = 1;
        sourceDesc.Usage = D3D11_USAGE_DEFAULT;
        sourceDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;

        hr = g_pD3D11Device->CreateTexture2D(&sourceDesc, nullptr, &g_pD3D11SourceTexture);
        if (FAILED(hr))
        {
            dbg_log("[DX11] Failed to create source texture, hr=0x%x", hr);
            CleanupDX11();
            return false;
        }

        // Create shader resource view for source texture
        g_pD3D11SourceSRV = BicubicScaler::CreateSRV(g_pD3D11Device, g_pD3D11SourceTexture);
        if (!g_pD3D11SourceSRV)
        {
            dbg_log("[DX11] Failed to create source SRV");
            CleanupDX11();
            return false;
        }
        dbg_log("[DX11] Created source texture and SRV %dx%d", gameWidth, gameHeight);

        // Initialize bicubic scaler
        if (!BicubicScaler::Initialize(g_pD3D11Device))
        {
            dbg_log("[DX11] Failed to initialize bicubic scaler");
            CleanupDX11();
            return false;
        }
        g_dx11ScalerInitialized = true;
        dbg_log("[DX11] Bicubic scaler initialized");

        // Initialize CuNNy neural network scaler
        if (!CuNNyScaler::Initialize(g_pD3D11Device))
        {
            CuNNyScaler::FatalRenderingError("CuNNy initialization");
        }
        dbg_log("[DX11] CuNNy neural network scaler initialized");

        // Initialize DirectShow video capture for DX11 rendering
        DirectShowVideoScale::InitializeDX11(g_pD3D11Device, g_pD3D11Context);
        dbg_log("[DX11] DirectShowVideoScale DX11 initialized");

        g_dx11Width = screenWidth;
        g_dx11Height = screenHeight;
        g_dx11GameWidth = gameWidth;
        g_dx11GameHeight = gameHeight;
        g_dx11Active = true;
        dbg_log("[DX11] Initialization complete");
        return true;
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

            // Initialize DX11 hybrid rendering
            HWND hWnd = hFocusWindow;
            if (!hWnd && pPresentationParameters)
                hWnd = pPresentationParameters->hDeviceWindow;

            UINT width = pPresentationParameters ? pPresentationParameters->BackBufferWidth : 800;
            UINT height = pPresentationParameters ? pPresentationParameters->BackBufferHeight : 600;

            // Create D3D9 offscreen surface for copying render target
            IDirect3DDevice9* pDevice = *ppReturnedDeviceInterface;
            if (g_pD3D9CopySurface)
            {
                g_pD3D9CopySurface->Release();
                g_pD3D9CopySurface = nullptr;
            }
            HRESULT hrCopy = pDevice->CreateOffscreenPlainSurface(
                width, height,
                D3DFMT_X8R8G8B8,
                D3DPOOL_SYSTEMMEM,
                &g_pD3D9CopySurface,
                nullptr
            );
            if (SUCCEEDED(hrCopy))
            {
                dbg_log("  Created D3D9 copy surface %dx%d", width, height);
            }
            else
            {
                dbg_log("  Failed to create D3D9 copy surface, hr=0x%x", hrCopy);
            }

            if (hWnd && InitializeDX11(hWnd, width, height))
            {
                dbg_log("  DX11 hybrid mode enabled");
            }
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

        // Release D3D9 copy surface before Reset
        if (g_pD3D9CopySurface)
        {
            g_pD3D9CopySurface->Release();
            g_pD3D9CopySurface = nullptr;
        }

        // Always cleanup DX11 before Reset (will recreate after)
        CleanupDX11();

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
            BorderlessState::CalculateScaling();
            dbg_log("  [Borderless] Native resolution: %dx%d",
                BorderlessState::g_screenWidth, BorderlessState::g_screenHeight);

            pPresentationParameters->Windowed = TRUE;
            pPresentationParameters->FullScreen_RefreshRateInHz = 0;  // Required for windowed mode

            // Keep D3D9 backbuffer at GAME resolution
            // DX11 will handle scaling to screen resolution
            dbg_log("  [Borderless] Keeping backbuffer at game resolution");

            // Mark borderless mode as active BEFORE calling Reset
            // This will block ChangeDisplaySettingsExA and window manipulation
            BorderlessState::g_borderlessActive = true;

            LogPresentParameters("  Modified for borderless", pPresentationParameters);
        }
        else if (requestingWindowed)
        {
            dbg_log("  [Borderless] Game requesting windowed mode, deactivating borderless");
            dbg_log("  [Borderless] Current state: gameW=%d, gameH=%d, screenW=%d, screenH=%d, scaledW=%d, scaledH=%d, offsetX=%d, offsetY=%d",
                BorderlessState::g_gameWidth, BorderlessState::g_gameHeight,
                BorderlessState::g_screenWidth, BorderlessState::g_screenHeight,
                BorderlessState::g_scaledWidth, BorderlessState::g_scaledHeight,
                BorderlessState::g_offsetX, BorderlessState::g_offsetY);
            BorderlessState::g_borderlessActive = false;
            // Reset log counters so we can see what happens after returning to windowed
            presentLogCount = 0;
            endSceneLogCount = 0;
            setViewportLogCount = 0;
            setRenderTargetLogCount = 0;
            stretchRectLogCount = 0;
            getBackBufferLogCount = 0;

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

            // Force the PresentParameters to have explicit backbuffer size
            // This ensures D3D9 doesn't use any cached window size info
            if (pPresentationParameters->BackBufferWidth == 0 || pPresentationParameters->BackBufferHeight == 0)
            {
                pPresentationParameters->BackBufferWidth = BorderlessState::g_gameWidth;
                pPresentationParameters->BackBufferHeight = BorderlessState::g_gameHeight;
                dbg_log("  [Borderless] Forced backbuffer size to %dx%d",
                    pPresentationParameters->BackBufferWidth, pPresentationParameters->BackBufferHeight);
            }

            // DON'T cleanup DX11 here - we'll resize it after Reset and keep using it
            // This avoids the issue where DXGI leaves stale content on the window
        }

        HRESULT hr = oReset(pThis, pPresentationParameters);

        dbg_log("  Reset returned 0x%x", hr);

        // Log actual backbuffer dimensions after Reset
        if (SUCCEEDED(hr))
        {
            IDirect3DSurface9* pBB = nullptr;
            if (SUCCEEDED(pThis->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &pBB)) && pBB)
            {
                D3DSURFACE_DESC desc;
                if (SUCCEEDED(pBB->GetDesc(&desc)))
                {
                    dbg_log("  [Post-Reset] Actual backbuffer: %dx%d, format=%d", desc.Width, desc.Height, desc.Format);
                }
                pBB->Release();
            }

            // Log current viewport
            D3DVIEWPORT9 vp;
            if (SUCCEEDED(pThis->GetViewport(&vp)))
            {
                dbg_log("  [Post-Reset] Current viewport: X=%d, Y=%d, Width=%d, Height=%d",
                    vp.X, vp.Y, vp.Width, vp.Height);
            }

            // Log window client rect
            HWND hWnd = BorderlessState::g_mainGameWindow;
            if (!hWnd && pPresentationParameters)
                hWnd = pPresentationParameters->hDeviceWindow;
            if (hWnd)
            {
                RECT clientRect;
                GetClientRect(hWnd, &clientRect);
                dbg_log("  [Post-Reset] Window client rect: %dx%d",
                    clientRect.right - clientRect.left, clientRect.bottom - clientRect.top);

                RECT windowRect;
                GetWindowRect(hWnd, &windowRect);
                dbg_log("  [Post-Reset] Window rect: pos=(%d,%d), size=%dx%d",
                    windowRect.left, windowRect.top,
                    windowRect.right - windowRect.left, windowRect.bottom - windowRect.top);
            }

            // If we just returned from borderless mode, force viewport to game resolution
            if (requestingWindowed)
            {
                D3DVIEWPORT9 forceVP;
                forceVP.X = 0;
                forceVP.Y = 0;
                forceVP.Width = BorderlessState::g_gameWidth;
                forceVP.Height = BorderlessState::g_gameHeight;
                forceVP.MinZ = 0.0f;
                forceVP.MaxZ = 1.0f;
                HRESULT hrVP = oSetViewport(pThis, &forceVP);
                dbg_log("  [Post-Reset] Forced viewport to %dx%d, hr=0x%x",
                    forceVP.Width, forceVP.Height, hrVP);
            }
        }

        // After successful Reset with borderless mode, set up the window
        if (SUCCEEDED(hr) && requestingFullscreen)
        {
            // Set up borderless window
            HWND hWnd = BorderlessState::g_mainGameWindow;
            if (!hWnd && pPresentationParameters)
                hWnd = pPresentationParameters->hDeviceWindow;

            SetupBorderlessWindow(hWnd);

            // Reset log counters
            presentLogCount = 0;
            endSceneLogCount = 0;
        }

        // Always set up render target redirection for both DX11 and pure DX9 scaling paths
        // This allows proper scaling and pillarboxing in borderless mode
        if (SUCCEEDED(hr))
        {
            UINT gameWidth = pPresentationParameters ? pPresentationParameters->BackBufferWidth : 800;
            UINT gameHeight = pPresentationParameters ? pPresentationParameters->BackBufferHeight : 600;

            // Use game resolution from BorderlessState if available
            if (BorderlessState::g_gameWidth > 0 && BorderlessState::g_gameHeight > 0)
            {
                gameWidth = BorderlessState::g_gameWidth;
                gameHeight = BorderlessState::g_gameHeight;
            }

            dbg_log("  [RT] Setting up render target redirection at %dx%d", gameWidth, gameHeight);

            // Get backbuffer reference (we won't render to it, but need to restore it sometimes)
            if (g_pOriginalBackBuffer)
            {
                g_pOriginalBackBuffer->Release();
                g_pOriginalBackBuffer = nullptr;
            }
            pThis->GetRenderTarget(0, &g_pOriginalBackBuffer);
            LogSurfaceInfo("[RT] Original backbuffer", g_pOriginalBackBuffer);

            // Create render target at game resolution for game to render to
            if (g_pTestRenderTarget)
            {
                g_pTestRenderTarget->Release();
                g_pTestRenderTarget = nullptr;
            }
            HRESULT hrCreate = pThis->CreateRenderTarget(
                gameWidth, gameHeight,
                D3DFMT_X8R8G8B8,
                D3DMULTISAMPLE_NONE,
                0,
                FALSE,  // Not lockable - we'll use GetRenderTargetData
                &g_pTestRenderTarget,
                nullptr
            );

            if (SUCCEEDED(hrCreate))
            {
                LogSurfaceInfo("[RT] Game render target", g_pTestRenderTarget);

                // Redirect rendering to our surface
                HRESULT hrSet = oSetRenderTarget(pThis, 0, g_pTestRenderTarget);
                if (SUCCEEDED(hrSet))
                {
                    g_testRenderTargetActive = true;
                    dbg_log("  [RT] Render target redirection active");
                }
                else
                {
                    dbg_log("  [RT] Failed to set render target, hr=0x%x", hrSet);
                }
            }
            else
            {
                dbg_log("  [RT] Failed to create game render target, hr=0x%x", hrCreate);
            }
        }

        // Reinitialize DX11 - DX11 is the sole presenter to the window
        if (SUCCEEDED(hr))
        {
            HWND hWnd = BorderlessState::g_mainGameWindow;
            if (!hWnd && pPresentationParameters)
                hWnd = pPresentationParameters->hDeviceWindow;

            UINT gameWidth = pPresentationParameters ? pPresentationParameters->BackBufferWidth : 800;
            UINT gameHeight = pPresentationParameters ? pPresentationParameters->BackBufferHeight : 600;

            // Use game resolution from BorderlessState if in borderless or returning from it
            if (BorderlessState::g_gameWidth > 0 && BorderlessState::g_gameHeight > 0)
            {
                gameWidth = BorderlessState::g_gameWidth;
                gameHeight = BorderlessState::g_gameHeight;
            }

            UINT screenWidth, screenHeight;
            if (BorderlessState::g_borderlessActive)
            {
                // Borderless: DX11 swapchain at screen resolution for scaling
                screenWidth = BorderlessState::g_screenWidth;
                screenHeight = BorderlessState::g_screenHeight;
                dbg_log("  [Reset] Borderless mode: game=%dx%d, screen=%dx%d", gameWidth, gameHeight, screenWidth, screenHeight);
            }
            else
            {
                // Windowed: DX11 swapchain at game resolution (1:1)
                screenWidth = gameWidth;
                screenHeight = gameHeight;
                dbg_log("  [Reset] Windowed mode: game=%dx%d, DX11=%dx%d", gameWidth, gameHeight, screenWidth, screenHeight);
            }

            // Recreate D3D9 copy surface at GAME resolution
            HRESULT hrCopy = pThis->CreateOffscreenPlainSurface(
                gameWidth, gameHeight,
                D3DFMT_X8R8G8B8,
                D3DPOOL_SYSTEMMEM,
                &g_pD3D9CopySurface,
                nullptr
            );
            if (SUCCEEDED(hrCopy))
            {
                dbg_log("  [Reset] Recreated D3D9 copy surface %dx%d", gameWidth, gameHeight);
            }
            else
            {
                dbg_log("  [Reset] Failed to create D3D9 copy surface, hr=0x%x", hrCopy);
            }

            // Reinitialize DX11
            if (hWnd && InitializeDX11ForHybrid(hWnd, screenWidth, screenHeight, gameWidth, gameHeight))
            {
                dbg_log("  [Reset] DX11 reinitialized");
            }
            else
            {
                dbg_log("  [Reset] Failed to reinitialize DX11");
            }
        }

        return hr;
    }

    HRESULT WINAPI Present_Hook(IDirect3DDevice9* pThis, const RECT* pSourceRect, const RECT* pDestRect,
        HWND hDestWindowOverride, const RGNDATA* pDirtyRegion)
    {
        if (RuntimeConfig::DebugLogging() && presentLogCount < 20)
        {
            dbg_log("IDirect3DDevice9::Present: src=%s, dst=%s, hwnd=0x%p, dx11Active=%d, borderless=%d",
                pSourceRect ? "set" : "null",
                pDestRect ? "set" : "null",
                hDestWindowOverride,
                g_dx11Active ? 1 : 0,
                BorderlessState::g_borderlessActive ? 1 : 0);
            presentLogCount++;
        }

        // DX11 hybrid path - use in both windowed and borderless modes
        // D3D9 renders to offscreen RT, we copy to DX11 and present via DX11 only
        if (g_dx11Active && g_testRenderTargetActive && g_pTestRenderTarget &&
            g_pDXGISwapChain && g_pD3D11Context && g_pD3D9CopySurface && g_pD3D11StagingTexture)
        {
            UINT srcWidth = g_dx11GameWidth;
            UINT srcHeight = g_dx11GameHeight;

            if (RuntimeConfig::DebugLogging() && presentLogCount <= 10)
            {
                dbg_log("  [DX11] %s mode: D3D9 RT %dx%d -> DX11 %dx%d",
                    BorderlessState::g_borderlessActive ? "Borderless" : "Windowed",
                    srcWidth, srcHeight, g_dx11Width, g_dx11Height);
            }

            // 1. Copy D3D9 render target to system memory surface
            HRESULT hr = pThis->GetRenderTargetData(g_pTestRenderTarget, g_pD3D9CopySurface);
            if (FAILED(hr))
            {
                dbg_log("  [DX11] GetRenderTargetData failed, hr=0x%x", hr);
                // Don't fall back to D3D9 Present - that would conflict with DX11 swapchain
                // Just re-set render target and return
                oSetRenderTarget(pThis, 0, g_pTestRenderTarget);
                return S_OK;
            }

            // 2. Lock D3D9 surface and copy to DX11 staging texture
            D3DLOCKED_RECT d3d9Locked;
            hr = g_pD3D9CopySurface->LockRect(&d3d9Locked, nullptr, D3DLOCK_READONLY);
            if (FAILED(hr))
            {
                dbg_log("  [DX11] LockRect failed, hr=0x%x", hr);
                oSetRenderTarget(pThis, 0, g_pTestRenderTarget);
                return S_OK;
            }

            D3D11_MAPPED_SUBRESOURCE d3d11Mapped;
            HRESULT hrMap = g_pD3D11Context->Map(g_pD3D11StagingTexture, 0, D3D11_MAP_WRITE, 0, &d3d11Mapped);
            if (FAILED(hrMap))
            {
                g_pD3D9CopySurface->UnlockRect();
                dbg_log("  [DX11] Map staging texture failed, hr=0x%x", hrMap);
                oSetRenderTarget(pThis, 0, g_pTestRenderTarget);
                return S_OK;
            }

            // Copy row by row
            {
                BYTE* pSrc = (BYTE*)d3d9Locked.pBits;
                BYTE* pDst = (BYTE*)d3d11Mapped.pData;
                UINT rowBytes = srcWidth * 4;

                for (UINT y = 0; y < srcHeight; y++)
                {
                    memcpy(pDst, pSrc, rowBytes);
                    pSrc += d3d9Locked.Pitch;
                    pDst += d3d11Mapped.RowPitch;
                }
            }

            g_pD3D11Context->Unmap(g_pD3D11StagingTexture, 0);
            g_pD3D9CopySurface->UnlockRect();

            // Copy staging texture to source texture (for shader input)
            g_pD3D11Context->CopyResource(g_pD3D11SourceTexture, g_pD3D11StagingTexture);

            // 3. Render to swapchain backbuffer
            float clearColor[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
            g_pD3D11Context->ClearRenderTargetView(g_pD3D11RTV, clearColor);

            if (BorderlessState::g_borderlessActive)
            {
                // CuNNy 2x upscale + Lanczos downscale
                ID3D11ShaderResourceView* cunnyOutput = CuNNyScaler::Upscale2x(
                    g_pD3D11Context, g_pD3D11SourceSRV, srcWidth, srcHeight);
                if (!cunnyOutput)
                    CuNNyScaler::FatalRenderingError("CuNNy upscale");

                UINT upscaledWidth = srcWidth * 2;
                UINT upscaledHeight = srcHeight * 2;

                if (RuntimeConfig::DebugLogging() && presentLogCount <= 10)
                {
                    dbg_log("  [DX11] CuNNy 2x upscale: %dx%d -> %dx%d",
                        srcWidth, srcHeight, upscaledWidth, upscaledHeight);
                }

                ID3D11ShaderResourceView* downscaleOutput = CuNNyScaler::Downscale(
                    g_pD3D11Context, cunnyOutput,
                    upscaledWidth, upscaledHeight,
                    BorderlessState::g_scaledWidth, BorderlessState::g_scaledHeight);
                if (!downscaleOutput)
                    CuNNyScaler::FatalRenderingError("Lanczos downscale");

                if (RuntimeConfig::DebugLogging() && presentLogCount <= 10)
                {
                    dbg_log("  [DX11] Lanczos downscale: %dx%d -> %dx%d",
                        upscaledWidth, upscaledHeight,
                        BorderlessState::g_scaledWidth, BorderlessState::g_scaledHeight);
                }

                // Final 1:1 copy with positioning
                BicubicScaler::Scale(
                    g_pD3D11Context,
                    downscaleOutput,
                    g_pD3D11RTV,
                    BorderlessState::g_scaledWidth, BorderlessState::g_scaledHeight,
                    g_dx11Width, g_dx11Height,
                    BorderlessState::g_offsetX, BorderlessState::g_offsetY,
                    BorderlessState::g_scaledWidth, BorderlessState::g_scaledHeight
                );
            }
            else
            {
                // Windowed mode: 1:1 copy (no scaling)
                BicubicScaler::Scale(
                    g_pD3D11Context,
                    g_pD3D11SourceSRV,
                    g_pD3D11RTV,
                    srcWidth, srcHeight,
                    g_dx11Width, g_dx11Height,
                    0, 0,  // No offset
                    srcWidth, srcHeight  // No scaling
                );

                if (RuntimeConfig::DebugLogging() && presentLogCount <= 10)
                {
                    dbg_log("  [DX11] 1:1 copy: %dx%d", srcWidth, srcHeight);
                }
            }

            // 4. Present via DXGI
            HRESULT hrPresent = g_pDXGISwapChain->Present(1, 0);

            if (RuntimeConfig::DebugLogging() && presentLogCount <= 10)
            {
                dbg_log("  [DX11] DXGI Present returned 0x%x", hrPresent);

                // Log window state after present
                HWND hWnd = BorderlessState::g_mainGameWindow;
                if (hWnd)
                {
                    RECT clientRect;
                    GetClientRect(hWnd, &clientRect);
                    dbg_log("  [DX11] Window client after present: %dx%d",
                        clientRect.right - clientRect.left, clientRect.bottom - clientRect.top);
                }
            }

            // Re-set render target for next frame
            oSetRenderTarget(pThis, 0, g_pTestRenderTarget);

            // Skip D3D9 Present - DX11 is the sole presenter
            return S_OK;
        }

        // Fallback: DX11 not ready yet, use original D3D9 Present
        if (RuntimeConfig::DebugLogging() && presentLogCount <= 20)
        {
            dbg_log("  [Present] DX11 not ready (dx11Active=%d, rtActive=%d), using D3D9 fallback",
                g_dx11Active ? 1 : 0, g_testRenderTargetActive ? 1 : 0);
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

            if (pSourceRect)
                dbg_log("  srcRect: (%d,%d)-(%d,%d)", pSourceRect->left, pSourceRect->top, pSourceRect->right, pSourceRect->bottom);
            else
                dbg_log("  srcRect: null (full surface)");

            if (pDestRect)
                dbg_log("  dstRect: (%d,%d)-(%d,%d)", pDestRect->left, pDestRect->top, pDestRect->right, pDestRect->bottom);
            else
                dbg_log("  dstRect: null (full surface)");

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
            else
            {
                dbg_log("IDirect3DDevice9::GetBackBuffer: chain=%d, idx=%d -> hr=0x%x",
                    iSwapChain, iBackBuffer, hr);
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

    // DX11 resource accessors (for DX11Video)
    bool IsDX11Active() { return g_dx11Active; }
    ID3D11DeviceContext* GetDX11Context() { return g_pD3D11Context; }
    ID3D11RenderTargetView* GetDX11RTV() { return g_pD3D11RTV; }
    IDXGISwapChain1* GetDXGISwapChain() { return g_pDXGISwapChain; }
    void GetDX11Dimensions(UINT* pWidth, UINT* pHeight)
    {
        if (pWidth) *pWidth = g_dx11Width;
        if (pHeight) *pHeight = g_dx11Height;
    }

    bool Install()
    {
        dbg_log("DX11Hooks::Install() called");

        HMODULE hD3D9 = GetModuleHandleA("d3d9.dll");
        if (!hD3D9)
        {
            // d3d9.dll not loaded yet, we'll need to hook LoadLibraryA to catch it
            // For now, try loading it
            hD3D9 = LoadLibraryA("d3d9.dll");
            if (!hD3D9)
            {
                dbg_log("DX11Hooks::Install: Failed to get/load d3d9.dll");
                return false;
            }
        }

        oDirect3DCreate9 = (decltype(oDirect3DCreate9))GetProcAddress(hD3D9, "Direct3DCreate9");
        if (!oDirect3DCreate9)
        {
            dbg_log("DX11Hooks::Install: Failed to find Direct3DCreate9");
            return false;
        }

        dbg_log("DX11Hooks::Install: Found Direct3DCreate9 at 0x%p", oDirect3DCreate9);

        DetourTransactionBegin();
        DetourUpdateThread(GetCurrentThread());
        DetourAttach(&(PVOID&)oDirect3DCreate9, Direct3DCreate9_Hook);
        LONG error = DetourTransactionCommit();

        if (error == NO_ERROR)
        {
            dbg_log("DX11Hooks::Install: Hook installed successfully");
            return true;
        }

        dbg_log("DX11Hooks::Install: Failed to install hook, Detours error: %d", error);
        return false;
    }
}
