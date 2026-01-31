#include "pch.h"

#include <string>
#include <windows.h>
#include <dshow.h>
#include <control.h>
#include <d3d11.h>

#include "SharedConstants.h"
#include "PillarboxedState.h"
#include "BicubicScaler.h"
#include "DX11Video.h"

#pragma comment(lib, "strmiids.lib")

// ISampleGrabber and ISampleGrabberCB interfaces (from deprecated qedit.h)
// These are needed for video frame capture
MIDL_INTERFACE("0579154A-2B53-4994-B0D0-E773148EFF85")
ISampleGrabberCB : public IUnknown
{
public:
    virtual HRESULT STDMETHODCALLTYPE SampleCB(double SampleTime, IMediaSample *pSample) = 0;
    virtual HRESULT STDMETHODCALLTYPE BufferCB(double SampleTime, BYTE *pBuffer, long BufferLen) = 0;
};

MIDL_INTERFACE("6B652FFF-11FE-4fce-92AD-0266B5D7C78F")
ISampleGrabber : public IUnknown
{
public:
    virtual HRESULT STDMETHODCALLTYPE SetOneShot(BOOL OneShot) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetMediaType(const AM_MEDIA_TYPE *pType) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetConnectedMediaType(AM_MEDIA_TYPE *pType) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetBufferSamples(BOOL BufferThem) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetCurrentBuffer(long *pBufferSize, long *pBuffer) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetCurrentSample(IMediaSample **ppSample) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetCallback(ISampleGrabberCB *pCallback, long WhichMethodToCallback) = 0;
};

static const IID IID_ISampleGrabber = { 0x6B652FFF, 0x11FE, 0x4fce, { 0x92, 0xAD, 0x02, 0x66, 0xB5, 0xD7, 0xC7, 0x8F } };
static const IID IID_ISampleGrabberCB = { 0x0579154A, 0x2B53, 0x4994, { 0xB0, 0xD0, 0xE7, 0x73, 0x14, 0x8E, 0xFF, 0x85 } };

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

// DirectShow video scaling for pillarboxed mode
// Hooks IVideoWindow::SetWindowPosition to scale video to match our pillarboxed scaling
// Also captures video frames for DX11 rendering
namespace DirectShowVideoScale
{
    static const GUID LOCAL_CLSID_FilterGraph = { 0xe436ebb3, 0x524f, 0x11ce, { 0x9f, 0x53, 0x00, 0x20, 0xaf, 0x0b, 0xa7, 0x70 } };
    static const GUID LOCAL_CLSID_SampleGrabber = { 0xc1f400a0, 0x3f08, 0x11d3, { 0x9f, 0x0b, 0x00, 0x60, 0x08, 0x03, 0x9e, 0x37 } };
    static const GUID LOCAL_CLSID_NullRenderer = { 0xc1f400a4, 0x3f08, 0x11d3, { 0x9f, 0x0b, 0x00, 0x60, 0x08, 0x03, 0x9e, 0x37 } };

    // Original function pointers
    static HRESULT(WINAPI* oCoCreateInstance)(REFCLSID, LPUNKNOWN, DWORD, REFIID, LPVOID*) = nullptr;
    static HRESULT(STDMETHODCALLTYPE* oGB_QueryInterface)(IGraphBuilder*, REFIID, void**) = nullptr;
    static HRESULT(STDMETHODCALLTYPE* oGB_RenderFile)(IGraphBuilder*, LPCWSTR, LPCWSTR) = nullptr;
    static HRESULT(STDMETHODCALLTYPE* oGB_Render)(IGraphBuilder*, IPin*) = nullptr;
    static HRESULT(STDMETHODCALLTYPE* oGB_AddSourceFilter)(IGraphBuilder*, LPCWSTR, LPCWSTR, IBaseFilter**) = nullptr;
    static HRESULT(STDMETHODCALLTYPE* oVW_SetWindowPosition)(IVideoWindow*, long, long, long, long) = nullptr;
    static HRESULT(STDMETHODCALLTYPE* oVW_put_Visible)(IVideoWindow*, long) = nullptr;
    static HRESULT(STDMETHODCALLTYPE* oMC_Run)(IMediaControl*) = nullptr;
    static HRESULT(STDMETHODCALLTYPE* oMC_Stop)(IMediaControl*) = nullptr;

