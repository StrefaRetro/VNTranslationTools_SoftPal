#pragma once

#include <d3d11.h>
#include <dxgi1_2.h>

namespace DX11Hooks {
    bool Install();

    // DX11 resource accessors (for DX11Video)
    bool IsDX11Active();
    ID3D11DeviceContext* GetDX11Context();
    ID3D11RenderTargetView* GetDX11RTV();
    IDXGISwapChain1* GetDXGISwapChain();
    void GetDX11Dimensions(UINT* pWidth, UINT* pHeight);
}
