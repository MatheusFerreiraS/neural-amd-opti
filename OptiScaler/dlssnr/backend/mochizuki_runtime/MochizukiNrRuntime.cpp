// MochizukiNrRuntime.dll: mochizuki0323's Vulkan network (third_party/mochizuki) behind the lmxxf C ABI.
//
// The network runs on a Vulkan device of its own, on the game's adapter. Before the cut, the game's colour and
// motion vectors are copied into D3D12 buffers that Vulkan imports. Between the two halves of the game's command
// list, a Vulkan submit waits on the game queue's shared fence, runs the network and writes the result into a third
// shared buffer, and the game queue waits for it. After the cut, the result is copied into the texture handed to
// Super Resolution.
#include "LmxxfNrApi.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d12.h>
#define VK_USE_PLATFORM_WIN32_KHR
#include <vulkan/vulkan.h>

#include "nr_log.hpp"
#include "nr_runtime.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <iterator>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace
{
thread_local char g_lastError[256] = {};

void SetError(const char* text)
{
    std::strncpy(g_lastError, text ? text : "", sizeof(g_lastError) - 1);
    g_lastError[sizeof(g_lastError) - 1] = 0;
}

int32_t Fail(int32_t status, const char* text)
{
    SetError(text);
    return status;
}

std::wstring DllDirectory()
{
    HMODULE mod = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCWSTR>(&LmxxfNrGetApi), &mod);
    wchar_t path[MAX_PATH] {};
    if (!mod || !GetModuleFileNameW(mod, path, MAX_PATH))
        return {};
    std::wstring dir(path);
    dir.resize(dir.find_last_of(L"\\/"));
    return dir;
}

// The runtime's own lines and the network's, in mochizuki_nr.log beside the DLL.
void LogLine(const char* line)
{
    static std::mutex mutex;
    static const std::wstring path = DllDirectory() + L"\\mochizuki_nr.log";
    std::lock_guard lock(mutex);
    FILE* f = _wfopen(path.c_str(), L"ab");
    if (!f)
        return;
    SYSTEMTIME t {};
    GetLocalTime(&t);
    std::fprintf(f, "%02u:%02u:%02u.%03u %s\r\n", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds, line);
    std::fclose(f);
}

void Check(HRESULT hr, const char* what)
{
    if (FAILED(hr))
    {
        char text[160];
        std::snprintf(text, sizeof text, "%s failed, HRESULT 0x%08lX", what, static_cast<unsigned long>(hr));
        throw std::runtime_error(text);
    }
}

void VkCheck(VkResult r, const char* what)
{
    if (r != VK_SUCCESS)
        throw std::runtime_error(std::string(what) + " failed, VkResult " + std::to_string(int(r)));
}

struct Format
{
    VkFormat vk = VK_FORMAT_UNDEFINED;
    UINT bytes = 0;
};

// The Vulkan format with the same bits as the D3D12 one, so a buffer copy carries the texels unchanged.
Format ColourFormat(DXGI_FORMAT f)
{
    switch (f)
    {
    case DXGI_FORMAT_R16G16B16A16_FLOAT:
    case DXGI_FORMAT_R16G16B16A16_TYPELESS:
        return { VK_FORMAT_R16G16B16A16_SFLOAT, 8 };
    case DXGI_FORMAT_R32G32B32A32_FLOAT:
    case DXGI_FORMAT_R32G32B32A32_TYPELESS:
        return { VK_FORMAT_R32G32B32A32_SFLOAT, 16 };
    case DXGI_FORMAT_R11G11B10_FLOAT:
        return { VK_FORMAT_B10G11R11_UFLOAT_PACK32, 4 };
    case DXGI_FORMAT_R10G10B10A2_UNORM:
    case DXGI_FORMAT_R10G10B10A2_TYPELESS:
        return { VK_FORMAT_A2B10G10R10_UNORM_PACK32, 4 };
    case DXGI_FORMAT_R8G8B8A8_UNORM:
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
    case DXGI_FORMAT_R8G8B8A8_TYPELESS:
        return { VK_FORMAT_R8G8B8A8_UNORM, 4 };
    case DXGI_FORMAT_B8G8R8A8_UNORM:
    case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
    case DXGI_FORMAT_B8G8R8A8_TYPELESS:
        return { VK_FORMAT_B8G8R8A8_UNORM, 4 };
    default:
        return {};
    }
}

// The runtime blits the vectors into its own RG32F image, so any float format with at least two channels will do;
// only red and green are read.
Format MotionFormat(DXGI_FORMAT f)
{
    switch (f)
    {
    case DXGI_FORMAT_R16G16_FLOAT:
    case DXGI_FORMAT_R16G16_TYPELESS:
        return { VK_FORMAT_R16G16_SFLOAT, 4 };
    case DXGI_FORMAT_R32G32_FLOAT:
    case DXGI_FORMAT_R32G32_TYPELESS:
        return { VK_FORMAT_R32G32_SFLOAT, 8 };
    case DXGI_FORMAT_R16G16B16A16_FLOAT:
    case DXGI_FORMAT_R16G16B16A16_TYPELESS:
        return { VK_FORMAT_R16G16B16A16_SFLOAT, 8 };
    case DXGI_FORMAT_R32G32B32A32_FLOAT:
    case DXGI_FORMAT_R32G32B32A32_TYPELESS:
        return { VK_FORMAT_R32G32B32A32_SFLOAT, 16 };
    default:
        return {};
    }
}

// Scene-referred float colour: the network sees it through the runtime's sRGB encode.
bool IsLinear(VkFormat f)
{
    return f == VK_FORMAT_R16G16B16A16_SFLOAT || f == VK_FORMAT_B10G11R11_UFLOAT_PACK32 ||
           f == VK_FORMAT_R32G32B32A32_SFLOAT;
}

