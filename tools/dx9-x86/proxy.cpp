// 32-bit d3d9.dll for the games OptiScaler.dll (x64 only) cannot load into. It forwards every export
// to the system d3d9.dll and, at each Present of the device's own swap chain, hands the back buffer
// to a 64-bit host (host.cpp, in the OptiScaler64 folder beside this file) that runs OptiScaler on a
// D3D11 swap chain on this game's window. While the host presents, the game's own Present is
// skipped: one presenter owns the window for the whole session, whatever NR, frame generation or
// the menu are doing. A frame the host cannot take is presented natively and the host is started
// again, up to three times; after that the session stays native.
#include <windows.h>
#include <d3d9.h>
#include <d3d9on12.h>
#include <dxgi1_2.h>
#include <psapi.h>
#include <wrl/client.h>
#include <algorithm>
#include <atomic>
#include <bitset>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <string>
#include <vector>
#include "dx9_wire.h"

using Microsoft::WRL::ComPtr;
using namespace dx9wire;

namespace
{
HMODULE self = nullptr, system9 = nullptr;
INIT_ONCE systemOnce = INIT_ONCE_STATIC_INIT;
std::recursive_mutex lock;
std::filesystem::path hostFolder;
ProxyStatus status {};
std::atomic_bool menuOpen { false };

using Create9 = HRESULT(STDMETHODCALLTYPE*)(IDirect3D9*, UINT, D3DDEVTYPE, HWND, DWORD, D3DPRESENT_PARAMETERS*,
                                            IDirect3DDevice9**);
using Create9Ex = HRESULT(STDMETHODCALLTYPE*)(IDirect3D9Ex*, UINT, D3DDEVTYPE, HWND, DWORD, D3DPRESENT_PARAMETERS*,
                                              D3DDISPLAYMODEEX*, IDirect3DDevice9Ex**);
using Present9 = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, const RECT*, const RECT*, HWND, const RGNDATA*);
using Present9Ex = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9Ex*, const RECT*, const RECT*, HWND, const RGNDATA*,
                                               DWORD);
using PresentChain = HRESULT(STDMETHODCALLTYPE*)(IDirect3DSwapChain9*, const RECT*, const RECT*, HWND, const RGNDATA*,
                                                 DWORD);
using Reset9 = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, D3DPRESENT_PARAMETERS*);
using Reset9Ex = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9Ex*, D3DPRESENT_PARAMETERS*, D3DDISPLAYMODEEX*);
Create9 create9 = nullptr;
Create9Ex create9Ex = nullptr;
Present9 present9 = nullptr;
Present9Ex present9Ex = nullptr;
PresentChain presentChain = nullptr;
Reset9 reset9 = nullptr;
Reset9Ex reset9Ex = nullptr;

void Log(const char* format, ...)
{
    char line[1024] {};
    SYSTEMTIME t {};
    GetLocalTime(&t);
    int used = sprintf_s(line, "%02u:%02u:%02u.%03u ", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds);
    va_list args;
    va_start(args, format);
    vsprintf_s(line + used, sizeof(line) - used, format, args);
    va_end(args);
    FILE* file = nullptr;
    if (_wfopen_s(&file, (hostFolder / L"dx9-x86.log").c_str(), L"a") == 0 && file)
    {
        fprintf(file, "%s\n", line);
        fclose(file);
    }
}

BOOL CALLBACK LoadSystem(PINIT_ONCE, PVOID, PVOID*)
{
    wchar_t folder[MAX_PATH] {};
    GetSystemDirectoryW(folder, MAX_PATH);
    system9 = LoadLibraryW((std::wstring(folder) + L"\\d3d9.dll").c_str());
    return TRUE;
}
template <class T> T System(const char* name)
{
    InitOnceExecuteOnce(&systemOnce, LoadSystem, nullptr, nullptr);
    return reinterpret_cast<T>(system9 ? GetProcAddress(system9, name) : nullptr);
}

bool Replace(void** slot, void* replacement, void** original)
{
    DWORD old = 0;
    if (*slot == replacement || !VirtualProtect(slot, sizeof(void*), PAGE_EXECUTE_READWRITE, &old))
        return false;
    *original = InterlockedExchangePointer(reinterpret_cast<PVOID volatile*>(slot), replacement);
    VirtualProtect(slot, sizeof(void*), old, &old);
    return true;
}

// [Dx9X86] in OptiScaler.ini beside the host: true, false, 1 or 0; anything else is the default.
bool Setting(const wchar_t* key, bool fallback)
{
    wchar_t value[16] {};
    GetPrivateProfileStringW(L"Dx9X86", key, L"", value, 16, (hostFolder / L"OptiScaler.ini").c_str());
    if (!_wcsicmp(value, L"true") || !wcscmp(value, L"1"))
        return true;
    if (!_wcsicmp(value, L"false") || !wcscmp(value, L"0"))
        return false;
    return fallback;
}

// The host presents into a child window of the game's window, never into the game's window itself.
// A flip swap chain whose process ends without releasing it leaves its window unable to take another
// one (measured: E_ACCESSDENIED ten seconds after the presenting process was ended), and the game's
// own Present no longer shows there either. A lost host then only costs its child window: it is
// destroyed, the game's Present shows again, and the next host gets a new one. The child is made on
// the game window's thread, which pumps its messages; it takes no input (disabled, and transparent to
// hit testing), so the mouse and keys still reach the game's window. It is asked for and ended with
// posted messages: an engine that renders on its own thread may hold its window thread waiting for
// that very thread, so the Present never waits for the window thread.
const wchar_t* const surfaceClass = L"OptiScalerDx9X86Surface";
UINT surfaceRequest = 0;
std::atomic<HWND> madeSurface { nullptr };
// The game's top-level windows (those of its device window and its focus window).
std::atomic<HWND> gameRoot { nullptr }, focusRoot { nullptr };

