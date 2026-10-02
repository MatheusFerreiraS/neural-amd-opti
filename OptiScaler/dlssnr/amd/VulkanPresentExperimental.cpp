#include "pch.h"
#include "VulkanPresentExperimental.h"
#include "PresentExperimental.h"
#include "AmdBridge.h"

#include <Config.h>
#include <Logger.h>
#include <Util.h>
#include <State.h>
#include <menu/menu_common.h>
#include <proxies/XeFG_Proxy.h>
#include <proxies/XeFGPacing.h>
#include <proxies/XeLL_Proxy.h>
#include <d3d11.h>
#include <d3d12.h>
#include <dxgi1_2.h>
#include <dxgi1_4.h>
#include <wrl/client.h>

#include <mutex>
#include <chrono>
#include <atomic>
#include <future>
#include <thread>
#include <vector>
#include <algorithm>

namespace AmdVkPresent
{
using Microsoft::WRL::ComPtr;
namespace
{
// Set while the XeFG window is shown, read without the bridge lock on every present.
std::atomic<bool> fgLive = false;

LRESULT CALLBACK FgWindowProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam)
{
    if (message == WM_NCHITTEST) return HTTRANSPARENT;
    if (message == WM_MOUSEACTIVATE)
        return MA_NOACTIVATE;
    if (message == WM_DESTROY)
        PostQuitMessage(0);
    return DefWindowProcW(window, message, wparam, lparam);
}

// The XeFG output window is a disabled child of the game window, so mouse input falls through to the
// game and it can never take focus. A child window shares the input queue of its parent's thread;
// it gets a thread of its own that pumps its messages, because the game's present thread often
// never does and its pending messages would then hold back all input of the game window.
struct FgWindow
{
    HWND window = nullptr;
    std::thread thread;

    bool Create(HWND parent, uint32_t width, uint32_t height)
    {
        static const bool registered = []
        {
            WNDCLASSW type {};
            type.lpfnWndProc = FgWindowProc;
            type.hInstance = GetModuleHandleW(nullptr);
            type.lpszClassName = L"OptiScalerVulkanXeFG";
            return RegisterClassW(&type) != 0;
        }();
        if (!registered)
            return false;
        auto created = std::make_shared<std::promise<HWND>>();
        auto result = created->get_future();
        thread = std::thread(
            [created, parent, width, height]
            {
                HWND window = CreateWindowExW(WS_EX_NOPARENTNOTIFY, L"OptiScalerVulkanXeFG", L"",
                                              WS_CHILD | WS_VISIBLE | WS_DISABLED, 0, 0, width, height, parent, nullptr,
                                              GetModuleHandleW(nullptr), nullptr);
                if (!window)
                    LOG_ERROR("Vulkan XeFG: child window failed ({})", GetLastError());
                created->set_value(window);
                MSG message;
                while (window && GetMessageW(&message, nullptr, 0, 0) > 0)
                    DispatchMessageW(&message);
            });
        if (result.wait_for(std::chrono::seconds(2)) != std::future_status::ready)
        {
            LOG_ERROR("Vulkan XeFG: child window creation did not finish");
            thread.detach();
            return false;
        }
        window = result.get();
        return window != nullptr;
    }

    void Destroy()
    {
        if (window)
            PostMessageW(window, WM_CLOSE, 0, 0);
        window = nullptr;
        if (!thread.joinable())
            return;
        if (WaitForSingleObject(thread.native_handle(), 2000) == WAIT_OBJECT_0)
            thread.join();
        else
        {
            LOG_WARN("Vulkan XeFG: child window thread did not exit");
            thread.detach();
        }
    }
};

struct FgPresenter
{
    static void Log(const char* message, xefg_swapchain_logging_level_t level, void*)
    {
        if (level >= XEFG_SWAPCHAIN_LOGGING_LEVEL_WARNING)
            LOG_WARN("Vulkan XeFG SDK: {}", message);
    }

    FgWindow output;
    HWND window = nullptr;
    xefg_swapchain_handle_t context = nullptr;
    xell_context_handle_t xell = nullptr;
    ComPtr<IDXGISwapChain3> swap;
    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList> command;
    ComPtr<ID3D12Fence> fence;
    ComPtr<ID3D12Resource> hudless;
    ComPtr<ID3D12CommandQueue> queue;
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
        if (!window) return;
        ShowWindowAsync(window, show ? SW_SHOWNA : SW_HIDE);
        visible = fgLive = show;
    }

    void Pause()
    {
        if (context && enabled)
            XeFGProxy::SetEnabled()(context, false);
        enabled = false;
        if (visible) Show(false);
        resetOnResume = true;
        previous = {};
    }

    void Release()
    {
        Pause();
        swap.Reset();
        if (context)
            XeFGProxy::Destroy()(context);
        context = nullptr;
        if (xell)
            XeLLProxy::DestroyContext()(xell);
        xell = nullptr;
        // Generated frames still in flight read the hudless copy and the NR guides; D3D12 frees a
        // resource at once, so the queue drains before any of them goes.
        if (queue && fence && SUCCEEDED(queue->Signal(fence.Get(), ++serial)) && fence->GetCompletedValue() < serial)
        {
            HANDLE drained = CreateEventW(nullptr, FALSE, FALSE, nullptr);
            if (drained && SUCCEEDED(fence->SetEventOnCompletion(serial, drained)))
                WaitForSingleObject(drained, 4000);
            if (drained)
                CloseHandle(drained);
        }
        queue.Reset();
        command.Reset();
        allocator.Reset();
        fence.Reset();
        hudless.Reset();
        output.Destroy();
        window = nullptr;
        frameId = serial = 0;
        interpolated = maximum = 0;
        enabled = false;
        visible = false;
        resetOnResume = true;
        previous = {};
    }

