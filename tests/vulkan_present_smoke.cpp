#define NOMINMAX
#define VK_USE_PLATFORM_WIN32_KHR
#include <vulkan/vulkan.h>
#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <thread>
#include <vector>

// Regression for the Vulkan final-image route. Run in a folder holding OptiScaler.dll, OptiScaler.ini,
// a danielblnc runtime and XeFG (tools\test-vulkan-present.cmd prepares one). The window lives on one
// thread and Vulkan presents from another that never pumps messages, the way emulators such as yuzu
// work. Real keyboard and mouse input drives NR, XeFG, the menu, focus loss, minimize, resize and the
// toggles; each step checks OptiScaler.log, the XeFG window and whether the game window still gets
// its input. With "off" as argument it checks instead that PresentWithoutUpscaler=false leaves the
// game, the menu and the keys alone. Exit code is the number of failed steps.

namespace
{
constexpr wchar_t kFgClass[] = L"OptiScalerVulkanXeFG";
constexpr int kWidth = 1280, kHeight = 720;

HWND game = nullptr, other = nullptr;
DWORD guiThread = 0;
std::atomic<int> mouseMoves { 0 }, keyDowns { 0 }, clicks { 0 }, frames { 0 };
// Milliseconds of CPU work the render thread spends on each frame, to stand in for a heavy game.
std::atomic<int> workMs { 0 };
std::atomic<bool> stopRender { false }, freeze { false }, renderFailed { false };
int failures = 0;

void Print(const char* format, auto... values)
{
    std::printf(format, values...);
    std::printf("\n");
    std::fflush(stdout);
}

void Step(const char* name, bool ok, const char* detail = "")
{
    SYSTEMTIME now;
    GetLocalTime(&now);
    Print("%02u:%02u:%02u.%03u %s %s%s%s", now.wHour, now.wMinute, now.wSecond, now.wMilliseconds, ok ? "PASS" : "FAIL",
          name, *detail ? ": " : "", detail);
    failures += ok ? 0 : 1;
}

// Log -------------------------------------------------------------------------------------------

std::string ReadLog()
{
    std::ifstream file("OptiScaler.log", std::ios::binary);
    return { std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>() };
}

size_t Mark() { return ReadLog().size(); }

int Count(const char* needle, size_t from)
{
    const auto log = ReadLog();
    int count = 0;
    for (auto at = log.find(needle, from); at != std::string::npos; at = log.find(needle, at + 1))
        ++count;
    return count;
}

template <typename Check> bool WaitFor(Check check, DWORD timeout)
{
    for (const auto start = GetTickCount64(); GetTickCount64() - start < timeout; Sleep(50))
        if (check())
            return true;
    return check();
}

bool WaitLog(const char* needle, size_t from, DWORD timeout)
{
    return WaitFor([&] { return Count(needle, from) > 0; }, timeout);
}

// True once a "Vulkan XeFG: frame" status line after `from` shows the provider presenting
// `presented` frames per real frame without an error.
bool Presented(size_t from, int presented)
{
    const auto log = ReadLog();
    const std::string needle = "Vulkan XeFG: frame ";
    for (auto at = log.find(needle, from); at != std::string::npos; at = log.find(needle, at + 1))
    {
        const auto line = log.substr(at, log.find('\n', at) - at);
        int frame = 0, shown = 0, generated = 0, result = -1;
        if (sscanf_s(line.c_str(), "Vulkan XeFG: frame %d, presented %d, generated %d, result %d", &frame, &shown,
                     &generated, &result) == 4 &&
            shown >= presented && generated == 1 && result == 0)
            return true;
    }
    return false;
}

bool WaitPresented(size_t from, DWORD timeout = 15000)
{
    return WaitFor([&] { return Presented(from, 3); }, timeout);
}

// Windows ---------------------------------------------------------------------------------------

HWND FgWindow() { return FindWindowExW(game, nullptr, kFgClass, nullptr); }
bool FgVisible() { return IsWindowVisible(FgWindow()) != FALSE; }

// A game window that stops answering ends the run: nothing after it can be measured.
bool GuiAlive()
{
    DWORD_PTR result = 0;
    if (SendMessageTimeoutW(game, WM_NULL, 0, 0, SMTO_ABORTIFHUNG | SMTO_BLOCK, 2000, &result))
        return true;
    Step("game window answers", false, "hung, stopping");
    TerminateProcess(GetCurrentProcess(), 100 + failures);
    return false;
}

bool FramesAdvance()
{
    const int before = frames;
    return WaitFor([&] { return frames > before + 10; }, 5000);
}

// Real presents of the test game per second over `ms`.
double PresentsPerSecond(DWORD ms)
{
    const int before = frames;
    const auto start = GetTickCount64();
    Sleep(ms);
    return (frames - before) * 1000.0 / double(GetTickCount64() - start);
}

bool Foreground(HWND window)
{
    for (int i = 0; i < 20 && GetForegroundWindow() != window; ++i)
    {
        // An Alt press lifts the foreground lock when no window or another process holds it.
        INPUT alt[2] {};
        alt[0].type = alt[1].type = INPUT_KEYBOARD;
        alt[0].ki.wVk = alt[1].ki.wVk = VK_MENU;
        alt[1].ki.dwFlags = KEYEVENTF_KEYUP;
        SendInput(1, &alt[0], sizeof(INPUT));
        const DWORD current = GetWindowThreadProcessId(GetForegroundWindow(), nullptr);
        AttachThreadInput(GetCurrentThreadId(), current, TRUE);
        BringWindowToTop(window);
        SetForegroundWindow(window);
        AttachThreadInput(GetCurrentThreadId(), current, FALSE);
        SendInput(1, &alt[1], sizeof(INPUT));
        if (GetForegroundWindow() != window)
            SwitchToThisWindow(window, TRUE);
        Sleep(150);
    }
    return GetForegroundWindow() == window;
}

// OptiScaler drops SendInput calls from the game process while its menu is open, so input comes
// from a second copy of this program: "input key <vk>", "input move <x> <y> ..." or
// "input click <x> <y>" in screen pixels.
int Inject(int argc, wchar_t** argv)
{
    if (argc >= 4 && !wcscmp(argv[2], L"key"))
    {
        const auto vk = static_cast<WORD>(_wtoi(argv[3]));
        const bool extended = vk == VK_INSERT || vk == VK_END || vk == VK_HOME || vk == VK_PRIOR || vk == VK_NEXT;
        INPUT input {};
        input.type = INPUT_KEYBOARD;
        input.ki.wVk = vk;
        input.ki.wScan = static_cast<WORD>(MapVirtualKeyW(vk, MAPVK_VK_TO_VSC));
        input.ki.dwFlags = extended ? KEYEVENTF_EXTENDEDKEY : 0;
        UINT sent = SendInput(1, &input, sizeof(input));
        Sleep(80);
        input.ki.dwFlags |= KEYEVENTF_KEYUP;
        sent += SendInput(1, &input, sizeof(input));
        return sent == 2 ? 0 : 1;
    }
    const int virtualX = GetSystemMetrics(SM_XVIRTUALSCREEN), virtualY = GetSystemMetrics(SM_YVIRTUALSCREEN);
    const int virtualW = GetSystemMetrics(SM_CXVIRTUALSCREEN), virtualH = GetSystemMetrics(SM_CYVIRTUALSCREEN);
    for (int i = 3; i + 1 < argc; i += 2)
    {
        INPUT input {};
        input.type = INPUT_MOUSE;
        input.mi.dx = static_cast<LONG>((_wtoi(argv[i]) - virtualX) * 65535LL / (virtualW - 1));
        input.mi.dy = static_cast<LONG>((_wtoi(argv[i + 1]) - virtualY) * 65535LL / (virtualH - 1));
        input.mi.dwFlags = MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_VIRTUALDESK;
        if (SendInput(1, &input, sizeof(input)) != 1)
            return 1;
        Sleep(40);
    }
    // "input click <x> <y>" ends with a left click where the cursor stopped.
    if (!wcscmp(argv[2], L"click"))
        for (DWORD flag : { MOUSEEVENTF_LEFTDOWN, MOUSEEVENTF_LEFTUP })
        {
            Sleep(150);
            INPUT input {};
            input.type = INPUT_MOUSE;
            input.mi.dwFlags = flag;
            if (SendInput(1, &input, sizeof(input)) != 1)
                return 1;
        }
    return 0;
}

bool RunInjector(const std::wstring& arguments)
{
    wchar_t path[MAX_PATH] {};
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    std::wstring command = L"\"" + std::wstring(path) + L"\" input " + arguments;
    STARTUPINFOW startup { sizeof(startup) };
    PROCESS_INFORMATION process {};
    if (!CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &startup, &process))
        return false;
    DWORD code = 1;
    if (WaitForSingleObject(process.hProcess, 10000) == WAIT_OBJECT_0)
        GetExitCodeProcess(process.hProcess, &code);
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    return code == 0;
}