    // Track hooked interfaces to avoid double-hooking
    static IGraphBuilder* g_pGraphBuilder = nullptr;
    static IVideoWindow* g_pVideoWindow = nullptr;
    static IMediaControl* g_pMediaControl = nullptr;

    // SampleGrabber for video frame capture
    static IBaseFilter* g_pSampleGrabberFilter = nullptr;
    static ISampleGrabber* g_pSampleGrabber = nullptr;
    static IBaseFilter* g_pNullRenderer = nullptr;

    // DX11 resources for video rendering
    static ID3D11Device* g_pD3D11Device = nullptr;
    static ID3D11DeviceContext* g_pD3D11Context = nullptr;
    static ID3D11Texture2D* g_pVideoTexture = nullptr;
    static ID3D11ShaderResourceView* g_pVideoSRV = nullptr;

    // Video state (volatile for cross-thread visibility)
    static volatile bool g_videoPlaying = false;
    static volatile UINT g_videoWidth = 0;
    static volatile UINT g_videoHeight = 0;
    static volatile bool g_frameReady = false;
    static volatile bool g_dx11Initialized = false;
    static CRITICAL_SECTION g_frameLock;
    static bool g_lockInitialized = false;

    // Forward declarations
    static void CopyVideoFrame(const BYTE* pData, UINT width, UINT height, UINT stride);

    // SampleGrabber callback implementation
    class SampleGrabberCallback : public ISampleGrabberCB
    {
    public:
        STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override
        {
            if (riid == IID_IUnknown || riid == IID_ISampleGrabberCB)
            {
                *ppv = static_cast<ISampleGrabberCB*>(this);
                return S_OK;
            }
            *ppv = nullptr;
            return E_NOINTERFACE;
        }

        STDMETHODIMP_(ULONG) AddRef() override { return 2; }
        STDMETHODIMP_(ULONG) Release() override { return 1; }

        STDMETHODIMP SampleCB(double SampleTime, IMediaSample* pSample) override
        {
            return S_OK;  // Not used
        }

        STDMETHODIMP BufferCB(double SampleTime, BYTE* pBuffer, long BufferLen) override
        {
            static int frameCount = 0;
            frameCount++;
            if (frameCount <= 5 || frameCount % 100 == 0)
            {
                dbg_log("BufferCB: frame=%d, time=%.3f, buffer=%p, len=%d, playing=%d, dx11=%d",
                    frameCount, SampleTime, pBuffer, BufferLen, g_videoPlaying ? 1 : 0, g_dx11Initialized ? 1 : 0);
            }

            if (!g_videoPlaying || !pBuffer || BufferLen <= 0 || !g_dx11Initialized)
                return S_OK;

            // Copy frame data (with vertical flip for bottom-up DIBs)
            CopyVideoFrame(pBuffer, g_videoWidth, g_videoHeight, g_videoWidth * 4);

            // Present the video frame via DX11 (since D3D9 Present is not being called during video)
            if (g_frameReady && g_pVideoSRV)
            {
                DX11Video::PresentVideoFrame(g_pVideoSRV, g_videoWidth, g_videoHeight);
            }

            return S_OK;
        }
    };

    static SampleGrabberCallback g_sampleGrabberCallback;

    static void PatchVtable(void** vtable, int index, void* hookFunc, void** originalFunc)
    {
        DWORD oldProtect;
        if (VirtualProtect(&vtable[index], sizeof(void*), PAGE_EXECUTE_READWRITE, &oldProtect))
        {
            if (*originalFunc == nullptr)
                *originalFunc = vtable[index];
            vtable[index] = hookFunc;
            VirtualProtect(&vtable[index], sizeof(void*), oldProtect, &oldProtect);
        }
    }

