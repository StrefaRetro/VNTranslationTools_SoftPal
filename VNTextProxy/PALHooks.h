#pragma once

#include <d3d11.h>

namespace PALGrabCurrentText {
	bool Install();
	const unsigned char* get();
}

namespace PALVideoFix {
	bool Install();
}

namespace DirectShowVideoScale {
	bool Install();

	// DX11 video rendering support
	void InitializeDX11(ID3D11Device* pDevice, ID3D11DeviceContext* pContext);
	void CleanupDX11();
}