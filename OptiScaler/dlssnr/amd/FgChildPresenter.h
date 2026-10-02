#pragma once

#include "PresentExperimental.h"

#include <Config.h>
#include <Logger.h>
#include <State.h>
#include <menu/menu_common.h>
#include <proxies/XeFG_Proxy.h>
#include <proxies/XeFGPacing.h>
#include <proxies/XeLL_Proxy.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <wrl/client.h>

#include <algorithm>
#include <chrono>

// XeFG for routes whose game does not present through DXGI (Vulkan, OpenGL): a D3D12 swapchain in a
// child window covering the game's client area, fed with the final-image NR colour and its
// optical-flow guides.
namespace AmdPresentExperimental
{
inline LRESULT CALLBACK FgWindowProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam)
{
    if (message == WM_NCHITTEST)
        return HTTRANSPARENT;
    if (message == WM_MOUSEACTIVATE)
        return MA_ACTIVATE;
    return DefWindowProcW(window, message, wparam, lparam);
}

struct FgPresenter
{
    // Names the route in every log line, "Vulkan" or "OpenGL".
    const char* api = "";

    static void Log(const char* message, xefg_swapchain_logging_level_t level, void* api)
    {
        if (level >= XEFG_SWAPCHAIN_LOGGING_LEVEL_WARNING)
            LOG_WARN("{} XeFG SDK: {}", static_cast<const char*>(api), message);
    }

    HWND window = nullptr;
    xefg_swapchain_handle_t context = nullptr;
    xell_context_handle_t xell = nullptr;
    ComPtr<IDXGISwapChain3> swap;
    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList> command;
    ComPtr<ID3D12Fence> fence;
    uint64_t serial = 0;
    uint32_t frameId = 0;
    int interpolated = 0;
    int maximum = 0;
    bool enabled = false;
    bool visible = false;
    bool resetOnResume = true;
    std::chrono::steady_clock::time_point previous {};

    void Show(bool show)
    {
        if (!window)
            return;
        const int command = show ? SW_SHOWNA : SW_HIDE;
        if (GetWindowThreadProcessId(window, nullptr) == GetCurrentThreadId())
            ShowWindow(window, command);
        else
            ShowWindowAsync(window, command);
        visible = show;
    }

    void Pause()
    {
        if (context && enabled)
            XeFGProxy::SetEnabled()(context, false);
        enabled = false;
        if (visible)
            Show(false);
        resetOnResume = true;
        previous = {};
    }

    void Release()
    {
        Pause();
        command.Reset();
        allocator.Reset();
        fence.Reset();
        swap.Reset();
        if (context)
            XeFGProxy::Destroy()(context);
        context = nullptr;
        if (xell)
            XeLLProxy::DestroyContext()(xell);
        xell = nullptr;
        if (window && !DestroyWindow(window))
            PostMessageW(window, WM_CLOSE, 0, 0);
        window = nullptr;
        frameId = serial = 0;
        interpolated = maximum = 0;
        enabled = false;
        visible = false;
        resetOnResume = true;
        previous = {};
    }

