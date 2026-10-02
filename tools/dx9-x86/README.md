# OptiScaler for 32-bit D3D9 games

OptiScaler.dll is x64 only, and so are the NR runtimes and XeFG, so a 32-bit game cannot load any of
them. This folder holds a pair that brings the 64-bit OptiScaler to such a game without changing it:

- `proxy.cpp` builds a 32-bit `d3d9.dll`. It forwards every export to the system `d3d9.dll`, starts
  the host and, at each Present of the device's own swap chain, copies the back buffer to the host.
- `host.cpp` builds `OptiScalerDx9Host.exe` (x64). It loads `OptiScaler.dll` from its own folder
  before it creates anything, then creates a D3D11 device on the game's adapter and a swap chain on a
  child window of the game's window. Each frame it receives is copied into that swap chain and
  presented, so OptiScaler sees an ordinary D3D11 program: final-image NR (`PresentWithoutUpscaler`),
  XeFG through its D3D11-to-D3D12 swap chain and the full menu all run as they do in a D3D11 game,
  from the same `OptiScaler.ini`.
- `dx9_wire.h` is the pipe between the two. The pipe I/O is the AMD NR ReShade add-on's x86 bridge
  transport (`reshade/core/x86bridge/bridge_io.h`).

While the host presents, the game's own Present is skipped: one presenter owns the picture for the
whole session, whatever NR, frame generation or the menu do, so turning them on and off never hands
the window from one swap chain to another.

## The child window, and a host that is lost

The host never presents into the game's window itself. A flip swap chain made by another process
that ends without releasing it leaves its window unable to take any other flip swap chain (measured:
`E_ACCESSDENIED` ten seconds after the presenting process was ended), and the game's own Present no
longer shows there either: the picture stays frozen while the game runs on. So the proxy makes a
child window covering the game's client area, on the game window's own thread, and the host presents
into that. The child takes no input (it is disabled and transparent to hit testing), so the mouse and
keys still reach the game's window, and it follows the game window's size.

When the host does not answer, reports its device lost, refuses a new back buffer size or dies, the
proxy closes the pipe (the host releases its swap chain and leaves; one still there after five
seconds is ended), destroys the child window, presents that frame natively, and starts a new host
with a new child window: a second later, or at once when the host refused a new size (XeFG cannot
resize while anything holds a back buffer, so a Reset to a new size with frame generation on costs a
new host). After four failures in a row, each before the host presented 600 frames, the session
stays native. The host starts on a thread of its own; the game presents natively until it is up.

## How a frame crosses

- D3D9Ex devices (Source engine games such as Half-Life 2): the back buffer is copied into an
  A8R8G8B8 render target created with a shared handle, which the host opens directly. The game side
  waits for that copy on the GPU before it hands the frame over.
- Plain D3D9 devices, which cannot share a texture: the copy is read back to system memory and
  written into a section of memory the host maps; the host uploads it.

With `Pipelined=true` (the default) the game side hands a frame over and returns to the game,
which renders its next frame while the host presents; the host's answer is collected at the next
Present, before the shared copy is written again. `Pipelined=false` waits for the host at every Present.
At interval 0 the host presents with tearing allowed, so a game without vsync is not held to the
display's rate.

The game provides colour only. Motion comes from FidelityFX Optical Flow and depth is synthetic,
exactly as in OptiScaler's D3D11 final-image route.

## The menu

The host draws OptiScaler's menu into its swap chain. OptiScaler's input layer sees that the window
belongs to another process and reads the keyboard and mouse itself (polled keys and cursor, raw mouse
through an input sink). While the menu is open, the proxy holds the game's mouse and keyboard
messages, `WM_INPUT`, and `GetCursorPos`/`SetCursorPos` from the game, so the camera does not move
under the menu. Games that read the mouse through DirectInput still see it.

## Install

Put these where the game loads `d3d9.dll` from (for Half-Life 2 that is `bin`):

```
d3d9.dll                      the 32-bit proxy
OptiScaler64\
  OptiScalerDx9Host.exe
  OptiScaler.dll              the 64-bit OptiScaler build
  OptiScaler.ini
  dlssnr_amd_pass1.dll        the danielblnc runtime, and its dlssnr_on_amd_weights.bin
  libxess_fg.dll, libxell.dll for XeFG
```

Never put a `version.dll` in `OptiScaler64`: the host would load it as the system's. `OptiScaler.ini`
needs at least:

```ini
[DlssNr]
Enabled=true
PresentWithoutUpscaler=true

[FrameGen]
Enabled=true
FGInput=upscaler
FGOutput=xefg
```

The `[FrameGen]` part is only for XeFG. The proxy reads its own keys from the same file:

```ini
[Dx9X86]
; false leaves the game alone (the proxy only forwards)
Enabled=true
; A fullscreen D3D9 device owns the display and the host's swap chain would never be seen, so a
; fullscreen request is turned into a window of the same size. false keeps fullscreen.
ForceWindowed=true
; true sends frames through memory even when the device can share a texture (for testing)
SharedMemory=false
; true lets the game render its next frame while the host presents this one; false waits for the
; host at every Present
Pipelined=true
```

Logs, all in `OptiScaler64`: `dx9-x86.log` (game side), `dx9-host.log` (host), `OptiScaler.log`
(with `LogToFile=true`; the one from the host before is kept as `OptiScaler.previous.log`) and
`amd_presr.log` (NR).

## Build and test

`tools\build-dx9-x86.cmd` builds the proxy, the host and the x86 smoke program into
`exports\dx9-x86`. `tools\test-dx9-x86.cmd` builds them, stages `exports\release-local\OptiScaler.dll`
with a runtime and XeFG, and runs `tests\dx9_x86_smoke.cpp` through every case under the GPU lock:
native D3D9, the bridge on D3D9 and D3D9Ex, the memory transport, NR with its toggle key, NR with
XeFG with the frame generation key and a device Reset to a new size, NR and frame generation both
turned off, the menu with mouse input held from the game, a Reset without frame generation, the host
ended mid-run with and without frame generation, and frame times at 1920x1080.