    bool Create(HWND parent, ID3D12Device* device, ID3D12CommandQueue* queue,
                uint32_t width, uint32_t height, DXGI_FORMAT format)
    {
        if (!parent || !XeFGProxy::InitXeFG() || !XeFGProxy::D3D12CreateContext() ||
            !XeFGProxy::GetProperties() || !XeFGProxy::D3D12InitFromSwapChainDesc() ||
            !XeFGProxy::D3D12GetSwapChainPtr() || !XeFGProxy::D3D12TagFrameResource() ||
            !XeFGProxy::TagFrameConstants() || !XeFGProxy::SetPresentId() ||
            !XeFGProxy::SetEnabled() || !XeFGProxy::GetLastPresentStatus())
            return false;
        if (!output.Create(parent, width, height))
            return false;
        this->queue = queue;
        window = output.window;
        visible = true;
        const auto created = XeFGProxy::D3D12CreateContext()(device, &context);
        if (created != XEFG_SWAPCHAIN_RESULT_SUCCESS)
        {
            LOG_ERROR("Vulkan XeFG: D3D12CreateContext failed ({})", (int) created);
            return false;
        }
        if (XeFGProxy::SetLoggingCallback())
            XeFGProxy::SetLoggingCallback()(context, XEFG_SWAPCHAIN_LOGGING_LEVEL_WARNING, Log, nullptr);
        if (!XeLLProxy::InitXeLL() || !XeLLProxy::D3D12CreateContext() ||
            !XeLLProxy::SetSleepMode() || !XeLLProxy::DestroyContext() ||
            !XeFGProxy::SetLatencyReduction())
        {
            LOG_ERROR("Vulkan XeFG: XeLL functions unavailable");
            return false;
        }
        const auto xellCreated = XeLLProxy::D3D12CreateContext()(device, &xell);
        if (xellCreated != XELL_RESULT_SUCCESS)
        {
            LOG_ERROR("Vulkan XeFG: XeLL context failed ({})", (int) xellCreated);
            return false;
        }
        xell_sleep_params_t sleep {};
        sleep.bLowLatencyMode = true;
        const auto sleepSet = XeLLProxy::SetSleepMode()(xell, &sleep);
        const auto latencySet = XeFGProxy::SetLatencyReduction()(context, xell);
        if (sleepSet != XELL_RESULT_SUCCESS || latencySet != XEFG_SWAPCHAIN_RESULT_SUCCESS)
        {
            LOG_ERROR("Vulkan XeFG: XeLL setup failed ({}, {})", (int) sleepSet, (int) latencySet);
            return false;
        }
        xefg_swapchain_properties_t properties {};
        const auto queried = XeFGProxy::GetProperties()(context, &properties);
        if (queried != XEFG_SWAPCHAIN_RESULT_SUCCESS || properties.maxSupportedInterpolations == 0)
        {
            LOG_ERROR("Vulkan XeFG: GetProperties failed ({}, maximum {})", (int) queried,
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
        parameters.uiMode = XEFG_SWAPCHAIN_UI_MODE_BACKBUFFER_HUDLESS;
        ComPtr<IDXGIFactory2> factory;
        const auto factoryResult = CreateDXGIFactory1(IID_PPV_ARGS(&factory));
        if (FAILED(factoryResult))
        {
            LOG_ERROR("Vulkan XeFG: DXGI factory failed ({:X})", (UINT) factoryResult);
            return false;
        }
        {
            ScopedVulkanCreatingSC creating;
            const auto initialized = XeFGProxy::D3D12InitFromSwapChainDesc()(
                context, window, &description, nullptr, queue, factory.Get(), &parameters);
            if (initialized != XEFG_SWAPCHAIN_RESULT_SUCCESS)
            {
                LOG_ERROR("Vulkan XeFG: InitFromSwapChainDesc failed ({})", (int) initialized);
                return false;
            }
            const auto obtained = XeFGProxy::D3D12GetSwapChainPtr()(context, IID_PPV_ARGS(&swap));
            if (obtained != XEFG_SWAPCHAIN_RESULT_SUCCESS)
            {
                LOG_ERROR("Vulkan XeFG: GetSwapChainPtr failed ({})", (int) obtained);
                return false;
            }
        }
        // The back buffer carries OptiScaler's menu and overlays and the hudless copy only the game, so
        // XeFG interpolates the game and lays the overlay unwarped over every generated frame.
        if (XeFGProxy::SetUiCompositionState())
        {
            const auto composition =
                XeFGProxy::SetUiCompositionState()(context, XEFG_SWAPCHAIN_UI_COMPOSITION_STATE_ENABLED);
            if (composition != XEFG_SWAPCHAIN_RESULT_SUCCESS)
                LOG_WARN("Vulkan XeFG: UI composition unavailable ({})", (int) composition);
        }
        D3D12_HEAP_PROPERTIES heap { D3D12_HEAP_TYPE_DEFAULT };
        D3D12_RESOURCE_DESC hudlessDesc {};
        hudlessDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        hudlessDesc.Width = width;
        hudlessDesc.Height = height;
        hudlessDesc.DepthOrArraySize = hudlessDesc.MipLevels = 1;
        hudlessDesc.Format = format;
        hudlessDesc.SampleDesc.Count = 1;
        if (FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &hudlessDesc,
                                                   D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&hudless))) ||
            FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator))) ||
            FAILED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr,
                                             IID_PPV_ARGS(&command))) ||
            FAILED(command->Close()) || FAILED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence))))
            return false;
        const DWORD parentThread = GetWindowThreadProcessId(parent, nullptr);
        LOG_INFO("Vulkan XeFG presenter created at {}x{}, maximum {} interpolations; parent thread {}, presenter thread {}",
                 width, height, maximum, parentThread, GetCurrentThreadId());
        return true;
    }

    // `overlaid` is the final image with OptiScaler's overlay drawn on it, `colour` the same image without.
    bool Present(ID3D12Resource* overlaid, ID3D12Resource* colour, const AmdPresentExperimental::Guides& guides,
                 ID3D12CommandQueue* queue)
    {
        if (!swap || !guides.motion || !guides.depth) return false;
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
        command->CopyResource(back.Get(), overlaid);
        AmdPresentExperimental::Transition(command.Get(), back.Get(), D3D12_RESOURCE_STATE_COPY_DEST,
                                            D3D12_RESOURCE_STATE_PRESENT);
        AmdPresentExperimental::Transition(command.Get(), hudless.Get(), D3D12_RESOURCE_STATE_COMMON,
                                           D3D12_RESOURCE_STATE_COPY_DEST);
        command->CopyResource(hudless.Get(), colour);
        AmdPresentExperimental::Transition(command.Get(), hudless.Get(), D3D12_RESOURCE_STATE_COPY_DEST,
                                           D3D12_RESOURCE_STATE_COMMON);
        if (FAILED(command->Close())) return false;
        ID3D12CommandList* lists[] { command.Get() };
        queue->ExecuteCommandLists(1, lists);
        if (FAILED(queue->Signal(fence.Get(), ++serial))) return false;
        if (fence->GetCompletedValue() < serial)
        {
            HANDLE completed = CreateEventW(nullptr, FALSE, FALSE, nullptr);
            if (!completed) return false;
            const bool done = SUCCEEDED(fence->SetEventOnCompletion(serial, completed)) &&
                              WaitForSingleObject(completed, 4000) == WAIT_OBJECT_0;
            CloseHandle(completed);
            if (!done) return false;
        }
        const uint32_t id = ++frameId;
        auto tag = [&](xefg_swapchain_resource_type_t type, ID3D12Resource* resource) {
            xefg_swapchain_d3d12_resource_data_t data {};
            data.type = type;
            data.validity = XEFG_SWAPCHAIN_RV_UNTIL_NEXT_PRESENT;
            data.resourceSize = { guides.width, guides.height };
            data.pResource = resource;
            data.incomingState = D3D12_RESOURCE_STATE_COMMON;
            return XeFGProxy::D3D12TagFrameResource()(context, nullptr, id, &data) ==
                   XEFG_SWAPCHAIN_RESULT_SUCCESS;
        };
        if (!tag(XEFG_SWAPCHAIN_RES_MOTION_VECTOR, guides.motion.Get()) ||
            !tag(XEFG_SWAPCHAIN_RES_DEPTH, guides.depth.Get()) || !tag(XEFG_SWAPCHAIN_RES_HUDLESS_COLOR, hudless.Get()))
            return false;
        xefg_swapchain_frame_constant_data_t constants {};
        for (int i : { 0, 5, 10, 15 })
            constants.viewMatrix[i] = constants.projectionMatrix[i] = 1.0f;
        constants.motionVectorScaleX = constants.motionVectorScaleY = 1.0f;
        const auto now = std::chrono::steady_clock::now();
        const float elapsed = previous.time_since_epoch().count() ?
            std::chrono::duration<float, std::milli>(now - previous).count() : 16.7f;
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
        if (FAILED(swap->Present(0, 0))) return false;
        resetOnResume = false;
        if (id == 2 || id % 30 == 0)
        {
            xefg_swapchain_present_status_t status {};
            if (XeFGProxy::GetLastPresentStatus()(context, &status) == XEFG_SWAPCHAIN_RESULT_SUCCESS)
                LOG_INFO("Vulkan XeFG: frame {}, presented {}, generated {}, result {}, visible {}", id,
                         status.framesPresented, status.isFrameGenEnabled, (int) status.frameGenResult,
                         IsWindowVisible(window) != FALSE);
        }
        return true;
    }
};

