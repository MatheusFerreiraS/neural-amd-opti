#pragma once
// The pipe between the 32-bit d3d9.dll (proxy.cpp) and the 64-bit host (host.cpp). The game
// process owns the pipe and starts the host; every request gets one Ack. Handle and Transfer are
// the bounded, peer-aware pipe I/O of the AMD NR ReShade add-on's x86 bridge
// (reshade/core/x86bridge/bridge_io.h), copied here so this pair builds from this repository.
#include <windows.h>
#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace dx9wire
{
constexpr uint32_t Magic = 0x39445850, Version = 1;
constexpr DWORD IpcTimeoutMs = 5000, StartupTimeoutMs = 60000;

enum class Kind : uint32_t
{
    Hello = 1,
    Build = 2,
    Frame = 3,
    Drop = 4,
    Quit = 5
};
enum class Result : uint32_t
{
    Ready = 1,
    Presented = 2,
    Failed = 3,
    DeviceLost = 4
};
// How a frame crosses: a D3D9 shared texture the host opens by its handle, or a section of
// memory the game side fills from a system-memory copy of the back buffer.
enum class Transport : uint32_t
{
    SharedTexture = 1,
    SharedMemory = 2
};
// Ack::state, OptiScaler's own state read by the host after each Present.
constexpr uint32_t StateMenu = 1, StateFrameGen = 2;

#pragma pack(push, 1)
struct Header
{
    uint32_t magic = Magic, version = Version;
    Kind kind = Kind::Hello;
    uint32_t bytes = 0;
};
// window: the game's HWND. A window handle is the same 32-bit value in both processes.
struct Hello
{
    uint32_t pid = 0, window = 0, luidLow = 0;
    int32_t luidHigh = 0;
};
// handle: a D3D9 shared handle (sign-extended on the host) or the section handle duplicated
// into the host. pitch is the row pitch of the section. window: the game side's child window the
// host presents into.
struct Build
{
    uint64_t generation = 0, handle = 0;
    uint32_t width = 0, height = 0, format = 0, pitch = 0;
    Transport transport = Transport::SharedTexture;
    uint32_t window = 0;
};
struct Frame
{
    uint64_t generation = 0, id = 0;
    uint32_t syncInterval = 0, reserved = 0;
};
struct Ack
{
    Header header;
    Result result = Result::Failed;
    uint32_t error = 0;
    uint64_t generation = 0, frame = 0;
    uint32_t state = 0, reserved = 0;
};
#pragma pack(pop)
static_assert(sizeof(Header) == 16 && sizeof(Hello) == 16 && sizeof(Build) == 40 && sizeof(Frame) == 24);
static_assert(sizeof(Ack) == 48 && offsetof(Ack, state) == 40);
static_assert(std::is_trivially_copyable_v<Build> && std::is_trivially_copyable_v<Ack>);

// The game side's counters, read by tests\dx9_x86_smoke.cpp through the proxy's
// OptiScalerX86Status export. transport is a Transport value; state is the last Ack::state.
struct ProxyStatus
{
    uint32_t hostStarts, faults, bridged, native, menuFrames, blockedInput, state, transport, disabled, width, height;
};

inline uint32_t BodyBytes(Kind kind)
{
    switch (kind)
    {
    case Kind::Hello:
        return sizeof(Hello);
    case Kind::Build:
        return sizeof(Build);
    case Kind::Frame:
        return sizeof(Frame);
    case Kind::Drop:
    case Kind::Quit:
        return 0;
    }
    return UINT32_MAX;
}
inline bool ValidHeader(const Header& h)
{
    return h.magic == Magic && h.version == Version && BodyBytes(h.kind) != UINT32_MAX && h.bytes == BodyBytes(h.kind);
}

struct Handle
{
    HANDLE value = nullptr;
    Handle() = default;
    explicit Handle(HANDLE h) : value(h) {}
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
    ~Handle() { reset(); }
    void reset(HANDLE h = nullptr)
    {
        if (value && value != INVALID_HANDLE_VALUE)
            CloseHandle(value);
        value = h;
    }
    explicit operator bool() const { return value && value != INVALID_HANDLE_VALUE; }
};

// Waits for this I/O, a bounded timeout, or the peer process's death. Cancelled I/O is retired
// before the stack OVERLAPPED and buffer go away.
inline bool Transfer(HANDLE pipe, HANDLE peer, void* data, uint32_t size, bool write, DWORD timeoutMs = IpcTimeoutMs)
{
    auto* at = static_cast<unsigned char*>(data);
    while (size)
    {
        Handle ev(CreateEventW(nullptr, TRUE, FALSE, nullptr));
        if (!ev)
            return false;
        OVERLAPPED ov {};
        ov.hEvent = ev.value;
        DWORD n = 0;
        BOOL ok = write ? WriteFile(pipe, at, size, &n, &ov) : ReadFile(pipe, at, size, &n, &ov);
        if (!ok)
        {
            if (GetLastError() != ERROR_IO_PENDING)
                return false;
            HANDLE waits[] { ev.value, peer };
            const DWORD rc = WaitForMultipleObjects(2, waits, FALSE, timeoutMs);
            if (rc != WAIT_OBJECT_0)
            {
                CancelIoEx(pipe, &ov);
                if (GetOverlappedResult(pipe, &ov, &n, TRUE) && n && n <= size)
                {
                    at += n;
                    size -= n;
                    continue;
                }
                SetLastError(rc == WAIT_TIMEOUT ? ERROR_TIMEOUT : ERROR_BROKEN_PIPE);
                return false;
            }
            if (!GetOverlappedResult(pipe, &ov, &n, FALSE))
                return false;
        }
        if (!n || n > size)
        {
            SetLastError(ERROR_INVALID_DATA);
            return false;
        }
        at += n;
        size -= n;
    }
    return true;
}
inline bool Send(HANDLE p, HANDLE peer, const void* b, uint32_t n, DWORD t = IpcTimeoutMs)
{
    return Transfer(p, peer, const_cast<void*>(b), n, true, t);
}
inline bool Receive(HANDLE p, HANDLE peer, void* b, uint32_t n, DWORD t = IpcTimeoutMs)
{
    return Transfer(p, peer, b, n, false, t);
}
// A request is a post and the collection of its answer. The game side may post a frame, let the game
// run, and collect the answer at its next Present; it collects before anything else uses the pipe.
inline bool Post(HANDLE p, HANDLE peer, Kind kind, const void* body, uint32_t bytes, DWORD timeoutMs = IpcTimeoutMs)
{
    Header h;
    h.kind = kind;
    h.bytes = bytes;
    return ValidHeader(h) && Send(p, peer, &h, sizeof(h), timeoutMs) &&
           (!bytes || Send(p, peer, body, bytes, timeoutMs));
}
inline bool Collect(HANDLE p, HANDLE peer, Kind kind, Ack& ack, DWORD timeoutMs = IpcTimeoutMs)
{
    return Receive(p, peer, &ack, sizeof(ack), timeoutMs) && ack.header.magic == Magic &&
           ack.header.version == Version && ack.header.kind == kind && ack.header.bytes == sizeof(Ack) - sizeof(Header);
}
inline bool Request(HANDLE p, HANDLE peer, Kind kind, const void* body, uint32_t bytes, Ack& ack,
                    DWORD timeoutMs = IpcTimeoutMs)
{
    return Post(p, peer, kind, body, bytes, timeoutMs) && Collect(p, peer, kind, ack, timeoutMs);
}
} // namespace dx9wire
