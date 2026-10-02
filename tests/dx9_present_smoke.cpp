// A 64-bit D3D9 game in miniature for OptiScaler loaded as d3d9.dll beside it. It renders a scrolling pattern,
// presents through IDirect3DDevice9::Present, PresentEx or IDirect3DSwapChain9::Present, and follows a script of
// key presses posted to its window (the menu, NR and frame-generation keys), device Resets with a new size and screen
// samples. It prints the frame time of each scripted phase and whether sampled frames still change on screen.
//   dx9_present_smoke.exe [--ex] [--chain] [--msaa] [--vsync] [--borderless] [--size WxH] [--frames N] [--shots folder]
//                         [--script "frame:action,..."]
// Actions: menu (Insert), nr (Numpad 5), fg (End), still (stops or restarts the scrolling), reset=WxH, shot=name,
// motion=name, mark=name.
#define NOMINMAX
#include <windows.h>
#include <d3d9.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <vector>

namespace
{
using Create9 = IDirect3D9*(WINAPI*) (UINT);
using Create9Ex = HRESULT(WINAPI*)(UINT, IDirect3D9Ex**);

struct Vertex
{
    float x, y, z, rhw;
    D3DCOLOR colour;
};

struct Shot
{
    std::vector<unsigned char> pixels;
    int width = 0, height = 0;
};

HWND window = nullptr;
bool quit = false;
bool borderless = false;

LRESULT CALLBACK Proc(HWND hwnd, UINT message, WPARAM w, LPARAM l)
{
    if (message == WM_CLOSE || message == WM_DESTROY)
        quit = true;

    return DefWindowProcW(hwnd, message, w, l);
}

void Pump()
{
    MSG message;

    while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE))
    {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
}

void PostKey(UINT vk)
{
    const UINT scan = MapVirtualKeyW(vk, MAPVK_VK_TO_VSC);
    const bool extended = vk == VK_INSERT || vk == VK_END || vk == VK_HOME || vk == VK_DELETE;
    const LPARAM base = 1 | (LPARAM(scan) << 16) | (extended ? (1 << 24) : 0);
    PostMessageW(window, WM_KEYDOWN, vk, base);
    PostMessageW(window, WM_KEYUP, vk, base | (LPARAM(1) << 30) | (LPARAM(1) << 31));
}

void ClientSize(int width, int height)
{
    RECT rect { 0, 0, width, height };
    const int origin = borderless ? 0 : 40;

    if (!borderless)
        AdjustWindowRect(&rect, WS_OVERLAPPEDWINDOW, FALSE);

    SetWindowPos(window, HWND_TOPMOST, origin, origin, rect.right - rect.left, rect.bottom - rect.top,
                 SWP_SHOWWINDOW);
}