    bool Create(HWND parent, ID3D12Device* device, ID3D12CommandQueue* queue, uint32_t width, uint32_t height,
                DXGI_FORMAT format)
    {
        if (!parent || !XeFGProxy::InitXeFG() || !XeFGProxy::D3D12CreateContext() || !XeFGProxy::GetProperties() ||
            !XeFGProxy::D3D12InitFromSwapChainDesc() || !XeFGProxy::D3D12GetSwapChainPtr() ||
            !XeFGProxy::D3D12TagFrameResource() || !XeFGProxy::TagFrameConstants() || !XeFGProxy::SetPresentId() ||
            !XeFGProxy::SetEnabled() || !XeFGProxy::GetLastPresentStatus())
            return false;
        static const wchar_t className[] = L"OptiScalerVulkanXeFG";
        static bool registered = []
        {
            WNDCLASSW type {};
            type.lpfnWndProc = FgWindowProc;
            type.hInstance = GetModuleHandleW(nullptr);
            type.lpszClassName = className;
            return RegisterClassW(&type) != 0;
        }();
        if (!registered)
            return false;
        window = CreateWindowExW(WS_EX_TRANSPARENT, className, L"", WS_CHILD | WS_VISIBLE, 0, 0, width, height, parent,
                                 nullptr, GetModuleHandleW(nullptr), nullptr);
        if (!window)
        {
            LOG_ERROR("{} XeFG: child window failed ({})", api, GetLastError());
            return false;
        }
        visible = true;
        const auto created = XeFGProxy::D3D12CreateContext()(device, &context);
        if (created != XEFG_SWAPCHAIN_RESULT_SUCCESS)
        {
            LOG_ERROR("{} XeFG: D3D12CreateContext failed ({})", api, (int) created);
            return false;
        }
        if (XeFGProxy::SetLoggingCallback())
            XeFGProxy::SetLoggingCallback()(context, XEFG_SWAPCHAIN_LOGGING_LEVEL_WARNING, Log, const_cast<char*>(api));
        if (!XeLLProxy::InitXeLL() || !XeLLProxy::D3D12CreateContext() || !XeLLProxy::SetSleepMode() ||
            !XeLLProxy::DestroyContext() || !XeFGProxy::SetLatencyReduction())
        {
            LOG_ERROR("{} XeFG: XeLL functions unavailable", api);
            return false;
        }
        const auto xellCreated = XeLLProxy::D3D12CreateContext()(device, &xell);
        if (xellCreated != XELL_RESULT_SUCCESS)
        {
            LOG_ERROR("{} XeFG: XeLL context failed ({})", api, (int) xellCreated);
            return false;
        }
        xell_sleep_params_t sleep {};
        sleep.bLowLatencyMode = true;
        const auto sleepSet = XeLLProxy::SetSleepMode()(xell, &sleep);
        const auto latencySet = XeFGProxy::SetLatencyReduction()(context, xell);
        if (sleepSet != XELL_RESULT_SUCCESS || latencySet != XEFG_SWAPCHAIN_RESULT_SUCCESS)
        {
            LOG_ERROR("{} XeFG: XeLL setup failed ({}, {})", api, (int) sleepSet, (int) latencySet);
            return false;
        }
        xefg_swapchain_properties_t properties {};
        const auto queried = XeFGProxy::GetProperties()(context, &properties);
        if (queried != XEFG_SWAPCHAIN_RESULT_SUCCESS || properties.maxSupportedInterpolations == 0)
        {
            LOG_ERROR("{} XeFG: GetProperties failed ({}, maximum {})", api, (int) queried,
                      properties.maxSupportedInterpolations);
            return false;
        }
        maximum = static_cast<int>(properties.maxSupportedInterpolations);
        DXGI_SWAP_CHAIN_DESC1 description {};
        description.Width = width;
        description.Height = height;
        description.Format = format;
        description.SampleDesc.Count = 1;
        description.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        description.BufferCount = 3;
        description.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
        description.Scaling = DXGI_SCALING_STRETCH;
        xefg_swapchain_d3d12_init_params_t parameters {};
        parameters.maxInterpolatedFrames = properties.maxSupportedInterpolations;
        ComPtr<IDXGIFactory2> factory;
        const auto factoryResult = CreateDXGIFactory1(IID_PPV_ARGS(&factory));
        if (FAILED(factoryResult))
        {
            LOG_ERROR("{} XeFG: DXGI factory failed ({:X})", api, (UINT) factoryResult);
            return false;
        }
        {
            ScopedVulkanCreatingSC creating;
            const auto initialized = XeFGProxy::D3D12InitFromSwapChainDesc()(context, window, &description, nullptr,
                                                                             queue, factory.Get(), &parameters);
            if (initialized != XEFG_SWAPCHAIN_RESULT_SUCCESS)
            {
                LOG_ERROR("{} XeFG: InitFromSwapChainDesc failed ({})", api, (int) initialized);
                return false;
            }
            const auto obtained = XeFGProxy::D3D12GetSwapChainPtr()(context, IID_PPV_ARGS(&swap));
            if (obtained != XEFG_SWAPCHAIN_RESULT_SUCCESS)
            {
                LOG_ERROR("{} XeFG: GetSwapChainPtr failed ({})", api, (int) obtained);
                return false;
            }
        }
        if (FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator))) ||
            FAILED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr,
                                             IID_PPV_ARGS(&command))) ||
            FAILED(command->Close()) || FAILED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence))))
            return false;
        const DWORD parentThread = GetWindowThreadProcessId(parent, nullptr);
        LOG_INFO("{} XeFG presenter created at {}x{}, maximum {} interpolations; parent thread {}, presenter thread {}",
                 api, width, height, maximum, parentThread, GetCurrentThreadId());
        return true;
    }

    bool Present(ID3D12Resource* colour, const AmdPresentExperimental::Guides& guides, ID3D12CommandQueue* queue)
    {
        if (!swap || !guides.motion || !guides.depth)
            return false;
        const int requested = std::clamp(Config::Instance()->FGXeFGInterpolationCount.value_or(2), 1, maximum);
        if (requested != interpolated)
        {
            if (XeFGProxy::SetNumInterpolatedFrames() &&
                XeFGProxy::SetNumInterpolatedFrames()(context, requested) != XEFG_SWAPCHAIN_RESULT_SUCCESS)
                return false;
            interpolated = requested;
        }
        ComPtr<ID3D12Resource> back;
        if (FAILED(swap->GetBuffer(swap->GetCurrentBackBufferIndex(), IID_PPV_ARGS(&back))) ||
            FAILED(allocator->Reset()) || FAILED(command->Reset(allocator.Get(), nullptr)))
            return false;
        AmdPresentExperimental::Transition(command.Get(), back.Get(), D3D12_RESOURCE_STATE_PRESENT,
                                           D3D12_RESOURCE_STATE_COPY_DEST);
        command->CopyResource(back.Get(), colour);
        AmdPresentExperimental::Transition(command.Get(), back.Get(), D3D12_RESOURCE_STATE_COPY_DEST,
                                           D3D12_RESOURCE_STATE_PRESENT);
        if (FAILED(command->Close()))
            return false;
        ID3D12CommandList* lists[] { command.Get() };
        queue->ExecuteCommandLists(1, lists);
        if (FAILED(queue->Signal(fence.Get(), ++serial)))
            return false;
        if (fence->GetCompletedValue() < serial)
        {
            HANDLE completed = CreateEventW(nullptr, FALSE, FALSE, nullptr);
            if (!completed)
                return false;
            const bool done = SUCCEEDED(fence->SetEventOnCompletion(serial, completed)) &&
                              WaitForSingleObject(completed, 4000) == WAIT_OBJECT_0;
            CloseHandle(completed);
            if (!done)
                return false;
        }
        const uint32_t id = ++frameId;
        auto tag = [&](xefg_swapchain_resource_type_t type, ID3D12Resource* resource)
        {
            xefg_swapchain_d3d12_resource_data_t data {};
            data.type = type;
            data.validity = XEFG_SWAPCHAIN_RV_UNTIL_NEXT_PRESENT;
            data.resourceSize = { guides.width, guides.height };
            data.pResource = resource;
            data.incomingState = D3D12_RESOURCE_STATE_COMMON;
            return XeFGProxy::D3D12TagFrameResource()(context, nullptr, id, &data) == XEFG_SWAPCHAIN_RESULT_SUCCESS;
        };
        if (!tag(XEFG_SWAPCHAIN_RES_MOTION_VECTOR, guides.motion.Get()) ||
            !tag(XEFG_SWAPCHAIN_RES_DEPTH, guides.depth.Get()))
            return false;
        xefg_swapchain_frame_constant_data_t constants {};
        for (int i : { 0, 5, 10, 15 })
            constants.viewMatrix[i] = constants.projectionMatrix[i] = 1.0f;
        constants.motionVectorScaleX = constants.motionVectorScaleY = 1.0f;
        const auto now = std::chrono::steady_clock::now();
        const float elapsed = previous.time_since_epoch().count()
                                  ? std::chrono::duration<float, std::milli>(now - previous).count()
                                  : 16.7f;
        previous = now;
        constants.frameRenderTime = std::clamp(elapsed, 1.0f, 100.0f);
        XeFGPacing::NoteFedFrameTime(constants.frameRenderTime);
        constants.resetHistory = resetOnResume || guides.reset || elapsed > 250.0f;
        if (XeFGProxy::TagFrameConstants()(context, id, &constants) != XEFG_SWAPCHAIN_RESULT_SUCCESS ||
            XeFGProxy::SetPresentId()(context, id) != XEFG_SWAPCHAIN_RESULT_SUCCESS)
            return false;
        if (!enabled)
        {
            if (XeFGProxy::SetEnabled()(context, true) != XEFG_SWAPCHAIN_RESULT_SUCCESS)
                return false;
            enabled = true;
        }
        if (!visible)
            Show(true);
        ScopedVulkanCreatingSC creating;
        if (FAILED(swap->Present(0, 0)))
            return false;
        resetOnResume = false;
        if (id == 2 || id % 30 == 0)
        {
            xefg_swapchain_present_status_t status {};
            if (XeFGProxy::GetLastPresentStatus()(context, &status) == XEFG_SWAPCHAIN_RESULT_SUCCESS)
                LOG_INFO("{} XeFG: frame {}, presented {}, generated {}, result {}, visible {}", api, id,
                         status.framesPresented, status.isFrameGenEnabled, (int) status.frameGenResult,
                         IsWindowVisible(window) != FALSE);
        }
        return true;
    }

    // Shows generated frames in a D3D12 child window over the game's own window. Paused while the
    // menu is open, so the menu drawn on the game's swapchain stays visible, and while the game is
    // in the background. A failure stands frame generation down for the session; NR continues.
    void Step(HWND hwnd, ID3D12Device* device, ID3D12CommandQueue* queue, ID3D12Resource* colour,
              const AmdPresentExperimental::Guides& guides, bool& failed)
    {
        const bool wantFg = Config::Instance()->FGEnabled.value_or_default() &&
                            State::Instance().activeFgInput == FGInput::Upscaler &&
                            State::Instance().activeFgOutput == FGOutput::XeFG;
        const bool menuOpen = MenuCommon::IsVisible();
        const HWND root = GetAncestor(hwnd, GA_ROOT);
        const HWND foreground = GetForegroundWindow();
        const bool background = root && IsWindowVisible(root) && foreground && GetAncestor(foreground, GA_ROOT) != root;
        if (!wantFg || menuOpen || background)
        {
            if (window && (enabled || visible))
            {
                Pause();
                if (menuOpen)
                    LOG_INFO("{} XeFG paused while OptiScaler menu is open", api);
                else if (background)
                    LOG_INFO("{} XeFG paused while game window is in background", api);
            }
            if (!wantFg)
                failed = false;
        }
        if (wantFg && !menuOpen && !background && !failed && guides.motion && guides.depth)
        {
            const auto description = colour->GetDesc();
            if (!window && !Create(hwnd, device, queue, static_cast<uint32_t>(description.Width), description.Height,
                                   description.Format))
            {
                Release();
                failed = true;
                LOG_ERROR("{} XeFG: D3D12 presenter initialization failed", api);
            }
            if (window && !Present(colour, guides, queue))
            {
                Release();
                failed = true;
                LOG_ERROR("{} XeFG: presentation failed; NR will continue", api);
            }
        }
        else if (window && !guides.motion)
            Pause();
    }
};
} // namespace AmdPresentExperimental