struct Vulkan
{
    VkInstance instance {};
    VkPhysicalDevice physical {};
    VkDevice device {};
    VkQueue queue {};
    uint32_t family = 0;
    VkPhysicalDeviceMemoryProperties memory {};
    PFN_vkGetMemoryWin32HandlePropertiesKHR memoryHandleProperties {};
    PFN_vkImportSemaphoreWin32HandleKHR importSemaphore {};
    std::string name;

    void Create(LUID luid)
    {
        VkApplicationInfo app { VK_STRUCTURE_TYPE_APPLICATION_INFO };
        app.pApplicationName = "OptiScaler NR";
        app.apiVersion = VK_API_VERSION_1_3;
        VkInstanceCreateInfo ii { VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO };
        ii.pApplicationInfo = &app;
        VkCheck(vkCreateInstance(&ii, nullptr, &instance), "vkCreateInstance");

        uint32_t count = 0;
        vkEnumeratePhysicalDevices(instance, &count, nullptr);
        std::vector<VkPhysicalDevice> all(count);
        vkEnumeratePhysicalDevices(instance, &count, all.data());
        for (VkPhysicalDevice p : all)
        {
            VkPhysicalDeviceIDProperties id { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES };
            VkPhysicalDeviceProperties2 props { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, &id };
            vkGetPhysicalDeviceProperties2(p, &props);
            if (id.deviceLUIDValid && !std::memcmp(id.deviceLUID, &luid, sizeof luid))
            {
                physical = p;
                name = props.properties.deviceName;
                break;
            }
        }
        if (!physical)
            throw std::runtime_error("no Vulkan device matches the game's D3D12 adapter");

        vkGetPhysicalDeviceQueueFamilyProperties(physical, &count, nullptr);
        std::vector<VkQueueFamilyProperties> families(count);
        vkGetPhysicalDeviceQueueFamilyProperties(physical, &count, families.data());
        family = count;
        for (uint32_t i = 0; i < count; ++i)
            if ((families[i].queueFlags & (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT)) ==
                (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT))
            {
                family = i;
                break;
            }
        if (family == count)
            throw std::runtime_error("no graphics and compute queue family");

        // What nrvk::Context::create enables, plus the timeline semaphore the game's fence is imported as.
        VkPhysicalDeviceCooperativeMatrixFeaturesKHR coop {
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_COOPERATIVE_MATRIX_FEATURES_KHR
        };
        VkPhysicalDeviceShaderFloat8FeaturesEXT fp8 { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_FLOAT8_FEATURES_EXT };
        VkPhysicalDeviceVulkan11Features f11 { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES };
        VkPhysicalDeviceVulkan12Features f12 { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES };
        VkPhysicalDeviceVulkan13Features f13 { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES };
        VkPhysicalDeviceWorkgroupMemoryExplicitLayoutFeaturesKHR wml {
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_WORKGROUP_MEMORY_EXPLICIT_LAYOUT_FEATURES_KHR
        };
        coop.cooperativeMatrix = VK_TRUE;
        fp8.shaderFloat8 = fp8.shaderFloat8CooperativeMatrix = VK_TRUE;
        f11.storageBuffer16BitAccess = VK_TRUE;
        f12.storageBuffer8BitAccess = f12.shaderFloat16 = f12.shaderInt8 = f12.vulkanMemoryModel = VK_TRUE;
        f12.timelineSemaphore = VK_TRUE;
        f13.subgroupSizeControl = f13.synchronization2 = VK_TRUE;
        wml.workgroupMemoryExplicitLayout = wml.workgroupMemoryExplicitLayout8BitAccess =
            wml.workgroupMemoryExplicitLayout16BitAccess = VK_TRUE;
        coop.pNext = &fp8;
        fp8.pNext = &f11;
        f11.pNext = &f12;
        f12.pNext = &f13;
        f13.pNext = &wml;
        const char* extensions[] = { VK_KHR_COOPERATIVE_MATRIX_EXTENSION_NAME, VK_EXT_SHADER_FLOAT8_EXTENSION_NAME,
                                     VK_KHR_WORKGROUP_MEMORY_EXPLICIT_LAYOUT_EXTENSION_NAME,
                                     VK_KHR_EXTERNAL_MEMORY_WIN32_EXTENSION_NAME,
                                     VK_KHR_EXTERNAL_SEMAPHORE_WIN32_EXTENSION_NAME };
        const float priority = 1.0f;
        VkDeviceQueueCreateInfo qi { VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO };
        qi.queueFamilyIndex = family;
        qi.queueCount = 1;
        qi.pQueuePriorities = &priority;
        VkDeviceCreateInfo di { VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, &coop };
        di.queueCreateInfoCount = 1;
        di.pQueueCreateInfos = &qi;
        di.enabledExtensionCount = uint32_t(std::size(extensions));
        di.ppEnabledExtensionNames = extensions;
        VkCheck(vkCreateDevice(physical, &di, nullptr, &device), "vkCreateDevice");
        vkGetDeviceQueue(device, family, 0, &queue);
        vkGetPhysicalDeviceMemoryProperties(physical, &memory);
        memoryHandleProperties = reinterpret_cast<PFN_vkGetMemoryWin32HandlePropertiesKHR>(
            vkGetDeviceProcAddr(device, "vkGetMemoryWin32HandlePropertiesKHR"));
        importSemaphore = reinterpret_cast<PFN_vkImportSemaphoreWin32HandleKHR>(
            vkGetDeviceProcAddr(device, "vkImportSemaphoreWin32HandleKHR"));
        if (!memoryHandleProperties || !importSemaphore)
            throw std::runtime_error("the Win32 external memory entry points are missing");
    }