// Input the game must not see while OptiScaler's menu is open. The host reads the mouse and keys
// itself; the game's queued input messages and its cursor calls are what is held back here.
POINT heldCursor {};
decltype(&GetCursorPos) realGetCursorPos = GetCursorPos;
decltype(&SetCursorPos) realSetCursorPos = SetCursorPos;
// Keys and mouse buttons (by virtual key) the game saw go down, from window messages and from raw
// input. Their release still reaches the game while the menu is open, so none stays held in the game.
std::bitset<256> gameDown, rawDown;
// Characters typed while the menu is open, passed to the host with the next frame for the menu's text
// fields.
std::mutex typedLock;
std::wstring typed;

bool HeldInput(UINT message)
{
    return (message >= WM_MOUSEFIRST && message <= WM_MOUSELAST) || message == WM_INPUT || message == WM_KEYDOWN ||
           message == WM_KEYUP || message == WM_CHAR || message == WM_DEADCHAR || message == WM_UNICHAR;
}
LRESULT CALLBACK SurfaceProc(HWND window, UINT message, WPARAM w, LPARAM l)
{
    return message == WM_NCHITTEST ? HTTRANSPARENT : DefWindowProcW(window, message, w, l);
}
HWND MakeSurface(HWND parent)
{
    static const ATOM type = []
    {
        WNDCLASSW kind {};
        kind.lpfnWndProc = SurfaceProc;
        kind.hInstance = self;
        kind.lpszClassName = surfaceClass;
        return RegisterClassW(&kind);
    }();
    RECT client {};
    GetClientRect(parent, &client);
    return CreateWindowExW(WS_EX_NOPARENTNOTIFY, MAKEINTATOM(type), L"",
                           WS_CHILD | WS_VISIBLE | WS_DISABLED | WS_CLIPSIBLINGS, 0, 0, client.right, client.bottom,
                           parent, nullptr, self, nullptr);
}

// The virtual key of a key or mouse button message, and whether it presses or releases it; 0 for other
// messages.
int MessageKey(const MSG& msg, bool& down)
{
    down = false;
    switch (msg.message)
    {
    case WM_KEYDOWN:
        down = true;
        [[fallthrough]];
    case WM_KEYUP:
        return static_cast<int>(msg.wParam & 0xFF);
    case WM_LBUTTONDOWN:
    case WM_LBUTTONDBLCLK:
        down = true;
        [[fallthrough]];
    case WM_LBUTTONUP:
        return VK_LBUTTON;
    case WM_RBUTTONDOWN:
    case WM_RBUTTONDBLCLK:
        down = true;
        [[fallthrough]];
    case WM_RBUTTONUP:
        return VK_RBUTTON;
    case WM_MBUTTONDOWN:
    case WM_MBUTTONDBLCLK:
        down = true;
        [[fallthrough]];
    case WM_MBUTTONUP:
        return VK_MBUTTON;
    case WM_XBUTTONDOWN:
    case WM_XBUTTONDBLCLK:
        down = true;
        [[fallthrough]];
    case WM_XBUTTONUP:
        return GET_XBUTTON_WPARAM(msg.wParam) == XBUTTON1 ? VK_XBUTTON1 : VK_XBUTTON2;
    }
    return 0;
}

// Follows the keys and buttons a raw input packet presses and releases; true when it releases one the
// game saw go down.
bool RawRelease(LPARAM handle, bool menu)
{
    RAWINPUT data {};
    UINT size = sizeof(data);
    if (GetRawInputData(reinterpret_cast<HRAWINPUT>(handle), RID_INPUT, &data, &size, sizeof(RAWINPUTHEADER)) ==
        UINT(-1))
        return false;
    bool owed = false;
    const auto follow = [&](UINT vk, bool down, bool up)
    {
        if (up && rawDown[vk])
        {
            rawDown[vk] = false;
            owed = true;
        }
        else if (down && !menu)
            rawDown[vk] = true;
    };
    if (data.header.dwType == RIM_TYPEKEYBOARD && data.data.keyboard.VKey < 256)
    {
        const bool up = (data.data.keyboard.Flags & RI_KEY_BREAK) != 0;
        follow(data.data.keyboard.VKey, !up, up);
    }
    else if (data.header.dwType == RIM_TYPEMOUSE)
    {
        // RI_MOUSE_LEFT_BUTTON_DOWN and _UP, then right, middle, button 4 and button 5, two bits each.
        const UINT buttons[] { VK_LBUTTON, VK_RBUTTON, VK_MBUTTON, VK_XBUTTON1, VK_XBUTTON2 };
        const USHORT flags = data.data.mouse.usButtonFlags;
        for (UINT b = 0; b < 5; ++b)
            follow(buttons[b], (flags & (1u << (2 * b))) != 0, (flags & (2u << (2 * b))) != 0);
    }
    return owed;
}

void Consume(MSG& msg)
{
    msg.message = WM_NULL;
    msg.wParam = 0;
    msg.lParam = 0;
}

