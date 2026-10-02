#include "pch.h"

#include "GL_Hooks.h"

#include <Config.h>
#include <Logger.h>
#include <State.h>

#include <menu/menu_overlay_gl.h>
#include <dlssnr/amd/GLPresentExperimental.h>
#include <misc/FrameLimit.h>

#include <detours/detours.h>

#include "Hook_Utils.h"

typedef BOOL(WINAPI* PFN_wglSwapBuffers)(HDC);
typedef BOOL(WINAPI* PFN_wglSwapLayerBuffers)(HDC, UINT);
typedef HGLRC(WINAPI* PFN_wglCreateContext)(HDC);
typedef BOOL(WINAPI* PFN_wglMakeCurrent)(HDC, HGLRC);
typedef HGLRC(WINAPI* PFN_wglGetCurrentContext)();
typedef HDC(WINAPI* PFN_wglGetCurrentDC)();
typedef PROC(WINAPI* PFN_wglGetProcAddress)(LPCSTR);
typedef BOOL(WINAPI* PFN_wglSwapIntervalEXT)(int);
typedef int(WINAPI* PFN_wglGetSwapIntervalEXT)();

static HMODULE _module = nullptr;
static PFN_wglSwapBuffers o_wglSwapBuffers = nullptr;
static PFN_wglSwapLayerBuffers o_wglSwapLayerBuffers = nullptr;
static PFN_wglCreateContext o_wglCreateContext = nullptr;
static PFN_wglMakeCurrent o_wglMakeCurrent = nullptr;
static PFN_wglGetCurrentContext o_wglGetCurrentContext = nullptr;
static PFN_wglGetCurrentDC o_wglGetCurrentDC = nullptr;
static PFN_wglGetProcAddress o_wglGetProcAddress = nullptr;

// OptiScaler draws in a context of its own, created on the game's device context. The default
// framebuffer belongs to the window, so both contexts see the same back buffer, while the game's
// bindings and state stay untouched and nothing OptiScaler creates dies with a context the game
// deletes. A device context with another pixel format gets a new context; the old one is left
// alive, and generation tells the menu and the NR route to rebuild their objects.
struct OverlayContext
{
    HGLRC context = nullptr;
    HGLRC gameContext = nullptr;
    HDC gameDc = nullptr;
    uint32_t generation = 0;
    bool current = false;
    bool failed = false;

    bool Enter(HDC hdc)
    {
        if (current)
            return true;

        if (failed)
            return false;

        gameContext = o_wglGetCurrentContext();
        gameDc = o_wglGetCurrentDC();

        if (context == nullptr || !o_wglMakeCurrent(hdc, context))
        {
            auto created = o_wglCreateContext(hdc);
            if (created == nullptr || !o_wglMakeCurrent(hdc, created))
            {
                // Without a context of its own OptiScaler stays out of OpenGL for the session.
                LOG_ERROR("OpenGL: OptiScaler context unavailable ({}); menu and NR are off", GetLastError());
                o_wglMakeCurrent(gameDc, gameContext);
                failed = true;
                return false;
            }

            context = created;
            generation++;
            LOG_INFO("OpenGL: OptiScaler context {:X} created on the game's device context", (size_t) context);
        }

        current = true;
        return true;
    }

    void Leave()
    {
        if (!current)
            return;

        o_wglMakeCurrent(gameDc, gameContext);
        current = false;
    }
};

static OverlayContext _overlay;
static thread_local bool _inPresent = false;

static void Present(HDC hdc)
{
    if (_inPresent)
        return;

    HWND hwnd = WindowFromDC(hdc);
    RECT rect {};
    if (hwnd == nullptr || IsIconic(hwnd) || !GetClientRect(hwnd, &rect) || rect.right <= 0 || rect.bottom <= 0)
        return;

    _inPresent = true;
    State::Instance().swapchainApi = OpenGL;

    const auto width = static_cast<uint32_t>(rect.right);
    const auto height = static_cast<uint32_t>(rect.bottom);
    const bool nr = AmdGlPresent::Wanted();
    const bool setup = MenuOverlayGl::NeedsContext(hwnd);

    if ((nr || setup) && _overlay.Enter(hdc))
    {
        if (nr)
            AmdGlPresent::Process(hwnd, width, height, _overlay.generation);

        if (setup)
            MenuOverlayGl::Setup(hwnd, _overlay.generation);
    }

    // The menu frame is built on the CPU; the overlay context is needed only to draw it.
    if (MenuOverlayGl::Frame() && _overlay.Enter(hdc))
        MenuOverlayGl::Draw(_overlay.generation);

    _overlay.Leave();
    _inPresent = false;
}

