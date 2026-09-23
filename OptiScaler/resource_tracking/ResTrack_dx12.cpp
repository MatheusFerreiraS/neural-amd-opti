#include "pch.h"

#include <dlssnr/DlssNr_ExposureScan.h>

#include "ResTrack_dx12.h"

#include <Config.h>
#include <State.h>
#include <Util.h>

#include <menu/menu_overlay_dx.h>

#include <algorithm>
#include <future>

#include <magic_enum_utility.hpp>
#include <include/d3dx/d3dx12.h>
#include <detours/detours.h>

#ifndef STDMETHODCALLTYPE
#include <Unknwn.h> // or <objbase.h> to get STDMETHODCALLTYPE
#endif

#ifdef USE_SPINLOCK_MUTEX
#define LOCK_GUARD(mutex) std::lock_guard<SpinLock> name(mutex)
#else
#define LOCK_GUARD(mutex) std::lock_guard<std::mutex> name(mutex)
#endif

// Device hooks for FG
typedef void(STDMETHODCALLTYPE* PFN_CreateRenderTargetView)(ID3D12Device* This, ID3D12Resource* pResource,
                                                            D3D12_RENDER_TARGET_VIEW_DESC* pDesc,
                                                            D3D12_CPU_DESCRIPTOR_HANDLE DestDescriptor);
typedef void(STDMETHODCALLTYPE* PFN_CreateShaderResourceView)(ID3D12Device* This, ID3D12Resource* pResource,
                                                              D3D12_SHADER_RESOURCE_VIEW_DESC* pDesc,
                                                              D3D12_CPU_DESCRIPTOR_HANDLE DestDescriptor);
typedef void(STDMETHODCALLTYPE* PFN_CreateUnorderedAccessView)(ID3D12Device* This, ID3D12Resource* pResource,
                                                               ID3D12Resource* pCounterResource,
                                                               D3D12_UNORDERED_ACCESS_VIEW_DESC* pDesc,
                                                               D3D12_CPU_DESCRIPTOR_HANDLE DestDescriptor);
typedef HRESULT(STDMETHODCALLTYPE* PFN_CreateGraphicsPipelineState)(ID3D12Device* This,
                                                                    const D3D12_GRAPHICS_PIPELINE_STATE_DESC* pDesc,
                                                                    REFIID riid, void** ppPipelineState);
typedef HRESULT(STDMETHODCALLTYPE* PFN_CreateComputePipelineState)(ID3D12Device* This,
                                                                   const D3D12_COMPUTE_PIPELINE_STATE_DESC* pDesc,
                                                                   REFIID riid, void** ppPipelineState);
typedef HRESULT(STDMETHODCALLTYPE* PFN_CreatePipelineState)(ID3D12Device2* This,
                                                            const D3D12_PIPELINE_STATE_STREAM_DESC* pDesc, REFIID riid,
                                                            void** ppPipelineState);
typedef void(STDMETHODCALLTYPE* PFN_CreateDepthStencilView)(ID3D12Device* This, ID3D12Resource* pResource,
                                                            const D3D12_DEPTH_STENCIL_VIEW_DESC* pDesc,
                                                            D3D12_CPU_DESCRIPTOR_HANDLE DestDescriptor);
typedef void(STDMETHODCALLTYPE* PFN_CreateConstantBufferView)(ID3D12Device* This,
                                                              const D3D12_CONSTANT_BUFFER_VIEW_DESC* pDesc,
                                                              D3D12_CPU_DESCRIPTOR_HANDLE DestDescriptor);

typedef void(STDMETHODCALLTYPE* PFN_CreateSampler)(ID3D12Device* This, const D3D12_SAMPLER_DESC* pDesc,
                                                   D3D12_CPU_DESCRIPTOR_HANDLE DestDescriptor);

typedef HRESULT(STDMETHODCALLTYPE* PFN_CreateDescriptorHeap)(ID3D12Device* This,
                                                             D3D12_DESCRIPTOR_HEAP_DESC* pDescriptorHeapDesc,
                                                             REFIID riid, void** ppvHeap);
typedef ULONG(STDMETHODCALLTYPE* PFN_HeapRelease)(ID3D12DescriptorHeap* This);
typedef void(STDMETHODCALLTYPE* PFN_CopyDescriptors)(ID3D12Device* This, UINT NumDestDescriptorRanges,
                                                     D3D12_CPU_DESCRIPTOR_HANDLE* pDestDescriptorRangeStarts,
                                                     UINT* pDestDescriptorRangeSizes, UINT NumSrcDescriptorRanges,
                                                     D3D12_CPU_DESCRIPTOR_HANDLE* pSrcDescriptorRangeStarts,
                                                     UINT* pSrcDescriptorRangeSizes,
                                                     D3D12_DESCRIPTOR_HEAP_TYPE DescriptorHeapsType);
typedef void(STDMETHODCALLTYPE* PFN_CopyDescriptorsSimple)(ID3D12Device* This, UINT NumDescriptors,
                                                           D3D12_CPU_DESCRIPTOR_HANDLE DestDescriptorRangeStart,
                                                           D3D12_CPU_DESCRIPTOR_HANDLE SrcDescriptorRangeStart,
                                                           D3D12_DESCRIPTOR_HEAP_TYPE DescriptorHeapsType);

// Command list hooks for FG
typedef void(STDMETHODCALLTYPE* PFN_OMSetRenderTargets)(ID3D12GraphicsCommandList* This,
                                                        UINT NumRenderTargetDescriptors,
                                                        D3D12_CPU_DESCRIPTOR_HANDLE* pRenderTargetDescriptors,
                                                        BOOL RTsSingleHandleToDescriptorRange,
                                                        D3D12_CPU_DESCRIPTOR_HANDLE* pDepthStencilDescriptor);
typedef void(STDMETHODCALLTYPE* PFN_SetGraphicsRootDescriptorTable)(ID3D12GraphicsCommandList* This,
                                                                    UINT RootParameterIndex,
                                                                    D3D12_GPU_DESCRIPTOR_HANDLE BaseDescriptor);
typedef void(STDMETHODCALLTYPE* PFN_SetComputeRootDescriptorTable)(ID3D12GraphicsCommandList* This,
                                                                   UINT RootParameterIndex,
                                                                   D3D12_GPU_DESCRIPTOR_HANDLE BaseDescriptor);
typedef void(STDMETHODCALLTYPE* PFN_DrawIndexedInstanced)(ID3D12GraphicsCommandList* This, UINT IndexCountPerInstance,
                                                          UINT InstanceCount, UINT StartIndexLocation,
                                                          INT BaseVertexLocation, UINT StartInstanceLocation);
typedef void(STDMETHODCALLTYPE* PFN_DrawInstanced)(ID3D12GraphicsCommandList* This, UINT VertexCountPerInstance,
                                                   UINT InstanceCount, UINT StartVertexLocation,
                                                   UINT StartInstanceLocation);
typedef void(STDMETHODCALLTYPE* PFN_Dispatch)(ID3D12GraphicsCommandList* This, UINT ThreadGroupCountX,
                                              UINT ThreadGroupCountY, UINT ThreadGroupCountZ);
typedef void(STDMETHODCALLTYPE* PFN_ExecuteBundle)(ID3D12GraphicsCommandList* This,
                                                   ID3D12GraphicsCommandList* pCommandList);
typedef void(STDMETHODCALLTYPE* PFN_ResourceBarrier)(ID3D12GraphicsCommandList* This, UINT NumBarriers,
                                                     const D3D12_RESOURCE_BARRIER* pBarriers);
typedef void(STDMETHODCALLTYPE* PFN_SetPipelineState)(ID3D12GraphicsCommandList* This,
                                                      ID3D12PipelineState* pPipelineState);
typedef HRESULT(STDMETHODCALLTYPE* PFN_Reset)(ID3D12GraphicsCommandList* This, ID3D12CommandAllocator* pAllocator,
                                              ID3D12PipelineState* pInitialState);
typedef void(STDMETHODCALLTYPE* PFN_SetMarker)(ID3D12GraphicsCommandList* This, UINT Metadata, const void* pData,
                                               UINT Size);
typedef void(STDMETHODCALLTYPE* PFN_BeginEvent)(ID3D12GraphicsCommandList* This, UINT Metadata, const void* pData,
                                                UINT Size);
typedef void(STDMETHODCALLTYPE* PFN_EndEvent)(ID3D12GraphicsCommandList* This);
typedef HRESULT(STDMETHODCALLTYPE* PFN_Close)(ID3D12GraphicsCommandList* This);

typedef void(STDMETHODCALLTYPE* PFN_ExecuteCommandLists)(ID3D12CommandQueue* This, UINT NumCommandLists,
                                                         ID3D12CommandList* const* ppCommandLists);

typedef ULONG(STDMETHODCALLTYPE* PFN_Release)(ID3D12Resource* This);
typedef ULONG(STDMETHODCALLTYPE* PFN_PsoRelease)(ID3D12PipelineState* This);

// Original method calls for device
static PFN_CreateRenderTargetView o_CreateRenderTargetView = nullptr;
static PFN_CreateShaderResourceView o_CreateShaderResourceView = nullptr;
static PFN_CreateUnorderedAccessView o_CreateUnorderedAccessView = nullptr;
static PFN_CreateGraphicsPipelineState o_CreateGraphicsPipelineState = nullptr;
static PFN_CreateComputePipelineState o_CreateComputePipelineState = nullptr;
static PFN_CreatePipelineState o_CreatePipelineState = nullptr;
static PFN_CreateDepthStencilView o_CreateDepthStencilView = nullptr;
static PFN_CreateConstantBufferView o_CreateConstantBufferView = nullptr;
static PFN_CreateSampler o_CreateSampler = nullptr;

static PFN_CreateDescriptorHeap o_CreateDescriptorHeap = nullptr;
static PFN_HeapRelease o_HeapRelease = nullptr;
static PFN_CopyDescriptors o_CopyDescriptors = nullptr;
static PFN_CopyDescriptorsSimple o_CopyDescriptorsSimple = nullptr;

// Original method calls for command list
static PFN_Dispatch o_Dispatch = nullptr;
static PFN_DrawInstanced o_DrawInstanced = nullptr;
static PFN_DrawIndexedInstanced o_DrawIndexedInstanced = nullptr;
static PFN_ExecuteBundle o_ExecuteBundle = nullptr;
static PFN_ResourceBarrier o_ResourceBarrier = nullptr;
static PFN_SetPipelineState o_SetPipelineState = nullptr;
static PFN_Reset o_Reset = nullptr;
static PFN_SetMarker o_SetMarker = nullptr;
static PFN_BeginEvent o_BeginEvent = nullptr;
static PFN_EndEvent o_EndEvent = nullptr;
static PFN_Close o_Close = nullptr;

static PFN_ExecuteCommandLists o_ExecuteCommandLists = nullptr;
static PFN_Release o_Release = nullptr;
static PFN_PsoRelease o_PsoRelease = nullptr;
// Detours rewrites the function body, not the COM vtable slot. Keep the original resource
// Release entry so a PSO that shares the same implementation can reuse hkRelease instead of
// attempting to detour the already-detoured address a second time.
static PVOID gResourceReleaseEntry = nullptr;

static ULONG hkPsoRelease(ID3D12PipelineState* This);

static PFN_OMSetRenderTargets o_OMSetRenderTargets = nullptr;
static PFN_SetGraphicsRootDescriptorTable o_SetGraphicsRootDescriptorTable = nullptr;
static PFN_SetComputeRootDescriptorTable o_SetComputeRootDescriptorTable = nullptr;

static std::mutex _hudlessTrackMutex;
static ankerl::unordered_dense::map<ID3D12GraphicsCommandList*,
                                    ankerl::unordered_dense::map<ID3D12Resource*, ResourceInfo>>
    fgPossibleHudless[BUFFER_COUNT];

// heaps section

static std::shared_mutex _heapRegistryMutex;
static std::vector<std::shared_ptr<HeapInfo>> fgHeaps;

static std::set<void*> _notFoundCmdLists;
static std::unordered_map<FG_ResourceType, void*> _resCmdList[BUFFER_COUNT];

struct HeapCacheTLS
{
    unsigned genSeen = 0;
    std::shared_ptr<HeapInfo> heap;
    uint64_t heapVersion = 0;
};

static thread_local HeapCacheTLS cache;
static thread_local HeapCacheTLS cacheRTV;
static thread_local HeapCacheTLS cacheCBV;
static thread_local HeapCacheTLS cacheSRV;
static thread_local HeapCacheTLS cacheUAV;
static std::atomic<unsigned> gHeapGeneration { 1 };

static thread_local HeapCacheTLS cacheGR;
static thread_local HeapCacheTLS cacheCR;

static std::atomic<bool> gRRResourceInspectorEnabled { false };
static std::atomic<int> gRRResourceCandidateIndex { 0 };
static std::atomic<uintptr_t> gRRResourceCandidateAddress { 0 };
static std::atomic<uint32_t> gRRResourceChannel { 0 };
static std::atomic<float> gRRResourceViewScale { 1.0f };
static std::atomic<uint64_t> gRRResourceInspectorFrame { 0 };
static std::mutex gRRResourceCandidateMutex;
static std::vector<RRResourceCandidate> gRRResourceCandidates;
static std::mutex gRRResourceStateMutex;
static std::unordered_map<ID3D12Resource*, D3D12_RESOURCE_STATES> gRRResourceStates;
struct RRResourceActivity
{
    uint64_t lastWriteFrame = 0;
    uint64_t previousWriteFrame = 0;
    uint64_t writtenFrameCount = 0;
    uint64_t writeTransitionCount = 0;
    uint64_t passWriteCount = 0;
    std::string lastWritePass;
    bool emissivePassMatch = false;
    bool writeObserved = false;
};
static std::unordered_map<ID3D12Resource*, RRResourceActivity> gRRResourceActivity;
struct RRTrackedResourceViews
{
    uint32_t srvViews = 0;
    uint32_t uavViews = 0;
    uint32_t rtvViews = 0;
};
static std::mutex gRRTrackedResourceMutex;
static std::unordered_map<ID3D12Resource*, RRTrackedResourceViews> gRRTrackedResources;
struct RRCommandListMarkerState
{
    std::vector<std::string> stack;
    std::string lastMarker;
};
struct RRMarkerInventoryItem
{
    uint64_t count = 0;
    uint64_t lastFrame = 0;
};
static std::mutex gRRMarkerMutex;
static std::unordered_map<ID3D12GraphicsCommandList*, RRCommandListMarkerState> gRRCommandListMarkers;
static std::unordered_map<std::string, RRMarkerInventoryItem> gRRMarkerInventory;

enum class RRPsoKind : uint8_t
{
    Unknown = 0,
    Graphics = 1,
    Compute = 2,
    Stream = 3,
};

struct RRPsoMetadata
{
    uint64_t id = 0;
    uint64_t hash = 0;
    RRPsoKind kind = RRPsoKind::Unknown;
};

struct RRPsoProducerStats
{
    RRPsoMetadata metadata {};
    uint64_t hits = 0;
    uint64_t lastFrame = 0;
    bool ambiguous = false;
};

struct RRCommandListPsoState
{
    ID3D12PipelineState* currentPso = nullptr;
    std::vector<ID3D12Resource*> renderTargets;
    ID3D12PipelineState* lastGraphicsPso = nullptr;
    uint64_t renderTargetGeneration = 0;
    uint64_t lastGraphicsGeneration = std::numeric_limits<uint64_t>::max();
    uint64_t lastGraphicsFrame = std::numeric_limits<uint64_t>::max();
    ID3D12PipelineState* lastComputePso = nullptr;
    uint32_t dispatchesSinceBarrier = 0;
};

static std::atomic<uint64_t> gRRNextPsoId { 1 };
static std::mutex gRRPsoMutex;
static std::unordered_map<ID3D12PipelineState*, RRPsoMetadata> gRRPsoMetadata;
static std::unordered_map<ID3D12GraphicsCommandList*, RRCommandListPsoState> gRRCommandListPsoStates;
static std::unordered_map<ID3D12Resource*, std::unordered_map<ID3D12PipelineState*, RRPsoProducerStats>>
    gRRResourcePsoProducers;

static uint64_t HashRRBytes(const void* data, size_t size, uint64_t hash = 1469598103934665603ull)
{
    if (data == nullptr || size == 0)
        return hash;

    const auto* bytes = static_cast<const uint8_t*>(data);
    for (size_t index = 0; index < size; ++index)
    {
        hash ^= bytes[index];
        hash *= 1099511628211ull;
    }
    return hash;
}

static uint64_t HashRRShader(const D3D12_SHADER_BYTECODE& shader, uint64_t hash)
{
    return HashRRBytes(shader.pShaderBytecode, shader.BytecodeLength, hash);
}

static uint64_t HashRRGraphicsPipeline(const D3D12_GRAPHICS_PIPELINE_STATE_DESC& desc)
{
    uint64_t hash = HashRRShader(desc.VS, 1469598103934665603ull);
    hash = HashRRShader(desc.PS, hash);
    hash = HashRRShader(desc.DS, hash);
    hash = HashRRShader(desc.HS, hash);
    hash = HashRRShader(desc.GS, hash);
    hash = HashRRBytes(&desc.NumRenderTargets, sizeof(desc.NumRenderTargets), hash);
    hash = HashRRBytes(desc.RTVFormats, sizeof(desc.RTVFormats), hash);
    hash = HashRRBytes(&desc.DSVFormat, sizeof(desc.DSVFormat), hash);
    hash = HashRRBytes(&desc.PrimitiveTopologyType, sizeof(desc.PrimitiveTopologyType), hash);
    hash = HashRRBytes(&desc.SampleDesc, sizeof(desc.SampleDesc), hash);
    return hash;
}

static uint64_t HashRRComputePipeline(const D3D12_COMPUTE_PIPELINE_STATE_DESC& desc)
{
    return HashRRShader(desc.CS, 1469598103934665603ull);
}

// Caller has proved the object is on its final external reference. Erasing by address is safe
// for both the dedicated PSO hook and the shared-IUnknown path through hkRelease; a resource
// address that has no PSO metadata exits immediately.
static void CleanupRRPsoReferences(void* object)
{
    auto* pipelineState = static_cast<ID3D12PipelineState*>(object);
    std::scoped_lock psoLock(gRRPsoMutex);
    if (gRRPsoMetadata.erase(pipelineState) == 0)
        return;

    for (auto& [resource, producers] : gRRResourcePsoProducers)
        producers.erase(pipelineState);

    // Recorded bindings would re-register the dying pointer through later draws/dispatches on
    // these command lists, resurrecting metadata for an address the driver may recycle.
    for (auto& [commandList, state] : gRRCommandListPsoStates)
    {
        if (state.currentPso == pipelineState)
            state.currentPso = nullptr;
        if (state.lastGraphicsPso == pipelineState)
            state.lastGraphicsPso = nullptr;
        if (state.lastComputePso == pipelineState)
            state.lastComputePso = nullptr;
    }
}

