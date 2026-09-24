# NR backend selector

How OptiScaler picks one of the two AMD NR runtimes, and how the lmxxf backend records its work.
Both runtimes ship since 0.2.0-amd-nr.

## Choosing the runtime

`[DlssNr] NrBackend` is read once at launch (`Selector.cpp`). Each backend installs its own D3D12
hooks when the device is created, so the menu's "NR runtime" combo applies on the next launch.

| `NrBackend` | Active backend |
|---|---|
| `daniel` | `DanielBackend`, runtime `dlssnr_amd_pass1-3.dll` |
| `lmxxf` | `LmxxfBackend`, runtime `LmxxfNrRuntime.dll` |
| `off` or `none` | None: no AMD Record, the original colour goes to SR |
| `auto`, empty or missing | lmxxf when `LmxxfNrRuntime.dll` sits beside OptiScaler and `dlssnr_amd_pass1.dll` does not, otherwise daniel |
| Anything else | daniel |

`AmdBridge::HasFiles` looks for the active backend's runtime DLL. The `submission/` hooks are armed
only while lmxxf is the active backend (`SubmissionHooksWanted`).

`LmxxfBackend::Record` refuses frames after the upscale (`afterUpscale`), so lmxxf runs only before
Super Resolution. This is a local change on top of TheAutomatic's lmxxf files.

## Toolchain

`LmxxfNrRuntime.dll` is MinGW, built by `tools\build-lmxxf-runtime.cmd exports\lmxxf-runtime` from
`lmxxf_runtime/` and the vendored source in `third_party/lmxxf` (pinned in its `UPSTREAM.md`).
OptiScaler (MSVC) talks to it through `LmxxfNrApi.h` only. `tools/PACKAGE_RELEASE.ps1` ships it with
the gfx1201 modules and the top-level shaders.

- Modules: `lmxxf-modules` beside the DLL (or `LMXXF_MODULES_DIR`).
- Weights, never shipped: the first of `native-game-tiled-assets\`, `lmxxf-weights\` and the folder
  named in `lmxxf-weights-dir.txt`, all beside OptiScaler, then `LMXXF_WEIGHTS_DIR`. These are the
  tiled assets; the 0.24.2 `HIP/` folder is never read.

## Record sandwich (fail-closed)

`LmxxfBackend::Record`: PrepareFrame → **require** `ILogicalCommandList` proxy → RecordInputs → Split → RecordOutputs → `SetPendingEnqueue(EnqueueHip)`.
Non-proxy / Split fail → `CancelUnsubmitted`, return **nullptr** (ordinary SR). No Record-time EnqueueHip.
The runtime owns one job per session. If another Evaluate arrives before the previous
game list is submitted, Record returns the original Color for that Evaluate and
keeps the earlier job intact. Cancelling the earlier job after its output has
already been handed to SR would invalidate the recorded continuation.
`Pending()` is a process-wide singleton because the product has one active NR
backend. Its slot is keyed by the logical list supplied to the between callback;
submitting other game or FG lists does not consume it. Multiple simultaneous NR
backends would require a per-backend or per-session registry.

## Product Execute

When `ExpandEnabled()`, `AmdBridge::ExecuteBatch` always `ExecuteExpanded` (QI proxy → `ExecuteOnWithBetween`).
`ExecuteExpanded` forwards the current logical list as an explicit callback argument.
If `EnqueueHip` fails, the callback reads the runtime's thread-local error on the
submission thread and retains it for the next `PrepareFrame` failure log. `Retire`
and `ResetHistory` failures are also logged at their call sites. This preserves
the original error when the runtime subsequently reports only a poisoned session.
`PendingListIndex` stays **-1** (Daniel-only batch isolation); lmxxf intentionally does not use it.

## Admission and continuation

- Split refused on: open query at the cut / invalid query scope / predication / enhanced barrier / open split barrier / aliasing / render pass / RTAS / meta / root·sample overflow → ordinary SR. Completed queries and timestamp EndQuery remain eligible.
- Continuation seed: viewport/scissor/topology/PSO/rootsig/heaps/blend/stencil/OM + IA/SO/VRS/strip-cut/view-mask + RootBindState + sample positions + depth bounds.
- `ResourceStateBook::ApplyExecuteDecay` updates **our book** only; it does not rewrite game barriers.

## Known limits

- `QueryCapabilities` always reports `hip_ready=0`. Whether HIP loaded is in `OptiScaler.log`.
- `RootBindState` tracks up to 64 root parameters and 64 root constants; past that the split is refused.
- Execute decay has not been checked against the D3D12 debug layer in a live game.
- While the hooks are armed, lists created with `CreateCommandList1` are wrapped as well.
- A render resolution above 1080p needs `LmxxfFitLarge` and can hitch.
