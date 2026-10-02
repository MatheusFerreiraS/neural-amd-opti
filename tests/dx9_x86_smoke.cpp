// 32-bit D3D9 program for tools\test-dx9-x86.cmd. It loads the d3d9.dll beside it (the proxy in
// tools\dx9-x86) or, with --native, the system one, renders a moving scene into a real window and
// presents it, and checks what the proxy reports through OptiScalerX86Status: frames the 64-bit
// host presented, host restarts, OptiScaler's menu state and the input held from the game.
//   --ex                 Direct3DCreate9Ex and a D3D9Ex device
//   --frames N           frames to present (default 240)
//   --size WxH           back buffer and client size (default 1280x720)
//   --vsync              present interval one (default immediate)
//   --native             load the system d3d9.dll
//   --expect-bridged     every frame but the first few must be presented by the host
//   --expect-fg          OptiScaler must report frame generation active at the end
//   --menu-at F          press the menu key (Insert) at F, move the mouse over the window, press it again
//   --key-at F VK        press and release a key (hex or decimal virtual key) at frame F
//   --reset-at F WxH     Reset the device to a new size at frame F
//   --kill-host-at F     end the host process at frame F; the proxy must start another
//   --screen             fail when the window's pixels on screen stop changing
//   --timeout S          give up after S seconds (default 120)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <d3d9.h>
#include <tlhelp32.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include "../tools/dx9-x86/dx9_wire.h"

namespace
{
long mouseMessages = 0, keyMessages = 0;

LRESULT CALLBACK Proc(HWND window, UINT message, WPARAM w, LPARAM l)
{
    if (message >= WM_MOUSEFIRST && message <= WM_MOUSELAST)
        ++mouseMessages;
    if (message == WM_KEYDOWN || message == WM_KEYUP || message == WM_CHAR)
        ++keyMessages;
    if (message == WM_CLOSE)
        return 0;
    return DefWindowProcW(window, message, w, l);
}

int Fail(const char* what)
{
    printf("FAIL %s\n", what);
    fflush(stdout);
    return 1;
}

bool Foreground(HWND window)
{
    const HWND current = GetForegroundWindow();
    if (current == window)
        return true;
    if (!current)
        return false;
    const DWORD other = GetWindowThreadProcessId(current, nullptr), me = GetCurrentThreadId();
    AttachThreadInput(me, other, TRUE);
    SetForegroundWindow(window);
    BringWindowToTop(window);
    SetFocus(window);
    AttachThreadInput(me, other, FALSE);
    return GetForegroundWindow() == window;
}

// Keys go out only while this window is in front, so nothing reaches another program.
bool Key(HWND window, WORD vk, bool up)
{
    if (GetForegroundWindow() != window)
        return false;
    INPUT input {};
    input.type = INPUT_KEYBOARD;
    input.ki.wVk = vk;
    input.ki.wScan = static_cast<WORD>(MapVirtualKeyW(vk, MAPVK_VK_TO_VSC));
    input.ki.dwFlags =
        (up ? KEYEVENTF_KEYUP : 0) |
        (vk == VK_INSERT || vk == VK_END || vk == VK_HOME || vk == VK_PRIOR || vk == VK_NEXT ? KEYEVENTF_EXTENDEDKEY
                                                                                             : 0);
    return SendInput(1, &input, sizeof(input)) == 1;
}

bool MoveMouse(HWND window, int x, int y)
{
    if (GetForegroundWindow() != window)
        return false;
    POINT point { x, y };
    ClientToScreen(window, &point);
    INPUT input {};
    input.type = INPUT_MOUSE;
    input.mi.dx = static_cast<LONG>(point.x * 65535.0 / (GetSystemMetrics(SM_CXSCREEN) - 1));
    input.mi.dy = static_cast<LONG>(point.y * 65535.0 / (GetSystemMetrics(SM_CYSCREEN) - 1));
    input.mi.dwFlags = MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE;
    return SendInput(1, &input, sizeof(input)) == 1;
}

COLORREF ScreenPixel(HWND window, int x, int y)
{
    POINT point { x, y };
    ClientToScreen(window, &point);
    // Another program's window over this point would make the sample mean nothing.
    if (WindowFromPoint(point) != window)
        return CLR_INVALID;
    HDC screen = GetDC(nullptr);
    const COLORREF colour = GetPixel(screen, point.x, point.y);
    ReleaseDC(nullptr, screen);
    return colour;
}

DWORD HostProcess()
{
    DWORD found = 0;
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    PROCESSENTRY32W entry { sizeof(entry) };
    for (BOOL more = Process32FirstW(snapshot, &entry); more && !found; more = Process32NextW(snapshot, &entry))
        if (entry.th32ParentProcessID == GetCurrentProcessId() &&
            _wcsicmp(entry.szExeFile, L"OptiScalerDx9Host.exe") == 0)
            found = entry.th32ProcessID;
    CloseHandle(snapshot);
    return found;
}

struct Vertex
{
    float x, y, z, rhw;
    DWORD colour;
};

void Draw(IDirect3DDevice9* device, UINT width, UINT height, int frame)
{
    const float t = frame * 0.05f;
    const auto channel = [&](float phase) { return static_cast<DWORD>(127.5f + 127.5f * std::sin(t + phase)); };
    device->Clear(0, nullptr, D3DCLEAR_TARGET, D3DCOLOR_XRGB(channel(0), channel(2.1f), channel(4.2f)), 1, 0);
    device->BeginScene();
    const float x = std::fmod(frame * 8.0f, static_cast<float>(width));
    const float w = width / 8.0f;
    const Vertex bar[] {
        { x, 0, 0, 1, 0xffffffff },
        { x + w, 0, 0, 1, 0xffff2020 },
        { x, float(height), 0, 1, 0xff2020ff },
        { x + w, float(height), 0, 1, 0xff20ff20 },
    };
    device->SetFVF(D3DFVF_XYZRHW | D3DFVF_DIFFUSE);
    device->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
    device->SetRenderState(D3DRS_LIGHTING, FALSE);
    device->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, bar, sizeof(Vertex));
    device->EndScene();
}

