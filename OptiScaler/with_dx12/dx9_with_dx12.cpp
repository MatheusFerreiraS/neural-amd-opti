#include "pch.h"
#include "dx9_with_dx12.h"
#include "with_dx12.h"

#include <Config.h>
#include <Logger.h>
#include <State.h>
#include <Util.h>

#include <dlssnr/amd/AmdBridge.h>
#include <dlssnr/amd/PresentExperimental.h>
#include <menu/menu_overlay_base.h>
#include <menu/menu_overlay_dx.h>
#include <menu/menu_overlay_dx9.h>
#include <proxies/XeFG_Proxy.h>

#include <dxgi1_6.h>

using Microsoft::WRL::ComPtr;

namespace
{
constexpr UINT MinimumSize = 64;
constexpr DXGI_FORMAT FrameFormat = DXGI_FORMAT_B8G8R8A8_UNORM;

bool NrWanted()
{
    const auto config = Config::Instance();
    return AmdPresentExperimental::IsTarget() && config->DlssNrEnabled.value_or_default() &&
           config->NrBackend.value_or_default() == "daniel";
}

// Chosen at startup, as on D3D11: changing FGInput or FGOutput needs a restart.
bool PresenterWanted()
{
    const auto& state = State::Instance();
    return AmdPresentExperimental::IsTarget() && state.activeFgInput == FGInput::Upscaler &&
           state.activeFgOutput == FGOutput::XeFG;
}

AmdPreSr::Settings NrSettings()
{
    auto settings =
        DlssNr::AmdBridge::SettingsFromConfig(*Config::Instance(), Config::Instance()->AmdNrScale.value_or_default());
    settings.spinDraw = 0;
    return settings;
}

ComPtr<ID3D12Resource> Buffer(ID3D12Device* device, D3D12_HEAP_TYPE heap, UINT64 size, D3D12_RESOURCE_STATES state)
{
    D3D12_HEAP_PROPERTIES properties {};
    properties.Type = heap;
    D3D12_RESOURCE_DESC desc {};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Width = size;
    desc.Height = desc.DepthOrArraySize = desc.MipLevels = 1;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ComPtr<ID3D12Resource> buffer;
    device->CreateCommittedResource(&properties, D3D12_HEAP_FLAG_NONE, &desc, state, nullptr, IID_PPV_ARGS(&buffer));
    return buffer;
}

// The XeFG guides come from the network's optical flow, so frame generation runs while NR does. This follows the
// D3D11 route's feed (dx11_with_dx12_sc.cpp) and also turns generation off while NR is off.
void FeedFrameGeneration(ID3D12Device* device, UINT width, UINT height, bool ready,
                         const AmdPresentExperimental::Guides& guides)
{
    auto fg = State::Instance().currentFG;

    if (fg == nullptr || State::Instance().activeFgInput != FGInput::Upscaler)
        return;

    const bool enabled = Config::Instance()->FGEnabled.value_or_default();

    if (enabled)
        fg->StartNewFrame();

    if (!ready && fg->IsActive())
        fg->Deactivate();

    FG_Constants constants {};
    constants.flags |= FG_Flags::DisplayResolutionMVs;
    constants.displayWidth = ready ? guides.width : width;
    constants.displayHeight = ready ? guides.height : height;

    if (ready || fg->FrameGenerationContext() == nullptr || !enabled)
        fg->EvaluateState(device, constants);

    if (!ready || !fg->IsActive() || fg->IsPaused())
        return;

    fg->SetCameraValues(0.1f, 1000.0f, 1.5707963f, float(guides.width) / float(guides.height));
    fg->SetMVScale(1.0f, 1.0f);
    fg->SetJitter(0.0f, 0.0f);
    fg->SetReset(guides.reset);
    fg->SetInterpolationRect(guides.width, guides.height);

    Dx12Resource resource {};
    resource.width = guides.width;
    resource.height = guides.height;
    resource.state = D3D12_RESOURCE_STATE_COMMON;
    resource.validity = FG_ResourceValidity::UntilPresent;
    resource.type = FG_ResourceType::Velocity;
    resource.resource = guides.motion.Get();
    const bool motionTagged = fg->SetResource(&resource);
    resource.type = FG_ResourceType::Depth;
    resource.resource = guides.depth.Get();

    if (!motionTagged || !fg->SetResource(&resource))
    {
        LOG_WARN("D3D9 XeFG rejected the motion or depth guides");
        fg->Deactivate();
    }
}

struct Bridge
{
    // The device and window followed. Only compared: the surfaces below hold the device.
    IDirect3DDevice9* game = nullptr;
    HWND window = nullptr;
    ULONGLONG lastPresent = 0;

