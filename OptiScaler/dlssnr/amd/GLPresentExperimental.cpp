#include "pch.h"

#include "GLPresentExperimental.h"
#include "PresentExperimental.h"
#include "FgChildPresenter.h"
#include "AmdBridge.h"
#include "../backend/Selector.h"

#include <Config.h>
#include <Logger.h>
#include <Util.h>
#include <State.h>
#include <hooks/GL_Hooks.h>

#include <d3d12.h>
#include <dxgi1_4.h>
#include <wrl/client.h>

#include <chrono>
#include <cstring>
#include <vector>

namespace AmdGlPresent
{
using Microsoft::WRL::ComPtr;

namespace
{
typedef unsigned int GLenum;
typedef unsigned int GLuint;
typedef unsigned int GLbitfield;
typedef int GLint;
typedef int GLsizei;
typedef unsigned long long GLuint64;

constexpr GLenum GL_TEXTURE_2D_ = 0x0DE1;
constexpr GLenum GL_RGBA_ = 0x1908;
constexpr GLenum GL_UNSIGNED_BYTE_ = 0x1401;
constexpr GLenum GL_RGBA8_ = 0x8058;
constexpr GLenum GL_SAMPLES_ = 0x80A9;
constexpr GLenum GL_NEAREST_ = 0x2600;
constexpr GLenum GL_TEXTURE_MIN_FILTER_ = 0x2801;
constexpr GLenum GL_COLOR_BUFFER_BIT_ = 0x4000;
constexpr GLenum GL_FRAMEBUFFER_ = 0x8D40;
constexpr GLenum GL_READ_FRAMEBUFFER_ = 0x8CA8;
constexpr GLenum GL_DRAW_FRAMEBUFFER_ = 0x8CA9;
constexpr GLenum GL_COLOR_ATTACHMENT0_ = 0x8CE0;
constexpr GLenum GL_FRAMEBUFFER_COMPLETE_ = 0x8CD5;
constexpr GLenum GL_TEXTURE_TILING_EXT_ = 0x9580;
constexpr GLenum GL_DEDICATED_MEMORY_OBJECT_EXT_ = 0x9581;
constexpr GLenum GL_OPTIMAL_TILING_EXT_ = 0x9584;
constexpr GLenum GL_HANDLE_TYPE_D3D12_RESOURCE_EXT_ = 0x958A;
constexpr GLenum GL_DEVICE_LUID_EXT_ = 0x9599;

struct Gl
{
    void(WINAPI* finish)();
    GLenum(WINAPI* getError)();
    void(WINAPI* getIntegerv)(GLenum, GLint*);
    void(WINAPI* genTextures)(GLsizei, GLuint*);
    void(WINAPI* deleteTextures)(GLsizei, const GLuint*);
    void(WINAPI* bindTexture)(GLenum, GLuint);
    void(WINAPI* texParameteri)(GLenum, GLenum, GLint);
    void(WINAPI* texImage2D)(GLenum, GLint, GLint, GLsizei, GLsizei, GLint, GLenum, GLenum, const void*);
    void(WINAPI* genFramebuffers)(GLsizei, GLuint*);
    void(WINAPI* deleteFramebuffers)(GLsizei, const GLuint*);
    void(WINAPI* bindFramebuffer)(GLenum, GLuint);
    void(WINAPI* framebufferTexture2D)(GLenum, GLenum, GLenum, GLuint, GLint);
    GLenum(WINAPI* checkFramebufferStatus)(GLenum);
    void(WINAPI* blitFramebuffer)(GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLbitfield, GLenum);
    void(WINAPI* createMemoryObjects)(GLsizei, GLuint*);
    void(WINAPI* deleteMemoryObjects)(GLsizei, const GLuint*);
    void(WINAPI* memoryObjectParameteriv)(GLuint, GLenum, const GLint*);
    void(WINAPI* importMemoryWin32Handle)(GLuint, GLuint64, GLenum, void*);
    void(WINAPI* texStorageMem2D)(GLenum, GLsizei, GLenum, GLsizei, GLsizei, GLuint, GLuint64);
    void(WINAPI* getUnsignedBytev)(GLenum, unsigned char*);

    template <typename T> static bool Resolve(T& target, const char* name)
    {
        target = reinterpret_cast<T>(GLHooks::GetProc(name));
        return target != nullptr;
    }