DWORD WINAPI Watchdog(LPVOID seconds)
{
    Sleep(static_cast<DWORD>(reinterpret_cast<uintptr_t>(seconds)) * 1000);
    printf("FAIL timeout\n");
    fflush(stdout);
    TerminateProcess(GetCurrentProcess(), 3);
    return 0;
}
} // namespace

int main(int argc, char** argv)
{
    bool ex = false, native = false, vsync = false, expectBridged = false, expectFg = false, screen = false;
    int frames = 240, menuAt = -1, resetAt = -1, killAt = -1, timeout = 120;
    UINT width = 1280, height = 720, resetWidth = 0, resetHeight = 0;
    std::vector<std::pair<int, WORD>> keys;
    for (int i = 1; i < argc; ++i)
    {
        const std::string a = argv[i];
        auto next = [&] { return i + 1 < argc ? argv[++i] : "0"; };
        if (a == "--ex")
            ex = true;
        else if (a == "--native")
            native = true;
        else if (a == "--vsync")
            vsync = true;
        else if (a == "--expect-bridged")
            expectBridged = true;
        else if (a == "--expect-fg")
            expectFg = true;
        else if (a == "--screen")
            screen = true;
        else if (a == "--frames")
            frames = atoi(next());
        else if (a == "--timeout")
            timeout = atoi(next());
        else if (a == "--size")
            sscanf_s(next(), "%ux%u", &width, &height);
        else if (a == "--menu-at")
            menuAt = atoi(next());
        else if (a == "--kill-host-at")
            killAt = atoi(next());
        else if (a == "--reset-at")
        {
            resetAt = atoi(next());
            sscanf_s(next(), "%ux%u", &resetWidth, &resetHeight);
        }
        else if (a == "--key-at")
        {
            const int at = atoi(next());
            keys.push_back({ at, static_cast<WORD>(strtoul(next(), nullptr, 0)) });
        }
        else
            return Fail(("unknown option " + a).c_str());
    }
    CreateThread(nullptr, 0, Watchdog, reinterpret_cast<LPVOID>(static_cast<uintptr_t>(timeout)), 0, nullptr);

    wchar_t path[MAX_PATH] {};
    if (native)
    {
        GetSystemDirectoryW(path, MAX_PATH);
        wcscat_s(path, L"\\d3d9.dll");
    }
    else
    {
        GetModuleFileNameW(nullptr, path, MAX_PATH);
        wcscpy_s(wcsrchr(path, L'\\') + 1, MAX_PATH - (wcsrchr(path, L'\\') + 1 - path), L"d3d9.dll");
    }
    const HMODULE d3d9 = LoadLibraryW(path);
    if (!d3d9)
        return Fail("d3d9.dll did not load");
    using StatusFn = void(WINAPI*)(dx9wire::ProxyStatus*);
    const auto readStatus = reinterpret_cast<StatusFn>(GetProcAddress(d3d9, "OptiScalerX86Status"));
    if (!native && !readStatus)
        return Fail("the d3d9.dll beside the program is not the proxy");
    auto Status = [&]
    {
        dx9wire::ProxyStatus s {};
        if (readStatus)
            readStatus(&s);
        return s;
    };

    WNDCLASSW type {};
    type.lpfnWndProc = Proc;
    type.hInstance = GetModuleHandleW(nullptr);
    type.hCursor = LoadCursor(nullptr, IDC_ARROW);
    type.lpszClassName = L"OptiDx9X86Smoke";
    RegisterClassW(&type);
    RECT rect { 0, 0, LONG(width), LONG(height) };
    AdjustWindowRect(&rect, WS_OVERLAPPEDWINDOW, FALSE);
    const int left = std::max(0, GetSystemMetrics(SM_CXSCREEN) - int(rect.right - rect.left) - 40);
    HWND window =
        CreateWindowExW(WS_EX_TOPMOST, type.lpszClassName, L"OptiScaler DX9 x86 smoke", WS_OVERLAPPEDWINDOW, left, 40,
                        rect.right - rect.left, rect.bottom - rect.top, nullptr, nullptr, type.hInstance, nullptr);
    if (!window)
        return Fail("window");
    ShowWindow(window, SW_SHOW);
    UpdateWindow(window);
    const bool front = Foreground(window);

    IDirect3D9* api = nullptr;
    IDirect3D9Ex* apiEx = nullptr;
    if (ex)
    {
        auto create =
            reinterpret_cast<HRESULT(WINAPI*)(UINT, IDirect3D9Ex**)>(GetProcAddress(d3d9, "Direct3DCreate9Ex"));
        if (!create || FAILED(create(D3D_SDK_VERSION, &apiEx)))
            return Fail("Direct3DCreate9Ex");
        api = apiEx;
    }
    else
    {
        auto create = reinterpret_cast<IDirect3D9*(WINAPI*) (UINT)>(GetProcAddress(d3d9, "Direct3DCreate9"));
        api = create ? create(D3D_SDK_VERSION) : nullptr;
        if (!api)
            return Fail("Direct3DCreate9");
    }
    D3DPRESENT_PARAMETERS params {};
    params.BackBufferWidth = width;
    params.BackBufferHeight = height;
    params.BackBufferFormat = D3DFMT_X8R8G8B8;
    params.BackBufferCount = 1;
    params.SwapEffect = D3DSWAPEFFECT_DISCARD;
    params.hDeviceWindow = window;
    params.Windowed = TRUE;
    params.PresentationInterval = vsync ? D3DPRESENT_INTERVAL_ONE : D3DPRESENT_INTERVAL_IMMEDIATE;
    IDirect3DDevice9* device = nullptr;
    IDirect3DDevice9Ex* deviceEx = nullptr;
    const DWORD flags = D3DCREATE_HARDWARE_VERTEXPROCESSING | D3DCREATE_FPU_PRESERVE;
    if (ex)
    {
        if (FAILED(
                apiEx->CreateDeviceEx(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, window, flags, &params, nullptr, &deviceEx)))
            return Fail("CreateDeviceEx");
        device = deviceEx;
    }
    else if (FAILED(api->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, window, flags, &params, &device)))
        return Fail("CreateDevice");

    COLORREF samples[2] {};
    int sampled = 0;
    unsigned bridgedAtKill = 0, menuSeen = 0, menuGone = 0, inputSent = 0, keysSent = 0, keysSkipped = 0;
    long mouseWhileMenu = 0;
    bool menuOpenNow = false;
    unsigned bridgedAtReset = 0;
    for (int frame = 0; frame < frames; ++frame)
    {
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE))
        {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        for (const auto& [at, vk] : keys)
        {
            if (frame == at || frame == at + 6)
            {
                const bool sent = Key(window, vk, frame != at);
                (sent ? keysSent : keysSkipped)++;
            }
        }
        if (menuAt >= 0)
        {
            // Open at menuAt, move the mouse over the window while it is open, close 60 frames later.
            if (frame == menuAt || frame == menuAt + 60)
                (Key(window, VK_INSERT, false) ? keysSent : keysSkipped)++;
            if (frame == menuAt + 6 || frame == menuAt + 66)
                (Key(window, VK_INSERT, true) ? keysSent : keysSkipped)++;
            const bool open = (Status().state & dx9wire::StateMenu) != 0;
            if (open && !menuOpenNow)
            {
                menuSeen = frame;
                mouseWhileMenu = mouseMessages;
            }
            if (!open && menuOpenNow)
            {
                menuGone = frame;
                mouseWhileMenu = mouseMessages - mouseWhileMenu;
            }
            menuOpenNow = open;
            if (open && frame >= menuAt + 20 && frame < menuAt + 50)
                inputSent += MoveMouse(window, 100 + (frame % 10) * 20, 100 + (frame % 7) * 15) ? 1 : 0;
        }
        if (frame == killAt)
        {
            bridgedAtKill = Status().bridged;
            const DWORD pid = HostProcess();
            HANDLE process = pid ? OpenProcess(PROCESS_TERMINATE | SYNCHRONIZE, FALSE, pid) : nullptr;
            if (!process)
                return Fail("no host process to end");
            TerminateProcess(process, 9);
            WaitForSingleObject(process, 5000);
            CloseHandle(process);
            printf("ended host %lu at frame %d\n", pid, frame);
        }
        if (frame == resetAt)
        {
            bridgedAtReset = Status().bridged;
            width = resetWidth;
            height = resetHeight;
            RECT r { 0, 0, LONG(width), LONG(height) };
            AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);
            SetWindowPos(window, nullptr, 0, 0, r.right - r.left, r.bottom - r.top, SWP_NOMOVE | SWP_NOZORDER);
            params.BackBufferWidth = width;
            params.BackBufferHeight = height;
            const HRESULT hr = ex ? deviceEx->ResetEx(&params, nullptr) : device->Reset(&params);
            if (FAILED(hr))
                return Fail("Reset");
            printf("reset to %ux%u at frame %d\n", width, height, frame);
        }
        Draw(device, width, height, frame);
        const HRESULT hr = ex ? deviceEx->PresentEx(nullptr, nullptr, nullptr, nullptr, 0)
                              : device->Present(nullptr, nullptr, nullptr, nullptr);
        if (FAILED(hr))
        {
            printf("Present failed %08lX at frame %d\n", hr, frame);
            return Fail("Present");
        }
        // Right of centre: an on-screen display such as RivaTuner's draws over the left side.
        if (frame == frames - 40 || frame == frames - 10)
        {
            Sleep(30);
            samples[sampled++] = ScreenPixel(window, width * 3 / 4, height / 2);
        }
    }

    const auto s = Status();
    const bool covered = samples[0] == CLR_INVALID || samples[1] == CLR_INVALID;
    const bool screenMoves = !covered && samples[0] != samples[1];
    printf("status: bridged=%u native=%u hostStarts=%u faults=%u disabled=%u transport=%u size=%ux%u state=%u "
           "menuFrames=%u blockedInput=%u\n",
           s.bridged, s.native, s.hostStarts, s.faults, s.disabled, s.transport, s.width, s.height, s.state,
           s.menuFrames, s.blockedInput);
    printf("screen=%s samples=%06lX,%06lX foreground=%d keysSent=%u keysSkipped=%u\n",
           covered       ? "covered"
           : screenMoves ? "ok"
                         : "unchanged",
           samples[0], samples[1], front ? 1 : 0, keysSent, keysSkipped);

    int failed = 0;
    if (screen && !screenMoves)
        failed = Fail("the window's pixels on screen did not change between samples");
    if (expectBridged && (s.bridged + 8 < unsigned(frames) - (killAt >= 0 ? 8 : 0) || s.disabled))
        failed = Fail("frames were not presented by the host");
    if (expectFg && !(s.state & dx9wire::StateFrameGen))
        failed = Fail("frame generation was not active at the end");
    if (resetAt >= 0 && (s.width != resetWidth || s.height != resetHeight || s.bridged < bridgedAtReset + 20))
        failed = Fail("the bridge did not carry on at the new size after Reset");
    if (killAt >= 0)
    {
        printf("restart: bridged before end %u, after %u, starts %u, faults %u\n", bridgedAtKill, s.bridged,
               s.hostStarts, s.faults);
        if (s.hostStarts < 2 || s.faults < 1 || s.bridged < bridgedAtKill + 30)
            failed = Fail("the host was not started again after it ended");
    }
    if (menuAt >= 0)
    {
        printf("menu: opened at %u, closed at %u, mouse moves sent %u, mouse messages the game saw %ld, held %u\n",
               menuSeen, menuGone, inputSent, mouseWhileMenu, s.blockedInput);
        if (keysSkipped)
            printf("SKIP menu: this window was not in front, no keys were sent\n");
        else if (!menuSeen || !menuGone)
            failed = Fail("the menu did not open and close with Insert");
        else if (inputSent && (mouseWhileMenu != 0 || s.blockedInput == 0))
            failed = Fail("mouse input reached the game while the menu was open");
    }
    if (!keys.empty() && keysSkipped)
        printf("SKIP keys: %u key events not sent (window not in front)\n", keysSkipped);

    device->Release();
    api->Release();
    DestroyWindow(window);
    if (!failed)
        printf("PASS\n");
    fflush(stdout);
    return failed;
}
