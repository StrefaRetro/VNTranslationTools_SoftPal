#include "pch.h"
#include "BicubicScaler.h"
#include <d3dcompiler.h>

#pragma comment(lib, "d3dcompiler.lib")

namespace BicubicScaler
{
    // Shader source code
    static const char* g_shaderSource = R"(
// Bicubic interpolation shader for upscaling

cbuffer ScalerConstants : register(b0)
{
    float2 srcTexelSize;    // 1.0 / srcDimensions
    float2 dstTexelSize;    // 1.0 / dstDimensions (unused but useful for debugging)
    float2 srcDimensions;   // source texture size
    float2 dstDimensions;   // destination size
};

Texture2D srcTexture : register(t0);
SamplerState linearSampler : register(s0);

struct VS_INPUT
{
    float2 pos : POSITION;
    float2 tex : TEXCOORD0;
};

struct PS_INPUT
{
    float4 pos : SV_POSITION;
    float2 tex : TEXCOORD0;
};

// Vertex shader - fullscreen triangle
PS_INPUT VS_Main(VS_INPUT input)
{
    PS_INPUT output;
    output.pos = float4(input.pos, 0.0, 1.0);
    output.tex = input.tex;
    return output;
}

// Cubic weight function (Catmull-Rom spline)
// This gives sharper results than Mitchell-Netravali for upscaling
float4 CubicWeights(float x)
{
    float x2 = x * x;
    float x3 = x2 * x;

    // Catmull-Rom weights
    float4 w;
    w.x = -0.5*x3 + x2 - 0.5*x;           // weight for p0
    w.y = 1.5*x3 - 2.5*x2 + 1.0;          // weight for p1
    w.z = -1.5*x3 + 2.0*x2 + 0.5*x;       // weight for p2
    w.w = 0.5*x3 - 0.5*x2;                // weight for p3

    return w;
}

// Sample texture with bicubic interpolation
float4 SampleBicubic(float2 texcoord)
{
    // Convert to texel coordinates
    float2 texelCoord = texcoord * srcDimensions - 0.5;
    float2 texelFloor = floor(texelCoord);
    float2 texelFrac = texelCoord - texelFloor;

    // Get cubic weights for x and y
    float4 xWeights = CubicWeights(texelFrac.x);
    float4 yWeights = CubicWeights(texelFrac.y);

    // Sample 4x4 grid of texels
    float4 result = float4(0, 0, 0, 0);

    [unroll]
    for (int j = -1; j <= 2; j++)
    {
        float yWeight = (j == -1) ? yWeights.x : (j == 0) ? yWeights.y : (j == 1) ? yWeights.z : yWeights.w;

        [unroll]
        for (int i = -1; i <= 2; i++)
        {
            float xWeight = (i == -1) ? xWeights.x : (i == 0) ? xWeights.y : (i == 1) ? xWeights.z : xWeights.w;

            // Clamp coordinates to valid range
            float2 sampleCoord = (texelFloor + float2(i, j) + 0.5) * srcTexelSize;
            sampleCoord = clamp(sampleCoord, 0.0, 1.0);

            // Point sample (no filtering)
            float4 texel = srcTexture.SampleLevel(linearSampler, sampleCoord, 0);
            result += texel * xWeight * yWeight;
        }
    }

    return saturate(result);
}

// Pixel shader
float4 PS_Main(PS_INPUT input) : SV_TARGET
{
    return SampleBicubic(input.tex);
}
)";

    // Resources
    static ID3D11VertexShader* g_pVertexShader = nullptr;
    static ID3D11PixelShader* g_pPixelShader = nullptr;
    static ID3D11InputLayout* g_pInputLayout = nullptr;
    static ID3D11Buffer* g_pVertexBuffer = nullptr;
    static ID3D11Buffer* g_pConstantBuffer = nullptr;
    static ID3D11SamplerState* g_pSamplerState = nullptr;
    static ID3D11RasterizerState* g_pRasterizerState = nullptr;
    static ID3D11BlendState* g_pBlendState = nullptr;

    struct Vertex
    {
        float pos[2];
        float tex[2];
    };

    struct ScalerConstants
    {
        float srcTexelSize[2];
        float dstTexelSize[2];
        float srcDimensions[2];
        float dstDimensions[2];
    };

    bool Initialize(ID3D11Device* pDevice)
    {
        HRESULT hr;

        // Compile vertex shader
        ID3DBlob* pVSBlob = nullptr;
        ID3DBlob* pErrorBlob = nullptr;
        hr = D3DCompile(
            g_shaderSource,
            strlen(g_shaderSource),
            "BicubicScaler",
            nullptr,
            nullptr,
            "VS_Main",
            "vs_4_0",
            D3DCOMPILE_OPTIMIZATION_LEVEL3,
            0,
            &pVSBlob,
            &pErrorBlob
        );
        if (FAILED(hr))
        {
            if (pErrorBlob)
            {
                OutputDebugStringA((char*)pErrorBlob->GetBufferPointer());
                pErrorBlob->Release();
            }
            return false;
        }

        hr = pDevice->CreateVertexShader(
            pVSBlob->GetBufferPointer(),
            pVSBlob->GetBufferSize(),
            nullptr,
            &g_pVertexShader
        );
        if (FAILED(hr))
        {
            pVSBlob->Release();
            return false;
        }

        // Create input layout
        D3D11_INPUT_ELEMENT_DESC inputDesc[] = {
            { "POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0 },
            { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 8, D3D11_INPUT_PER_VERTEX_DATA, 0 }
        };
        hr = pDevice->CreateInputLayout(
            inputDesc,
            2,
            pVSBlob->GetBufferPointer(),
            pVSBlob->GetBufferSize(),
            &g_pInputLayout
        );
        pVSBlob->Release();
        if (FAILED(hr)) return false;

        // Compile pixel shader
        ID3DBlob* pPSBlob = nullptr;
        hr = D3DCompile(
            g_shaderSource,
            strlen(g_shaderSource),
            "BicubicScaler",
            nullptr,
            nullptr,
            "PS_Main",
            "ps_4_0",
            D3DCOMPILE_OPTIMIZATION_LEVEL3,
            0,
            &pPSBlob,
            &pErrorBlob
        );
        if (FAILED(hr))
        {
            if (pErrorBlob)
            {
                OutputDebugStringA((char*)pErrorBlob->GetBufferPointer());
                pErrorBlob->Release();
            }
            return false;
        }

        hr = pDevice->CreatePixelShader(
            pPSBlob->GetBufferPointer(),
            pPSBlob->GetBufferSize(),
            nullptr,
            &g_pPixelShader
        );
        pPSBlob->Release();
        if (FAILED(hr)) return false;

        // Create vertex buffer (fullscreen quad as two triangles)
        Vertex vertices[] = {
            // Triangle 1
            { {-1.0f,  1.0f}, {0.0f, 0.0f} },  // top-left
            { { 1.0f,  1.0f}, {1.0f, 0.0f} },  // top-right
            { {-1.0f, -1.0f}, {0.0f, 1.0f} },  // bottom-left
            // Triangle 2
            { { 1.0f,  1.0f}, {1.0f, 0.0f} },  // top-right
            { { 1.0f, -1.0f}, {1.0f, 1.0f} },  // bottom-right
            { {-1.0f, -1.0f}, {0.0f, 1.0f} },  // bottom-left
        };

        D3D11_BUFFER_DESC vbDesc = {};
        vbDesc.ByteWidth = sizeof(vertices);
        vbDesc.Usage = D3D11_USAGE_IMMUTABLE;
        vbDesc.BindFlags = D3D11_BIND_VERTEX_BUFFER;

        D3D11_SUBRESOURCE_DATA vbData = {};
        vbData.pSysMem = vertices;

        hr = pDevice->CreateBuffer(&vbDesc, &vbData, &g_pVertexBuffer);
        if (FAILED(hr)) return false;

        // Create constant buffer
        D3D11_BUFFER_DESC cbDesc = {};
        cbDesc.ByteWidth = sizeof(ScalerConstants);
        cbDesc.Usage = D3D11_USAGE_DYNAMIC;
        cbDesc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        cbDesc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;

        hr = pDevice->CreateBuffer(&cbDesc, nullptr, &g_pConstantBuffer);
        if (FAILED(hr)) return false;

        // Create sampler state (point sampling - bicubic does its own filtering)
        D3D11_SAMPLER_DESC samplerDesc = {};
        samplerDesc.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
        samplerDesc.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
        samplerDesc.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
        samplerDesc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
        samplerDesc.MaxLOD = D3D11_FLOAT32_MAX;

        hr = pDevice->CreateSamplerState(&samplerDesc, &g_pSamplerState);
        if (FAILED(hr)) return false;

        // Create rasterizer state
        D3D11_RASTERIZER_DESC rastDesc = {};
        rastDesc.FillMode = D3D11_FILL_SOLID;
        rastDesc.CullMode = D3D11_CULL_NONE;

        hr = pDevice->CreateRasterizerState(&rastDesc, &g_pRasterizerState);
        if (FAILED(hr)) return false;

        // Create blend state (no blending)
        D3D11_BLEND_DESC blendDesc = {};
        blendDesc.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;

        hr = pDevice->CreateBlendState(&blendDesc, &g_pBlendState);
        if (FAILED(hr)) return false;

        return true;
    }

    void Cleanup()
    {
        if (g_pBlendState) { g_pBlendState->Release(); g_pBlendState = nullptr; }
        if (g_pRasterizerState) { g_pRasterizerState->Release(); g_pRasterizerState = nullptr; }
        if (g_pSamplerState) { g_pSamplerState->Release(); g_pSamplerState = nullptr; }
        if (g_pConstantBuffer) { g_pConstantBuffer->Release(); g_pConstantBuffer = nullptr; }
        if (g_pVertexBuffer) { g_pVertexBuffer->Release(); g_pVertexBuffer = nullptr; }
        if (g_pInputLayout) { g_pInputLayout->Release(); g_pInputLayout = nullptr; }
        if (g_pPixelShader) { g_pPixelShader->Release(); g_pPixelShader = nullptr; }
        if (g_pVertexShader) { g_pVertexShader->Release(); g_pVertexShader = nullptr; }
    }

    void Scale(
        ID3D11DeviceContext* pContext,
        ID3D11ShaderResourceView* pSourceSRV,
        ID3D11RenderTargetView* pDestRTV,
        UINT srcWidth, UINT srcHeight,
        UINT dstWidth, UINT dstHeight,
        UINT offsetX, UINT offsetY,
        UINT scaledWidth, UINT scaledHeight)
    {
        if (!g_pVertexShader || !g_pPixelShader) return;

        // Update constant buffer
        D3D11_MAPPED_SUBRESOURCE mapped;
        HRESULT hr = pContext->Map(g_pConstantBuffer, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped);
        if (SUCCEEDED(hr))
        {
            ScalerConstants* pConstants = (ScalerConstants*)mapped.pData;
            pConstants->srcTexelSize[0] = 1.0f / (float)srcWidth;
            pConstants->srcTexelSize[1] = 1.0f / (float)srcHeight;
            pConstants->dstTexelSize[0] = 1.0f / (float)dstWidth;
            pConstants->dstTexelSize[1] = 1.0f / (float)dstHeight;
            pConstants->srcDimensions[0] = (float)srcWidth;
            pConstants->srcDimensions[1] = (float)srcHeight;
            pConstants->dstDimensions[0] = (float)dstWidth;
            pConstants->dstDimensions[1] = (float)dstHeight;
            pContext->Unmap(g_pConstantBuffer, 0);
        }

        // Set viewport to the scaled region (pillarboxing)
        D3D11_VIEWPORT viewport = {};
        viewport.TopLeftX = (float)offsetX;
        viewport.TopLeftY = (float)offsetY;
        viewport.Width = (float)scaledWidth;
        viewport.Height = (float)scaledHeight;
        viewport.MinDepth = 0.0f;
        viewport.MaxDepth = 1.0f;
        pContext->RSSetViewports(1, &viewport);

        // Set render target
        pContext->OMSetRenderTargets(1, &pDestRTV, nullptr);

        // Set shaders
        pContext->VSSetShader(g_pVertexShader, nullptr, 0);
        pContext->PSSetShader(g_pPixelShader, nullptr, 0);
        pContext->PSSetConstantBuffers(0, 1, &g_pConstantBuffer);
        pContext->PSSetShaderResources(0, 1, &pSourceSRV);
        pContext->PSSetSamplers(0, 1, &g_pSamplerState);

        // Set input assembler
        UINT stride = sizeof(Vertex);
        UINT offset = 0;
        pContext->IASetVertexBuffers(0, 1, &g_pVertexBuffer, &stride, &offset);
        pContext->IASetInputLayout(g_pInputLayout);
        pContext->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

        // Set rasterizer and blend state
        pContext->RSSetState(g_pRasterizerState);
        pContext->OMSetBlendState(g_pBlendState, nullptr, 0xFFFFFFFF);

        // Draw fullscreen quad
        pContext->Draw(6, 0);

        // Unbind shader resource to avoid warnings
        ID3D11ShaderResourceView* nullSRV = nullptr;
        pContext->PSSetShaderResources(0, 1, &nullSRV);
    }

    ID3D11ShaderResourceView* CreateSRV(ID3D11Device* pDevice, ID3D11Texture2D* pTexture)
    {
        if (!pDevice || !pTexture) return nullptr;

        D3D11_TEXTURE2D_DESC texDesc;
        pTexture->GetDesc(&texDesc);

        D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
        srvDesc.Format = texDesc.Format;
        srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
        srvDesc.Texture2D.MipLevels = 1;

        ID3D11ShaderResourceView* pSRV = nullptr;
        pDevice->CreateShaderResourceView(pTexture, &srvDesc, &pSRV);
        return pSRV;
    }
}