    // getUnsignedBytev is optional: without it the D3D12 device goes on the default adapter.
    bool Load()
    {
        Resolve(getUnsignedBytev, "glGetUnsignedBytevEXT");
        return Resolve(finish, "glFinish") && Resolve(getError, "glGetError") &&
               Resolve(getIntegerv, "glGetIntegerv") && Resolve(genTextures, "glGenTextures") &&
               Resolve(deleteTextures, "glDeleteTextures") && Resolve(bindTexture, "glBindTexture") &&
               Resolve(texParameteri, "glTexParameteri") && Resolve(texImage2D, "glTexImage2D") &&
               Resolve(genFramebuffers, "glGenFramebuffers") && Resolve(deleteFramebuffers, "glDeleteFramebuffers") &&
               Resolve(bindFramebuffer, "glBindFramebuffer") &&
               Resolve(framebufferTexture2D, "glFramebufferTexture2D") &&
               Resolve(checkFramebufferStatus, "glCheckFramebufferStatus") &&
               Resolve(blitFramebuffer, "glBlitFramebuffer") &&
               Resolve(createMemoryObjects, "glCreateMemoryObjectsEXT") &&
               Resolve(deleteMemoryObjects, "glDeleteMemoryObjectsEXT") &&
               Resolve(memoryObjectParameteriv, "glMemoryObjectParameterivEXT") &&
               Resolve(importMemoryWin32Handle, "glImportMemoryWin32HandleEXT") &&
               Resolve(texStorageMem2D, "glTexStorageMem2DEXT");
    }

    bool Clean()
    {
        bool clean = true;
        for (int i = 0; i < 8 && getError() != 0; i++)
            clean = false;

        return clean;
    }
};

// The crossing is created in D3D12 and imported into OpenGL, R8G8B8A8 because the blit writes it
// through OpenGL's RGBA view. Two rules measured by the ReShade add-on's OpenGL route on AMD: the
// memory object is declared dedicated before the import, and the exported NT handle is never
// closed, because closing it faults inside the driver later.
struct Route
{
    Gl gl {};
    uint32_t generation = 0;
    bool stopped = false;
    ComPtr<ID3D12Device> device;
    ComPtr<ID3D12CommandQueue> queue;
    ComPtr<ID3D12Resource> shared;
    GLuint memory = 0, texture = 0, fbo = 0;
    GLuint resolveTexture = 0, resolveFbo = 0;
    GLint samples = 0;
    uint32_t width = 0, height = 0;
    std::vector<HANDLE> keptHandles;
    AmdPresentExperimental::FgPresenter fg { "OpenGL" };
    AmdPresentExperimental::Guides guides;
    bool fgFailed = false;
    bool loggedFirst = false;
    uint64_t totalMicros = 0;
    uint32_t measured = 0;

    void Stop(const std::string& why)
    {
        stopped = true;
        fg.Release();
        AmdPresentExperimental::Report("OpenGL NR: " + why);
        LOG_ERROR("OpenGL final-image NR: {}; the game's image is left alone", why);
    }

    // Objects of an older OptiScaler context are left to it.
    void ReleaseGl(bool sameContext)
    {
        if (sameContext)
        {
            GLuint framebuffers[] { fbo, resolveFbo };
            GLuint textures[] { texture, resolveTexture };
            gl.deleteFramebuffers(2, framebuffers);
            gl.deleteTextures(2, textures);
            if (memory != 0)
                gl.deleteMemoryObjects(1, &memory);
            gl.Clean();
        }

        memory = texture = fbo = resolveTexture = resolveFbo = 0;
        width = height = 0;
    }

    bool CreateDevice()
    {
        ComPtr<IDXGIAdapter1> adapter;
        unsigned char luid[8] {};
        if (gl.getUnsignedBytev != nullptr)
        {
            gl.getUnsignedBytev(GL_DEVICE_LUID_EXT_, luid);
            ComPtr<IDXGIFactory1> factory;
            if (gl.Clean() && SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))))
            {
                for (UINT i = 0;; ++i)
                {
                    ComPtr<IDXGIAdapter1> candidate;
                    if (factory->EnumAdapters1(i, &candidate) == DXGI_ERROR_NOT_FOUND)
                        break;

                    DXGI_ADAPTER_DESC1 description {};
                    if (SUCCEEDED(candidate->GetDesc1(&description)) &&
                        std::memcmp(&description.AdapterLuid, luid, sizeof(luid)) == 0)
                    {
                        adapter = candidate;
                        break;
                    }
                }
            }
        }

