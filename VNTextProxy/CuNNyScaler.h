#pragma once

#include <d3d11.h>

namespace CuNNyScaler
{
    // Initialize the CuNNy neural network scaler
    // This is a 2x upscaler - output will be 2x input dimensions
    bool Initialize(ID3D11Device* pDevice);

    // Cleanup resources
    void Cleanup();

    // Upscale source texture by 2x using CuNNy neural network
    // Returns the 2x upscaled texture (caller should NOT release it)
    // srcWidth/srcHeight: dimensions of source texture
    // The output texture will be srcWidth*2 x srcHeight*2
    ID3D11ShaderResourceView* Upscale2x(
        ID3D11DeviceContext* pContext,
        ID3D11ShaderResourceView* pSourceSRV,
        UINT srcWidth, UINT srcHeight
    );

    // Get the intermediate 2x upscaled texture for further processing
    ID3D11Texture2D* GetUpscaledTexture();
    ID3D11ShaderResourceView* GetUpscaledSRV();

    // Downscale using Lanczos2 with antiring (for final scale to target size)
    // srcSRV: the 2x upscaled texture from Upscale2x
    // srcW/srcH: source dimensions (2x upscaled size)
    // dstW/dstH: target output dimensions
    ID3D11ShaderResourceView* Downscale(
        ID3D11DeviceContext* pContext,
        ID3D11ShaderResourceView* pSourceSRV,
        UINT srcWidth, UINT srcHeight,
        UINT dstWidth, UINT dstHeight
    );

    // Get the downscaled output texture
    ID3D11Texture2D* GetDownscaledTexture();
    ID3D11ShaderResourceView* GetDownscaledSRV();

    // Check if CuNNy is available/initialized
    bool IsAvailable();

    // Check if downscale shader is available
    bool IsDownscaleAvailable();
}