// Metadata is keyed by raw pointer, so an entry must die with its object:
// otherwise a new PSO on a recycled address inherits the dead PSO's id and
// hash and dead entries pile up over a long session. The Release vtable slot
// is detoured once, lazily, using the first PSO that reaches this point --
// building a throwaway PSO for its vtable (like HookResource does) would need
// valid shader blobs. Caller must hold gRRPsoMutex.
static void EnsurePsoReleaseHook(ID3D12PipelineState* pipelineState)
{
    if (o_PsoRelease != nullptr)
        return;

    PVOID* pVTable = *(PVOID**) pipelineState;
    PVOID releaseEntry = pVTable[2];

    // Some D3D12 runtimes share their IUnknown implementation between resources and PSOs.
    // HookResource has already detoured that entry; hkRelease calls the same PSO cleanup helper
    // after its reference probe, so attaching a second detour would only create an ambiguous
    // hook chain.
    if (releaseEntry == gResourceReleaseEntry)
        return;

    o_PsoRelease = (PFN_PsoRelease) releaseEntry;

    if (o_PsoRelease == nullptr)
        return;

    DetourTransactionBegin();
    DetourUpdateThread(GetCurrentThread());
    DetourAttach(&(PVOID&) o_PsoRelease, hkPsoRelease);

    auto detourResult = DetourTransactionCommit();
    if (detourResult != NO_ERROR)
    {
        LOG_ERROR("Failed to hook PSO Release: {:X}", detourResult);
        o_PsoRelease = nullptr;
    }
}

static RRPsoMetadata& EnsureRRPsoMetadata(ID3D12PipelineState* pipelineState, RRPsoKind kind = RRPsoKind::Unknown,
                                          uint64_t hash = 0)
{
    EnsurePsoReleaseHook(pipelineState);

    RRPsoMetadata& metadata = gRRPsoMetadata[pipelineState];
    if (metadata.id == 0)
        metadata.id = gRRNextPsoId.fetch_add(1, std::memory_order_relaxed);
    if (metadata.kind == RRPsoKind::Unknown && kind != RRPsoKind::Unknown)
        metadata.kind = kind;
    if (metadata.hash == 0 && hash != 0)
        metadata.hash = hash;
    return metadata;
}

static void RecordRRPsoProducer(ID3D12Resource* resource, ID3D12PipelineState* pipelineState, bool ambiguous)
{
    if (resource == nullptr || pipelineState == nullptr)
        return;

    const RRPsoMetadata metadata = EnsureRRPsoMetadata(pipelineState);
    RRPsoProducerStats& stats = gRRResourcePsoProducers[resource][pipelineState];
    stats.metadata = metadata;
    ++stats.hits;
    stats.lastFrame = gRRResourceInspectorFrame.load(std::memory_order_relaxed);
    stats.ambiguous = stats.ambiguous || ambiguous;
}

// Called only while gRRPsoMutex is held.
static void RecordRRGraphicsOutputs(ID3D12GraphicsCommandList* commandList)
{
    const auto found = gRRCommandListPsoStates.find(commandList);
    if (found == gRRCommandListPsoStates.end())
        return;

    RRCommandListPsoState& state = found->second;
    if (state.currentPso == nullptr || state.renderTargets.empty())
        return;

    RRPsoMetadata& metadata = EnsureRRPsoMetadata(state.currentPso);
    if (metadata.kind == RRPsoKind::Unknown || metadata.kind == RRPsoKind::Stream)
        metadata.kind = RRPsoKind::Graphics;

    const uint64_t frame = gRRResourceInspectorFrame.load(std::memory_order_relaxed);
    if (state.lastGraphicsPso == state.currentPso && state.lastGraphicsGeneration == state.renderTargetGeneration &&
        state.lastGraphicsFrame == frame)
    {
        return;
    }

    for (ID3D12Resource* resource : state.renderTargets)
        RecordRRPsoProducer(resource, state.currentPso, false);

    state.lastGraphicsPso = state.currentPso;
    state.lastGraphicsGeneration = state.renderTargetGeneration;
    state.lastGraphicsFrame = frame;
}

static void RecordRRComputeOutput(ID3D12GraphicsCommandList* commandList, ID3D12Resource* resource)
{
    if (resource == nullptr)
        return;

    std::scoped_lock psoLock(gRRPsoMutex);
    const auto found = gRRCommandListPsoStates.find(commandList);
    if (found == gRRCommandListPsoStates.end())
        return;

    RRCommandListPsoState& state = found->second;
    if (state.lastComputePso == nullptr)
        return;

    RRPsoMetadata& metadata = EnsureRRPsoMetadata(state.lastComputePso);
    if (metadata.kind == RRPsoKind::Unknown || metadata.kind == RRPsoKind::Stream)
        metadata.kind = RRPsoKind::Compute;
    RecordRRPsoProducer(resource, state.lastComputePso, state.dispatchesSinceBarrier > 1);
    state.dispatchesSinceBarrier = 0;
}

static const char* GetRRPsoKindName(uint8_t kind)
{
    switch (static_cast<RRPsoKind>(kind))
    {
    case RRPsoKind::Graphics:
        return "Graphics";
    case RRPsoKind::Compute:
        return "Compute";
    case RRPsoKind::Stream:
        return "Stream";
    default:
        return "Unknown";
    }
}

static std::string DecodeRRMarkerName(const void* data, UINT size)
{
    if (data == nullptr || size == 0)
        return {};

    constexpr UINT kMaxMarkerBytes = 4096;
    const auto* bytes = static_cast<const uint8_t*>(data);
    const size_t byteCount = std::min<size_t>(size, kMaxMarkerBytes);
    const auto printable = [](uint8_t value) { return value >= 32 && value <= 126; };

    std::string best;
    for (size_t begin = 0; begin < byteCount;)
    {
        while (begin < byteCount && !printable(bytes[begin]))
            ++begin;
        size_t end = begin;
        while (end < byteCount && printable(bytes[end]))
            ++end;
        if (end - begin >= 4 && end - begin > best.size())
            best.assign(reinterpret_cast<const char*>(bytes + begin), end - begin);
        begin = end + 1;
    }

    for (size_t alignment = 0; alignment < 2; ++alignment)
    {
        for (size_t begin = alignment; begin + 1 < byteCount;)
        {
            while (begin + 1 < byteCount && !(printable(bytes[begin]) && bytes[begin + 1] == 0))
            {
                begin += 2;
            }
            size_t end = begin;
            std::string candidate;
            while (end + 1 < byteCount && printable(bytes[end]) && bytes[end + 1] == 0)
            {
                candidate.push_back(static_cast<char>(bytes[end]));
                end += 2;
            }
            if (candidate.size() >= 4 && candidate.size() > best.size())
                best = std::move(candidate);
            begin = end + 2;
        }
    }

    while (!best.empty() && std::isspace(static_cast<unsigned char>(best.front())))
        best.erase(best.begin());
    while (!best.empty() && std::isspace(static_cast<unsigned char>(best.back())))
        best.pop_back();
    return best;
}

static bool IsRREmissiveMarker(std::string_view name)
{
    return std::search(name.begin(), name.end(), "emiss", "emiss" + 5, [](char left, char right)
                       { return std::tolower(static_cast<unsigned char>(left)) == right; }) != name.end();
}

static void ObserveRRMarker(const std::string& name)
{
    if (name.empty())
        return;

    constexpr size_t kMaxMarkerInventory = 2048;
    if (!gRRMarkerInventory.contains(name) && gRRMarkerInventory.size() >= kMaxMarkerInventory)
    {
        return;
    }

    RRMarkerInventoryItem& item = gRRMarkerInventory[name];
    ++item.count;
    item.lastFrame = gRRResourceInspectorFrame.load(std::memory_order_relaxed);
}

static std::pair<std::string, bool> GetRRActivePass(ID3D12GraphicsCommandList* commandList)
{
    std::scoped_lock markerLock(gRRMarkerMutex);
    const auto found = gRRCommandListMarkers.find(commandList);
    if (found == gRRCommandListMarkers.end())
        return {};

    const RRCommandListMarkerState& markers = found->second;
    std::string path;
    for (const std::string& marker : markers.stack)
    {
        if (marker.empty())
            continue;
        if (!path.empty())
            path += " > ";
        path += marker;
    }
    if (path.empty())
        path = markers.lastMarker;
    return { path, IsRREmissiveMarker(path) };
}

static uint32_t GetRRFormatChannelCount(DXGI_FORMAT format)
{
    switch (format)
    {
    case DXGI_FORMAT_R8_UNORM:
    case DXGI_FORMAT_R8_SNORM:
    case DXGI_FORMAT_R16_FLOAT:
    case DXGI_FORMAT_R16_UNORM:
    case DXGI_FORMAT_R16_SNORM:
    case DXGI_FORMAT_R32_FLOAT:
    case DXGI_FORMAT_R32_TYPELESS:
    case DXGI_FORMAT_R16_TYPELESS:
    case DXGI_FORMAT_R8_TYPELESS:
        return 1;

    case DXGI_FORMAT_R8G8_UNORM:
    case DXGI_FORMAT_R8G8_SNORM:
    case DXGI_FORMAT_R16G16_FLOAT:
    case DXGI_FORMAT_R16G16_UNORM:
    case DXGI_FORMAT_R16G16_SNORM:
    case DXGI_FORMAT_R32G32_FLOAT:
    case DXGI_FORMAT_R8G8_TYPELESS:
    case DXGI_FORMAT_R16G16_TYPELESS:
    case DXGI_FORMAT_R32G32_TYPELESS:
        return 2;

    case DXGI_FORMAT_R8G8B8A8_UNORM:
    case DXGI_FORMAT_R8G8B8A8_SNORM:
    case DXGI_FORMAT_R8G8B8A8_TYPELESS:
    case DXGI_FORMAT_B8G8R8A8_UNORM:
    case DXGI_FORMAT_B8G8R8A8_TYPELESS:
    case DXGI_FORMAT_R10G10B10A2_UNORM:
    case DXGI_FORMAT_R10G10B10A2_TYPELESS:
    case DXGI_FORMAT_R16G16B16A16_FLOAT:
    case DXGI_FORMAT_R16G16B16A16_UNORM:
    case DXGI_FORMAT_R16G16B16A16_SNORM:
    case DXGI_FORMAT_R16G16B16A16_TYPELESS:
    case DXGI_FORMAT_R32G32B32A32_FLOAT:
    case DXGI_FORMAT_R32G32B32A32_TYPELESS:
        return 4;

    default:
        return 0;
    }
}

// Retain the original helper names because descriptor-copy tracking uses them
// to keep candidate views alive even when HUD tracking is disabled. "Scalar"
// now also includes packed normalized/float formats so specular occlusion can
// be discovered in any component.
static bool IsRRScalarCandidateFormat(DXGI_FORMAT format) { return GetRRFormatChannelCount(format) > 0; }

static bool IsRRScalarCandidateResource(ID3D12Resource* resource, DXGI_FORMAT viewFormat)
{
    if (resource == nullptr)
        return false;

    const D3D12_RESOURCE_DESC desc = resource->GetDesc();
    const DXGI_FORMAT effectiveFormat = viewFormat == DXGI_FORMAT_UNKNOWN ? desc.Format : viewFormat;
    return desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D && desc.SampleDesc.Count == 1 && desc.Width >= 256 &&
           desc.Height >= 256 && !(desc.Flags & D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE) &&
           IsRRScalarCandidateFormat(effectiveFormat);
}

static void TrackRRResourceView(ID3D12Resource* resource, DXGI_FORMAT viewFormat, ResourceType type)
{
    // Every other RR-inspector hook site checks this first. Without it this runs on
    // every SRV/UAV/RTV creation in the process and grows an unbounded map for users
    // who never open the inspector.
    if (!gRRResourceInspectorEnabled.load(std::memory_order_relaxed))
        return;

    if (!IsRRScalarCandidateResource(resource, viewFormat))
        return;

    std::scoped_lock lock(gRRTrackedResourceMutex);
    RRTrackedResourceViews& views = gRRTrackedResources[resource];
    if (type == SRV)
        ++views.srvViews;
    else if (type == UAV)
        ++views.uavViews;
    else if (type == RTV)
        ++views.rtvViews;
}

// Whether the preview can read this resource without first transitioning it.
//
// The preview is built by the conversion pass, which is a compute shader, so the non-pixel bit
// is the one that matters. Accepting either shader-read bit let a resource the title had only
// ever handed to a pixel pass reach that one, where the read is undefined - and the inspector
// deliberately records no barrier of its own, because the resource belongs to the title and its
// state is only ever what the tracker observed. Restoring a state that was inferred rather than
// declared is a worse failure than a preview that declines to run, so a resource in the wrong
// read state is reported as non-readable and left alone.
static bool IsRRResourceComputeReadable(D3D12_RESOURCE_STATES state)
{
    constexpr D3D12_RESOURCE_STATES kComputeRead = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    constexpr D3D12_RESOURCE_STATES kWritable =
        D3D12_RESOURCE_STATE_RENDER_TARGET | D3D12_RESOURCE_STATE_UNORDERED_ACCESS | D3D12_RESOURCE_STATE_DEPTH_WRITE |
        D3D12_RESOURCE_STATE_COPY_DEST | D3D12_RESOURCE_STATE_RESOLVE_DEST;
    return (state & kComputeRead) != 0 && (state & kWritable) == 0;
}

static bool IsRRResourceWritable(D3D12_RESOURCE_STATES state)
{
    constexpr D3D12_RESOURCE_STATES kWritable =
        D3D12_RESOURCE_STATE_RENDER_TARGET | D3D12_RESOURCE_STATE_UNORDERED_ACCESS | D3D12_RESOURCE_STATE_DEPTH_WRITE |
        D3D12_RESOURCE_STATE_COPY_DEST | D3D12_RESOURCE_STATE_RESOLVE_DEST;
    return (state & kWritable) != 0;
}

// Called while gRRResourceStateMutex is held.
static void RecordRRResourceWrite(ID3D12Resource* resource, const std::string& passName, bool emissivePassMatch)
{
    if (resource == nullptr)
        return;

    RRResourceActivity& activity = gRRResourceActivity[resource];
    const uint64_t frame = gRRResourceInspectorFrame.load(std::memory_order_relaxed);
    ++activity.writeTransitionCount;
    if (!passName.empty())
    {
        activity.lastWritePass = passName;
        activity.emissivePassMatch = emissivePassMatch;
        ++activity.passWriteCount;
    }

    if (!activity.writeObserved || activity.lastWriteFrame != frame)
    {
        if (activity.writeObserved)
            activity.previousWriteFrame = activity.lastWriteFrame;
        activity.lastWriteFrame = frame;
        ++activity.writtenFrameCount;
        activity.writeObserved = true;
    }
}

void ResTrack_Dx12::SetRRResourceInspectorEnabled(bool enabled)
{
    gRRResourceInspectorEnabled.store(enabled, std::memory_order_release);

    if (enabled)
    {
        // The device hook is not installed for FSR-RR at device-creation time, because
        // this inspector is off by default and the upscaler can be switched at runtime.
        // Install it on demand instead. HookDevice is idempotent, so this is a no-op
        // when OptiFG's HUD fix already hooked the device. Descriptor heaps created
        // before this point are not tracked, but every consumer null-checks the heap
        // lookup and the candidate registry is fed by the view-creation hooks, which
        // games exercise continuously.
        ResTrack_Dx12::HookDevice(State::Instance().currentD3D12Device);
    }
    else
    {
        {
            std::scoped_lock lock(gRRResourceStateMutex);
            gRRResourceStates.clear();
            gRRResourceActivity.clear();
        }
        {
            std::scoped_lock markerLock(gRRMarkerMutex);
            gRRCommandListMarkers.clear();
            gRRMarkerInventory.clear();
        }
        {
            std::scoped_lock psoLock(gRRPsoMutex);
            gRRCommandListPsoStates.clear();
            gRRResourcePsoProducers.clear();
            // PSO metadata is only collected while the inspector is on, so it
            // is cleared here as well instead of surviving until global
            // teardown; ids restart on the next enable, matching teardown.
            gRRPsoMetadata.clear();
            gRRNextPsoId.store(1, std::memory_order_release);
        }
        {
            // This map is only fed while the inspector is on and otherwise shrinks
            // solely through hkRelease, so without this it survives every level load
            // for the rest of the process.
            std::scoped_lock resourceLock(gRRTrackedResourceMutex);
            gRRTrackedResources.clear();
        }
    }
}

bool ResTrack_Dx12::IsRRResourceInspectorEnabled()
{
    return gRRResourceInspectorEnabled.load(std::memory_order_acquire);
}

void ResTrack_Dx12::NotifyRRResourceInspectorFrame(uint64_t frameIndex)
{
    gRRResourceInspectorFrame.store(frameIndex, std::memory_order_release);
}

void ResTrack_Dx12::SetRRResourceCandidateIndex(int index)
{
    std::scoped_lock candidateLock(gRRResourceCandidateMutex);
    if (gRRResourceCandidates.empty())
    {
        gRRResourceCandidateIndex.store(0, std::memory_order_release);
        return;
    }

    const int selected = std::clamp(index, 0, static_cast<int>(gRRResourceCandidates.size()) - 1);
    gRRResourceCandidateIndex.store(selected, std::memory_order_release);
    gRRResourceCandidateAddress.store(reinterpret_cast<uintptr_t>(gRRResourceCandidates[selected].resourceAddress),
                                      std::memory_order_release);
}

int ResTrack_Dx12::GetRRResourceCandidateIndex() { return gRRResourceCandidateIndex.load(std::memory_order_acquire); }

void ResTrack_Dx12::SetRRResourceChannel(uint32_t channel)
{
    gRRResourceChannel.store(std::min(channel, 3u), std::memory_order_release);
}

uint32_t ResTrack_Dx12::GetRRResourceChannel() { return gRRResourceChannel.load(std::memory_order_acquire); }

void ResTrack_Dx12::SetRRResourceViewScale(float scale)
{
    gRRResourceViewScale.store(std::clamp(scale, 0.01f, 100.0f), std::memory_order_release);
}

float ResTrack_Dx12::GetRRResourceViewScale() { return gRRResourceViewScale.load(std::memory_order_acquire); }