    UINT width = 0, height = 0;
    D3DFORMAT format = D3DFMT_UNKNOWN, copyFormat = D3DFMT_UNKNOWN;
    bool multisampled = false;

    // staging: a render target for MSAA resolve or format conversion, and the target of the copy back.
    ComPtr<IDirect3DTexture9> stagingTexture;
    ComPtr<IDirect3DSurface9> staging, readSys, writeSys;

    ID3D12Device* device12 = nullptr;
    ID3D12CommandQueue* queue12 = nullptr;
    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList> list;
    ComPtr<ID3D12Fence> fence;
    HANDLE event = nullptr;
    UINT64 serial = 0;
    bool d3d12Failed = false;

    ComPtr<ID3D12Resource> upload, readback, frame;
    BYTE* uploaded = nullptr;
    BYTE* readBack = nullptr;
    UINT pitch = 0;

    ComPtr<IDXGISwapChain4> swap;
    UINT swapWidth = 0, swapHeight = 0, swapFlags = 0;
    bool presenterFailed = false;
    bool fullscreenSaid = false;
    bool tearing = false;

    UINT64 frames = 0;
    bool menuVisible = false;
    bool fgGenerating = false;
    float nrScale = 1.0f;
    double spentCapture = 0, spentNeural = 0, spentBack = 0, spentMenu = 0, spentPresent = 0;
    UINT timed = 0;

    bool Wait()
    {
        if (fence->GetCompletedValue() >= serial)
            return true;

        return SUCCEEDED(fence->SetEventOnCompletion(serial, event)) &&
               WaitForSingleObject(event, 4000) == WAIT_OBJECT_0;
    }

    bool Begin() { return Wait() && SUCCEEDED(allocator->Reset()) && SUCCEEDED(list->Reset(allocator.Get(), nullptr)); }

    bool Submit()
    {
        if (FAILED(list->Close()))
            return false;

        ID3D12CommandList* lists[] = { list.Get() };
        queue12->ExecuteCommandLists(1, lists);
        return SUCCEEDED(queue12->Signal(fence.Get(), ++serial));
    }

    D3D12_TEXTURE_COPY_LOCATION Footprint(ID3D12Resource* buffer) const
    {
        D3D12_TEXTURE_COPY_LOCATION location {};
        location.pResource = buffer;
        location.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        location.PlacedFootprint.Footprint = { FrameFormat, width, height, 1, pitch };
        return location;
    }

    static D3D12_TEXTURE_COPY_LOCATION Subresource(ID3D12Resource* texture)
    {
        D3D12_TEXTURE_COPY_LOCATION location {};
        location.pResource = texture;
        location.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        return location;
    }