    static bool CreateVideoTextures(UINT width, UINT height)
    {
        if (!g_pD3D11Device)
            return false;

        // Release old textures
        if (g_pVideoSRV) { g_pVideoSRV->Release(); g_pVideoSRV = nullptr; }
        if (g_pVideoTexture) { g_pVideoTexture->Release(); g_pVideoTexture = nullptr; }

        // Create dynamic texture (CPU writable, shader readable)
        D3D11_TEXTURE2D_DESC desc = {};
        desc.Width = width;
        desc.Height = height;
        desc.MipLevels = 1;
        desc.ArraySize = 1;
        desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        desc.SampleDesc.Count = 1;
        desc.Usage = D3D11_USAGE_DYNAMIC;
        desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;

        HRESULT hr = g_pD3D11Device->CreateTexture2D(&desc, nullptr, &g_pVideoTexture);
        if (FAILED(hr))
        {
            dbg_log("DirectShowVideoScale: Failed to create video texture, hr=0x%x", hr);
            return false;
        }

        // Create shader resource view
        g_pVideoSRV = BicubicScaler::CreateSRV(g_pD3D11Device, g_pVideoTexture);
        if (!g_pVideoSRV)
        {
            dbg_log("DirectShowVideoScale: Failed to create video SRV");
            g_pVideoTexture->Release();
            g_pVideoTexture = nullptr;
            return false;
        }

        dbg_log("DirectShowVideoScale: Created video textures %dx%d", width, height);
        return true;
    }

    static void CopyVideoFrame(const BYTE* pData, UINT width, UINT height, UINT stride)
    {
        static int copyCount = 0;
        copyCount++;

        if (!g_pD3D11Context || !g_pVideoTexture || !pData)
        {
            if (copyCount <= 5)
                dbg_log("CopyVideoFrame: SKIPPED ctx=%p tex=%p data=%p", g_pD3D11Context, g_pVideoTexture, pData);
            return;
        }

        EnterCriticalSection(&g_frameLock);

        D3D11_MAPPED_SUBRESOURCE mapped;
        HRESULT hr = g_pD3D11Context->Map(g_pVideoTexture, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped);
        if (SUCCEEDED(hr))
        {
            // Copy with vertical flip (DirectShow DIBs are bottom-up)
            const BYTE* pSrc = pData + (height - 1) * stride;
            BYTE* pDst = (BYTE*)mapped.pData;

            if (copyCount <= 5)
                dbg_log("CopyVideoFrame: copying %dx%d, stride=%d", width, height, stride);

            for (UINT y = 0; y < height; y++)
            {
                memcpy(pDst, pSrc, width * 4);
                pSrc -= stride;
                pDst += mapped.RowPitch;
            }

            g_pD3D11Context->Unmap(g_pVideoTexture, 0);
            if (!g_frameReady && copyCount <= 5)
                dbg_log("CopyVideoFrame: First frame ready!");
            g_frameReady = true;
        }
        else if (copyCount <= 5)
        {
            dbg_log("CopyVideoFrame: Map failed hr=0x%x", hr);
        }

        LeaveCriticalSection(&g_frameLock);
    }

    static void GetVideoInfoFromGrabber()
    {
        dbg_log("GetVideoInfoFromGrabber: g_pSampleGrabber=%p", g_pSampleGrabber);
        if (!g_pSampleGrabber)
            return;

        AM_MEDIA_TYPE mt = {};
        HRESULT hr = g_pSampleGrabber->GetConnectedMediaType(&mt);
        dbg_log("  GetConnectedMediaType returned 0x%x", hr);

        if (SUCCEEDED(hr))
        {
            dbg_log("  formattype match=%d, pbFormat=%p",
                (mt.formattype == FORMAT_VideoInfo) ? 1 : 0, mt.pbFormat);

            if (mt.formattype == FORMAT_VideoInfo && mt.pbFormat)
            {
                VIDEOINFOHEADER* pVih = (VIDEOINFOHEADER*)mt.pbFormat;
                g_videoWidth = pVih->bmiHeader.biWidth;
                g_videoHeight = abs(pVih->bmiHeader.biHeight);
                dbg_log("  Video dimensions: %dx%d", g_videoWidth, g_videoHeight);

                // Create textures for this video size if DX11 is available
                if (g_dx11Initialized)
                {
                    CreateVideoTextures(g_videoWidth, g_videoHeight);
                }
            }

            if (mt.pbFormat)
                CoTaskMemFree(mt.pbFormat);
            if (mt.pUnk)
                mt.pUnk->Release();
        }
    }

    // Helper to get a pin from a filter
    static IPin* GetPin(IBaseFilter* pFilter, PIN_DIRECTION dir)
    {
        IEnumPins* pEnum = nullptr;
        if (FAILED(pFilter->EnumPins(&pEnum)))
            return nullptr;

        IPin* pPin = nullptr;
        while (pEnum->Next(1, &pPin, nullptr) == S_OK)
        {
            PIN_DIRECTION pinDir;
            pPin->QueryDirection(&pinDir);
            if (pinDir == dir)
            {
                pEnum->Release();
                return pPin;
            }
            pPin->Release();
        }
        pEnum->Release();
        return nullptr;
    }