void ResTrack_Dx12::RefreshRRResourceCandidates(uint32_t renderWidth, uint32_t renderHeight)
{
    std::vector<RRResourceCandidate> candidates;
    const uint64_t currentFrame = gRRResourceInspectorFrame.load(std::memory_order_acquire);
    {
        // This registry is populated directly by CreateSRV/CreateUAV/CreateRTV.
        // It intentionally does not depend on the HUD descriptor cache, whose
        // auto settings may discard non-HUD views.
        std::scoped_lock resourceLock(gRRTrackedResourceMutex);
        for (const auto& [resource, views] : gRRTrackedResources)
        {
            if (!resource)
                continue;

            const D3D12_RESOURCE_DESC desc = resource->GetDesc();
            if (desc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || desc.SampleDesc.Count != 1 ||
                desc.Width != renderWidth || desc.Height != renderHeight ||
                (desc.Flags & D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE))
            {
                continue;
            }

            RRResourceCandidate candidate {};
            candidate.resourceAddress = resource;
            char debugName[256] {};
            UINT debugNameSize = sizeof(debugName);
            if (SUCCEEDED(resource->GetPrivateData(WKPDID_D3DDebugObjectName, &debugNameSize, debugName)) &&
                debugNameSize > 0)
            {
                debugName[std::size(debugName) - 1] = '\0';
                candidate.debugName = debugName;
            }
            candidate.width = desc.Width;
            candidate.height = desc.Height;
            candidate.format = desc.Format;
            candidate.flags = desc.Flags;
            candidate.channelCount = GetRRFormatChannelCount(desc.Format);
            candidate.previewSupported = candidate.channelCount > 0;
            candidate.srvViews = views.srvViews;
            candidate.uavViews = views.uavViews;
            candidate.rtvViews = views.rtvViews;

            if (candidate.previewSupported && candidate.srvViews > 0)
                candidates.push_back(candidate);
        }
    }

    {
        std::scoped_lock stateLock(gRRResourceStateMutex);
        for (RRResourceCandidate& candidate : candidates)
        {
            ID3D12Resource* resource = static_cast<ID3D12Resource*>(candidate.resourceAddress);
            const auto state = gRRResourceStates.find(resource);
            if (state != gRRResourceStates.end())
            {
                candidate.stateKnown = true;
                candidate.state = state->second;
                candidate.computeReadable = IsRRResourceComputeReadable(candidate.state);
            }

            candidate.currentFrame = currentFrame;
            const auto activity = gRRResourceActivity.find(resource);
            if (activity != gRRResourceActivity.end() && activity->second.writeObserved)
            {
                const RRResourceActivity& observed = activity->second;
                candidate.writeObserved = true;
                candidate.lastWriteFrame = observed.lastWriteFrame;
                candidate.previousWriteFrame = observed.previousWriteFrame;
                candidate.writtenFrameCount = observed.writtenFrameCount;
                candidate.writeTransitionCount = observed.writeTransitionCount;
                candidate.passWriteCount = observed.passWriteCount;
                candidate.lastWritePass = observed.lastWritePass;
                candidate.emissivePassMatch = observed.emissivePassMatch;
                candidate.writeAgeFrames =
                    currentFrame >= observed.lastWriteFrame ? currentFrame - observed.lastWriteFrame : 0;
                candidate.lastWriteInterval =
                    observed.previousWriteFrame > 0 && observed.lastWriteFrame >= observed.previousWriteFrame
                        ? observed.lastWriteFrame - observed.previousWriteFrame
                        : 0;
                candidate.active = candidate.writeAgeFrames <= 4;
                candidate.alternating =
                    candidate.active && candidate.lastWriteInterval >= 2 && candidate.lastWriteInterval <= 4;
            }
        }
    }

    {
        std::scoped_lock psoLock(gRRPsoMutex);
        for (RRResourceCandidate& candidate : candidates)
        {
            ID3D12Resource* resource = static_cast<ID3D12Resource*>(candidate.resourceAddress);
            const auto producers = gRRResourcePsoProducers.find(resource);
            if (producers == gRRResourcePsoProducers.end())
                continue;

            candidate.producerCount = static_cast<uint32_t>(producers->second.size());
            const RRPsoProducerStats* topProducer = nullptr;
            for (const auto& [pipelineState, stats] : producers->second)
            {
                if (topProducer == nullptr || stats.hits > topProducer->hits ||
                    (stats.hits == topProducer->hits && stats.lastFrame > topProducer->lastFrame))
                {
                    topProducer = &stats;
                }
            }

            if (topProducer != nullptr)
            {
                candidate.producerPsoId = topProducer->metadata.id;
                candidate.producerPsoHash = topProducer->metadata.hash;
                candidate.producerKind = static_cast<uint8_t>(topProducer->metadata.kind);
                candidate.producerHitCount = topProducer->hits;
                candidate.producerAmbiguous = topProducer->ambiguous;
            }
        }
    }

    std::sort(candidates.begin(), candidates.end(),
              [](const RRResourceCandidate& left, const RRResourceCandidate& right)
              {
                  if (left.channelCount != right.channelCount)
                      return left.channelCount < right.channelCount;
                  if (left.format != right.format)
                      return left.format < right.format;
                  return left.resourceAddress < right.resourceAddress;
              });

    {
        std::scoped_lock candidateLock(gRRResourceCandidateMutex);
        gRRResourceCandidates = std::move(candidates);
        if (gRRResourceCandidates.empty())
        {
            gRRResourceCandidateIndex.store(0, std::memory_order_release);
            return;
        }

        const uintptr_t selectedAddress = gRRResourceCandidateAddress.load(std::memory_order_acquire);
        int selected = -1;
        if (selectedAddress != 0)
        {
            const auto found =
                std::find_if(gRRResourceCandidates.begin(), gRRResourceCandidates.end(),
                             [selectedAddress](const RRResourceCandidate& candidate)
                             { return reinterpret_cast<uintptr_t>(candidate.resourceAddress) == selectedAddress; });
            if (found != gRRResourceCandidates.end())
                selected = static_cast<int>(std::distance(gRRResourceCandidates.begin(), found));
        }

        if (selected < 0)
        {
            selected = std::clamp(gRRResourceCandidateIndex.load(std::memory_order_acquire), 0,
                                  static_cast<int>(gRRResourceCandidates.size()) - 1);
            gRRResourceCandidateAddress.store(
                reinterpret_cast<uintptr_t>(gRRResourceCandidates[selected].resourceAddress),
                std::memory_order_release);
        }
        gRRResourceCandidateIndex.store(selected, std::memory_order_release);
    }
}

std::vector<RRResourceCandidate> ResTrack_Dx12::GetRRResourceCandidates()
{
    std::scoped_lock lock(gRRResourceCandidateMutex);
    return gRRResourceCandidates;
}

ID3D12Resource* ResTrack_Dx12::AcquireRRResourceCandidate()
{
    if (!IsRRResourceInspectorEnabled())
        return nullptr;

    void* selectedAddress = nullptr;
    {
        std::scoped_lock candidateLock(gRRResourceCandidateMutex);
        const uintptr_t selected = gRRResourceCandidateAddress.load(std::memory_order_acquire);
        const auto candidate = std::find_if(gRRResourceCandidates.begin(), gRRResourceCandidates.end(),
                                            [selected](const RRResourceCandidate& value)
                                            { return reinterpret_cast<uintptr_t>(value.resourceAddress) == selected; });
        if (candidate == gRRResourceCandidates.end())
            return nullptr;

        if (!candidate->previewSupported)
            return nullptr;
        selectedAddress = candidate->resourceAddress;
    }

    ID3D12Resource* resource = static_cast<ID3D12Resource*>(selectedAddress);
    {
        std::scoped_lock stateLock(gRRResourceStateMutex);
        const auto state = gRRResourceStates.find(resource);
        if (state == gRRResourceStates.end() || !IsRRResourceComputeReadable(state->second))
            return nullptr;
    }

    std::scoped_lock resourceLock(gRRTrackedResourceMutex);
    if (!gRRTrackedResources.contains(resource))
        return nullptr;

    resource->AddRef();
    return resource;
}

void ResTrack_Dx12::LogRRScalarResourceCandidates(uint32_t renderWidth, uint32_t renderHeight)
{
    RefreshRRResourceCandidates(renderWidth, renderHeight);
    const std::vector<RRResourceCandidate> candidates = GetRRResourceCandidates();

    LOG_INFO("[RR_RESOURCE_CANDIDATE] snapshot: render={}x{}, previewableResources={}", renderWidth, renderHeight,
             candidates.size());
    for (size_t index = 0; index < candidates.size(); ++index)
    {
        const RRResourceCandidate& candidate = candidates[index];
        const auto formatName = magic_enum::enum_name(candidate.format);
        LOG_INFO("[RR_RESOURCE_CANDIDATE] candidate[{}]: ptr={:X}, size={}x{}, format={}({}), "
                 "flags={:#x}, channels={}, views=[SRV:{}, UAV:{}, RTV:{}], writable={}, "
                 "stateKnown={}, state={:#x}, computeReadable={}, name='{}', writeObserved={}, "
                 "active={}, alternating={}, writeAge={}, interval={}, writtenFrames={}, transitions={}, "
                 "passWrites={}, emissivePassMatch={}, lastWritePass='{}', producer=[id:{}, hash:{:016X}, "
                 "kind:{}, hits:{}, count:{}, ambiguous:{}]",
                 index, reinterpret_cast<uintptr_t>(candidate.resourceAddress), candidate.width, candidate.height,
                 formatName.empty() ? "UNKNOWN" : formatName, static_cast<uint32_t>(candidate.format),
                 static_cast<uint32_t>(candidate.flags), candidate.channelCount, candidate.srvViews, candidate.uavViews,
                 candidate.rtvViews, !!(candidate.flags & D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS),
                 candidate.stateKnown, static_cast<uint32_t>(candidate.state), candidate.computeReadable,
                 candidate.debugName, candidate.writeObserved, candidate.active, candidate.alternating,
                 candidate.writeObserved ? std::to_string(candidate.writeAgeFrames) : "never",
                 candidate.lastWriteInterval, candidate.writtenFrameCount, candidate.writeTransitionCount,
                 candidate.passWriteCount, candidate.emissivePassMatch, candidate.lastWritePass,
                 candidate.producerPsoId, candidate.producerPsoHash, candidate.producerKind, candidate.producerHitCount,
                 candidate.producerCount, candidate.producerAmbiguous);
    }

    if (candidates.empty())
    {
        struct InventoryItem
        {
            ID3D12Resource* resource = nullptr;
            D3D12_RESOURCE_DESC desc {};
            RRTrackedResourceViews views {};
            uint64_t sizeDelta = 0;
        };

        std::vector<InventoryItem> inventory;
        {
            std::scoped_lock resourceLock(gRRTrackedResourceMutex);
            inventory.reserve(gRRTrackedResources.size());
            for (const auto& [resource, views] : gRRTrackedResources)
            {
                if (resource == nullptr || views.srvViews == 0)
                    continue;

                const D3D12_RESOURCE_DESC desc = resource->GetDesc();
                if (desc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D)
                    continue;

                InventoryItem item {};
                item.resource = resource;
                item.desc = desc;
                item.views = views;
                item.sizeDelta = static_cast<uint64_t>(std::abs(static_cast<int64_t>(desc.Width) - renderWidth)) +
                                 static_cast<uint64_t>(std::abs(static_cast<int64_t>(desc.Height) - renderHeight));
                inventory.push_back(item);
            }
        }

        std::sort(inventory.begin(), inventory.end(), [](const InventoryItem& left, const InventoryItem& right)
                  { return left.sizeDelta < right.sizeDelta; });

        constexpr size_t kInventoryLogLimit = 64;
        LOG_INFO("[RR_RESOURCE_INVENTORY] no exact preview candidates; trackedTexture2DResources={}, "
                 "logging closest {}",
                 inventory.size(), std::min(inventory.size(), kInventoryLogLimit));
        for (size_t index = 0; index < std::min(inventory.size(), kInventoryLogLimit); ++index)
        {
            const InventoryItem& item = inventory[index];
            const auto formatName = magic_enum::enum_name(item.desc.Format);
            LOG_INFO("[RR_RESOURCE_INVENTORY] resource[{}]: ptr={:X}, size={}x{}, format={}({}), "
                     "flags={:#x}, mips={}, arrays={}, views=[SRV:{}, UAV:{}, RTV:{}], sizeDelta={}",
                     index, reinterpret_cast<uintptr_t>(item.resource), item.desc.Width, item.desc.Height,
                     formatName.empty() ? "UNKNOWN" : formatName, static_cast<uint32_t>(item.desc.Format),
                     static_cast<uint32_t>(item.desc.Flags), item.desc.MipLevels, item.desc.DepthOrArraySize,
                     item.views.srvViews, item.views.uavViews, item.views.rtvViews, item.sizeDelta);
        }
    }
}

void ResTrack_Dx12::LogRREmissivePassSnapshot(uint32_t renderWidth, uint32_t renderHeight)
{
    RefreshRRResourceCandidates(renderWidth, renderHeight);
    const std::vector<RRResourceCandidate> candidates = GetRRResourceCandidates();

    std::vector<std::pair<std::string, RRMarkerInventoryItem>> markers;
    {
        std::scoped_lock markerLock(gRRMarkerMutex);
        markers.reserve(gRRMarkerInventory.size());
        for (const auto& marker : gRRMarkerInventory)
            markers.push_back(marker);
    }
    std::sort(markers.begin(), markers.end(),
              [](const auto& left, const auto& right)
              {
                  const bool leftEmissive = IsRREmissiveMarker(left.first);
                  const bool rightEmissive = IsRREmissiveMarker(right.first);
                  if (leftEmissive != rightEmissive)
                      return leftEmissive > rightEmissive;
                  if (left.second.lastFrame != right.second.lastFrame)
                      return left.second.lastFrame > right.second.lastFrame;
                  return left.second.count > right.second.count;
              });

    const size_t emissiveMarkerCount = static_cast<size_t>(std::count_if(
        markers.begin(), markers.end(), [](const auto& marker) { return IsRREmissiveMarker(marker.first); }));
    const size_t associatedResourceCount =
        static_cast<size_t>(std::count_if(candidates.begin(), candidates.end(), [](const RRResourceCandidate& candidate)
                                          { return !candidate.lastWritePass.empty(); }));
    const size_t emissiveResourceCount =
        static_cast<size_t>(std::count_if(candidates.begin(), candidates.end(), [](const RRResourceCandidate& candidate)
                                          { return candidate.emissivePassMatch; }));

    LOG_INFO("[RR_EMISSIVE_PASS] snapshot: render={}x{}, markerNames={}, emissiveMarkerNames={}, "
             "passAssociatedResources={}, emissivePassResources={}",
             renderWidth, renderHeight, markers.size(), emissiveMarkerCount, associatedResourceCount,
             emissiveResourceCount);

    constexpr size_t kMarkerLogLimit = 128;
    for (size_t index = 0; index < std::min(markers.size(), kMarkerLogLimit); ++index)
    {
        const auto& [name, item] = markers[index];
        LOG_INFO("[RR_EMISSIVE_PASS] marker[{}]: emissiveMatch={}, count={}, lastFrame={}, name='{}'", index,
                 IsRREmissiveMarker(name), item.count, item.lastFrame, name);
    }

    for (size_t index = 0; index < candidates.size(); ++index)
    {
        const RRResourceCandidate& candidate = candidates[index];
        if (candidate.lastWritePass.empty())
            continue;
        const auto formatName = magic_enum::enum_name(candidate.format);
        LOG_INFO("[RR_EMISSIVE_PASS] resource[{}]: ptr={:X}, size={}x{}, format={}({}), "
                 "emissiveMatch={}, passWrites={}, writeAge={}, views=[SRV:{}, UAV:{}, RTV:{}], pass='{}'",
                 index, reinterpret_cast<uintptr_t>(candidate.resourceAddress), candidate.width, candidate.height,
                 formatName.empty() ? "UNKNOWN" : formatName, static_cast<uint32_t>(candidate.format),
                 candidate.emissivePassMatch, candidate.passWriteCount,
                 candidate.writeObserved ? std::to_string(candidate.writeAgeFrames) : "never", candidate.srvViews,
                 candidate.uavViews, candidate.rtvViews, candidate.lastWritePass);
    }
}

void ResTrack_Dx12::LogRRPsoProducerSnapshot(uint32_t renderWidth, uint32_t renderHeight)
{
    RefreshRRResourceCandidates(renderWidth, renderHeight);
    const std::vector<RRResourceCandidate> candidates = GetRRResourceCandidates();
    const size_t associated =
        static_cast<size_t>(std::count_if(candidates.begin(), candidates.end(), [](const RRResourceCandidate& candidate)
                                          { return candidate.producerPsoId != 0; }));
    const size_t ambiguous =
        static_cast<size_t>(std::count_if(candidates.begin(), candidates.end(), [](const RRResourceCandidate& candidate)
                                          { return candidate.producerAmbiguous; }));

    LOG_INFO("[RR_PSO_PRODUCER] snapshot: render={}x{}, resources={}, associated={}, ambiguous={}", renderWidth,
             renderHeight, candidates.size(), associated, ambiguous);
    for (size_t index = 0; index < candidates.size(); ++index)
    {
        const RRResourceCandidate& candidate = candidates[index];
        if (candidate.producerPsoId == 0)
            continue;

        const auto formatName = magic_enum::enum_name(candidate.format);
        LOG_INFO("[RR_PSO_PRODUCER] resource[{}]: ptr={:X}, format={}({}), producerId={}, "
                 "producerHash={:016X}, kind={}, hits={}, producerCount={}, ambiguous={}, "
                 "writeAge={}, views=[SRV:{}, UAV:{}, RTV:{}]",
                 index, reinterpret_cast<uintptr_t>(candidate.resourceAddress),
                 formatName.empty() ? "UNKNOWN" : formatName, static_cast<uint32_t>(candidate.format),
                 candidate.producerPsoId, candidate.producerPsoHash, GetRRPsoKindName(candidate.producerKind),
                 candidate.producerHitCount, candidate.producerCount, candidate.producerAmbiguous,
                 candidate.writeObserved ? std::to_string(candidate.writeAgeFrames) : "never", candidate.srvViews,
                 candidate.uavViews, candidate.rtvViews);
    }
}

bool ResTrack_Dx12::CheckResource(ID3D12Resource* resource)
{
    if (State::Instance().isShuttingDown)
        return false;

    auto resDesc = resource->GetDesc();

    if (resDesc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D)
        return false;

    auto& s = State::Instance();

    if (resDesc.Height != s.currentSwapchainDesc.BufferDesc.Height ||
        resDesc.Width != s.currentSwapchainDesc.BufferDesc.Width)
    {
        auto result = Config::Instance()->FGRelaxedResolutionCheck.value_or_default() &&
                      resDesc.Height >= s.currentSwapchainDesc.BufferDesc.Height - 32 &&
                      resDesc.Height <= s.currentSwapchainDesc.BufferDesc.Height + 32 &&
                      resDesc.Width >= s.currentSwapchainDesc.BufferDesc.Width - 32 &&
                      resDesc.Width <= s.currentSwapchainDesc.BufferDesc.Width + 32;

        // LOG_TRACK("Resource: {}x{} ({}), Swapchain: {}x{} ({}), Relaxed Result: {}", resDesc.Width, resDesc.Height,
        //           (UINT) resDesc.Format, scDesc.BufferDesc.Width, scDesc.BufferDesc.Height,
        //           (UINT) scDesc.BufferDesc.Format, result);

        return result;
    }

    return true;
}

inline static IID streamlineRiid {};
inline static std::once_flag streamlineRiidInitFlag;

bool ResTrack_Dx12::CheckForRealObject(const std::string functionName, IUnknown* pObject, IUnknown** ppRealObject)
{
    std::call_once(streamlineRiidInitFlag,
                   []() { IIDFromString(L"{ADEC44E2-61F0-45C3-AD9F-1B37379284FF}", &streamlineRiid); });

    auto qResult = pObject->QueryInterface(streamlineRiid, (void**) ppRealObject);

    if (qResult == S_OK && *ppRealObject != nullptr)
    {
        LOG_INFO("{} Streamline proxy found!", functionName);
        (*ppRealObject)->Release();
        return true;
    }

    return false;
}

#pragma region Resource methods

bool ResTrack_Dx12::CreateBufferResource(ID3D12Device* InDevice, ResourceInfo* InSource, D3D12_RESOURCE_STATES InState,
                                         ID3D12Resource** OutResource)
{
    if (InDevice == nullptr || InSource == nullptr || InSource->buffer == nullptr)
        return false;

    if (*OutResource != nullptr)
    {
        auto bufDesc = (*OutResource)->GetDesc();

        if (bufDesc.Width != (UINT64) (InSource->width) || bufDesc.Height != (UINT) (InSource->height) ||
            bufDesc.Format != InSource->format)
        {
            (*OutResource)->Release();
            (*OutResource) = nullptr;
        }
        else
            return true;
    }

    D3D12_HEAP_PROPERTIES heapProperties;
    D3D12_HEAP_FLAGS heapFlags;
    HRESULT hr = InSource->buffer->GetHeapProperties(&heapProperties, &heapFlags);

    if (hr != S_OK)
    {
        LOG_ERROR("GetHeapProperties result: {0:X}", (UINT64) hr);
        return false;
    }

    D3D12_RESOURCE_DESC texDesc = InSource->buffer->GetDesc();
    texDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET | D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

    hr = InDevice->CreateCommittedResource(&heapProperties, D3D12_HEAP_FLAG_NONE, &texDesc, InState, nullptr,
                                           IID_PPV_ARGS(OutResource));

    if (hr != S_OK)
    {
        LOG_ERROR("CreateCommittedResource result: {0:X}", (UINT64) hr);
        return false;
    }

    (*OutResource)->SetName(L"fgHudlessSCBufferCopy");
    return true;
}