Shot Capture()
{
    Shot shot;
    RECT client {};
    GetClientRect(window, &client);
    POINT origin { 0, 0 };
    ClientToScreen(window, &origin);
    shot.width = client.right;
    shot.height = client.bottom;

    HDC screen = GetDC(nullptr);
    HDC memory = CreateCompatibleDC(screen);
    BITMAPINFO info {};
    info.bmiHeader.biSize = sizeof(info.bmiHeader);
    info.bmiHeader.biWidth = shot.width;
    info.bmiHeader.biHeight = -shot.height;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    void* bits = nullptr;
    HBITMAP bitmap = CreateDIBSection(screen, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
    auto old = SelectObject(memory, bitmap);
    BitBlt(memory, 0, 0, shot.width, shot.height, screen, origin.x, origin.y, SRCCOPY | CAPTUREBLT);
    GdiFlush();
    shot.pixels.assign(static_cast<unsigned char*>(bits),
                       static_cast<unsigned char*>(bits) + size_t(shot.width) * shot.height * 4);
    SelectObject(memory, old);
    DeleteObject(bitmap);
    DeleteDC(memory);
    ReleaseDC(nullptr, screen);
    return shot;
}

double Mean(const Shot& shot)
{
    double sum = 0;

    for (size_t i = 0; i + 3 < shot.pixels.size(); i += 4)
        sum += 0.0722 * shot.pixels[i] + 0.7152 * shot.pixels[i + 1] + 0.2126 * shot.pixels[i + 2];

    return shot.pixels.empty() ? 0 : sum / (shot.pixels.size() / 4);
}

double Changed(const Shot& a, const Shot& b)
{
    if (a.pixels.size() != b.pixels.size() || a.pixels.empty())
        return 1.0;

    size_t differ = 0;

    for (size_t i = 0; i < a.pixels.size(); i += 4)
        differ += std::abs(int(a.pixels[i]) - int(b.pixels[i])) +
                      std::abs(int(a.pixels[i + 1]) - int(b.pixels[i + 1])) +
                      std::abs(int(a.pixels[i + 2]) - int(b.pixels[i + 2])) >
                  24;

    return double(differ) / (a.pixels.size() / 4);
}

void Save(const Shot& shot, const std::string& path)
{
    BITMAPFILEHEADER file {};
    BITMAPINFOHEADER header {};
    header.biSize = sizeof(header);
    header.biWidth = shot.width;
    header.biHeight = -shot.height;
    header.biPlanes = 1;
    header.biBitCount = 32;
    file.bfType = 0x4D42;
    file.bfOffBits = sizeof(file) + sizeof(header);
    file.bfSize = DWORD(file.bfOffBits + shot.pixels.size());

    if (FILE* out = std::fopen(path.c_str(), "wb"))
    {
        std::fwrite(&file, sizeof(file), 1, out);
        std::fwrite(&header, sizeof(header), 1, out);
        std::fwrite(shot.pixels.data(), 1, shot.pixels.size(), out);
        std::fclose(out);
    }
}

// Checkerboard cells scrolling right with a bar moving down: enough structure for the optical flow.
void Draw(IDirect3DDevice9* device, int frame, int width, int height)
{
    std::vector<Vertex> vertices;
    const int cell = 64;
    const int offset = (frame * 6) % (cell * 2);

    auto quad = [&](float x0, float y0, float x1, float y1, D3DCOLOR colour)
    {
        const Vertex v[6] = { { x0, y0, 0, 1, colour }, { x1, y0, 0, 1, colour }, { x0, y1, 0, 1, colour },
                              { x1, y0, 0, 1, colour }, { x1, y1, 0, 1, colour }, { x0, y1, 0, 1, colour } };
        vertices.insert(vertices.end(), v, v + 6);
    };

    for (int y = 0; y < height; y += cell)
        for (int x = -cell * 2; x < width; x += cell)
            if (((x + cell * 2) / cell + y / cell) % 2 == 0)
                quad(float(x + offset), float(y), float(x + offset + cell), float(y + cell),
                     D3DCOLOR_XRGB(230, 200 - (y * 120 / height), 60 + (x & 127)));

    const float bar = float((frame * 4) % height);
    quad(0, bar, float(width), bar + 24, D3DCOLOR_XRGB(250, 250, 250));

    device->Clear(0, nullptr, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER, D3DCOLOR_XRGB(30, 60, 110), 1.0f, 0);
    device->BeginScene();
    device->SetFVF(D3DFVF_XYZRHW | D3DFVF_DIFFUSE);
    device->SetRenderState(D3DRS_LIGHTING, FALSE);
    device->SetRenderState(D3DRS_ZENABLE, FALSE);
    device->DrawPrimitiveUP(D3DPT_TRIANGLELIST, UINT(vertices.size() / 3), vertices.data(), sizeof(Vertex));
    device->EndScene();
}
} // namespace