    uint32_t MemoryType(uint32_t bits, VkMemoryPropertyFlags wanted) const
    {
        for (uint32_t i = 0; i < memory.memoryTypeCount; ++i)
            if ((bits & (1u << i)) && (memory.memoryTypes[i].propertyFlags & wanted) == wanted)
                return i;
        for (uint32_t i = 0; i < memory.memoryTypeCount; ++i)
            if (bits & (1u << i))
                return i;
        throw std::runtime_error("no memory type for an image or buffer");
    }

    void Destroy()
    {
        if (device)
            vkDestroyDevice(device, nullptr);
        if (instance)
            vkDestroyInstance(instance, nullptr);
        device = {};
        instance = {};
    }
};

// A D3D12 buffer the Vulkan device reads and writes through an imported handle.
struct SharedBuffer
{
    ID3D12Resource* resource {};
    HANDLE handle {};
    VkBuffer buffer {};
    VkDeviceMemory memory {};

    void Create(ID3D12Device* d, Vulkan& vk, UINT64 bytes)
    {
        D3D12_HEAP_PROPERTIES hp {};
        hp.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC rd {};
        rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        rd.Width = (bytes + 65535) & ~UINT64(65535);
        rd.Height = rd.DepthOrArraySize = rd.MipLevels = 1;
        rd.SampleDesc.Count = 1;
        rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        Check(d->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_SHARED, &rd, D3D12_RESOURCE_STATE_COMMON, nullptr,
                                         IID_PPV_ARGS(&resource)),
              "shared buffer");
        Check(d->CreateSharedHandle(resource, nullptr, GENERIC_ALL, nullptr, &handle), "shared buffer handle");

        VkExternalMemoryBufferCreateInfo external { VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO };
        external.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D12_RESOURCE_BIT;
        VkBufferCreateInfo bi { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, &external };
        bi.size = rd.Width;
        bi.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        VkCheck(vkCreateBuffer(vk.device, &bi, nullptr, &buffer), "vkCreateBuffer (shared)");
        VkMemoryRequirements req {};
        vkGetBufferMemoryRequirements(vk.device, buffer, &req);
        VkMemoryWin32HandlePropertiesKHR props { VK_STRUCTURE_TYPE_MEMORY_WIN32_HANDLE_PROPERTIES_KHR };
        VkCheck(vk.memoryHandleProperties(vk.device, VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D12_RESOURCE_BIT, handle, &props),
                "vkGetMemoryWin32HandlePropertiesKHR");
        VkMemoryDedicatedAllocateInfo dedicated { VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO };
        dedicated.buffer = buffer;
        VkImportMemoryWin32HandleInfoKHR import { VK_STRUCTURE_TYPE_IMPORT_MEMORY_WIN32_HANDLE_INFO_KHR, &dedicated };
        import.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D12_RESOURCE_BIT;
        import.handle = handle;
        VkMemoryAllocateInfo ai { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, &import };
        ai.allocationSize = std::max<UINT64>(req.size, d->GetResourceAllocationInfo(0, 1, &rd).SizeInBytes);
        ai.memoryTypeIndex =
            vk.MemoryType(req.memoryTypeBits & props.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        VkCheck(vkAllocateMemory(vk.device, &ai, nullptr, &memory), "import of a D3D12 buffer");
        VkCheck(vkBindBufferMemory(vk.device, buffer, memory, 0), "vkBindBufferMemory (shared)");
    }

    void Release(VkDevice device)
    {
        if (buffer)
            vkDestroyBuffer(device, buffer, nullptr);
        if (memory)
            vkFreeMemory(device, memory, nullptr);
        if (handle)
            CloseHandle(handle);
        if (resource)
            resource->Release();
        *this = {};
    }
};

struct Image
{
    VkImage image {};
    VkDeviceMemory memory {};

