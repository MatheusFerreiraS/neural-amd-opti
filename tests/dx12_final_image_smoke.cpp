// D3D12 final-image smoke: a real D3D12 app with OptiScaler.dll loaded as dxgi.dll beside it.
// tools\test-dx12-final-image.cmd stages the folder and runs every scenario.
//
//   dx12_final_image_smoke.exe <nr|fg> [--bgra] [--debug-layer] [--lock <dir>] [--timeout <seconds>]
//                              [--log-level <OptiScaler LogLevel, 2 by default>] [--cpu-ms <game CPU ms per frame>]
//                              [--dump]
//
// The parent writes OptiScaler.ini from OptiScaler.template.ini, takes the GPU lock, runs itself as a
// child with a timeout and asserts on the child's results and on OptiScaler.log. The child renders a
// moving scene, posts the menu (Insert), frame generation (End) and Neural Rendering (Numpad 5) keys,
// resizes the swapchain and reads back the frame it presented to see whether NR changed it.
// "nr" runs NR alone; "fg" runs NR with XeFG multi frame generation (OptiFG input).
#define NOMINMAX
#define _CRT_SECURE_NO_WARNINGS
#include <windows.h>
#include <d3d12.h>
#include <d3dcompiler.h>
#include <dxgi1_6.h>
#include <dbghelp.h>
#include <wrl/client.h>
#include <algorithm>
#include <chrono>
#include <cstring>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

using Microsoft::WRL::ComPtr;
namespace fs = std::filesystem;

static fs::path Here()
{
    wchar_t path[MAX_PATH] {};
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    return fs::path(path).parent_path();
}

static std::FILE* results = nullptr;
static void Out(const char* format, auto... args)
{
    std::printf(format, args...);
    std::printf("\n");
    if (results)
    {
        std::fprintf(results, format, args...);
        std::fprintf(results, "\n");
        std::fflush(results);
    }
    std::fflush(stdout);
}

[[noreturn]] static void Fail(const char* what, HRESULT hr = S_OK)
{
    Out("child-fail %s hr=0x%08X", what, (unsigned) hr);
    ExitProcess(3);
}
static void Check(HRESULT hr, const char* what)
{
    if (FAILED(hr))
        Fail(what, hr);
}

static std::string ReadAll(const fs::path& path)
{
    std::ifstream file(path, std::ios::binary);
    std::stringstream text;
    text << file.rdbuf();
    return text.str();
}

static int Count(const std::string& text, const std::string& needle)
{
    int n = 0;
    for (size_t at = text.find(needle); at != std::string::npos; at = text.find(needle, at + 1))
        ++n;
    return n;
}

// Runtime history resets for a settings change after startup, as a Model scale change makes.
static int SettingsResets(const std::string& runtimeLog)
{
    int resets = 0;
    for (size_t at = runtimeLog.find("AMD history reset: frame="); at != std::string::npos;
         at = runtimeLog.find("AMD history reset: frame=", at + 1))
    {
        const auto line = runtimeLog.substr(at, runtimeLog.find("\n", at) - at);
        resets += std::atoi(line.c_str() + 25) > 30 && line.find("settings=1") != std::string::npos;
    }
    return resets;
}

// ---------------------------------------------------------------- child

// Any crash, on any thread: the faulting stack, symbolized when a PDB sits beside the module.
static LONG WINAPI Crash(EXCEPTION_POINTERS* pointers)
{
    Out("child-crash code=0x%08X address=%p", (unsigned) pointers->ExceptionRecord->ExceptionCode,
        pointers->ExceptionRecord->ExceptionAddress);
    const HANDLE process = GetCurrentProcess();
    SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS | SYMOPT_LOAD_LINES);
    SymInitializeW(process, Here().c_str(), TRUE);
    CONTEXT context = *pointers->ContextRecord;
    STACKFRAME64 frame {};
    frame.AddrPC = { context.Rip, 0, AddrModeFlat };
    frame.AddrFrame = { context.Rbp, 0, AddrModeFlat };
    frame.AddrStack = { context.Rsp, 0, AddrModeFlat };
    for (int i = 0; i < 48 && StackWalk64(IMAGE_FILE_MACHINE_AMD64, process, GetCurrentThread(), &frame, &context,
                                          nullptr, SymFunctionTableAccess64, SymGetModuleBase64, nullptr);
         ++i)
    {
        const DWORD64 pc = frame.AddrPC.Offset;
        HMODULE module = nullptr;
        wchar_t name[MAX_PATH] = L"?";
        if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                               (LPCWSTR) pc, &module))
            GetModuleFileNameW(module, name, MAX_PATH);
        alignas(SYMBOL_INFO) char storage[sizeof(SYMBOL_INFO) + 256] {};
        auto symbol = (SYMBOL_INFO*) storage;
        symbol->SizeOfStruct = sizeof(SYMBOL_INFO);
        symbol->MaxNameLen = 255;
        DWORD64 displacement = 0;
        const bool named = SymFromAddr(process, pc, &displacement, symbol);
        IMAGEHLP_LINE64 line { sizeof(line) };
        DWORD column = 0;
        const bool lined = SymGetLineFromAddr64(process, pc, &column, &line);
        Out("child-crash frame %d %ls+0x%llx %s+0x%llx %s:%lu", i, fs::path(name).filename().c_str(),
            pc - (DWORD64) module, named ? symbol->Name : "?", displacement, lined ? line.FileName : "?",
            lined ? line.LineNumber : 0);
    }
    return EXCEPTION_EXECUTE_HANDLER;
}

struct Options
{
    std::string scenario;
    std::wstring logLevel = L"2";
    std::wstring modelScale = L"auto"; // AmdModelScale at start
    double cpuMs = 0;                  // simulated game CPU time per frame, before recording
    bool dump = false;                 // writes each phase's first probe frame as <phase>.bmp
    bool bgra = false;
    bool debugLayer = false;
};

