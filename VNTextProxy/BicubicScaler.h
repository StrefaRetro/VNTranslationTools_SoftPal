#pragma once

#include <d3d11.h>

namespace BicubicScaler
{
    // Initialize the bicubic scaler (shaders, vertex buffer, sampler, etc.)
    bool Initialize(ID3D11Device* pDevice);

    // Cleanup resources
    void Cleanup();

    // Scale source texture to render target with bicubic filtering
    // srcWidth/srcHeight: dimensions of source texture
    // dstWidth/dstHeight: dimensions of destination (screen)
    // offsetX/offsetY, scaledWidth/scaledHeight: destination rectangle for pillarboxing
    void Scale(
        ID3D11DeviceContext* pContext,
        ID3D11ShaderResourceView* pSourceSRV,
        ID3D11RenderTargetView* pDestRTV,
        UINT srcWidth, UINT srcHeight,
        UINT dstWidth, UINT dstHeight,
        UINT offsetX, UINT offsetY,
        UINT scaledWidth, UINT scaledHeight
    );

    // Create a shader resource view for a texture
    ID3D11ShaderResourceView* CreateSRV(ID3D11Device* pDevice, ID3D11Texture2D* pTexture);
}
