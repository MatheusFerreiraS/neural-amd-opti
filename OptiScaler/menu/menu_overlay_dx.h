#pragma once

#include "SysUtils.h"
#include <d3d11_4.h>
#include <d3d12.h>
#include <dxgi1_6.h>

namespace MenuOverlayDx
{
ID3D12GraphicsCommandList* MenuCommandList();
void CleanupRenderTarget(bool clearQueue, HWND hWnd);
// While held, Present draws nothing and creates no render targets, so a swapchain can be resized from another thread.
void HoldForResize(bool hold);
void Present(IDXGISwapChain* pSwapChain, UINT SyncInterval, UINT Flags,
             const DXGI_PRESENT_PARAMETERS* pPresentParameters, IUnknown* pDevice, HWND hWnd, bool isUWP);
void ApplyThemeStyle();
} // namespace MenuOverlayDx
