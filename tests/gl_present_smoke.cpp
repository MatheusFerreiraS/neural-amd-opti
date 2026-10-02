// A real OpenGL app for OptiScaler's opengl32.dll route. Run it beside OptiScaler.dll renamed
// opengl32.dll: it links opengl32.lib, so the loader picks the proxy from the exe folder.
//
// It presents through gdi32's SwapBuffers like most games, posts the menu key (Insert) to open and
// close the menu, and checks after every present that the game's context and GL state are as it
// left them. What reached the screen is read back through DWM (PrintWindow with
// PW_RENDERFULLCONTENT): the open menu has to leave pixels that are not the clear colour, and in nr
// mode a red block drawn at the top right has to come back red and at the top right, which catches a
// flip or a red/blue swap in the crossing to D3D12.
//
//   gl_present_smoke.exe [frames] [menu|nr]
// Exit 0 on success, 1 on a failed check, 3 when the watchdog fires.

#include <windows.h>
#include <GL/gl.h>
#include <dwmapi.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#ifndef GL_SCISSOR_TEST
#define GL_SCISSOR_TEST 0x0C11
#endif

constexpr int kWidth = 1280, kHeight = 720;

static int Fail(const char* what)
{
    std::printf("FAIL: %s\n", what);
    std::fflush(stdout);
    return 1;
}

static DWORD WINAPI Watchdog(void*)
{
    Sleep(120000);
    std::printf("FAIL: watchdog, the app did not finish in 120 s\n");
    std::fflush(stdout);
    ExitProcess(3);
}

static LRESULT CALLBACK WindowProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam)
{
    if (msg == WM_CLOSE)
        return 0;
    return DefWindowProcW(hwnd, msg, wparam, lparam);
}

static void PostKey(HWND hwnd, WPARAM vk)
{
    const UINT scan = MapVirtualKeyW(static_cast<UINT>(vk), MAPVK_VK_TO_VSC);
    PostMessageW(hwnd, WM_KEYDOWN, vk, 1 | (scan << 16));
    PostMessageW(hwnd, WM_KEYUP, vk, 1 | (scan << 16) | (1u << 30) | (1u << 31));
}