struct Bridge
{
    VkInstance instance = VK_NULL_HANDLE;
    VkPhysicalDevice physical = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    VkSwapchainKHR swapchain = VK_NULL_HANDLE;
    VkQueue queue = VK_NULL_HANDLE;
    uint32_t family = UINT32_MAX;
    uint32_t enabledFamily = UINT32_MAX;
    VkExtent2D extent {};
    VkFormat format = VK_FORMAT_UNDEFINED;
    HWND hwnd = nullptr;
    FgPresenter fg;
    bool fgFailed = false;
    std::atomic<bool> fgPending = false;
    uint32_t fgImage = 0;
    std::vector<VkImage> images;
    std::vector<VkSemaphore> ready, overlaid;
    VkCommandPool pool = VK_NULL_HANDLE;
    VkCommandBuffer command = VK_NULL_HANDLE;
    VkFence fence = VK_NULL_HANDLE;
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    void* mapped = nullptr;
    bool copyAllowed = false;
    bool stopped = false;
    bool submitted = false;
    bool gpuInterop = false;
    bool sharedInitialized = false;
    VkImage sharedImage = VK_NULL_HANDLE, overlayImage = VK_NULL_HANDLE;
    VkDeviceMemory sharedMemory = VK_NULL_HANDLE, overlayMemory = VK_NULL_HANDLE;
    bool overlayInitialized = false;
    ComPtr<ID3D12Device> d12;
    ComPtr<ID3D12CommandQueue> queue12;
    ComPtr<ID3D12Resource> shared12, overlay12;
    AmdPresentExperimental::Guides fgGuides;
    ComPtr<ID3D11Device> d11;
    ComPtr<ID3D11DeviceContext> immediate;
    ComPtr<ID3D11Texture2D> colour, readback;

    void Release()
    {
        fg.Release();
        if (device && queue)
            vkQueueWaitIdle(queue);
        if (shared12)
            AmdPresentExperimental::BeforeResize();
        shared12.Reset();
        overlay12.Reset();
        fgGuides = {};
        for (auto image : { sharedImage, overlayImage })
            if (image)
                vkDestroyImage(device, image, nullptr);
        for (auto memory : { sharedMemory, overlayMemory })
            if (memory)
                vkFreeMemory(device, memory, nullptr);
        sharedImage = overlayImage = VK_NULL_HANDLE;
        sharedMemory = overlayMemory = VK_NULL_HANDLE;
        gpuInterop = sharedInitialized = overlayInitialized = fgPending = false;
        if (mapped) vkUnmapMemory(device, memory);
        mapped = nullptr;
        if (buffer) vkDestroyBuffer(device, buffer, nullptr);
        if (memory) vkFreeMemory(device, memory, nullptr);
        for (auto s : ready) if (s) vkDestroySemaphore(device, s, nullptr);
        for (auto s : overlaid)
            if (s)
                vkDestroySemaphore(device, s, nullptr);
        if (fence) vkDestroyFence(device, fence, nullptr);
        if (pool) vkDestroyCommandPool(device, pool, nullptr);
        buffer = VK_NULL_HANDLE; memory = VK_NULL_HANDLE; fence = VK_NULL_HANDLE;
        pool = VK_NULL_HANDLE; command = VK_NULL_HANDLE;
        ready.clear();
        overlaid.clear();
        images.clear();
        readback.Reset(); colour.Reset();
        swapchain = VK_NULL_HANDLE; queue = VK_NULL_HANDLE; submitted = false;
        copyAllowed = false; stopped = false;
        fgFailed = false;
    }