    // Find video renderer by looking for a filter with video input and no output
    static IBaseFilter* FindVideoRenderer(IGraphBuilder* pGB)
    {
        IEnumFilters* pEnum = nullptr;
        if (FAILED(pGB->EnumFilters(&pEnum)))
            return nullptr;

        IBaseFilter* pFilter = nullptr;
        while (pEnum->Next(1, &pFilter, nullptr) == S_OK)
        {
            // Check if this filter has an input pin connected with video
            IPin* pInputPin = GetPin(pFilter, PINDIR_INPUT);
            if (pInputPin)
            {
                // Check if input is connected
                IPin* pConnected = nullptr;
                if (SUCCEEDED(pInputPin->ConnectedTo(&pConnected)) && pConnected)
                {
                    // Check media type
                    AM_MEDIA_TYPE mt;
                    if (SUCCEEDED(pConnected->ConnectionMediaType(&mt)))
                    {
                        bool isVideo = (mt.majortype == MEDIATYPE_Video);
                        if (mt.pbFormat) CoTaskMemFree(mt.pbFormat);
                        if (mt.pUnk) mt.pUnk->Release();

                        if (isVideo)
                        {
                            // Check if it has no output pin (renderer characteristic)
                            IPin* pOutputPin = GetPin(pFilter, PINDIR_OUTPUT);
                            if (!pOutputPin)
                            {
                                pConnected->Release();
                                pInputPin->Release();
                                pEnum->Release();
                                dbg_log("  Found video renderer filter");
                                return pFilter;
                            }
                            if (pOutputPin) pOutputPin->Release();
                        }
                    }
                    pConnected->Release();
                }
                pInputPin->Release();
            }
            pFilter->Release();
        }
        pEnum->Release();
        return nullptr;
    }