    void Create(Vulkan& vk, VkFormat format, uint32_t w, uint32_t h)
    {
        VkImageCreateInfo ci { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
        ci.imageType = VK_IMAGE_TYPE_2D;
        ci.format = format;
        ci.extent = { w, h, 1 };
        ci.mipLevels = ci.arrayLayers = 1;
        ci.samples = VK_SAMPLE_COUNT_1_BIT;
        ci.tiling = VK_IMAGE_TILING_OPTIMAL;
        ci.usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
        ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        VkCheck(vkCreateImage(vk.device, &ci, nullptr, &image), "vkCreateImage");
        VkMemoryRequirements req {};
        vkGetImageMemoryRequirements(vk.device, image, &req);
        VkMemoryAllocateInfo ai { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
        ai.allocationSize = req.size;
        ai.memoryTypeIndex = vk.MemoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        VkCheck(vkAllocateMemory(vk.device, &ai, nullptr, &memory), "vkAllocateMemory (image)");
        VkCheck(vkBindImageMemory(vk.device, image, memory, 0), "vkBindImageMemory");
    }

    void Release(VkDevice device)
    {
        if (image)
            vkDestroyImage(device, image, nullptr);
        if (memory)
            vkFreeMemory(device, memory, nullptr);
        *this = {};
    }
};

void ImageBarrier(VkCommandBuffer c, VkImage image, VkImageLayout from, VkImageLayout to, VkPipelineStageFlags srcStage,
                  VkAccessFlags srcAccess, VkPipelineStageFlags dstStage, VkAccessFlags dstAccess)
{
    VkImageMemoryBarrier b { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
    b.srcAccessMask = srcAccess;
    b.dstAccessMask = dstAccess;
    b.oldLayout = from;
    b.newLayout = to;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = image;
    b.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
    vkCmdPipelineBarrier(c, srcStage, dstStage, 0, 0, nullptr, 0, nullptr, 1, &b);
}

void Transition(ID3D12GraphicsCommandList* c, ID3D12Resource* r, D3D12_RESOURCE_STATES from, D3D12_RESOURCE_STATES to)
{
    if (from == to)
        return;
    D3D12_RESOURCE_BARRIER b {};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition = { r, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, from, to };
    c->ResourceBarrier(1, &b);
}

// A texture's top-left w x h rows, packed the way both APIs copy buffers.
struct Footprint
{
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT d3d {};
    VkBufferImageCopy vk {};
    UINT64 bytes = 0;

    Footprint() = default;
    Footprint(DXGI_FORMAT format, UINT texelBytes, UINT w, UINT h)
    {
        const UINT pitch =
            (w * texelBytes + D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1) & ~UINT(D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1);
        d3d.Footprint = { format, w, h, 1, pitch };
        vk.bufferRowLength = pitch / texelBytes;
        vk.bufferImageHeight = h;
        vk.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
        vk.imageExtent = { w, h, 1 };
        bytes = UINT64(pitch) * h;
    }
};

void CopyToBuffer(ID3D12GraphicsCommandList* c, ID3D12Resource* texture, D3D12_RESOURCE_STATES state,
                  ID3D12Resource* buffer, const Footprint& fp)
{
    Transition(c, texture, state, D3D12_RESOURCE_STATE_COPY_SOURCE);
    Transition(c, buffer, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST);
    D3D12_TEXTURE_COPY_LOCATION dst { buffer, D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT, {} };
    dst.PlacedFootprint = fp.d3d;
    D3D12_TEXTURE_COPY_LOCATION src { texture, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX, {} };
    const D3D12_BOX box { 0, 0, 0, fp.d3d.Footprint.Width, fp.d3d.Footprint.Height, 1 };
    c->CopyTextureRegion(&dst, 0, 0, 0, &src, &box);
    Transition(c, buffer, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COMMON);
    Transition(c, texture, D3D12_RESOURCE_STATE_COPY_SOURCE, state);
}

struct Job
{
    uint32_t state = LMXXF_NR_JOB_NONE;
    ID3D12Resource* colour {};
    D3D12_RESOURCE_STATES colourState {};
    ID3D12Resource* motion {};
    D3D12_RESOURCE_STATES motionState {};
    float motionScaleX = 0, motionScaleY = 0;
    bool reset = false;
};

// Everything sized by the frame. Rebuilt, after a drain, when any of it changes.
struct Geometry
{
    UINT width = 0, height = 0;
    DXGI_FORMAT colourFormat = DXGI_FORMAT_UNKNOWN;
    UINT64 colourWidth = 0;
    UINT colourHeight = 0;
    DXGI_FORMAT motionFormat = DXGI_FORMAT_UNKNOWN;
    UINT motionWidth = 0, motionHeight = 0;
    bool operator==(const Geometry&) const = default;
};

constexpr uint32_t kSlots = 3;

struct Session
{
    ID3D12Device* device {};
    ID3D12CommandQueue* queue {};
    ID3D12Fence* fence {};
    HANDLE fenceHandle {};
    UINT64 fenceValue = 0;
    Vulkan vk;
    VkSemaphore timeline {};
    VkCommandPool pool {};
    VkCommandBuffer commands[kSlots] {};
    VkFence done[kSlots] {};
    bool inFlight[kSlots] {};
    uint32_t nextSlot = 0;
    std::mutex submitMutex; // the VkQueue: frames and the background build share it
    bool failed = false;

    // The network for one extent and colour format, built off the render thread.
    std::unique_ptr<nr::Runtime> runtime;
    UINT netWidth = 0, netHeight = 0;
    VkFormat netFormat = VK_FORMAT_UNDEFINED;
    std::thread builder;
    std::mutex buildMutex;
    bool building = false, buildDone = false;
    std::unique_ptr<nr::Runtime> built;
    std::string buildError;
    double buildSeconds = 0;

    Geometry geometry;
    Footprint colourFootprint, motionFootprint;
    VkFormat colourVk = VK_FORMAT_UNDEFINED, motionVk = VK_FORMAT_UNDEFINED;
    SharedBuffer input, output, motionBuffer;
    Image colourImage, motionImage;
    ID3D12Resource* result {}; // what Super Resolution gets in place of the game's colour

    Job job;
    uint64_t lastFrameId = 0;
    ULONGLONG lastFrameTick = 0;
    bool resetPending = true;
    uint64_t frames = 0;

    HRESULT DrainD3D12()
    {
        if (!device || !queue)
            return S_OK;
        ID3D12Fence* f = nullptr;
        HRESULT hr = device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&f));
        if (FAILED(hr))
            return hr;
        HANDLE ev = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        hr = queue->Signal(f, 1);
        if (SUCCEEDED(hr))
            hr = f->SetEventOnCompletion(1, ev);
        if (SUCCEEDED(hr) && WaitForSingleObject(ev, 30000) != WAIT_OBJECT_0)
            hr = HRESULT_FROM_WIN32(ERROR_TIMEOUT);
        if (SUCCEEDED(hr))
        {
            CloseHandle(ev);
            f->Release();
        }
        return hr;
    }

    // Both sides idle, so sized resources and the network can go.
    void Drain()
    {
        Check(DrainD3D12(), "drain of the game queue");
        if (vk.device)
        {
            std::lock_guard lock(submitMutex);
            VkCheck(vkQueueWaitIdle(vk.queue), "vkQueueWaitIdle");
            for (bool& f : inFlight)
                f = false;
        }
    }

    void ReleaseGeometry()
    {
        if (vk.device)
        {
            input.Release(vk.device);
            output.Release(vk.device);
            motionBuffer.Release(vk.device);
            colourImage.Release(vk.device);
            motionImage.Release(vk.device);
        }
        if (result)
            result->Release();
        result = nullptr;
        geometry = {};
    }

    void CreateGeometry(const Geometry& g, ID3D12Resource* colour, const Format& cf, const Format& mf)
    {
        colourFootprint = Footprint(g.colourFormat, cf.bytes, g.width, g.height);
        colourVk = cf.vk;
        input.Create(device, vk, colourFootprint.bytes);
        output.Create(device, vk, colourFootprint.bytes);
        colourImage.Create(vk, cf.vk, g.width, g.height);
        motionVk = mf.vk;
        if (mf.vk)
        {
            motionFootprint = Footprint(g.motionFormat, mf.bytes, g.motionWidth, g.motionHeight);
            motionBuffer.Create(device, vk, motionFootprint.bytes);
            motionImage.Create(vk, mf.vk, g.motionWidth, g.motionHeight);
        }
        D3D12_RESOURCE_DESC td = colour->GetDesc();
        td.MipLevels = 1;
        td.DepthOrArraySize = 1;
        td.SampleDesc = { 1, 0 };
        td.Alignment = 0;
        td.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        D3D12_HEAP_PROPERTIES hp {};
        hp.Type = D3D12_HEAP_TYPE_DEFAULT;
        Check(device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &td,
                                              D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, nullptr,
                                              IID_PPV_ARGS(&result)),
              "output texture");
        geometry = g;
    }

