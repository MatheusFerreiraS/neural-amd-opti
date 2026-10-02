#pragma once
#include "AmdPreSr.h"
#include "ResidualStabilizer.h"
#include "StallWatch.h"
#include <Config.h>
#include <Logger.h>
#include <wrl/client.h>
#include <d3dcompiler.h>
#include <d3d11_4.h>
#include <dxgi1_4.h>
#include <cstring>
#include <algorithm>
#include <cmath>
#include <mutex>
#include <memory>
#include <stdexcept>
#include <vector>
#define ffxGetCommandListDX12 OptiFfxGetCommandListDX12
#define ffxGetDeviceDX12 OptiFfxGetDeviceDX12
#define ffxGetResourceDX12 OptiFfxGetResourceDX12
#include "../../../external/FidelityFX-SDK-v2/Kits/FidelityFX/backend/dx12/ffx_dx12.h"
#include "../../../external/FidelityFX-SDK-v2/Kits/FidelityFX/framegeneration/fsr3/include/ffx_opticalflow.h"
#undef ffxGetCommandListDX12
#undef ffxGetDeviceDX12
#undef ffxGetResourceDX12
namespace AmdPresentExperimental
{
using Microsoft::WRL::ComPtr;
inline bool IsTarget()
{
    return Config::Instance()->DlssNrPresent.value_or_default();
}
inline std::string status = "Final-image NR: waiting for a D3D11, D3D12 or Vulkan frame";
inline std::mutex mutex;
// The runtime loads once per process and stays on the device it was built on.
inline bool runtimeLoaded = false;
inline void Check(HRESULT h)
{
    if (FAILED(h))
        throw std::runtime_error("Final-image NR D3D12 failure: " + std::to_string((UINT) h));
}
inline void Transition(ID3D12GraphicsCommandList* c, ID3D12Resource* r, D3D12_RESOURCE_STATES a,
                       D3D12_RESOURCE_STATES b)
{
    D3D12_RESOURCE_BARRIER v {};
    v.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    v.Transition = { r, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, a, b };
    c->ResourceBarrier(1, &v);
}
struct Context
{
    ComPtr<ID3D12Device> device;
    ComPtr<ID3D12CommandQueue> queue;
    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList> cmd;
    ComPtr<ID3D12Fence> fence;
    UINT64 serial = 0;
    ComPtr<ID3D12Resource> input, motion, depth, output, heldBack;
    // Guides frame generation may still hold from before a resize, kept until two frames after it.
    ComPtr<ID3D12Resource> retired[2];
    UINT64 retiredUntil = 0;
    UINT busyFrames = 0;
    bool failedReported = false, formatReported = false;
    // The D3D11 routes let the GPU run a frame's NR while the game prepares the next one; the runtime then skips a
    // frame it has no free slot for. Otherwise every frame waits for its NR before Present.
    bool pipelined = false;
    ComPtr<ID3D12DescriptorHeap> heap, clearCpu;
    ComPtr<ID3D12RootSignature> root;
    ComPtr<ID3D12PipelineState> pipeline;
    ComPtr<ID3D12RootSignature> graphicsRoot;
    ComPtr<ID3D12PipelineState> graphicsPipeline;
    ComPtr<ID3D12DescriptorHeap> rtvHeap;
    FfxOpticalflowContext flowContext {};
    std::vector<unsigned char> flowScratch;
    ComPtr<ID3D12Resource> flowInput, flowSparse, flowScd;
    ComPtr<ID3D12Resource> flowScdReadback;
    ComPtr<ID3D12DescriptorHeap> flowHeap;
    ComPtr<ID3D12RootSignature> flowRoot;
    ComPtr<ID3D12PipelineState> flowDensify;
    bool flowCreated = false, flowFailed = false;
    bool sceneReadbackPending = false;
    UINT flowFrames = 0;
    AmdPreSr::Backend* backend = nullptr;
    std::unique_ptr<AmdPreSr::ResidualStabilizer> stabilizer;
    bool stableLast = false;
    bool lastFlowValid = false, lastReset = false, lastNeural = true;
    ULONGLONG lastModifiedTick = 0;
    UINT width = 0, height = 0;
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
    bool stopped = false;
    bool WaitForFrame()
    {
        const auto completed = fence->GetCompletedValue();
        if (completed == UINT64_MAX)
            return false;
        if (completed >= serial)
            return true;
        HANDLE done = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (!done)
            return false;
        const bool signaled = SUCCEEDED(fence->SetEventOnCompletion(serial, done)) &&
                              WaitForSingleObject(done, 4000) == WAIT_OBJECT_0;
        CloseHandle(done);
        return signaled;
    }
    void ReleaseBackBuffer()
    {
        if (WaitForFrame())
            heldBack.Reset();
        else
        {
            stopped = true;
            status = "Final-image NR: GPU timeout; backbuffer retained until process exit";
        }
    }
    void BeforeResize()
    {
        ReleaseBackBuffer();
        if (stopped)
            return;
        if (flowCreated)
            ffxOpticalflowContextDestroy(&flowContext);
        flowCreated = false;
        flowFrames = 0;
        sceneReadbackPending = false;
        flowInput.Reset();
        flowSparse.Reset();
        flowScd.Reset();
        flowScdReadback.Reset();
        input.Reset();
        output.Reset();
        if (motion)
        {
            retired[0] = std::move(motion);
            retired[1] = std::move(depth);
            retiredUntil = serial + 2;
        }
        width = height = 0;
        format = DXGI_FORMAT_UNKNOWN;
        if (backend)
            backend->InvalidateHistory();
        if (stabilizer)
            stabilizer->Invalidate();
        stableLast = false;
        lastFlowValid = false;
    }
    Context(ID3D12Device* d, ID3D12CommandQueue* q) : device(d), queue(q)
    {
        Check(d->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)));
        Check(d->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr, IID_PPV_ARGS(&cmd)));
        Check(cmd->Close());
        Check(d->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)));
        D3D12_DESCRIPTOR_HEAP_DESC hd {};
        hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        hd.NumDescriptors = 2;
        hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        Check(d->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&heap)));
        hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
        Check(d->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&clearCpu)));
        D3D12_DESCRIPTOR_RANGE ranges[2] {};
        ranges[0] = { D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0, 0, 0 };
        ranges[1] = { D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1, 0, 0, 1 };
        D3D12_ROOT_PARAMETER params[2] {};
        params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        params[0].DescriptorTable = { 2, ranges };
        params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        params[1].Constants = { 0, 0, 2 };
        D3D12_ROOT_SIGNATURE_DESC rd {};
        rd.NumParameters = 2;
        rd.pParameters = params;
        ComPtr<ID3DBlob> b, e;
        Check(D3D12SerializeRootSignature(&rd, D3D_ROOT_SIGNATURE_VERSION_1, &b, &e));
        Check(d->CreateRootSignature(0, b->GetBufferPointer(), b->GetBufferSize(), IID_PPV_ARGS(&root)));
        const char* shader = "Texture2D<float4> src:register(t0); RWTexture2D<float4> dst:register(u0); cbuffer "
                             "C:register(b0){uint w,h;} [numthreads(8,8,1)] void main(uint3 "
                             "p:SV_DispatchThreadID){if(p.x<w&&p.y<h)dst[p.xy]=src.Load(int3(p.xy,0));}";
        Check(D3DCompile(shader, strlen(shader), "Final-image compose", nullptr, nullptr, "main", "cs_5_0", 0, 0, &b,
                         &e));
        D3D12_COMPUTE_PIPELINE_STATE_DESC pd {};
        pd.pRootSignature = root.Get();
        pd.CS = { b->GetBufferPointer(), b->GetBufferSize() };
        Check(d->CreateComputePipelineState(&pd, IID_PPV_ARGS(&pipeline)));
    }
    ComPtr<ID3D12Resource> Texture(DXGI_FORMAT f, D3D12_RESOURCE_FLAGS flags, UINT w = 0, UINT h = 0)
    {
        D3D12_HEAP_PROPERTIES hp {};
        hp.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC r {};
        r.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        r.Width = w ? w : width;
        r.Height = h ? h : height;
        r.DepthOrArraySize = 1;
        r.MipLevels = 1;
        r.Format = f;
        r.SampleDesc.Count = 1;
        r.Flags = flags;
        ComPtr<ID3D12Resource> out;
        Check(device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &r, D3D12_RESOURCE_STATE_COMMON, nullptr,
                                              IID_PPV_ARGS(&out)));
        return out;
    }
    bool FlowReady()
    {
        if (flowCreated)
            return true;
        if (flowFailed)
            return false;
        try
        {
            flowScratch.resize(ffxGetScratchMemorySizeDX12(FFX_OPTICALFLOW_CONTEXT_COUNT));
            FfxOpticalflowContextDescription desc {};
            desc.resolution = { width, height };
            if (ffxGetInterfaceDX12(&desc.backendInterface, OptiFfxGetDeviceDX12(device.Get()),
                                    flowScratch.data(), flowScratch.size(), FFX_OPTICALFLOW_CONTEXT_COUNT) != FFX_OK ||
                ffxOpticalflowContextCreate(&flowContext, &desc) != FFX_OK)
                throw std::runtime_error("FidelityFX Optical Flow initialization failed");
            flowCreated = true;
            FfxOpticalflowSharedResourceDescriptions shared {};
            if (ffxOpticalflowGetSharedResourceDescriptions(&flowContext, &shared) != FFX_OK)
                throw std::runtime_error("FidelityFX Optical Flow resources failed");
            auto v = shared.opticalFlowVector.resourceDescription;
            auto s = shared.opticalFlowSCD.resourceDescription;
            flowInput = Texture(DXGI_FORMAT_R8G8B8A8_UNORM, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
            flowSparse = Texture(DXGI_FORMAT_R16G16_SINT, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, v.width, v.height);
            flowScd = Texture(DXGI_FORMAT_R32_UINT, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, s.width, std::max(s.height, 1u));
            D3D12_HEAP_PROPERTIES hp {};
            hp.Type = D3D12_HEAP_TYPE_READBACK;
            D3D12_RESOURCE_DESC readback {};
            readback.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
            readback.Width = 256;
            readback.Height = 1;
            readback.DepthOrArraySize = readback.MipLevels = 1;
            readback.SampleDesc.Count = 1;
            readback.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
            Check(device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &readback,
                D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&flowScdReadback)));
            D3D12_DESCRIPTOR_HEAP_DESC hd {};
            hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
            hd.NumDescriptors = 5;
            hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
            Check(device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&flowHeap)));
            D3D12_DESCRIPTOR_RANGE ranges[2] {};
            ranges[0] = { D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 2, 0, 0, 0 };
            ranges[1] = { D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1, 0, 0, 2 };
            D3D12_ROOT_PARAMETER params[2] {};
            params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
            params[0].DescriptorTable = { 2, ranges };
            params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
            params[1].Constants = { 0, 0, 3 };
            D3D12_ROOT_SIGNATURE_DESC signature {};
            signature.NumParameters = 2;
            signature.pParameters = params;
            ComPtr<ID3DBlob> sig;
            Check(D3D12SerializeRootSignature(&signature, D3D_ROOT_SIGNATURE_VERSION_1, &sig, nullptr));
            Check(device->CreateRootSignature(0, sig->GetBufferPointer(), sig->GetBufferSize(), IID_PPV_ARGS(&flowRoot)));
            const char* shader = R"(Texture2D<int2> src:register(t0); Texture2D<float> depth:register(t1);
                RWTexture2D<float2> dst:register(u0); cbuffer C:register(b0){uint w,h,useDepth;}
                [numthreads(8,8,1)] void main(uint3 p:SV_DispatchThreadID){
                if(p.x>=w||p.y>=h)return; float2 q=(float2(p.xy)+0.5)/8.0-0.5;
                int2 a=int2(floor(q)); float2 t=frac(q); uint sw,sh; src.GetDimensions(sw,sh);
                float zp=useDepth!=0?depth.Load(int3(p.xy,0)):0; float2 v=0; float sum=0;
                [unroll] for(int i=0;i<4;i++){int2 o=int2(i&1,i>>1);
                int2 b=clamp(a+o,int2(0,0),int2(sw-1,sh-1));
                float k=(o.x?t.x:1-t.x)*(o.y?t.y:1-t.y);
                if(useDepth!=0){int2 z=clamp(b*8+4,int2(0,0),int2(w-1,h-1));
                float zb=depth.Load(int3(z,0)); float r=abs(zp-zb)/max(max(zp,zb),1e-6)/0.1;
                k*=1.0/(1.0+r*r)+1e-3;}
                v+=k*float2(src.Load(int3(b,0)));sum+=k;}
                dst[p.xy]=v/max(sum,1e-9);})";
            ComPtr<ID3DBlob> b, e;
            Check(D3DCompile(shader, strlen(shader), "Optical Flow densify", nullptr, nullptr, "main", "cs_5_0", 0, 0, &b, &e));
            D3D12_COMPUTE_PIPELINE_STATE_DESC pd {};
            pd.pRootSignature = flowRoot.Get();
            pd.CS = { b->GetBufferPointer(), b->GetBufferSize() };
            Check(device->CreateComputePipelineState(&pd, IID_PPV_ARGS(&flowDensify)));
            return true;
        }
        catch (...)
        {
            flowFailed = true;
            return false;
        }
    }
    void FlowView(UINT slot, ID3D12Resource* resource, DXGI_FORMAT fmt, bool uav)
    {
        auto handle = flowHeap->GetCPUDescriptorHandleForHeapStart();
        handle.ptr += SIZE_T(slot) * device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        if (uav)
        {
            D3D12_UNORDERED_ACCESS_VIEW_DESC v {};
            v.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
            v.Format = fmt;
            device->CreateUnorderedAccessView(resource, nullptr, &v, handle);
        }
        else
        {
            D3D12_SHADER_RESOURCE_VIEW_DESC v {};
            v.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
            v.Format = fmt;
            v.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            v.Texture2D.MipLevels = 1;
            device->CreateShaderResourceView(resource, &v, handle);
        }
    }
    bool RecordFlow(bool realDepth)
    {
        if (!FlowReady())
            return false;
        auto table = flowHeap->GetGPUDescriptorHandleForHeapStart();
        const auto inc = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        FlowView(0, input.Get(), format, false);
        FlowView(1, flowInput.Get(), DXGI_FORMAT_R8G8B8A8_UNORM, true);
        FlowView(2, flowSparse.Get(), DXGI_FORMAT_R16G16_SINT, false);
        FlowView(3, depth.Get(), DXGI_FORMAT_R32_FLOAT, false);
        FlowView(4, motion.Get(), DXGI_FORMAT_R16G16_FLOAT, true);
        ID3D12DescriptorHeap* heaps[] = { flowHeap.Get() };
        cmd->SetDescriptorHeaps(1, heaps);
        cmd->SetComputeRootSignature(root.Get());
        const UINT size[] = { width, height };
        cmd->SetComputeRoot32BitConstants(1, 2, size, 0);
        cmd->SetComputeRootDescriptorTable(0, table);
        cmd->SetPipelineState(pipeline.Get());
        Transition(cmd.Get(), flowInput.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        cmd->Dispatch((width + 7) / 8, (height + 7) / 8, 1);
        Transition(cmd.Get(), flowInput.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        FfxOpticalflowDispatchDescription dispatch {};
        dispatch.commandList = OptiFfxGetCommandListDX12(cmd.Get());
        dispatch.color = OptiFfxGetResourceDX12(flowInput.Get(), ffxGetResourceDescriptionDX12(flowInput.Get()),
                                                L"OptiScaler flow input", FFX_API_RESOURCE_STATE_COMPUTE_READ);
        // Optical Flow returns its outputs in the state given here, where they rest between frames.
        dispatch.opticalFlowVector = OptiFfxGetResourceDX12(
            flowSparse.Get(), ffxGetResourceDescriptionDX12(flowSparse.Get(), FFX_API_RESOURCE_USAGE_UAV),
            L"OptiScaler flow vectors", FFX_API_RESOURCE_STATE_COMMON);
        dispatch.opticalFlowSCD = OptiFfxGetResourceDX12(
            flowScd.Get(), ffxGetResourceDescriptionDX12(flowScd.Get(), FFX_API_RESOURCE_USAGE_UAV),
            L"OptiScaler scene change", FFX_API_RESOURCE_STATE_COMMON);
        dispatch.reset = flowFrames == 0;
        dispatch.backbufferTransferFunction = FFX_API_BACKBUFFER_TRANSFER_FUNCTION_SRGB;
        dispatch.minMaxLuminance = { 0.f, 1.f };
        if (ffxOpticalflowContextDispatch(&flowContext, &dispatch) != FFX_OK)
        {
            flowFailed = true;
            return false;
        }
        const bool valid = flowFrames++ >= 6;
        Transition(cmd.Get(), flowSparse.Get(), D3D12_RESOURCE_STATE_COMMON,
                   D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        if (valid)
        {
            cmd->SetDescriptorHeaps(1, heaps);
            cmd->SetComputeRootSignature(flowRoot.Get());
            cmd->SetPipelineState(flowDensify.Get());
            const UINT constants[] = { width, height, realDepth ? 1u : 0u };
            cmd->SetComputeRoot32BitConstants(1, 3, constants, 0);
            table.ptr += UINT64(2) * inc;
            cmd->SetComputeRootDescriptorTable(0, table);
            Transition(cmd.Get(), motion.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            cmd->Dispatch((width + 7) / 8, (height + 7) / 8, 1);
            Transition(cmd.Get(), motion.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        }
        Transition(cmd.Get(), flowSparse.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                   D3D12_RESOURCE_STATE_COMMON);
        Transition(cmd.Get(), flowInput.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON);
        return valid;
    }
    bool ReadSceneCut()
    {
        if (!sceneReadbackPending)
            return false;
        sceneReadbackPending = false;
        void* mapped = nullptr;
        const D3D12_RANGE range { 0, sizeof(UINT) };
        Check(flowScdReadback->Map(0, &range, &mapped));
        float score = 0;
        std::memcpy(&score, mapped, sizeof(score));
        flowScdReadback->Unmap(0, nullptr);
        return std::isfinite(score) && score > .45f;
    }
    void QueueSceneCut()
    {
        if (!flowCreated || flowFailed || flowFrames <= 6)
            return;
        Transition(cmd.Get(), flowScd.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_SOURCE);
        D3D12_TEXTURE_COPY_LOCATION src {};
        src.pResource = flowScd.Get();
        src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        D3D12_TEXTURE_COPY_LOCATION dst {};
        dst.pResource = flowScdReadback.Get();
        dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        dst.PlacedFootprint.Footprint.Format = DXGI_FORMAT_R32_UINT;
        dst.PlacedFootprint.Footprint.Width = 3;
        dst.PlacedFootprint.Footprint.Height = dst.PlacedFootprint.Footprint.Depth = 1;
        dst.PlacedFootprint.Footprint.RowPitch = 256;
        cmd->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
        Transition(cmd.Get(), flowScd.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COMMON);
        sceneReadbackPending = true;
    }
    // With neural off only the optical-flow guides are made and the frame is left as it is. Without guides a
    // frame the runtime cannot take ends early.
    bool Frame(ID3D12Resource* back, D3D12_RESOURCE_STATES backState, const std::filesystem::path& directory,
               AmdPreSr::Settings settings, ID3D12Resource* realDepth = nullptr, bool neural = true,
               bool guides = false)
    {
        if (stopped)
            return false;
        lastFlowValid = false;
        // One job the GPU held for seconds stands NR down until the game restarts, as on the upscaler path.
        auto& stall = stallwatch::Watch::Get();
        stall.limitMs.store(
            static_cast<UINT64>(std::max(0, Config::Instance()->AmdStallStandDownMs.value_or_default())));
        if (const UINT64 held = stall.Tripped())
        {
            stopped = true;
            status = "Final-image NR: off until the game restarts. The GPU held one NR job for " +
                     std::to_string(held) + " ms; AmdStallStandDownMs=0 turns this off.";
            LOG_WARN("{}", status);
            return false;
        }
        // The previous frame ran on the GPU while the game prepared this one; its list and descriptors are reused now.
        if (!WaitForFrame())
        {
            stopped = true;
            status = "Final-image NR: GPU timeout; stopped";
            return false;
        }
        heldBack.Reset();
        if (serial >= retiredUntil)
        {
            retired[0].Reset();
            retired[1].Reset();
        }
        // A failed runtime stays failed and a busy one skips this frame: the frame goes without NR and only
        // frame generation's guides are left to make. A pipelined frame leaves busy slots to the runtime, as with
        // an upscaler.
        const bool failed = neural && backend && backend->Failed();
        const bool busy = neural && backend && !failed && !pipelined && !backend->Ready();
        if (failed && !failedReported)
        {
            failedReported = true;
            LOG_WARN("Final-image NR: runtime failed, NR off until the game restarts: {}", backend->Status());
        }
        if (busy && (++busyFrames <= 3 || busyFrames % 300 == 0))
            LOG_INFO("Final-image NR: runtime still busy, frame left without NR ({})", busyFrames);
        if ((failed || busy) && !guides)
        {
            stableLast = false;
            status = backend->Status();
            return false;
        }
        auto desc = back->GetDesc();
        if (!input)
        {
            if (desc.Format != DXGI_FORMAT_R8G8B8A8_UNORM && desc.Format != DXGI_FORMAT_B8G8R8A8_UNORM)
            {
                if (!formatReported)
                    LOG_WARN("Final-image NR: backbuffer format {} is not RGBA8 or BGRA8; no NR and no XeFG guides",
                             (UINT) desc.Format);
                formatReported = true;
                status = "Final-image NR: unsupported backbuffer format";
                return false;
            }
            if (desc.Format == DXGI_FORMAT_B8G8R8A8_UNORM &&
                !(desc.Flags & D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET))
            {
                status = "Final-image NR: BGRA backbuffer cannot be a render target";
                return false;
            }
            width = (UINT) desc.Width;
            height = desc.Height;
            format = desc.Format;
            input = Texture(format, D3D12_RESOURCE_FLAG_NONE);
            if (format == DXGI_FORMAT_R8G8B8A8_UNORM)
                output = Texture(format, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
            motion = Texture(DXGI_FORMAT_R16G16_FLOAT, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
            depth = Texture(DXGI_FORMAT_R32_FLOAT, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
            if (!stabilizer)
                stabilizer = std::make_unique<AmdPreSr::ResidualStabilizer>(device.Get());
        }
        if (desc.Width != width || desc.Height != height || desc.Format != format)
        {
            status = "Final-image NR: output size changed; waiting for swapchain reset";
            return false;
        }
        if (neural && !backend && !runtimeLoaded)
        {
            backend = new AmdPreSr::Backend(device.Get(), queue.Get(), directory);
            runtimeLoaded = true;
        }
        const bool record = neural && backend && !failed && !busy;
        if (!pipelined)
            heldBack = back;
        Check(allocator->Reset());
        Check(cmd->Reset(allocator.Get(), nullptr));
        Transition(cmd.Get(), back, backState, D3D12_RESOURCE_STATE_COPY_SOURCE);
        Transition(cmd.Get(), input.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST);
        cmd->CopyResource(input.Get(), back);
        Transition(cmd.Get(), back, D3D12_RESOURCE_STATE_COPY_SOURCE, backState);
        Transition(cmd.Get(), input.Get(), D3D12_RESOURCE_STATE_COPY_DEST,
                   D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        ID3D12DescriptorHeap* heaps[] = { heap.Get() };
        cmd->SetDescriptorHeaps(1, heaps);
        auto cpu = heap->GetCPUDescriptorHandleForHeapStart();
        auto gpu = heap->GetGPUDescriptorHandleForHeapStart();
        auto clear = clearCpu->GetCPUDescriptorHandleForHeapStart();
        for (auto r : { motion.Get(), depth.Get() })
        {
            if (r == depth.Get() && realDepth)
            {
                Transition(cmd.Get(), depth.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST);
                Transition(cmd.Get(), realDepth, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_SOURCE);
                cmd->CopyResource(depth.Get(), realDepth);
                Transition(cmd.Get(), realDepth, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COMMON);
                Transition(cmd.Get(), depth.Get(), D3D12_RESOURCE_STATE_COPY_DEST,
                           D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
                continue;
            }
            D3D12_UNORDERED_ACCESS_VIEW_DESC u {};
            u.Format = r->GetDesc().Format;
            u.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
            // Distinct descriptors for the two clears, never overwritten while executing.
            device->CreateUnorderedAccessView(r, nullptr, &u, cpu);
            device->CreateUnorderedAccessView(r, nullptr, &u, clear);
            Transition(cmd.Get(), r, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            float values[4] = { r == depth.Get() ? 0.5f : 0.f, 0, 0, 0 };
            cmd->ClearUnorderedAccessViewFloat(gpu, clear, r, values, 0, nullptr);
            Transition(cmd.Get(), r, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                       D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            clear.ptr += device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
            cpu.ptr += device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
            gpu.ptr += device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        }
        const bool flowValid = RecordFlow(realDepth != nullptr);
        const bool sceneCut = ReadSceneCut();
        QueueSceneCut();
        // History that missed frames (scene cut, flow just valid, NR back on) starts over.
        if (backend && (sceneCut || (flowValid && flowFrames == 7) || (neural && !lastNeural)))
        {
            backend->InvalidateHistory();
            stabilizer->Invalidate();
        }
        lastNeural = neural;
        AmdPreSr::Frame f {};
        f.colour = input.Get();
        f.motion = motion.Get();
        f.depth = depth.Get();
        f.width = width;
        f.height = height;
        f.reset = sceneCut || (backend && backend->RecordedFrames() == 0);
        lastFlowValid = flowValid;
        // Frame generation resets on scene cuts only: a runtime that never records a frame must not reset it.
        lastReset = sceneCut;
        settings.passes = std::clamp(settings.passes, 1u, 3u);
        settings.modelScale = std::clamp(settings.modelScale, 0.5f, 1.f);
        settings.fixedSeed = true;
        // No RenoDX composition here, so Effect strength stays the runtime's own strength.
        settings.strength = std::clamp(Config::Instance()->AmdEffectStrength.value_or_default(), 0.f, 2.f);
        const auto before = record ? backend->RecordedFrames() : 0;
        auto result = record ? backend->Record(cmd.Get(), f, settings) : nullptr;
        const bool modified = result && backend->RecordedFrames() != before;
        if (modified)
        {
            const auto now = GetTickCount64();
            const auto& cfg = *Config::Instance();
            const float strength = cfg.AmdStabilizerStrength.has_value()
                                       ? cfg.AmdStabilizerStrength.value_or_default() : .8f;
            const float threshold = cfg.AmdStabilizerThreshold.has_value()
                                        ? cfg.AmdStabilizerThreshold.value_or_default() : 4.f;
            if (strength > 0)
                result = stabilizer->Record(cmd.Get(), f, result,
                                            stableLast && now - lastModifiedTick < 250 && !f.reset,
                                            std::clamp(strength, 0.f, 1.f), std::clamp(threshold, .5f, 8.f));
            else
            {
                stabilizer->Invalidate();
                stabilizer->Idle();
            }
            lastModifiedTick = now;
        }
        else
            stabilizer->Invalidate();
        stableLast = modified;
        // Composition uses a separate heap: guide descriptors must remain unchanged.
        // A skipped record is allowed; never reuse an old neural frame.
        Transition(cmd.Get(), input.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON);
        Transition(cmd.Get(), motion.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                   D3D12_RESOURCE_STATE_COMMON);
        Transition(cmd.Get(), depth.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON);
        if (modified)
            Compose(back, backState, result);
        Check(cmd->Close());
        ID3D12CommandList* lists[] = { cmd.Get() };
        if (backend)
            backend->Submitting(queue.Get(), 1, lists);
        queue->ExecuteCommandLists(1, lists);
        if (backend)
            backend->Submitted(queue.Get(), 1, lists);
        Check(queue->Signal(fence.Get(), ++serial));
        if (!pipelined)
            ReleaseBackBuffer();
        if (!stopped)
            status = std::string("Final-image NR (") +
                     (flowValid    ? "FidelityFX Optical Flow"
                      : flowFailed ? "synthetic guides; Optical Flow unavailable"
                                   : "Optical Flow warming up") +
                     (realDepth ? ", real depth" : ", synthetic depth") + (sceneCut ? ", scene reset" : "") +
                     ", stabilized): " +
                     (!neural   ? std::string("NR off, guides only")
                      : backend ? backend->Status()
                                : std::string("NR runs on the game's other D3D12 device; restart the game for NR"));
        return modified && !stopped;
    }
    ComPtr<ID3D12DescriptorHeap> composeHeap;
    void ComposeBgra(ID3D12Resource* back, D3D12_RESOURCE_STATES backState)
    {
        if (!graphicsPipeline)
        {
            D3D12_DESCRIPTOR_RANGE range { D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0, 0, 0 };
            D3D12_ROOT_PARAMETER param {};
            param.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
            param.DescriptorTable = { 1, &range };
            param.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
            D3D12_ROOT_SIGNATURE_DESC desc {};
            desc.NumParameters = 1;
            desc.pParameters = &param;
            ComPtr<ID3DBlob> blob, errors;
            Check(D3D12SerializeRootSignature(&desc, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &errors));
            Check(device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(),
                                              IID_PPV_ARGS(&graphicsRoot)));
            const char* vs = "float4 main(uint id:SV_VertexID):SV_Position {"
                             "float2 p=float2((id<<1)&2,id&2);return float4(p*float2(2,-2)+float2(-1,1),0,1);}";
            const char* ps = "Texture2D<float4> src:register(t0);"
                             "float4 main(float4 p:SV_Position):SV_Target {return src.Load(int3(p.xy,0));}";
            ComPtr<ID3DBlob> vertex, pixel;
            Check(D3DCompile(vs, strlen(vs), "Final-image VS", nullptr, nullptr, "main", "vs_5_0", 0, 0,
                             &vertex, &errors));
            Check(D3DCompile(ps, strlen(ps), "Final-image PS", nullptr, nullptr, "main", "ps_5_0", 0, 0,
                             &pixel, &errors));
            D3D12_GRAPHICS_PIPELINE_STATE_DESC pso {};
            pso.pRootSignature = graphicsRoot.Get();
            pso.VS = { vertex->GetBufferPointer(), vertex->GetBufferSize() };
            pso.PS = { pixel->GetBufferPointer(), pixel->GetBufferSize() };
            pso.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
            pso.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
            pso.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
            pso.RasterizerState.DepthClipEnable = TRUE;
            pso.SampleMask = UINT_MAX;
            pso.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
            pso.NumRenderTargets = 1;
            pso.RTVFormats[0] = DXGI_FORMAT_B8G8R8A8_UNORM;
            pso.SampleDesc.Count = 1;
            Check(device->CreateGraphicsPipelineState(&pso, IID_PPV_ARGS(&graphicsPipeline)));
            D3D12_DESCRIPTOR_HEAP_DESC heapDesc {};
            heapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
            heapDesc.NumDescriptors = 1;
            Check(device->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(&rtvHeap)));
        }
        const auto rtv = rtvHeap->GetCPUDescriptorHandleForHeapStart();
        device->CreateRenderTargetView(back, nullptr, rtv);
        Transition(cmd.Get(), back, backState, D3D12_RESOURCE_STATE_RENDER_TARGET);
        auto srvHeap = composeHeap.Get();
        cmd->SetDescriptorHeaps(1, &srvHeap);
        cmd->SetGraphicsRootSignature(graphicsRoot.Get());
        cmd->SetPipelineState(graphicsPipeline.Get());
        cmd->SetGraphicsRootDescriptorTable(0, composeHeap->GetGPUDescriptorHandleForHeapStart());
        D3D12_VIEWPORT vp { 0.f, 0.f, static_cast<float>(width), static_cast<float>(height), 0.f, 1.f };
        D3D12_RECT rect { 0, 0, static_cast<LONG>(width), static_cast<LONG>(height) };
        cmd->RSSetViewports(1, &vp);
        cmd->RSSetScissorRects(1, &rect);
        cmd->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
        cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        cmd->DrawInstanced(3, 1, 0, 0);
        Transition(cmd.Get(), back, D3D12_RESOURCE_STATE_RENDER_TARGET, backState);
    }
    void Compose(ID3D12Resource* back, D3D12_RESOURCE_STATES backState, ID3D12Resource* result)
    {
        if (!composeHeap)
        {
            D3D12_DESCRIPTOR_HEAP_DESC d {};
            d.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
            d.NumDescriptors = 2;
            d.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
            Check(device->CreateDescriptorHeap(&d, IID_PPV_ARGS(&composeHeap)));
        }
        auto cpu = composeHeap->GetCPUDescriptorHandleForHeapStart();
        D3D12_SHADER_RESOURCE_VIEW_DESC srv {};
        srv.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srv.Texture2D.MipLevels = 1;
        device->CreateShaderResourceView(result, &srv, cpu);
        if (format == DXGI_FORMAT_B8G8R8A8_UNORM)
        {
            ComposeBgra(back, backState);
            return;
        }
        cpu.ptr += device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        D3D12_UNORDERED_ACCESS_VIEW_DESC u {};
        u.Format = format;
        u.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        device->CreateUnorderedAccessView(output.Get(), nullptr, &u, cpu);
        Transition(cmd.Get(), output.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        auto h = composeHeap.Get();
        cmd->SetDescriptorHeaps(1, &h);
        cmd->SetComputeRootSignature(root.Get());
        cmd->SetPipelineState(pipeline.Get());
        cmd->SetComputeRootDescriptorTable(0, composeHeap->GetGPUDescriptorHandleForHeapStart());
        UINT dims[] = { width, height };
        cmd->SetComputeRoot32BitConstants(1, 2, dims, 0);
        cmd->Dispatch((width + 7) / 8, (height + 7) / 8, 1);
        Transition(cmd.Get(), output.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
        Transition(cmd.Get(), back, backState, D3D12_RESOURCE_STATE_COPY_DEST);
        cmd->CopyResource(back, output.Get());
        Transition(cmd.Get(), back, D3D12_RESOURCE_STATE_COPY_DEST, backState);
        Transition(cmd.Get(), output.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COMMON);
    }
};
inline Context* context = nullptr;
// One per D3D12 device the game presented from, kept for the process like the runtime.
inline std::vector<Context*> contexts;
// A game can present from another queue or device than before, as Red Dead Redemption does when its frame
// generation swapchain replaces the first one. Every frame waits for its own work, so after that wait the
// context moves to the new queue; the runtime orders its own work across the move.
inline bool Follow(ID3D12Device* device, ID3D12CommandQueue* queue)
{
    static UINT moves = 0;
    if (context && context->device.Get() != device)
    {
        context->BeforeResize();
        context = nullptr;
        for (auto c : contexts)
            if (c->device.Get() == device)
                context = c;
        if (++moves <= 3 || moves % 300 == 0)
            LOG_INFO("Final-image NR: the game presents from another D3D12 device ({})", moves);
    }
    if (!context)
        context = contexts.emplace_back(new Context(device, queue));
    if (context->queue.Get() == queue)
        return true;
    if (!context->WaitForFrame())
    {
        context->stopped = true;
        status = "Final-image NR: GPU timeout; stopped";
        return false;
    }
    context->queue = queue;
    if (++moves <= 3 || moves % 300 == 0)
        LOG_INFO("Final-image NR: the game presents from another D3D12 queue ({})", moves);
    return true;
}
struct Guides
{
    ComPtr<ID3D12Resource> motion, depth;
    UINT width = 0, height = 0;
    bool reset = false;
};
inline std::mutex shownMutex;
inline std::string shown = status;
// Holds the NR work and, on release, shows the status it left. The menu and the FPS overlay read it from the present
// thread, so they never wait for a frame's NR work.
struct Lock
{
    std::lock_guard<std::mutex> work { mutex };
    ~Lock()
    {
        std::lock_guard g(shownMutex);
        shown = status;
    }
};
inline std::string Status()
{
    std::lock_guard g(shownMutex);
    return shown;
}
inline void Report(std::string message)
{
    Lock g;
    status = std::move(message);
}
inline bool Render(IDXGISwapChain* sc, ID3D12CommandQueue* queue, const std::filesystem::path& directory,
                   AmdPreSr::Settings s, Guides* guides = nullptr, bool neural = true, bool pipelined = false)
{
    Lock g;
    if (!queue)
        return false;
    try
    {
        ComPtr<IDXGISwapChain3> swap;
        Check(sc->QueryInterface(IID_PPV_ARGS(&swap)));
        ComPtr<ID3D12Device> d;
        Check(queue->GetDevice(IID_PPV_ARGS(&d)));
        if (!Follow(d.Get(), queue))
            return false;
        // Without the runtime on this device only frame generation's guides are left to make.
        if (!guides && !context->backend && runtimeLoaded)
        {
            status = "Final-image NR: NR runs on the game's other D3D12 device; restart the game for NR";
            return false;
        }
        ComPtr<ID3D12Resource> b;
        Check(swap->GetBuffer(swap->GetCurrentBackBufferIndex(), IID_PPV_ARGS(&b)));
        context->pipelined = pipelined;
        context->Frame(b.Get(), D3D12_RESOURCE_STATE_PRESENT, directory, s, nullptr, neural, guides != nullptr);
        if (guides && !context->stopped && context->lastFlowValid)
        {
            guides->motion = context->motion;
            guides->depth = context->depth;
            guides->width = context->width;
            guides->height = context->height;
            guides->reset = context->lastReset;
            return true;
        }
    }
    catch (const std::exception& e)
    {
        status = e.what();
        if (context)
            context->stopped = true;
    }
    return false;
}
inline bool RenderResource(ID3D12Resource* resource, ID3D12Device* device, ID3D12CommandQueue* queue,
                           const std::filesystem::path& directory, AmdPreSr::Settings settings,
                           Guides* guides = nullptr)
{
    Lock g;
    if (guides)
        *guides = {};
    try
    {
        if (!context)
            context = new Context(device, queue);
        if (context->device.Get() != device || context->queue.Get() != queue)
        {
            status = "Final-image NR: Vulkan D3D12 device changed; restart required";
            return false;
        }
        const bool rendered = context->Frame(resource, D3D12_RESOURCE_STATE_COMMON, directory, settings);
        // Vulkan copies the shared image back as soon as this returns.
        if (!context->WaitForFrame())
        {
            context->stopped = true;
            status = "Final-image NR: GPU timeout; stopped";
        }
        if (guides && rendered && !context->stopped && context->lastFlowValid)
        {
            guides->motion = context->motion;
            guides->depth = context->depth;
            guides->width = context->width;
            guides->height = context->height;
            guides->reset = context->lastReset;
        }
        return !context->stopped;
    }
    catch (const std::exception& e)
    {
        status = e.what();
        if (context)
            context->stopped = true;
        return false;
    }
}

// The D3D11 colour crossing follows the ReShade add-on's shared-texture route. A shared fence
// orders it on the GPU: D3D12 starts after D3D11's copy and D3D11 goes on after NR, so neither
// API waits on the CPU and only D3D11 touches the backbuffer.
struct Bridge11
{
    ComPtr<ID3D11Device> game;
    ComPtr<ID3D11DeviceContext> immediate;
    ComPtr<ID3D11DeviceContext4> immediate4;
    ComPtr<ID3D11Fence> fence11;
    ComPtr<ID3D12Fence> fence12;
    UINT64 fenceValue = 0;
    ComPtr<ID3D12Device> work;
    ComPtr<ID3D12CommandQueue> queue;
    ComPtr<ID3D11Texture2D> shared11;
    ComPtr<ID3D12Resource> shared12;
    ComPtr<ID3D11Texture2D> sharedDepth11;
    ComPtr<ID3D12Resource> sharedDepth12;
    ComPtr<ID3D11RenderTargetView> depthTarget;
    ComPtr<ID3D11DeviceContext> depthDeferred;
    ComPtr<ID3D11VertexShader> depthVs;
    ComPtr<ID3D11PixelShader> depthPs;
    HANDLE handle = nullptr;
    HANDLE depthHandle = nullptr;
    bool depthDisabled = false;
    Context* engine = nullptr;
    UINT width = 0, height = 0;
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
    bool stopped = false;

    bool Wait11()
    {
        D3D11_QUERY_DESC desc {};
        desc.Query = D3D11_QUERY_EVENT;
        ComPtr<ID3D11Query> done;
        if (FAILED(game->CreateQuery(&desc, &done)))
            return false;
        immediate->End(done.Get());
        immediate->Flush();
        const auto start = GetTickCount64();
        for (;;)
        {
            const HRESULT hr = immediate->GetData(done.Get(), nullptr, 0, 0);
            if (hr == S_OK)
                return SUCCEEDED(game->GetDeviceRemovedReason());
            if (hr != S_FALSE || GetTickCount64() - start > 2000 || FAILED(game->GetDeviceRemovedReason()))
                return false;
            Sleep(1);
        }
    }
    void ReleaseShared()
    {
        depthTarget.Reset();
        sharedDepth12.Reset();
        sharedDepth11.Reset();
        if (depthHandle)
            CloseHandle(depthHandle);
        depthHandle = nullptr;
        shared12.Reset();
        shared11.Reset();
        if (handle)
            CloseHandle(handle);
        handle = nullptr;
        width = height = 0;
        format = DXGI_FORMAT_UNKNOWN;
    }
    void BeforeResize()
    {
        if (engine)
            engine->BeforeResize();
        if (!stopped && (!engine || !engine->stopped))
            ReleaseShared();
        else
            stopped = true;
    }
    void Start(ID3D11Device* device)
    {
        game = device;
        game->GetImmediateContext(&immediate);
        ComPtr<IDXGIDevice> dxgi;
        ComPtr<IDXGIAdapter> adapter;
        Check(game.As(&dxgi));
        Check(dxgi->GetAdapter(&adapter));
        Check(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&work)));
        D3D12_COMMAND_QUEUE_DESC queueDesc {};
        queueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        Check(work->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&queue)));
        // Without D3D11.4 fences (an overlay's device wrapper, an old runtime) the CPU waits for each side instead.
        ComPtr<ID3D11Device5> device5;
        HANDLE shared = nullptr;
        if (FAILED(game.As(&device5)) || FAILED(immediate.As(&immediate4)) ||
            FAILED(device5->CreateFence(0, D3D11_FENCE_FLAG_SHARED, IID_PPV_ARGS(&fence11))) ||
            FAILED(fence11->CreateSharedHandle(nullptr, GENERIC_ALL, nullptr, &shared)) ||
            FAILED(work->OpenSharedHandle(shared, IID_PPV_ARGS(&fence12))))
        {
            LOG_WARN("Final-image NR: no shared D3D11 fence, waiting on the CPU");
            fence11.Reset();
            fence12.Reset();
        }
        if (shared)
            CloseHandle(shared);
        engine = new Context(work.Get(), queue.Get());
        engine->pipelined = true;
    }
    void EnsureShared(UINT w, UINT h, DXGI_FORMAT f)
    {
        if (shared12 && width == w && height == h && format == f)
            return;
        BeforeResize();
        if (stopped)
            return;
        D3D11_TEXTURE2D_DESC desc {};
        desc.Width = w;
        desc.Height = h;
        desc.MipLevels = desc.ArraySize = desc.SampleDesc.Count = 1;
        desc.Format = f;
        desc.Usage = D3D11_USAGE_DEFAULT;
        desc.BindFlags = D3D11_BIND_SHADER_RESOURCE |
                         (f == DXGI_FORMAT_B8G8R8A8_UNORM ? D3D11_BIND_RENDER_TARGET : 0u);
        desc.MiscFlags = D3D11_RESOURCE_MISC_SHARED_NTHANDLE | D3D11_RESOURCE_MISC_SHARED;
        Check(game->CreateTexture2D(&desc, nullptr, &shared11));
        ComPtr<IDXGIResource1> resource;
        Check(shared11.As(&resource));
        Check(resource->CreateSharedHandle(nullptr, DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE,
                                           nullptr, &handle));
        Check(work->OpenSharedHandle(handle, IID_PPV_ARGS(&shared12)));
        width = w;
        height = h;
        format = f;
    }
    bool CaptureDepth(UINT w, UINT h)
    {
        if (depthDisabled)
            return false;
        ComPtr<ID3D11DepthStencilView> dsv;
        immediate->OMGetRenderTargets(0, nullptr, &dsv);
        if (!dsv)
            return false;
        ComPtr<ID3D11Resource> resource;
        dsv->GetResource(&resource);
        ComPtr<ID3D11Texture2D> source;
        if (FAILED(resource.As(&source)))
            return false;
        D3D11_TEXTURE2D_DESC desc {};
        source->GetDesc(&desc);
        if (desc.Width != w || desc.Height != h || desc.SampleDesc.Count != 1 ||
            !(desc.BindFlags & D3D11_BIND_SHADER_RESOURCE) ||
            AmdPreSr::DepthReadFormat(desc.Format) == DXGI_FORMAT_UNKNOWN)
            return false;
        D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc {};
        srvDesc.Format = AmdPreSr::DepthReadFormat(desc.Format);
        srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
        srvDesc.Texture2D.MipLevels = 1;
        ComPtr<ID3D11ShaderResourceView> srv;
        if (FAILED(game->CreateShaderResourceView(source.Get(), &srvDesc, &srv)))
            return false;
        try
        {
            if (!sharedDepth11)
            {
                D3D11_TEXTURE2D_DESC out {};
                out.Width = w;
                out.Height = h;
                out.MipLevels = out.ArraySize = out.SampleDesc.Count = 1;
                out.Format = DXGI_FORMAT_R32_FLOAT;
                out.Usage = D3D11_USAGE_DEFAULT;
                out.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
                out.MiscFlags = D3D11_RESOURCE_MISC_SHARED_NTHANDLE | D3D11_RESOURCE_MISC_SHARED;
                Check(game->CreateTexture2D(&out, nullptr, &sharedDepth11));
                Check(game->CreateRenderTargetView(sharedDepth11.Get(), nullptr, &depthTarget));
                ComPtr<IDXGIResource1> share;
                Check(sharedDepth11.As(&share));
                Check(share->CreateSharedHandle(nullptr, DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE,
                                                nullptr, &depthHandle));
                Check(work->OpenSharedHandle(depthHandle, IID_PPV_ARGS(&sharedDepth12)));
            }
            if (!depthDeferred)
            {
                Check(game->CreateDeferredContext(0, &depthDeferred));
                static constexpr char vs[] = "float4 main(uint id:SV_VertexID):SV_Position {"
                    "float2 p=float2((id<<1)&2,id&2);return float4(p*float2(2,-2)+float2(-1,1),0,1);}";
                static constexpr char ps[] = "Texture2D<float> z:register(t0);"
                    "float main(float4 p:SV_Position):SV_Target{return z.Load(int3(p.xy,0));}";
                ComPtr<ID3DBlob> blob, errors;
                Check(D3DCompile(vs, sizeof(vs), "Depth capture VS", nullptr, nullptr, "main", "vs_5_0", 0, 0,
                                 &blob, &errors));
                Check(game->CreateVertexShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr, &depthVs));
                Check(D3DCompile(ps, sizeof(ps), "Depth capture PS", nullptr, nullptr, "main", "ps_5_0", 0, 0,
                                 &blob, &errors));
                Check(game->CreatePixelShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr, &depthPs));
            }
            D3D11_VIEWPORT viewport {};
            viewport.Width = float(w);
            viewport.Height = float(h);
            viewport.MaxDepth = 1;
            ID3D11RenderTargetView* target = depthTarget.Get();
            ID3D11ShaderResourceView* view = srv.Get();
            depthDeferred->RSSetViewports(1, &viewport);
            depthDeferred->OMSetRenderTargets(1, &target, nullptr);
            depthDeferred->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            depthDeferred->VSSetShader(depthVs.Get(), nullptr, 0);
            depthDeferred->PSSetShader(depthPs.Get(), nullptr, 0);
            depthDeferred->PSSetShaderResources(0, 1, &view);
            depthDeferred->Draw(3, 0);
            ComPtr<ID3D11CommandList> commands;
            Check(depthDeferred->FinishCommandList(FALSE, &commands));
            immediate->ExecuteCommandList(commands.Get(), TRUE);
            return true;
        }
        catch (...)
        {
            depthDisabled = true;
            return false;
        }
    }
    void FrameTexture(ID3D11Texture2D* back, ID3D11Device* device, const std::filesystem::path& directory,
                      AmdPreSr::Settings settings, bool captureDepth)
    {
        if (stopped || (engine && engine->stopped))
            return;
        if (!game)
            Start(device);
        if (game.Get() != device)
        {
            status = "Final-image NR: D3D11 device changed; restart required";
            stopped = true;
            return;
        }
        D3D11_TEXTURE2D_DESC desc {};
        back->GetDesc(&desc);
        if (desc.SampleDesc.Count != 1 ||
            (desc.Format != DXGI_FORMAT_R8G8B8A8_UNORM && desc.Format != DXGI_FORMAT_B8G8R8A8_UNORM))
        {
            status = "Final-image NR: D3D11 backbuffer format or MSAA unsupported";
            return;
        }
        EnsureShared(desc.Width, desc.Height, desc.Format);
        if (stopped || engine->stopped)
            return;
        const bool realDepth = captureDepth && CaptureDepth(desc.Width, desc.Height);
        immediate->CopyResource(shared11.Get(), back);
        if (!fence11)
        {
            if (!Wait11())
                throw std::runtime_error("Final-image NR: D3D11 input copy timed out");
            const bool modified = engine->Frame(shared12.Get(), D3D12_RESOURCE_STATE_COMMON, directory, settings,
                                                realDepth ? sharedDepth12.Get() : nullptr);
            if (!engine->WaitForFrame())
                throw std::runtime_error("Final-image NR: GPU timeout; stopped");
            if (modified)
                immediate->CopyResource(back, shared11.Get());
            immediate->Flush();
            return;
        }
        Check(immediate4->Signal(fence11.Get(), ++fenceValue));
        immediate->Flush();
        Check(queue->Wait(fence12.Get(), fenceValue));
        const bool modified = engine->Frame(shared12.Get(), D3D12_RESOURCE_STATE_COMMON, directory, settings,
                                            realDepth ? sharedDepth12.Get() : nullptr);
        Check(queue->Signal(fence12.Get(), ++fenceValue));
        Check(immediate4->Wait(fence11.Get(), fenceValue));
        if (modified)
            immediate->CopyResource(back, shared11.Get());
    }
    void Frame(IDXGISwapChain* sc, ID3D11Device* device, const std::filesystem::path& directory,
               AmdPreSr::Settings settings)
    {
        ComPtr<IDXGISwapChain3> swap3;
        const UINT index = SUCCEEDED(sc->QueryInterface(IID_PPV_ARGS(&swap3)))
                               ? swap3->GetCurrentBackBufferIndex() : 0;
        ComPtr<ID3D11Texture2D> back;
        Check(sc->GetBuffer(index, IID_PPV_ARGS(&back)));
        FrameTexture(back.Get(), device, directory, settings, true);
    }
};
inline Bridge11* bridge11 = nullptr;
inline void Render11(IDXGISwapChain* sc, ID3D11Device* device, const std::filesystem::path& directory,
                     AmdPreSr::Settings settings)
{
    Lock g;
    try
    {
        if (!bridge11)
            bridge11 = new Bridge11;
        bridge11->Frame(sc, device, directory, settings);
    }
    catch (const std::exception& e)
    {
        status = e.what();
        if (bridge11)
            bridge11->stopped = true;
    }
}
inline bool RenderTexture11(ID3D11Texture2D* texture, ID3D11Device* device,
                            const std::filesystem::path& directory, AmdPreSr::Settings settings)
{
    Lock g;
    try
    {
        if (!bridge11)
            bridge11 = new Bridge11;
        bridge11->FrameTexture(texture, device, directory, settings, false);
        return !bridge11->stopped && !(bridge11->engine && bridge11->engine->stopped);
    }
    catch (const std::exception& e)
    {
        status = e.what();
        if (bridge11)
            bridge11->stopped = true;
        return false;
    }
}
inline void BeforeResize()
{
    Lock g;
    if (context)
        context->BeforeResize();
    if (bridge11)
        bridge11->BeforeResize();
}
// The motion and depth last handed to XeFG, which keeps plain pointers to them and may still read them on its own
// queue: a resize of XeFG's swapchain holds them until it returns.
inline Guides LastGuides()
{
    Lock g;
    Guides guides;
    if (context)
    {
        guides.motion = context->motion;
        guides.depth = context->depth;
    }
    return guides;
}
} // namespace AmdPresentExperimental