    static bool InsertSampleGrabberIntoGraph(IGraphBuilder* pGB)
    {
        // Only insert SampleGrabber for DX11 mode
        if (!g_dx11Initialized)
        {
            dbg_log("InsertSampleGrabberIntoGraph: DX11 not initialized, skipping");
            return false;
        }

        dbg_log("InsertSampleGrabberIntoGraph: Finding video renderer...");

        // Find the video renderer
        IBaseFilter* pVideoRenderer = FindVideoRenderer(pGB);
        if (!pVideoRenderer)
        {
            dbg_log("  Could not find video renderer");
            return false;
        }

        // Get renderer's input pin
        IPin* pRendererInput = GetPin(pVideoRenderer, PINDIR_INPUT);
        if (!pRendererInput)
        {
            dbg_log("  Could not get renderer input pin");
            pVideoRenderer->Release();
            return false;
        }

        // Get the upstream pin connected to the renderer
        IPin* pUpstreamOutput = nullptr;
        HRESULT hr = pRendererInput->ConnectedTo(&pUpstreamOutput);
        if (FAILED(hr) || !pUpstreamOutput)
        {
            dbg_log("  Could not get upstream pin, hr=0x%x", hr);
            pRendererInput->Release();
            pVideoRenderer->Release();
            return false;
        }

        // Get media type for reconnection
        AM_MEDIA_TYPE connectionMt;
        hr = pRendererInput->ConnectionMediaType(&connectionMt);
        if (FAILED(hr))
        {
            dbg_log("  Could not get connection media type, hr=0x%x", hr);
            pUpstreamOutput->Release();
            pRendererInput->Release();
            pVideoRenderer->Release();
            return false;
        }

        // Log video info
        if (connectionMt.formattype == FORMAT_VideoInfo && connectionMt.pbFormat)
        {
            VIDEOINFOHEADER* pVih = (VIDEOINFOHEADER*)connectionMt.pbFormat;
            g_videoWidth = pVih->bmiHeader.biWidth;
            g_videoHeight = abs(pVih->bmiHeader.biHeight);
            dbg_log("  Video info from connection: %dx%d", g_videoWidth, g_videoHeight);
        }

        dbg_log("  Creating SampleGrabber...");

        // Create sample grabber filter
        hr = oCoCreateInstance(LOCAL_CLSID_SampleGrabber, nullptr, CLSCTX_INPROC_SERVER,
            IID_IBaseFilter, (void**)&g_pSampleGrabberFilter);
        if (FAILED(hr))
        {
            dbg_log("  Failed to create SampleGrabber filter, hr=0x%x", hr);
            if (connectionMt.pbFormat) CoTaskMemFree(connectionMt.pbFormat);
            pUpstreamOutput->Release();
            pRendererInput->Release();
            pVideoRenderer->Release();
            return false;
        }

        hr = g_pSampleGrabberFilter->QueryInterface(IID_ISampleGrabber, (void**)&g_pSampleGrabber);
        if (FAILED(hr))
        {
            dbg_log("  Failed to get ISampleGrabber, hr=0x%x", hr);
            g_pSampleGrabberFilter->Release();
            g_pSampleGrabberFilter = nullptr;
            if (connectionMt.pbFormat) CoTaskMemFree(connectionMt.pbFormat);
            pUpstreamOutput->Release();
            pRendererInput->Release();
            pVideoRenderer->Release();
            return false;
        }

        // Set media type to RGB32
        AM_MEDIA_TYPE grabberMt = {};
        grabberMt.majortype = MEDIATYPE_Video;
        grabberMt.subtype = MEDIASUBTYPE_RGB32;
        grabberMt.formattype = FORMAT_VideoInfo;
        g_pSampleGrabber->SetMediaType(&grabberMt);

        // Configure sample grabber
        g_pSampleGrabber->SetOneShot(FALSE);
        g_pSampleGrabber->SetBufferSamples(FALSE);
        g_pSampleGrabber->SetCallback(&g_sampleGrabberCallback, 1);  // BufferCB

        // Add sample grabber to graph
        hr = pGB->AddFilter(g_pSampleGrabberFilter, L"SampleGrabber");
        if (FAILED(hr))
        {
            dbg_log("  Failed to add SampleGrabber to graph, hr=0x%x", hr);
            g_pSampleGrabber->Release();
            g_pSampleGrabberFilter->Release();
            g_pSampleGrabber = nullptr;
            g_pSampleGrabberFilter = nullptr;
            if (connectionMt.pbFormat) CoTaskMemFree(connectionMt.pbFormat);
            pUpstreamOutput->Release();
            pRendererInput->Release();
            pVideoRenderer->Release();
            return false;
        }

        // Create null renderer
        hr = oCoCreateInstance(LOCAL_CLSID_NullRenderer, nullptr, CLSCTX_INPROC_SERVER,
            IID_IBaseFilter, (void**)&g_pNullRenderer);
        if (FAILED(hr))
        {
            dbg_log("  Failed to create NullRenderer, hr=0x%x", hr);
            // Continue without null renderer - will try to use original renderer
        }
        else
        {
            hr = pGB->AddFilter(g_pNullRenderer, L"NullRenderer");
            if (FAILED(hr))
            {
                dbg_log("  Failed to add NullRenderer to graph, hr=0x%x", hr);
                g_pNullRenderer->Release();
                g_pNullRenderer = nullptr;
            }
        }

        // Now disconnect and reconnect
        dbg_log("  Disconnecting existing connection...");
        hr = pGB->Disconnect(pUpstreamOutput);
        dbg_log("  Disconnect upstream: 0x%x", hr);
        hr = pGB->Disconnect(pRendererInput);
        dbg_log("  Disconnect renderer: 0x%x", hr);

        // Get SampleGrabber pins
        IPin* pGrabberInput = GetPin(g_pSampleGrabberFilter, PINDIR_INPUT);
        IPin* pGrabberOutput = GetPin(g_pSampleGrabberFilter, PINDIR_OUTPUT);

        if (!pGrabberInput || !pGrabberOutput)
        {
            dbg_log("  Could not get SampleGrabber pins");
            if (pGrabberInput) pGrabberInput->Release();
            if (pGrabberOutput) pGrabberOutput->Release();
            if (connectionMt.pbFormat) CoTaskMemFree(connectionMt.pbFormat);
            pUpstreamOutput->Release();
            pRendererInput->Release();
            pVideoRenderer->Release();
            return false;
        }

        // Connect: upstream -> SampleGrabber
        dbg_log("  Connecting upstream -> SampleGrabber...");
        hr = pGB->Connect(pUpstreamOutput, pGrabberInput);
        dbg_log("  Connect result: 0x%x", hr);

        if (SUCCEEDED(hr))
        {
            // Connect: SampleGrabber -> NullRenderer (or original renderer)
            IPin* pFinalInput = nullptr;
            if (g_pNullRenderer)
            {
                pFinalInput = GetPin(g_pNullRenderer, PINDIR_INPUT);
                dbg_log("  Connecting SampleGrabber -> NullRenderer...");
            }
            else
            {
                pFinalInput = pRendererInput;
                pFinalInput->AddRef();
                dbg_log("  Connecting SampleGrabber -> original renderer...");
            }

            if (pFinalInput)
            {
                hr = pGB->Connect(pGrabberOutput, pFinalInput);
                dbg_log("  Connect result: 0x%x", hr);
                pFinalInput->Release();
            }
        }

        // If using NullRenderer successfully, remove original video renderer
        if (SUCCEEDED(hr) && g_pNullRenderer)
        {
            dbg_log("  Removing original video renderer...");
            pGB->RemoveFilter(pVideoRenderer);
        }

        // Create textures for video
        if (SUCCEEDED(hr) && g_videoWidth > 0 && g_videoHeight > 0)
        {
            CreateVideoTextures(g_videoWidth, g_videoHeight);
        }

        pGrabberInput->Release();
        pGrabberOutput->Release();
        if (connectionMt.pbFormat) CoTaskMemFree(connectionMt.pbFormat);
        pUpstreamOutput->Release();
        pRendererInput->Release();
        pVideoRenderer->Release();

        dbg_log("InsertSampleGrabberIntoGraph: %s", SUCCEEDED(hr) ? "SUCCESS" : "FAILED");
        return SUCCEEDED(hr);
    }