// Every message the game window's thread takes from its queue passes here first: the child window
// requests, and the game's input while the menu is open. A message hook sees them whatever window
// procedures the game or an overlay install later.
void Filter(MSG& msg)
{
    if (msg.message == surfaceRequest && surfaceRequest)
    {
        if (msg.wParam)
        {
            if (const HWND unused = madeSurface.exchange(MakeSurface(msg.hwnd)))
                DestroyWindow(unused);
        }
        else if (IsWindow(reinterpret_cast<HWND>(msg.lParam)))
            DestroyWindow(reinterpret_cast<HWND>(msg.lParam));
        Consume(msg);
        return;
    }
    if (!HeldInput(msg.message))
        return;
    // Raw input is the game's whichever of its windows it is registered to.
    const HWND root = msg.hwnd ? GetAncestor(msg.hwnd, GA_ROOT) : nullptr;
    if (msg.message != WM_INPUT && (!root || (root != gameRoot && root != focusRoot)))
        return;
    const bool menu = menuOpen;
    bool down = false, owed = false;
    if (const int vk = MessageKey(msg, down); vk && down && !menu)
        gameDown[vk] = true;
    else if (vk && !down && gameDown[vk])
    {
        gameDown[vk] = false;
        owed = true;
    }
    if (msg.message == WM_INPUT)
        owed = RawRelease(msg.lParam, menu);
    if (!menu || owed)
        return;
    if (msg.message == WM_CHAR || msg.message == WM_UNICHAR)
    {
        std::lock_guard guard(typedLock);
        if (msg.wParam >= 0x20 && msg.wParam < 0xFFFF && typed.size() < 256)
            typed.push_back(static_cast<wchar_t>(msg.wParam));
    }
    // The characters of a held key, for the menu (the game's own TranslateMessage gets WM_NULL).
    else if (msg.message == WM_KEYDOWN)
        TranslateMessage(&msg);
    // The clean-up the game's handling of WM_INPUT would have done.
    else if (msg.message == WM_INPUT && GET_RAWINPUT_CODE_WPARAM(msg.wParam) == RIM_INPUT)
        DefWindowProcW(msg.hwnd, msg.message, msg.wParam, msg.lParam);
    ++status.blockedInput;
    Consume(msg);
}
LRESULT CALLBACK MessageHook(int code, WPARAM removal, LPARAM l)
{
    if (code == HC_ACTION && (removal & PM_REMOVE) && l)
        Filter(*reinterpret_cast<MSG*>(l));
    return CallNextHookEx(nullptr, code, removal, l);
}

BOOL WINAPI HeldGetCursorPos(LPPOINT point)
{
    if (menuOpen && point)
    {
        *point = heldCursor;
        return TRUE;
    }
    return realGetCursorPos(point);
}
BOOL WINAPI HeldSetCursorPos(int x, int y) { return menuOpen ? TRUE : realSetCursorPos(x, y); }

void PatchImports(HMODULE module)
{
    auto base = reinterpret_cast<BYTE*>(module);
    auto dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
    auto nt = reinterpret_cast<IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    const auto& directory = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!directory.VirtualAddress)
        return;
    for (auto entry = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(base + directory.VirtualAddress); entry->Name; ++entry)
    {
        if (_stricmp(reinterpret_cast<char*>(base + entry->Name), "user32.dll") != 0)
            continue;
        for (auto thunk = reinterpret_cast<IMAGE_THUNK_DATA*>(base + entry->FirstThunk); thunk->u1.Function; ++thunk)
        {
            void* replacement = nullptr;
            if (thunk->u1.Function == reinterpret_cast<uintptr_t>(realGetCursorPos))
                replacement = reinterpret_cast<void*>(HeldGetCursorPos);
            else if (thunk->u1.Function == reinterpret_cast<uintptr_t>(realSetCursorPos))
                replacement = reinterpret_cast<void*>(HeldSetCursorPos);
            void* original = nullptr;
            if (replacement)
                Replace(reinterpret_cast<void**>(&thunk->u1.Function), replacement, &original);
        }
    }
}
// Every module loaded so far: run at each host start and each time the menu opens, so modules the
// game loads later are covered too.
void PatchCursorCalls()
{
    HMODULE modules[1024] {};
    DWORD bytes = 0;
    if (K32EnumProcessModules(GetCurrentProcess(), modules, sizeof(modules), &bytes))
        for (DWORD i = 0; i < bytes / sizeof(HMODULE) && i < 1024; ++i)
            if (modules[i] != self)
                PatchImports(modules[i]);
}

// The message hook on the threads of the game's device and focus windows (once per thread), and the
// cursor calls.
void HookGame(HWND window, HWND focus)
{
    static std::vector<DWORD> hooked;
    if (!surfaceRequest)
        surfaceRequest = RegisterWindowMessageW(L"OptiScalerDx9X86Surface");
    gameRoot = GetAncestor(window, GA_ROOT);
    focusRoot = GetAncestor(focus, GA_ROOT);
    for (const HWND target : { window, focus })
    {
        const DWORD thread = GetWindowThreadProcessId(target, nullptr);
        if (!thread || std::find(hooked.begin(), hooked.end(), thread) != hooked.end())
            continue;
        if (SetWindowsHookExW(WH_GETMESSAGE, MessageHook, self, thread))
        {
            hooked.push_back(thread);
            Log("messages of thread %lu hooked: the menu holds the game's input there", thread);
        }
        else
            Log("cannot hook the messages of thread %lu (%lu)", thread, GetLastError());
    }
    PatchCursorCalls();
}

// The window a device presents to, and its focus window.
HWND DeviceWindow(IDirect3DDevice9* game, HWND& focus)
{
    D3DDEVICE_CREATION_PARAMETERS creation {};
    D3DPRESENT_PARAMETERS params {};
    ComPtr<IDirect3DSwapChain9> chain;
    if (FAILED(game->GetCreationParameters(&creation)) || FAILED(game->GetSwapChain(0, &chain)) ||
        FAILED(chain->GetPresentParameters(&params)))
        return nullptr;
    const HWND window = params.hDeviceWindow ? params.hDeviceWindow : creation.hFocusWindow;
    focus = creation.hFocusWindow ? creation.hFocusWindow : window;
    return window;
}

