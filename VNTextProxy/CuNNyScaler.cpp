#include "pch.h"
#include "CuNNyScaler.h"
#include "Util/RuntimeConfig.h"
#include <d3dcompiler.h>
#include <fstream>
#include <sstream>

#pragma comment(lib, "d3dcompiler.lib")

namespace CuNNyScaler
{
    static FILE* g_cunnyLog = nullptr;
    static void cunny_log(const char* format, ...)
    {
        if (!RuntimeConfig::DebugLogging())
            return;
        if (!g_cunnyLog)
            g_cunnyLog = _fsopen("./CuNNy.log", "w", _SH_DENYNO);
        if (g_cunnyLog)
        {
            va_list args;
            va_start(args, format);
            vfprintf(g_cunnyLog, format, args);
            fprintf(g_cunnyLog, "\n");
            va_end(args);
            fflush(g_cunnyLog);
        }
    }
    // Common D3D11 header for all passes
    static const char* g_d3d11Header = R"(
#define V4 min16float4
#define M4 min16float4x4

cbuffer Constants : register(b0) {
    uint2 inputSize;
    uint2 outputSize;
    float2 inputPt;
    float2 outputPt;
};

SamplerState SP : register(s0);
SamplerState SL : register(s1);

uint2 GetInputSize() { return inputSize; }
uint2 GetOutputSize() { return outputSize; }
float2 GetInputPt() { return inputPt; }
float2 GetOutputPt() { return outputPt; }
uint2 Rmp8x8(uint idx) { return uint2(idx % 8, idx / 8); }
)";

    static ID3D11Device* g_pDevice = nullptr;
    static ID3D11ComputeShader* g_pPass1CS = nullptr;
    static ID3D11ComputeShader* g_pPass2CS = nullptr;
    static ID3D11ComputeShader* g_pPass3CS = nullptr;
    static ID3D11ComputeShader* g_pPass4CS = nullptr;
    static ID3D11ComputeShader* g_pDownscaleCS = nullptr;
    static ID3D11Buffer* g_pConstantBuffer = nullptr;
    static ID3D11SamplerState* g_pPointSampler = nullptr;
    static ID3D11SamplerState* g_pLinearSampler = nullptr;

    static ID3D11Texture2D* g_pT[6] = {};
    static ID3D11ShaderResourceView* g_pTSRV[6] = {};
    static ID3D11UnorderedAccessView* g_pTUAV[6] = {};

    static ID3D11Texture2D* g_pOutput = nullptr;
    static ID3D11ShaderResourceView* g_pOutputSRV = nullptr;
    static ID3D11UnorderedAccessView* g_pOutputUAV = nullptr;

    // Downscale output (final target size)
    static ID3D11Texture2D* g_pDownscaleOutput = nullptr;
    static ID3D11ShaderResourceView* g_pDownscaleOutputSRV = nullptr;
    static ID3D11UnorderedAccessView* g_pDownscaleOutputUAV = nullptr;
    static UINT g_downscaleWidth = 0, g_downscaleHeight = 0;

    static UINT g_currentWidth = 0, g_currentHeight = 0;
    static bool g_initialized = false;

    struct Constants {
        UINT inputWidth, inputHeight, outputWidth, outputHeight;
        float inputPtX, inputPtY, outputPtX, outputPtY;
    };

    static std::string LoadFile(const char* name) {
        const char* paths[] = { name, "./CuNNy-fast-NVL.hlsl", "../VNTranslationTools/VNTextProxy/CuNNy-fast-NVL.hlsl" };
        cunny_log("LoadFile: Looking for shader file...");
        for (auto p : paths) {
            cunny_log("  Trying path: %s", p);
            std::ifstream f(p);
            if (f) {
                std::stringstream ss;
                ss << f.rdbuf();
                std::string content = ss.str();
                cunny_log("  SUCCESS! Loaded %zu bytes from: %s", content.length(), p);
                return content;
            } else {
                cunny_log("  Failed to open: %s", p);
            }
        }
        cunny_log("LoadFile: FAILED - could not find shader file in any path");
        return "";
    }

    static std::string ExtractPass(const std::string& src, int num) {
        std::string marker = "//!PASS " + std::to_string(num);
        cunny_log("ExtractPass %d: Looking for marker '%s'", num, marker.c_str());
        size_t start = src.find(marker);
        if (start == std::string::npos) {
            cunny_log("ExtractPass %d: FAILED - marker not found", num);
            return "";
        }
        std::string nextMarker = "//!PASS " + std::to_string(num + 1);
        size_t end = src.find(nextMarker);
        if (end == std::string::npos) end = src.length();
        std::string result = src.substr(start, end - start);
        cunny_log("ExtractPass %d: SUCCESS - extracted %zu bytes", num, result.length());
        return result;
    }

    static std::string ExtractFunctionBody(const std::string& pass, int num) {
        std::string funcName = "void Pass" + std::to_string(num);
        cunny_log("ExtractFunctionBody %d: Looking for '%s'", num, funcName.c_str());
        size_t funcStart = pass.find(funcName);
        if (funcStart == std::string::npos) {
            cunny_log("ExtractFunctionBody %d: FAILED - function not found", num);
            // Log first 200 chars to help debug
            cunny_log("ExtractFunctionBody %d: pass starts with: %.200s", num, pass.c_str());
            return "";
        }
        cunny_log("ExtractFunctionBody %d: Found function at offset %zu", num, funcStart);

        // Find opening brace
        size_t braceStart = pass.find('{', funcStart);
        if (braceStart == std::string::npos) {
            cunny_log("ExtractFunctionBody %d: FAILED - no opening brace found", num);
            return "";
        }

        // Find matching closing brace
        int depth = 1;
        size_t i = braceStart + 1;
        while (i < pass.length() && depth > 0) {
            if (pass[i] == '{') depth++;
            else if (pass[i] == '}') depth--;
            i++;
        }

        std::string body = pass.substr(braceStart + 1, i - braceStart - 2);
        cunny_log("ExtractFunctionBody %d: SUCCESS - extracted %zu bytes", num, body.length());
        return body;
    }

    static ID3D11ComputeShader* CompileCS(const std::string& src, const char* name) {
        cunny_log("CompileCS: Compiling %s (%zu bytes)", name, src.length());
        ID3DBlob *blob = nullptr, *err = nullptr;
        HRESULT hr = D3DCompile(src.c_str(), src.length(), name, nullptr, nullptr,
            "main", "cs_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &blob, &err);
        if (FAILED(hr)) {
            cunny_log("CompileCS: FAILED to compile %s (hr=0x%08X)", name, hr);
            if (err) {
                cunny_log("CompileCS: Shader error:\n%s", (char*)err->GetBufferPointer());
                err->Release();
            }
            return nullptr;
        }
        ID3D11ComputeShader* cs = nullptr;
        HRESULT createHr = g_pDevice->CreateComputeShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr, &cs);
        blob->Release();
        if (FAILED(createHr)) {
            cunny_log("CompileCS: FAILED to create shader %s (hr=0x%08X)", name, createHr);
            return nullptr;
        }
        cunny_log("CompileCS: SUCCESS - created %s", name);
        return cs;
    }

    static std::string BuildPass1(const std::string& body) {
        return std::string(g_d3d11Header) + R"(
Texture2D<float4> INPUT : register(t0);
RWTexture2D<float4> T0 : register(u0);
RWTexture2D<float4> T1 : register(u1);
RWTexture2D<float4> T2 : register(u2);

#define O(t, x, y) t.SampleLevel(SP, pos + float2(x, y) * pt, 0)
#define L0(x, y) min16float(dot(float3(0.299, 0.587, 0.114), O(INPUT, x, y).rgb))

[numthreads(64, 1, 1)]
void main(uint3 tid : SV_GroupThreadID, uint3 gid : SV_GroupID) {
    uint2 blockStart = gid.xy * 8;
)" + body + "\n}";
    }

    static std::string BuildPass2(const std::string& body) {
        return std::string(g_d3d11Header) + R"(
Texture2D<float4> T0 : register(t0);
Texture2D<float4> T1 : register(t1);
Texture2D<float4> T2 : register(t2);
RWTexture2D<float4> T3 : register(u0);
RWTexture2D<float4> T4 : register(u1);
RWTexture2D<float4> T5 : register(u2);

#define O(t, x, y) t.SampleLevel(SP, pos + float2(x, y) * pt, 0)
#define L0(x, y) V4(O(T0, x, y))
#define L1(x, y) V4(O(T1, x, y))
#define L2(x, y) V4(O(T2, x, y))

[numthreads(64, 1, 1)]
void main(uint3 tid : SV_GroupThreadID, uint3 gid : SV_GroupID) {
    uint2 blockStart = gid.xy * 8;
)" + body + "\n}";
    }

    static std::string BuildPass3(const std::string& body) {
        return std::string(g_d3d11Header) + R"(
Texture2D<float4> T3 : register(t0);
Texture2D<float4> T4 : register(t1);
Texture2D<float4> T5 : register(t2);
RWTexture2D<float4> T0 : register(u0);
RWTexture2D<float4> T1 : register(u1);

#define O(t, x, y) t.SampleLevel(SP, pos + float2(x, y) * pt, 0)
#define L0(x, y) V4(O(T3, x, y))
#define L1(x, y) V4(O(T4, x, y))
#define L2(x, y) V4(O(T5, x, y))

[numthreads(64, 1, 1)]
void main(uint3 tid : SV_GroupThreadID, uint3 gid : SV_GroupID) {
    uint2 blockStart = gid.xy * 8;
)" + body + "\n}";
    }

    static std::string BuildPass4(const std::string& body) {
        return std::string(g_d3d11Header) + R"(
Texture2D<float4> INPUT : register(t0);
Texture2D<float4> T0 : register(t1);
Texture2D<float4> T1 : register(t2);
RWTexture2D<float4> OUTPUT : register(u0);

#define O(t, x, y) t.SampleLevel(SP, pos + float2(x, y) * pt, 0)
#define L0(x, y) V4(O(T0, x, y))
#define L1(x, y) V4(O(T1, x, y))

[numthreads(64, 1, 1)]
void main(uint3 tid : SV_GroupThreadID, uint3 gid : SV_GroupID) {
    uint2 blockStart = gid.xy * 16;
)" + body + "\n}";
    }

    static std::string LoadDownscaleFile() {
        const char* paths[] = { "Downscale.hlsl", "./Downscale.hlsl", "../VNTranslationTools/VNTextProxy/Downscale.hlsl" };
        cunny_log("LoadDownscaleFile: Looking for shader file...");
        for (auto p : paths) {
            cunny_log("  Trying path: %s", p);
            std::ifstream f(p);
            if (f) {
                std::stringstream ss;
                ss << f.rdbuf();
                std::string content = ss.str();
                cunny_log("  SUCCESS! Loaded %zu bytes from: %s", content.length(), p);
                return content;
            }
        }
        cunny_log("LoadDownscaleFile: FAILED - could not find shader file");
        return "";
    }

    static std::string ExtractDownscaleBody(const std::string& src) {
        // Find "float4 Pass1(float2 p)" function
        cunny_log("ExtractDownscaleBody: Looking for 'float4 Pass1'");
        size_t funcStart = src.find("float4 Pass1");
        if (funcStart == std::string::npos) {
            cunny_log("ExtractDownscaleBody: FAILED - function not found");
            return "";
        }

        size_t braceStart = src.find('{', funcStart);
        if (braceStart == std::string::npos) return "";

        int depth = 1;
        size_t i = braceStart + 1;
        while (i < src.length() && depth > 0) {
            if (src[i] == '{') depth++;
            else if (src[i] == '}') depth--;
            i++;
        }

        std::string body = src.substr(braceStart + 1, i - braceStart - 2);
        cunny_log("ExtractDownscaleBody: SUCCESS - extracted %zu bytes", body.length());
        return body;
    }

    static std::string ExtractDownscaleFunctions(const std::string& src) {
        // Extract lanczos function and macros before Pass1
        size_t passStart = src.find("float4 Pass1");
        if (passStart == std::string::npos) return "";

        // Find start after //!OUT OUTPUT line
        size_t funcStart = src.find("float lanczos");
        if (funcStart == std::string::npos || funcStart > passStart) return "";

        return src.substr(funcStart, passStart - funcStart);
    }

    static std::string BuildDownscalePass(const std::string& functions, const std::string& body) {
        return std::string(g_d3d11Header) + R"(
Texture2D<float4> INPUT : register(t0);
RWTexture2D<float4> OUTPUT : register(u0);

SamplerState S : register(s0);  // Point sampler for Gather operations

static const float blur = 1.0;

)" + functions + R"(

[numthreads(8, 8, 1)]
void main(uint3 dtid : SV_DispatchThreadID) {
    uint2 outSz = GetOutputSize();
    if (dtid.x >= outSz.x || dtid.y >= outSz.y) return;
    float2 p = (dtid.xy + 0.5) / float2(outSz);
)" + body + R"(
    OUTPUT[dtid.xy] = result;
}
)";
    }

    static bool CreateDownscaleTexture(UINT w, UINT h) {
        if (g_pDownscaleOutput) { g_pDownscaleOutput->Release(); g_pDownscaleOutput = nullptr; }
        if (g_pDownscaleOutputSRV) { g_pDownscaleOutputSRV->Release(); g_pDownscaleOutputSRV = nullptr; }
        if (g_pDownscaleOutputUAV) { g_pDownscaleOutputUAV->Release(); g_pDownscaleOutputUAV = nullptr; }

        D3D11_TEXTURE2D_DESC td = {};
        td.Width = w; td.Height = h; td.MipLevels = 1; td.ArraySize = 1;
        td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        td.SampleDesc.Count = 1; td.Usage = D3D11_USAGE_DEFAULT;
        td.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;

        if (FAILED(g_pDevice->CreateTexture2D(&td, nullptr, &g_pDownscaleOutput))) return false;
        if (FAILED(g_pDevice->CreateShaderResourceView(g_pDownscaleOutput, nullptr, &g_pDownscaleOutputSRV))) return false;
        if (FAILED(g_pDevice->CreateUnorderedAccessView(g_pDownscaleOutput, nullptr, &g_pDownscaleOutputUAV))) return false;

        g_downscaleWidth = w; g_downscaleHeight = h;
        cunny_log("CreateDownscaleTexture: Created %dx%d texture", w, h);
        return true;
    }

    static bool CreateTextures(UINT w, UINT h) {
        for (int i = 0; i < 6; i++) {
            if (g_pT[i]) { g_pT[i]->Release(); g_pT[i] = nullptr; }
            if (g_pTSRV[i]) { g_pTSRV[i]->Release(); g_pTSRV[i] = nullptr; }
            if (g_pTUAV[i]) { g_pTUAV[i]->Release(); g_pTUAV[i] = nullptr; }
        }
        if (g_pOutput) { g_pOutput->Release(); g_pOutput = nullptr; }
        if (g_pOutputSRV) { g_pOutputSRV->Release(); g_pOutputSRV = nullptr; }
        if (g_pOutputUAV) { g_pOutputUAV->Release(); g_pOutputUAV = nullptr; }

        D3D11_TEXTURE2D_DESC td = {};
        td.Width = w; td.Height = h; td.MipLevels = 1; td.ArraySize = 1;
        td.Format = DXGI_FORMAT_R16G16B16A16_FLOAT; // Higher precision for NN
        td.SampleDesc.Count = 1; td.Usage = D3D11_USAGE_DEFAULT;
        td.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;

        for (int i = 0; i < 6; i++) {
            if (FAILED(g_pDevice->CreateTexture2D(&td, nullptr, &g_pT[i]))) return false;
            if (FAILED(g_pDevice->CreateShaderResourceView(g_pT[i], nullptr, &g_pTSRV[i]))) return false;
            if (FAILED(g_pDevice->CreateUnorderedAccessView(g_pT[i], nullptr, &g_pTUAV[i]))) return false;
        }

        td.Width = w * 2; td.Height = h * 2;
        td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        if (FAILED(g_pDevice->CreateTexture2D(&td, nullptr, &g_pOutput))) return false;
        if (FAILED(g_pDevice->CreateShaderResourceView(g_pOutput, nullptr, &g_pOutputSRV))) return false;
        if (FAILED(g_pDevice->CreateUnorderedAccessView(g_pOutput, nullptr, &g_pOutputUAV))) return false;

        g_currentWidth = w; g_currentHeight = h;
        return true;
    }

    bool Initialize(ID3D11Device* pDevice) {
        cunny_log("=== CuNNy Initialize starting ===");
        g_pDevice = pDevice;

        D3D11_BUFFER_DESC cbd = {};
        cbd.ByteWidth = sizeof(Constants);
        cbd.Usage = D3D11_USAGE_DYNAMIC;
        cbd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        cbd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        if (FAILED(pDevice->CreateBuffer(&cbd, nullptr, &g_pConstantBuffer))) {
            cunny_log("Initialize: FAILED to create constant buffer");
            return false;
        }
        cunny_log("Initialize: Constant buffer created");

        D3D11_SAMPLER_DESC sd = {};
        sd.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
        sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
        if (FAILED(pDevice->CreateSamplerState(&sd, &g_pPointSampler))) {
            cunny_log("Initialize: FAILED to create point sampler");
            return false;
        }
        sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
        if (FAILED(pDevice->CreateSamplerState(&sd, &g_pLinearSampler))) {
            cunny_log("Initialize: FAILED to create linear sampler");
            return false;
        }
        cunny_log("Initialize: Samplers created");

        std::string shader = LoadFile("CuNNy-fast-NVL.hlsl");
        if (shader.empty()) {
            cunny_log("Initialize: FAILED - shader file not found or empty");
            return false;
        }
        cunny_log("Initialize: Shader file loaded, %zu bytes", shader.length());

        // Extract and compile each pass
        for (int p = 1; p <= 4; p++) {
            cunny_log("Initialize: Processing pass %d", p);
            std::string pass = ExtractPass(shader, p);
            std::string body = ExtractFunctionBody(pass, p);
            if (body.empty()) {
                cunny_log("Initialize: FAILED - could not extract body for pass %d", p);
                return false;
            }
            cunny_log("Initialize: Extracted body for pass %d (%zu bytes)", p, body.length());

            std::string fullShader;
            switch (p) {
                case 1: fullShader = BuildPass1(body); break;
                case 2: fullShader = BuildPass2(body); break;
                case 3: fullShader = BuildPass3(body); break;
                case 4: fullShader = BuildPass4(body); break;
            }
            cunny_log("Initialize: Built full shader for pass %d (%zu bytes)", p, fullShader.length());

            ID3D11ComputeShader** ppCS = nullptr;
            switch (p) {
                case 1: ppCS = &g_pPass1CS; break;
                case 2: ppCS = &g_pPass2CS; break;
                case 3: ppCS = &g_pPass3CS; break;
                case 4: ppCS = &g_pPass4CS; break;
            }

            *ppCS = CompileCS(fullShader, ("Pass" + std::to_string(p)).c_str());
            if (!*ppCS) {
                cunny_log("Initialize: FAILED - could not compile pass %d", p);
                return false;
            }
        }

        // Load and compile downscale shader
        cunny_log("Initialize: Loading Downscale shader");
        std::string downscaleSrc = LoadDownscaleFile();
        if (!downscaleSrc.empty()) {
            std::string functions = ExtractDownscaleFunctions(downscaleSrc);
            std::string body = ExtractDownscaleBody(downscaleSrc);
            if (!body.empty()) {
                // The original returns float4, we need to capture it
                // Replace "return" with "float4 result ="
                size_t returnPos = body.rfind("return");
                if (returnPos != std::string::npos) {
                    body.replace(returnPos, 6, "float4 result =");
                }
                std::string fullShader = BuildDownscalePass(functions, body);
                cunny_log("Initialize: Built downscale shader (%zu bytes)", fullShader.length());
                g_pDownscaleCS = CompileCS(fullShader, "Downscale");
                if (!g_pDownscaleCS) {
                    cunny_log("Initialize: WARNING - Downscale shader failed to compile, will use direct copy");
                }
            }
        } else {
            cunny_log("Initialize: WARNING - Downscale.hlsl not found, will use direct copy");
        }

        g_initialized = true;
        cunny_log("=== CuNNy Initialize SUCCESS - all 4 passes compiled ===");
        return true;
    }

    void Cleanup() {
        if (g_pPass1CS) { g_pPass1CS->Release(); g_pPass1CS = nullptr; }
        if (g_pPass2CS) { g_pPass2CS->Release(); g_pPass2CS = nullptr; }
        if (g_pPass3CS) { g_pPass3CS->Release(); g_pPass3CS = nullptr; }
        if (g_pPass4CS) { g_pPass4CS->Release(); g_pPass4CS = nullptr; }
        if (g_pDownscaleCS) { g_pDownscaleCS->Release(); g_pDownscaleCS = nullptr; }
        if (g_pConstantBuffer) { g_pConstantBuffer->Release(); g_pConstantBuffer = nullptr; }
        if (g_pPointSampler) { g_pPointSampler->Release(); g_pPointSampler = nullptr; }
        if (g_pLinearSampler) { g_pLinearSampler->Release(); g_pLinearSampler = nullptr; }
        for (int i = 0; i < 6; i++) {
            if (g_pT[i]) { g_pT[i]->Release(); g_pT[i] = nullptr; }
            if (g_pTSRV[i]) { g_pTSRV[i]->Release(); g_pTSRV[i] = nullptr; }
            if (g_pTUAV[i]) { g_pTUAV[i]->Release(); g_pTUAV[i] = nullptr; }
        }
        if (g_pOutput) { g_pOutput->Release(); g_pOutput = nullptr; }
        if (g_pOutputSRV) { g_pOutputSRV->Release(); g_pOutputSRV = nullptr; }
        if (g_pOutputUAV) { g_pOutputUAV->Release(); g_pOutputUAV = nullptr; }
        if (g_pDownscaleOutput) { g_pDownscaleOutput->Release(); g_pDownscaleOutput = nullptr; }
        if (g_pDownscaleOutputSRV) { g_pDownscaleOutputSRV->Release(); g_pDownscaleOutputSRV = nullptr; }
        if (g_pDownscaleOutputUAV) { g_pDownscaleOutputUAV->Release(); g_pDownscaleOutputUAV = nullptr; }
        g_pDevice = nullptr;
        g_initialized = false;
    }

    ID3D11ShaderResourceView* Upscale2x(ID3D11DeviceContext* ctx,
        ID3D11ShaderResourceView* srcSRV, UINT w, UINT h)
    {
        if (!g_initialized) return nullptr;
        if (w != g_currentWidth || h != g_currentHeight)
            if (!CreateTextures(w, h)) return nullptr;

        D3D11_MAPPED_SUBRESOURCE m;
        if (SUCCEEDED(ctx->Map(g_pConstantBuffer, 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) {
            Constants* c = (Constants*)m.pData;
            c->inputWidth = w; c->inputHeight = h;
            c->outputWidth = w * 2; c->outputHeight = h * 2;
            c->inputPtX = 1.0f / w; c->inputPtY = 1.0f / h;
            c->outputPtX = 0.5f / w; c->outputPtY = 0.5f / h;
            ctx->Unmap(g_pConstantBuffer, 0);
        }

        ctx->CSSetConstantBuffers(0, 1, &g_pConstantBuffer);
        ID3D11SamplerState* samplers[] = { g_pPointSampler, g_pLinearSampler };
        ctx->CSSetSamplers(0, 2, samplers);

        ID3D11UnorderedAccessView* nullUAV[3] = {};
        ID3D11ShaderResourceView* nullSRV[3] = {};
        UINT dispatchX = (w + 7) / 8, dispatchY = (h + 7) / 8;

        // Pass 1: INPUT -> T0, T1, T2
        ctx->CSSetShader(g_pPass1CS, nullptr, 0);
        ctx->CSSetShaderResources(0, 1, &srcSRV);
        ID3D11UnorderedAccessView* uav1[] = { g_pTUAV[0], g_pTUAV[1], g_pTUAV[2] };
        ctx->CSSetUnorderedAccessViews(0, 3, uav1, nullptr);
        ctx->Dispatch(dispatchX, dispatchY, 1);
        ctx->CSSetUnorderedAccessViews(0, 3, nullUAV, nullptr);
        ctx->CSSetShaderResources(0, 1, nullSRV);

        // Pass 2: T0, T1, T2 -> T3, T4, T5
        ctx->CSSetShader(g_pPass2CS, nullptr, 0);
        ID3D11ShaderResourceView* srv2[] = { g_pTSRV[0], g_pTSRV[1], g_pTSRV[2] };
        ctx->CSSetShaderResources(0, 3, srv2);
        ID3D11UnorderedAccessView* uav2[] = { g_pTUAV[3], g_pTUAV[4], g_pTUAV[5] };
        ctx->CSSetUnorderedAccessViews(0, 3, uav2, nullptr);
        ctx->Dispatch(dispatchX, dispatchY, 1);
        ctx->CSSetUnorderedAccessViews(0, 3, nullUAV, nullptr);
        ctx->CSSetShaderResources(0, 3, nullSRV);

        // Pass 3: T3, T4, T5 -> T0, T1
        ctx->CSSetShader(g_pPass3CS, nullptr, 0);
        ID3D11ShaderResourceView* srv3[] = { g_pTSRV[3], g_pTSRV[4], g_pTSRV[5] };
        ctx->CSSetShaderResources(0, 3, srv3);
        ID3D11UnorderedAccessView* uav3[] = { g_pTUAV[0], g_pTUAV[1] };
        ctx->CSSetUnorderedAccessViews(0, 2, uav3, nullptr);
        ctx->Dispatch(dispatchX, dispatchY, 1);
        ctx->CSSetUnorderedAccessViews(0, 2, nullUAV, nullptr);
        ctx->CSSetShaderResources(0, 3, nullSRV);

        // Pass 4: INPUT, T0, T1 -> OUTPUT
        ctx->CSSetShader(g_pPass4CS, nullptr, 0);
        ID3D11ShaderResourceView* srv4[] = { srcSRV, g_pTSRV[0], g_pTSRV[1] };
        ctx->CSSetShaderResources(0, 3, srv4);
        ctx->CSSetUnorderedAccessViews(0, 1, &g_pOutputUAV, nullptr);
        ctx->Dispatch((w * 2 + 15) / 16, (h * 2 + 15) / 16, 1);
        ctx->CSSetUnorderedAccessViews(0, 1, nullUAV, nullptr);
        ctx->CSSetShaderResources(0, 3, nullSRV);

        return g_pOutputSRV;
    }

    ID3D11ShaderResourceView* Downscale(ID3D11DeviceContext* ctx,
        ID3D11ShaderResourceView* srcSRV, UINT srcW, UINT srcH,
        UINT dstW, UINT dstH)
    {
        if (!g_initialized || !g_pDownscaleCS) return nullptr;

        // Create/resize downscale output texture if needed
        if (dstW != g_downscaleWidth || dstH != g_downscaleHeight) {
            if (!CreateDownscaleTexture(dstW, dstH)) return nullptr;
        }

        // Update constants for downscale pass
        D3D11_MAPPED_SUBRESOURCE m;
        if (SUCCEEDED(ctx->Map(g_pConstantBuffer, 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) {
            Constants* c = (Constants*)m.pData;
            c->inputWidth = srcW; c->inputHeight = srcH;
            c->outputWidth = dstW; c->outputHeight = dstH;
            c->inputPtX = 1.0f / srcW; c->inputPtY = 1.0f / srcH;
            c->outputPtX = 1.0f / dstW; c->outputPtY = 1.0f / dstH;
            ctx->Unmap(g_pConstantBuffer, 0);
        }

        ctx->CSSetConstantBuffers(0, 1, &g_pConstantBuffer);
        ID3D11SamplerState* samplers[] = { g_pPointSampler, g_pLinearSampler };
        ctx->CSSetSamplers(0, 2, samplers);

        // Run downscale pass
        ctx->CSSetShader(g_pDownscaleCS, nullptr, 0);
        ctx->CSSetShaderResources(0, 1, &srcSRV);
        ctx->CSSetUnorderedAccessViews(0, 1, &g_pDownscaleOutputUAV, nullptr);
        ctx->Dispatch((dstW + 7) / 8, (dstH + 7) / 8, 1);

        // Unbind
        ID3D11UnorderedAccessView* nullUAV = nullptr;
        ID3D11ShaderResourceView* nullSRV = nullptr;
        ctx->CSSetUnorderedAccessViews(0, 1, &nullUAV, nullptr);
        ctx->CSSetShaderResources(0, 1, &nullSRV);

        return g_pDownscaleOutputSRV;
    }

    ID3D11Texture2D* GetUpscaledTexture() { return g_pOutput; }
    ID3D11ShaderResourceView* GetUpscaledSRV() { return g_pOutputSRV; }
    ID3D11Texture2D* GetDownscaledTexture() { return g_pDownscaleOutput; }
    ID3D11ShaderResourceView* GetDownscaledSRV() { return g_pDownscaleOutputSRV; }
    bool IsAvailable() { return g_initialized; }
    bool IsDownscaleAvailable() { return g_initialized && g_pDownscaleCS != nullptr; }
}