void Key(WORD vk)
{
    if (!RunInjector(L"key " + std::to_wstring(vk)))
        Print("key %02x was not injected", vk);
    Sleep(1200); // past the menu's shortcut debounce
}

RECT ClientOnScreen()
{
    RECT rect {};
    GetClientRect(game, &rect);
    MapWindowPoints(game, nullptr, reinterpret_cast<POINT*>(&rect), 2);
    return rect;
}

// Moves the real cursor across the client area and presses a key, then checks that the game
// window's own procedure received both. A window that holds the input queue stops this.
bool GameGetsInput(const char*& why)
{
    POINT saved {};
    GetCursorPos(&saved);
    const RECT rect = ClientOnScreen();
    const int moves = mouseMoves, keys = keyDowns, clicked = clicks;
    std::wstring path = L"click";
    for (int i = 1; i <= 8; ++i)
        path += L" " + std::to_wstring(rect.left + (rect.right - rect.left) * i / 10) + L" " +
                std::to_wstring(rect.top + (rect.bottom - rect.top) * (i % 2 ? 1 : 2) / 3);
    RunInjector(path);
    Key('K');
    const bool mouse = WaitFor([&] { return mouseMoves > moves; }, 2000);
    const bool keyboard = WaitFor([&] { return keyDowns > keys; }, 2000);
    const bool click = WaitFor([&] { return clicks > clicked; }, 2000);
    // A disabled child passes its clicks to the game window.
    const bool disabled = !FgWindow() || !IsWindowEnabled(FgWindow());
    RunInjector(L"move " + std::to_wstring(saved.x) + L" " + std::to_wstring(saved.y));
    why = !mouse      ? "mouse moves did not reach the game window"
          : !keyboard ? "key did not reach the game window"
          : !click    ? "click did not reach the game window"
          : !disabled ? "XeFG window is enabled"
                      : "";
    return mouse && keyboard && click && disabled;
}