bool AdapterLuid(IDirect3DDevice9* device, LUID& luid)
{
    D3DDEVICE_CREATION_PARAMETERS creation {};
    ComPtr<IDirect3D9> api;
    if (FAILED(device->GetCreationParameters(&creation)) || FAILED(device->GetDirect3D(&api)))
        return false;
    const auto monitor = api->GetAdapterMonitor(creation.AdapterOrdinal);
    ComPtr<IDXGIFactory1> factory;
    if (!monitor || FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))))
        return false;
    ComPtr<IDXGIAdapter1> adapter;
    for (UINT a = 0; factory->EnumAdapters1(a, &adapter) != DXGI_ERROR_NOT_FOUND; ++a)
    {
        ComPtr<IDXGIOutput> output;
        for (UINT o = 0; adapter->EnumOutputs(o, &output) != DXGI_ERROR_NOT_FOUND; ++o)
        {
            DXGI_OUTPUT_DESC desc {};
            DXGI_ADAPTER_DESC1 info {};
            if (SUCCEEDED(output->GetDesc(&desc)) && desc.Monitor == monitor && SUCCEEDED(adapter->GetDesc1(&info)))
            {
                luid = info.AdapterLuid;
                return true;
            }
        }
    }
    return false;
}

struct Host
{
    Handle pipe, process, job;
    // An ended host and its job, until it has left: the next host waits for it (Retire).
    Handle retired, retiredJob;

    // Closing the pipe asks the host to release its swap chain and leave.
    void Stop()
    {
        pipe.reset();
        if (process)
        {
            retired.reset(process.value);
            retiredJob.reset(job.value);
            process.value = job.value = nullptr;
        }
    }
    // A host that has not left within five seconds is ended.
    void Retire()
    {
        if (retired && WaitForSingleObject(retired.value, 5000) == WAIT_TIMEOUT)
            TerminateProcess(retired.value, 7);
        retired.reset();
        retiredJob.reset();
    }

    bool Start(const LUID& luid, HWND window)
    {
        const auto exe = hostFolder / L"OptiScalerDx9Host.exe";
        LARGE_INTEGER ticks {};
        QueryPerformanceCounter(&ticks);
        const auto name = L"\\\\.\\pipe\\opti-dx9-x86-" + std::to_wstring(GetCurrentProcessId()) + L"-" +
                          std::to_wstring(ticks.QuadPart);
        pipe.reset(CreateNamedPipeW(
            name.c_str(), PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED | FILE_FLAG_FIRST_PIPE_INSTANCE,
            PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS, 1, 4096, 4096, 0, nullptr));
        Handle connected(CreateEventW(nullptr, TRUE, FALSE, nullptr));
        if (!pipe || !connected)
            return false;
        OVERLAPPED overlap {};
        overlap.hEvent = connected.value;
        const bool pending = !ConnectNamedPipe(pipe.value, &overlap) && GetLastError() == ERROR_IO_PENDING;
        auto cancel = [&]
        {
            if (pending)
            {
                DWORD n = 0;
                CancelIoEx(pipe.value, &overlap);
                GetOverlappedResult(pipe.value, &overlap, &n, TRUE);
            }
            return false;
        };
        // The host dies with the game: the job closes with this process.
        job.reset(CreateJobObjectW(nullptr, nullptr));
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits {};
        limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        if (!job || !SetInformationJobObject(job.value, JobObjectExtendedLimitInformation, &limits, sizeof(limits)))
            return cancel();
        auto command = L"\"" + exe.wstring() + L"\" \"" + name + L"\" " + std::to_wstring(GetCurrentProcessId());
        STARTUPINFOW startup { sizeof(startup) };
        PROCESS_INFORMATION info {};
        if (!CreateProcessW(exe.c_str(), command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW | CREATE_SUSPENDED,
                            nullptr, hostFolder.c_str(), &startup, &info))
        {
            Log("cannot start %ls (%lu)", exe.c_str(), GetLastError());
            return cancel();
        }
        process.reset(info.hProcess);
        Handle thread(info.hThread);
        if (!AssignProcessToJobObject(job.value, process.value) || ResumeThread(thread.value) == DWORD(-1))
        {
            TerminateProcess(process.value, 1);
            return cancel();
        }
        if (pending)
        {
            HANDLE waits[] { connected.value, process.value };
            DWORD n = 0;
            if (WaitForMultipleObjects(2, waits, FALSE, StartupTimeoutMs) != WAIT_OBJECT_0 ||
                !GetOverlappedResult(pipe.value, &overlap, &n, FALSE))
                return cancel();
        }
        ULONG client = 0;
        if (!GetNamedPipeClientProcessId(pipe.value, &client) || client != info.dwProcessId)
            return false;
        dx9wire::Hello hello;
        hello.pid = GetCurrentProcessId();
        hello.window = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(window));
        hello.luidLow = luid.LowPart;
        hello.luidHigh = luid.HighPart;
        Ack ack;
        return Request(pipe.value, process.value, Kind::Hello, &hello, sizeof(hello), ack, StartupTimeoutMs) &&
               ack.result == Result::Ready;
    }
};

// The copy of the back buffer the host reads: an A8R8G8B8 render target shared by handle, or, when
// the device cannot share one, a render target read back into a section of memory.
struct Stage
{
    ComPtr<IDirect3DTexture9> texture;
    ComPtr<IDirect3DSurface9> surface, readback;
    HANDLE shared = nullptr;
    Handle section;
    void* view = nullptr;
    UINT width = 0, height = 0;
    D3DFORMAT format = D3DFMT_UNKNOWN;

    void Release()
    {
        if (view)
            UnmapViewOfFile(view);
        view = nullptr;
        section.reset();
        readback.Reset();
        surface.Reset();
        texture.Reset();
        shared = nullptr;
        width = height = 0;
        format = D3DFMT_UNKNOWN;
    }
};

struct Bridge
{
    Host host;
    Stage stage;
    IDirect3DDevice9* device = nullptr;
    HWND window = nullptr;
    UINT syncInterval = 1;
    uint64_t generation = 0, frame = 0;
    bool built = false, disabled = false, pipelined = true;
    // A frame posted to the host whose answer is collected at the next Present (or before anything
    // else uses the pipe), so the game renders its next frame while the host presents this one.
    uint64_t pending = 0;
    // The host starts on its own thread while the game presents natively, then takes the window.
    enum class Link
    {
        Down,
        Starting,
        Up
    } link = Link::Down;
    Handle starter;
    LUID startLuid {};
    HWND focus = nullptr;
    // This host's child window, and when one was last asked of the game window's thread.
    HWND surface = nullptr;
    bool surfaceAsked = false;
    ULONGLONG surfaceAskedAt = 0;
    // Failures in a row. A host that presented 600 frames before it failed starts the count again, so
    // only a host that keeps failing early leaves the session native.
    uint32_t strikes = 0;
    uint64_t sinceStart = 0;
    ULONGLONG retryAt = 0;