void ResTrack_Dx12::ResourceBarrier(ID3D12GraphicsCommandList* InCommandList, ID3D12Resource* InResource,
                                    D3D12_RESOURCE_STATES InBeforeState, D3D12_RESOURCE_STATES InAfterState)
{
    if (InBeforeState == InAfterState)
        return;

    D3D12_RESOURCE_BARRIER barrier = {};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = InResource;
    barrier.Transition.StateBefore = InBeforeState;
    barrier.Transition.StateAfter = InAfterState;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    InCommandList->ResourceBarrier(1, &barrier);
}

#pragma endregion

#pragma region Heap helpers

SIZE_T ResTrack_Dx12::GetGPUHandle(ID3D12Device* This, SIZE_T cpuHandle, D3D12_DESCRIPTOR_HEAP_TYPE type)
{
    std::shared_lock lock(_heapRegistryMutex);
    for (const auto& heap : fgHeaps)
    {
        if (heap != nullptr && heap->active.load(std::memory_order_acquire) && heap->cpuStart <= cpuHandle &&
            heap->cpuEnd > cpuHandle && heap->gpuStart != 0)
        {
            auto incSize = This->GetDescriptorHandleIncrementSize(type);
            auto addr = cpuHandle - heap->cpuStart;
            auto index = addr / incSize;
            return heap->gpuStart + (index * incSize);
        }
    }

    return NULL;
}

SIZE_T ResTrack_Dx12::GetCPUHandle(ID3D12Device* This, SIZE_T gpuHandle, D3D12_DESCRIPTOR_HEAP_TYPE type)
{
    std::shared_lock lock(_heapRegistryMutex);
    for (const auto& heap : fgHeaps)
    {
        if (heap != nullptr && heap->active.load(std::memory_order_acquire) && heap->gpuStart <= gpuHandle &&
            heap->gpuEnd > gpuHandle && heap->cpuStart != 0)
        {
            auto incSize = This->GetDescriptorHandleIncrementSize(type);
            auto addr = gpuHandle - heap->gpuStart;
            auto index = addr / incSize;
            return heap->cpuStart + (index * incSize);
        }
    }

    return NULL;
}

static std::shared_ptr<HeapInfo> FindHeapByCpuHandle(SIZE_T cpuHandle, HeapCacheTLS& heapCache)
{
    const auto currentGen = gHeapGeneration.load(std::memory_order_acquire);
    auto* cachedHeap = heapCache.heap.get();
    if (heapCache.genSeen == currentGen && cachedHeap != nullptr &&
        cachedHeap->version.load(std::memory_order_relaxed) == heapCache.heapVersion &&
        cachedHeap->active.load(std::memory_order_acquire) && cachedHeap->cpuStart <= cpuHandle &&
        cpuHandle < cachedHeap->cpuEnd)
    {
        return heapCache.heap;
    }

    std::shared_lock lock(_heapRegistryMutex);
    const auto registryGen = gHeapGeneration.load(std::memory_order_acquire);
    for (const auto& heap : fgHeaps)
    {
        if (heap != nullptr && heap->active.load(std::memory_order_acquire) && heap->cpuStart <= cpuHandle &&
            cpuHandle < heap->cpuEnd)
        {
            heapCache.genSeen = registryGen;
            heapCache.heap = heap;
            heapCache.heapVersion = heap->version.load(std::memory_order_relaxed);
            return heap;
        }
    }

    heapCache.genSeen = registryGen;
    heapCache.heapVersion = 0;
    heapCache.heap.reset();
    return nullptr;
}

static std::shared_ptr<HeapInfo> FindHeapByGpuHandle(SIZE_T gpuHandle, HeapCacheTLS& heapCache)
{
    if (gpuHandle == NULL)
        return nullptr;

    const auto currentGen = gHeapGeneration.load(std::memory_order_acquire);
    auto* cachedHeap = heapCache.heap.get();
    if (heapCache.genSeen == currentGen && cachedHeap != nullptr &&
        cachedHeap->version.load(std::memory_order_relaxed) == heapCache.heapVersion &&
        cachedHeap->active.load(std::memory_order_acquire) && cachedHeap->gpuStart <= gpuHandle &&
        gpuHandle < cachedHeap->gpuEnd)
    {
        return heapCache.heap;
    }

    std::shared_lock lock(_heapRegistryMutex);
    const auto registryGen = gHeapGeneration.load(std::memory_order_acquire);
    for (const auto& heap : fgHeaps)
    {
        if (heap != nullptr && heap->active.load(std::memory_order_acquire) && heap->gpuStart <= gpuHandle &&
            gpuHandle < heap->gpuEnd)
        {
            heapCache.genSeen = registryGen;
            heapCache.heap = heap;
            heapCache.heapVersion = heap->version.load(std::memory_order_relaxed);
            return heap;
        }
    }

    heapCache.genSeen = registryGen;
    heapCache.heapVersion = 0;
    heapCache.heap.reset();
    return nullptr;
}

std::shared_ptr<HeapInfo> ResTrack_Dx12::GetHeapByCpuHandleCBV(SIZE_T cpuHandle)
{
    return FindHeapByCpuHandle(cpuHandle, cacheCBV);
}

std::shared_ptr<HeapInfo> ResTrack_Dx12::GetHeapByCpuHandleRTV(SIZE_T cpuHandle)
{
    return FindHeapByCpuHandle(cpuHandle, cacheRTV);
}

std::shared_ptr<HeapInfo> ResTrack_Dx12::GetHeapByCpuHandleSRV(SIZE_T cpuHandle)
{
    return FindHeapByCpuHandle(cpuHandle, cacheSRV);
}

std::shared_ptr<HeapInfo> ResTrack_Dx12::GetHeapByCpuHandleUAV(SIZE_T cpuHandle)
{
    return FindHeapByCpuHandle(cpuHandle, cacheUAV);
}

std::shared_ptr<HeapInfo> ResTrack_Dx12::GetHeapByCpuHandle(SIZE_T cpuHandle)
{
    return FindHeapByCpuHandle(cpuHandle, cache);
}

std::shared_ptr<HeapInfo> ResTrack_Dx12::GetHeapByGpuHandleGR(SIZE_T gpuHandle)
{
    return FindHeapByGpuHandle(gpuHandle, cacheGR);
}

std::shared_ptr<HeapInfo> ResTrack_Dx12::GetHeapByGpuHandleCR(SIZE_T gpuHandle)
{
    return FindHeapByGpuHandle(gpuHandle, cacheCR);
}

#pragma endregion

#pragma region Hudless methods

void ResTrack_Dx12::FillResourceInfo(ID3D12Resource* resource, ResourceInfo* info)
{
    auto desc = resource->GetDesc();
    info->buffer = resource;
    info->width = desc.Width;
    info->height = desc.Height;
    info->format = desc.Format;
    info->flags = desc.Flags;
}

bool ResTrack_Dx12::IsHudFixActive()
{
    if (!Config::Instance()->FGEnabled.value_or_default() || !Config::Instance()->FGHUDFix.value_or_default())
    {
        LOG_TRACK(
            "!Config::Instance()->FGEnabled.value_or_default() || !Config::Instance()->FGHUDFix.value_or_default()");
        return false;
    }

    if (State::Instance().currentFG == nullptr || State::Instance().currentFeature == nullptr ||
        State::Instance().fgChanged)
    {
        LOG_TRACK("State::Instance().currentFG == nullptr || State::Instance().currentFeature == nullptr || "
                  "State::Instance().fgChanged");
        return false;
    }

    if (!State::Instance().currentFG->IsActive())
    {
        LOG_TRACK("!State::Instance().currentFG->IsActive()");
        return false;
    }

    if (!_presentDone)
    {
        LOG_TRACK("!_presentDone");
        return false;
    }

    if (Hudfix_Dx12::SkipHudlessChecks())
    {
        LOG_TRACK("!Hudfix_Dx12::SkipHudlessChecks()");
        return false;
    }

    if (!Hudfix_Dx12::IsResourceCheckActive())
    {
        // LOG_TRACK("!Hudfix_Dx12::IsResourceCheckActive()");
        return false;
    }

    return true;
}

#pragma endregion

#pragma region Resource input hooks

void ResTrack_Dx12::hkCreateRenderTargetView(ID3D12Device* This, ID3D12Resource* pResource,
                                             D3D12_RENDER_TARGET_VIEW_DESC* pDesc,
                                             D3D12_CPU_DESCRIPTOR_HANDLE DestDescriptor)
{
    // force hdr for swapchain buffer
    if (pResource != nullptr && pDesc != nullptr && Config::Instance()->ForceHDR.value_or_default())
    {
        for (size_t i = 0; i < State::Instance().scBuffers.size(); i++)
        {
            if (State::Instance().scBuffers[i] == pResource)
            {
                if (Config::Instance()->UseHDR10.value_or_default())
                    pDesc->Format = DXGI_FORMAT_R10G10B10A2_UNORM;
                else
                    pDesc->Format = DXGI_FORMAT_R16G16B16A16_FLOAT;

                break;
            }
        }
    }

    o_CreateRenderTargetView(This, pResource, pDesc, DestDescriptor);

    const DXGI_FORMAT viewFormat = pDesc == nullptr ? DXGI_FORMAT_UNKNOWN : pDesc->Format;
    const bool isTexture2D = pDesc == nullptr || pDesc->ViewDimension == D3D12_RTV_DIMENSION_TEXTURE2D;
    if (isTexture2D)
        TrackRRResourceView(pResource, viewFormat, RTV);

    if (Config::Instance()->FGHudfixDisableRTV.value_or_default())
        return;

    const bool isHudResource = pResource != nullptr && pDesc != nullptr &&
                               pDesc->ViewDimension == D3D12_RTV_DIMENSION_TEXTURE2D && CheckResource(pResource);
    // Retaining a candidate in the HUD descriptor cache is only useful while the
    // inspector is running, and RefreshRRResourceCandidates does not read that cache
    // anyway - its registry is fed directly by these hooks. Leaving this ungated
    // widens OptiFG's hudless candidate set for every user.
    const bool isRRCandidate = isTexture2D && gRRResourceInspectorEnabled.load(std::memory_order_relaxed) &&
                               IsRRScalarCandidateResource(pResource, viewFormat);

    if (!isHudResource && !isRRCandidate)
    {
        auto heap = GetHeapByCpuHandleRTV(DestDescriptor.ptr);

        if (heap != nullptr)
            heap->ClearByCpuHandle(DestDescriptor.ptr);

        return;
    }

    // if (!CheckResource(pResource))
    //     return;

    auto heap = GetHeapByCpuHandleRTV(DestDescriptor.ptr);
    if (heap != nullptr)
    {
        ResourceInfo resInfo {};
        FillResourceInfo(pResource, &resInfo);
        if (viewFormat != DXGI_FORMAT_UNKNOWN)
            resInfo.format = viewFormat;
        resInfo.type = RTV;
        resInfo.captureInfo = CaptureInfo::CreateRTV;
        heap->SetByCpuHandle(DestDescriptor.ptr, resInfo);
    }
    // else
    //{
    //     LOG_TRACK("Heap not found for RTV: {:X}", DestDescriptor.ptr);
    // }
}

void ResTrack_Dx12::hkCreateShaderResourceView(ID3D12Device* This, ID3D12Resource* pResource,
                                               D3D12_SHADER_RESOURCE_VIEW_DESC* pDesc,
                                               D3D12_CPU_DESCRIPTOR_HANDLE DestDescriptor)
{
    // force hdr for swapchain buffer
    if (pResource != nullptr && pDesc != nullptr && Config::Instance()->ForceHDR.value_or_default())
    {
        for (size_t i = 0; i < State::Instance().scBuffers.size(); i++)
        {
            if (State::Instance().scBuffers[i] == pResource)
            {
                if (Config::Instance()->UseHDR10.value_or_default())
                    pDesc->Format = DXGI_FORMAT_R10G10B10A2_UNORM;
                else
                    pDesc->Format = DXGI_FORMAT_R16G16B16A16_FLOAT;

                break;
            }
        }
    }

    o_CreateShaderResourceView(This, pResource, pDesc, DestDescriptor);

    const DXGI_FORMAT viewFormat = pDesc == nullptr ? DXGI_FORMAT_UNKNOWN : pDesc->Format;
    const bool isTexture2D = pDesc == nullptr || pDesc->ViewDimension == D3D12_SRV_DIMENSION_TEXTURE2D;
    if (isTexture2D)
        TrackRRResourceView(pResource, viewFormat, SRV);

    if (Config::Instance()->FGHudfixDisableSRV.value_or_default())
        return;

    const bool isHudResource = pResource != nullptr && pDesc != nullptr &&
                               pDesc->ViewDimension == D3D12_SRV_DIMENSION_TEXTURE2D && CheckResource(pResource);
    // Retaining a candidate in the HUD descriptor cache is only useful while the
    // inspector is running, and RefreshRRResourceCandidates does not read that cache
    // anyway - its registry is fed directly by these hooks. Leaving this ungated
    // widens OptiFG's hudless candidate set for every user.
    const bool isRRCandidate = isTexture2D && gRRResourceInspectorEnabled.load(std::memory_order_relaxed) &&
                               IsRRScalarCandidateResource(pResource, viewFormat);

    if (!isHudResource && !isRRCandidate)
    {
        auto heap = GetHeapByCpuHandleSRV(DestDescriptor.ptr);

        if (heap != nullptr)
            heap->ClearByCpuHandle(DestDescriptor.ptr);

        return;
    }

    // if (!CheckResource(pResource))
    //     return;

    auto heap = GetHeapByCpuHandleSRV(DestDescriptor.ptr);
    if (heap != nullptr)
    {
        ResourceInfo resInfo {};
        FillResourceInfo(pResource, &resInfo);
        if (viewFormat != DXGI_FORMAT_UNKNOWN)
            resInfo.format = viewFormat;
        resInfo.type = SRV;
        resInfo.captureInfo = CaptureInfo::CreateSRV;
        heap->SetByCpuHandle(DestDescriptor.ptr, resInfo);
    }
    // else
    //{
    //     LOG_TRACK("Heap not found for SRV: {:X}", DestDescriptor.ptr);
    // }
}

void ResTrack_Dx12::hkCreateUnorderedAccessView(ID3D12Device* This, ID3D12Resource* pResource,
                                                ID3D12Resource* pCounterResource,
                                                D3D12_UNORDERED_ACCESS_VIEW_DESC* pDesc,
                                                D3D12_CPU_DESCRIPTOR_HANDLE DestDescriptor)
{
    if (pResource != nullptr && pDesc != nullptr && Config::Instance()->ForceHDR.value_or_default())
    {
        for (size_t i = 0; i < State::Instance().scBuffers.size(); i++)
        {
            if (State::Instance().scBuffers[i] == pResource)
            {
                if (Config::Instance()->UseHDR10.value_or_default())
                    pDesc->Format = DXGI_FORMAT_R10G10B10A2_UNORM;
                else
                    pDesc->Format = DXGI_FORMAT_R16G16B16A16_FLOAT;

                break;
            }
        }
    }

    // Every unordered access view the game makes passes through here, which is the one place a
    // buffer shaped like an exposure can be spotted without knowing anything about the game. Silent
    // and cheap for everything that does not match, and inert unless the scan is switched on.
    DlssNr::ExposureScan::NoteUav(pResource, pDesc);

    o_CreateUnorderedAccessView(This, pResource, pCounterResource, pDesc, DestDescriptor);

    const DXGI_FORMAT viewFormat = pDesc == nullptr ? DXGI_FORMAT_UNKNOWN : pDesc->Format;
    const bool isTexture2D = pDesc == nullptr || pDesc->ViewDimension == D3D12_UAV_DIMENSION_TEXTURE2D;
    if (isTexture2D)
        TrackRRResourceView(pResource, viewFormat, UAV);

    if (Config::Instance()->FGHudfixDisableUAV.value_or_default())
        return;

    const bool isHudResource = pResource != nullptr && pDesc != nullptr &&
                               pDesc->ViewDimension == D3D12_UAV_DIMENSION_TEXTURE2D && CheckResource(pResource);
    // Retaining a candidate in the HUD descriptor cache is only useful while the
    // inspector is running, and RefreshRRResourceCandidates does not read that cache
    // anyway - its registry is fed directly by these hooks. Leaving this ungated
    // widens OptiFG's hudless candidate set for every user.
    const bool isRRCandidate = isTexture2D && gRRResourceInspectorEnabled.load(std::memory_order_relaxed) &&
                               IsRRScalarCandidateResource(pResource, viewFormat);

    if (!isHudResource && !isRRCandidate)
    {
        auto heap = GetHeapByCpuHandleUAV(DestDescriptor.ptr);

        if (heap != nullptr)
            heap->ClearByCpuHandle(DestDescriptor.ptr);

        return;
    }

    // if (!CheckResource(pResource))
    //     return;

    auto heap = GetHeapByCpuHandleUAV(DestDescriptor.ptr);
    if (heap != nullptr)
    {
        ResourceInfo resInfo {};
        FillResourceInfo(pResource, &resInfo);
        if (viewFormat != DXGI_FORMAT_UNKNOWN)
            resInfo.format = viewFormat;
        resInfo.type = UAV;
        resInfo.captureInfo = CaptureInfo::CreateUAV;
        heap->SetByCpuHandle(DestDescriptor.ptr, resInfo);
    }
    // else
    //{
    //     LOG_TRACK("Heap not found for UAV: {:X}", DestDescriptor.ptr);
    // }
}

#pragma endregion

void ResTrack_Dx12::hkExecuteCommandLists(ID3D12CommandQueue* This, UINT NumCommandLists,
                                          ID3D12CommandList* const* ppCommandLists)
{
    auto fg = State::Instance().currentFG;

    if (fg != nullptr && fg->IsActive() && !fg->IsPaused())
    {
        LOG_TRACK("NumCommandLists: {}", NumCommandLists);

        std::vector<FG_ResourceType> found;
        auto fIndex = fg->GetIndex();

        do
        {
            std::lock_guard<std::mutex> lock2(_resourceCommandListMutex);

            if (!_notFoundCmdLists.empty())
            {
                for (size_t i = 0; i < NumCommandLists; i++)
                {
                    if (_notFoundCmdLists.contains(ppCommandLists[i]))
                    {
                        LOG_WARN("Found last frames cmdList: {:X}", (size_t) ppCommandLists[i]);
                        _notFoundCmdLists.erase(ppCommandLists[i]);
                    }
                }
            }

            if (_resCmdList[fIndex].empty())
                break;

            for (size_t i = 0; i < NumCommandLists; i++)
            {
                LOG_TRACK("ppCommandLists[{}]: {:X}", i, (size_t) ppCommandLists[i]);

                for (const auto& pair : _resCmdList[fIndex])
                {
                    if (pair.second == ppCommandLists[i])
                    {
                        LOG_DEBUG("found {} cmdList: {:X}, queue: {:X}", (UINT) pair.first, (size_t) pair.second,
                                  (size_t) This);
                        fg->SetResourceReady(pair.first);
                        found.push_back(pair.first);
                    }
                }

                for (size_t i = 0; i < found.size(); i++)
                {
                    _resCmdList[fIndex].erase(found[i]);
                }

                if (_resCmdList[fIndex].empty())
                    break;
            }

        } while (false);

        if (!found.empty())
        {
            o_ExecuteCommandLists(This, NumCommandLists, ppCommandLists);

            for (size_t i = 0; i < found.size(); i++)
            {
                fg->SetCommandQueue(found[i], This);
            }

            return;
        }
    }

    LOG_TRACK("Done NumCommandLists: {}", NumCommandLists);

    o_ExecuteCommandLists(This, NumCommandLists, ppCommandLists);
}