// The client area as DWM last composed it, top row first, BGRA.
static bool Capture(HWND hwnd, std::vector<unsigned char>& pixels)
{
    BITMAPINFO info {};
    info.bmiHeader.biSize = sizeof(info.bmiHeader);
    info.bmiHeader.biWidth = kWidth;
    info.bmiHeader.biHeight = -kHeight;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    void* bits = nullptr;
    HDC screen = GetDC(nullptr);
    HDC memory = CreateCompatibleDC(screen);
    HBITMAP bitmap = CreateDIBSection(screen, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
    bool captured = false;
    if (memory && bitmap)
    {
        auto previous = SelectObject(memory, bitmap);
        captured = PrintWindow(hwnd, memory, PW_CLIENTONLY | PW_RENDERFULLCONTENT) != FALSE;
        GdiFlush();
        if (captured)
            pixels.assign(static_cast<unsigned char*>(bits), static_cast<unsigned char*>(bits) + kWidth * kHeight * 4);
        SelectObject(memory, previous);
    }
    if (bitmap)
        DeleteObject(bitmap);
    if (memory)
        DeleteDC(memory);
    ReleaseDC(nullptr, screen);
    return captured;
}

static void SaveBmp(const char* path, const std::vector<unsigned char>& bgra)
{
    BITMAPFILEHEADER file {};
    BITMAPINFOHEADER header {};
    header.biSize = sizeof(header);
    header.biWidth = kWidth;
    header.biHeight = -kHeight;
    header.biPlanes = 1;
    header.biBitCount = 32;
    file.bfType = 0x4D42;
    file.bfOffBits = sizeof(file) + sizeof(header);
    file.bfSize = file.bfOffBits + static_cast<DWORD>(bgra.size());
    FILE* out = nullptr;
    if (fopen_s(&out, path, "wb") == 0 && out)
    {
        std::fwrite(&file, sizeof(file), 1, out);
        std::fwrite(&header, sizeof(header), 1, out);
        std::fwrite(bgra.data(), 1, bgra.size(), out);
        std::fclose(out);
    }
}

static int ForeignPixels(const std::vector<unsigned char>& bgra)
{
    int foreign = 0;
    for (size_t i = 0; i < bgra.size(); i += 4)
        if (bgra[i] < 240 || bgra[i + 1] > 15 || bgra[i + 2] < 240)
            foreign++;
    return foreign;
}

// Average red, green and blue over a box given in top-left coordinates.
static void Average(const std::vector<unsigned char>& bgra, int x0, int y0, int x1, int y1, double rgb[3])
{
    double sum[3] {};
    for (int y = y0; y < y1; y++)
        for (int x = x0; x < x1; x++)
        {
            const auto* p = &bgra[(size_t(y) * kWidth + x) * 4];
            sum[0] += p[2];
            sum[1] += p[1];
            sum[2] += p[0];
        }
    const double count = double(x1 - x0) * (y1 - y0);
    for (int i = 0; i < 3; i++)
        rgb[i] = sum[i] / count;
}

static void Rect(float x0, float y0, float x1, float y1)
{
    glBegin(GL_QUADS);
    glVertex2f(x0, y0);
    glVertex2f(x1, y0);
    glVertex2f(x1, y1);
    glVertex2f(x0, y1);
    glEnd();
}

int main(int argc, char** argv)
{
    const int frames = argc > 1 ? std::atoi(argv[1]) : 300;
    const bool nr = argc > 2 && std::strcmp(argv[2], "nr") == 0;
    const ULONGLONG launched = GetTickCount64();
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    CreateThread(nullptr, 0, Watchdog, nullptr, 0, nullptr);

    const auto module = GetModuleHandleW(nullptr);
    WNDCLASSW type {};
    type.style = CS_OWNDC;
    type.lpfnWndProc = WindowProc;
    type.hInstance = module;
    type.hCursor = LoadCursorW(nullptr, (LPCWSTR) IDC_ARROW);
    type.lpszClassName = L"OptiGlPresentSmoke";
    if (!RegisterClassW(&type))
        return Fail("RegisterClass");

    RECT rect { 0, 0, kWidth, kHeight };
    AdjustWindowRect(&rect, WS_OVERLAPPEDWINDOW, FALSE);
    HWND hwnd = CreateWindowW(type.lpszClassName, L"Opti OpenGL smoke", WS_OVERLAPPEDWINDOW, 40, 40,
                              rect.right - rect.left, rect.bottom - rect.top, nullptr, nullptr, module, nullptr);
    if (!hwnd)
        return Fail("CreateWindow");
    ShowWindow(hwnd, SW_SHOW);

    // A game is the foreground window, and frame generation pauses when it is not. A process started
    // from a script is not allowed to take the foreground, unless its input is attached to the
    // thread that has it.
    const DWORD foregroundThread = GetWindowThreadProcessId(GetForegroundWindow(), nullptr);
    AttachThreadInput(foregroundThread, GetCurrentThreadId(), TRUE);
    SetForegroundWindow(hwnd);
    BringWindowToTop(hwnd);
    AttachThreadInput(foregroundThread, GetCurrentThreadId(), FALSE);
    std::printf("window in the foreground: %s\n", GetForegroundWindow() == hwnd ? "yes" : "no");

    HDC hdc = GetDC(hwnd);
    PIXELFORMATDESCRIPTOR pfd { sizeof(pfd), 1 };
    pfd.dwFlags = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER;
    pfd.iPixelType = PFD_TYPE_RGBA;
    pfd.cColorBits = 32;
    pfd.cAlphaBits = 8;
    pfd.cDepthBits = 24;
    const int format = ChoosePixelFormat(hdc, &pfd);
    if (format == 0 || !SetPixelFormat(hdc, format, &pfd))
        return Fail("pixel format");

    HGLRC context = wglCreateContext(hdc);
    if (!context || !wglMakeCurrent(hdc, context))
        return Fail("context");

    wchar_t proxyPath[MAX_PATH] {};
    GetModuleFileNameW(GetModuleHandleW(L"opengl32.dll"), proxyPath, MAX_PATH);
    std::printf("opengl32.dll in use: %ls, context after %llu ms\n", proxyPath, GetTickCount64() - launched);
    std::printf("GL_VERSION %s, GL_RENDERER %s\n", (const char*) glGetString(GL_VERSION),
                (const char*) glGetString(GL_RENDERER));

    // State the hooks must not disturb.
    glClearColor(1.0f, 0.0f, 1.0f, 1.0f);
    glEnable(GL_SCISSOR_TEST);
    glScissor(0, 0, kWidth, kHeight);
    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    glOrtho(0, kWidth, 0, kHeight, -1, 1);

    // A fixed noise patch: flat colours pass through the network unchanged, detail does not.
    constexpr int kPatchX = 450, kPatchTop = 40, kPatchW = 200, kPatchH = 120;
    std::vector<unsigned char> patch(size_t(kPatchW) * kPatchH * 4);
    for (size_t i = 0; i < patch.size(); i += 4)
    {
        const auto hash = static_cast<unsigned>(i * 2654435761u) >> 24;
        patch[i] = patch[i + 1] = patch[i + 2] = static_cast<unsigned char>(64 + hash / 2);
        patch[i + 3] = 255;
    }

    const ULONGLONG started = GetTickCount64();
    ULONGLONG phase[2] { started, started };
    std::vector<unsigned char> closed, open;
    bool capturedClosed = false, capturedOpen = false;
    const int openAt = frames / 4, closeAt = frames * 3 / 4;
    for (int frame = 0; frame < frames; frame++)
    {
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE))
        {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }

        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        if (nr)
        {
            // A red block at the top right, a green one below it, and moving content in between so
            // the network and the optical flow see motion. OpenGL's origin is bottom left. The left
            // side is left alone: on-screen display tools draw there.
            glRasterPos2i(kPatchX, kHeight - kPatchTop - kPatchH);
            glDrawPixels(kPatchW, kPatchH, GL_RGBA, GL_UNSIGNED_BYTE, patch.data());
            glColor3f(1.0f, 0.0f, 0.0f);
            Rect(kWidth - 300, kHeight - 200, kWidth - 40, kHeight - 40);
            glColor3f(0.0f, 1.0f, 0.0f);
            Rect(kWidth - 300, 40, kWidth - 40, 200);
            for (int i = 0; i < 8; i++)
            {
                const float x = float((frame * 4 + i * 150) % kWidth);
                glColor3f(0.1f + 0.1f * i, 0.8f - 0.08f * i, 0.3f + 0.06f * i);
                Rect(x, 260.0f + i * 25, x + 90, 300.0f + i * 25);
            }
        }

        if (!SwapBuffers(hdc))
            return Fail("SwapBuffers");

        if (wglGetCurrentContext() != context || wglGetCurrentDC() != hdc)
            return Fail("the game's context is not current after the present");
        GLfloat clear[4] {};
        glGetFloatv(GL_COLOR_CLEAR_VALUE, clear);
        if (clear[0] != 1.0f || clear[1] != 0.0f || clear[2] != 1.0f || !glIsEnabled(GL_SCISSOR_TEST))
            return Fail("GL state changed across the present");
        if (glGetError() != GL_NO_ERROR)
            return Fail("GL error in the game's context");

        if (frame == openAt - 5 || frame == closeAt - 5)
        {
            glFinish();
            DwmFlush();
            if (frame == openAt - 5)
                capturedClosed = Capture(hwnd, closed);
            else
                capturedOpen = Capture(hwnd, open);
        }
        if (frame == openAt)
        {
            phase[0] = GetTickCount64();
            PostKey(hwnd, VK_INSERT);
        }
        if (frame == closeAt)
        {
            phase[1] = GetTickCount64();
            PostKey(hwnd, VK_INSERT);
        }
    }
    const ULONGLONG ended = GetTickCount64();

    std::printf("%d frames in %llu ms: %llu before the menu opens, %llu open, %llu after\n", frames, ended - started,
                phase[0] - started, phase[1] - phase[0], ended - phase[1]);

    if (!capturedClosed || !capturedOpen)
        return Fail("PrintWindow could not capture the window");
    SaveBmp("menu_closed.bmp", closed);
    SaveBmp("menu_open.bmp", open);

    if (nr)
    {
        double topRight[3], bottomRight[3], clearArea[3];
        Average(closed, kWidth - 280, 60, kWidth - 60, 180, topRight);
        Average(closed, kWidth - 280, kHeight - 180, kWidth - 60, kHeight - 60, bottomRight);
        Average(closed, 700, 60, 900, 180, clearArea);
        double patchDifference = 0;
        for (int y = 0; y < kPatchH; y++)
            for (int x = 0; x < kPatchW; x++)
            {
                const auto* shown = &closed[(size_t(kPatchTop + y) * kWidth + kPatchX + x) * 4];
                const auto* drawn = &patch[(size_t(kPatchH - 1 - y) * kPatchW + x) * 4];
                for (int c = 0; c < 3; c++)
                    patchDifference += std::abs(int(shown[c]) - int(drawn[2 - c]));
            }
        patchDifference /= double(kPatchW) * kPatchH * 3;
        std::printf("noise patch: mean difference %.2f from what was drawn\n", patchDifference);
        std::printf("after NR: top right %.0f,%.0f,%.0f, bottom right %.0f,%.0f,%.0f, clear area %.0f,%.0f,%.0f\n",
                    topRight[0], topRight[1], topRight[2], bottomRight[0], bottomRight[1], bottomRight[2], clearArea[0],
                    clearArea[1], clearArea[2]);
        if (topRight[0] < 150 || topRight[1] > 100 || topRight[2] > 100)
            return Fail("the red block is not red at the top right after NR");
        if (bottomRight[1] < 150 || bottomRight[0] > 100 || bottomRight[2] > 100)
            return Fail("the green block is not green at the bottom right after NR");
        if (clearArea[0] < 150 || clearArea[2] < 150 || clearArea[1] > 100)
            return Fail("the clear area is not the clear colour after NR");
    }

    const int closedForeign = ForeignPixels(closed), openForeign = ForeignPixels(open);
    std::printf("presented pixels other than the clear colour: %d with the menu closed, %d with it open\n",
                closedForeign, openForeign);
    if (openForeign - closedForeign < 5000)
        return Fail("the open menu did not reach the screen");

    wglMakeCurrent(nullptr, nullptr);
    wglDeleteContext(context);
    ReleaseDC(hwnd, hdc);
    DestroyWindow(hwnd);
    std::printf("PASS: %d frames presented\n", frames);
    std::fflush(stdout);
    return 0;
}