    // IVideoWindow::SetWindowPosition hook - scales video position/size in pillarboxed mode
    static HRESULT STDMETHODCALLTYPE VW_SetWindowPosition_Hook(IVideoWindow* pThis, long Left, long Top, long Width, long Height)
    {
        dbg_log("IVideoWindow::SetWindowPosition: %d,%d %dx%d", Left, Top, Width, Height);

        // In DX11 mode, we render video ourselves - hide the video window
        if (g_dx11Initialized && PillarboxedState::g_pillarboxedActive)
        {
            dbg_log("  [DX11] Video window hidden, rendering via DX11");
            // Position window off-screen or at 0,0 with minimal size
            return oVW_SetWindowPosition(pThis, -10000, -10000, 1, 1);
        }

        // DX9 mode: scale video window position
        if (PillarboxedState::g_pillarboxedActive)
        {
            long scaledLeft = PillarboxedState::g_offsetX;
            long scaledTop = PillarboxedState::g_offsetY;
            long scaledWidth = PillarboxedState::g_scaledWidth;
            long scaledHeight = PillarboxedState::g_scaledHeight;
            dbg_log("  [Pillarboxed] Scaled to: %d,%d %dx%d", scaledLeft, scaledTop, scaledWidth, scaledHeight);
            return oVW_SetWindowPosition(pThis, scaledLeft, scaledTop, scaledWidth, scaledHeight);
        }

        return oVW_SetWindowPosition(pThis, Left, Top, Width, Height);
    }

    // IVideoWindow::put_Visible hook - keep video window hidden in DX11 mode
    static HRESULT STDMETHODCALLTYPE VW_put_Visible_Hook(IVideoWindow* pThis, long Visible)
    {
        dbg_log("IVideoWindow::put_Visible: %d", Visible);

        // In DX11 mode, keep video window hidden
        if (g_dx11Initialized && PillarboxedState::g_pillarboxedActive)
        {
            dbg_log("  [DX11] Keeping video window hidden");
            return oVW_put_Visible(pThis, OAFALSE);
        }

        return oVW_put_Visible(pThis, Visible);
    }

    // IMediaControl::Run hook - track video playback start
    static HRESULT STDMETHODCALLTYPE MC_Run_Hook(IMediaControl* pThis)
    {
        dbg_log("IMediaControl::Run called");

        HRESULT hr = oMC_Run(pThis);
        dbg_log("  oMC_Run returned 0x%x", hr);

        if (SUCCEEDED(hr))
        {
            // Get video dimensions from SampleGrabber if available
            GetVideoInfoFromGrabber();

            dbg_log("  After GetVideoInfoFromGrabber: width=%d, height=%d", g_videoWidth, g_videoHeight);

            if (g_videoWidth > 0 && g_videoHeight > 0)
            {
                g_videoPlaying = true;
                g_frameReady = false;
                dbg_log("  Video playback STARTED: %dx%d", g_videoWidth, g_videoHeight);
            }
            else
            {
                dbg_log("  Video dimensions not available, playback not started");
            }
        }

        return hr;
    }

