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
// Final-image NR on a native D3D12 game's frame at the frame generation swapchain's Present, before
// frame generation reads it, and the optical-flow motion and depth for XeFG.
void FinalImageFrameGen(IDXGISwapChain* fgSwapChain);
void ApplyThemeStyle();
} // namespace MenuOverlayDx