#pragma region Heap hooks

static ULONG STDMETHODCALLTYPE hkHeapRelease(ID3D12DescriptorHeap* This)
{
    if (State::Instance().isShuttingDown)
        return o_HeapRelease(This);

    std::shared_ptr<HeapInfo> heapInfo;
    {
        std::shared_lock lock(_heapRegistryMutex);
        for (const auto& heap : fgHeaps)
        {
            if (heap != nullptr && heap->heap == This && heap->active.load(std::memory_order_acquire))
            {
                heapInfo = heap;
                break;
            }
        }
    }

    if (heapInfo == nullptr)
        return o_HeapRelease(This);

    This->AddRef();
    if (o_HeapRelease(This) <= 1)
    {
        bool deactivated = false;
        {
            std::unique_lock lock(_heapRegistryMutex);
            deactivated = heapInfo->DeactivateAndClear();
        }

        if (deactivated)
        {
            LOG_INFO("Heap released: {:X}", (size_t) This);
            gHeapGeneration.fetch_add(1, std::memory_order_release);
        }
    }

    return o_HeapRelease(This);
}

HRESULT ResTrack_Dx12::hkCreateDescriptorHeap(ID3D12Device* This, D3D12_DESCRIPTOR_HEAP_DESC* pDescriptorHeapDesc,
                                              REFIID riid, void** ppvHeap)
{
    auto result = o_CreateDescriptorHeap(This, pDescriptorHeapDesc, riid, ppvHeap);

    if (State::Instance().skipHeapCapture)
        return result;

    // try to calculate handle ranges for heap
    if (result == S_OK && (pDescriptorHeapDesc->Type == D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV ||
                           pDescriptorHeapDesc->Type == D3D12_DESCRIPTOR_HEAP_TYPE_RTV))
    {
        auto heap = (ID3D12DescriptorHeap*) (*ppvHeap);

        if (!o_HeapRelease)
        {
            PVOID* vtbl = *(PVOID**) heap;
            o_HeapRelease = (PFN_HeapRelease) vtbl[2];
            DetourTransactionBegin();
            DetourUpdateThread(GetCurrentThread());
            DetourAttach(&(PVOID&) o_HeapRelease, hkHeapRelease);
            auto detourResult = DetourTransactionCommit();
            if (detourResult != NO_ERROR)
            {
                LOG_ERROR("Failed to hook Heap Release: {:X}", detourResult);
                o_HeapRelease = nullptr;
            }
        }

        auto increment = This->GetDescriptorHandleIncrementSize(pDescriptorHeapDesc->Type);
        auto numDescriptors = pDescriptorHeapDesc->NumDescriptors;
        auto cpuStart = (SIZE_T) (heap->GetCPUDescriptorHandleForHeapStart().ptr);
        auto cpuEnd = cpuStart + (increment * numDescriptors);
        auto gpuStart = (SIZE_T) (heap->GetGPUDescriptorHandleForHeapStart().ptr);
        auto gpuEnd = gpuStart + (increment * numDescriptors);
        auto type = (UINT) pDescriptorHeapDesc->Type;

        LOG_TRACE("Heap: {:X}, Heap type: {}, Cpu: {}-{}, Gpu: {}-{}, Desc count: {}", (size_t) *ppvHeap, type,
                  cpuStart, cpuEnd, gpuStart, gpuEnd, numDescriptors);
        {
            std::unique_lock lock(_heapRegistryMutex);
            size_t count = fgHeaps.size();
            bool foundEmpty = false;
            for (size_t i = 0; i < count; i++)
            {
                if (fgHeaps[i] != nullptr && !fgHeaps[i]->active.load(std::memory_order_acquire))
                {

                    fgHeaps[i] = std::make_shared<HeapInfo>(heap, cpuStart, cpuEnd, gpuStart, gpuEnd, numDescriptors,
                                                            increment, type);

                    gHeapGeneration.fetch_add(1, std::memory_order_release);
                    foundEmpty = true;
                    LOG_DEBUG("Reusing empty heap slot: {}", i);
                    break;
                }
            }

            if (!foundEmpty)
            {
                // Reallocate vector if needed
                if (fgHeaps.capacity() == fgHeaps.size())
                    fgHeaps.reserve(fgHeaps.size() + 65536);

                fgHeaps.push_back(std::make_shared<HeapInfo>(heap, cpuStart, cpuEnd, gpuStart, gpuEnd, numDescriptors,
                                                             increment, type));

                gHeapGeneration.fetch_add(1, std::memory_order_release);
                LOG_DEBUG("Adding new heap slot: {}", fgHeaps.size() - 1);
            }
        }
    }
    else
    {
        if (ppvHeap != nullptr && *ppvHeap != nullptr)
        {
            auto heap = (ID3D12DescriptorHeap*) (*ppvHeap);
            LOG_TRACE("Skipping, Heap type: {}, Cpu: {}, Gpu: {}", (UINT) pDescriptorHeapDesc->Type,
                      heap->GetCPUDescriptorHandleForHeapStart().ptr, heap->GetGPUDescriptorHandleForHeapStart().ptr);
        }
    }

    return result;
}

ULONG ResTrack_Dx12::hkRelease(ID3D12Resource* This)
{
    if (State::Instance().isShuttingDown)
        return o_Release(This);

    std::vector<TrackedResourceSlot> toClean;
    {
        std::lock_guard lock(_trackedResourcesMutex);

        This->AddRef();
        auto refCount = o_Release(This);

        if (refCount <= 1)
        {
            std::scoped_lock stateLock(gRRResourceStateMutex);
            gRRResourceStates.erase(This);
            gRRResourceActivity.erase(This);

            {
                std::scoped_lock rrResourceLock(gRRTrackedResourceMutex);
                gRRTrackedResources.erase(This);
            }
            {
                std::scoped_lock psoLock(gRRPsoMutex);
                gRRResourcePsoProducers.erase(This);
            }

            // A runtime may use the same IUnknown::Release implementation for resources and
            // PSOs. In that case this hook also owns PSO cleanup; the helper is a no-op for an
            // ordinary resource address.
            CleanupRRPsoReferences(This);

            if (auto it = _trackedResources.find(This); it != _trackedResources.end())
            {
                toClean = std::move(it->second);
                _trackedResources.erase(it);
            }

            State::Instance().capturedHudlesses.erase(This);
        }
    }

    // Clean descriptor slots outside the reverse-index lock.
    for (const auto& slot : toClean)
    {
        if (auto heap = slot.heap.lock())
            heap->ClearSlotIfMatches(slot.index, This);
    }

    return o_Release(This);
}

ULONG hkPsoRelease(ID3D12PipelineState* This)
{
    if (State::Instance().isShuttingDown)
        return o_PsoRelease(This);

    // Same probe as hkRelease: keep the object alive while its metadata is
    // cleaned up, then perform the caller's release.
    This->AddRef();
    auto refCount = o_PsoRelease(This);

    if (refCount <= 1)
        CleanupRRPsoReferences(This);

    return o_PsoRelease(This);
}

HRESULT ResTrack_Dx12::hkCreateGraphicsPipelineState(ID3D12Device* This,
                                                     const D3D12_GRAPHICS_PIPELINE_STATE_DESC* pDesc, REFIID riid,
                                                     void** ppPipelineState)
{
    const HRESULT result = o_CreateGraphicsPipelineState(This, pDesc, riid, ppPipelineState);
    if (SUCCEEDED(result) && pDesc != nullptr && ppPipelineState != nullptr && *ppPipelineState != nullptr &&
        gRRResourceInspectorEnabled.load(std::memory_order_relaxed))
    {
        std::scoped_lock psoLock(gRRPsoMutex);
        EnsureRRPsoMetadata(static_cast<ID3D12PipelineState*>(*ppPipelineState), RRPsoKind::Graphics,
                            HashRRGraphicsPipeline(*pDesc));
    }
    return result;
}

HRESULT ResTrack_Dx12::hkCreateComputePipelineState(ID3D12Device* This, const D3D12_COMPUTE_PIPELINE_STATE_DESC* pDesc,
                                                    REFIID riid, void** ppPipelineState)
{
    const HRESULT result = o_CreateComputePipelineState(This, pDesc, riid, ppPipelineState);
    if (SUCCEEDED(result) && pDesc != nullptr && ppPipelineState != nullptr && *ppPipelineState != nullptr &&
        gRRResourceInspectorEnabled.load(std::memory_order_relaxed))
    {
        std::scoped_lock psoLock(gRRPsoMutex);
        EnsureRRPsoMetadata(static_cast<ID3D12PipelineState*>(*ppPipelineState), RRPsoKind::Compute,
                            HashRRComputePipeline(*pDesc));
    }
    return result;
}

HRESULT ResTrack_Dx12::hkCreatePipelineState(ID3D12Device2* This, const D3D12_PIPELINE_STATE_STREAM_DESC* pDesc,
                                             REFIID riid, void** ppPipelineState)
{
    const HRESULT result = o_CreatePipelineState(This, pDesc, riid, ppPipelineState);
    if (SUCCEEDED(result) && pDesc != nullptr && ppPipelineState != nullptr && *ppPipelineState != nullptr &&
        gRRResourceInspectorEnabled.load(std::memory_order_relaxed))
    {
        const uint64_t hash = HashRRBytes(pDesc->pPipelineStateSubobjectStream, pDesc->SizeInBytes);
        std::scoped_lock psoLock(gRRPsoMutex);
        EnsureRRPsoMetadata(static_cast<ID3D12PipelineState*>(*ppPipelineState), RRPsoKind::Stream, hash);
    }
    return result;
}

void ResTrack_Dx12::hkResourceBarrier(ID3D12GraphicsCommandList* This, UINT NumBarriers,
                                      const D3D12_RESOURCE_BARRIER* pBarriers)
{
    std::vector<ID3D12Resource*> computeWrites;
    if (gRRResourceInspectorEnabled.load(std::memory_order_relaxed) && pBarriers != nullptr)
    {
        const auto [passName, emissivePassMatch] = GetRRActivePass(This);
        std::scoped_lock stateLock(gRRResourceStateMutex);
        for (UINT index = 0; index < NumBarriers; ++index)
        {
            const D3D12_RESOURCE_BARRIER& barrier = pBarriers[index];
            if (barrier.Type == D3D12_RESOURCE_BARRIER_TYPE_ALIASING)
            {
                if (barrier.Aliasing.pResourceBefore != nullptr)
                {
                    gRRResourceStates.erase(barrier.Aliasing.pResourceBefore);
                    gRRResourceActivity.erase(barrier.Aliasing.pResourceBefore);
                }
                if (barrier.Aliasing.pResourceAfter != nullptr)
                {
                    gRRResourceStates.erase(barrier.Aliasing.pResourceAfter);
                    gRRResourceActivity.erase(barrier.Aliasing.pResourceAfter);
                }
                continue;
            }

            if (barrier.Type == D3D12_RESOURCE_BARRIER_TYPE_UAV)
            {
                RecordRRResourceWrite(barrier.UAV.pResource, passName, emissivePassMatch);
                if (barrier.UAV.pResource != nullptr)
                    computeWrites.push_back(barrier.UAV.pResource);
                continue;
            }

            if (barrier.Type != D3D12_RESOURCE_BARRIER_TYPE_TRANSITION || barrier.Transition.pResource == nullptr)
            {
                continue;
            }

            ID3D12Resource* resource = barrier.Transition.pResource;
            const D3D12_RESOURCE_DESC desc = resource->GetDesc();
            const bool wholeResource = barrier.Transition.Subresource == D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES ||
                                       (desc.MipLevels == 1 && desc.DepthOrArraySize == 1);

            if (!wholeResource || (barrier.Flags & D3D12_RESOURCE_BARRIER_FLAG_BEGIN_ONLY) != 0)
            {
                gRRResourceStates.erase(resource);
                continue;
            }

            gRRResourceStates[resource] = barrier.Transition.StateAfter;
            if (IsRRResourceWritable(barrier.Transition.StateBefore) &&
                !IsRRResourceWritable(barrier.Transition.StateAfter))
            {
                RecordRRResourceWrite(resource, passName, emissivePassMatch);
                if ((barrier.Transition.StateBefore & D3D12_RESOURCE_STATE_UNORDERED_ACCESS) != 0)
                {
                    computeWrites.push_back(resource);
                }
            }
        }
    }

    for (ID3D12Resource* resource : computeWrites)
        RecordRRComputeOutput(This, resource);

    o_ResourceBarrier(This, NumBarriers, pBarriers);
}

void ResTrack_Dx12::hkSetPipelineState(ID3D12GraphicsCommandList* This, ID3D12PipelineState* pPipelineState)
{
    if (gRRResourceInspectorEnabled.load(std::memory_order_relaxed) && This != MenuOverlayDx::MenuCommandList())
    {
        std::scoped_lock psoLock(gRRPsoMutex);
        RRCommandListPsoState& state = gRRCommandListPsoStates[This];
        state.currentPso = pPipelineState;
        if (pPipelineState != nullptr)
            EnsureRRPsoMetadata(pPipelineState);
    }
    o_SetPipelineState(This, pPipelineState);
}

HRESULT ResTrack_Dx12::hkReset(ID3D12GraphicsCommandList* This, ID3D12CommandAllocator* pAllocator,
                               ID3D12PipelineState* pInitialState)
{
    const HRESULT result = o_Reset(This, pAllocator, pInitialState);
    if (SUCCEEDED(result) && gRRResourceInspectorEnabled.load(std::memory_order_relaxed))
    {
        std::scoped_lock psoLock(gRRPsoMutex);
        RRCommandListPsoState& state = gRRCommandListPsoStates[This];
        state = {};
        state.currentPso = pInitialState;
        if (pInitialState != nullptr)
            EnsureRRPsoMetadata(pInitialState);
    }
    return result;
}

void ResTrack_Dx12::hkSetMarker(ID3D12GraphicsCommandList* This, UINT Metadata, const void* pData, UINT Size)
{
    if (gRRResourceInspectorEnabled.load(std::memory_order_relaxed))
    {
        std::string name = DecodeRRMarkerName(pData, Size);
        if (!name.empty())
        {
            std::scoped_lock markerLock(gRRMarkerMutex);
            RRCommandListMarkerState& state = gRRCommandListMarkers[This];
            state.lastMarker = name;
            ObserveRRMarker(name);
        }
    }
    o_SetMarker(This, Metadata, pData, Size);
}

void ResTrack_Dx12::hkBeginEvent(ID3D12GraphicsCommandList* This, UINT Metadata, const void* pData, UINT Size)
{
    if (gRRResourceInspectorEnabled.load(std::memory_order_relaxed))
    {
        std::string name = DecodeRRMarkerName(pData, Size);
        std::scoped_lock markerLock(gRRMarkerMutex);
        RRCommandListMarkerState& state = gRRCommandListMarkers[This];
        state.stack.push_back(name);
        if (!name.empty())
        {
            state.lastMarker = name;
            ObserveRRMarker(name);
        }
    }
    o_BeginEvent(This, Metadata, pData, Size);
}

void ResTrack_Dx12::hkEndEvent(ID3D12GraphicsCommandList* This)
{
    if (gRRResourceInspectorEnabled.load(std::memory_order_relaxed))
    {
        std::scoped_lock markerLock(gRRMarkerMutex);
        const auto found = gRRCommandListMarkers.find(This);
        if (found != gRRCommandListMarkers.end() && !found->second.stack.empty())
            found->second.stack.pop_back();
    }
    o_EndEvent(This);
}

void ResTrack_Dx12::hkCopyDescriptors(ID3D12Device* This, UINT NumDestDescriptorRanges,
                                      D3D12_CPU_DESCRIPTOR_HANDLE* pDestDescriptorRangeStarts,
                                      UINT* pDestDescriptorRangeSizes, UINT NumSrcDescriptorRanges,
                                      D3D12_CPU_DESCRIPTOR_HANDLE* pSrcDescriptorRangeStarts,
                                      UINT* pSrcDescriptorRangeSizes, D3D12_DESCRIPTOR_HEAP_TYPE DescriptorHeapsType)
{
    o_CopyDescriptors(This, NumDestDescriptorRanges, pDestDescriptorRangeStarts, pDestDescriptorRangeSizes,
                      NumSrcDescriptorRanges, pSrcDescriptorRangeStarts, pSrcDescriptorRangeSizes, DescriptorHeapsType);

    // Early exit conditions - consistent validation
    if (DescriptorHeapsType != D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV &&
        DescriptorHeapsType != D3D12_DESCRIPTOR_HEAP_TYPE_RTV)
        return;

    if (NumDestDescriptorRanges == 0 || pDestDescriptorRangeStarts == nullptr)
        return;

    const UINT inc = This->GetDescriptorHandleIncrementSize(DescriptorHeapsType);

    if (!Config::Instance()->FGAlwaysTrackHeaps.value_or_default() && !IsHudFixActive())
    {
        const auto rangeContainsScalarCandidate =
            [inc](UINT rangeCount, const D3D12_CPU_DESCRIPTOR_HANDLE* starts, const UINT* sizes)
        {
            if (rangeCount == 0 || starts == nullptr)
                return false;

            for (UINT range = 0; range < rangeCount; ++range)
            {
                const UINT descriptorCount = sizes == nullptr ? 1u : sizes[range];
                auto heap = GetHeapByCpuHandle(starts[range].ptr);
                if (heap == nullptr)
                    continue;

                for (UINT descriptor = 0; descriptor < descriptorCount; ++descriptor)
                {
                    const SIZE_T handle = starts[range].ptr + static_cast<SIZE_T>(descriptor) * inc;
                    ResourceInfo info {};
                    if (heap->GetByCpuHandle(handle, info) && info.buffer != nullptr &&
                        IsRRScalarCandidateFormat(info.format))
                    {
                        return true;
                    }
                }
            }

            return false;
        };

        // Process a copy only when it propagates a scalar candidate or
        // overwrites a destination slot that previously contained one.
        if (!rangeContainsScalarCandidate(NumSrcDescriptorRanges, pSrcDescriptorRangeStarts,
                                          pSrcDescriptorRangeSizes) &&
            !rangeContainsScalarCandidate(NumDestDescriptorRanges, pDestDescriptorRangeStarts,
                                          pDestDescriptorRangeSizes))
        {
            return;
        }
    }

    // Validate that we have source descriptors to copy
    bool haveSources = (NumSrcDescriptorRanges > 0 && pSrcDescriptorRangeStarts != nullptr);

    // Track positions in both source and destination ranges
    UINT srcRangeIndex = 0;
    UINT srcOffsetInRange = 0;
    UINT destRangeIndex = 0;
    UINT destOffsetInRange = 0;

    // Cache for heap lookups to avoid repeated lookups within the same range
    std::shared_ptr<HeapInfo> cachedDestHeap;
    SIZE_T cachedDestRangeStart = 0;
    UINT cachedDestRangeSize = 0;
    std::shared_ptr<HeapInfo> cachedSrcHeap;
    SIZE_T cachedSrcRangeStart = 0;
    UINT cachedSrcRangeSize = 0;

    // Process all destination descriptors
    while (destRangeIndex < NumDestDescriptorRanges)
    {
        // Update destination heap cache if we've moved to a new range
        if (destOffsetInRange == 0)
        {
            cachedDestRangeStart = pDestDescriptorRangeStarts[destRangeIndex].ptr;
            cachedDestRangeSize =
                (pDestDescriptorRangeSizes == nullptr) ? 1 : pDestDescriptorRangeSizes[destRangeIndex];
            cachedDestHeap = GetHeapByCpuHandle(cachedDestRangeStart);
        }

        // Calculate current destination handle
        const SIZE_T destHandle = cachedDestRangeStart + (static_cast<SIZE_T>(destOffsetInRange) * inc);

        // Get or update source information
        ResourceInfo srcInfo {};
        bool haveSrcInfo = false;
        if (haveSources && srcRangeIndex < NumSrcDescriptorRanges)
        {
            // Update source heap cache if we've moved to a new range
            if (srcOffsetInRange == 0)
            {
                cachedSrcRangeStart = pSrcDescriptorRangeStarts[srcRangeIndex].ptr;
                cachedSrcRangeSize =
                    (pSrcDescriptorRangeSizes == nullptr) ? 1 : pSrcDescriptorRangeSizes[srcRangeIndex];
                cachedSrcHeap = GetHeapByCpuHandle(cachedSrcRangeStart);
            }

            // Calculate current source handle
            const SIZE_T srcHandle = cachedSrcRangeStart + (static_cast<SIZE_T>(srcOffsetInRange) * inc);

            if (cachedSrcHeap != nullptr)
                haveSrcInfo = cachedSrcHeap->GetByCpuHandle(srcHandle, srcInfo);

            // Advance source position
            srcOffsetInRange++;
            if (srcOffsetInRange >= cachedSrcRangeSize)
            {
                srcOffsetInRange = 0;
                srcRangeIndex++;
            }
        }

        if (cachedDestHeap != nullptr)
        {
            if (haveSrcInfo)
                cachedDestHeap->SetByCpuHandle(destHandle, srcInfo);
            else
                cachedDestHeap->ClearByCpuHandle(destHandle);
        }

        // Advance destination position
        destOffsetInRange++;
        if (destOffsetInRange >= cachedDestRangeSize)
        {
            destOffsetInRange = 0;
            destRangeIndex++;
        }
    }
}

