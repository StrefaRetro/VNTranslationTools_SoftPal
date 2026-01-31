#include "pch.h"
#include "DX11Video.h"
#include "DX11Hooks.h"
#include "PillarboxedState.h"
#include "BicubicScaler.h"
#include "CuNNyScaler.h"
#include "SharedConstants.h"

static FILE* g_logFile = nullptr;
static void dbg_log(const char* format, ...)
{
    if (!RuntimeConfig::DebugLogging())
        return;
    if (!g_logFile)
        g_logFile = _fsopen("./DX11Video.log", "w", _SH_DENYNO);
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

namespace DX11Video {

    void PresentVideoFrame(ID3D11ShaderResourceView* pVideoSRV, UINT width, UINT height)
    {
        // Get DX11 resources from D3D9Hooks
        ID3D11DeviceContext* pContext = DX11Hooks::GetDX11Context();
        ID3D11RenderTargetView* pRTV = DX11Hooks::GetDX11RTV();
        IDXGISwapChain* pSwapChain = DX11Hooks::GetDXGISwapChain();
        UINT screenWidth, screenHeight;
        DX11Hooks::GetDX11Dimensions(&screenWidth, &screenHeight);

        if (!DX11Hooks::IsDX11Active() || !pSwapChain || !pContext || !pRTV || !pVideoSRV)
            return;

        static int videoFrameCount = 0;
        videoFrameCount++;
        if (videoFrameCount <= 5 || videoFrameCount % 100 == 0)
        {
            dbg_log("[DX11] PresentVideoFrame #%d: %dx%d, SRV=%p", videoFrameCount, width, height, pVideoSRV);
        }

        // Clear the render target to black
        float clearColor[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
        pContext->ClearRenderTargetView(pRTV, clearColor);

        if (PillarboxedState::g_pillarboxedActive)
        {
            // Pillarboxed mode: CuNNy upscale + Lanczos downscale with pillarboxing
            UINT scaledWidth = PillarboxedState::g_scaledWidth;
            UINT scaledHeight = PillarboxedState::g_scaledHeight;
            int offsetX = PillarboxedState::g_offsetX;
            int offsetY = PillarboxedState::g_offsetY;

            // CuNNy 2x upscale
            ID3D11ShaderResourceView* cunnyOutput = CuNNyScaler::Upscale2x(
                pContext, pVideoSRV, width, height);
            if (!cunnyOutput)
                CuNNyScaler::FatalRenderingError("video CuNNy upscale");

            UINT upscaledWidth = width * 2;
            UINT upscaledHeight = height * 2;

            if (videoFrameCount <= 5)
            {
                dbg_log("[DX11] Video CuNNy 2x upscale: %dx%d -> %dx%d",
                    width, height, upscaledWidth, upscaledHeight);
            }

            // Lanczos downscale for final scale to target size
            ID3D11ShaderResourceView* downscaleOutput = CuNNyScaler::Downscale(
                pContext, cunnyOutput,
                upscaledWidth, upscaledHeight,
                scaledWidth, scaledHeight);
            if (!downscaleOutput)
                CuNNyScaler::FatalRenderingError("video Lanczos downscale");

            if (videoFrameCount <= 5)
            {
                dbg_log("[DX11] Video Lanczos downscale: %dx%d -> %dx%d",
                    upscaledWidth, upscaledHeight, scaledWidth, scaledHeight);
            }

            // Final 1:1 copy with positioning
            BicubicScaler::Scale(
                pContext,
                downscaleOutput,
                pRTV,
                scaledWidth, scaledHeight,
                screenWidth, screenHeight,
                offsetX, offsetY,
                scaledWidth, scaledHeight
            );
        }
        else
        {
            // Windowed mode: 1:1 copy (no scaling)
            if (videoFrameCount <= 5)
            {
                dbg_log("[DX11] Video 1:1 copy: %dx%d", width, height);
            }

            BicubicScaler::Scale(
                pContext,
                pVideoSRV,
                pRTV,
                width, height,
                screenWidth, screenHeight,
                0, 0,  // No offset
                width, height  // No scaling
            );
        }

        // Present
        HRESULT hr = pSwapChain->Present(1, 0);
        if (videoFrameCount <= 5)
        {
            dbg_log("[DX11] PresentVideoFrame: Present returned 0x%x", hr);
        }
    }

}