    // Ends the link: closing the pipe asks the host to leave, and its child window is destroyed.
    void Down()
    {
        host.Stop();
        if (surface && IsWindow(surface))
            PostMessageW(GetAncestor(surface, GA_PARENT), surfaceRequest, 0, LPARAM(surface));
        surface = nullptr;
        surfaceAsked = false;
        link = Link::Down;
        built = false;
        pending = 0;
        menuOpen = false;
    }

    void Fault(const char* why, bool retryNow = false)
    {
        Log("host lost (%s); this frame is presented natively", why);
        Down();
        ++status.faults;
        strikes = sinceStart >= 600 ? 1 : strikes + 1;
        sinceStart = 0;
        if (strikes > 3)
        {
            disabled = true;
            status.disabled = 1;
            Log("host failed %u times in a row; the game presents natively for the rest of the session", strikes);
        }
        retryAt = GetTickCount64() + (retryNow ? 0 : 1000ull * strikes);
    }

    // The game presents to a new window: a new host for it, straight away. Not a failure.
    void Restart(const char* why)
    {
        Log("host %u ended: %s; a new host starts", status.hostStarts, why);
        Down();
        retryAt = 0;
    }

    void Drop()
    {
        Answer();
        if (built)
        {
            Ack ack;
            if (!Request(host.pipe.value, host.process.value, Kind::Drop, nullptr, 0, ack))
                Fault("no answer to a drop");
        }
        built = false;
        stage.Release();
    }

    bool Connect(IDirect3DDevice9* game)
    {
        LUID luid {};
        if (!(window = DeviceWindow(game, focus)) || !AdapterLuid(game, luid))
            return false;
        HookGame(window, focus);
        startLuid = luid;
        ++status.hostStarts;
        starter.reset(CreateThread(
            nullptr, 0,
            [](LPVOID context) -> DWORD
            {
                auto bridge = static_cast<Bridge*>(context);
                bridge->host.Retire();
                return bridge->host.Start(bridge->startLuid, bridge->window) ? 1 : 0;
            },
            this, 0, nullptr));
        return static_cast<bool>(starter);
    }

    // Up once the host answered its Hello; until then the game presents natively.
    bool Started()
    {
        if (link == Link::Starting && WaitForSingleObject(starter.value, 0) == WAIT_OBJECT_0)
        {
            DWORD started = 0;
            GetExitCodeThread(starter.value, &started);
            starter.reset();
            if (!started)
            {
                Fault("start");
                return false;
            }
            link = Link::Up;
            sinceStart = 0;
            pipelined = Setting(L"Pipelined", true);
            Log("host %u connected for window %p", status.hostStarts, window);
        }
        return link == Link::Up;
    }

    // The child window comes from the game window's thread; until it has made one, the game presents
    // natively. Asked again every 3 s while none has come.
    bool Surface()
    {
        if (!surface)
        {
            const HWND made = madeSurface.exchange(nullptr);
            if (made && GetAncestor(made, GA_PARENT) == window)
                surface = made;
            else if (made && IsWindow(made))
                PostMessageW(GetAncestor(made, GA_PARENT), surfaceRequest, 0, LPARAM(made));
        }
        if (!surface)
        {
            const ULONGLONG now = GetTickCount64();
            if (surfaceAsked && now - surfaceAskedAt < 3000)
                return false;
            if (surfaceAsked)
                Log("no child window from the game window's thread after 3 s; asking again");
            surfaceAsked = PostMessageW(window, surfaceRequest, 1, 0) != FALSE;
            surfaceAskedAt = now;
            return false;
        }
        surfaceAsked = false;
        // The child follows the game window's client area (not while it is minimized).
        RECT client {}, child {};
        if (!IsIconic(window) && GetClientRect(window, &client) && GetClientRect(surface, &child) &&
            (client.right != child.right || client.bottom != child.bottom))
            SetWindowPos(surface, nullptr, 0, 0, client.right, client.bottom,
                         SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOMOVE | SWP_ASYNCWINDOWPOS);
        return true;
    }