    ComPtr<IDXGIAdapter> FindAdapter(IDirect3DDevice9* device)
    {
        ComPtr<IDXGIFactory4> factory;
        ComPtr<IDirect3D9> api;
        D3DDEVICE_CREATION_PARAMETERS creation {};

        if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))) || FAILED(device->GetDirect3D(&api)) ||
            FAILED(device->GetCreationParameters(&creation)))
            return nullptr;

        ComPtr<IDXGIAdapter> adapter;
        ComPtr<IDirect3D9Ex> apiEx;
        LUID luid {};

        if (SUCCEEDED(api.As(&apiEx)) && SUCCEEDED(apiEx->GetAdapterLUID(creation.AdapterOrdinal, &luid)) &&
            SUCCEEDED(factory->EnumAdapterByLuid(luid, IID_PPV_ARGS(&adapter))))
            return adapter;

        const HMONITOR monitor = api->GetAdapterMonitor(creation.AdapterOrdinal);
        ComPtr<IDXGIAdapter1> candidate;

        for (UINT i = 0; factory->EnumAdapters1(i, &candidate) != DXGI_ERROR_NOT_FOUND; ++i)
        {
            ComPtr<IDXGIOutput> output;

            for (UINT j = 0; candidate->EnumOutputs(j, &output) != DXGI_ERROR_NOT_FOUND; ++j)
            {
                DXGI_OUTPUT_DESC desc {};

                if (SUCCEEDED(output->GetDesc(&desc)) && desc.Monitor == monitor)
                    return candidate;
            }
        }

        return nullptr;
    }

    bool EnsureD3D12(IDirect3DDevice9* device)
    {
        if (device12 != nullptr)
            return true;

        if (d3d12Failed)
            return false;

        d3d12Failed = true;
        auto adapter = FindAdapter(device);

        if (WithDx12::RequestD3D12Device(D3D_FEATURE_LEVEL_11_0, adapter.Get()) == nullptr ||
            WithDx12::GetD3D12CommandQueue() == nullptr)
        {
            LOG_ERROR("D3D9 bridge: no D3D12 device, NR and frame generation are off");
            return false;
        }

        device12 = WithDx12::GetD3D12Device();
        queue12 = WithDx12::GetD3D12CommandQueue();
        event = CreateEventW(nullptr, FALSE, FALSE, nullptr);

        if (event == nullptr ||
            FAILED(device12->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator))) ||
            FAILED(device12->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr,
                                               IID_PPV_ARGS(&list))) ||
            FAILED(list->Close()) || FAILED(device12->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence))))
        {
            LOG_ERROR("D3D9 bridge: D3D12 command objects failed");
            device12 = nullptr;
            return false;
        }

        d3d12Failed = false;
        LOG_INFO("D3D9 bridge: D3D12 device {:X}, queue {:X}", (size_t) device12, (size_t) queue12);
        return true;
    }

    void ReleaseFrameObjects()
    {
        if (fence != nullptr)
            Wait();

        staging.Reset();
        stagingTexture.Reset();
        readSys.Reset();
        writeSys.Reset();
        upload.Reset();
        readback.Reset();
        frame.Reset();
        uploaded = readBack = nullptr;
        width = height = 0;
    }

    bool Size(const D3DSURFACE_DESC& desc)
    {
        const bool msaa = desc.MultiSampleType != D3DMULTISAMPLE_NONE;

        if (desc.Width == width && desc.Height == height && desc.Format == format && msaa == multisampled)
            return staging != nullptr;

        ReleaseFrameObjects();
        AmdPresentExperimental::BeforeResize();

        const bool direct = desc.Format == D3DFMT_A8R8G8B8 || desc.Format == D3DFMT_X8R8G8B8;
        copyFormat = direct ? desc.Format : D3DFMT_A8R8G8B8;
        pitch = (desc.Width * 4 + D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1) & ~(D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1);
        upload =
            Buffer(device12, D3D12_HEAP_TYPE_UPLOAD, UINT64(pitch) * desc.Height, D3D12_RESOURCE_STATE_GENERIC_READ);

        if (FAILED(game->CreateTexture(desc.Width, desc.Height, 1, D3DUSAGE_RENDERTARGET, copyFormat, D3DPOOL_DEFAULT,
                                       &stagingTexture, nullptr)) ||
            FAILED(stagingTexture->GetSurfaceLevel(0, &staging)) ||
            FAILED(game->CreateOffscreenPlainSurface(desc.Width, desc.Height, copyFormat, D3DPOOL_SYSTEMMEM, &readSys,
                                                     nullptr)) ||
            upload == nullptr || FAILED(upload->Map(0, nullptr, reinterpret_cast<void**>(&uploaded))))
        {
            LOG_ERROR("D3D9 bridge: copy objects for {}x{} format {} failed", desc.Width, desc.Height,
                      (UINT) desc.Format);
            ReleaseFrameObjects();
            return false;
        }

        width = desc.Width;
        height = desc.Height;
        format = desc.Format;
        multisampled = msaa;
        LOG_INFO("D3D9 bridge: {}x{} format {}, MSAA {}, copies as format {}", width, height, (UINT) format,
                 (UINT) desc.MultiSampleType, (UINT) copyFormat);
        return true;
    }

    bool EnsureWriteBack()
    {
        if (frame != nullptr)
            return true;

        D3D12_HEAP_PROPERTIES properties {};
        properties.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC desc {};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        desc.Width = width;
        desc.Height = height;
        desc.DepthOrArraySize = desc.MipLevels = 1;
        desc.Format = FrameFormat;
        desc.SampleDesc.Count = 1;
        desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
        readback = Buffer(device12, D3D12_HEAP_TYPE_READBACK, UINT64(pitch) * height, D3D12_RESOURCE_STATE_COPY_DEST);

        if (readback == nullptr || FAILED(readback->Map(0, nullptr, reinterpret_cast<void**>(&readBack))) ||
            FAILED(device12->CreateCommittedResource(&properties, D3D12_HEAP_FLAG_NONE, &desc,
                                                     D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&frame))) ||
            FAILED(game->CreateOffscreenPlainSurface(width, height, copyFormat, D3DPOOL_SYSTEMMEM, &writeSys, nullptr)))
        {
            LOG_ERROR("D3D9 bridge: write-back objects failed");
            frame.Reset();
            return false;
        }

        return true;
    }

    // The D3D9 frame into the upload buffer. GetRenderTargetData queues the copy; the lock waits for it.
    bool Capture(IDirect3DSurface9* back)
    {
        IDirect3DSurface9* source = back;

        if (multisampled || copyFormat != format)
        {
            if (FAILED(game->StretchRect(back, nullptr, staging.Get(), nullptr, D3DTEXF_NONE)))
                return false;

            source = staging.Get();
        }

        D3DLOCKED_RECT locked {};

        if (FAILED(game->GetRenderTargetData(source, readSys.Get())) || !Wait() ||
            FAILED(readSys->LockRect(&locked, nullptr, D3DLOCK_READONLY)))
            return false;

        for (UINT y = 0; y < height; ++y)
            memcpy(uploaded + size_t(y) * pitch, static_cast<const BYTE*>(locked.pBits) + size_t(y) * locked.Pitch,
                   size_t(width) * 4);

        readSys->UnlockRect();
        return true;
    }

    // A multisampled backbuffer takes no StretchRect from a plain surface, so the frame is drawn over it.
    bool DrawOver(IDirect3DSurface9* back)
    {
        IDirect3DStateBlock9* saved = nullptr;

        if (FAILED(game->CreateStateBlock(D3DSBT_ALL, &saved)))
            return false;

        IDirect3DSurface9* target = nullptr;
        IDirect3DSurface9* depth = nullptr;
        game->GetRenderTarget(0, &target);
        game->GetDepthStencilSurface(&depth);
        game->SetRenderTarget(0, back);
        game->SetDepthStencilSurface(nullptr);
        game->SetVertexShader(nullptr);
        game->SetPixelShader(nullptr);
        game->SetFVF(D3DFVF_XYZRHW | D3DFVF_TEX1);
        game->SetTexture(0, stagingTexture.Get());
        game->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
        game->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
        game->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
        game->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
        game->SetTextureStageState(0, D3DTSS_TEXCOORDINDEX, 0);
        game->SetTextureStageState(0, D3DTSS_TEXTURETRANSFORMFLAGS, D3DTTFF_DISABLE);
        game->SetTextureStageState(1, D3DTSS_COLOROP, D3DTOP_DISABLE);
        game->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_POINT);
        game->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_POINT);
        game->SetSamplerState(0, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
        game->SetSamplerState(0, D3DSAMP_SRGBTEXTURE, FALSE);

        for (const D3DRENDERSTATETYPE off :
             { D3DRS_ZENABLE, D3DRS_ZWRITEENABLE, D3DRS_ALPHABLENDENABLE, D3DRS_ALPHATESTENABLE, D3DRS_LIGHTING,
               D3DRS_FOGENABLE, D3DRS_STENCILENABLE, D3DRS_SCISSORTESTENABLE, D3DRS_SRGBWRITEENABLE,
               D3DRS_CLIPPLANEENABLE })
            game->SetRenderState(off, FALSE);

        game->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
        game->SetRenderState(D3DRS_FILLMODE, D3DFILL_SOLID);
        game->SetRenderState(D3DRS_COLORWRITEENABLE, 0xF);

        const D3DVIEWPORT9 viewport { 0, 0, width, height, 0.0f, 1.0f };
        const float right = float(width) - 0.5f, bottom = float(height) - 0.5f;
        const float quad[4][6] = { { -0.5f, -0.5f, 0, 1, 0, 0 },
                                   { right, -0.5f, 0, 1, 1, 0 },
                                   { -0.5f, bottom, 0, 1, 0, 1 },
                                   { right, bottom, 0, 1, 1, 1 } };
        game->SetViewport(&viewport);
        const bool drawn = SUCCEEDED(game->BeginScene()) &&
                           SUCCEEDED(game->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, quad, sizeof(quad[0])));
        game->EndScene();

        game->SetRenderTarget(0, target);
        game->SetDepthStencilSurface(depth);

        if (target != nullptr)
            target->Release();

        if (depth != nullptr)
            depth->Release();

        saved->Apply();
        saved->Release();
        return drawn;
    }

    // Without frame generation the network works on a texture of its own and the result goes back to D3D9.
    bool NeuralInPlace(IDirect3DSurface9* back)
    {
        if (!EnsureWriteBack() || !Begin())
            return false;

        auto destination = Subresource(frame.Get());
        auto source = Footprint(upload.Get());
        AmdPresentExperimental::Transition(list.Get(), frame.Get(), D3D12_RESOURCE_STATE_COMMON,
                                           D3D12_RESOURCE_STATE_COPY_DEST);
        list->CopyTextureRegion(&destination, 0, 0, 0, &source, nullptr);
        AmdPresentExperimental::Transition(list.Get(), frame.Get(), D3D12_RESOURCE_STATE_COPY_DEST,
                                           D3D12_RESOURCE_STATE_COMMON);

        if (!Submit())
            return false;

        auto start = Util::MillisecondsNow();

        if (!AmdPresentExperimental::RenderResource(frame.Get(), device12, queue12, Util::DllPath().parent_path(),
                                                    NrSettings()) ||
            !AmdPresentExperimental::LastFrameModified())
        {
            spentNeural += Util::MillisecondsNow() - start;
            return false;
        }

        spentNeural += Util::MillisecondsNow() - start;
        start = Util::MillisecondsNow();

        if (!Begin())
            return false;

        destination = Footprint(readback.Get());
        source = Subresource(frame.Get());
        AmdPresentExperimental::Transition(list.Get(), frame.Get(), D3D12_RESOURCE_STATE_COMMON,
                                           D3D12_RESOURCE_STATE_COPY_SOURCE);
        list->CopyTextureRegion(&destination, 0, 0, 0, &source, nullptr);
        AmdPresentExperimental::Transition(list.Get(), frame.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE,
                                           D3D12_RESOURCE_STATE_COMMON);

        D3DLOCKED_RECT locked {};

        if (!Submit() || !Wait() || FAILED(writeSys->LockRect(&locked, nullptr, 0)))
            return false;

        for (UINT y = 0; y < height; ++y)
            memcpy(static_cast<BYTE*>(locked.pBits) + size_t(y) * locked.Pitch, readBack + size_t(y) * pitch,
                   size_t(width) * 4);

        writeSys->UnlockRect();

        const bool written =
            SUCCEEDED(game->UpdateSurface(writeSys.Get(), nullptr, staging.Get(), nullptr)) &&
            (multisampled ? DrawOver(back)
                          : SUCCEEDED(game->StretchRect(staging.Get(), nullptr, back, nullptr, D3DTEXF_NONE)));
        spentBack += Util::MillisecondsNow() - start;
        return written;
    }

    void ReleasePresenter()
    {
        if (swap == nullptr)
            return;

        LOG_INFO("D3D9 bridge: releasing the XeFG presenter");
        MenuOverlayDx::CleanupRenderTarget(true, window);
        Wait();
        swap.Reset();
        State::Instance().swapchainInteropApi = SwapchainInteropApi::None;
    }

    bool CreatePresenter()
    {
        // Only one ImGui renderer at a time: from here on the DX12 overlay draws the menu on this window.
        MenuOverlayDx9::Shutdown();

        ComPtr<IDXGIFactory5> factory;

        if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))))
        {
            presenterFailed = true;
            return false;
        }

        BOOL allowTearing = FALSE;
        tearing = SUCCEEDED(factory->CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING, &allowTearing,
                                                         sizeof(allowTearing))) &&
                  allowTearing;

        DXGI_SWAP_CHAIN_DESC1 desc {};
        desc.Width = width;
        desc.Height = height;
        desc.Format = FrameFormat;
        desc.SampleDesc.Count = 1;
        desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        desc.BufferCount = 3;
        desc.Scaling = DXGI_SCALING_STRETCH;
        desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
        desc.Flags = tearing ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0;

        // OptiScaler's factory hooks build the XeFG swapchain for a D3D12 queue, as for a D3D12 game.
        ComPtr<IDXGISwapChain1> created;
        const HRESULT result = factory->CreateSwapChainForHwnd(queue12, window, &desc, nullptr, nullptr, &created);

        if (FAILED(result) || created == nullptr ||
            static_cast<IDXGISwapChain*>(created.Get()) != State::Instance().currentFGSwapchain ||
            FAILED(created.As(&swap)))
        {
            LOG_ERROR("D3D9 bridge: no XeFG swapchain ({:X}); NR continues on the D3D9 device", (UINT) result);
            created.Reset();
            swap.Reset();
            presenterFailed = true;
            return false;
        }

        // The game never sees this swapchain: Alt+Enter and mode changes stay with the D3D9 device.
        factory->MakeWindowAssociation(window, DXGI_MWA_NO_WINDOW_CHANGES);
        swapWidth = width;
        swapHeight = height;
        swapFlags = desc.Flags;
        State::Instance().swapchainInteropApi = SwapchainInteropApi::Dx9wDx12;
        LOG_INFO("D3D9 bridge: XeFG presenter {:X} on window {:X}, {}x{}, tearing {}", (size_t) swap.Get(),
                 (size_t) window, width, height, tearing);
        return true;
    }

    bool PresentThroughSwap(IDirect3DSurface9* back, UINT interval)
    {
        if (swapWidth != width || swapHeight != height)
        {
            MenuOverlayDx::CleanupRenderTarget(true, window);
            Wait();
            const HRESULT resized = swap->ResizeBuffers(0, width, height, DXGI_FORMAT_UNKNOWN, swapFlags);
            LOG_INFO("D3D9 bridge: XeFG presenter {}x{} -> {}x{}: {:X}", swapWidth, swapHeight, width, height,
                     (UINT) resized);

            if (FAILED(resized))
            {
                ReleasePresenter();
                presenterFailed = true;
                return false;
            }

            swapWidth = width;
            swapHeight = height;
            State::Instance().scChanged = true;

            if (Config::Instance()->FGEnabled.value_or_default())
            {
                State::Instance().fgResetCapturedResources = true;
                State::Instance().fgOnlyUseCapturedResources = false;
                State::Instance().fgChanged = true;
            }
        }

        auto start = Util::MillisecondsNow();

        // Without the frame the swapchain would show stale buffers: give the window back to D3D9.
        if (!Capture(back))
        {
            LOG_ERROR("D3D9 bridge: the frame could not be read back");
            ReleasePresenter();
            presenterFailed = true;
            return false;
        }

        ComPtr<ID3D12Resource> buffer;

        if (Begin() && SUCCEEDED(swap->GetBuffer(swap->GetCurrentBackBufferIndex(), IID_PPV_ARGS(&buffer))))
        {
            auto destination = Subresource(buffer.Get());
            auto source = Footprint(upload.Get());
            AmdPresentExperimental::Transition(list.Get(), buffer.Get(), D3D12_RESOURCE_STATE_PRESENT,
                                               D3D12_RESOURCE_STATE_COPY_DEST);
            list->CopyTextureRegion(&destination, 0, 0, 0, &source, nullptr);
            AmdPresentExperimental::Transition(list.Get(), buffer.Get(), D3D12_RESOURCE_STATE_COPY_DEST,
                                               D3D12_RESOURCE_STATE_PRESENT);
            Submit();
        }

        buffer.Reset();
        spentCapture += Util::MillisecondsNow() - start;
        start = Util::MillisecondsNow();

        AmdPresentExperimental::Guides guides;
        const bool ready = NrWanted() && AmdPresentExperimental::Render(
                                             swap.Get(), queue12, Util::DllPath().parent_path(), NrSettings(), &guides);
        FeedFrameGeneration(device12, width, height, ready, guides);
        spentNeural += Util::MillisecondsNow() - start;

        start = Util::MillisecondsNow();
        const UINT sync = interval == D3DPRESENT_INTERVAL_IMMEDIATE ? 0 : 1;
        const HRESULT presented = swap->Present(sync, sync == 0 && tearing ? DXGI_PRESENT_ALLOW_TEARING : 0);
        spentPresent += Util::MillisecondsNow() - start;

        if (FAILED(presented))
            LOG_ERROR("D3D9 bridge: XeFG presenter Present failed {:X}", (UINT) presented);

        return true;
    }

    void Follow(IDirect3DDevice9* device, HWND target)
    {
        LOG_INFO("D3D9 bridge: following device {:X}, window {:X}", (size_t) device, (size_t) target);

        if (target != window)
            ReleasePresenter();

        if (device != game)
            MenuOverlayDx9::Shutdown();

        ReleaseFrameObjects();
        game = device;
        window = target;
    }

    void Report()
    {
        const auto fg = State::Instance().currentFG;
        const bool generating = fg != nullptr && fg->IsActive() && !fg->IsPaused();
        const float scale = Config::Instance()->AmdNrScale.value_or_default();

        if (MenuOverlayBase::IsVisible() != menuVisible)
        {
            menuVisible = !menuVisible;
            LOG_INFO("D3D9 bridge frame {}: menu {}", frames, menuVisible ? "open" : "closed");
        }

        if (generating != fgGenerating)
        {
            fgGenerating = generating;
            LOG_INFO("D3D9 bridge frame {}: frame generation {}", frames, generating ? "on" : "off");
        }

        if (scale != nrScale)
        {
            LOG_INFO("D3D9 bridge frame {}: NR scale {:.2f} -> {:.2f}", frames, nrScale, scale);
            nrScale = scale;
        }

        if (++timed < 300 && frames != 1)
            return;

        xefg_swapchain_present_status_t status {};
        const bool queried = swap != nullptr && fg != nullptr && fg->SwapchainContext() != nullptr &&
                             XeFGProxy::GetLastPresentStatus() != nullptr &&
                             XeFGProxy::GetLastPresentStatus()((xefg_swapchain_handle_t) fg->SwapchainContext(),
                                                               &status) == XEFG_SWAPCHAIN_RESULT_SUCCESS;
        LOG_INFO("D3D9 bridge frame {}: {}, NR {} at scale {:.2f}, FG {}, XeFG last present {} frames; average ms "
                 "capture {:.2f}, NR {:.2f}, copy back {:.2f}, menu {:.2f}, present {:.2f}; {}",
                 frames, swap != nullptr ? "XeFG presenter" : "D3D9 present", NrWanted(), scale, generating,
                 queried ? status.framesPresented : 0, spentCapture / timed, spentNeural / timed, spentBack / timed,
                 spentMenu / timed, spentPresent / timed, AmdPresentExperimental::Status());
        spentCapture = spentNeural = spentBack = spentMenu = spentPresent = 0;
        timed = 0;
    }

    bool Present(IDirect3DDevice9* device, IDirect3DSwapChain9* chain, HWND overrideWindow)
    {
        if (device->TestCooperativeLevel() != D3D_OK)
            return false;

        ComPtr<IDirect3DSurface9> back;
        D3DSURFACE_DESC desc {};
        D3DPRESENT_PARAMETERS params {};
        D3DDEVICE_CREATION_PARAMETERS creation {};

        if (FAILED(chain->GetBackBuffer(0, D3DBACKBUFFER_TYPE_MONO, &back)) || FAILED(back->GetDesc(&desc)) ||
            FAILED(chain->GetPresentParameters(&params)) || FAILED(device->GetCreationParameters(&creation)))
            return false;

        if (desc.Width < MinimumSize || desc.Height < MinimumSize)
        {
            static bool said = false;

            if (!said)
                LOG_INFO("D3D9 bridge: skipping a {}x{} backbuffer", desc.Width, desc.Height);

            said = true;
            return false;
        }

        HWND target = overrideWindow         ? overrideWindow
                      : params.hDeviceWindow ? params.hDeviceWindow
                                             : creation.hFocusWindow;
        const auto now = GetTickCount64();

        // One device and window at a time; another one takes over after two quiet seconds.
        if (device != game || target != window)
        {
            if (game != nullptr && now - lastPresent < 2000)
                return false;

            Follow(device, target);
        }

        lastPresent = now;
        ++frames;

        if (State::Instance().currentFeature == nullptr)
        {
            State::Instance().screenWidth = static_cast<float>(desc.Width);
            State::Instance().screenHeight = static_cast<float>(desc.Height);
        }

        // Exclusive fullscreen belongs to the D3D9 device: the presenter comes back with a windowed device.
        if (swap != nullptr && !params.Windowed)
            ReleasePresenter();

        if (PresenterWanted() && !params.Windowed && !fullscreenSaid)
        {
            LOG_WARN("D3D9 bridge: XeFG needs a windowed or borderless D3D9 device");
            fullscreenSaid = true;
        }

        if (PresenterWanted() && params.Windowed && !presenterFailed && EnsureD3D12(device) && Size(desc) &&
            (swap != nullptr || CreatePresenter()))
        {
            const bool presented = PresentThroughSwap(back.Get(), params.PresentationInterval);
            Report();
            return presented;
        }

        State::Instance().swapchainApi = API::DX9;

        if (NrWanted() && EnsureD3D12(device) && Size(desc))
        {
            const auto start = Util::MillisecondsNow();
            const bool captured = Capture(back.Get());
            spentCapture += Util::MillisecondsNow() - start;

            if (captured)
                NeuralInPlace(back.Get());
        }

        const auto start = Util::MillisecondsNow();
        MenuOverlayDx9::Present(device, back.Get(), target);
        spentMenu += Util::MillisecondsNow() - start;
        Report();
        return false;
    }

    void BeforeReset(IDirect3DDevice9* device)
    {
        if (device != game)
            return;

        ReleaseFrameObjects();
        MenuOverlayDx9::BeforeReset();
    }
};

std::mutex mutex;
// Process lifetime: no COM release under the loader lock.
Bridge* bridge = nullptr;
} // namespace

bool Dx9WithDx12::Present(IDirect3DDevice9* device, IDirect3DSwapChain9* chain, HWND overrideWindow)
{
    std::lock_guard lock(mutex);

    if (bridge == nullptr)
        bridge = new Bridge;

    return bridge->Present(device, chain, overrideWindow);
}

void Dx9WithDx12::BeforeReset(IDirect3DDevice9* device)
{
    std::lock_guard lock(mutex);

    if (bridge != nullptr)
        bridge->BeforeReset(device);
}