    // IMediaControl::Stop hook - track video playback stop
    static HRESULT STDMETHODCALLTYPE MC_Stop_Hook(IMediaControl* pThis)
    {
        dbg_log("IMediaControl::Stop");

        g_videoPlaying = false;
        g_frameReady = false;

        HRESULT hr = oMC_Stop(pThis);

        // Clean up SampleGrabber resources for next video
        if (g_pSampleGrabber) { g_pSampleGrabber->Release(); g_pSampleGrabber = nullptr; }
        if (g_pSampleGrabberFilter) { g_pSampleGrabberFilter->Release(); g_pSampleGrabberFilter = nullptr; }
        if (g_pNullRenderer) { g_pNullRenderer->Release(); g_pNullRenderer = nullptr; }

        return hr;
    }

    // IGraphBuilder::RenderFile hook - insert SampleGrabber after graph is built
    static HRESULT STDMETHODCALLTYPE GB_RenderFile_Hook(IGraphBuilder* pThis, LPCWSTR lpwstrFile, LPCWSTR lpwstrPlayList)
    {
        dbg_log("IGraphBuilder::RenderFile: %ls", lpwstrFile ? lpwstrFile : L"(null)");

        HRESULT hr = oGB_RenderFile(pThis, lpwstrFile, lpwstrPlayList);
        dbg_log("  RenderFile returned 0x%x", hr);

        // After graph is built, insert SampleGrabber into the video chain
        if (SUCCEEDED(hr) && g_dx11Initialized)
        {
            InsertSampleGrabberIntoGraph(pThis);
        }

        return hr;
    }

    // IGraphBuilder::Render hook - for rendering individual pins
    static HRESULT STDMETHODCALLTYPE GB_Render_Hook(IGraphBuilder* pThis, IPin* pPin)
    {
        dbg_log("IGraphBuilder::Render called");

        HRESULT hr = oGB_Render(pThis, pPin);
        dbg_log("  Render returned 0x%x", hr);

        // After graph is built, insert SampleGrabber into the video chain
        if (SUCCEEDED(hr) && g_dx11Initialized)
        {
            InsertSampleGrabberIntoGraph(pThis);
        }

        return hr;
    }

    // IGraphBuilder::AddSourceFilter hook - for adding source filters
    static HRESULT STDMETHODCALLTYPE GB_AddSourceFilter_Hook(IGraphBuilder* pThis, LPCWSTR lpwstrFileName, LPCWSTR lpwstrFilterName, IBaseFilter** ppFilter)
    {
        dbg_log("IGraphBuilder::AddSourceFilter: %ls", lpwstrFileName ? lpwstrFileName : L"(null)");

        HRESULT hr = oGB_AddSourceFilter(pThis, lpwstrFileName, lpwstrFilterName, ppFilter);
        dbg_log("  AddSourceFilter returned 0x%x", hr);

        return hr;
    }

    static void HookVideoWindow(IVideoWindow* pVW)
    {
        if (g_pVideoWindow) return;
        g_pVideoWindow = pVW;

        void** vtable = *(void***)pVW;
        // IVideoWindow vtable: 39 = SetWindowPosition, 24 = put_Visible
        PatchVtable(vtable, 39, (void*)VW_SetWindowPosition_Hook, (void**)&oVW_SetWindowPosition);
        PatchVtable(vtable, 24, (void*)VW_put_Visible_Hook, (void**)&oVW_put_Visible);
        dbg_log("DirectShowVideoScale: Hooked IVideoWindow");
    }

    static void HookMediaControl(IMediaControl* pMC)
    {
        if (g_pMediaControl) return;
        g_pMediaControl = pMC;

        void** vtable = *(void***)pMC;
        // IMediaControl inherits from IDispatch (not IUnknown directly)
        // IUnknown: 0=QueryInterface, 1=AddRef, 2=Release
        // IDispatch: 3=GetTypeInfoCount, 4=GetTypeInfo, 5=GetIDsOfNames, 6=Invoke
        // IMediaControl: 7=Run, 8=Pause, 9=Stop, 10=GetState, 11=RenderFile...
        PatchVtable(vtable, 7, (void*)MC_Run_Hook, (void**)&oMC_Run);
        PatchVtable(vtable, 9, (void*)MC_Stop_Hook, (void**)&oMC_Stop);
        dbg_log("DirectShowVideoScale: Hooked IMediaControl (Run@7, Stop@9)");
    }