    void Start()
    {
        vk.Create(device->GetAdapterLuid());
        Check(device->CreateFence(0, D3D12_FENCE_FLAG_SHARED, IID_PPV_ARGS(&fence)), "shared fence");
        Check(device->CreateSharedHandle(fence, nullptr, GENERIC_ALL, nullptr, &fenceHandle), "fence handle");
        VkSemaphoreTypeCreateInfo type { VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO };
        type.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
        VkSemaphoreCreateInfo si { VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO, &type };
        VkCheck(vkCreateSemaphore(vk.device, &si, nullptr, &timeline), "vkCreateSemaphore (timeline)");
        VkImportSemaphoreWin32HandleInfoKHR import { VK_STRUCTURE_TYPE_IMPORT_SEMAPHORE_WIN32_HANDLE_INFO_KHR };
        import.semaphore = timeline;
        import.handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_D3D12_FENCE_BIT;
        import.handle = fenceHandle;
        VkCheck(vk.importSemaphore(vk.device, &import), "import of the D3D12 fence");
        VkCommandPoolCreateInfo pi { VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
        pi.queueFamilyIndex = vk.family;
        pi.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        VkCheck(vkCreateCommandPool(vk.device, &pi, nullptr, &pool), "vkCreateCommandPool");
        VkCommandBufferAllocateInfo ai { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
        ai.commandPool = pool;
        ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        ai.commandBufferCount = kSlots;
        VkCheck(vkAllocateCommandBuffers(vk.device, &ai, commands), "vkAllocateCommandBuffers");
        VkFenceCreateInfo fi { VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
        for (VkFence& f : done)
            VkCheck(vkCreateFence(vk.device, &fi, nullptr, &f), "vkCreateFence");
        nr::logf("[mochizuki] Vulkan device %s, queue family %u", vk.name.c_str(), vk.family);
    }

    // True once a network for this extent and format is ready. Starts its build otherwise.
    bool EnsureNetwork(UINT w, UINT h, VkFormat format)
    {
        {
            std::lock_guard lock(buildMutex);
            if (buildDone)
            {
                builder.join();
                buildDone = building = false;
                if (!built)
                {
                    failed = true;
                    throw std::runtime_error(buildError);
                }
                runtime = std::move(built);
                nr::logf("[mochizuki] network ready at %ux%u in %.1f s", netWidth, netHeight, buildSeconds);
            }
            if (building)
                return false;
        }
        if (runtime && netWidth == w && netHeight == h && netFormat == format)
            return true;
        if (runtime)
        {
            Drain();
            runtime.reset();
        }
        netWidth = w;
        netHeight = h;
        netFormat = format;
        nr::RuntimeConfig config;
        const std::wstring dir = DllDirectory();
        char root[MAX_PATH * 2] {};
        WideCharToMultiByte(CP_ACP, 0, dir.c_str(), -1, root, sizeof root, nullptr, nullptr);
        config.root = root;
        config.width = w;
        config.height = h;
        config.colour_format = format;
        config.linear_input = IsLinear(format);
        nr::TemporalConfig temporal;
        temporal.enable = true;
        nr::HostDevice host;
        host.instance = vk.instance;
        host.physical = vk.physical;
        host.device = vk.device;
        host.queue = vk.queue;
        host.queue_family = vk.family;
        nr::logf("[mochizuki] building the network at %ux%u, VkFormat %d%s", w, h, int(format),
                 config.linear_input ? ", linear colour" : "");
        building = true;
        builder = std::thread(
            [this, host, config, temporal]
            {
                const auto t0 = std::chrono::steady_clock::now();
                std::unique_ptr<nr::Runtime> made;
                std::string error;
                try
                {
                    std::lock_guard lock(submitMutex);
                    made = std::make_unique<nr::Runtime>(host, config, nr::ControlMaskConfig {}, temporal);
                }
                catch (const std::exception& e)
                {
                    error = std::string("network build failed: ") + e.what();
                    nr::logf("[mochizuki] %s", error.c_str());
                }
                std::lock_guard lock(buildMutex);
                built = std::move(made);
                buildError = error;
                buildSeconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
                buildDone = true;
            });
        return false;
    }

    void Enqueue(ID3D12CommandQueue* producer)
    {
        std::lock_guard lock(submitMutex);
        const uint32_t slot = nextSlot;
        nextSlot = (nextSlot + 1) % kSlots;
        if (inFlight[slot])
        {
            VkCheck(vkWaitForFences(vk.device, 1, &done[slot], VK_TRUE, 5'000'000'000ull), "wait for an earlier frame");
            vkResetFences(vk.device, 1, &done[slot]);
            inFlight[slot] = false;
        }
        VkCommandBuffer c = commands[slot];
        VkCheck(vkResetCommandBuffer(c, 0), "vkResetCommandBuffer");
        VkCommandBufferBeginInfo bi { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
        bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        VkCheck(vkBeginCommandBuffer(c, &bi), "vkBeginCommandBuffer");

        auto upload = [&](const SharedBuffer& from, const Image& to, const Footprint& fp)
        {
            ImageBarrier(c, to.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                         VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_ACCESS_TRANSFER_WRITE_BIT);
            vkCmdCopyBufferToImage(c, from.buffer, to.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &fp.vk);
            ImageBarrier(c, to.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL,
                         VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
                         VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT);
        };
        upload(input, colourImage, colourFootprint);
        const bool motion = job.motion && motionImage.image;
        if (motion)
            upload(motionBuffer, motionImage, motionFootprint);

        nr::EngineFrame frame {};
        frame.colour.image = colourImage.image;
        frame.colour.format = colourVk;
        frame.colour.width = geometry.width;
        frame.colour.height = geometry.height;
        frame.reset = job.reset;
        if (motion)
        {
            frame.motion = frame.colour;
            frame.motion.image = motionImage.image;
            frame.motion.format = motionVk;
            frame.motion.width = geometry.motionWidth;
            frame.motion.height = geometry.motionHeight;
            frame.motion_scale_x = job.motionScaleX;
            frame.motion_scale_y = job.motionScaleY;
            runtime->record_engine(c, frame, nr::Controls {});
        }
        else
            runtime->record(c, frame.colour, nr::Controls {});

        ImageBarrier(c, colourImage.image, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                     VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_ACCESS_MEMORY_WRITE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                     VK_ACCESS_TRANSFER_READ_BIT);
        vkCmdCopyImageToBuffer(c, colourImage.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, output.buffer, 1,
                               &colourFootprint.vk);
        VkCheck(vkEndCommandBuffer(c), "vkEndCommandBuffer");

        // The producer half is already on the game queue: its signal releases our submit, ours releases the rest.
        const UINT64 produced = ++fenceValue, finished = ++fenceValue;
        Check(producer->Signal(fence, produced), "signal after the producer");
        VkTimelineSemaphoreSubmitInfo values { VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO };
        values.waitSemaphoreValueCount = 1;
        values.pWaitSemaphoreValues = &produced;
        values.signalSemaphoreValueCount = 1;
        values.pSignalSemaphoreValues = &finished;
        const VkPipelineStageFlags waitStage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
        VkSubmitInfo si { VK_STRUCTURE_TYPE_SUBMIT_INFO, &values };
        si.waitSemaphoreCount = 1;
        si.pWaitSemaphores = &timeline;
        si.pWaitDstStageMask = &waitStage;
        si.commandBufferCount = 1;
        si.pCommandBuffers = &c;
        si.signalSemaphoreCount = 1;
        si.pSignalSemaphores = &timeline;
        VkCheck(vkQueueSubmit(vk.queue, 1, &si, done[slot]), "vkQueueSubmit");
        inFlight[slot] = true;
        Check(producer->Wait(fence, finished), "wait for the network");
        ++frames;
    }

    ~Session()
    {
        if (builder.joinable())
            builder.join();
        if (failed || FAILED(DrainD3D12()))
            return; // the GPU may still use it all: leak rather than free under it
        if (vk.device)
            vkDeviceWaitIdle(vk.device);
        runtime.reset();
        built.reset();
        ReleaseGeometry();
        if (vk.device)
        {
            for (VkFence f : done)
                if (f)
                    vkDestroyFence(vk.device, f, nullptr);
            if (pool)
                vkDestroyCommandPool(vk.device, pool, nullptr);
            if (timeline)
                vkDestroySemaphore(vk.device, timeline, nullptr);
        }
        vk.Destroy();
        if (fenceHandle)
            CloseHandle(fenceHandle);
        if (fence)
            fence->Release();
        if (queue)
            queue->Release();
        if (device)
            device->Release();
    }
};

template <class Fn> int32_t Guard(Session* s, Fn&& fn)
{
    if (s && s->failed)
        return Fail(LMXXF_NR_UNAVAILABLE, "mochizuki session failed earlier; see mochizuki_nr.log");
    try
    {
        return fn();
    }
    catch (const std::exception& e)
    {
        if (s)
            s->failed = true;
        nr::logf("[mochizuki] error: %s", e.what());
        return Fail(LMXXF_NR_FAILED, e.what());
    }
    catch (...)
    {
        if (s)
            s->failed = true;
        return Fail(LMXXF_NR_FAILED, "unhandled exception");
    }
}

int32_t QueryCapabilities(LmxxfNrCapabilities* out)
{
    if (!out || out->struct_size != sizeof(LmxxfNrCapabilities))
        return Fail(LMXXF_NR_INVALID_ARGUMENT, "QueryCapabilities: struct_size mismatch");
    out->abi_version = LMXXF_NR_ABI_VERSION;
    out->max_input_width = 16384;
    out->max_input_height = 16384;
    out->history_supported = 1;
    out->overlap_supported = out->graph_supported = out->hip_ready = 0;
    out->gfx1201_target = 1;
    return LMXXF_NR_OK;
}

int32_t Create(const LmxxfNrCreateInfo* info, void** context)
{
    if (!info || !context || info->struct_size != sizeof(LmxxfNrCreateInfo))
        return Fail(LMXXF_NR_INVALID_ARGUMENT, "Create: struct_size mismatch");
    if (!info->device || !info->queue)
        return Fail(LMXXF_NR_INVALID_ARGUMENT, "Create: device and queue required");
    auto* s = new Session;
    static_cast<IUnknown*>(info->device)->QueryInterface(IID_PPV_ARGS(&s->device));
    static_cast<IUnknown*>(info->queue)->QueryInterface(IID_PPV_ARGS(&s->queue));
    if (!s->device || !s->queue)
    {
        delete s;
        return Fail(LMXXF_NR_INVALID_ARGUMENT, "Create: device or queue is not a D3D12 object");
    }
    *context = s;
    return LMXXF_NR_OK;
}

int32_t Destroy(void* context)
{
    delete static_cast<Session*>(context);
    return LMXXF_NR_OK;
}

int32_t PrepareSession(void* context)
{
    auto* s = static_cast<Session*>(context);
    return Guard(s,
                 [&]
                 {
                     if (!s->vk.device)
                         s->Start();
                     return int32_t(LMXXF_NR_OK);
                 });
}

int32_t PrepareFrame(void* context, const LmxxfNrFrameInfo* info, LmxxfNrJob* job)
{
    auto* s = static_cast<Session*>(context);
    if (!s || !info || !job || job->struct_size != sizeof(LmxxfNrJob) || info->struct_size < 64)
        return Fail(LMXXF_NR_INVALID_ARGUMENT, "PrepareFrame: bad arguments");
    job->handle = nullptr;
    job->private_output = nullptr;
    return Guard(
        s,
        [&]
        {
            if (!s->vk.device)
                return Fail(LMXXF_NR_NOT_IMPLEMENTED, "PrepareFrame: call PrepareSession first");
            auto* colour = static_cast<ID3D12Resource*>(info->color);
            if (!colour || !info->color_width || !info->color_height)
                return Fail(LMXXF_NR_INVALID_ARGUMENT, "PrepareFrame: colour and its size are required");
            const D3D12_RESOURCE_DESC cd = colour->GetDesc();
            const Format cf = ColourFormat(cd.Format);
            if (!cf.vk)
            {
                char text[96];
                std::snprintf(text, sizeof text, "colour format %u is not supported", unsigned(cd.Format));
                return Fail(LMXXF_NR_UNAVAILABLE, text);
            }
            Geometry g;
            g.width = std::min<UINT>(info->color_width, UINT(cd.Width));
            g.height = std::min<UINT>(info->color_height, cd.Height);
            g.colourFormat = cd.Format;
            g.colourWidth = cd.Width;
            g.colourHeight = cd.Height;
            auto* motion = static_cast<ID3D12Resource*>(info->motion);
            const bool temporal = info->struct_size >= offsetof(LmxxfNrFrameInfo, smooth_threshold) &&
                                  (info->flags & LMXXF_NR_FRAME_FLAG_TEMPORAL) && motion;
            Format mf;
            if (temporal)
            {
                const D3D12_RESOURCE_DESC md = motion->GetDesc();
                mf = MotionFormat(md.Format);
                static DXGI_FORMAT refused = DXGI_FORMAT_UNKNOWN;
                if (!mf.vk && refused != md.Format)
                {
                    refused = md.Format;
                    nr::logf("[mochizuki] motion vectors in DXGI format %u are not supported; running without history",
                             unsigned(md.Format));
                }
                if (mf.vk)
                {
                    g.motionFormat = md.Format;
                    g.motionWidth = std::min<UINT>(info->motion_width ? info->motion_width : g.width, UINT(md.Width));
                    g.motionHeight = std::min<UINT>(info->motion_height ? info->motion_height : g.height, md.Height);
                }
            }
            if (!s->EnsureNetwork(g.width, g.height, cf.vk))
                return Fail(LMXXF_NR_UNAVAILABLE, "building the network (the first time takes about half a minute)");
            if (!(s->geometry == g))
            {
                if (s->geometry.width)
                    s->Drain();
                s->ReleaseGeometry();
                s->CreateGeometry(g, colour, cf, mf);
                nr::logf("[mochizuki] frame %ux%u colour DXGI %u, motion %ux%u DXGI %u", g.width, g.height,
                         unsigned(g.colourFormat), g.motionWidth, g.motionHeight, unsigned(g.motionFormat));
            }

            // History carries over only to the frame right after this one.
            const ULONGLONG now = GetTickCount64();
            const bool continuous = info->frame_id == s->lastFrameId + 1 && now - s->lastFrameTick < 250;
            s->lastFrameId = info->frame_id;
            s->lastFrameTick = now;

            Job& j = s->job;
            j = {};
            j.colour = colour;
            j.colourState = static_cast<D3D12_RESOURCE_STATES>(info->color_state);
            if (mf.vk)
            {
                j.motion = motion;
                j.motionState = static_cast<D3D12_RESOURCE_STATES>(info->motion_state);
                // value * scale = pixels of the motion extent; the network reads previous uv = uv + mv.
                j.motionScaleX = info->motion_scale_x / float(g.motionWidth);
                j.motionScaleY = info->motion_scale_y / float(g.motionHeight);
                j.reset = info->reset || !continuous || s->resetPending;
                s->resetPending = false;
            }
            j.state = LMXXF_NR_JOB_PREPARED;
            job->handle = &j;
            job->private_output = s->result;
            SetError("");
            return int32_t(LMXXF_NR_OK);
        });
}

int32_t RecordInputs(void* context, void* job, void* command_list)
{
    auto* s = static_cast<Session*>(context);
    return Guard(s,
                 [&]
                 {
                     auto* c = static_cast<ID3D12GraphicsCommandList*>(command_list);
                     Job& j = s->job;
                     if (!c || job != &j || j.state != LMXXF_NR_JOB_PREPARED)
                         return Fail(LMXXF_NR_INVALID_ARGUMENT, "RecordInputs: job not prepared");
                     CopyToBuffer(c, j.colour, j.colourState, s->input.resource, s->colourFootprint);
                     if (j.motion)
                         CopyToBuffer(c, j.motion, j.motionState, s->motionBuffer.resource, s->motionFootprint);
                     j.state = LMXXF_NR_JOB_PRODUCER_SUBMITTED;
                     return int32_t(LMXXF_NR_OK);
                 });
}

int32_t EnqueueHip(void* context, void* job, void* command_queue)
{
    auto* s = static_cast<Session*>(context);
    return Guard(s,
                 [&]
                 {
                     Job& j = s->job;
                     auto* q = static_cast<ID3D12CommandQueue*>(command_queue ? command_queue : s->queue);
                     if (job != &j ||
                         (j.state != LMXXF_NR_JOB_PRODUCER_SUBMITTED && j.state != LMXXF_NR_JOB_CONSUMER_COMPLETE))
                         return Fail(LMXXF_NR_INVALID_ARGUMENT, "EnqueueHip: job not recorded");
                     s->Enqueue(q);
                     if (j.state == LMXXF_NR_JOB_PRODUCER_SUBMITTED)
                         j.state = LMXXF_NR_JOB_NR_COMPLETE;
                     SetError("");
                     return int32_t(LMXXF_NR_OK);
                 });
}

int32_t RecordOutputs(void* context, void* job, void* command_list)
{
    auto* s = static_cast<Session*>(context);
    return Guard(
        s,
        [&]
        {
            auto* c = static_cast<ID3D12GraphicsCommandList*>(command_list);
            Job& j = s->job;
            if (!c || job != &j || (j.state != LMXXF_NR_JOB_PRODUCER_SUBMITTED && j.state != LMXXF_NR_JOB_NR_COMPLETE))
                return Fail(LMXXF_NR_INVALID_ARGUMENT, "RecordOutputs: inputs not recorded");
            Transition(c, s->output.resource, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_SOURCE);
            Transition(c, s->result, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
            D3D12_TEXTURE_COPY_LOCATION dst { s->result, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX, {} };
            D3D12_TEXTURE_COPY_LOCATION src { s->output.resource, D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT, {} };
            src.PlacedFootprint = s->colourFootprint.d3d;
            c->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
            Transition(c, s->result, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            Transition(c, s->output.resource, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COMMON);
            if (j.state == LMXXF_NR_JOB_PRODUCER_SUBMITTED)
                j.state = LMXXF_NR_JOB_CONSUMER_COMPLETE;
            return int32_t(LMXXF_NR_OK);
        });
}

int32_t CancelUnsubmitted(void* context, void*)
{
    if (auto* s = static_cast<Session*>(context))
    {
        s->job.state = LMXXF_NR_JOB_RETIRED;
        s->resetPending = true;
    }
    return LMXXF_NR_OK;
}

int32_t Poll(void* context, void*, uint32_t* state)
{
    auto* s = static_cast<Session*>(context);
    if (state)
        *state = s ? s->job.state : uint32_t(LMXXF_NR_JOB_NONE);
    return LMXXF_NR_OK;
}

int32_t Retire(void* context, void*)
{
    if (auto* s = static_cast<Session*>(context))
        s->job.state = LMXXF_NR_JOB_RETIRED;
    return LMXXF_NR_OK;
}

int32_t ResetHistory(void* context)
{
    if (auto* s = static_cast<Session*>(context))
        s->resetPending = true;
    return LMXXF_NR_OK;
}

int32_t Drain(void* context)
{
    auto* s = static_cast<Session*>(context);
    return Guard(s,
                 [&]
                 {
                     s->Drain();
                     return int32_t(LMXXF_NR_OK);
                 });
}

int32_t GetStatus(void* context, char* buf, uint32_t chars)
{
    if (!buf || !chars)
        return Fail(LMXXF_NR_INVALID_ARGUMENT, "GetStatus: empty buffer");
    auto* s = static_cast<Session*>(context);
    char text[256];
    if (!s)
        std::snprintf(text, sizeof text, "no session");
    else if (s->failed)
        std::snprintf(text, sizeof text, "mochizuki failed (see mochizuki_nr.log)");
    else if (s->runtime)
        std::snprintf(text, sizeof text, "mochizuki %ux%u, network %.1f ms, %llu frames", s->netWidth, s->netHeight,
                      s->runtime->average_gpu_ms(), static_cast<unsigned long long>(s->frames));
    else
        std::snprintf(text, sizeof text, "mochizuki %s", s->building ? "building the network" : "idle");
    std::strncpy(buf, text, chars - 1);
    buf[chars - 1] = 0;
    return LMXXF_NR_OK;
}

int32_t GetLastError(char* buf, uint32_t chars)
{
    if (!buf || !chars)
        return Fail(LMXXF_NR_INVALID_ARGUMENT, "GetLastError: empty buffer");
    std::strncpy(buf, g_lastError, chars - 1);
    buf[chars - 1] = 0;
    return LMXXF_NR_OK;
}
} // namespace

extern "C" __declspec(dllexport) int32_t LmxxfNrGetApi(uint32_t abi_version, LmxxfNrApi* out)
{
    if (!out || out->struct_size != sizeof(LmxxfNrApi))
        return Fail(LMXXF_NR_INVALID_ARGUMENT, "GetApi: struct_size mismatch");
    if (abi_version != LMXXF_NR_ABI_VERSION)
        return Fail(LMXXF_NR_UNSUPPORTED_ABI, "GetApi: unsupported abi_version");
    nr::set_log_sink(LogLine);
    std::memset(out, 0, sizeof *out);
    out->struct_size = sizeof(LmxxfNrApi);
    out->abi_version = LMXXF_NR_ABI_VERSION;
    out->QueryCapabilities = QueryCapabilities;
    out->Create = Create;
    out->Destroy = Destroy;
    out->PrepareSession = PrepareSession;
    out->PrepareFrame = PrepareFrame;
    out->RecordInputs = RecordInputs;
    out->EnqueueHip = EnqueueHip;
    out->RecordOutputs = RecordOutputs;
    out->ExecuteAfterProducer = EnqueueHip;
    out->CancelUnsubmitted = CancelUnsubmitted;
    out->Poll = Poll;
    out->Retire = Retire;
    out->ResetHistory = ResetHistory;
    out->Drain = Drain;
    out->GetStatus = GetStatus;
    out->GetLastError = GetLastError;
    return LMXXF_NR_OK;
}