        if (FAILED(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device))))
            return false;

        D3D12_COMMAND_QUEUE_DESC queueDesc {};
        queueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        if (FAILED(device->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&queue))))
            return false;

        LOG_INFO("OpenGL final-image NR: D3D12 device on {} adapter", adapter ? "the context's" : "the default");
        return true;
    }

    bool Build(uint32_t w, uint32_t h)
    {
        // The D3D12 route sizes its textures on the first frame it sees.
        if (shared)
            AmdPresentExperimental::BeforeResize();

        fg.Release();
        ReleaseGl(true);
        shared.Reset();

        D3D12_HEAP_PROPERTIES heap {};
        heap.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC description {};
        description.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        description.Width = w;
        description.Height = h;
        description.DepthOrArraySize = description.MipLevels = 1;
        description.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        description.SampleDesc.Count = 1;

        HANDLE handle = nullptr;
        if (FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_SHARED, &description,
                                                   D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&shared))) ||
            FAILED(device->CreateSharedHandle(shared.Get(), nullptr, GENERIC_ALL, nullptr, &handle)))
        {
            LOG_ERROR("OpenGL final-image NR: shared D3D12 texture {}x{} failed", w, h);
            return false;
        }

        keptHandles.push_back(handle);
        const auto bytes = device->GetResourceAllocationInfo(0, 1, &description).SizeInBytes;

        gl.Clean();
        gl.createMemoryObjects(1, &memory);
        const GLint dedicated = 1;
        gl.memoryObjectParameteriv(memory, GL_DEDICATED_MEMORY_OBJECT_EXT_, &dedicated);
        gl.importMemoryWin32Handle(memory, bytes, GL_HANDLE_TYPE_D3D12_RESOURCE_EXT_, handle);
        if (memory == 0 || !gl.Clean())
        {
            LOG_ERROR("OpenGL final-image NR: importing the D3D12 texture was refused");
            return false;
        }

        gl.genTextures(1, &texture);
        gl.bindTexture(GL_TEXTURE_2D_, texture);
        gl.texParameteri(GL_TEXTURE_2D_, GL_TEXTURE_TILING_EXT_, static_cast<GLint>(GL_OPTIMAL_TILING_EXT_));
        gl.texStorageMem2D(GL_TEXTURE_2D_, 1, GL_RGBA8_, static_cast<GLsizei>(w), static_cast<GLsizei>(h), memory, 0);
        gl.bindTexture(GL_TEXTURE_2D_, 0);
        gl.genFramebuffers(1, &fbo);
        gl.bindFramebuffer(GL_DRAW_FRAMEBUFFER_, fbo);
        gl.framebufferTexture2D(GL_DRAW_FRAMEBUFFER_, GL_COLOR_ATTACHMENT0_, GL_TEXTURE_2D_, texture, 0);
        const auto status = gl.checkFramebufferStatus(GL_DRAW_FRAMEBUFFER_);

        // A multisampled default framebuffer is resolved first: one blit cannot resolve and flip.
        gl.bindFramebuffer(GL_FRAMEBUFFER_, 0);
        gl.getIntegerv(GL_SAMPLES_, &samples);
        if (samples > 1)
        {
            gl.genTextures(1, &resolveTexture);
            gl.bindTexture(GL_TEXTURE_2D_, resolveTexture);
            gl.texParameteri(GL_TEXTURE_2D_, GL_TEXTURE_MIN_FILTER_, static_cast<GLint>(GL_NEAREST_));
            gl.texImage2D(GL_TEXTURE_2D_, 0, static_cast<GLint>(GL_RGBA8_), static_cast<GLsizei>(w),
                          static_cast<GLsizei>(h), 0, GL_RGBA_, GL_UNSIGNED_BYTE_, nullptr);
            gl.bindTexture(GL_TEXTURE_2D_, 0);
            gl.genFramebuffers(1, &resolveFbo);
            gl.bindFramebuffer(GL_DRAW_FRAMEBUFFER_, resolveFbo);
            gl.framebufferTexture2D(GL_DRAW_FRAMEBUFFER_, GL_COLOR_ATTACHMENT0_, GL_TEXTURE_2D_, resolveTexture, 0);
            gl.bindFramebuffer(GL_FRAMEBUFFER_, 0);
        }

        if (status != GL_FRAMEBUFFER_COMPLETE_ || !gl.Clean())
        {
            LOG_ERROR("OpenGL final-image NR: the imported texture cannot be rendered to ({:X})", status);
            return false;
        }

        width = w;
        height = h;
        LOG_INFO("OpenGL final-image NR: back buffer {}x{} ({} samples) crosses as a D3D12 texture of {} bytes", w, h,
                 samples, bytes);
        return true;
    }

    // Y is flipped both ways: OpenGL's origin is bottom left, the D3D12 route's top left.
    bool BlitIn()
    {
        const auto w = static_cast<GLint>(width), h = static_cast<GLint>(height);
        gl.bindFramebuffer(GL_READ_FRAMEBUFFER_, 0);
        if (resolveFbo != 0)
        {
            gl.bindFramebuffer(GL_DRAW_FRAMEBUFFER_, resolveFbo);
            gl.blitFramebuffer(0, 0, w, h, 0, 0, w, h, GL_COLOR_BUFFER_BIT_, GL_NEAREST_);
            gl.bindFramebuffer(GL_READ_FRAMEBUFFER_, resolveFbo);
        }

        gl.bindFramebuffer(GL_DRAW_FRAMEBUFFER_, fbo);
        gl.blitFramebuffer(0, h, w, 0, 0, 0, w, h, GL_COLOR_BUFFER_BIT_, GL_NEAREST_);
        gl.bindFramebuffer(GL_FRAMEBUFFER_, 0);

        // D3D12 reads the texture next; the CPU wait is the hand-over.
        gl.finish();
        return gl.Clean();
    }

    bool BlitOut()
    {
        const auto w = static_cast<GLint>(width), h = static_cast<GLint>(height);
        gl.bindFramebuffer(GL_READ_FRAMEBUFFER_, fbo);
        gl.bindFramebuffer(GL_DRAW_FRAMEBUFFER_, 0);
        gl.blitFramebuffer(0, h, w, 0, 0, 0, w, h, GL_COLOR_BUFFER_BIT_, GL_NEAREST_);
        gl.bindFramebuffer(GL_FRAMEBUFFER_, 0);
        return gl.Clean();
    }
};