struct App
{
    Options options;
    HWND window = nullptr;
    PFN_D3D12_CREATE_DEVICE createDevice = nullptr;
    PFN_D3D12_SERIALIZE_ROOT_SIGNATURE serializeRoot = nullptr;
    ComPtr<ID3D12Device> device;
    ComPtr<ID3D12CommandQueue> queue;
    ComPtr<IDXGISwapChain3> swapchain;
    ComPtr<ID3D12InfoQueue> info;
    ComPtr<ID3D12RootSignature> root;
    ComPtr<ID3D12PipelineState> pipeline;
    ComPtr<ID3D12DescriptorHeap> rtvHeap;
    ComPtr<ID3D12Fence> fence;
    HANDLE fenceEvent = nullptr;
    UINT64 fenceValue = 0;
    static constexpr UINT bufferCount = 3;
    ComPtr<ID3D12CommandAllocator> allocators[bufferCount];
    UINT64 allocatorFence[bufferCount] {};
    ComPtr<ID3D12GraphicsCommandList> list;
    ComPtr<ID3D12Resource> reference, readBack, readReference;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint {};
    UINT width = 1280, height = 720;
    DXGI_FORMAT format = DXGI_FORMAT_R8G8B8A8_UNORM;
    UINT swapFlags = 0;
    UINT64 frame = 0;

    void Wait(UINT64 value)
    {
        if (fence->GetCompletedValue() >= value)
            return;
        Check(fence->SetEventOnCompletion(value, fenceEvent), "SetEventOnCompletion");
        if (WaitForSingleObject(fenceEvent, 10000) != WAIT_OBJECT_0)
            Fail("GPU fence wait timed out", device->GetDeviceRemovedReason());
    }
    void Idle()
    {
        Check(queue->Signal(fence.Get(), ++fenceValue), "Signal");
        Wait(fenceValue);
    }