// Left click at a point of the game's client area.
bool Click(int x, int y)
{
    const RECT rect = ClientOnScreen();
    const bool sent = RunInjector(L"click " + std::to_wstring(rect.left + x) + L" " + std::to_wstring(rect.top + y));
    Sleep(600);
    return sent;
}

bool FocusStaysWithGame(const char*& why)
{
    GUITHREADINFO info { sizeof(info) };
    GetGUIThreadInfo(guiThread, &info);
    const HWND fg = FgWindow();
    why = fg && (info.hwndFocus == fg || info.hwndActive == fg) ? "XeFG window holds focus" : "";
    return !*why;
}

std::vector<uint32_t> Capture()
{
    const RECT rect = ClientOnScreen();
    const int width = rect.right - rect.left, height = rect.bottom - rect.top;
    std::vector<uint32_t> pixels(size_t(width) * height);
    HDC screen = GetDC(nullptr);
    HDC memory = CreateCompatibleDC(screen);
    BITMAPINFO info {};
    info.bmiHeader = { sizeof(BITMAPINFOHEADER), width, -height, 1, 32, BI_RGB };
    void* bits = nullptr;
    HBITMAP bitmap = CreateDIBSection(screen, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
    auto previous = SelectObject(memory, bitmap);
    BitBlt(memory, 0, 0, width, height, screen, rect.left, rect.top, SRCCOPY | CAPTUREBLT);
    std::memcpy(pixels.data(), bits, pixels.size() * 4);
    SelectObject(memory, previous);
    DeleteObject(bitmap);
    DeleteDC(memory);
    ReleaseDC(nullptr, screen);
    return pixels;
}

// Saves a capture of the client area beside the log, for a person to look at.
void SaveCapture(const std::vector<uint32_t>& pixels, const char* name)
{
    const RECT rect = ClientOnScreen();
    const LONG width = rect.right - rect.left, height = rect.bottom - rect.top;
    if (pixels.size() != size_t(width) * height)
        return;
    BITMAPINFOHEADER info { sizeof(info), width, -height, 1, 32, BI_RGB };
    BITMAPFILEHEADER header {};
    header.bfType = 0x4D42;
    header.bfOffBits = sizeof(header) + sizeof(info);
    header.bfSize = header.bfOffBits + DWORD(pixels.size() * 4);
    std::ofstream file(name, std::ios::binary);
    file.write(reinterpret_cast<const char*>(&header), sizeof(header));
    file.write(reinterpret_cast<const char*>(&info), sizeof(info));
    file.write(reinterpret_cast<const char*>(pixels.data()), pixels.size() * 4);
}

bool Differs(uint32_t a, uint32_t b)
{
    int distance = 0;
    for (int shift : { 0, 8, 16 })
        distance += std::abs(int((a >> shift) & 255) - int((b >> shift) & 255));
    return distance > 60;
}

double Changed(const std::vector<uint32_t>& a, const std::vector<uint32_t>& b)
{
    if (a.size() != b.size() || a.empty())
        return 0.0;
    size_t changed = 0;
    for (size_t i = 0; i < a.size(); ++i)
        changed += Differs(a[i], b[i]);
    return double(changed) / a.size();
}

// With the menu open the block starts moving again under it. The menu's pixels in the rows the block
// sweeps are compared with the still capture: a menu that XeFG warped or ghosted on generated frames
// changes there, a menu laid over every frame does not. The rest of those rows shows the motion. The
// captures span about three seconds, the time the block takes to cross the frame.
bool MenuOverMotion(const std::vector<uint32_t>& closed, const std::vector<uint32_t>& open)
{
    const RECT rect = ClientOnScreen();
    const int width = rect.right - rect.left, height = rect.bottom - rect.top;
    if (open.size() != size_t(width) * height || closed.size() != open.size())
        return false;
    freeze = false;
    Sleep(300);
    constexpr int shots = 30;
    size_t menu = 0, menuChanged = 0, around = 0, aroundChanged = 0;
    for (int shot = 0; shot < shots; ++shot)
    {
        const auto moving = Capture();
        if (moving.size() != open.size())
            return false;
        if (shot % 10 == 0)
            SaveCapture(moving, ("capture-menu-moving-" + std::to_string(shot / 10) + ".bmp").c_str());
        for (int y = height / 2 - 64; y < height / 2 + 64; ++y)
            for (int x = 0; x < width; ++x)
            {
                const size_t i = size_t(y) * width + x;
                const bool inMenu = Differs(closed[i], open[i]);
                (inMenu ? menu : around) += 1;
                (inMenu ? menuChanged : aroundChanged) += Differs(moving[i], inMenu ? open[i] : closed[i]);
            }
        Sleep(70);
    }
    const double menuShare = menu ? double(menuChanged) / menu : 1.0;
    const double aroundShare = around ? double(aroundChanged) / around : 0.0;
    Print("menu over motion: %zu menu pixels in the moving rows, %.2f%% changed; the game around it %.2f%%",
          menu / shots, menuShare * 100.0, aroundShare * 100.0);
    return menu > 0 && menuShare < 0.03 && aroundShare > 0.05;
}

LRESULT CALLBACK GameProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam)
{
    if (message == WM_MOUSEMOVE)
        ++mouseMoves;
    else if (message == WM_LBUTTONDOWN)
        ++clicks;
    else if (message == WM_KEYDOWN && wparam == 'K')
        ++keyDowns;
    else if (message == WM_APP)
    {
        DestroyWindow(other);
        DestroyWindow(window);
        PostQuitMessage(0);
        return 0;
    }
    else if (message == WM_CLOSE || (message == WM_SYSCOMMAND && (wparam & 0xFFF0) == SC_KEYMENU))
        return 0;
    return DefWindowProcW(window, message, wparam, lparam);
}