void ResTrack_Dx12::hkCopyDescriptorsSimple(ID3D12Device* This, UINT NumDescriptors,
                                            D3D12_CPU_DESCRIPTOR_HANDLE DestDescriptorRangeStart,
                                            D3D12_CPU_DESCRIPTOR_HANDLE SrcDescriptorRangeStart,
                                            D3D12_DESCRIPTOR_HEAP_TYPE DescriptorHeapsType)
{
    o_CopyDescriptorsSimple(This, NumDescriptors, DestDescriptorRangeStart, SrcDescriptorRangeStart,
                            DescriptorHeapsType);

    if (DescriptorHeapsType != D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV &&
        DescriptorHeapsType != D3D12_DESCRIPTOR_HEAP_TYPE_RTV)
        return;

    auto size = This->GetDescriptorHandleIncrementSize(DescriptorHeapsType);

    if (!Config::Instance()->FGAlwaysTrackHeaps.value_or_default() && !IsHudFixActive())
    {
        bool containsScalarCandidate = false;
        auto srcHeap = GetHeapByCpuHandle(SrcDescriptorRangeStart.ptr);
        auto dstHeap = GetHeapByCpuHandle(DestDescriptorRangeStart.ptr);
        for (UINT descriptor = 0; descriptor < NumDescriptors && !containsScalarCandidate; ++descriptor)
        {
            const SIZE_T srcHandle = SrcDescriptorRangeStart.ptr + static_cast<SIZE_T>(descriptor) * size;
            const SIZE_T dstHandle = DestDescriptorRangeStart.ptr + static_cast<SIZE_T>(descriptor) * size;
            ResourceInfo srcInfo {};
            ResourceInfo dstInfo {};
            const bool hasSrc = srcHeap != nullptr && srcHeap->GetByCpuHandle(srcHandle, srcInfo);
            const bool hasDst = dstHeap != nullptr && dstHeap->GetByCpuHandle(dstHandle, dstInfo);
            containsScalarCandidate =
                (hasSrc && srcInfo.buffer != nullptr && IsRRScalarCandidateFormat(srcInfo.format)) ||
                (hasDst && dstInfo.buffer != nullptr && IsRRScalarCandidateFormat(dstInfo.format));
        }

        if (!containsScalarCandidate)
            return;
    }

    for (size_t i = 0; i < NumDescriptors; i++)
    {
        std::shared_ptr<HeapInfo> srcHeap;
        SIZE_T srcHandle = 0;

        // source
        if (SrcDescriptorRangeStart.ptr != 0)
        {
            srcHandle = SrcDescriptorRangeStart.ptr + i * size;
            srcHeap = GetHeapByCpuHandle(srcHandle);
        }

        auto destHandle = DestDescriptorRangeStart.ptr + i * size;
        auto dstHeap = GetHeapByCpuHandle(destHandle);

        // destination
        if (dstHeap == nullptr)
            continue;

        if (srcHeap == nullptr)
        {
            dstHeap->ClearByCpuHandle(destHandle);
            continue;
        }

        ResourceInfo buffer {};
        if (!srcHeap->GetByCpuHandle(srcHandle, buffer))
        {
            dstHeap->ClearByCpuHandle(destHandle);
            continue;
        }

        dstHeap->SetByCpuHandle(destHandle, buffer);
    }
}

#pragma endregion

#pragma region Shader input hooks

void ResTrack_Dx12::hkSetGraphicsRootDescriptorTable(ID3D12GraphicsCommandList* This, UINT RootParameterIndex,
                                                     D3D12_GPU_DESCRIPTOR_HANDLE BaseDescriptor)
{
    // Consistent early exit - always call original function
    auto shouldTrack = !Config::Instance()->FGHudfixDisableSGR.value_or_default() && BaseDescriptor.ptr != 0 &&
                       IsHudFixActive() && !Hudfix_Dx12::SkipHudlessChecks() &&
                       This != MenuOverlayDx::MenuCommandList();

    if (!shouldTrack)
    {
        o_SetGraphicsRootDescriptorTable(This, RootParameterIndex, BaseDescriptor);
        return;
    }

    auto heap = GetHeapByGpuHandleGR(BaseDescriptor.ptr);
    if (heap == nullptr)
    {
        LOG_DEBUG_ONLY("No heap for handle: {:X}", BaseDescriptor.ptr);
        o_SetGraphicsRootDescriptorTable(This, RootParameterIndex, BaseDescriptor);
        return;
    }

    ResourceInfo capturedBuffer {};
    if (!heap->GetByGpuHandle(BaseDescriptor.ptr, capturedBuffer) || capturedBuffer.buffer == nullptr)
    {
        LOG_DEBUG_ONLY("No resource at RootParameterIndex: {}, CommandList: {:X}, gpuHandle: {:X}", RootParameterIndex,
                       (SIZE_T) This, BaseDescriptor.ptr);
        o_SetGraphicsRootDescriptorTable(This, RootParameterIndex, BaseDescriptor);
        return;
    }

    LOG_DEBUG_ONLY("CommandList: {:X}, Resource: {:X}", (size_t) This, (size_t) capturedBuffer.buffer);

    // Only proceed with tracking if we have a valid buffer
    capturedBuffer.state = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    capturedBuffer.captureInfo = CaptureInfo::SetGR;

    // Track the resource
    bool capturedImmediately = false;
    if (Config::Instance()->FGImmediateCapture.value_or_default())
    {
        capturedImmediately = Hudfix_Dx12::CheckForHudless(This, &capturedBuffer, capturedBuffer.state);
    }

    if (!capturedImmediately)
    {
        auto fIndex = Hudfix_Dx12::ActivePresentFrame() % BUFFER_COUNT;

        if (!_useShards)
        {
            std::lock_guard<std::mutex> lock(_hudlessTrackMutex);

            if (!fgPossibleHudless[fIndex].contains(This))
            {
                ankerl::unordered_dense::map<ID3D12Resource*, ResourceInfo> newMap;
                newMap.reserve(32);
                fgPossibleHudless[fIndex].insert_or_assign(This, std::move(newMap));
            }

            LOG_TRACK("Tracking Resource: {:X}, Desc: {:X}", (size_t) capturedBuffer.buffer, BaseDescriptor.ptr);
            fgPossibleHudless[fIndex][This].insert_or_assign(capturedBuffer.buffer, capturedBuffer);
        }
        else
        {
            size_t shardIdx = GetShardIndex(This);
            auto& shard = _hudlessShards[fIndex][shardIdx];

            LOCK_GUARD(shard.mutex);

            if (!shard.map.contains(This))
            {
                ankerl::unordered_dense::map<ID3D12Resource*, ResourceInfo> newMap;
                newMap.reserve(32);
                shard.map.insert_or_assign(This, std::move(newMap));
            }

            LOG_TRACK("CmdList: {:X}, Tracking Resource: {:X}, Desc: {:X}, Format: {}", (size_t) This,
                      (size_t) capturedBuffer.buffer, BaseDescriptor.ptr, (UINT) capturedBuffer.format);

            shard.map[This].insert_or_assign(capturedBuffer.buffer, capturedBuffer);
        }
    }

    o_SetGraphicsRootDescriptorTable(This, RootParameterIndex, BaseDescriptor);
}

#pragma endregion

#pragma region Shader output hooks

void ResTrack_Dx12::hkOMSetRenderTargets(ID3D12GraphicsCommandList* This, UINT NumRenderTargetDescriptors,
                                         D3D12_CPU_DESCRIPTOR_HANDLE* pRenderTargetDescriptors,
                                         BOOL RTsSingleHandleToDescriptorRange,
                                         D3D12_CPU_DESCRIPTOR_HANDLE* pDepthStencilDescriptor)
{
    if (gRRResourceInspectorEnabled.load(std::memory_order_relaxed) && This != MenuOverlayDx::MenuCommandList())
    {
        std::vector<ID3D12Resource*> renderTargets;
        if (NumRenderTargetDescriptors > 0 && pRenderTargetDescriptors != nullptr)
        {
            renderTargets.reserve(NumRenderTargetDescriptors);
            for (UINT index = 0; index < NumRenderTargetDescriptors; ++index)
            {
                std::shared_ptr<HeapInfo> heap;
                SIZE_T handle = 0;
                if (RTsSingleHandleToDescriptorRange)
                {
                    heap = GetHeapByCpuHandleRTV(pRenderTargetDescriptors[0].ptr);
                    if (heap != nullptr)
                    {
                        handle = pRenderTargetDescriptors[0].ptr + static_cast<SIZE_T>(index) * heap->increment;
                    }
                }
                else
                {
                    handle = pRenderTargetDescriptors[index].ptr;
                    heap = GetHeapByCpuHandleRTV(handle);
                }

                ResourceInfo info {};
                if (heap != nullptr && heap->GetByCpuHandle(handle, info) && info.buffer != nullptr)
                    renderTargets.push_back(info.buffer);
            }
        }

        std::scoped_lock psoLock(gRRPsoMutex);
        RRCommandListPsoState& state = gRRCommandListPsoStates[This];
        state.renderTargets = std::move(renderTargets);
        ++state.renderTargetGeneration;
    }

    // Consistent early exit validation
    auto shouldTrack = State::Instance().activeFgInput == FGInput::Upscaler &&
                       !Config::Instance()->FGHudfixDisableOM.value_or_default() && NumRenderTargetDescriptors > 0 &&
                       pRenderTargetDescriptors != nullptr && IsHudFixActive() && !Hudfix_Dx12::SkipHudlessChecks() &&
                       This != MenuOverlayDx::MenuCommandList();

    if (!shouldTrack)
    {
        o_OMSetRenderTargets(This, NumRenderTargetDescriptors, pRenderTargetDescriptors,
                             RTsSingleHandleToDescriptorRange, pDepthStencilDescriptor);
        return;
    }

    LOG_DEBUG_ONLY("NumRenderTargetDescriptors: {}", NumRenderTargetDescriptors);

    auto fIndex = Hudfix_Dx12::ActivePresentFrame() % BUFFER_COUNT;

    // Process render targets
    for (size_t i = 0; i < NumRenderTargetDescriptors; i++)
    {
        std::shared_ptr<HeapInfo> heap;
        D3D12_CPU_DESCRIPTOR_HANDLE handle {};

        // Get the appropriate handle
        if (RTsSingleHandleToDescriptorRange)
        {
            heap = GetHeapByCpuHandleRTV(pRenderTargetDescriptors[0].ptr);
            if (heap == nullptr)
            {
                LOG_DEBUG_ONLY("No heap at index: {}", i);
                continue;
            }

            handle.ptr = pRenderTargetDescriptors[0].ptr + (i * heap->increment);
        }
        else
        {
            handle = pRenderTargetDescriptors[i];
            heap = GetHeapByCpuHandleRTV(handle.ptr);
            if (heap == nullptr)
            {
                LOG_DEBUG_ONLY("No heap at index: {}", i);
                continue;
            }
        }

        ResourceInfo capturedBuffer {};
        if (!heap->GetByCpuHandle(handle.ptr, capturedBuffer) || capturedBuffer.buffer == nullptr)
        {
            LOG_DEBUG_ONLY("No resource at index: {}, cpu: {:X}", i, handle.ptr);
            continue;
        }

        // Valid resource found, update state
        capturedBuffer.state = D3D12_RESOURCE_STATE_RENDER_TARGET;
        capturedBuffer.captureInfo = CaptureInfo::OMSetRTV;

        // Check for immediate capture
        bool capturedImmediately = false;
        if (Config::Instance()->FGImmediateCapture.value_or_default())
        {
            capturedImmediately = Hudfix_Dx12::CheckForHudless(This, &capturedBuffer, capturedBuffer.state);
            if (capturedImmediately)
                break; // Early exit if captured
        }

        // Track for later processing
        if (!capturedImmediately)
        {
            if (!_useShards)
            {
                std::lock_guard<std::mutex> lock(_hudlessTrackMutex);

                if (!fgPossibleHudless[fIndex].contains(This))
                {
                    ankerl::unordered_dense::map<ID3D12Resource*, ResourceInfo> newMap;
                    newMap.reserve(32);
                    fgPossibleHudless[fIndex].insert_or_assign(This, std::move(newMap));
                }

                LOG_TRACK("Tracking Resource: {:X}, Desc: {:X}", (size_t) capturedBuffer.buffer, handle.ptr);
                fgPossibleHudless[fIndex][This].insert_or_assign(capturedBuffer.buffer, capturedBuffer);
            }
            else
            {
                size_t shardIdx = GetShardIndex(This);
                auto& shard = _hudlessShards[fIndex][shardIdx];

                LOCK_GUARD(shard.mutex);

                if (!shard.map.contains(This))
                {
                    ankerl::unordered_dense::map<ID3D12Resource*, ResourceInfo> newMap;
                    newMap.reserve(32);
                    shard.map.insert_or_assign(This, std::move(newMap));
                }

                LOG_TRACK("CmdList: {:X}, Tracking Resource: {:X}, Desc: {:X}, Format: {}", (size_t) This,
                          (size_t) capturedBuffer.buffer, handle.ptr, (UINT) capturedBuffer.format);

                shard.map[This].insert_or_assign(capturedBuffer.buffer, capturedBuffer);
            }
        }
    }

    o_OMSetRenderTargets(This, NumRenderTargetDescriptors, pRenderTargetDescriptors, RTsSingleHandleToDescriptorRange,
                         pDepthStencilDescriptor);
}

#pragma endregion

#pragma region Compute paramter hooks

void ResTrack_Dx12::hkSetComputeRootDescriptorTable(ID3D12GraphicsCommandList* This, UINT RootParameterIndex,
                                                    D3D12_GPU_DESCRIPTOR_HANDLE BaseDescriptor)
{
    // Consistent early exit - always call original function
    auto shouldTrack = !Config::Instance()->FGHudfixDisableSCR.value_or_default() && BaseDescriptor.ptr != 0 &&
                       IsHudFixActive() && !Hudfix_Dx12::SkipHudlessChecks() &&
                       This != MenuOverlayDx::MenuCommandList();

    if (!shouldTrack)
    {
        o_SetComputeRootDescriptorTable(This, RootParameterIndex, BaseDescriptor);
        return;
    }

    auto heap = GetHeapByGpuHandleCR(BaseDescriptor.ptr);
    if (heap == nullptr)
    {
        LOG_DEBUG_ONLY("No heap for handle: {:X}", BaseDescriptor.ptr);
        o_SetComputeRootDescriptorTable(This, RootParameterIndex, BaseDescriptor);
        return;
    }

    ResourceInfo capturedBuffer {};
    if (!heap->GetByGpuHandle(BaseDescriptor.ptr, capturedBuffer) || capturedBuffer.buffer == nullptr)
    {
        LOG_DEBUG_ONLY("No resource at RootParameterIndex: {}, CommandList: {:X}, gpuHandle: {:X}", RootParameterIndex,
                       (SIZE_T) This, BaseDescriptor.ptr);
        o_SetComputeRootDescriptorTable(This, RootParameterIndex, BaseDescriptor);
        return;
    }

    LOG_DEBUG_ONLY("CommandList: {:X}, Resource: {:X}", (size_t) This, (size_t) capturedBuffer.buffer);

    // Only proceed with tracking if we have a valid buffer
    if (capturedBuffer.type == UAV)
        capturedBuffer.state = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    else
        capturedBuffer.state = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;

    capturedBuffer.captureInfo = CaptureInfo::SetCR;

    // Track the resource
    bool capturedImmediately = false;
    if (Config::Instance()->FGImmediateCapture.value_or_default())
    {
        capturedImmediately = Hudfix_Dx12::CheckForHudless(This, &capturedBuffer, capturedBuffer.state);
    }

    if (!capturedImmediately)
    {
        auto fIndex = Hudfix_Dx12::ActivePresentFrame() % BUFFER_COUNT;

        if (!_useShards)
        {
            std::lock_guard<std::mutex> lock(_hudlessTrackMutex);

            if (!fgPossibleHudless[fIndex].contains(This))
            {
                ankerl::unordered_dense::map<ID3D12Resource*, ResourceInfo> newMap;
                newMap.reserve(32);
                fgPossibleHudless[fIndex].insert_or_assign(This, std::move(newMap));
            }

            LOG_TRACK("Tracking Resource: {:X}, Desc: {:X}", (size_t) capturedBuffer.buffer, BaseDescriptor.ptr);
            fgPossibleHudless[fIndex][This].insert_or_assign(capturedBuffer.buffer, capturedBuffer);
        }
        else
        {
            size_t shardIdx = GetShardIndex(This);
            auto& shard = _hudlessShards[fIndex][shardIdx];

            LOCK_GUARD(shard.mutex);

            if (!shard.map.contains(This))
            {
                ankerl::unordered_dense::map<ID3D12Resource*, ResourceInfo> newMap;
                newMap.reserve(32);
                shard.map.insert_or_assign(This, std::move(newMap));
            }

            LOG_TRACK("CmdList: {:X}, Tracking Resource: {:X}, Desc: {:X}, Format: {}", (size_t) This,
                      (size_t) capturedBuffer.buffer, BaseDescriptor.ptr, (UINT) capturedBuffer.format);

            shard.map[This].insert_or_assign(capturedBuffer.buffer, capturedBuffer);
        }
    }

    o_SetComputeRootDescriptorTable(This, RootParameterIndex, BaseDescriptor);
}