    void CreateDevice()
    {
        // Through the proxy: the module named dxgi.dll in this folder is OptiScaler.
        HMODULE dxgi = LoadLibraryW(L"dxgi.dll");
        if (!dxgi)
            Fail("dxgi.dll did not load");
        wchar_t loaded[MAX_PATH] {};
        GetModuleFileNameW(dxgi, loaded, MAX_PATH);
        if (fs::path(loaded).parent_path() != Here())
            Fail("dxgi.dll is not the OptiScaler proxy beside the test");
        HMODULE d3d12 = LoadLibraryW(L"d3d12.dll");
        if (!d3d12)
            Fail("d3d12.dll did not load");
        // Resolved at run time: a static import would pull the proxy into the parent process too.
        createDevice = (PFN_D3D12_CREATE_DEVICE) GetProcAddress(d3d12, "D3D12CreateDevice");
        serializeRoot = (PFN_D3D12_SERIALIZE_ROOT_SIGNATURE) GetProcAddress(d3d12, "D3D12SerializeRootSignature");
        auto getDebug = (PFN_D3D12_GET_DEBUG_INTERFACE) GetProcAddress(d3d12, "D3D12GetDebugInterface");
        auto createFactory = (HRESULT(WINAPI*)(UINT, REFIID, void**)) GetProcAddress(dxgi, "CreateDXGIFactory2");
        if (!createDevice || !serializeRoot || !getDebug || !createFactory)
            Fail("missing D3D12/DXGI entry points");
        if (options.debugLayer)
        {
            ComPtr<ID3D12Debug> debug;
            if (FAILED(getDebug(IID_PPV_ARGS(&debug))))
                Fail("D3D12 debug layer unavailable");
            debug->EnableDebugLayer();
            Out("debug-layer on");
        }
        ComPtr<IDXGIFactory6> factory;
        Check(createFactory(0, IID_PPV_ARGS(&factory)), "CreateDXGIFactory2");
        ComPtr<IDXGIAdapter1> adapter;
        Check(factory->EnumAdapterByGpuPreference(0, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, IID_PPV_ARGS(&adapter)),
              "EnumAdapterByGpuPreference");
        DXGI_ADAPTER_DESC1 adapterDesc {};
        adapter->GetDesc1(&adapterDesc);
        Out("adapter %ls", adapterDesc.Description);
        Check(createDevice(adapter.Get(), D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&device)), "D3D12CreateDevice");
        if (options.debugLayer && SUCCEEDED(device.As(&info)))
        {
            info->SetMessageCountLimit(4096);
            D3D12_MESSAGE_SEVERITY allow[] = { D3D12_MESSAGE_SEVERITY_CORRUPTION, D3D12_MESSAGE_SEVERITY_ERROR };
            D3D12_INFO_QUEUE_FILTER filter {};
            filter.AllowList.NumSeverities = 2;
            filter.AllowList.pSeverityList = allow;
            info->PushStorageFilter(&filter);
        }
        D3D12_COMMAND_QUEUE_DESC queueDesc {};
        queueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        Check(device->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&queue)), "CreateCommandQueue");

        format = options.bgra ? DXGI_FORMAT_B8G8R8A8_UNORM : DXGI_FORMAT_R8G8B8A8_UNORM;
        BOOL tearing = FALSE;
        factory->CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING, &tearing, sizeof(tearing));
        swapFlags = tearing ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0;
        DXGI_SWAP_CHAIN_DESC1 desc {};
        desc.Width = width;
        desc.Height = height;
        desc.Format = format;
        desc.SampleDesc.Count = 1;
        desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        desc.BufferCount = bufferCount;
        desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
        desc.Flags = swapFlags;
        ComPtr<IDXGISwapChain1> swap1;
        Check(factory->CreateSwapChainForHwnd(queue.Get(), window, &desc, nullptr, nullptr, &swap1),
              "CreateSwapChainForHwnd");
        Check(swap1.As(&swapchain), "IDXGISwapChain3");
        Out("objects queue=%p", queue.Get());
        LogBuffers();
        factory->MakeWindowAssociation(window, DXGI_MWA_NO_ALT_ENTER);

        Check(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)), "CreateFence");
        fenceEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        for (auto& allocator : allocators)
            Check(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)),
                  "CreateCommandAllocator");
        Check(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocators[0].Get(), nullptr,
                                        IID_PPV_ARGS(&list)),
              "CreateCommandList");
        Check(list->Close(), "Close");
        D3D12_DESCRIPTOR_HEAP_DESC heap {};
        heap.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
        heap.NumDescriptors = bufferCount + 1;
        Check(device->CreateDescriptorHeap(&heap, IID_PPV_ARGS(&rtvHeap)), "CreateDescriptorHeap");
        CreatePipeline();
        CreateProbe();
    }

    // Addresses to tell the app's own objects apart in debug-layer messages.
    void LogBuffers()
    {
        for (UINT i = 0; i < bufferCount; ++i)
        {
            ComPtr<ID3D12Resource> buffer;
            if (SUCCEEDED(swapchain->GetBuffer(i, IID_PPV_ARGS(&buffer))))
                Out("objects backbuffer%u=%p", i, buffer.Get());
        }
    }

    void CreatePipeline()
    {
        D3D12_ROOT_PARAMETER param {};
        param.ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        param.Constants.Num32BitValues = 4;
        param.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
        D3D12_ROOT_SIGNATURE_DESC rootDesc {};
        rootDesc.NumParameters = 1;
        rootDesc.pParameters = &param;
        ComPtr<ID3DBlob> blob, errors;
        Check(serializeRoot(&rootDesc, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &errors), "D3D12SerializeRootSignature");
        Check(device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&root)),
              "CreateRootSignature");
        static const char vs[] = "float4 main(uint id:SV_VertexID):SV_Position{"
                                 "float2 p=float2((id<<1)&2,id&2);return float4(p*float2(2,-2)+float2(-1,1),0,1);}";
        // A camera pan over a checkerboard, a disc that moves on its own and a static HUD bar.
        static const char ps[] = "cbuffer C:register(b0){float t;float w;float h;float pad;};"
                                 "float4 main(float4 p:SV_Position):SV_Target{"
                                 "float2 q=p.xy+float2(t*120.0,t*40.0);"
                                 "float c=((int(floor(q.x/64.0))+int(floor(q.y/64.0)))&1)?0.8:0.2;"
                                 "float3 col=float3(c,c*0.9+0.1*p.x/w,c*0.8+0.2*p.y/h);"
                                 "float2 m=float2(w*(0.5+0.3*sin(t)),h*(0.5+0.25*cos(t*0.7)));"
                                 "if(length(p.xy-m)<h*0.12)col=float3(0.9,0.3,0.2);"
                                 "if(p.y>h-40.0)col=float3(0.1,0.1,0.12);"
                                 "return float4(col,1);}";
        ComPtr<ID3DBlob> vertex, pixel;
        Check(D3DCompile(vs, sizeof(vs) - 1, "vs", nullptr, nullptr, "main", "vs_5_0", 0, 0, &vertex, &errors),
              "D3DCompile vs");
        Check(D3DCompile(ps, sizeof(ps) - 1, "ps", nullptr, nullptr, "main", "ps_5_0", 0, 0, &pixel, &errors),
              "D3DCompile ps");
        D3D12_GRAPHICS_PIPELINE_STATE_DESC pso {};
        pso.pRootSignature = root.Get();
        pso.VS = { vertex->GetBufferPointer(), vertex->GetBufferSize() };
        pso.PS = { pixel->GetBufferPointer(), pixel->GetBufferSize() };
        pso.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
        pso.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
        pso.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
        pso.SampleMask = UINT_MAX;
        pso.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        pso.NumRenderTargets = 1;
        pso.RTVFormats[0] = format;
        pso.SampleDesc.Count = 1;
        Check(device->CreateGraphicsPipelineState(&pso, IID_PPV_ARGS(&pipeline)), "CreateGraphicsPipelineState");
    }

    // A reference copy of the frame rendered without OptiScaler in between, and two readback buffers.
    void CreateProbe()
    {
        D3D12_HEAP_PROPERTIES heap {};
        heap.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC desc {};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        desc.Width = width;
        desc.Height = height;
        desc.DepthOrArraySize = 1;
        desc.MipLevels = 1;
        desc.Format = format;
        desc.SampleDesc.Count = 1;
        desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
        reference.Reset();
        Check(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_COPY_SOURCE,
                                              nullptr, IID_PPV_ARGS(&reference)),
              "reference texture");
        UINT64 bytes = 0;
        device->GetCopyableFootprints(&desc, 0, 1, 0, &footprint, nullptr, nullptr, &bytes);
        heap.Type = D3D12_HEAP_TYPE_READBACK;
        D3D12_RESOURCE_DESC buffer {};
        buffer.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        buffer.Width = bytes;
        buffer.Height = buffer.DepthOrArraySize = buffer.MipLevels = 1;
        buffer.SampleDesc.Count = 1;
        buffer.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        readBack.Reset();
        readReference.Reset();
        Check(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &buffer, D3D12_RESOURCE_STATE_COPY_DEST,
                                              nullptr, IID_PPV_ARGS(&readBack)),
              "readback");
        Check(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &buffer, D3D12_RESOURCE_STATE_COPY_DEST,
                                              nullptr, IID_PPV_ARGS(&readReference)),
              "readback reference");
        auto rtv = rtvHeap->GetCPUDescriptorHandleForHeapStart();
        rtv.ptr += SIZE_T(bufferCount) * device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
        device->CreateRenderTargetView(reference.Get(), nullptr, rtv);
    }

    static void Barrier(ID3D12GraphicsCommandList* c, ID3D12Resource* r, D3D12_RESOURCE_STATES a,
                        D3D12_RESOURCE_STATES b)
    {
        D3D12_RESOURCE_BARRIER v {};
        v.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        v.Transition = { r, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, a, b };
        c->ResourceBarrier(1, &v);
    }

    void Draw(D3D12_CPU_DESCRIPTOR_HANDLE rtv)
    {
        const float constants[4] = { float(frame) / 60.0f, float(width), float(height), 0 };
        D3D12_VIEWPORT viewport { 0, 0, float(width), float(height), 0, 1 };
        D3D12_RECT scissor { 0, 0, LONG(width), LONG(height) };
        list->SetGraphicsRootSignature(root.Get());
        list->SetPipelineState(pipeline.Get());
        list->SetGraphicsRoot32BitConstants(0, 4, constants, 0);
        list->RSSetViewports(1, &viewport);
        list->RSSetScissorRects(1, &scissor);
        list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
        list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        list->DrawInstanced(3, 1, 0, 0);
    }

    struct ProbeResult
    {
        double changed = -1; // fraction of pixels whose colour differs by more than 2 levels
        double mean = 0;     // mean absolute difference per channel, in levels
    };

    // Renders one frame; with probe set, reads back what was presented and the reference.
    HRESULT Frame(ProbeResult* probe)
    {
        if (options.cpuMs > 0)
        {
            LARGE_INTEGER now, end, rate;
            QueryPerformanceFrequency(&rate);
            QueryPerformanceCounter(&end);
            end.QuadPart += LONGLONG(options.cpuMs * rate.QuadPart / 1000);
            do
                QueryPerformanceCounter(&now);
            while (now.QuadPart < end.QuadPart);
        }
        const UINT index = swapchain->GetCurrentBackBufferIndex();
        Wait(allocatorFence[index]);
        ComPtr<ID3D12Resource> back;
        Check(swapchain->GetBuffer(index, IID_PPV_ARGS(&back)), "GetBuffer");
        auto rtv = rtvHeap->GetCPUDescriptorHandleForHeapStart();
        const auto step = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
        auto backRtv = rtv;
        backRtv.ptr += SIZE_T(index) * step;
        device->CreateRenderTargetView(back.Get(), nullptr, backRtv);
        Check(allocators[index]->Reset(), "allocator Reset");
        Check(list->Reset(allocators[index].Get(), nullptr), "list Reset");
        Barrier(list.Get(), back.Get(), D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET);
        Draw(backRtv);
        Barrier(list.Get(), back.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PRESENT);
        if (probe)
        {
            auto referenceRtv = rtv;
            referenceRtv.ptr += SIZE_T(bufferCount) * step;
            Barrier(list.Get(), reference.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
            Draw(referenceRtv);
            Barrier(list.Get(), reference.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
        }
        Check(list->Close(), "list Close");
        ID3D12CommandList* lists[] = { list.Get() };
        queue->ExecuteCommandLists(1, lists);
        const HRESULT presented = swapchain->Present(0, swapFlags ? DXGI_PRESENT_ALLOW_TEARING : 0);
        if (probe && SUCCEEDED(presented))
        {
            // Same queue, after Present: sees what the game frame became on its way to the screen.
            Idle();
            Check(allocators[index]->Reset(), "probe allocator Reset");
            Check(list->Reset(allocators[index].Get(), nullptr), "probe list Reset");
            Barrier(list.Get(), back.Get(), D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_COPY_SOURCE);
            D3D12_TEXTURE_COPY_LOCATION dst { readBack.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT };
            dst.PlacedFootprint = footprint;
            D3D12_TEXTURE_COPY_LOCATION src { back.Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX };
            list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
            dst.pResource = readReference.Get();
            src.pResource = reference.Get();
            list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
            Barrier(list.Get(), back.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_PRESENT);
            Check(list->Close(), "probe list Close");
            queue->ExecuteCommandLists(1, lists);
            Idle();
            *probe = Compare();
        }
        Check(queue->Signal(fence.Get(), ++fenceValue), "Signal");
        allocatorFence[index] = fenceValue;
        ++frame;
        return presented;
    }

    // The presented frame as a top-down 32-bit BMP.
    void Dump(const char* phase)
    {
        void* data = nullptr;
        Check(readBack->Map(0, nullptr, &data), "Map dump");
        BITMAPFILEHEADER file {};
        BITMAPINFOHEADER info {};
        info.biSize = sizeof(info);
        info.biWidth = LONG(width);
        info.biHeight = -LONG(height);
        info.biPlanes = 1;
        info.biBitCount = 32;
        file.bfType = 0x4D42;
        file.bfOffBits = sizeof(file) + sizeof(info);
        file.bfSize = file.bfOffBits + width * height * 4;
        std::ofstream out(Here() / (std::string(phase) + ".bmp"), std::ios::binary);
        out.write((const char*) &file, sizeof(file));
        out.write((const char*) &info, sizeof(info));
        std::vector<unsigned char> row(width * 4);
        for (UINT y = 0; y < height; ++y)
        {
            std::memcpy(row.data(), (const unsigned char*) data + UINT64(y) * footprint.Footprint.RowPitch, row.size());
            if (format == DXGI_FORMAT_R8G8B8A8_UNORM)
                for (UINT x = 0; x < width; ++x)
                    std::swap(row[x * 4], row[x * 4 + 2]);
            out.write((const char*) row.data(), row.size());
        }
        readBack->Unmap(0, nullptr);
    }

    // Over the frame right of 35% of its width: a driver performance overlay may draw on the left.
    ProbeResult Compare()
    {
        const UINT left = width * 35 / 100;
        void* a = nullptr;
        void* b = nullptr;
        Check(readBack->Map(0, nullptr, &a), "Map");
        Check(readReference->Map(0, nullptr, &b), "Map reference");
        UINT64 changed = 0, sum = 0;
        for (UINT y = 0; y < height; ++y)
        {
            auto rowA = (const unsigned char*) a + UINT64(y) * footprint.Footprint.RowPitch;
            auto rowB = (const unsigned char*) b + UINT64(y) * footprint.Footprint.RowPitch;
            for (UINT x = left; x < width; ++x)
            {
                int worst = 0;
                for (int c = 0; c < 3; ++c)
                {
                    const int d = std::abs(int(rowA[x * 4 + c]) - int(rowB[x * 4 + c]));
                    worst = std::max(worst, d);
                    sum += d;
                }
                changed += worst > 2;
            }
        }
        readBack->Unmap(0, nullptr);
        readReference->Unmap(0, nullptr);
        const double pixels = double(width - left) * height;
        return { changed / pixels, sum / (pixels * 3) };
    }

    void Resize(UINT w, UINT h)
    {
        Idle();
        RECT rect { 0, 0, LONG(w), LONG(h) };
        AdjustWindowRect(&rect, WS_OVERLAPPEDWINDOW, FALSE);
        SetWindowPos(window, nullptr, 0, 0, rect.right - rect.left, rect.bottom - rect.top, SWP_NOMOVE | SWP_NOZORDER);
        Check(swapchain->ResizeBuffers(0, w, h, DXGI_FORMAT_UNKNOWN, swapFlags), "ResizeBuffers");
        LogBuffers();
        width = w;
        height = h;
        CreateProbe();
        for (auto& value : allocatorFence)
            value = 0;
    }

    // Holds the key for about as long as a person does, rendering meanwhile.
    void PressKey(UINT vk, bool extended, bool (*pump)())
    {
        const UINT scan = MapVirtualKeyW(vk, MAPVK_VK_TO_VSC);
        const LPARAM down = 1 | (LPARAM(scan) << 16) | (extended ? 1 << 24 : 0);
        const LPARAM up = down | (1u << 30) | (1u << 31);
        PostMessageW(window, WM_KEYDOWN, vk, down);
        const auto until = GetTickCount64() + 120;
        while (GetTickCount64() < until)
        {
            pump();
            const HRESULT hr = Frame(nullptr);
            if (FAILED(hr))
                Fail("Present while a key is held", hr);
        }
        PostMessageW(window, WM_KEYUP, vk, up);
    }

    void Render(double seconds, bool (*pump)())
    {
        const auto until = GetTickCount64() + ULONGLONG(seconds * 1000);
        while (GetTickCount64() < until)
        {
            pump();
            const HRESULT hr = Frame(nullptr);
            if (FAILED(hr))
                Fail("Present while driving the menu", hr);
        }
    }

    // A real cursor move and click, as the menu polls both. Places are those of a 1280x720 window at the
    // default menu scale, where the menu opens centred with its pages down the left.
    void Click(LONG x, LONG y, bool (*pump)())
    {
        SetWindowPos(window, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);
        POINT point { x, y };
        ClientToScreen(window, &point);
        SetCursorPos(point.x, point.y);
        Render(0.3, pump);
        INPUT click[2] {};
        click[0].type = click[1].type = INPUT_MOUSE;
        click[0].mi.dwFlags = MOUSEEVENTF_LEFTDOWN;
        click[1].mi.dwFlags = MOUSEEVENTF_LEFTUP;
        SendInput(1, &click[0], sizeof(INPUT));
        Render(0.2, pump);
        SendInput(1, &click[1], sizeof(INPUT));
        Render(0.5, pump);
        // Off the window, as a hand leaves the mouse, so the menu does not find it over a control when it reopens.
        SetCursorPos(0, 0);
        SetWindowPos(window, HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);
    }

    // What the window shows, the menu over frame generation's output included, as <name>.bmp.
    void Capture(const char* name)
    {
        RECT client {};
        GetClientRect(window, &client);
        HDC screen = GetDC(window);
        HDC memory = CreateCompatibleDC(screen);
        BITMAPINFO info {};
        info.bmiHeader = { sizeof(BITMAPINFOHEADER), client.right, -client.bottom, 1, 32 };
        void* bits = nullptr;
        HBITMAP bitmap = CreateDIBSection(screen, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
        SelectObject(memory, bitmap);
        PrintWindow(window, memory, PW_CLIENTONLY | PW_RENDERFULLCONTENT);
        BITMAPFILEHEADER file { 0x4D42 };
        file.bfOffBits = sizeof(file) + sizeof(BITMAPINFOHEADER);
        std::ofstream out(Here() / (std::string(name) + ".bmp"), std::ios::binary);
        out.write((const char*) &file, sizeof(file));
        out.write((const char*) &info.bmiHeader, sizeof(BITMAPINFOHEADER));
        out.write((const char*) bits, std::streamsize(client.right) * client.bottom * 4);
        DeleteObject(bitmap);
        DeleteDC(memory);
        ReleaseDC(window, screen);
    }

    // Sets the Model scale slider on the NR page, where the menu opens, a fifth of the way along (about 0.6)
    // and closes the menu.
    void ChangeModelScale(bool (*pump)())
    {
        // A click the menu misses (the window not yet in front, say) is tried once more.
        const int before = SettingsResets(ReadAll(Here() / L"amd_presr.log"));
        for (int attempt = 0; attempt < 2; ++attempt)
        {
            PressKey(VK_INSERT, true, pump);
            Render(1.0, pump);
            if (options.dump)
                Capture("scale-menu");
            Click(372 + 126 / 5, 395, pump);
            PressKey(VK_INSERT, true, pump);
            Render(0.5, pump);
            if (SettingsResets(ReadAll(Here() / L"amd_presr.log")) > before)
                break;
            Out("scale-click missed, attempt %d", attempt + 1);
        }
    }

    void DebugMessages(const char* phase)
    {
        if (!info)
            return;
        const UINT64 count = info->GetNumStoredMessagesAllowedByRetrievalFilter();
        std::map<std::string, int> unique;
        for (UINT64 i = 0; i < count; ++i)
        {
            SIZE_T size = 0;
            info->GetMessage(i, nullptr, &size);
            std::vector<char> storage(size);
            auto message = (D3D12_MESSAGE*) storage.data();
            if (SUCCEEDED(info->GetMessage(i, message, &size)))
                ++unique[std::string(message->pDescription, message->DescriptionByteLength)];
        }
        info->ClearStoredMessages();
        Out("debug %s errors=%llu unique=%zu", phase, count, unique.size());
        int shown = 0;
        for (auto& [text, n] : unique)
            if (shown++ < 12)
                Out("debug-message %s x%d %s", phase, n, text.substr(0, 400).c_str());
    }
};

static LRESULT CALLBACK WindowProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    if (msg == WM_CLOSE)
        return 0;
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

static bool Pump()
{
    MSG msg;
    while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE))
    {
        if (msg.message == WM_QUIT)
            return false;
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return true;
}

static int Child(const Options& options)
{
    results = _wfopen((Here() / L"dx12_smoke_result.txt").c_str(), L"w");
    SetUnhandledExceptionFilter(Crash);
    App app;
    app.options = options;
    WNDCLASSW type {};
    type.lpfnWndProc = WindowProc;
    type.hInstance = GetModuleHandleW(nullptr);
    type.lpszClassName = L"OptiDx12FinalImageSmoke";
    type.hCursor = LoadCursor(nullptr, IDC_ARROW);
    RegisterClassW(&type);
    RECT rect { 0, 0, LONG(app.width), LONG(app.height) };
    AdjustWindowRect(&rect, WS_OVERLAPPEDWINDOW, FALSE);
    app.window =
        CreateWindowW(type.lpszClassName, L"Opti D3D12 final-image smoke", WS_OVERLAPPEDWINDOW, 40, 40,
                      rect.right - rect.left, rect.bottom - rect.top, nullptr, nullptr, type.hInstance, nullptr);
    if (!app.window)
        Fail("CreateWindow");
    ShowWindow(app.window, SW_SHOW);
    SetForegroundWindow(app.window);
    app.CreateDevice();

    using Clock = std::chrono::steady_clock;
    const auto Seconds = [](Clock::time_point since)
    { return std::chrono::duration<double>(Clock::now() - since).count(); };

    // Warm-up: the runtime loads, optical flow settles, XeFG starts. Ends when a probe sees NR.
    auto start = Clock::now();
    auto lastProbe = start;
    bool warm = false;
    while (Seconds(start) < 60 && !warm)
    {
        if (!Pump())
            break;
        App::ProbeResult probe;
        const bool probeNow = Seconds(lastProbe) > 1.0;
        const HRESULT hr = app.Frame(probeNow ? &probe : nullptr);
        if (FAILED(hr))
            Fail("Present during warm-up", hr);
        if (probeNow)
        {
            lastProbe = Clock::now();
            warm = probe.changed > 0.3;
        }
    }
    Out("warmup seconds=%.1f nr=%d frames=%llu", Seconds(start), warm ? 1 : 0, app.frame);
    // Let XeFG run a few seconds on top of a working NR before measuring.
    const auto settle = Clock::now();
    while (Seconds(settle) < 3)
    {
        Pump();
        if (FAILED(app.Frame(nullptr)))
            Fail("Present while settling");
    }
    app.DebugMessages("warmup");

    struct Step
    {
        const char* phase; // measured after the action, for `seconds`
        int key;           // key to press first; 0 none, -1 resize, -2 Model scale from the menu
        double delay;      // seconds to wait after the action before measuring
        double seconds;
    };
    const Step steps[] = {
        { "steady", 0, 0, 4 },
        { "menu-open", VK_INSERT, 1, 4 },
        { "menu-closed", VK_INSERT, 1, 3 },
        { "fg-off", VK_END, 1.5, 3 },
        { "fg-on", VK_END, 2.5, 4 },
        { "nr-off", VK_NUMPAD5, 1.5, 3 },
        { "nr-on", VK_NUMPAD5, 2.5, 4 },
        { "scale", -2, 2.5, 4 },
        { "resized", -1, 3, 4 },
        { "menu-resized", VK_INSERT, 1, 3 },
        { "end", VK_INSERT, 1, 2 },
    };
    for (const auto& step : steps)
    {
        if (step.key > 0)
            app.PressKey(step.key, step.key == VK_INSERT || step.key == VK_END, Pump);
        else if (step.key == -1)
            app.Resize(1600, 900);
        else if (step.key == -2)
            app.ChangeModelScale(Pump);
        auto begin = Clock::now();
        while (Seconds(begin) < step.delay)
        {
            Pump();
            const HRESULT hr = app.Frame(nullptr);
            if (FAILED(hr))
                Fail(step.phase, hr);
        }
        UINT presentStart = 0, presentEnd = 0;
        app.swapchain->GetLastPresentCount(&presentStart);
        const UINT64 frameStart = app.frame;
        begin = Clock::now();
        std::vector<App::ProbeResult> probes;
        double nextProbe = step.seconds / 8, worst = 0;
        while (Seconds(begin) < step.seconds)
        {
            Pump();
            App::ProbeResult probe;
            const bool probeNow = Seconds(begin) >= nextProbe && probes.size() < 6;
            const auto started = Clock::now();
            const HRESULT hr = app.Frame(probeNow ? &probe : nullptr);
            // The longest frame that was not a probe: a stall in Present or in the frame before it.
            if (!probeNow)
                worst = std::max(worst, 1000 * Seconds(started));
            if (FAILED(hr))
                Fail(step.phase, hr);
            if (probeNow)
            {
                if (probes.empty() && app.options.dump)
                    app.Dump(step.phase);
                probes.push_back(probe);
                nextProbe += step.seconds / 8;
            }
        }
        const double elapsed = Seconds(begin);
        app.swapchain->GetLastPresentCount(&presentEnd);
        // Probe frames wait for the GPU and count in the rate. least is the probe NR changed least: a frame
        // that missed NR shows there.
        const UINT64 frames = app.frame - frameStart;
        double changed = 0, least = 1, mean = 0;
        for (auto& p : probes)
        {
            changed = std::max(changed, p.changed);
            least = std::min(least, p.changed);
            mean = std::max(mean, p.mean);
        }
        Out("phase %s frames=%llu seconds=%.2f fps=%.1f presents=%u ratio=%.2f changed=%.4f least=%.4f mean=%.3f "
            "size=%ux%u worst=%.1fms",
            step.phase, frames, elapsed, frames / elapsed, presentEnd - presentStart,
            frames ? double(presentEnd - presentStart) / frames : 0.0, changed, least, mean, app.width, app.height,
            worst);
        app.DebugMessages(step.phase);
    }
    app.Idle();
    Out("child-done frames=%llu", app.frame);
    if (results)
        std::fclose(results);
    results = nullptr;
    // Leave through the normal process exit, as a game does.
    return 0;
}

// ---------------------------------------------------------------- parent

struct Lock
{
    std::wstring dir;
    explicit Lock(std::wstring path) : dir(std::move(path))
    {
        if (dir.empty())
            return;
        for (;;)
        {
            if (CreateDirectoryW(dir.c_str(), nullptr))
                return;
            WIN32_FILE_ATTRIBUTE_DATA data {};
            if (GetFileAttributesExW(dir.c_str(), GetFileExInfoStandard, &data))
            {
                FILETIME now;
                GetSystemTimeAsFileTime(&now);
                const auto age =
                    (ULARGE_INTEGER { now.dwLowDateTime, now.dwHighDateTime }.QuadPart -
                     ULARGE_INTEGER { data.ftCreationTime.dwLowDateTime, data.ftCreationTime.dwHighDateTime }
                         .QuadPart) /
                    10000000ull;
                if (age > 20 * 60)
                {
                    RemoveDirectoryW(dir.c_str());
                    continue;
                }
            }
            std::printf("waiting for the GPU lock\n");
            Sleep(20000);
        }
    }
    ~Lock()
    {
        if (!dir.empty())
            RemoveDirectoryW(dir.c_str());
    }
};

struct PhaseResult
{
    double ratio = 0, changed = -1, least = -1, fps = 0;
};

static std::map<std::string, PhaseResult> ParsePhases(const std::string& text)
{
    std::map<std::string, PhaseResult> phases;
    std::istringstream lines(text);
    for (std::string line; std::getline(lines, line);)
    {
        char name[64] {};
        PhaseResult r;
        if (std::sscanf(line.c_str(),
                        "phase %63s frames=%*u seconds=%*f fps=%lf presents=%*u ratio=%lf changed=%lf least=%lf", name,
                        &r.fps, &r.ratio, &r.changed, &r.least) == 5)
            phases[name] = r;
    }
    return phases;
}

static int Parent(const Options& options, const std::wstring& lock, DWORD timeoutSeconds, const std::wstring& args)
{
    const auto dir = Here();
    std::error_code ec;
    if (!fs::copy_file(dir / L"OptiScaler.template.ini", dir / L"OptiScaler.ini", fs::copy_options::overwrite_existing,
                       ec))
    {
        std::printf("FAIL: OptiScaler.template.ini missing\n");
        return 2;
    }
    const auto ini = (dir / L"OptiScaler.ini").wstring();
    const bool fg = options.scenario == "fg";
    const std::pair<const wchar_t*, std::pair<const wchar_t*, const wchar_t*>> keys[] = {
        { L"Log", { L"LogToFile", L"true" } },
        { L"Log", { L"LogLevel", options.logLevel.c_str() } },
        { L"Log", { L"LogAsync", L"false" } },
        { L"DlssNr", { L"Enabled", L"true" } },
        { L"DlssNr", { L"PresentWithoutUpscaler", L"true" } },
        { L"DlssNr", { L"NrBackend", L"daniel" } },
        { L"FrameGen", { L"Enabled", fg ? L"true" : L"false" } },
        { L"FrameGen", { L"FGInput", fg ? L"upscaler" : L"nofg" } },
        { L"FrameGen", { L"FGOutput", fg ? L"xefg" : L"nofg" } },
        { L"XeFG", { L"InterpolationCount", L"2" } },
        { L"DlssNr", { L"AmdModelScale", options.modelScale.c_str() } },
    };
    for (auto& [section, kv] : keys)
        WritePrivateProfileStringW(section, kv.first, kv.second, ini.c_str());
    for (auto name : { L"OptiScaler.log", L"amd_presr.log", L"dx12_smoke_result.txt" })
        fs::remove(dir / name, ec);

    int code = 0;
    {
        Lock held(lock);
        wchar_t self[MAX_PATH] {};
        GetModuleFileNameW(nullptr, self, MAX_PATH);
        std::wstring command = L"\"" + std::wstring(self) + L"\" --child " + args;
        STARTUPINFOW startup { sizeof(startup) };
        PROCESS_INFORMATION process {};
        if (!CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE, 0, nullptr, dir.c_str(), &startup,
                            &process))
        {
            std::printf("FAIL: child did not start\n");
            return 2;
        }
        if (WaitForSingleObject(process.hProcess, timeoutSeconds * 1000) != WAIT_OBJECT_0)
        {
            TerminateProcess(process.hProcess, 9);
            WaitForSingleObject(process.hProcess, 10000);
            std::printf("FAIL: child timed out after %lu s\n", timeoutSeconds);
            code = 1;
        }
        DWORD exitCode = 0;
        GetExitCodeProcess(process.hProcess, &exitCode);
        CloseHandle(process.hProcess);
        CloseHandle(process.hThread);
        std::printf("child exit code %lu\n", exitCode);
        if (exitCode != 0)
            code = 1;
    }

    const auto result = ReadAll(dir / L"dx12_smoke_result.txt");
    const auto log = ReadAll(dir / L"OptiScaler.log");
    const auto runtimeLog = ReadAll(dir / L"amd_presr.log");
    const auto phases = ParsePhases(result);
    int failures = 0;
    auto expect = [&](bool ok, const char* what)
    {
        std::printf("%s: %s\n", ok ? "ok  " : "FAIL", what);
        failures += !ok;
    };
    auto phase = [&](const char* name) { return phases.contains(name) ? phases.at(name) : PhaseResult {}; };
    expect(code == 0, "child ran to completion and exited cleanly");
    expect(Count(result, "child-done") == 1, "child finished every phase");
    expect(Count(log, "887A0005") == 0 && Count(log, "Device removed reason") == 0, "no device removal in the log");
    expect(Count(log, "OptiInput::ApplyMenuVisibilityChangeLocked menu visibility changed 0 -> 1") >= 2,
           "menu opened twice");
    expect(Count(log, "OptiInput::ApplyMenuVisibilityChangeLocked menu visibility changed 1 -> 0") >= 2,
           "menu closed twice");
    // The runtime falls back for a frame on its own timeouts; a loaded machine can cause them, so they are counted.
    std::printf("info: NR runtime timeouts %d, frames left without NR while the runtime was busy %d\n",
                Count(runtimeLog, "AMD timeout"), Count(log, "runtime still busy, frame left without NR"));
    // A Model scale change restarts the runtime's history for its settings.
    expect(SettingsResets(runtimeLog) >= 1, "Model scale changed from the menu during the session");
    for (auto name :
         { "steady", "menu-open", "menu-closed", "fg-off", "fg-on", "nr-on", "scale", "resized", "menu-resized" })
    {
        std::string what = std::string("NR changed every probed frame in ") + name;
        expect(phase(name).least > 0.3, what.c_str());
    }
    // The menu's "Off" notification covers well under 5% of the frame; NR changes most of it.
    expect(phase("nr-off").changed < 0.05, "NR off leaves the presented frame untouched");
    if (fg)
    {
        expect(Count(log, "XeFG swapchain created") >= 1, "XeFG swapchain created");
        for (auto name :
             { "steady", "menu-open", "menu-closed", "fg-on", "nr-off", "nr-on", "scale", "resized", "menu-resized" })
        {
            std::string what = std::string("XeFG presents generated frames in ") + name + " (ratio >= 2.4)";
            expect(phase(name).ratio >= 2.4, what.c_str());
        }
        expect(phase("fg-off").ratio < 1.2, "FG off presents one frame per game frame");
    }
    else
    {
        for (auto& [name, r] : phases)
        {
            std::string what = "one present per game frame in " + name;
            expect(r.ratio > 0.9 && r.ratio < 1.1, what.c_str());
        }
    }
    if (options.debugLayer)
        expect(Count(result, "debug-layer on") == 1, "D3D12 debug layer enabled");
    std::printf("%s: %d failure(s)\n", failures ? "FAILED" : "PASSED", failures);
    return failures ? 1 : 0;
}