    bool Ensure(IDirect3DDevice9* game, const D3DSURFACE_DESC& desc)
    {
        if (device != game)
        {
            Drop();
            if (link != Link::Up)
                return false;
            device = game;
        }
        if (built && stage.width == desc.Width && stage.height == desc.Height && stage.format == desc.Format)
            return true;
        Drop();
        if (link != Link::Up)
            return false;
        if (stage.width != desc.Width || stage.height != desc.Height || stage.format != desc.Format || !stage.texture)
        {
            stage.Release();
            const bool cpu = Setting(L"SharedMemory", false);
            if (cpu ||
                FAILED(game->CreateTexture(desc.Width, desc.Height, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A8R8G8B8,
                                           D3DPOOL_DEFAULT, &stage.texture, &stage.shared)) ||
                !stage.shared)
            {
                stage.texture.Reset();
                stage.shared = nullptr;
                const DWORD bytes = desc.Width * desc.Height * 4;
                if (FAILED(game->CreateTexture(desc.Width, desc.Height, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A8R8G8B8,
                                               D3DPOOL_DEFAULT, &stage.texture, nullptr)) ||
                    FAILED(game->CreateOffscreenPlainSurface(desc.Width, desc.Height, D3DFMT_A8R8G8B8,
                                                             D3DPOOL_SYSTEMMEM, &stage.readback, nullptr)))
                    return false;
                stage.section.reset(
                    CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, bytes, nullptr));
                stage.view = stage.section ? MapViewOfFile(stage.section.value, FILE_MAP_WRITE, 0, 0, bytes) : nullptr;
                if (!stage.view)
                    return false;
            }
            if (FAILED(stage.texture->GetSurfaceLevel(0, &stage.surface)))
                return false;
            stage.width = desc.Width;
            stage.height = desc.Height;
            stage.format = desc.Format;
        }
        dx9wire::Build request;
        request.generation = ++generation;
        request.window = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(surface));
        request.width = stage.width;
        request.height = stage.height;
        request.format = DXGI_FORMAT_B8G8R8A8_UNORM;
        request.pitch = stage.width * 4;
        if (stage.shared)
        {
            request.transport = Transport::SharedTexture;
            request.handle = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(stage.shared));
        }
        else
        {
            HANDLE remote = nullptr;
            if (!DuplicateHandle(GetCurrentProcess(), stage.section.value, host.process.value, &remote, 0, FALSE,
                                 DUPLICATE_SAME_ACCESS))
                return false;
            request.transport = Transport::SharedMemory;
            request.handle = reinterpret_cast<uintptr_t>(remote);
        }
        status.transport = static_cast<uint32_t>(request.transport);
        status.width = stage.width;
        status.height = stage.height;
        Ack ack;
        // The host makes its swap chain here, and OptiScaler with it the frame generation swap chain.
        if (!Request(host.pipe.value, host.process.value, Kind::Build, &request, sizeof(request), ack, AnswerTimeoutMs))
        {
            Fault("no answer to a build");
            return false;
        }
        if (ack.result == Result::Unhooked)
        {
            ++status.unhooked;
            Fault("OptiScaler did not hook the host's swap chain");
            return false;
        }
        built = ack.result == Result::Ready && ack.generation == generation;
        Log("stage %llu: %ux%u D3D9 format %u through %s: %s", generation, stage.width, stage.height,
            static_cast<unsigned>(desc.Format), stage.shared ? "a shared texture" : "shared memory",
            built ? "ready" : "refused");
        return built;
    }

    bool Wait9(IDirect3DDevice9* game)
    {
        ComPtr<IDirect3DQuery9> query;
        if (FAILED(game->CreateQuery(D3DQUERYTYPE_EVENT, &query)) || FAILED(query->Issue(D3DISSUE_END)))
            return false;
        const ULONGLONG deadline = GetTickCount64() + GpuWaitMs;
        HRESULT done = S_FALSE;
        while ((done = query->GetData(nullptr, 0, D3DGETDATA_FLUSH)) == S_FALSE && GetTickCount64() < deadline)
            YieldProcessor();
        return done == S_OK;
    }

    bool Capture(IDirect3DDevice9* game, IDirect3DSurface9* back)
    {
        if (FAILED(game->StretchRect(back, nullptr, stage.surface.Get(), nullptr, D3DTEXF_NONE)))
            return false;
        if (stage.shared)
            return Wait9(game);
        D3DLOCKED_RECT from {};
        if (FAILED(game->GetRenderTargetData(stage.surface.Get(), stage.readback.Get())) ||
            FAILED(stage.readback->LockRect(&from, nullptr, D3DLOCK_READONLY)))
            return false;
        const UINT row = stage.width * 4;
        for (UINT y = 0; y < stage.height; ++y)
            std::memcpy(static_cast<BYTE*>(stage.view) + size_t(y) * row,
                        static_cast<const BYTE*>(from.pBits) + size_t(y) * from.Pitch, row);
        stage.readback->UnlockRect();
        return true;
    }

    // True when the host presented this frame and the game's Present must be skipped.
    bool Present(IDirect3DDevice9* game, IDirect3DSurface9* back)
    {
        if (disabled || !back)
            return false;
        D3DSURFACE_DESC desc {};
        if (FAILED(back->GetDesc(&desc)))
            return false;
        // A provisional buffer (The Sims 4 shows 16x16 first). Once the host owns the window its last
        // frame stays up rather than D3D9 presenting under it.
        if (desc.Width < 64 || desc.Height < 64)
            return link == Link::Up;
        if (link == Link::Down)
        {
            if (GetTickCount64() < retryAt)
                return false;
            if (!Connect(game))
            {
                Fault("start");
                return false;
            }
            link = Link::Starting;
        }
        if (!Started() || !Answer())
            return false;
        // A game that releases its window and device and makes new ones: the host's swap chain is on a
        // child of the old window, so the new window gets a new host.
        HWND focusNow = nullptr;
        if (const HWND now = DeviceWindow(game, focusNow); now && now != window)
        {
            Restart("the game presents to a new window");
            return false;
        }
        if (surface && !IsWindow(surface))
        {
            Restart("its child window is gone");
            return false;
        }
        if (!Surface())
            return false;
        if (!Ensure(game, desc))
        {
            // A host that cannot resize its swap chain (XeFG refuses while anything holds a back
            // buffer) is replaced by a new one at the new size straight away.
            if (link == Link::Up)
                Fault("the host could not take the new back buffer", true);
            return false;
        }
        if (!Capture(game, back))
        {
            Fault("the frame could not be handed over");
            return false;
        }
        dx9wire::Frame request;
        request.generation = generation;
        request.id = ++frame;
        request.syncInterval = syncInterval;
        {
            std::lock_guard guard(typedLock);
            request.textCount = static_cast<uint32_t>(std::min<size_t>(typed.size(), std::size(request.text)));
            std::copy_n(typed.begin(), request.textCount, request.text);
            typed.erase(0, request.textCount);
        }
        status.typed += request.textCount;
        if (!Post(host.pipe.value, host.process.value, Kind::Frame, &request, sizeof(request)))
        {
            Fault("the host stopped reading");
            return false;
        }
        pending = frame;
        return pipelined || Answer();
    }

    // Collects the answer to the frame in flight; false when the host failed it.
    bool Answer()
    {
        if (!pending)
            return true;
        const uint64_t id = pending;
        pending = 0;
        Ack ack;
        if (!Collect(host.pipe.value, host.process.value, Kind::Frame, ack, AnswerTimeoutMs))
        {
            Fault("no answer to a frame");
            return false;
        }
        if (ack.result == Result::DeviceLost)
        {
            Log("host reports its device lost (reason %08X)", ack.error);
            Fault("device lost");
            return false;
        }
        if (ack.result != Result::Presented || ack.frame != id)
        {
            Fault("the host could not present");
            return false;
        }
        status.state = ack.state;
        if ((ack.state & StateMenu) && !menuOpen)
        {
            realGetCursorPos(&heldCursor);
            PatchCursorCalls();
        }
        menuOpen = (ack.state & StateMenu) != 0;
        status.menuFrames += menuOpen ? 1 : 0;
        ++sinceStart;
        if (++status.bridged == 1)
            Log("first frame presented by the host");
        return true;
    }
};
// Never destroyed: releasing D3D9 objects from DllMain at process exit deadlocks D3D9Ex.
Bridge& bridge = *new Bridge;