    bool InitD3D11()
    {
        if (!d11)
        {
        VkPhysicalDeviceIDProperties id { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES };
        VkPhysicalDeviceProperties2 properties { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2 };
        properties.pNext = &id;
        vkGetPhysicalDeviceProperties2(physical, &properties);
        if (!id.deviceLUIDValid)
        {
            AmdPresentExperimental::Report("Vulkan NR: GPU has no Windows adapter LUID");
            return false;
        }
        LUID wanted {};
        std::memcpy(&wanted, id.deviceLUID, sizeof(wanted));
        ComPtr<IDXGIFactory1> factory;
        if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) return false;
        ComPtr<IDXGIAdapter1> adapter;
        for (UINT i = 0;; ++i)
        {
            ComPtr<IDXGIAdapter1> candidate;
            if (factory->EnumAdapters1(i, &candidate) == DXGI_ERROR_NOT_FOUND) break;
            DXGI_ADAPTER_DESC1 description {};
            if (SUCCEEDED(candidate->GetDesc1(&description)) &&
                description.AdapterLuid.LowPart == wanted.LowPart &&
                description.AdapterLuid.HighPart == wanted.HighPart)
            {
                adapter = candidate;
                break;
            }
        }
        if (!adapter)
        {
            AmdPresentExperimental::Report("Vulkan NR: matching D3D11 adapter unavailable");
            return false;
        }
        const D3D_FEATURE_LEVEL levels[] { D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0 };
        D3D_FEATURE_LEVEL selected {};
        auto hr = D3D11CreateDevice(adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr,
                                    D3D11_CREATE_DEVICE_BGRA_SUPPORT, levels, 2, D3D11_SDK_VERSION,
                                    &d11, &selected, &immediate);
        if (hr == E_INVALIDARG)
            hr = D3D11CreateDevice(adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr,
                                   D3D11_CREATE_DEVICE_BGRA_SUPPORT, levels + 1, 1, D3D11_SDK_VERSION,
                                   &d11, &selected, &immediate);
        if (FAILED(hr)) return false;
        }
        D3D11_TEXTURE2D_DESC desc {};
        desc.Width = extent.width; desc.Height = extent.height;
        desc.MipLevels = desc.ArraySize = desc.SampleDesc.Count = 1;
        desc.Format = format == VK_FORMAT_B8G8R8A8_UNORM || format == VK_FORMAT_B8G8R8A8_SRGB
                          ? DXGI_FORMAT_B8G8R8A8_UNORM : DXGI_FORMAT_R8G8B8A8_UNORM;
        desc.Usage = D3D11_USAGE_DEFAULT;
        desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        if (FAILED(d11->CreateTexture2D(&desc, nullptr, &colour))) return false;
        desc.Usage = D3D11_USAGE_STAGING;
        desc.BindFlags = 0;
        desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        return SUCCEEDED(d11->CreateTexture2D(&desc, nullptr, &readback));
    }

    bool InitInterop()
    {
        if (!vkGetDeviceProcAddr(device, "vkGetMemoryWin32HandlePropertiesKHR"))
            return false;
        if (!d12)
        {
            VkPhysicalDeviceIDProperties id { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES };
            VkPhysicalDeviceProperties2 properties { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2 };
            properties.pNext = &id;
            vkGetPhysicalDeviceProperties2(physical, &properties);
            if (!id.deviceLUIDValid) return false;
            LUID wanted {};
            std::memcpy(&wanted, id.deviceLUID, sizeof(wanted));
            ComPtr<IDXGIFactory1> factory;
            if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) return false;
            ComPtr<IDXGIAdapter1> adapter;
            for (UINT i = 0;; ++i)
            {
                ComPtr<IDXGIAdapter1> candidate;
                if (factory->EnumAdapters1(i, &candidate) == DXGI_ERROR_NOT_FOUND) break;
                DXGI_ADAPTER_DESC1 description {};
                if (SUCCEEDED(candidate->GetDesc1(&description)) &&
                    description.AdapterLuid.LowPart == wanted.LowPart &&
                    description.AdapterLuid.HighPart == wanted.HighPart)
                { adapter = candidate; break; }
            }
            if (!adapter || FAILED(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0,
                                                    IID_PPV_ARGS(&d12))))
                return false;
            D3D12_COMMAND_QUEUE_DESC queueDesc {};
            queueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
            if (FAILED(d12->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&queue12)))) return false;
        }
        return CreateShared(sharedImage, sharedMemory, shared12);
    }

    // A D3D12 texture of the swapchain size, imported into Vulkan as `target`.
    bool CreateShared(VkImage& target, VkDeviceMemory& targetMemory, ComPtr<ID3D12Resource>& target12)
    {
        auto getHandleProperties = reinterpret_cast<PFN_vkGetMemoryWin32HandlePropertiesKHR>(
            vkGetDeviceProcAddr(device, "vkGetMemoryWin32HandlePropertiesKHR"));
        const bool bgra = format == VK_FORMAT_B8G8R8A8_UNORM || format == VK_FORMAT_B8G8R8A8_SRGB;
        D3D12_HEAP_PROPERTIES heapProperties {};
        heapProperties.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC resourceDesc {};
        resourceDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        resourceDesc.Width = extent.width;
        resourceDesc.Height = extent.height;
        resourceDesc.DepthOrArraySize = resourceDesc.MipLevels = 1;
        resourceDesc.Format = bgra ? DXGI_FORMAT_B8G8R8A8_UNORM : DXGI_FORMAT_R8G8B8A8_UNORM;
        resourceDesc.SampleDesc.Count = 1;
        resourceDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_SIMULTANEOUS_ACCESS |
                             (bgra ? D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET : D3D12_RESOURCE_FLAG_NONE);
        ComPtr<ID3D12Resource> candidate;
        if (FAILED(d12->CreateCommittedResource(&heapProperties, D3D12_HEAP_FLAG_SHARED, &resourceDesc,
                                                D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&candidate))))
            return false;
        HANDLE handle = nullptr;
        if (FAILED(d12->CreateSharedHandle(candidate.Get(), nullptr, GENERIC_ALL, nullptr, &handle)))
            return false;
        VkExternalMemoryImageCreateInfo external { VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO };
        external.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D12_RESOURCE_BIT;
        VkImageCreateInfo imageInfo { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
        imageInfo.pNext = &external;
        imageInfo.imageType = VK_IMAGE_TYPE_2D;
        imageInfo.format = bgra ? VK_FORMAT_B8G8R8A8_UNORM : VK_FORMAT_R8G8B8A8_UNORM;
        imageInfo.extent = { extent.width, extent.height, 1 };
        imageInfo.mipLevels = imageInfo.arrayLayers = 1;
        imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
        imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
        imageInfo.usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        VkImage image = VK_NULL_HANDLE;
        VkDeviceMemory memory = VK_NULL_HANDLE;
        bool success = false;
        do
        {
            if (vkCreateImage(device, &imageInfo, nullptr, &image) != VK_SUCCESS) break;
            VkMemoryWin32HandlePropertiesKHR handleProperties { VK_STRUCTURE_TYPE_MEMORY_WIN32_HANDLE_PROPERTIES_KHR };
            if (getHandleProperties(device, VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D12_RESOURCE_BIT,
                                    handle, &handleProperties) != VK_SUCCESS) break;
            VkMemoryRequirements requirements {};
            vkGetImageMemoryRequirements(device, image, &requirements);
            VkPhysicalDeviceMemoryProperties memoryProperties {};
            vkGetPhysicalDeviceMemoryProperties(physical, &memoryProperties);
            uint32_t type = UINT32_MAX;
            const uint32_t allowed = requirements.memoryTypeBits & handleProperties.memoryTypeBits;
            for (uint32_t i = 0; i < memoryProperties.memoryTypeCount; ++i)
                if ((allowed & (1u << i)) &&
                    (memoryProperties.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT))
                { type = i; break; }
            if (type == UINT32_MAX) break;
            VkMemoryDedicatedAllocateInfo dedicated { VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO };
            dedicated.image = image;
            VkImportMemoryWin32HandleInfoKHR import { VK_STRUCTURE_TYPE_IMPORT_MEMORY_WIN32_HANDLE_INFO_KHR };
            import.pNext = &dedicated;
            import.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D12_RESOURCE_BIT;
            import.handle = handle;
            VkMemoryAllocateInfo allocation { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
            allocation.pNext = &import;
            allocation.allocationSize = requirements.size;
            allocation.memoryTypeIndex = type;
            if (vkAllocateMemory(device, &allocation, nullptr, &memory) != VK_SUCCESS ||
                vkBindImageMemory(device, image, memory, 0) != VK_SUCCESS)
                break;
            success = true;
        } while (false);
        CloseHandle(handle);
        if (!success)
        {
            if (image) vkDestroyImage(device, image, nullptr);
            if (memory) vkFreeMemory(device, memory, nullptr);
            return false;
        }
        target = image;
        targetMemory = memory;
        target12 = candidate;
        return true;
    }

    bool InitVulkan()
    {
        if (extent.width == 0 || extent.height == 0 || extent.width > 16384 || extent.height > 16384 ||
            images.empty() || enabledFamily == UINT32_MAX || !copyAllowed)
            return false;
        family = enabledFamily;
        VkQueue expected = VK_NULL_HANDLE;
        vkGetDeviceQueue(device, family, 0, &expected);
        if (queue != expected)
        {
            AmdPresentExperimental::Report("Vulkan NR: present queue differs from graphics queue");
            return false;
        }
        VkCommandPoolCreateInfo poolInfo { VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
        poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        poolInfo.queueFamilyIndex = family;
        if (vkCreateCommandPool(device, &poolInfo, nullptr, &pool) != VK_SUCCESS) return false;
        VkCommandBufferAllocateInfo alloc { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
        alloc.commandPool = pool; alloc.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; alloc.commandBufferCount = 1;
        if (vkAllocateCommandBuffers(device, &alloc, &command) != VK_SUCCESS) return false;
        VkFenceCreateInfo fenceInfo { VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
        if (vkCreateFence(device, &fenceInfo, nullptr, &fence) != VK_SUCCESS) return false;
        ready.resize(images.size(), VK_NULL_HANDLE);
        overlaid.resize(images.size(), VK_NULL_HANDLE);
        VkSemaphoreCreateInfo semaphoreInfo { VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
        for (auto& s : ready)
            if (vkCreateSemaphore(device, &semaphoreInfo, nullptr, &s) != VK_SUCCESS) return false;
        for (auto& s : overlaid)
            if (vkCreateSemaphore(device, &semaphoreInfo, nullptr, &s) != VK_SUCCESS)
                return false;
        if (InitInterop())
        {
            gpuInterop = true;
            LOG_INFO("Vulkan final-image NR: D3D12 shared GPU image active (no host readback)");
            return true;
        }
        LOG_WARN("Vulkan final-image NR: D3D12 image import unavailable, using host readback fallback");
        VkBufferCreateInfo bufferInfo { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
        bufferInfo.size = VkDeviceSize(extent.width) * extent.height * 4;
        bufferInfo.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        if (vkCreateBuffer(device, &bufferInfo, nullptr, &buffer) != VK_SUCCESS) return false;
        VkMemoryRequirements requirements {};
        vkGetBufferMemoryRequirements(device, buffer, &requirements);
        VkPhysicalDeviceMemoryProperties memoryProperties {};
        vkGetPhysicalDeviceMemoryProperties(physical, &memoryProperties);
        uint32_t type = UINT32_MAX;
        for (uint32_t i = 0; i < memoryProperties.memoryTypeCount; ++i)
            if ((requirements.memoryTypeBits & (1u << i)) &&
                (memoryProperties.memoryTypes[i].propertyFlags &
                 (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) ==
                 (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT))
            { type = i; break; }
        if (type == UINT32_MAX) return false;
        VkMemoryAllocateInfo memoryInfo { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
        memoryInfo.allocationSize = requirements.size; memoryInfo.memoryTypeIndex = type;
        if (vkAllocateMemory(device, &memoryInfo, nullptr, &memory) != VK_SUCCESS ||
            vkBindBufferMemory(device, buffer, memory, 0) != VK_SUCCESS ||
            vkMapMemory(device, memory, 0, bufferInfo.size, 0, &mapped) != VK_SUCCESS)
            return false;
        return InitD3D11();
    }

    void ImageBarrier(VkCommandBuffer cmd, VkImage image, VkImageLayout before, VkImageLayout after,
                      VkAccessFlags source, VkAccessFlags target)
    {
        VkImageMemoryBarrier barrier { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
        barrier.srcAccessMask = source; barrier.dstAccessMask = target;
        barrier.oldLayout = before; barrier.newLayout = after;
        barrier.srcQueueFamilyIndex = barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.image = image;
        barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        barrier.subresourceRange.levelCount = barrier.subresourceRange.layerCount = 1;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                             0, 0, nullptr, 0, nullptr, 1, &barrier);
    }

    bool Begin()
    {
        if (submitted && vkWaitForFences(device, 1, &fence, VK_TRUE, 4'000'000'000ull) != VK_SUCCESS)
            return false;
        submitted = false;
        if (vkResetFences(device, 1, &fence) != VK_SUCCESS ||
            vkResetCommandPool(device, pool, 0) != VK_SUCCESS)
            return false;
        VkCommandBufferBeginInfo begin { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
        begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        return vkBeginCommandBuffer(command, &begin) == VK_SUCCESS;
    }

    void SharedBarrier(VkImageLayout before, VkImageLayout after, uint32_t sourceFamily, uint32_t targetFamily,
                       VkAccessFlags sourceAccess, VkAccessFlags targetAccess, VkPipelineStageFlags sourceStage,
                       VkPipelineStageFlags targetStage, VkImage image = VK_NULL_HANDLE)
    {
        VkImageMemoryBarrier barrier { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
        barrier.srcAccessMask = sourceAccess;
        barrier.dstAccessMask = targetAccess;
        barrier.oldLayout = before;
        barrier.newLayout = after;
        barrier.srcQueueFamilyIndex = sourceFamily;
        barrier.dstQueueFamilyIndex = targetFamily;
        barrier.image = image ? image : sharedImage;
        barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        barrier.subresourceRange.levelCount = barrier.subresourceRange.layerCount = 1;
        vkCmdPipelineBarrier(command, sourceStage, targetStage, 0, 0, nullptr, 0, nullptr, 1, &barrier);
    }

    bool ProcessShared(VkPresentInfoKHR& present)
    {
        const uint32_t imageIndex = present.pImageIndices[0];
        if (imageIndex >= images.size() || !Begin()) return false;
        auto image = images[imageIndex];
        ImageBarrier(command, image, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                     0, VK_ACCESS_TRANSFER_READ_BIT);
        SharedBarrier(sharedInitialized ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_UNDEFINED,
                      VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_QUEUE_FAMILY_EXTERNAL, family,
                      0, VK_ACCESS_TRANSFER_WRITE_BIT,
                      VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
        VkImageCopy region {};
        region.srcSubresource.aspectMask = region.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        region.srcSubresource.layerCount = region.dstSubresource.layerCount = 1;
        region.extent = { extent.width, extent.height, 1 };
        vkCmdCopyImage(command, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                       sharedImage, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
        SharedBarrier(VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL,
                      family, VK_QUEUE_FAMILY_EXTERNAL, VK_ACCESS_TRANSFER_WRITE_BIT, 0,
                      VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);
        ImageBarrier(command, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
                     VK_ACCESS_TRANSFER_READ_BIT, 0);
        if (vkEndCommandBuffer(command) != VK_SUCCESS) return false;
        std::vector<VkPipelineStageFlags> waitStages(present.waitSemaphoreCount,
                                                    VK_PIPELINE_STAGE_TRANSFER_BIT);
        VkSubmitInfo submit { VK_STRUCTURE_TYPE_SUBMIT_INFO };
        submit.waitSemaphoreCount = present.waitSemaphoreCount;
        submit.pWaitSemaphores = present.pWaitSemaphores;
        submit.pWaitDstStageMask = waitStages.empty() ? nullptr : waitStages.data();
        submit.commandBufferCount = 1; submit.pCommandBuffers = &command;
        if (vkQueueSubmit(queue, 1, &submit, fence) != VK_SUCCESS) return false;
        submitted = true;
        if (vkWaitForFences(device, 1, &fence, VK_TRUE, 4'000'000'000ull) != VK_SUCCESS) return false;
        submitted = false;
        sharedInitialized = true;

        auto settings = DlssNr::AmdBridge::SettingsFromConfig(
            *Config::Instance(), Config::Instance()->AmdNrScale.value_or_default());
        settings.spinDraw = 0;
        if (!AmdPresentExperimental::RenderResource(shared12.Get(), d12.Get(), queue12.Get(),
                                                    Util::DllPath().parent_path(), settings, &fgGuides))
            stopped = true;
        static bool loggedGuides = false;
        if (fgGuides.motion && fgGuides.depth && !loggedGuides)
        {
            LOG_INFO("Vulkan XeFG inputs ready: D3D12 colour, motion and depth at {}x{}",
                     fgGuides.width, fgGuides.height);
            loggedGuides = true;
        }
        const bool wantFg = Config::Instance()->FGEnabled.value_or_default() &&
            State::Instance().activeFgInput == FGInput::Upscaler &&
            State::Instance().activeFgOutput == FGOutput::XeFG;
        const HWND root = GetAncestor(hwnd, GA_ROOT);
        const HWND foreground = GetForegroundWindow();
        const bool background = root && IsWindowVisible(root) && foreground &&
            GetAncestor(foreground, GA_ROOT) != root;
        if (!wantFg || background)
        {
            if (fg.window && (fg.enabled || fg.visible))
            {
                fg.Pause();
                LOG_INFO("Vulkan XeFG paused while {}",
                         !wantFg ? "frame generation is off" : "game window is in background");
            }
            if (!wantFg)
                fgFailed = false;
        }
        if (wantFg && !background && !fgFailed && fgGuides.motion && fgGuides.depth)
        {
            if ((!overlay12 && !CreateShared(overlayImage, overlayMemory, overlay12)) ||
                (!fg.window &&
                 !fg.Create(hwnd, d12.Get(), queue12.Get(), extent.width, extent.height, shared12->GetDesc().Format)))
            {
                fg.Release();
                fgFailed = true;
                LOG_ERROR("Vulkan XeFG: D3D12 presenter initialization failed");
            }
            fgPending = fg.window != nullptr;
            fgImage = imageIndex;
        }
        else if (fg.window && !fgGuides.motion)
            fg.Pause();

        if (!Begin()) return false;
        SharedBarrier(VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                      VK_QUEUE_FAMILY_EXTERNAL, family, 0, VK_ACCESS_TRANSFER_READ_BIT,
                      VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
        ImageBarrier(command, image, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                     0, VK_ACCESS_TRANSFER_WRITE_BIT);
        vkCmdCopyImage(command, sharedImage, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                       image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
        ImageBarrier(command, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
                     VK_ACCESS_TRANSFER_WRITE_BIT, 0);
        SharedBarrier(VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL,
                      family, VK_QUEUE_FAMILY_EXTERNAL, VK_ACCESS_TRANSFER_READ_BIT, 0,
                      VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);
        if (vkEndCommandBuffer(command) != VK_SUCCESS) return false;
        VkSubmitInfo output { VK_STRUCTURE_TYPE_SUBMIT_INFO };
        output.commandBufferCount = 1; output.pCommandBuffers = &command;
        output.signalSemaphoreCount = 1; output.pSignalSemaphores = &ready[imageIndex];
        if (vkQueueSubmit(queue, 1, &output, fence) != VK_SUCCESS) return false;
        submitted = true;
        present.waitSemaphoreCount = 1;
        present.pWaitSemaphores = &ready[imageIndex];
        return true;
    }

    // Runs once OptiScaler's overlay is in the swapchain image: hands that image to XeFG as the back
    // buffer and the NR output, still in shared12, as the hudless colour.
    bool PresentGenerated(VkPresentInfoKHR& present)
    {
        fgPending = false;
        const uint32_t imageIndex = present.pImageIndices[0];
        if (imageIndex != fgImage || !Begin())
            return false;
        auto image = images[imageIndex];
        ImageBarrier(command, image, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, 0,
                     VK_ACCESS_TRANSFER_READ_BIT);
        SharedBarrier(overlayInitialized ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_UNDEFINED,
                      VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_QUEUE_FAMILY_EXTERNAL, family, 0,
                      VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                      overlayImage);
        VkImageCopy region {};
        region.srcSubresource.aspectMask = region.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        region.srcSubresource.layerCount = region.dstSubresource.layerCount = 1;
        region.extent = { extent.width, extent.height, 1 };
        vkCmdCopyImage(command, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, overlayImage,
                       VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
        SharedBarrier(VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL, family, VK_QUEUE_FAMILY_EXTERNAL,
                      VK_ACCESS_TRANSFER_WRITE_BIT, 0, VK_PIPELINE_STAGE_TRANSFER_BIT,
                      VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, overlayImage);
        ImageBarrier(command, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
                     VK_ACCESS_TRANSFER_READ_BIT, 0);
        if (vkEndCommandBuffer(command) != VK_SUCCESS)
            return false;
        std::vector<VkPipelineStageFlags> waitStages(present.waitSemaphoreCount, VK_PIPELINE_STAGE_TRANSFER_BIT);
        VkSubmitInfo submit { VK_STRUCTURE_TYPE_SUBMIT_INFO };
        submit.waitSemaphoreCount = present.waitSemaphoreCount;
        submit.pWaitSemaphores = present.pWaitSemaphores;
        submit.pWaitDstStageMask = waitStages.empty() ? nullptr : waitStages.data();
        submit.commandBufferCount = 1;
        submit.pCommandBuffers = &command;
        submit.signalSemaphoreCount = 1;
        submit.pSignalSemaphores = &overlaid[imageIndex];
        if (vkQueueSubmit(queue, 1, &submit, fence) != VK_SUCCESS)
            return false;
        submitted = true;
        present.waitSemaphoreCount = 1;
        present.pWaitSemaphores = &overlaid[imageIndex];
        if (vkWaitForFences(device, 1, &fence, VK_TRUE, 4'000'000'000ull) != VK_SUCCESS)
            return false;
        submitted = false;
        overlayInitialized = true;
        if (!fg.Present(overlay12.Get(), shared12.Get(), fgGuides, queue12.Get()))
        {
            fg.Release();
            fgFailed = true;
            LOG_ERROR("Vulkan XeFG: presentation failed; NR will continue");
        }
        return true;
    }

    bool Process(VkPresentInfoKHR& present)
    {
        if (gpuInterop)
            return ProcessShared(present);
        const uint32_t imageIndex = present.pImageIndices[0];
        if (imageIndex >= images.size()) return false;
        if (!Begin()) return false;
        auto image = images[imageIndex];
        ImageBarrier(command, image, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                     0, VK_ACCESS_TRANSFER_READ_BIT);
        VkBufferImageCopy region {};
        region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        region.imageSubresource.layerCount = 1;
        region.imageExtent = { extent.width, extent.height, 1 };
        vkCmdCopyImageToBuffer(command, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buffer, 1, &region);
        ImageBarrier(command, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
                     VK_ACCESS_TRANSFER_READ_BIT, 0);
        VkBufferMemoryBarrier hostRead { VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER };
        hostRead.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        hostRead.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
        hostRead.srcQueueFamilyIndex = hostRead.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        hostRead.buffer = buffer; hostRead.size = VK_WHOLE_SIZE;
        vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT,
                             0, 0, nullptr, 1, &hostRead, 0, nullptr);
        if (vkEndCommandBuffer(command) != VK_SUCCESS) return false;
        std::vector<VkPipelineStageFlags> waitStages(present.waitSemaphoreCount,
                                                    VK_PIPELINE_STAGE_TRANSFER_BIT);
        VkSubmitInfo submit { VK_STRUCTURE_TYPE_SUBMIT_INFO };
        submit.waitSemaphoreCount = present.waitSemaphoreCount;
        submit.pWaitSemaphores = present.pWaitSemaphores;
        submit.pWaitDstStageMask = waitStages.empty() ? nullptr : waitStages.data();
        submit.commandBufferCount = 1; submit.pCommandBuffers = &command;
        if (vkQueueSubmit(queue, 1, &submit, fence) != VK_SUCCESS) return false;
        submitted = true;
        if (vkWaitForFences(device, 1, &fence, VK_TRUE, 4'000'000'000ull) != VK_SUCCESS) return false;
        submitted = false;

        const UINT pitch = extent.width * 4;
        immediate->UpdateSubresource(colour.Get(), 0, nullptr, mapped, pitch, 0);
        auto settings = DlssNr::AmdBridge::SettingsFromConfig(
            *Config::Instance(), Config::Instance()->AmdNrScale.value_or_default());
        settings.spinDraw = 0;
        if (!AmdPresentExperimental::RenderTexture11(colour.Get(), d11.Get(), Util::DllPath().parent_path(), settings))
            stopped = true;
        immediate->CopyResource(readback.Get(), colour.Get());
        D3D11_MAPPED_SUBRESOURCE result {};
        if (FAILED(immediate->Map(readback.Get(), 0, D3D11_MAP_READ, 0, &result))) return false;
        for (UINT y = 0; y < extent.height; ++y)
            std::memcpy(static_cast<unsigned char*>(mapped) + size_t(y) * pitch,
                        static_cast<unsigned char*>(result.pData) + size_t(y) * result.RowPitch, pitch);
        immediate->Unmap(readback.Get(), 0);

        if (!Begin()) return false;
        VkBufferMemoryBarrier hostWrite { VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER };
        hostWrite.srcAccessMask = VK_ACCESS_HOST_WRITE_BIT;
        hostWrite.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        hostWrite.srcQueueFamilyIndex = hostWrite.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        hostWrite.buffer = buffer; hostWrite.size = VK_WHOLE_SIZE;
        vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_HOST_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                             0, 0, nullptr, 1, &hostWrite, 0, nullptr);
        ImageBarrier(command, image, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                     0, VK_ACCESS_TRANSFER_WRITE_BIT);
        vkCmdCopyBufferToImage(command, buffer, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
        ImageBarrier(command, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
                     VK_ACCESS_TRANSFER_WRITE_BIT, 0);
        if (vkEndCommandBuffer(command) != VK_SUCCESS) return false;
        VkSubmitInfo output { VK_STRUCTURE_TYPE_SUBMIT_INFO };
        output.commandBufferCount = 1; output.pCommandBuffers = &command;
        output.signalSemaphoreCount = 1; output.pSignalSemaphores = &ready[imageIndex];
        if (vkQueueSubmit(queue, 1, &output, fence) != VK_SUCCESS) return false;
        submitted = true;
        present.waitSemaphoreCount = 1;
        present.pWaitSemaphores = &ready[imageIndex];
        return true;
    }
};

std::mutex mutex;
Bridge bridge;
}

void Created(VkInstance instance, VkPhysicalDevice physicalDevice, VkDevice device, HWND hwnd,
             uint32_t graphicsFamily, VkSwapchainKHR swapchain, const VkSwapchainCreateInfoKHR& description)
{
    std::lock_guard lock(mutex);
    const bool sameDevice = bridge.device == device;
    bridge.Release();
    if (!sameDevice)
    {
        bridge.queue12.Reset();
        bridge.d12.Reset();
        bridge.immediate.Reset();
        bridge.d11.Reset();
    }
    bridge.instance = instance; bridge.physical = physicalDevice; bridge.device = device;
    bridge.hwnd = hwnd;
    bridge.enabledFamily = graphicsFamily;
    bridge.swapchain = swapchain; bridge.extent = description.imageExtent;
    bridge.format = description.imageFormat;
    const bool rgba8 = bridge.format == VK_FORMAT_R8G8B8A8_UNORM ||
                       bridge.format == VK_FORMAT_R8G8B8A8_SRGB ||
                       bridge.format == VK_FORMAT_B8G8R8A8_UNORM ||
                       bridge.format == VK_FORMAT_B8G8R8A8_SRGB;
    bridge.copyAllowed = rgba8 && (description.imageUsage &
        (VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT)) ==
        (VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT);
    LOG_INFO("Vulkan final-image bridge: swapchain {}x{}, format {}, transfer {}",
             bridge.extent.width, bridge.extent.height, (int) bridge.format, bridge.copyAllowed);
    uint32_t count = 0;
    if (vkGetSwapchainImagesKHR(device, swapchain, &count, nullptr) == VK_SUCCESS && count > 0)
    {
        bridge.images.resize(count);
        if (vkGetSwapchainImagesKHR(device, swapchain, &count, bridge.images.data()) != VK_SUCCESS)
            bridge.images.clear();
    }
}

void Destroyed(VkDevice device, VkSwapchainKHR swapchain)
{
    std::lock_guard lock(mutex);
    if (bridge.device == device && bridge.swapchain == swapchain) bridge.Release();
}

bool Process(VkQueue queue, VkPresentInfoKHR& present)
{
    auto config = Config::Instance();
    if (!config->DlssNrEnabled.value_or_default() || !config->DlssNrPresent.value_or_default() ||
        config->NrBackend.value_or_default() != "daniel" || present.swapchainCount != 1 ||
        State::Instance().swapchainApi != API::Vulkan)
    {
        // XeFG shows the last frame it was given until it is paused.
        if (fgLive)
        {
            std::lock_guard lock(mutex);
            bridge.fg.Pause();
            LOG_INFO("Vulkan XeFG paused while final-image NR is off");
        }
        return true;
    }
    static std::once_flag logged;
    std::call_once(logged, [&] {
        LOG_INFO("Vulkan final-image bridge: enabled {}, final image {}, backend {}, API {}",
                 config->DlssNrEnabled.value_or_default(), config->DlssNrPresent.value_or_default(),
                 config->NrBackend.value_or_default(), (int) State::Instance().swapchainApi);
    });
    std::lock_guard lock(mutex);
    bridge.fgPending = false;
    if (bridge.swapchain != present.pSwapchains[0] || bridge.stopped)
    {
        if (bridge.fg.enabled || bridge.fg.visible)
            bridge.fg.Pause();
        return true;
    }
    bridge.queue = queue;
    if (!bridge.pool && !bridge.InitVulkan())
    {
        bridge.stopped = true;
        AmdPresentExperimental::Report("Vulkan NR: bridge initialization failed");
        return true;
    }
    const auto start = std::chrono::steady_clock::now();
    if (!bridge.Process(present))
    {
        bridge.stopped = true;
        AmdPresentExperimental::Report("Vulkan NR: copy or synchronization failed; restart required");
        return false;
    }
    static uint64_t totalMicros = 0;
    static unsigned measured = 0;
    totalMicros += static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now() - start).count());
    if (++measured == 120)
    {
        LOG_INFO("Vulkan final-image bridge: {} average {:.1f} ms over 120 presents",
                 bridge.gpuInterop ? "shared D3D12 image" : "host readback", totalMicros / 120000.0);
        measured = 0;
        totalMicros = 0;
    }
    static std::once_flag copiedOnce;
    std::call_once(copiedOnce, [&] {
        LOG_INFO("Vulkan final-image bridge: first frame processed using {}",
                 bridge.gpuInterop ? "shared D3D12 image" : "host readback");
    });
    return true;
}

bool PresentGenerated(VkPresentInfoKHR& present)
{
    if (!bridge.fgPending)
        return true;
    std::lock_guard lock(mutex);
    if (!bridge.fgPending || bridge.swapchain != present.pSwapchains[0])
        return true;
    if (bridge.PresentGenerated(present))
        return true;
    bridge.stopped = true;
    bridge.fg.Release();
    AmdPresentExperimental::Report("Vulkan NR: copy or synchronization failed; restart required");
    return false;
}
} // namespace AmdVkPresent