int wmain(int argc, wchar_t** argv)
{
    Options options;
    bool child = false;
    std::wstring lock;
    DWORD timeout = 240;
    for (int i = 1; i < argc; ++i)
    {
        const std::wstring arg = argv[i];
        if (arg == L"--child")
            child = true;
        else if (arg == L"--bgra")
            options.bgra = true;
        else if (arg == L"--debug-layer")
            options.debugLayer = true;
        else if (arg == L"--lock" && i + 1 < argc)
            lock = argv[++i];
        else if (arg == L"--dump")
            options.dump = true;
        else if (arg == L"--cpu-ms" && i + 1 < argc)
            options.cpuMs = std::wcstod(argv[++i], nullptr);
        else if (arg == L"--model-scale" && i + 1 < argc)
            options.modelScale = argv[++i];
        else if (arg == L"--log-level" && i + 1 < argc)
            options.logLevel = argv[++i];
        else if (arg == L"--timeout" && i + 1 < argc)
            timeout = std::wcstoul(argv[++i], nullptr, 10);
        else
            for (wchar_t c : arg)
                options.scenario += char(c);
    }
    if (options.scenario != "nr" && options.scenario != "fg")
    {
        std::printf("usage: dx12_final_image_smoke.exe <nr|fg> [--bgra] [--debug-layer] [--lock <dir>] "
                    "[--timeout <seconds>]\n");
        return 2;
    }
    if (child)
        return Child(options);
    std::wstring args(options.scenario.begin(), options.scenario.end());
    args += options.bgra ? L" --bgra" : L"";
    args += options.debugLayer ? L" --debug-layer" : L"";
    args += options.cpuMs > 0 ? L" --cpu-ms " + std::to_wstring(options.cpuMs) : L"";
    args += options.dump ? L" --dump" : L"";
    return Parent(options, lock, timeout, args);
}