#pragma endregion

#pragma region Shader finalizer hooks

// Capture if render target matches, wait for DrawIndexed
void ResTrack_Dx12::hkDrawInstanced(ID3D12GraphicsCommandList* This, UINT VertexCountPerInstance, UINT InstanceCount,
                                    UINT StartVertexLocation, UINT StartInstanceLocation)
{
    o_DrawInstanced(This, VertexCountPerInstance, InstanceCount, StartVertexLocation, StartInstanceLocation);

    if (gRRResourceInspectorEnabled.load(std::memory_order_relaxed) && This != MenuOverlayDx::MenuCommandList())
    {
        std::scoped_lock psoLock(gRRPsoMutex);
        RecordRRGraphicsOutputs(This);
    }

    if (State::Instance().activeFgInput != FGInput::Upscaler || !IsHudFixActive())
    {
        LOG_TRACK("Skipping {:X}", (size_t) This);
        return;
    }

    LOG_TRACK("CmdList: {:X}", (size_t) This);

    auto fIndex = Hudfix_Dx12::ActivePresentFrame() % BUFFER_COUNT;

    if (!_useShards)
    {
        if (This == MenuOverlayDx::MenuCommandList())
        {
            std::lock_guard<std::mutex> lock(_hudlessTrackMutex);
            fgPossibleHudless[fIndex].erase(This);
            return;
        }

        ankerl::unordered_dense::map<ID3D12Resource*, ResourceInfo> val0;
        {
            std::lock_guard<std::mutex> lock(_hudlessTrackMutex);

            if (fgPossibleHudless[fIndex].size() == 0 || !fgPossibleHudless[fIndex].contains(This))
                return;

            val0 = std::move(fgPossibleHudless[fIndex][This]);
            fgPossibleHudless[fIndex].erase(This);
        }

        do
        {
            // if this command list does not have entries skip
            if (val0.size() == 0)
                break;

            if (Config::Instance()->FGHudfixDisableDI.value_or_default())
                break;

            std::lock_guard<std::mutex> lock(_drawMutex);
            for (auto& [key, val] : val0)
            {
                val.captureInfo |= CaptureInfo::DrawInstanced;

                if (Hudfix_Dx12::CheckForHudless(This, &val, val.state))
                    break;
            }

        } while (false);
    }
    else
    {
        size_t shardIdx = GetShardIndex(This);
        auto& shard = _hudlessShards[fIndex][shardIdx];

        if (This == MenuOverlayDx::MenuCommandList() && shard.map.contains(This))
        {
            LOCK_GUARD(shard.mutex);

            shard.map.erase(This);
            return;
        }

        ankerl::unordered_dense::map<ID3D12Resource*, ResourceInfo> val0;
        {

            LOCK_GUARD(shard.mutex);

            // if can't find output skip
            if (shard.map.size() == 0)
            {
                LOG_DEBUG_ONLY("Early exit");
                return;
            }

            if (!shard.map.contains(This))
                return;

            val0 = std::move(shard.map[This]);
            shard.map.erase(This);
        }

        do
        {
            // if this command list does not have entries skip
            if (val0.size() == 0)
                break;

            if (Config::Instance()->FGHudfixDisableDI.value_or_default())
                break;

            for (auto& [key, val] : val0)
            {
                std::lock_guard<std::mutex> lock(_drawMutex);

                val.captureInfo |= CaptureInfo::DrawInstanced;

                if (Hudfix_Dx12::CheckForHudless(This, &val, val.state))
                    break;
            }

        } while (false);
    }
}

void ResTrack_Dx12::hkDrawIndexedInstanced(ID3D12GraphicsCommandList* This, UINT IndexCountPerInstance,
                                           UINT InstanceCount, UINT StartIndexLocation, INT BaseVertexLocation,
                                           UINT StartInstanceLocation)
{
    o_DrawIndexedInstanced(This, IndexCountPerInstance, InstanceCount, StartIndexLocation, BaseVertexLocation,
                           StartInstanceLocation);

    if (gRRResourceInspectorEnabled.load(std::memory_order_relaxed) && This != MenuOverlayDx::MenuCommandList())
    {
        std::scoped_lock psoLock(gRRPsoMutex);
        RecordRRGraphicsOutputs(This);
    }

    if (State::Instance().activeFgInput != FGInput::Upscaler || !IsHudFixActive())
    {
        LOG_TRACK("Skipping CmdList: {:X}", (size_t) This);
        return;
    }

    LOG_TRACK("CmdList: {:X}", (size_t) This);

    auto fIndex = Hudfix_Dx12::ActivePresentFrame() % BUFFER_COUNT;

    if (!_useShards)
    {
        if (This == MenuOverlayDx::MenuCommandList())
        {
            std::lock_guard<std::mutex> lock(_hudlessTrackMutex);
            fgPossibleHudless[fIndex].erase(This);
            return;
        }

        ankerl::unordered_dense::map<ID3D12Resource*, ResourceInfo> val0;
        {
            std::lock_guard<std::mutex> lock(_hudlessTrackMutex);

            if (fgPossibleHudless[fIndex].size() == 0 || !fgPossibleHudless[fIndex].contains(This))
                return;

            val0 = std::move(fgPossibleHudless[fIndex][This]);
            fgPossibleHudless[fIndex].erase(This);
        }

        do
        {
            // if this command list does not have entries skip
            if (val0.size() == 0)
                break;

            if (Config::Instance()->FGHudfixDisableDII.value_or_default())
                break;

            std::lock_guard<std::mutex> lock(_drawMutex);
            for (auto& [key, val] : val0)
            {
                val.captureInfo |= CaptureInfo::DrawIndexedInstanced;

                if (Hudfix_Dx12::CheckForHudless(This, &val, val.state))
                    break;
            }

        } while (false);
    }
    else
    {
        size_t shardIdx = GetShardIndex(This);
        auto& shard = _hudlessShards[fIndex][shardIdx];

        if (This == MenuOverlayDx::MenuCommandList() && shard.map.contains(This))
        {
            LOCK_GUARD(shard.mutex);

            shard.map.erase(This);
            return;
        }

        ankerl::unordered_dense::map<ID3D12Resource*, ResourceInfo> val0;
        {
            LOCK_GUARD(shard.mutex);

            // if can't find output skip
            if (shard.map.size() == 0)
            {
                LOG_DEBUG_ONLY("Early exit");
                return;
            }

            if (!shard.map.contains(This))
                return;

            val0 = std::move(shard.map[This]);
            shard.map.erase(This);
        }

        do
        {
            // if this command list does not have entries skip
            if (val0.size() == 0)
                break;

            if (Config::Instance()->FGHudfixDisableDII.value_or_default())
                break;

            for (auto& [key, val] : val0)
            {
                // LOG_DEBUG("Waiting _drawMutex {:X}", (size_t)val.buffer);
                std::lock_guard<std::mutex> lock(_drawMutex);

                val.captureInfo |= CaptureInfo::DrawIndexedInstanced;

                if (Hudfix_Dx12::CheckForHudless(This, &val, val.state))
                    break;
            }

        } while (false);
    }
}

void ResTrack_Dx12::hkExecuteBundle(ID3D12GraphicsCommandList* This, ID3D12GraphicsCommandList* pCommandList)
{
    LOG_FUNC();

    IFGFeature_Dx12* fg = State::Instance().currentFG;
    auto index = fg != nullptr ? fg->GetIndex() : 0;

    {
        std::lock_guard<std::mutex> lock(_resourceCommandListMutex);

        if (fg != nullptr && fg->IsActive() && (_resourceCommandList[index].size() > 0 || !_resCmdList[index].empty()))
        {
            if (_notFoundCmdLists.contains(pCommandList))
                LOG_WARN("Found last frames cmdList: {:X}", (size_t) This);

            auto& frameCmdList = _resourceCommandList[index];
            for (std::unordered_map<FG_ResourceType, ID3D12GraphicsCommandList*>::iterator it = frameCmdList.begin();
                 it != frameCmdList.end(); ++it)
            {
                if (it->second == pCommandList)
                    it->second = This;
            }

            for (std::unordered_map<FG_ResourceType, void*>::iterator it = _resCmdList[index].begin();
                 it != _resCmdList[index].end(); ++it)
            {
                if (it->second == pCommandList)
                    it->second = This;
            }
        }
    }

    o_ExecuteBundle(This, pCommandList);
}

HRESULT ResTrack_Dx12::hkClose(ID3D12GraphicsCommandList* This)
{
    auto fg = State::Instance().currentFG;
    auto index = fg != nullptr ? fg->GetIndex() : 0;

    if (fg != nullptr && fg->IsActive() && !fg->IsPaused() && _resourceCommandList[index].size() > 0)
    {
        LOG_TRACK("CmdList: {:X}", (size_t) This);

        std::lock_guard<std::mutex> lock(_resourceCommandListMutex);

        if (_notFoundCmdLists.contains(This))
            LOG_WARN("Found last frames cmdList: {:X}", (size_t) This);

        std::vector<FG_ResourceType> found;

        for (const auto& pair : _resourceCommandList[index])
        {
            if (This == pair.second)
            {
                if (!fg->IsResourceReady(pair.first))
                {
                    LOG_DEBUG("{} cmdList: {:X}", (UINT) pair.first, (size_t) This);
                    _resCmdList[index][pair.first] = pair.second;
                    found.push_back(pair.first);
                }
            }
        }

        for (size_t i = 0; i < found.size(); i++)
        {
            _resourceCommandList[index].erase(found[i]);
        }
    }

    if (gRRResourceInspectorEnabled.load(std::memory_order_relaxed))
    {
        {
            std::scoped_lock markerLock(gRRMarkerMutex);
            gRRCommandListMarkers.erase(This);
        }
        {
            std::scoped_lock psoLock(gRRPsoMutex);
            gRRCommandListPsoStates.erase(This);
        }
    }

    return o_Close(This);
}

void ResTrack_Dx12::hkDispatch(ID3D12GraphicsCommandList* This, UINT ThreadGroupCountX, UINT ThreadGroupCountY,
                               UINT ThreadGroupCountZ)
{
    o_Dispatch(This, ThreadGroupCountX, ThreadGroupCountY, ThreadGroupCountZ);

    if (gRRResourceInspectorEnabled.load(std::memory_order_relaxed) && This != MenuOverlayDx::MenuCommandList())
    {
        std::scoped_lock psoLock(gRRPsoMutex);
        RRCommandListPsoState& state = gRRCommandListPsoStates[This];
        if (state.currentPso != nullptr)
        {
            RRPsoMetadata& metadata = EnsureRRPsoMetadata(state.currentPso);
            if (metadata.kind == RRPsoKind::Unknown || metadata.kind == RRPsoKind::Stream)
                metadata.kind = RRPsoKind::Compute;
            state.lastComputePso = state.currentPso;
            ++state.dispatchesSinceBarrier;
        }
    }

    if (State::Instance().activeFgInput != FGInput::Upscaler || !IsHudFixActive())
    {
        LOG_TRACK("Skipping {:X}", (size_t) This);
        return;
    }

    LOG_TRACK("CmdList: {:X}", (size_t) This);

    auto fIndex = Hudfix_Dx12::ActivePresentFrame() % BUFFER_COUNT;

    if (!_useShards)
    {
        if (This == MenuOverlayDx::MenuCommandList())
        {
            std::lock_guard<std::mutex> lock(_hudlessTrackMutex);
            fgPossibleHudless[fIndex].erase(This);
            return;
        }

        ankerl::unordered_dense::map<ID3D12Resource*, ResourceInfo> val0;
        {
            std::lock_guard<std::mutex> lock(_hudlessTrackMutex);

            if (fgPossibleHudless[fIndex].size() == 0 || !fgPossibleHudless[fIndex].contains(This))
                return;

            val0 = std::move(fgPossibleHudless[fIndex][This]);
            fgPossibleHudless[fIndex].erase(This);
        }

        do
        {
            // if this command list does not have entries skip
            if (val0.size() == 0)
                break;

            if (Config::Instance()->FGHudfixDisableDispatch.value_or_default())
                break;

            std::lock_guard<std::mutex> lock(_drawMutex);
            for (auto& [key, val] : val0)
            {
                val.captureInfo |= CaptureInfo::Dispatch;

                if (Hudfix_Dx12::CheckForHudless(This, &val, val.state))
                    break;
            }
        } while (false);
    }
    else
    {
        size_t shardIdx = GetShardIndex(This);
        auto& shard = _hudlessShards[fIndex][shardIdx];

        if (This == MenuOverlayDx::MenuCommandList() && shard.map.contains(This))
        {
            LOCK_GUARD(shard.mutex);

            shard.map.erase(This);
            return;
        }

        ankerl::unordered_dense::map<ID3D12Resource*, ResourceInfo> val0;
        {

            LOCK_GUARD(shard.mutex);

            // if can't find output skip
            if (shard.map.size() == 0)
            {
                LOG_DEBUG_ONLY("Early exit");
                return;
            }

            if (!shard.map.contains(This))
                return;

            val0 = std::move(shard.map[This]);
            shard.map.erase(This);
        }

        do
        {
            // if this command list does not have entries skip
            if (val0.size() == 0)
                break;

            if (Config::Instance()->FGHudfixDisableDispatch.value_or_default())
                break;

            for (auto& [key, val] : val0)
            {
                // LOG_DEBUG("Waiting _drawMutex {:X}", (size_t)val.buffer);
                std::lock_guard<std::mutex> lock(_drawMutex);

                val.captureInfo |= CaptureInfo::Dispatch;
                if (Hudfix_Dx12::CheckForHudless(This, &val, val.state))
                {
                    break;
                }
            }
        } while (false);
    }
}

#pragma endregion

void ResTrack_Dx12::HookResource(ID3D12Device* InDevice)
{
    if (o_Release != nullptr)
        return;

    ID3D12Resource* tmp = nullptr;
    auto d = CD3DX12_RESOURCE_DESC::Buffer(4);
    auto heapProps = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD);

    HRESULT hr = InDevice->CreateCommittedResource(&heapProps, D3D12_HEAP_FLAG_NONE, &d,
                                                   D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&tmp));

    if (hr == S_OK)
    {
        PVOID* pVTable = *(PVOID**) tmp;
        gResourceReleaseEntry = pVTable[2];
        o_Release = (PFN_Release) pVTable[2];

        if (o_Release != nullptr)
        {
            DetourTransactionBegin();
            DetourUpdateThread(GetCurrentThread());
            DetourAttach(&(PVOID&) o_Release, hkRelease);
            auto detourResult = DetourTransactionCommit();

            if (detourResult != NO_ERROR)
            {
                LOG_ERROR("Failed to hook Heap Release: {:X}", detourResult);
                o_Release = nullptr;
                gResourceReleaseEntry = nullptr;
                tmp->Release();
            }
            else
            {
                o_Release(tmp); // drop temp
            }
        }
        else
        {
            tmp->Release();
        }
    }
}

void ResTrack_Dx12::HookCommandList(ID3D12Device* InDevice)
{

    if (o_OMSetRenderTargets != nullptr)
        return;

    ID3D12GraphicsCommandList* commandList = nullptr;
    ID3D12CommandAllocator* commandAllocator = nullptr;

    if (InDevice->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&commandAllocator)) == S_OK)
    {
        if (InDevice->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, commandAllocator, nullptr,
                                        IID_PPV_ARGS(&commandList)) == S_OK)
        {
            ID3D12GraphicsCommandList* realCL = nullptr;
            if (!CheckForRealObject(__FUNCTION__, commandList, (IUnknown**) &realCL))
                realCL = commandList;

            // Get the vtable pointer
            PVOID* pVTable = *(PVOID**) realCL;

            // hudless shader
            o_OMSetRenderTargets = (PFN_OMSetRenderTargets) pVTable[46];
            o_SetGraphicsRootDescriptorTable = (PFN_SetGraphicsRootDescriptorTable) pVTable[32];

            o_DrawInstanced = (PFN_DrawInstanced) pVTable[12];
            o_DrawIndexedInstanced = (PFN_DrawIndexedInstanced) pVTable[13];
            o_Dispatch = (PFN_Dispatch) pVTable[14];
            o_Close = (PFN_Close) pVTable[9];
            o_Reset = (PFN_Reset) pVTable[10];
            o_SetPipelineState = (PFN_SetPipelineState) pVTable[25];
            o_ResourceBarrier = (PFN_ResourceBarrier) pVTable[26];
            o_SetMarker = (PFN_SetMarker) pVTable[56];
            o_BeginEvent = (PFN_BeginEvent) pVTable[57];
            o_EndEvent = (PFN_EndEvent) pVTable[58];

            // hudless compute
            o_SetComputeRootDescriptorTable = (PFN_SetComputeRootDescriptorTable) pVTable[31];

            o_ExecuteBundle = (PFN_ExecuteBundle) pVTable[27];

            if (o_OMSetRenderTargets != nullptr)
            {
                DetourTransactionBegin();
                DetourUpdateThread(GetCurrentThread());

                if (o_OMSetRenderTargets != nullptr)
                    DetourAttach(&(PVOID&) o_OMSetRenderTargets, hkOMSetRenderTargets);

                if (o_DrawIndexedInstanced != nullptr)
                    DetourAttach(&(PVOID&) o_DrawIndexedInstanced, hkDrawIndexedInstanced);

                if (o_DrawInstanced != nullptr)
                    DetourAttach(&(PVOID&) o_DrawInstanced, hkDrawInstanced);

                if (o_Dispatch != nullptr)
                    DetourAttach(&(PVOID&) o_Dispatch, hkDispatch);

                // Root descriptor tracking is only needed by the HUD fix.
                if (State::Instance().activeFgInput == FGInput::Upscaler)
                {
                    if (o_SetGraphicsRootDescriptorTable != nullptr)
                        DetourAttach(&(PVOID&) o_SetGraphicsRootDescriptorTable, hkSetGraphicsRootDescriptorTable);

                    if (o_SetComputeRootDescriptorTable != nullptr)
                        DetourAttach(&(PVOID&) o_SetComputeRootDescriptorTable, hkSetComputeRootDescriptorTable);
                }

                if (o_Close != nullptr)
                    DetourAttach(&(PVOID&) o_Close, hkClose);

                if (o_Reset != nullptr)
                    DetourAttach(&(PVOID&) o_Reset, hkReset);

                if (o_SetPipelineState != nullptr)
                    DetourAttach(&(PVOID&) o_SetPipelineState, hkSetPipelineState);

                if (o_ResourceBarrier != nullptr)
                    DetourAttach(&(PVOID&) o_ResourceBarrier, hkResourceBarrier);

                if (o_SetMarker != nullptr)
                    DetourAttach(&(PVOID&) o_SetMarker, hkSetMarker);

                if (o_BeginEvent != nullptr)
                    DetourAttach(&(PVOID&) o_BeginEvent, hkBeginEvent);

                if (o_EndEvent != nullptr)
                    DetourAttach(&(PVOID&) o_EndEvent, hkEndEvent);

                if (o_ExecuteBundle != nullptr)
                    DetourAttach(&(PVOID&) o_ExecuteBundle, hkExecuteBundle);

                auto detourResult = DetourTransactionCommit();
                if (detourResult != NO_ERROR)
                {
                    LOG_ERROR("Failed to hook CommandList methods: {:X}", detourResult);
                    o_OMSetRenderTargets = nullptr;
                    o_SetGraphicsRootDescriptorTable = nullptr;
                    o_DrawInstanced = nullptr;
                    o_DrawIndexedInstanced = nullptr;
                    o_Dispatch = nullptr;
                    o_Close = nullptr;
                    o_SetComputeRootDescriptorTable = nullptr;
                    o_ExecuteBundle = nullptr;
                }
            }

            commandList->Close();
            commandList->Release();
        }

        commandAllocator->Reset();
        commandAllocator->Release();
    }
}