    // IGraphBuilder::QueryInterface hook - catches IVideoWindow and IMediaControl requests
    static HRESULT STDMETHODCALLTYPE GB_QueryInterface_Hook(IGraphBuilder* pThis, REFIID riid, void** ppvObject)
    {
        HRESULT hr = oGB_QueryInterface(pThis, riid, ppvObject);

        if (SUCCEEDED(hr) && ppvObject && *ppvObject)
        {
            if (riid == IID_IVideoWindow)
            {
                HookVideoWindow((IVideoWindow*)*ppvObject);
            }
            else if (riid == IID_IMediaControl)
            {
                HookMediaControl((IMediaControl*)*ppvObject);
            }
        }

        return hr;
    }

    static void HookGraphBuilder(IGraphBuilder* pGB)
    {
        if (g_pGraphBuilder) return;
        g_pGraphBuilder = pGB;

        void** vtable = *(void***)pGB;
        // IFilterGraph vtable (inherited by IGraphBuilder):
        // 0=QueryInterface, 1=AddRef, 2=Release
        // 3=AddFilter, 4=RemoveFilter, 5=EnumFilters, 6=FindFilterByName
        // 7=ConnectDirect, 8=Reconnect, 9=Disconnect, 10=SetDefaultSyncSource
        // IGraphBuilder extends IFilterGraph:
        // 11=Connect, 12=Render, 13=RenderFile, 14=AddSourceFilter
        PatchVtable(vtable, 0, (void*)GB_QueryInterface_Hook, (void**)&oGB_QueryInterface);
        PatchVtable(vtable, 12, (void*)GB_Render_Hook, (void**)&oGB_Render);
        PatchVtable(vtable, 13, (void*)GB_RenderFile_Hook, (void**)&oGB_RenderFile);
        PatchVtable(vtable, 14, (void*)GB_AddSourceFilter_Hook, (void**)&oGB_AddSourceFilter);
        dbg_log("DirectShowVideoScale: Hooked IGraphBuilder (QI@0, Render@12, RenderFile@13, AddSourceFilter@14)");

        // Also hook IMediaControl immediately
        IMediaControl* pMC = nullptr;
        if (SUCCEEDED(pGB->QueryInterface(IID_IMediaControl, (void**)&pMC)))
        {
            HookMediaControl(pMC);
            pMC->Release();
        }
    }

    // CoCreateInstance hook - catches FilterGraph creation
    static HRESULT WINAPI CoCreateInstance_Hook(REFCLSID rclsid, LPUNKNOWN pUnkOuter, DWORD dwClsContext, REFIID riid, LPVOID* ppv)
    {
        HRESULT hr = oCoCreateInstance(rclsid, pUnkOuter, dwClsContext, riid, ppv);

        if (SUCCEEDED(hr) && ppv && *ppv && rclsid == LOCAL_CLSID_FilterGraph)
        {
            dbg_log("DirectShowVideoScale: FilterGraph created");

            // Reset hooked interfaces for new graph
            g_pGraphBuilder = nullptr;
            g_pVideoWindow = nullptr;
            g_pMediaControl = nullptr;

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
        if (!g_lockInitialized)
        {
            InitializeCriticalSection(&g_frameLock);
            g_lockInitialized = true;
        }

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

    // DX11 initialization - called from D3D9Hooks when DX11 is ready
    void InitializeDX11(ID3D11Device* pDevice, ID3D11DeviceContext* pContext)
    {
        dbg_log("DirectShowVideoScale: InitializeDX11");
        g_pD3D11Device = pDevice;
        g_pD3D11Context = pContext;
        g_dx11Initialized = true;
    }

    void CleanupDX11()
    {
        dbg_log("DirectShowVideoScale: CleanupDX11");
        g_videoPlaying = false;
        g_frameReady = false;
        g_dx11Initialized = false;

        if (g_pVideoSRV) { g_pVideoSRV->Release(); g_pVideoSRV = nullptr; }
        if (g_pVideoTexture) { g_pVideoTexture->Release(); g_pVideoTexture = nullptr; }

        g_pD3D11Device = nullptr;
        g_pD3D11Context = nullptr;
    }
}