Route route;
} // namespace

bool Wanted()
{
    auto config = Config::Instance();
    const bool wanted = config->DlssNrEnabled.value_or_default() && config->DlssNrPresent.value_or_default() &&
                        !route.stopped && DlssNr::Backend::ActiveKindFromConfig() == DlssNr::Backend::Kind::Daniel;

    // Generated frames end with NR; the child window must not keep showing the last one.
    if (!wanted && route.fg.window && (route.fg.enabled || route.fg.visible))
        route.fg.Pause();

    return wanted;
}

void Process(HWND hwnd, uint32_t width, uint32_t height, uint32_t generation)
{
    auto& r = route;
    if (r.stopped)
        return;

    if (r.generation != generation)
    {
        if (r.generation != 0)
            r.ReleaseGl(false);

        r.generation = generation;
        if (!r.gl.Load())
        {
            r.Stop("the OpenGL driver has no EXT_memory_object_win32");
            return;
        }
    }

    if (!r.device && !r.CreateDevice())
    {
        r.Stop("D3D12 device unavailable");
        return;
    }

    if ((r.fbo == 0 || r.width != width || r.height != height) && !r.Build(width, height))
    {
        r.Stop("the back buffer could not cross to D3D12");
        return;
    }

    const auto start = std::chrono::steady_clock::now();
    if (!r.BlitIn())
    {
        r.Stop("the blit out of the back buffer failed");
        return;
    }

    auto settings =
        DlssNr::AmdBridge::SettingsFromConfig(*Config::Instance(), Config::Instance()->AmdNrScale.value_or_default());
    settings.spinDraw = 0;
    if (!AmdPresentExperimental::RenderResource(r.shared.Get(), r.device.Get(), r.queue.Get(),
                                                Util::DllPath().parent_path(), settings, &r.guides))
    {
        r.Stop(AmdPresentExperimental::Status());
        return;
    }

    r.fg.Step(hwnd, r.device.Get(), r.queue.Get(), r.shared.Get(), r.guides, r.fgFailed);

    if (!r.BlitOut())
    {
        r.Stop("the blit back into the back buffer failed");
        return;
    }

    if (!r.loggedFirst)
    {
        r.loggedFirst = true;
        LOG_INFO("OpenGL final-image NR: first frame processed at {}x{}", r.width, r.height);
    }

    r.totalMicros += static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - start).count());
    if (++r.measured == 120)
    {
        LOG_INFO("OpenGL final-image NR: {:.2f} ms average over 120 presents; {}", r.totalMicros / 120000.0,
                 AmdPresentExperimental::Status());
        r.measured = 0;
        r.totalMicros = 0;
    }
}
} // namespace AmdGlPresent