int main(int argc, char** argv)
{
    bool ex = false, chainPresent = false, msaa = false, vsync = false;
    int width = 1280, height = 720, frames = 300;
    std::string shots = ".", script;

    for (int i = 1; i < argc; ++i)
    {
        const std::string arg = argv[i];
        const char* next = i + 1 < argc ? argv[i + 1] : "";

        if (arg == "--ex")
            ex = true;
        else if (arg == "--chain")
            chainPresent = true;
        else if (arg == "--msaa")
            msaa = true;
        else if (arg == "--borderless")
            borderless = true;
        else if (arg == "--vsync")
            vsync = true;
        else if (arg == "--size" && std::sscanf(next, "%dx%d", &width, &height) == 2)
            ++i;
        else if (arg == "--frames")
            frames = std::atoi(argv[++i]);
        else if (arg == "--shots")
            shots = argv[++i];
        else if (arg == "--script")
            script = argv[++i];
    }

    std::multimap<int, std::string> actions;

    for (size_t start = 0; start < script.size();)
    {
        size_t end = script.find(',', start);
        const std::string item = script.substr(start, end == std::string::npos ? std::string::npos : end - start);
        const size_t colon = item.find(':');

        if (colon != std::string::npos)
            actions.emplace(std::atoi(item.c_str()), item.substr(colon + 1));

        start = end == std::string::npos ? script.size() : end + 1;
    }

    // The proxy beside this executable, never the system d3d9.dll.
    wchar_t path[MAX_PATH] {};
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    std::wstring proxy(path);
    proxy = proxy.substr(0, proxy.find_last_of(L"\\/") + 1) + L"d3d9.dll";
    HMODULE d3d9 = LoadLibraryW(proxy.c_str());

    if (d3d9 == nullptr || GetProcAddress(d3d9, "GetBehaviorValue") == nullptr)
    {
        std::printf("FAIL: d3d9.dll beside the test is not OptiScaler\n");
        return 2;
    }

    WNDCLASSW windowClass {};
    windowClass.lpfnWndProc = Proc;
    windowClass.hInstance = GetModuleHandleW(nullptr);
    windowClass.lpszClassName = L"OptiDx9Smoke";
    windowClass.hCursor = LoadCursor(nullptr, IDC_ARROW);
    RegisterClassW(&windowClass);
    window = CreateWindowExW(WS_EX_TOPMOST, windowClass.lpszClassName, L"Opti D3D9 smoke",
                             borderless ? WS_POPUP : WS_OVERLAPPEDWINDOW, 40, 40,
                             width, height, nullptr, nullptr, windowClass.hInstance, nullptr);
    ClientSize(width, height);
    SetForegroundWindow(window);
    Pump();

    IDirect3D9* api = nullptr;
    IDirect3D9Ex* apiEx = nullptr;

    if (ex)
    {
        auto create = reinterpret_cast<Create9Ex>(GetProcAddress(d3d9, "Direct3DCreate9Ex"));

        if (create == nullptr || FAILED(create(D3D_SDK_VERSION, &apiEx)))
            return 3;

        api = apiEx;
    }
    else
    {
        auto create = reinterpret_cast<Create9>(GetProcAddress(d3d9, "Direct3DCreate9"));
        api = create ? create(D3D_SDK_VERSION) : nullptr;
    }

    if (api == nullptr)
    {
        std::printf("FAIL: Direct3DCreate9\n");
        return 3;
    }

    D3DPRESENT_PARAMETERS params {};
    params.Windowed = TRUE;
    params.SwapEffect = D3DSWAPEFFECT_DISCARD;
    params.hDeviceWindow = window;
    params.BackBufferWidth = width;
    params.BackBufferHeight = height;
    params.BackBufferFormat = D3DFMT_X8R8G8B8;
    params.BackBufferCount = 1;
    params.EnableAutoDepthStencil = TRUE;
    params.AutoDepthStencilFormat = D3DFMT_D24S8;
    params.MultiSampleType = msaa ? D3DMULTISAMPLE_4_SAMPLES : D3DMULTISAMPLE_NONE;
    params.PresentationInterval = vsync ? D3DPRESENT_INTERVAL_ONE : D3DPRESENT_INTERVAL_IMMEDIATE;

    IDirect3DDevice9* device = nullptr;
    IDirect3DDevice9Ex* deviceEx = nullptr;
    HRESULT result = ex ? apiEx->CreateDeviceEx(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, window,
                                                D3DCREATE_HARDWARE_VERTEXPROCESSING, &params, nullptr, &deviceEx)
                        : api->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, window,
                                            D3DCREATE_HARDWARE_VERTEXPROCESSING, &params, &device);

    if (ex)
        device = deviceEx;

    if (FAILED(result))
    {
        std::printf("FAIL: CreateDevice %08X\n", unsigned(result));
        return 4;
    }

    std::printf("device %s, %dx%d, present through %s\n", ex ? "D3D9Ex" : "D3D9", width, height,
                chainPresent ? "swapchain"
                : ex         ? "PresentEx"
                             : "Present");

    std::map<std::string, Shot> pending;
    std::multimap<int, std::string> motions;
    std::string phase = "start";
    LARGE_INTEGER frequency, phaseStart, previous;
    QueryPerformanceFrequency(&frequency);
    QueryPerformanceCounter(&phaseStart);
    previous = phaseStart;
    int phaseFrames = 0, failures = 0, scene = 0;
    bool still = false;
    double worst = 0;

    auto endPhase = [&](const std::string& next)
    {
        LARGE_INTEGER now;
        QueryPerformanceCounter(&now);
        const double ms = 1000.0 * double(now.QuadPart - phaseStart.QuadPart) / double(frequency.QuadPart);

        if (phaseFrames > 0)
            std::printf("phase %s: %d frames, %.2f ms average, %.2f ms worst\n", phase.c_str(), phaseFrames,
                        ms / phaseFrames, worst);

        phase = next;
        phaseStart = now;
        phaseFrames = 0;
        worst = 0;
    };

    for (int frame = 0; frame < frames && !quit; ++frame)
    {
        for (auto [it, end] = actions.equal_range(frame); it != end; ++it)
        {
            const std::string& action = it->second;
            std::printf("frame %d: %s\n", frame, action.c_str());

            if (action == "menu")
                PostKey(VK_INSERT);
            else if (action == "nr")
                PostKey(VK_NUMPAD5);
            else if (action == "fg")
                PostKey(VK_END);
            else if (action == "still")
                still = !still;
            else if (action.rfind("mark=", 0) == 0)
                endPhase(action.substr(5));
            else if (action.rfind("shot=", 0) == 0)
            {
                const Shot shot = Capture();
                Save(shot, shots + "\\" + action.substr(5) + ".bmp");
                std::printf("shot %s: mean %.1f\n", action.substr(5).c_str(), Mean(shot));
            }
            else if (action.rfind("motion=", 0) == 0)
            {
                pending[action.substr(7)] = Capture();
                motions.emplace(frame + 3, action.substr(7));
            }
            else if (action.rfind("reset=", 0) == 0 && std::sscanf(action.c_str() + 6, "%dx%d", &width, &height) == 2)
            {
                ClientSize(width, height);
                Pump();
                params.BackBufferWidth = width;
                params.BackBufferHeight = height;
                const HRESULT reset = ex ? deviceEx->ResetEx(&params, nullptr) : device->Reset(&params);
                std::printf("reset %dx%d: %08X\n", width, height, unsigned(reset));

                if (FAILED(reset))
                {
                    std::printf("FAIL: Reset\n");
                    return 5;
                }
            }
        }

        for (auto [it, end] = motions.equal_range(frame); it != end; ++it)
        {
            const double changed = Changed(pending[it->second], Capture());
            std::printf("motion %s: %.3f of the pixels changed\n", it->second.c_str(), changed);
        }

        Pump();
        scene += still ? 0 : 1;
        Draw(device, scene, width, height);

        if (chainPresent)
        {
            IDirect3DSwapChain9* chain = nullptr;
            device->GetSwapChain(0, &chain);
            result = chain->Present(nullptr, nullptr, nullptr, nullptr, 0);
            chain->Release();
        }
        else
        {
            result = ex ? deviceEx->PresentEx(nullptr, nullptr, nullptr, nullptr, 0)
                        : device->Present(nullptr, nullptr, nullptr, nullptr);
        }

        if (FAILED(result) && ++failures < 5)
            std::printf("present %d failed: %08X\n", frame, unsigned(result));

        LARGE_INTEGER now;
        QueryPerformanceCounter(&now);
        worst = std::max(worst, 1000.0 * double(now.QuadPart - previous.QuadPart) / double(frequency.QuadPart));
        previous = now;
        ++phaseFrames;
    }

    endPhase("end");
    device->Release();
    api->Release();
    DestroyWindow(window);
    std::printf("DONE %d present failures\n", failures);
    return failures == 0 ? 0 : 6;
}