thread_local bool inPresent = false;

// Runs the bridge for one Present of the device's own swap chain to its own window; false means
// present natively.
bool Bridged(IDirect3DDevice9* game, IDirect3DSwapChain9* chain, HWND target)
{
    if (inPresent)
        return false;
    std::lock_guard guard(lock);
    ComPtr<IDirect3DSwapChain9> implicit;
    D3DPRESENT_PARAMETERS params {};
    if (FAILED(game->GetSwapChain(0, &implicit)) || (chain && implicit.Get() != chain) ||
        (target && SUCCEEDED(implicit->GetPresentParameters(&params)) && params.hDeviceWindow &&
         target != params.hDeviceWindow))
        return false;
    ComPtr<IDirect3DSurface9> back;
    if (FAILED(game->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &back)))
        return false;
    inPresent = true;
    const bool done = bridge.Present(game, back.Get());
    inPresent = false;
    if (!done)
        ++status.native;
    return done;
}

HRESULT STDMETHODCALLTYPE HookPresent(IDirect3DDevice9* device, const RECT* source, const RECT* dest, HWND window,
                                      const RGNDATA* dirty)
{
    if (Bridged(device, nullptr, window))
        return S_OK;
    const bool outer = !inPresent;
    inPresent = true;
    const HRESULT result = present9(device, source, dest, window, dirty);
    inPresent = !outer;
    return result;
}
HRESULT STDMETHODCALLTYPE HookPresentEx(IDirect3DDevice9Ex* device, const RECT* source, const RECT* dest, HWND window,
                                        const RGNDATA* dirty, DWORD flags)
{
    if (!(flags & D3DPRESENT_DONOTFLIP) && Bridged(device, nullptr, window))
        return S_OK;
    const bool outer = !inPresent;
    inPresent = true;
    const HRESULT result = present9Ex(device, source, dest, window, dirty, flags);
    inPresent = !outer;
    return result;
}
HRESULT STDMETHODCALLTYPE HookChain(IDirect3DSwapChain9* chain, const RECT* source, const RECT* dest, HWND window,
                                    const RGNDATA* dirty, DWORD flags)
{
    ComPtr<IDirect3DDevice9> device;
    if (!inPresent && SUCCEEDED(chain->GetDevice(&device)) && Bridged(device.Get(), chain, window))
        return S_OK;
    const bool outer = !inPresent;
    inPresent = true;
    const HRESULT result = presentChain(chain, source, dest, window, dirty, flags);
    inPresent = !outer;
    return result;
}

// A fullscreen D3D9 device owns the display, and the host's swap chain on its window would never
// be seen; Windowed=1 keeps the game's own window at its size. [Dx9X86] ForceWindowed=0 opts out.
void Prepare(D3DPRESENT_PARAMETERS* params)
{
    if (!params)
        return;
    if (!params->Windowed && Setting(L"ForceWindowed", true))
    {
        params->Windowed = TRUE;
        params->FullScreen_RefreshRateInHz = 0;
        Log("fullscreen %ux%u requested; running windowed so the host's swap chain is visible", params->BackBufferWidth,
            params->BackBufferHeight);
    }
    bridge.syncInterval = params->PresentationInterval == D3DPRESENT_INTERVAL_IMMEDIATE ? 0
                          : params->PresentationInterval == D3DPRESENT_INTERVAL_TWO     ? 2
                          : params->PresentationInterval == D3DPRESENT_INTERVAL_THREE   ? 3
                          : params->PresentationInterval == D3DPRESENT_INTERVAL_FOUR    ? 4
                                                                                        : 1;
}
HRESULT STDMETHODCALLTYPE HookReset(IDirect3DDevice9* device, D3DPRESENT_PARAMETERS* params)
{
    {
        std::lock_guard guard(lock);
        bridge.Drop();
        Prepare(params);
    }
    return reset9(device, params);
}
HRESULT STDMETHODCALLTYPE HookResetEx(IDirect3DDevice9Ex* device, D3DPRESENT_PARAMETERS* params, D3DDISPLAYMODEEX* mode)
{
    {
        std::lock_guard guard(lock);
        bridge.Drop();
        Prepare(params);
    }
    return reset9Ex(device, params, params && params->Windowed ? nullptr : mode);
}