void ResTrack_Dx12::HookToQueue(ID3D12Device* InDevice)
{
    if (o_ExecuteCommandLists != nullptr)
        return;

    ID3D12CommandQueue* queue = nullptr;
    D3D12_COMMAND_QUEUE_DESC queueDesc = {};
    queueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    queueDesc.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;
    queueDesc.NodeMask = 0;
    queueDesc.Priority = D3D12_COMMAND_QUEUE_PRIORITY_NORMAL;

    auto hr = InDevice->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&queue));

    if (hr == S_OK)
    {
        ID3D12CommandQueue* realQueue = nullptr;
        if (!CheckForRealObject(__FUNCTION__, queue, (IUnknown**) &realQueue))
            realQueue = queue;

        // Get the vtable pointer
        PVOID* pVTable = *(PVOID**) realQueue;

        o_ExecuteCommandLists = (PFN_ExecuteCommandLists) pVTable[10];

        DetourTransactionBegin();
        DetourUpdateThread(GetCurrentThread());

        if (o_ExecuteCommandLists != nullptr)
            DetourAttach(&(PVOID&) o_ExecuteCommandLists, hkExecuteCommandLists);

        auto detourResult = DetourTransactionCommit();
        if (detourResult != NO_ERROR)
        {
            LOG_ERROR("Failed to hook CommandList methods: {:X}", detourResult);
            o_ExecuteCommandLists = nullptr;
        }

        queue->Release();
    }
}

void ResTrack_Dx12::HookDevice(ID3D12Device* device)
{
    if (o_CreateDescriptorHeap != nullptr || State::Instance().activeFgInput == FGInput::NvngxFG)
        return;

    if (device == nullptr)
        return;

    bool initializeTracking = false;
    {
        std::unique_lock lock(_heapRegistryMutex);
        if (fgHeaps.capacity() < 65536)
        {
            fgHeaps.reserve(65536);
            initializeTracking = true;
        }
    }

    if (initializeTracking)
    {
        _useShards = Config::Instance()->FGUseShards.value_or_default();
        std::scoped_lock lock(_trackedResourcesMutex);
        _trackedResources.reserve(1024);
    }

    LOG_FUNC();

    ID3D12Device* realDevice = nullptr;
    if (!CheckForRealObject(__FUNCTION__, device, (IUnknown**) &realDevice))
        realDevice = device;

    // Get the vtable pointer
    PVOID* pVTable = *(PVOID**) realDevice;

    // Hudfix
    o_CreateDescriptorHeap = (PFN_CreateDescriptorHeap) pVTable[14];
    o_CreateGraphicsPipelineState = (PFN_CreateGraphicsPipelineState) pVTable[10];
    o_CreateComputePipelineState = (PFN_CreateComputePipelineState) pVTable[11];
    o_CreateShaderResourceView = (PFN_CreateShaderResourceView) pVTable[18];
    o_CreateUnorderedAccessView = (PFN_CreateUnorderedAccessView) pVTable[19];
    o_CreateRenderTargetView = (PFN_CreateRenderTargetView) pVTable[20];
    o_CreateSampler = (PFN_CreateSampler) pVTable[22];
    o_CopyDescriptors = (PFN_CopyDescriptors) pVTable[23];
    o_CopyDescriptorsSimple = (PFN_CopyDescriptorsSimple) pVTable[24];

    ID3D12Device2* device2 = nullptr;
    if (SUCCEEDED(realDevice->QueryInterface(IID_PPV_ARGS(&device2))) && device2 != nullptr)
    {
        PVOID* device2VTable = *(PVOID**) device2;
        o_CreatePipelineState = (PFN_CreatePipelineState) device2VTable[47];
    }

    // o_CreateDepthStencilView = (PFN_CreateDepthStencilView) pVTable[21];
    // o_CreateConstantBufferView = (PFN_CreateConstantBufferView) pVTable[17];

    // Apply the detour

    if (o_CreateDescriptorHeap != nullptr)
    {
        DetourTransactionBegin();
        DetourUpdateThread(GetCurrentThread());

        if (o_CreateDescriptorHeap != nullptr)
            DetourAttach(&(PVOID&) o_CreateDescriptorHeap, hkCreateDescriptorHeap);

        if (o_CreateGraphicsPipelineState != nullptr)
            DetourAttach(&(PVOID&) o_CreateGraphicsPipelineState, hkCreateGraphicsPipelineState);

        if (o_CreateComputePipelineState != nullptr)
            DetourAttach(&(PVOID&) o_CreateComputePipelineState, hkCreateComputePipelineState);

        if (o_CreatePipelineState != nullptr)
            DetourAttach(&(PVOID&) o_CreatePipelineState, hkCreatePipelineState);

        if (o_CreateRenderTargetView != nullptr)
            DetourAttach(&(PVOID&) o_CreateRenderTargetView, hkCreateRenderTargetView);

        if (o_CreateShaderResourceView != nullptr)
            DetourAttach(&(PVOID&) o_CreateShaderResourceView, hkCreateShaderResourceView);

        if (o_CreateUnorderedAccessView != nullptr)
            DetourAttach(&(PVOID&) o_CreateUnorderedAccessView, hkCreateUnorderedAccessView);

        if (o_CopyDescriptors != nullptr)
            DetourAttach(&(PVOID&) o_CopyDescriptors, hkCopyDescriptors);

        if (o_CopyDescriptorsSimple != nullptr)
            DetourAttach(&(PVOID&) o_CopyDescriptorsSimple, hkCopyDescriptorsSimple);

        auto detourResult = DetourTransactionCommit();
        if (detourResult != NO_ERROR)
        {
            LOG_ERROR("Failed to hook Descriptor methods: {:X}", detourResult);
            o_CreateDescriptorHeap = nullptr;
            o_CreateRenderTargetView = nullptr;
            o_CreateShaderResourceView = nullptr;
            o_CreateUnorderedAccessView = nullptr;
            o_CopyDescriptors = nullptr;
            o_CopyDescriptorsSimple = nullptr;
        }
    }

    if (device2 != nullptr)
        device2->Release();

    HookToQueue(device);
    HookCommandList(device);
    HookResource(device);
}

void ResTrack_Dx12::ReleaseDeviceHooks()
{
    LOG_DEBUG("");

    DetourTransactionBegin();
    DetourUpdateThread(GetCurrentThread());

    if (o_CreateDescriptorHeap != nullptr)
        DetourDetach(&(PVOID&) o_CreateDescriptorHeap, hkCreateDescriptorHeap);

    if (o_CreateGraphicsPipelineState != nullptr)
        DetourDetach(&(PVOID&) o_CreateGraphicsPipelineState, hkCreateGraphicsPipelineState);

    if (o_CreateComputePipelineState != nullptr)
        DetourDetach(&(PVOID&) o_CreateComputePipelineState, hkCreateComputePipelineState);

    if (o_CreatePipelineState != nullptr)
        DetourDetach(&(PVOID&) o_CreatePipelineState, hkCreatePipelineState);

    if (o_CreateRenderTargetView != nullptr)
        DetourDetach(&(PVOID&) o_CreateRenderTargetView, hkCreateRenderTargetView);

    if (o_CreateShaderResourceView != nullptr)
        DetourDetach(&(PVOID&) o_CreateShaderResourceView, hkCreateShaderResourceView);

    if (o_CreateUnorderedAccessView != nullptr)
        DetourDetach(&(PVOID&) o_CreateUnorderedAccessView, hkCreateUnorderedAccessView);

    if (o_CopyDescriptors != nullptr)
        DetourDetach(&(PVOID&) o_CopyDescriptors, hkCopyDescriptors);

    if (o_CopyDescriptorsSimple != nullptr)
        DetourDetach(&(PVOID&) o_CopyDescriptorsSimple, hkCopyDescriptorsSimple);

    // Queue
    if (o_ExecuteCommandLists != nullptr)
        DetourDetach(&(PVOID&) o_ExecuteCommandLists, hkExecuteCommandLists);

    // CommandList
    if (o_OMSetRenderTargets != nullptr)
        DetourDetach(&(PVOID&) o_OMSetRenderTargets, hkOMSetRenderTargets);

    if (o_SetGraphicsRootDescriptorTable != nullptr)
        DetourDetach(&(PVOID&) o_SetGraphicsRootDescriptorTable, hkSetGraphicsRootDescriptorTable);

    if (o_SetComputeRootDescriptorTable != nullptr)
        DetourDetach(&(PVOID&) o_SetComputeRootDescriptorTable, hkSetComputeRootDescriptorTable);

    if (o_DrawIndexedInstanced != nullptr)
        DetourDetach(&(PVOID&) o_DrawIndexedInstanced, hkDrawIndexedInstanced);

    if (o_DrawInstanced != nullptr)
        DetourDetach(&(PVOID&) o_DrawInstanced, hkDrawInstanced);

    if (o_Dispatch != nullptr)
        DetourDetach(&(PVOID&) o_Dispatch, hkDispatch);

    if (o_Close != nullptr)
        DetourDetach(&(PVOID&) o_Close, hkClose);

    if (o_Reset != nullptr)
        DetourDetach(&(PVOID&) o_Reset, hkReset);

    if (o_SetPipelineState != nullptr)
        DetourDetach(&(PVOID&) o_SetPipelineState, hkSetPipelineState);

    if (o_ResourceBarrier != nullptr)
        DetourDetach(&(PVOID&) o_ResourceBarrier, hkResourceBarrier);

    if (o_SetMarker != nullptr)
        DetourDetach(&(PVOID&) o_SetMarker, hkSetMarker);

    if (o_BeginEvent != nullptr)
        DetourDetach(&(PVOID&) o_BeginEvent, hkBeginEvent);

    if (o_EndEvent != nullptr)
        DetourDetach(&(PVOID&) o_EndEvent, hkEndEvent);

    if (o_ExecuteBundle != nullptr)
        DetourDetach(&(PVOID&) o_ExecuteBundle, hkExecuteBundle);

    // Resource
    if (o_Release != nullptr)
        DetourDetach(&(PVOID&) o_Release, hkRelease);

    if (o_PsoRelease != nullptr)
        DetourDetach(&(PVOID&) o_PsoRelease, hkPsoRelease);

    DetourTransactionCommit();

    // Device
    o_CreateDescriptorHeap = nullptr;
    o_CreateGraphicsPipelineState = nullptr;
    o_CreateComputePipelineState = nullptr;
    o_CreatePipelineState = nullptr;
    o_CreateRenderTargetView = nullptr;
    o_CreateShaderResourceView = nullptr;
    o_CreateUnorderedAccessView = nullptr;
    o_CopyDescriptors = nullptr;
    o_CopyDescriptorsSimple = nullptr;

    // Queue
    o_ExecuteCommandLists = nullptr;

    // CommandList
    o_OMSetRenderTargets = nullptr;
    o_SetGraphicsRootDescriptorTable = nullptr;
    o_SetComputeRootDescriptorTable = nullptr;
    o_DrawIndexedInstanced = nullptr;
    o_DrawInstanced = nullptr;
    o_Dispatch = nullptr;
    o_Close = nullptr;
    o_Reset = nullptr;
    o_SetPipelineState = nullptr;
    o_ResourceBarrier = nullptr;
    o_SetMarker = nullptr;
    o_BeginEvent = nullptr;
    o_EndEvent = nullptr;
    o_ExecuteBundle = nullptr;

    // Resource
    o_Release = nullptr;
    o_PsoRelease = nullptr;
    gResourceReleaseEntry = nullptr;

    {
        std::scoped_lock resourceLock(gRRTrackedResourceMutex);
        gRRTrackedResources.clear();
    }
    {
        std::scoped_lock stateLock(gRRResourceStateMutex);
        gRRResourceStates.clear();
        gRRResourceActivity.clear();
    }
    {
        std::scoped_lock candidateLock(gRRResourceCandidateMutex);
        gRRResourceCandidates.clear();
    }
    {
        std::scoped_lock markerLock(gRRMarkerMutex);
        gRRCommandListMarkers.clear();
        gRRMarkerInventory.clear();
    }
    {
        std::scoped_lock psoLock(gRRPsoMutex);
        gRRPsoMetadata.clear();
        gRRCommandListPsoStates.clear();
        gRRResourcePsoProducers.clear();
        gRRNextPsoId.store(1, std::memory_order_release);
    }
    gRRResourceCandidateAddress.store(0, std::memory_order_release);
}

void ResTrack_Dx12::ReleaseHooks()
{
    LOG_DEBUG("");

    DetourTransactionBegin();
    DetourUpdateThread(GetCurrentThread());

    // if (o_CreateDescriptorHeap != nullptr)
    //     DetourDetach(&(PVOID&) o_CreateDescriptorHeap, hkCreateDescriptorHeap);

    // if (o_CreateRenderTargetView != nullptr)
    //     DetourDetach(&(PVOID&) o_CreateRenderTargetView, hkCreateRenderTargetView);

    // if (o_CreateShaderResourceView != nullptr)
    //     DetourDetach(&(PVOID&) o_CreateShaderResourceView, hkCreateShaderResourceView);

    // if (o_CreateUnorderedAccessView != nullptr)
    //     DetourDetach(&(PVOID&) o_CreateUnorderedAccessView, hkCreateUnorderedAccessView);

    // if (o_CopyDescriptors != nullptr)
    //     DetourDetach(&(PVOID&) o_CopyDescriptors, hkCopyDescriptors);

    // if (o_CopyDescriptorsSimple != nullptr)
    //     DetourDetach(&(PVOID&) o_CopyDescriptorsSimple, hkCopyDescriptorsSimple);

    // o_CreateDescriptorHeap = nullptr;
    // o_CreateRenderTargetView = nullptr;
    // o_CreateShaderResourceView = nullptr;
    // o_CreateUnorderedAccessView = nullptr;
    // o_CopyDescriptors = nullptr;
    // o_CopyDescriptorsSimple = nullptr;

    // if (o_ExecuteCommandLists != nullptr)
    //     DetourAttach(&(PVOID&) o_ExecuteCommandLists, hkExecuteCommandLists);

    // o_ExecuteCommandLists = nullptr;

    // if (o_Release != nullptr)
    //     DetourAttach(&(PVOID&) o_Release, hkRelease);

    // o_Release = nullptr;

    if (o_OMSetRenderTargets != nullptr)
        DetourDetach(&(PVOID&) o_OMSetRenderTargets, hkOMSetRenderTargets);

    if (o_SetGraphicsRootDescriptorTable != nullptr)
        DetourDetach(&(PVOID&) o_SetGraphicsRootDescriptorTable, hkSetGraphicsRootDescriptorTable);

    if (o_SetComputeRootDescriptorTable != nullptr)
        DetourDetach(&(PVOID&) o_SetComputeRootDescriptorTable, hkSetComputeRootDescriptorTable);

    if (o_DrawIndexedInstanced != nullptr)
        DetourDetach(&(PVOID&) o_DrawIndexedInstanced, hkDrawIndexedInstanced);

    if (o_DrawInstanced != nullptr)
        DetourDetach(&(PVOID&) o_DrawInstanced, hkDrawInstanced);

    if (o_Dispatch != nullptr)
        DetourDetach(&(PVOID&) o_Dispatch, hkDispatch);

    if (o_Close != nullptr)
        DetourDetach(&(PVOID&) o_Close, hkClose);

    if (o_Reset != nullptr)
        DetourDetach(&(PVOID&) o_Reset, hkReset);

    if (o_SetPipelineState != nullptr)
        DetourDetach(&(PVOID&) o_SetPipelineState, hkSetPipelineState);

    if (o_ResourceBarrier != nullptr)
        DetourDetach(&(PVOID&) o_ResourceBarrier, hkResourceBarrier);

    if (o_SetMarker != nullptr)
        DetourDetach(&(PVOID&) o_SetMarker, hkSetMarker);

    if (o_BeginEvent != nullptr)
        DetourDetach(&(PVOID&) o_BeginEvent, hkBeginEvent);

    if (o_EndEvent != nullptr)
        DetourDetach(&(PVOID&) o_EndEvent, hkEndEvent);

    if (o_ExecuteBundle != nullptr)
        DetourDetach(&(PVOID&) o_ExecuteBundle, hkExecuteBundle);

    o_OMSetRenderTargets = nullptr;
    o_SetGraphicsRootDescriptorTable = nullptr;
    o_SetComputeRootDescriptorTable = nullptr;
    o_DrawIndexedInstanced = nullptr;
    o_DrawInstanced = nullptr;
    o_Dispatch = nullptr;
    o_Close = nullptr;
    o_Reset = nullptr;
    o_SetPipelineState = nullptr;
    o_ResourceBarrier = nullptr;
    o_SetMarker = nullptr;
    o_BeginEvent = nullptr;
    o_EndEvent = nullptr;
    o_ExecuteBundle = nullptr;

    DetourTransactionCommit();
}

void ResTrack_Dx12::ClearPossibleHudless()
{
    LOG_DEBUG("");

    auto hfIndex = Hudfix_Dx12::ActivePresentFrame() % BUFFER_COUNT;

    if (!_useShards)
    {
        std::lock_guard<std::mutex> lock(_hudlessTrackMutex);
        fgPossibleHudless[hfIndex].clear();
    }
    else
    {
        for (size_t i = 0; i < SHARD_COUNT; i++)
        {
            auto& shard = _hudlessShards[hfIndex][i];

            LOCK_GUARD(shard.mutex);

            shard.map.clear();
        }
    }

    std::lock_guard<std::mutex> lock2(_resourceCommandListMutex);

    auto fg = State::Instance().currentFG;
    if (fg != nullptr)
    {
        auto fIndex = fg->GetIndex();

        if (_notFoundCmdLists.size() > 10)
            _notFoundCmdLists.clear();

        for (const auto& pair : _resourceCommandList[fIndex])
        {
            LOG_WARN("{} cmdList: {:X}, not closed!", (UINT) pair.first, (size_t) pair.second);
            _notFoundCmdLists.insert(pair.second);
        }

        _resourceCommandList[fIndex].clear();

        for (const auto& pair : _resCmdList[fIndex])
        {
            LOG_WARN("{} cmdList: {:X}, not executed!", (UINT) pair.first, (size_t) pair.second);
            _notFoundCmdLists.insert(pair.second);
        }

        _resCmdList[fIndex].clear();
    }
}

void ResTrack_Dx12::SetResourceCmdList(FG_ResourceType type, ID3D12GraphicsCommandList* cmdList)
{
    auto fg = State::Instance().currentFG;
    if (fg != nullptr && fg->IsActive())
    {
        auto index = fg->GetIndex();

        ID3D12GraphicsCommandList* realCmdList = nullptr;
        if (!CheckForRealObject(__FUNCTION__, cmdList, (IUnknown**) &realCmdList))
            realCmdList = cmdList;

        _resourceCommandList[index][type] = realCmdList;
        LOG_DEBUG("_resourceCommandList[{}][{}]: {:X}", index, magic_enum::enum_name(type), (size_t) realCmdList);
    }
}