// The menu's V-Sync settings, as on DXGI: off is interval 0, on is the chosen interval, at least 1.
// The swap interval belongs to the game's context, which is current here.
static void ApplyVsync(HDC hdc)
{
    auto config = Config::Instance();
    if (!config->ForceVsync.has_value() || o_wglGetCurrentDC() != hdc)
        return;

    static auto setInterval = (PFN_wglSwapIntervalEXT) GLHooks::GetProc("wglSwapIntervalEXT");
    static auto getInterval = (PFN_wglGetSwapIntervalEXT) GLHooks::GetProc("wglGetSwapIntervalEXT");
    if (setInterval == nullptr || getInterval == nullptr)
        return;

    const int interval =
        config->ForceVsync.value() ? std::max(1, static_cast<int>(config->VsyncInterval.value_or_default())) : 0;
    static int logged = -1;
    if (getInterval() != interval && setInterval(interval) && logged != interval)
    {
        logged = interval;
        LOG_INFO("OpenGL: swap interval set to {}", interval);
    }
}

static void AfterPresent()
{
    if (!State::Instance().reflexLimitsFps)
        FrameLimit::sleep(false);
}

VALIDATE_HOOK(hkwglSwapBuffers, PFN_wglSwapBuffers)
static BOOL WINAPI hkwglSwapBuffers(HDC hdc)
{
    Present(hdc);
    ApplyVsync(hdc);
    auto result = o_wglSwapBuffers(hdc);
    AfterPresent();
    return result;
}

VALIDATE_HOOK(hkwglSwapLayerBuffers, PFN_wglSwapLayerBuffers)
static BOOL WINAPI hkwglSwapLayerBuffers(HDC hdc, UINT planes)
{
    if (planes & WGL_SWAP_MAIN_PLANE)
    {
        Present(hdc);
        ApplyVsync(hdc);
    }

    auto result = o_wglSwapLayerBuffers(hdc, planes);

    if (planes & WGL_SWAP_MAIN_PLANE)
        AfterPresent();

    return result;
}

PROC GLHooks::GetProc(const char* name)
{
    PROC proc = o_wglGetProcAddress != nullptr ? o_wglGetProcAddress(name) : nullptr;

    // wglGetProcAddress may answer 1, 2, 3 or -1 for functions it does not know
    const auto value = reinterpret_cast<intptr_t>(proc);
    if (value >= -1 && value <= 3)
        proc = _module != nullptr ? GetProcAddress(_module, name) : nullptr;

    return proc;
}

void GLHooks::Hook(HMODULE opengl32)
{
    if (o_wglSwapBuffers != nullptr)
        return;

    _module = opengl32;
    o_wglSwapBuffers = (PFN_wglSwapBuffers) GetProcAddress(opengl32, "wglSwapBuffers");
    o_wglSwapLayerBuffers = (PFN_wglSwapLayerBuffers) GetProcAddress(opengl32, "wglSwapLayerBuffers");
    o_wglCreateContext = (PFN_wglCreateContext) GetProcAddress(opengl32, "wglCreateContext");
    o_wglMakeCurrent = (PFN_wglMakeCurrent) GetProcAddress(opengl32, "wglMakeCurrent");
    o_wglGetCurrentContext = (PFN_wglGetCurrentContext) GetProcAddress(opengl32, "wglGetCurrentContext");
    o_wglGetCurrentDC = (PFN_wglGetCurrentDC) GetProcAddress(opengl32, "wglGetCurrentDC");
    o_wglGetProcAddress = (PFN_wglGetProcAddress) GetProcAddress(opengl32, "wglGetProcAddress");

    if (o_wglSwapBuffers == nullptr || o_wglCreateContext == nullptr || o_wglMakeCurrent == nullptr ||
        o_wglGetCurrentContext == nullptr || o_wglGetCurrentDC == nullptr || o_wglGetProcAddress == nullptr)
    {
        LOG_ERROR("OpenGL: opengl32.dll is missing wgl entry points");
        o_wglSwapBuffers = nullptr;
        return;
    }

    DetourTransactionBegin();
    DetourUpdateThread(GetCurrentThread());

    DetourAttach(&(PVOID&) o_wglSwapBuffers, hkwglSwapBuffers);

    if (o_wglSwapLayerBuffers != nullptr)
        DetourAttach(&(PVOID&) o_wglSwapLayerBuffers, hkwglSwapLayerBuffers);

    auto result = DetourTransactionCommit();
    if (result != NO_ERROR)
        LOG_ERROR("OpenGL: hooking wglSwapBuffers failed ({:X})", result);
    else
        LOG_INFO("OpenGL: wglSwapBuffers hooked");
}