void HookDevice(IDirect3DDevice9* device)
{
    std::lock_guard guard(lock);
    auto table = *reinterpret_cast<void***>(device);
    Replace(&table[17], reinterpret_cast<void*>(HookPresent), reinterpret_cast<void**>(&present9));
    Replace(&table[16], reinterpret_cast<void*>(HookReset), reinterpret_cast<void**>(&reset9));
    ComPtr<IDirect3DSwapChain9> chain;
    if (SUCCEEDED(device->GetSwapChain(0, &chain)))
        Replace(&(*reinterpret_cast<void***>(chain.Get()))[3], reinterpret_cast<void*>(HookChain),
                reinterpret_cast<void**>(&presentChain));
    ComPtr<IDirect3DDevice9Ex> ex;
    if (SUCCEEDED(device->QueryInterface(IID_PPV_ARGS(&ex))))
    {
        auto entries = *reinterpret_cast<void***>(ex.Get());
        Replace(&entries[121], reinterpret_cast<void*>(HookPresentEx), reinterpret_cast<void**>(&present9Ex));
        Replace(&entries[132], reinterpret_cast<void*>(HookResetEx), reinterpret_cast<void**>(&reset9Ex));
    }
}
HRESULT STDMETHODCALLTYPE HookCreate(IDirect3D9* api, UINT adapter, D3DDEVTYPE type, HWND window, DWORD flags,
                                     D3DPRESENT_PARAMETERS* params, IDirect3DDevice9** out)
{
    Prepare(params);
    const auto result = create9(api, adapter, type, window, flags, params, out);
    if (SUCCEEDED(result) && out && *out)
        HookDevice(*out);
    return result;
}
HRESULT STDMETHODCALLTYPE HookCreateEx(IDirect3D9Ex* api, UINT adapter, D3DDEVTYPE type, HWND window, DWORD flags,
                                       D3DPRESENT_PARAMETERS* params, D3DDISPLAYMODEEX* mode, IDirect3DDevice9Ex** out)
{
    Prepare(params);
    const auto result =
        create9Ex(api, adapter, type, window, flags, params, params && params->Windowed ? nullptr : mode, out);
    if (SUCCEEDED(result) && out && *out)
        HookDevice(*out);
    return result;
}
void HookApi(IDirect3D9* api)
{
    std::lock_guard guard(lock);
    if (!Setting(L"Enabled", true))
        return;
    auto table = *reinterpret_cast<void***>(api);
    Replace(&table[16], reinterpret_cast<void*>(HookCreate), reinterpret_cast<void**>(&create9));
    ComPtr<IDirect3D9Ex> ex;
    if (SUCCEEDED(api->QueryInterface(IID_PPV_ARGS(&ex))))
        Replace(&(*reinterpret_cast<void***>(ex.Get()))[20], reinterpret_cast<void*>(HookCreateEx),
                reinterpret_cast<void**>(&create9Ex));
}
} // namespace

extern "C" BOOL WINAPI DllMain(HINSTANCE module, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH)
    {
        self = module;
        DisableThreadLibraryCalls(module);
        wchar_t path[MAX_PATH] {};
        GetModuleFileNameW(module, path, MAX_PATH);
        hostFolder = std::filesystem::path(path).parent_path() / L"OptiScaler64";
    }
    return TRUE;
}

extern "C" void WINAPI _OptiScalerX86Status(ProxyStatus* out)
{
    std::lock_guard guard(lock);
    if (out)
        *out = status;
}
extern "C" IDirect3D9* WINAPI _Direct3DCreate9(UINT version)
{
    auto fn = System<decltype(&Direct3DCreate9)>("Direct3DCreate9");
    auto api = fn ? fn(version) : nullptr;
    if (api)
        HookApi(api);
    return api;
}
extern "C" HRESULT WINAPI _Direct3DCreate9Ex(UINT version, IDirect3D9Ex** out)
{
    auto fn = System<decltype(&Direct3DCreate9Ex)>("Direct3DCreate9Ex");
    const auto result = fn ? fn(version, out) : E_FAIL;
    if (SUCCEEDED(result) && out && *out)
        HookApi(*out);
    return result;
}
extern "C" IDirect3D9* WINAPI _Direct3DCreate9On12(UINT version, D3D9ON12_ARGS* args, UINT count)
{
    auto fn = System<decltype(&Direct3DCreate9On12)>("Direct3DCreate9On12");
    auto api = fn ? fn(version, args, count) : nullptr;
    if (api)
        HookApi(api);
    return api;
}
extern "C" HRESULT WINAPI _Direct3DCreate9On12Ex(UINT version, D3D9ON12_ARGS* args, UINT count, IDirect3D9Ex** out)
{
    auto fn = System<decltype(&Direct3DCreate9On12Ex)>("Direct3DCreate9On12Ex");
    const auto result = fn ? fn(version, args, count, out) : E_FAIL;
    if (SUCCEEDED(result) && out && *out)
        HookApi(*out);
    return result;
}
#define FORWARD(returnType, name, args, call, fallback)                                                                \
    extern "C" returnType WINAPI _##name args                                                                          \
    {                                                                                                                  \
        auto fn = System<decltype(&name)>(#name);                                                                      \
        return fn ? fn call : fallback;                                                                                \
    }
#define FORWARD_VOID(name, args, call)                                                                                 \
    extern "C" void WINAPI _##name args                                                                                \
    {                                                                                                                  \
        auto fn = System<decltype(&name)>(#name);                                                                      \
        if (fn)                                                                                                        \
            fn call;                                                                                                   \
    }
FORWARD(int, D3DPERF_BeginEvent, (D3DCOLOR colour, LPCWSTR name), (colour, name), 0)
FORWARD(int, D3DPERF_EndEvent, (), (), 0)
FORWARD(DWORD, D3DPERF_GetStatus, (), (), 0)
FORWARD(BOOL, D3DPERF_QueryRepeatFrame, (), (), FALSE)
FORWARD_VOID(D3DPERF_SetMarker, (D3DCOLOR colour, LPCWSTR name), (colour, name))
FORWARD_VOID(D3DPERF_SetOptions, (DWORD options), (options))
FORWARD_VOID(D3DPERF_SetRegion, (D3DCOLOR colour, LPCWSTR name), (colour, name))