void Gui(HANDLE ready)
{
    const auto module = GetModuleHandleW(nullptr);
    WNDCLASSW type {};
    type.lpfnWndProc = GameProc;
    type.hInstance = module;
    type.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));
    type.lpszClassName = L"OptiVulkanPresentSmoke";
    RegisterClassW(&type);
    RECT rect { 0, 0, kWidth, kHeight };
    AdjustWindowRect(&rect, WS_OVERLAPPEDWINDOW, FALSE);
    game = CreateWindowW(type.lpszClassName, L"Opti Vulkan smoke", WS_OVERLAPPEDWINDOW, 40, 40, rect.right - rect.left,
                         rect.bottom - rect.top, nullptr, nullptr, module, nullptr);
    other = CreateWindowW(type.lpszClassName, L"Opti Vulkan smoke other app", WS_OVERLAPPEDWINDOW, 200, 200, 400, 300,
                          nullptr, nullptr, module, nullptr);
    ShowWindow(game, SW_SHOW);
    guiThread = GetCurrentThreadId();
    SetEvent(ready);
    MSG message;
    while (GetMessageW(&message, nullptr, 0, 0) > 0)
    {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
}

// Vulkan ----------------------------------------------------------------------------------------

#define VK_CHECK(call)                                                                                                 \
    do                                                                                                                 \
    {                                                                                                                  \
        const VkResult checked = (call);                                                                               \
        if (checked != VK_SUCCESS)                                                                                     \
        {                                                                                                              \
            Print("render: %s returned %d", #call, (int) checked);                                                     \
            return false;                                                                                              \
        }                                                                                                              \
    } while (false)

struct Renderer
{
    VkInstance instance = VK_NULL_HANDLE;
    VkSurfaceKHR surface = VK_NULL_HANDLE;
    VkPhysicalDevice physical = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    VkQueue queue = VK_NULL_HANDLE;
    uint32_t family = UINT32_MAX;
    VkSurfaceFormatKHR format {};
    VkSwapchainKHR chain = VK_NULL_HANDLE;
    VkExtent2D extent {};
    std::vector<VkImage> images;
    std::vector<VkSemaphore> acquired, rendered;
    VkCommandPool pool = VK_NULL_HANDLE;
    VkCommandBuffer command = VK_NULL_HANDLE;
    VkFence fence = VK_NULL_HANDLE;
    VkBuffer bar = VK_NULL_HANDLE;
    VkDeviceMemory barMemory = VK_NULL_HANDLE;
    uint32_t acquireSlot = 0;
    int position = 0;

    bool Init()
    {
        const char* extensions[] { VK_KHR_SURFACE_EXTENSION_NAME, VK_KHR_WIN32_SURFACE_EXTENSION_NAME };
        VkApplicationInfo app { VK_STRUCTURE_TYPE_APPLICATION_INFO };
        app.apiVersion = VK_API_VERSION_1_1;
        VkInstanceCreateInfo instanceInfo { VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO };
        instanceInfo.pApplicationInfo = &app;
        instanceInfo.enabledExtensionCount = 2;
        instanceInfo.ppEnabledExtensionNames = extensions;
        VK_CHECK(vkCreateInstance(&instanceInfo, nullptr, &instance));
        VkWin32SurfaceCreateInfoKHR surfaceInfo { VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR };
        surfaceInfo.hinstance = GetModuleHandleW(nullptr);
        surfaceInfo.hwnd = game;
        VK_CHECK(vkCreateWin32SurfaceKHR(instance, &surfaceInfo, nullptr, &surface));
        uint32_t count = 0;
        VK_CHECK(vkEnumeratePhysicalDevices(instance, &count, nullptr));
        std::vector<VkPhysicalDevice> physicals(count);
        VK_CHECK(vkEnumeratePhysicalDevices(instance, &count, physicals.data()));
        for (auto candidate : physicals)
        {
            uint32_t families = 0;
            vkGetPhysicalDeviceQueueFamilyProperties(candidate, &families, nullptr);
            std::vector<VkQueueFamilyProperties> properties(families);
            vkGetPhysicalDeviceQueueFamilyProperties(candidate, &families, properties.data());
            for (uint32_t i = 0; i < families && !physical; ++i)
            {
                VkBool32 present = VK_FALSE;
                vkGetPhysicalDeviceSurfaceSupportKHR(candidate, i, surface, &present);
                if (present && (properties[i].queueFlags & VK_QUEUE_GRAPHICS_BIT))
                {
                    physical = candidate;
                    family = i;
                }
            }
        }
        if (!physical)
            return false;
        const float priority = 1.0f;
        VkDeviceQueueCreateInfo queueInfo { VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO };
        queueInfo.queueFamilyIndex = family;
        queueInfo.queueCount = 1;
        queueInfo.pQueuePriorities = &priority;
        const char* swapchainExtension = VK_KHR_SWAPCHAIN_EXTENSION_NAME;
        VkDeviceCreateInfo deviceInfo { VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO };
        deviceInfo.queueCreateInfoCount = 1;
        deviceInfo.pQueueCreateInfos = &queueInfo;
        deviceInfo.enabledExtensionCount = 1;
        deviceInfo.ppEnabledExtensionNames = &swapchainExtension;
        VK_CHECK(vkCreateDevice(physical, &deviceInfo, nullptr, &device));
        vkGetDeviceQueue(device, family, 0, &queue);

        uint32_t formats = 0;
        VK_CHECK(vkGetPhysicalDeviceSurfaceFormatsKHR(physical, surface, &formats, nullptr));
        std::vector<VkSurfaceFormatKHR> options(formats);
        VK_CHECK(vkGetPhysicalDeviceSurfaceFormatsKHR(physical, surface, &formats, options.data()));
        format = options[0];
        for (auto option : options)
            if (option.format == VK_FORMAT_B8G8R8A8_UNORM)
                format = option;

        VkCommandPoolCreateInfo poolInfo { VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
        poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        poolInfo.queueFamilyIndex = family;
        VK_CHECK(vkCreateCommandPool(device, &poolInfo, nullptr, &pool));
        VkCommandBufferAllocateInfo allocate { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
        allocate.commandPool = pool;
        allocate.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        allocate.commandBufferCount = 1;
        VK_CHECK(vkAllocateCommandBuffers(device, &allocate, &command));
        VkFenceCreateInfo fenceInfo { VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
        fenceInfo.flags = VK_FENCE_CREATE_SIGNALED_BIT;
        VK_CHECK(vkCreateFence(device, &fenceInfo, nullptr, &fence));

        // A white 128x128 block that slides across the frame gives the optical flow real motion.
        VkBufferCreateInfo bufferInfo { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
        bufferInfo.size = 128 * 128 * 4;
        bufferInfo.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
        VK_CHECK(vkCreateBuffer(device, &bufferInfo, nullptr, &bar));
        VkMemoryRequirements requirements {};
        vkGetBufferMemoryRequirements(device, bar, &requirements);
        VkPhysicalDeviceMemoryProperties memory {};
        vkGetPhysicalDeviceMemoryProperties(physical, &memory);
        VkMemoryAllocateInfo memoryInfo { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
        memoryInfo.allocationSize = requirements.size;
        for (uint32_t i = 0; i < memory.memoryTypeCount; ++i)
            if ((requirements.memoryTypeBits & (1u << i)) &&
                (memory.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) &&
                (memory.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT))
            {
                memoryInfo.memoryTypeIndex = i;
                break;
            }
        VK_CHECK(vkAllocateMemory(device, &memoryInfo, nullptr, &barMemory));
        VK_CHECK(vkBindBufferMemory(device, bar, barMemory, 0));
        void* mapped = nullptr;
        VK_CHECK(vkMapMemory(device, barMemory, 0, bufferInfo.size, 0, &mapped));
        std::memset(mapped, 0xFF, bufferInfo.size);
        vkUnmapMemory(device, barMemory);
        return true;
    }

    // Returns false on a Vulkan error; a minimized window leaves the old chain in place.
    bool Recreate()
    {
        VkSurfaceCapabilitiesKHR caps {};
        VK_CHECK(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(physical, surface, &caps));
        if (caps.currentExtent.width == 0 || caps.currentExtent.height == 0)
            return true;
        VK_CHECK(vkDeviceWaitIdle(device));
        VkSwapchainCreateInfoKHR info { VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR };
        info.surface = surface;
        info.minImageCount = std::max(caps.minImageCount, 3u);
        if (caps.maxImageCount)
            info.minImageCount = std::min(info.minImageCount, caps.maxImageCount);
        info.imageFormat = format.format;
        info.imageColorSpace = format.colorSpace;
        info.imageExtent = caps.currentExtent;
        info.imageArrayLayers = 1;
        info.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        info.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
        info.preTransform = caps.currentTransform;
        info.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
        info.presentMode = VK_PRESENT_MODE_FIFO_KHR;
        info.clipped = VK_TRUE;
        info.oldSwapchain = chain;
        VkSwapchainKHR created = VK_NULL_HANDLE;
        VK_CHECK(vkCreateSwapchainKHR(device, &info, nullptr, &created));
        if (chain)
            vkDestroySwapchainKHR(device, chain, nullptr);
        chain = created;
        extent = caps.currentExtent;
        uint32_t count = 0;
        VK_CHECK(vkGetSwapchainImagesKHR(device, chain, &count, nullptr));
        images.resize(count);
        VK_CHECK(vkGetSwapchainImagesKHR(device, chain, &count, images.data()));
        for (auto semaphore : acquired)
            vkDestroySemaphore(device, semaphore, nullptr);
        for (auto semaphore : rendered)
            vkDestroySemaphore(device, semaphore, nullptr);
        acquired.assign(count + 1, VK_NULL_HANDLE);
        rendered.assign(count, VK_NULL_HANDLE);
        VkSemaphoreCreateInfo semaphoreInfo { VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
        for (auto& semaphore : acquired)
            VK_CHECK(vkCreateSemaphore(device, &semaphoreInfo, nullptr, &semaphore));
        for (auto& semaphore : rendered)
            VK_CHECK(vkCreateSemaphore(device, &semaphoreInfo, nullptr, &semaphore));
        Print("render: swapchain %ux%u, %u images", extent.width, extent.height, count);
        return true;
    }

    bool Frame()
    {
        VkSurfaceCapabilitiesKHR caps {};
        VK_CHECK(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(physical, surface, &caps));
        if (caps.currentExtent.width == 0 || caps.currentExtent.height == 0)
        {
            Sleep(16);
            return true;
        }
        if (!chain || caps.currentExtent.width != extent.width || caps.currentExtent.height != extent.height)
            return Recreate();

        VK_CHECK(vkWaitForFences(device, 1, &fence, VK_TRUE, 5'000'000'000ull));
        const uint32_t slot = acquireSlot++ % acquired.size();
        uint32_t index = 0;
        auto result = vkAcquireNextImageKHR(device, chain, 5'000'000'000ull, acquired[slot], VK_NULL_HANDLE, &index);
        if (result == VK_ERROR_OUT_OF_DATE_KHR)
            return Recreate();
        if (result != VK_SUCCESS && result != VK_SUBOPTIMAL_KHR)
        {
            Print("render: acquire returned %d", (int) result);
            return false;
        }
        VK_CHECK(vkResetFences(device, 1, &fence));
        VK_CHECK(vkResetCommandBuffer(command, 0));
        VkCommandBufferBeginInfo begin { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
        begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        VK_CHECK(vkBeginCommandBuffer(command, &begin));
        VkImageMemoryBarrier barrier { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
        barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        barrier.srcQueueFamilyIndex = barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.image = images[index];
        barrier.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
        vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr,
                             0, nullptr, 1, &barrier);
        const VkClearColorValue background { { 0.15f, 0.35f, 0.6f, 1.0f } };
        vkCmdClearColorImage(command, images[index], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &background, 1,
                             &barrier.subresourceRange);
        if (!freeze)
            position = (position + 12) % int(extent.width - 128);
        VkBufferImageCopy copy {};
        copy.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
        copy.imageOffset = { position, int(extent.height / 2) - 64, 0 };
        copy.imageExtent = { 128, 128, 1 };
        vkCmdCopyBufferToImage(command, bar, images[index], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
        barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        barrier.dstAccessMask = 0;
        barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        barrier.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
        vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0,
                             nullptr, 0, nullptr, 1, &barrier);
        VK_CHECK(vkEndCommandBuffer(command));
        const VkPipelineStageFlags stage = VK_PIPELINE_STAGE_TRANSFER_BIT;
        VkSubmitInfo submit { VK_STRUCTURE_TYPE_SUBMIT_INFO };
        submit.waitSemaphoreCount = 1;
        submit.pWaitSemaphores = &acquired[slot];
        submit.pWaitDstStageMask = &stage;
        submit.commandBufferCount = 1;
        submit.pCommandBuffers = &command;
        submit.signalSemaphoreCount = 1;
        submit.pSignalSemaphores = &rendered[index];
        if (workMs)
            Sleep(workMs);
        VK_CHECK(vkQueueSubmit(queue, 1, &submit, fence));
        VkPresentInfoKHR present { VK_STRUCTURE_TYPE_PRESENT_INFO_KHR };
        present.waitSemaphoreCount = 1;
        present.pWaitSemaphores = &rendered[index];
        present.swapchainCount = 1;
        present.pSwapchains = &chain;
        present.pImageIndices = &index;
        result = vkQueuePresentKHR(queue, &present);
        ++frames;
        if (result == VK_ERROR_OUT_OF_DATE_KHR || result == VK_SUBOPTIMAL_KHR)
            return Recreate();
        if (result != VK_SUCCESS)
        {
            Print("render: present returned %d", (int) result);
            return false;
        }
        return true;
    }

    void Destroy()
    {
        if (device)
            vkDeviceWaitIdle(device);
        for (auto semaphore : acquired)
            vkDestroySemaphore(device, semaphore, nullptr);
        for (auto semaphore : rendered)
            vkDestroySemaphore(device, semaphore, nullptr);
        if (chain)
            vkDestroySwapchainKHR(device, chain, nullptr);
        if (bar)
            vkDestroyBuffer(device, bar, nullptr);
        if (barMemory)
            vkFreeMemory(device, barMemory, nullptr);
        if (fence)
            vkDestroyFence(device, fence, nullptr);
        if (pool)
            vkDestroyCommandPool(device, pool, nullptr);
        if (device)
            vkDestroyDevice(device, nullptr);
        if (surface)
            vkDestroySurfaceKHR(instance, surface, nullptr);
        if (instance)
            vkDestroyInstance(instance, nullptr);
    }
};

void Render()
{
    Renderer renderer;
    if (!renderer.Init() || !renderer.Recreate())
        renderFailed = true;
    while (!renderFailed && !stopRender)
        if (!renderer.Frame())
            renderFailed = true;
    renderer.Destroy();
}

// The NR key must pause XeFG and take its window off the game whatever came before it, and bring
// both back the second time.
void NrOffAndOn(const std::string& when)
{
    auto mark = Mark();
    Key(VK_NUMPAD5);
    const bool paused = WaitLog("Vulkan XeFG paused while final-image NR is off", mark, 3000);
    const bool off = paused && WaitFor([] { return !FgVisible(); }, 3000) && FramesAdvance();
    Step(("NR key turns NR off and hides XeFG " + when).c_str(), off,
         !paused ? "no pause line"
         : off   ? ""
                 : "XeFG window still visible");
    mark = Mark();
    Key(VK_NUMPAD5);
    Step(("NR key turns NR and XeFG back on " + when).c_str(), off && WaitPresented(mark) && FgVisible(),
         off ? "" : "NR was never off");
}

// PresentWithoutUpscaler on: NR, XeFG, the menu, focus, minimize, resize and both keys.
void FinalImageOn(std::thread& render)
{
    const char* why = "";

    Step("NR through the shared D3D12 image", WaitLog("D3D12 shared GPU image active", 0, 60000) &&
                                                  WaitLog("first frame processed using shared D3D12 image", 0, 60000));
    Step("XeFG presents generated frames (3x)", WaitPresented(0, 60000) && FgVisible());
    Step("XeFG window is not owned by the render thread",
         FgWindow() && GetWindowThreadProcessId(FgWindow(), nullptr) != GetThreadId(render.native_handle()));

    Step("game window keeps focus with XeFG on", FocusStaysWithGame(why), why);
    Step("game window gets mouse and keyboard with XeFG on", GameGetsInput(why), why);
    NrOffAndOn("as the first toggle");

    {
        freeze = true;
        Sleep(800);
        const auto before = Capture();
        const auto mark = Mark();
        Key(VK_INSERT);
        const bool opened = WaitLog("menu visibility changed 0 -> 1", mark, 3000);
        Sleep(2500);
        const bool stayed = opened && Count("menu visibility changed 1 -> 0", mark) == 0;
        Step("menu opens with XeFG on and stays open", stayed,
             !opened  ? "never opened"
             : stayed ? ""
                      : "closed by itself");
        Step("XeFG keeps generating with the menu open",
             Count("Vulkan XeFG paused", mark) == 0 && WaitPresented(Mark(), 5000) && FgVisible());
        const auto open = Capture();
        const double changed = Changed(before, open);
        Print("menu covers %.1f%% of the visible frame", changed * 100.0);
        Step("menu is visible on screen over the XeFG output", changed > 0.05);
        SaveCapture(before, "capture-menu-closed.bmp");
        SaveCapture(open, "capture-menu-open.bmp");
        Step("menu holds still over moving generated frames", MenuOverMotion(before, open));

        // Positions in the 1280x720 client area at the menu's automatic scale: the FG section button
        // on the left and the "Active" box of Frame Generation (Vulkan XeFG) in that section.
        const auto section = Capture();
        Click(338, 242);
        const auto fgSection = Capture();
        SaveCapture(fgSection, "capture-menu-fg.bmp");
        Step("a click opens the menu's FG section over the XeFG output", Changed(section, fgSection) > 0.03);
        auto clickMark = Mark();
        Click(361, 325);
        const bool paused = WaitLog("Vulkan XeFG paused while frame generation is off", clickMark, 3000) &&
                            WaitFor([] { return !FgVisible(); }, 3000);
        Step("clicking Active in the menu turns XeFG off", paused);
        clickMark = Mark();
        Click(361, 325);
        Step("clicking Active again turns XeFG back on", paused && WaitPresented(clickMark) && FgVisible(),
             paused ? "" : "XeFG was never off");
        Click(338, 206);
        const auto closeMark = Mark();
        Key(VK_INSERT);
        Step("menu closes with its key", WaitLog("menu visibility changed 1 -> 0", closeMark, 3000));
        freeze = false;
    }

    {
        // XeFG paces its generated frames inside its Present; the game's own present must not wait
        // through that, so turning FG on may not lower the real frame rate. The render thread works
        // 30 ms per frame here: with a light frame the 60 Hz test display caps what XeFG puts out (about
        // 120 frames a second, so 40 real at 3x) and the comparison would measure the display.
        workMs = 30;
        Sleep(1500);
        const double withFg = PresentsPerSecond(5000);
        auto mark = Mark();
        Key(VK_END);
        const bool paused = WaitLog("Vulkan XeFG paused while frame generation is off", mark, 3000);
        const bool off = paused && WaitFor([] { return !FgVisible(); }, 3000) && FramesAdvance();
        Step("FG key turns XeFG off and hides its window", off, paused ? "" : "no pause line");
        const double withoutFg = PresentsPerSecond(5000);
        workMs = 0;
        char rates[96];
        std::snprintf(rates, sizeof(rates), "%.1f real fps with XeFG, %.1f with NR only", withFg, withoutFg);
        Step("XeFG keeps the game's real frame rate", off && withFg >= 0.9 * withoutFg, rates);
        mark = Mark();
        Key(VK_END);
        Step("FG key turns XeFG back on", off && WaitPresented(mark) && FgVisible(), off ? "" : "FG was never off");
    }
    NrOffAndOn("after the FG key");

    {
        auto mark = Mark();
        ShowWindowAsync(other, SW_SHOW);
        const bool switched = Foreground(other);
        const bool paused = WaitLog("paused while game window is in background", mark, 3000) &&
                            WaitFor([] { return !FgVisible(); }, 3000);
        Step("focus loss pauses XeFG and hides its window", switched && paused);
        Step("game window answers while in background", GuiAlive() && FramesAdvance());
        mark = Mark();
        const bool back = Foreground(game);
        ShowWindowAsync(other, SW_HIDE);
        Step("focus return resumes XeFG", back && WaitPresented(mark) && FgVisible());
        Step("game window keeps focus after focus return", FocusStaysWithGame(why), why);
        Step("game window gets mouse and keyboard after focus return", GameGetsInput(why), why);
        Step("game window answers after focus return", GuiAlive());
    }

    {
        ShowWindowAsync(game, SW_MINIMIZE);
        Sleep(1500);
        const bool alive = GuiAlive() && !renderFailed;
        const auto mark = Mark();
        ShowWindowAsync(game, SW_RESTORE);
        const bool back = Foreground(game);
        Step("minimize and restore", alive && back && WaitPresented(mark) && FgVisible() && FramesAdvance());
    }

    {
        const auto mark = Mark();
        RECT rect { 0, 0, 1600, 900 };
        AdjustWindowRect(&rect, WS_OVERLAPPEDWINDOW, FALSE);
        SetWindowPos(game, nullptr, 0, 0, rect.right - rect.left, rect.bottom - rect.top,
                     SWP_NOMOVE | SWP_NOZORDER | SWP_ASYNCWINDOWPOS);
        const bool recreated = WaitLog("Vulkan final-image bridge: swapchain 1600x900", mark, 5000) &&
                               WaitLog("Vulkan XeFG presenter created at 1600x900", mark, 15000);
        RECT client {};
        GetClientRect(FgWindow(), &client);
        Step("resize recreates the swapchain and the XeFG presenter",
             recreated && WaitPresented(mark) && client.right == 1600 && client.bottom == 900);
        NrOffAndOn("right after a resize");
        Step("game window gets mouse and keyboard after resize", GameGetsInput(why), why);
        Step("XeFG still generating after resize", WaitPresented(Mark()) && FramesAdvance());
    }
}

// PresentWithoutUpscaler off: the game presents, the menu works and nothing of the final-image route
// runs, whatever key is pressed.
void FinalImageOff()
{
    Step("game presents with the final image off", WaitFor([] { return frames > 30; }, 30000));
    Sleep(2000);
    auto untouched = []
    {
        const int work = Count("Vulkan final-image bridge: swapchain", 0) + Count("first frame processed", 0) +
                         Count("D3D12 shared GPU image", 0) + Count("Vulkan XeFG", 0);
        return work == 0 && !FgWindow();
    };
    Step("no NR or XeFG work with the final image off", untouched());
    const auto mark = Mark();
    Key(VK_INSERT);
    const bool opened = WaitLog("menu visibility changed 0 -> 1", mark, 3000);
    Sleep(1500);
    Step("menu opens and stays open with the final image off",
         opened && Count("menu visibility changed 1 -> 0", mark) == 0 && FramesAdvance());
    const auto closeMark = Mark();
    Key(VK_INSERT);
    Step("menu closes with its key", WaitLog("menu visibility changed 1 -> 0", closeMark, 3000));
    Key(VK_END);
    Key(VK_NUMPAD5);
    Step("FG and NR keys start nothing with the final image off", FramesAdvance() && untouched());
    Key(VK_NUMPAD5);
    Key(VK_END);
    RECT rect { 0, 0, 1600, 900 };
    AdjustWindowRect(&rect, WS_OVERLAPPEDWINDOW, FALSE);
    SetWindowPos(game, nullptr, 0, 0, rect.right - rect.left, rect.bottom - rect.top,
                 SWP_NOMOVE | SWP_NOZORDER | SWP_ASYNCWINDOWPOS);
    Sleep(1500);
    Step("resize with the final image off", GuiAlive() && FramesAdvance() && !renderFailed && untouched());
}
} // namespace

int wmain(int argc, wchar_t** argv)
{
    if (argc >= 3 && !wcscmp(argv[1], L"input"))
        return Inject(argc, argv);
    if (!LoadLibraryW(L"vulkan-1.dll") || !LoadLibraryW(L"OptiScaler.dll"))
    {
        Print("FAIL setup: OptiScaler.dll or vulkan-1.dll did not load (%lu)", GetLastError());
        return 100;
    }
    Sleep(500);
    HANDLE ready = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    std::thread gui(Gui, ready);
    WaitForSingleObject(ready, 5000);
    if (!game || !Foreground(game))
    {
        Print("FAIL setup: the test window cannot take the foreground (locked or busy desktop)");
        PostMessageW(game, WM_APP, 0, 0);
        gui.join();
        return 101;
    }
    std::thread render(Render);
    if (argc >= 2 && !wcscmp(argv[1], L"off"))
        FinalImageOff();
    else
        FinalImageOn(render);

    Step("XeFG SDK reported no warning", Count("Vulkan XeFG SDK:", 0) == 0);
    Step("no bridge failure in the log", Count("restart required", 0) + Count("synchronization failed", 0) +
                                                     Count("presentation failed", 0) +
                                                     Count("initialization failed", 0) ==
                                                 0 &&
                                             !renderFailed);

    stopRender = true;
    render.join();
    Step("XeFG window is gone after the device", WaitFor([] { return FgWindow() == nullptr; }, 3000));
    PostMessageW(game, WM_APP, 0, 0);
    gui.join();
    Print("%d failed", failures);
    return failures;
}
