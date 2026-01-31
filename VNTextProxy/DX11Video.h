#pragma once

#include <d3d11.h>

namespace DX11Video {
    // Present a video frame via DX11 (called from DirectShow callback thread)
    void PresentVideoFrame(ID3D11ShaderResourceView* pVideoSRV, UINT width, UINT height);
}
