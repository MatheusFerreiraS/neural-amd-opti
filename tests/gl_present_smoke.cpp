// A real OpenGL app for OptiScaler's opengl32.dll route. Run it beside OptiScaler.dll renamed
// opengl32.dll: it links opengl32.lib, so the loader picks the proxy from the exe folder.
//
// It presents through gdi32's SwapBuffers like most games, posts the menu key (Insert) to open and
// close the menu, and checks after every present that the game's context and GL state are as it
// left them. With a copy swap the back buffer keeps the presented image, so the pixels can show
// whether the menu was drawn; when the driver does not keep it the check says so and is skipped.
//
//   gl_present_smoke.exe [frames] [menu|nr]
// Exit 0 on success, 1 on a failed check, 3 when the watchdog fires.

#include <windows.h>
#include <GL/gl.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#ifndef GL_SCISSOR_TEST
#define GL_SCISSOR_TEST 0x0C11
#endif

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

// Pixels of the back buffer that are not the clear colour, after the present.
static int ForeignPixels(int width, int height)
{
    std::vector<unsigned char> pixels(size_t(width) * height * 4);
    glReadBuffer(GL_BACK);
    glReadPixels(0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());
    int foreign = 0;
    for (size_t i = 0; i < pixels.size(); i += 4)
        if (pixels[i] < 250 || pixels[i + 1] > 5 || pixels[i + 2] < 250)
            foreign++;
    return foreign;
}

int main(int argc, char** argv)
{
    const int frames = argc > 1 ? std::atoi(argv[1]) : 300;
    const bool nr = argc > 2 && std::strcmp(argv[2], "nr") == 0;
    const ULONGLONG launched = GetTickCount64();
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

    RECT rect { 0, 0, 1280, 720 };
    AdjustWindowRect(&rect, WS_OVERLAPPEDWINDOW, FALSE);
    HWND hwnd = CreateWindowW(type.lpszClassName, L"Opti OpenGL smoke", WS_OVERLAPPEDWINDOW, 40, 40,
                              rect.right - rect.left, rect.bottom - rect.top, nullptr, nullptr, module, nullptr);
    if (!hwnd)
        return Fail("CreateWindow");
    ShowWindow(hwnd, SW_SHOW);
    SetForegroundWindow(hwnd);

    HDC hdc = GetDC(hwnd);
    PIXELFORMATDESCRIPTOR pfd { sizeof(pfd), 1 };
    pfd.dwFlags = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER | PFD_SWAP_COPY;
    pfd.iPixelType = PFD_TYPE_RGBA;
    pfd.cColorBits = 32;
    pfd.cAlphaBits = 8;
    pfd.cDepthBits = 24;
    const int format = ChoosePixelFormat(hdc, &pfd);
    if (format == 0 || !SetPixelFormat(hdc, format, &pfd))
        return Fail("pixel format");
    DescribePixelFormat(hdc, format, sizeof(pfd), &pfd);

    HGLRC context = wglCreateContext(hdc);
    if (!context || !wglMakeCurrent(hdc, context))
        return Fail("context");

    HMODULE proxy = GetModuleHandleW(L"opengl32.dll");
    wchar_t proxyPath[MAX_PATH] {};
    GetModuleFileNameW(proxy, proxyPath, MAX_PATH);
    std::printf("opengl32.dll in use: %ls, context after %llu ms\n", proxyPath, GetTickCount64() - launched);
    std::printf("GL_VERSION %s, GL_RENDERER %s, copy swap requested, pixel format %d flags %lX\n",
                (const char*) glGetString(GL_VERSION), (const char*) glGetString(GL_RENDERER), format, pfd.dwFlags);

    // State the hooks must not disturb.
    glClearColor(1.0f, 0.0f, 1.0f, 1.0f);
    glEnable(GL_SCISSOR_TEST);
    glScissor(0, 0, 1280, 720);

    const ULONGLONG started = GetTickCount64();
    ULONGLONG phase[2] { started, started };
    int copySwap = -1;
    int menuPixels = -1;
    int closedPixels = -1;
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
            // Moving content, so the network and the optical flow see motion.
            glMatrixMode(GL_PROJECTION);
            glLoadIdentity();
            glOrtho(0, 1280, 0, 720, -1, 1);
            for (int i = 0; i < 12; i++)
            {
                const float x = float((frame * 4 + i * 110) % 1280);
                glColor3f(0.1f + 0.07f * i, 0.8f - 0.05f * i, 0.3f + 0.04f * i);
                glBegin(GL_QUADS);
                glVertex2f(x, 100.0f + i * 40);
                glVertex2f(x + 90, 100.0f + i * 40);
                glVertex2f(x + 60, 200.0f + i * 40);
                glVertex2f(x, 200.0f + i * 40);
                glEnd();
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

        if (!nr && frame == openAt - 5)
        {
            closedPixels = ForeignPixels(1280, 720);
            copySwap = closedPixels == 0 ? 1 : 0;
        }
        if (frame == openAt)
        {
            phase[0] = GetTickCount64();
            PostKey(hwnd, VK_INSERT);
        }
        if (!nr && frame == closeAt - 5 && copySwap == 1)
            menuPixels = ForeignPixels(1280, 720);
        if (frame == closeAt)
        {
            phase[1] = GetTickCount64();
            PostKey(hwnd, VK_INSERT);
        }
    }
    const ULONGLONG ended = GetTickCount64();

    std::printf("%d frames in %llu ms: %llu before the menu opens, %llu open, %llu after\n", frames,
                ended - started, phase[0] - started, phase[1] - phase[0], ended - phase[1]);
    if (!nr)
    {
        if (copySwap == 1)
        {
            std::printf("menu pixels: %d with the menu closed, %d with it open\n", closedPixels, menuPixels);
            if (menuPixels < 2000)
                return Fail("the open menu left no pixels in the back buffer");
        }
        else
            std::printf("menu pixels: not checked, the driver does not keep the back buffer (%d foreign "
                        "pixels with the menu closed)\n",
                        closedPixels);
    }

    wglMakeCurrent(nullptr, nullptr);
    wglDeleteContext(context);
    ReleaseDC(hwnd, hdc);
    DestroyWindow(hwnd);
    std::printf("PASS: %d frames presented\n", frames);
    std::fflush(stdout);
    return 0;
}
