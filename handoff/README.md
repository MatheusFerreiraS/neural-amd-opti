# Handoff — OptiScaler AMD (three-fork merge)

What this repository is, how it got this way, how to build and install it, and what is
still open. Written for whoever picks this up next, including a future me.

Paths below are written generically:

- `<REPO>` — this checkout
- `<GAME_DIR>` — the folder holding the game executable (for many titles that is the
  folder with the `.exe`; for Cyberpunk 2077 it is `.../bin/x64`)

---

## 1. What this is

One OptiScaler build for AMD hardware, merged from three forks that each solved a
different part of the same problem.

| Source | Branch / head at merge | What it contributes |
|---|---|---|
| `MatheusFerreiraS/neural-amd-opti` | `dlss-neural-rendering` @ `7b7220bb` | Base. DLSS-NR plumbing (`nvngx_dlssnr` proxy), native-Vulkan NR, exposure scan, MFG unlock, Streamline packaging |
| `TheAutomatic/dlss-5-amd-project` | `main` @ `2792909e` | `OptiScaler/dlssnr/amd/` — bridge into the danielblnc DLSS-NR-on-AMD HIP runtime, multi-slot scheduling, D3D12 state freeze/restore, native RTGI, AmdLook |
| `burak113/OptiScaler` | `ffx-denoise-experimental` @ `3da4808e` | FSR-RR — the FidelityFX denoiser wired in as a Ray Reconstruction provider, its floor/signal/responsivity controls, fakenvapi work, PT/RR gate bypass |

Upstream of all three: `optiscaler/OptiScaler` → `Dagherbou/OptiScaler_DLSSNR` →
`wilsjo2/OptiScaler-DLSSNR-PreSR-Multipass`. The AMD neural runtime itself is
`danielblnc/DLSS-NR-on-AMD` and is **not** reimplemented here — this repo is the bridge
into it.

**The two denoise routes are alternatives, not a stack.** AMD neural rendering
(`[DlssNr]`) runs the neural model through the AMD runtime; FSR-RR (`[FSR-RR]`) uses
AMD's FidelityFX denoiser as the RR provider. Super-resolution stays FFX/FSR in both.

### Current state

Everything is on `dlss-neural-rendering`. The work was committed on the branch `amd-nr-0.1.0`
(the three-fork merge exactly as it had been staged, then everything section 5 lists after the
merge, then formatting), which pull request #1 merged into `dlss-neural-rendering`. Fixes since
then are committed there directly.

Two GitHub releases, both built by `tools/PACKAGE_RELEASE.ps1`:

- `v0.1.0-amd-nr`: the FidelityFX upscaler, frame generation and denoiser never load from the
  package layout, so FSR falls back to FSR 2 and Ray Reconstruction is greyed out. Do not use it.
- `v0.1.1-amd-nr`: that fixed (section 5, "FidelityFX modules load from the OptiScaler folder").

The build reports itself as `0.1.1-amd-nr`, and the packager writes
`dist/OptiScaler-0.1.1-amd-nr.zip`. Work since then (the lmxxf runtime, effect strength, colour
grade, XeSS multi frame generation) is committed on the branch but not released; see section 5,
"After 0.1.1".

The [AMD-NR ReShade Installer](https://github.com/zmodelerlover/AMD-NR-ReShade-Installer)
(v0.4.0 and later) installs this build as its OptiScaler route. Its payload manifest pins the
`v0.1.1-amd-nr` release zip by URL and SHA-256, and every file it extracts from it by hash. It
takes the runtime 0.3.1 (`b108d640…`) from the Hugging Face dataset `zmodelerlover/amd-nr`,
never from this repository. Never replace an asset on a published tag: every installer would
refuse the new bytes. A new release is a new tag, and then new pins in that installer's
`payload/payload.json` (its `handoffs/HANDOFF-2026-09-23.md` has the steps).

---

## 2. How the merge was done (reproducible)

Useful if you ever need to redo it or pull the sources forward.

The three forks share real ancestry, which allowed proper three-way merges instead of
hand-porting files:

```
optiscaler/OptiScaler
  |- Dagherbou/OptiScaler_DLSSNR
  |    |- 21274132  <- in this fork's history
  |         |- this fork  (dlss-neural-rendering)
  |         |- wilsjo2/main  (PreSR-Multipass)
  |              |- MatheusGViana (vendored as a folder, history lost)
  |                   |- TheAutomatic/dlss-5-amd-project
  |- 65a5f6f9  <- merge base with burak113
```

**burak113** shares linear git history, so it was an ordinary
`git merge --no-commit` (base `65a5f6f9`, 6 conflicts).

**dlss5** had no shared history — it vendored an OptiScaler snapshot as the folder
`OptiScaler-DLSSNR-PreSR-Multipass-main/`. The trick that made this a real merge:

1. Find the `wilsjo2` commit closest to that vendored snapshot by counting differing
   files. It was `6cdb5f7e` ("Document rebuilt v0.6.2 swapchain fixes release").
2. Build a synthetic commit whose tree is the vendored subtree and whose parent is that
   commit, so git can compute a genuine merge base (`21274132`):

```sh
git commit-tree "dlss5/main^{tree}:OptiScaler-DLSSNR-PreSR-Multipass-main" -p 6cdb5f7e \
  -m "dlss5 graft"
```

3. Merge that synthetic commit. 13 conflicts instead of a 126-file manual port.

**One correction is required before step 3.** The dlss5 tree flattened six submodules
(`magic_enum`, `nvapi`, `simpleini`, `spdlog`, `unordered_dense`, `vulkan`) into plain
files. Merging as-is replaces the submodules with loose files. Rebuild the graft tree
with the gitlinks restored:

```sh
export GIT_INDEX_FILE=/tmp/graft.idx
git read-tree "dlss5/main^{tree}:OptiScaler-DLSSNR-PreSR-Multipass-main"
for n in magic_enum nvapi simpleini spdlog unordered_dense vulkan; do
  sha=$(git ls-tree 6cdb5f7e:external "$n" | awk '{print $3}')
  git rm -r -q --cached "external/$n"
  git update-index --add --cacheinfo "160000,$sha,external/$n"
done
git write-tree   # feed this tree to commit-tree instead
```

### Merge reconciliations worth knowing

The non-obvious calls, because a future merge will hit them again:

- **Pass ceiling.** This fork's `kMaxPasses = 30` design was kept; dlss5's
  `MaxPassCount` is now an **alias** of it, so both lineages' arrays size from one
  number. (The AMD backend clamps to 3 separately — see section 5.)
- **The DLSS-NR core.** Both lineages renamed the same ancestor function: this fork to
  `EvaluateAtSeam`, dlss5 to `EvaluateInternal`. They were unified into one function
  whose signature carries both sets of parameters (`sourceIn/destIn/destArrival` from
  here, `forcePost/submissionEpoch` from dlss5). The body references both names for the
  same flag, so a local `const bool beforeUpscale = preUpscale;` alias sits at the top.
- **The multi-pass loop** came from dlss5, because the surrounding auto-merged code
  already used its `effectivePasses` / `PassTuning`. Taking that loop silently dropped
  the only `g_nr.wroteTarget = true` assignment, which would have made
  `PreUpscaleResult()` and `EvaluateStage()` report "nothing written" forever. It was
  re-added at the end of a successful resolve. **Check this if you re-merge.**
- **`swapchainEncoding`** was superseded by burak's richer `outputColorSpace` model.
- **`Shader_Dx11` over-release**: dlss5's fix is correct (those pointers are identity
  caches; the SRV/UAV own the references).
- **State-envelope guard**: this fork's placement won — it sits after the device is
  acquired and releases it plus restores the barrier on the skip path. dlss5's version
  at the top of `Dispatch` was a duplicate and was removed.

### Out-of-tree pieces

dlss5's top-level `tools/`, `tests/`, `shaders/`, `package-amd-presr/`, `.githooks/`
were brought in. Because the vendored folder was flattened to the repo root, the
pre-build step's path had to change from `$(ProjectDir)..\..\tools\` to
`$(ProjectDir)..\tools\` in `OptiScaler.vcxproj`. dlss5 had also deleted
`OptiScaler/shaders/shader_tools/{dxc,fxc,dxv}.exe` and friends; the remaining `.bat`
scripts call them via `%~dp0`, so they were restored.

---

## 3. Building

```sh
git submodule update --init
```

Then MSBuild, `Release|x64`, PlatformToolset **v143**. Visual Studio 2022 provides it
directly; VS 2026 (v18) also ships it — check for
`VC/Auxiliary/Build/Microsoft.VCToolsVersion.v143.default.txt`.

```sh
MSBuild.exe OptiScaler.sln -p:Configuration=Release -p:Platform=x64 -m -v:minimal
```

`OptiScaler.vcxproj` sets `VcpkgEnabled=false`. With a machine-wide `vcpkg integrate install`, the
build otherwise links vcpkg's freetype import library instead of the static
`external/freetype/freetype.lib`, and the DLL then refuses to load in any game folder that has no
`freetype.dll` of its own. Every build made on the development machine before that property was
added had this dependency; Cyberpunk 2077 hid it because a `freetype.dll` sits in its folder. Check with
`dumpbin /dependents`: `freetype.dll` must not appear.

The `clang-format Check` workflow runs clang-format **20** over `OptiScaler/` (except
`OptiScaler/include/`) on every push and pull request. Visual Studio ships a newer clang-format
that breaks some lines differently, so format with the same version before pushing:
`pip install clang-format==20.1.8`, then `clang-format -i --style=file <files>`. Regenerated shader
headers under `precompile/` need it too; the upstream ones are formatted the same way.

The binary is produced as `x64/Release/OptiScaler.dll` and then **moved** by a
post-build step into `x64/Release/a/` together with the rest of the package. Look for
it there, not in `x64/Release/`.

> **Long paths.** `external/FidelityFX-SDK-v2` has deep shader paths. Checking out this
> repo under an already-deep directory fails submodule init with "Filename too long".
> Keep the checkout shallow in the filesystem, or set `git config core.longpaths true`.

---

## 4. Installing (AMD neural path)

Everything goes in `<GAME_DIR>`:

| File | Source | Notes |
|---|---|---|
| `dxgi.dll` | `x64/Release/a/OptiScaler.dll`, renamed | The proxy |
| `OptiScaler.ini` | `x64/Release/a/OptiScaler.ini` | |
| `OptiScaler/` | `x64/Release/a/OptiScaler/` | FFX / XeSS / Agility deps |
| `dlssnr_amd_pass1.dll`, `dlssnr_amd_pass2.dll`, `dlssnr_amd_pass3.dll` | three copies of danielblnc's `version.dll` 0.3.0 or 0.3.1 | One **pass**, not one slot |
| `dlssnr_on_amd_weights.bin` | produced by `dlssnr_on_amd_setup.exe` | Game-agnostic; copy between games freely |
| `amd_fidelityfx_denoiser_dx12.dll` | AMD redistributable | Only needed for FSR-RR. Not built by this repo; loaded by name |

`tools/install-amd-presr.ps1` automates this. It is interactive — do not run it with
`-NonInteractive` in a folder that has other mods, because that branch moves aside any
injection DLL it finds without asking (it would take REFramework's `dinput8.dll` with
it).

### Rules that are easy to get wrong

- **danielblnc's `version.dll` must NOT stay in the game folder.** OptiScaler hosts the
  runtime itself through the `pass` copies. Leaving the author's proxy means two things
  drive the same runtime. The installer moves it aside for this reason.
- **The pass DLLs are identified by SHA-256**, not by name — see
  `OptiScaler/dlssnr/amd/AmdLayout.h` (`kAmd0217`, `kAmd03`, `kAmd031`). An unsupported
  build is refused. All passes must be the same version.
- **Proxy choice**: `dxgi.dll` is the safe default. `d3d12.dll` has a reported
  Streamline conflict that **greys out Cyberpunk 2077's Ray Reconstruction option** —
  see the warning in `setup_windows.bat`. For Vulkan titles use `winmm.dll`.
- Titles that gate Ray Reconstruction on NVIDIA hardware need OptiScaler's spoofing
  (`Dxgi=auto`) for the option to appear at all.

### Minimum config for the AMD neural path

```ini
[Upscalers]
Dx12Upscaler=fsr-rr        ; or ffx/xess — fsr-rr routes RR to the FidelityFX denoiser

[DlssNr]
Enabled=true               ; ships disabled; 'auto' resolves to false
ApplyAfterRR=true          ; the AMD placement: true = after the finished frame, false = before SR
                           ; true is REQUIRED for any title driving Ray Reconstruction
RRWorkingScale=1.0         ; 1.0 = model over every pixel of the finished frame
```

`RunBeforeSR` does nothing on the AMD backend. `ApplyAfterRR` alone places the model there (section
5, "One placement switch").

---

## 5. What was added after the merge

Beyond reconciling the three forks:

**AMD neural rendering after the upscaler.** The AMD bridge only ever substituted the
upscaler's *input* (pre-SR) and refused every post-upscale position outright. FSR-RR
denoises and enlarges in one dispatch, so there is no seam before it — which meant the
model could never follow it. Added:

- `Frame::afterUpscale` plus `guideWidth/guideHeight` (`AmdPreSr.h`)
- Relaxed guide-extent validation for that mode, and depth always resampled there. This
  was tractable because the depth/motion conversion shaders already ratio-map, which
  works for upsampling as well as downsampling.
- `AmdBridge::After()` — reads `NVSDK_NGX_Parameter_Output` instead of `Color` and hands
  its answer back rather than substituting
- `WriteAmdPostAnswer()` in `DlssNr_Dx12.cpp` — writes that answer into the frame with a
  format-converting blit (`OS_Dx12`, the same resampler the supersampling legs use),
  restoring the frame's arrival state

**One pass count for the AMD backend.** It was reading `DlssNrRRPasses` in the post
placement, which left the `Passes` control doing nothing in the only placement an RR
title ever reaches. It now reads `DlssNrPasses` in both.

**One placement switch for the AMD backend.** Every Super Resolution evaluate offers the pass
both seams: first `EvaluateBeforeUpscale`, which the NGX entry and both bridges call unconditionally
for SR, then `EvaluateAfterUpscale`. The AMD branch of `EvaluateAtSeam` only gated the seam after
the upscaler. With "After the finished frame" chosen on an SR title, the backend got the render-size
frame and the output-size frame on the same evaluate. `AmdBridge::Run`'s settling check (a static
width/height/scale) saw a size change on every call, reset its 300 ms timer and invalidated history
each time, so the model never ran, or ran twice where render and output sizes match. Cyberpunk's
`amd_presr.log` recorded 939 of these size flips in about 16 minutes.

Now `ApplyAfterRR` alone decides, and the AMD branch declines the seam not chosen before the backend
sees it. The "Processing point" combo reads and writes only that key. It used to read `RunBeforeSR`,
which the backend never consulted, so a fresh install displayed "After the finished frame" while
running before SR. The amber warning for RR titles in the "before" placement now says the model is
not running; it used to claim the model ran after the frame regardless. Defaults are unchanged:
`ApplyAfterRR=false` is the before-SR placement dlss5 always ran. Checked in Cyberpunk 2077 in both
placements, with and without RR.

**sRGB is the default encoding.** `[DlssNr] AmdEncoding` defaults to 2 (sRGB) instead of 0
(Auto) in `Config.h`, the menu's reset button and the packaged INI. In Cyberpunk 2077 it was the
steadiest option and held highlights best. Auto was removed because it did exactly what Linear (1)
does: neither converts, while sRGB and Gamma 2.2 decode before the model and re-encode after. The
combo offers Linear, sRGB and Gamma 2.2. The stored numbers stay 1/2/3 so existing INIs keep their
meaning, and `AmdBridge.cpp` clamps a leftover `AmdEncoding=0` to Linear.

**"Every-frame" is now "Disable temporal stabilization", off by default.**
`[DlssNr] AmdEveryFrame` sets the runtime's `temporal` byte
(`AmdPreSr.cpp`, `L->temporal = everyFrame ? 0 : 1`), so with it on the model runs without
temporal history. Its only other effect, the post-Execute
wait in `WaitAfterSubmitIfEveryFrame`, is skipped whenever more than one slot is configured (the
default is 3), except during a native rebuild. The old label named neither effect. The menu label,
help text and INI comments now describe it, and the default moved from `true` to `false` (history
on) in `Config.h` and the packaged INI. The INI key keeps its name so existing files still load.

**Dynamic NR resolution (AMD backend).** `[DlssNr] AmdDynamicScale` (default off) and
`AmdDynamicTargetFps` (default 60). While the rendered frame rate stays under the target, the model
scale steps down from the configured one (`AmdModelScale` before SR, `RRWorkingScale` after the
finished frame) through 85, 70, 55 and 40% of it, never under 25% of the frame. The controller is
`OptiScaler/dlssnr/amd/DynamicScale.h`, stepped once per frame from `AmdBridge::Run`, where the
interval between calls is the rendered frame time even with frame generation on. It changes rarely
on purpose: down after 2 s under the target, up after 5 s with 15% headroom, a 10 s hold after any
change and 60 s after a step down before it tries to go back up. Every scale change rebuilds the
runtime's staging and restarts the model's history, and in Cyberpunk's logs NR was off for 0.3 to
5.4 s after each change (median 1.25 s over 31 changes in one session, 2.8 s over 15 in the first
dynamic-scale test). A step must cut the scale by at least 10%, so the 25% floor cannot turn the
last level into a rebuild for almost nothing (from a 50% ceiling it stops at 27.5% instead of
going on to 25%). In that test, going from 50% to 35% raised the rendered frame rate from about 60
to 67-74 fps, and the lower levels gave no consistent gain. A continuous controller would keep the
effect blinking; that cost only goes away if the runtime can run on a subregion of a fixed
allocation, which it cannot today. A frame cap at or under the target hides the headroom, so the
level never climbs back; the menu help says to set the target a little under the cap.

Every host of this runtime has been seen to keep a little more VRAM after each resolution change.
OptiScaler releases everything it allocates per size (slot colours, guide crops, scale scratch,
the encoding output), so the growth is most likely inside the runtime's staging rebuild, which
OptiScaler cannot fix. The controller therefore makes at most 8 changes per session
(`DynamicScale::kMaxChanges`) and then holds its level until the option is turned off and on. Each
"AMD boundary: settings change" line in `amd_presr.log` now ends with the process's local VRAM use
and budget (`VramUsage` in `AmdBridge.cpp`), so the cost per change can be read off a session:
standing still and flipping the NR resolution slider back and forth gives the cleanest numbers,
since the game's own streaming moves the total too.
`tests/amd_dynamic_scale.cpp` covers the controller and runs first in
`tools/test-amd-host-contracts.cmd`.
Exercised once in Cyberpunk 2077 in the "after" placement: it stepped down every 12 s as designed,
never found the headroom to step up, and the session ended cleanly.

**Neural pass meter (AMD backend).** The AMD section of the menu shows "Neural pass: N ms per
frame (M fps on its own)". `AmdBridge::Run` wraps `Backend::Record` in a `GpuTime_Dx12` pair, the
same timestamp helper the NVIDIA path and the upscalers use, so the number is the GPU time of
everything Record puts on the game's list for all passes: input copies, the wait for the model and
the applied result. Readings under 0.1 ms are dropped because a refused Record records nothing, and
the rest is smoothed (10% per frame) so the text stays readable. The fps is 1000 divided by that
time, the rate the model alone could sustain.

**Tabbed main menu.** The two-column table (`RenderMainMenuTable`) became `RenderMainMenuTabs` in
`menu_common.cpp`: Neural, Upscaling, Frame Gen, Image, Interface and Advanced, each tab a list of
the same section functions the table called. Above the tabs, `RenderMainMenuStatusPills` shows one
pill per route into OptiScaler (nvngx.dll or nvngx_dlss/dlssd on DLSS-capable GPUs, nvngx
replacement, libxess, FSR hooks, FSR 3.1, SR and FG), green when present; the "Exists / Doesn't
Exist" lines of the no-upscaler message were removed in its favour. The theme keeps unselected tabs
neutral and the selected one in the accent colour, and the DLSS-NR header opens by default since it
fills the Neural tab. The window still auto-resizes, so its width is the widest of the pill row and
the open tab.

**Tools and packaging after the flatten.** The dlss5 scripts still pointed into the vendored
`OptiScaler-DLSSNR-PreSR-Multipass-main/` folder and at a Visual Studio BuildTools install at a
fixed path. The tests include headers from `OptiScaler/` directly now, the `.cmd` scripts find
Visual Studio through `vswhere`, and `tools/build-release-local.cmd` (all regression tests, then a
Release build into `exports/release-local/`) passes end to end. `tools/PACKAGE_RELEASE.ps1` reads
from the repository root, packages whichever of `x64/Release/a/OptiScaler.dll` and
`exports/release-local/OptiScaler.dll` is newer, adds `amd_fidelityfx_denoiser_dx12.dll` for
FSR-RR, and ships the root `README.md`, which gained an "Installing the release" section. Its
existing tripwire still refuses the danielblnc runtime (`version.dll`, `dlssnr_amd_pass*.dll`,
`dlssnr_on_amd_setup.exe`), the weights and any `nvngx*.dll`, in the staged folder and again in
the finished zip, so users supply those themselves as the README explains.

**FidelityFX modules load from the OptiScaler folder (0.1.1).** The package keeps
`amd_fidelityfx_upscaler_dx12.dll`, `..._framegeneration_dx12.dll` and `..._denoiser_dx12.dll` in
`OptiScaler/`, next to the loader. burak113's `LoadFfxModuleDx12` (in `FfxApi_Proxy.h`) loaded
these by bare name, which only searches the game folder, while the base and dlss5 lineages went
through `Util::LoadProxyLibrary` with the Opti dll path. The merge took burak's version, so in
0.1.0 none of the three loaded: FSR fell back to FSR 2, and on a non-DLSS GPU
`NVNGX_Parameter.cpp` reports `SuperSamplingDenoising.Available` only when the upscaler and the
denoiser are both ready, so the game greyed out Ray Reconstruction. The function now uses
`LoadProxyLibrary` too: Opti dll path first, then by name. Confirmed in Cyberpunk 2077 from a
clean install of the package: all three load from `OptiScaler/`, FSR 3.x runs, and FSR-RR (FSR
Ray Regeneration 1.2.0) creates its context. `amd_fidelityfx_radiancecache_dx12.dll` is not
shipped, so the log carries a harmless "Can't find" warning for it.

**"Upscaler failed to run!" on preset changes.** `FSRDFeatureDx12::UpdateSize` refuses a
frame and requests a rebuild when the render size exceeds the allocation ceiling the
context was created with — a planned bail-out, not a failure, but it raised an error
toast. Now suppressed when the feature itself set `changeBackend`. The underlying churn
was also removed: the ceiling is rounded up to a multiple of 8, because titles and
Streamline round the same ratio differently (1129x635 created vs 1130x636 dispatched
forced a full rebuild — and a temporal history reset — on every preset change).

**Menu honesty pass.** The AMD section had fixed text claiming "before Super Resolution"
whichever placement ran, and the switch that chose it lived in a branch the AMD backend
never reaches (the section returns early). Now: a single **Processing point** combo
(before / after), one scale slider bound to whichever placement is live, a warning when
a Ray Reconstruction title makes the choice moot, the pass slider capped at 3, and the
NVIDIA-chain-only controls (pass-limit lift, per-pass overrides, the second after-RR
pass counter) hidden while the AMD backend is running.

**Diagnostics.** A one-shot probe logging the bound normals resource, its format, the
resolved roughness source, and whether the Streamline `NormalRoughness` tag was
available. Also `[FSR-RR] TaggedNormalRoughness` (**default off**, see section 7).

**Packaging.** `tools/PACKAGE_RELEASE.ps1` rewrites the whole `[DlssNr]` section when
building a release, so it did not know the post-RR keys. `RRWorkingScale` and `RRPasses`
were added there; `Enabled=false` is unchanged.

### After 0.1.1 (committed, not released)

**Second NR runtime: lmxxf.** Ported from TheAutomatic's `release/1.9.0` up to `c127e04b`
(tag `v1.9.1-alpha`, lmxxf upstream 0.29). The upstream folder
`OptiScaler-DLSSNR-PreSR-Multipass-main/` maps to this repo's root; `tools/`, `tests/` and
`third_party/` stay at the root. The next sync diffs from `c127e04b`. Their lmxxf files
(`dlssnr/backend/`, `dlssnr/submission/`, `lmxxf_runtime/`) are best taken whole from upstream,
then re-apply the one local change (`LmxxfBackend::Record` refuses `afterUpscale` frames) and
run clang-format. `third_party/lmxxf/` is vendored MIT source plus gfx1201 modules;
`tools/build-lmxxf-runtime.cmd exports\lmxxf-runtime` builds `LmxxfNrRuntime.dll`, and the
packager ships it with the modules and the top-level `*.hlsl`. The weights
(`native-game-tiled-assets\`) are never shipped.

**Choosing the runtime.** `[DlssNr] NrBackend` (`daniel`, `lmxxf`, `off`, `auto`). The menu's
"NR runtime" combo appears when both runtimes are present and applies on the next launch:
`Backend::Selector` latches the value read at startup, because each backend installs its own
D3D12 hooks when the device is created. `NrBackend`/`LmxxfDiagnostic` used to be skipped on
save whenever they equalled the default, so switching back to daniel never reached the ini;
they are now written as `auto`.

**lmxxf menu.** Its own block: runtime label, neural pass meter, Detail/Colour strength, Debug
view. Everything else in the AMD section belongs to the danielblnc runtime and is hidden. The
meter works on lmxxf because the two timestamps land on either side of the list split, around
the HIP work.

**Effect strength and colour grade (danielblnc).** The runtime's `[DlssNrOnAmd] Scale`
(0.3.1 RVA `0x9ad14`, default 4/128) scales how much of the network reaches the frame: 0 leaves
the frame untouched. It sits where NVIDIA's `style/128` would be but is not the style; tested in
Cyberpunk, 0/128 did nothing and 1/128 and 2/128 were weak. `AmdEffectStrength` (0 to 1 of 4/128,
layout field `scale`, 0.3.1 only). `AmdColourGrade` applies NVIDIA's post-network grade for
Model B (natural: -0.10 EV, contrast -0.25, saturation -10%) or C (cinematic: saturation -15%)
in the AmdLook shader, at full strength. NVIDIA's real style input (network parameter `+0x94`
in `nvngx_dlssnr.dll`) has no known slot in the danielblnc runtime.

**Logging.** `slEvaluateFeature` logged at Info once per frame (5 MB in 17 minutes); it is Debug
now.

**XeSS multi frame generation on AMD.** Ported from
[Coldwood1026/OptiScalerDp4aUnlock](https://github.com/Coldwood1026/OptiScalerDp4aUnlock), a fork
of upstream at `5ee53e38`, which is already in this history: `git diff 5ee53e38 coldwood/master --
OptiScaler OptiScaler.ini`, then `git apply -3`. `XeFGUnlock.h` patches `libxess_fg.dll` so it
reports and accepts more than one interpolated frame, `XeLLUnLock.h` raises the frame count
`libxell.dll` accepts in `xellSetGeneratedFramesCount`, and `XeFGPacing.h` spaces out each
generated frame above 2X. All three patch the loaded image, check every byte first and roll back
on a mismatch. They only know `libxess_fg` build `0x69CB0F4D` and `libxell` 1.3.2 (`0x6A561284`),
the ones in `external/xess`: a newer XeSS SDK there needs new RVAs. The game is told the real
ceiling, and the menu's Auto follows the game's DLSSG multiplier. Kept from our side in the
merge: the generated labels of "Override DLSSG Ratio" (theirs went back to fixed arrays, which
overflow past 6X) and the struct-version guard on `numFramesToGenerateMax`. Their Intel
`ExtraPacing` default had been inserted between an `if` and its `else if` in `getGpuInfo`; it now
sits before the chain.

One addition of ours. `libxess_fg` has no static `libxell` import: it calls
`GetModuleHandleExA("libxell.dll")` and gets the first module of that name. Cyberpunk loads its
own 1.1 before OptiScaler starts, and that copy lacks `xellSetGeneratedFramesCount` and
`xellSetDisplayInfo`, so XeFG refused OptiScaler's XeLL context ("XeLL context is not supported";
the missing export is only logged at debug level). `RedirectAllExports` cannot cover exports the
old copy does not have, and loading ours first does not help either, since the game's copy is
already there. `XeFGProxy::PointXeLLLookupAtOurs` replaces that import in our `libxess_fg` with a
lookup that returns OptiScaler's `libxell`. Tested in Cyberpunk: 2X to 8X switched live, Auto
followed the game. Above 4X it needs VSync or a frame rate cap.

---

## 6. Diagnostics playbook

When FSR-RR "runs but does nothing", the denoiser dispatching successfully proves
nothing — its inputs can be empty while every dispatch reports success.

Set `[FSR-RR] Diagnostics=true` and read `OptiScaler.log` for `[RR_INPUT_PROBE]`. The
probe records every 60th conversion. What to look at:

| Line | Healthy | Meaning when wrong |
|---|---|---|
| `floor/raw crossing` | low % | high % = the residual collapses and the denoiser is handed nothing |
| `specular signal input ... dead(<1e-5)` | low % | 100% = no specular radiance reaches the denoiser |
| `motion ... exactZeroXY` | low % | 100% = no motion, so no temporal accumulation |
| `normals channels: roughness ... exactZero` | low % | 100% = no material guidance |
| `skip signal ... avgLuma` | low | high = the frame is bypassing the denoiser and reaching the screen raw |

Debug views (`Debug View` in the advanced window) render these per pixel;
`DenoiserFraction` and `SkipRawInject` are the two worth knowing.

`docs/fsrd_pipeline_contract.md` is the authoritative description of the FSR-RR
preprocessor, including the energy split it maintains:

```
floor    = isolation * filter(raw)
residual = max(0, raw - floor)
out      = floor + denoise(demod_remod(residual)) + skip
```

**Read the probe's format field carefully.** It reports the *RR-facing* buffer, which is
OptiScaler's own converted allocation, not the title's source texture. Mistaking one for
the other costs a full debugging round-trip.

For the AMD backend, `AmdBridge::Status()` drives the menu status line, and the runtime
version in use comes from `AmdBridge::RuntimeName()`.

---

## 7. Known issues and open items

- **The clang-format check on tag `v0.1.0-amd-nr` stays red.** The tag points at `531c221a`,
  which predates the formatting commit `a12c4a9f`; the branch and its pull request pass. The
  formatting changed no code, so the released DLL is unaffected. It was left alone because moving
  the tag would change what a published release points at. `v0.1.1-amd-nr` is formatted.
- **`[FSR-RR] TaggedNormalRoughness` has never been observed to fire.** It was written
  for a title whose normals binding looked like it carried no roughness; the real cause
  turned out to be an external mod zeroing the ray-tracing buffers. It is left in reach,
  **default off**, because it is unvalidated — not because it is known to help.
- **Motion vectors read 100% zero** in the RR-facing probe during the session that had
  the external mod active. Not re-measured since that mod was disabled. If FSR-RR
  misbehaves on a new title, check this first.
- **The post-upscale enlarge** uses the AMD runtime's internal shader, not a
  user-selectable filter. `Record()` always returns the already-enlarged image, so
  routing it through `OS_Dx12` instead would mean changing that contract.
- **The Vulkan and D3D11 paths** raise the same "Upscaler failed to run!" toast shape
  that was fixed for D3D12. Not changed — FSR-RR is D3D12-only and there was no evidence
  of it firing there.
- **`AmdSlots` reaches 5** but only 3 pass DLLs exist. Slots and passes are *different*
  arrays (`slots[kMaxSlots=5]` vs `runtime[3]`); slots need no extra files.
- The post-upscale placement is **compile-verified and lightly exercised**, not
  systematically tested across titles. Depth and motion are point-upsampled from the
  render grid, so watch for ghosting and shimmer.

---

## 8. Gotchas that cost real time

- **External mods can zero the ray-tracing buffers.** REFramework's "Raytracing Tweaks"
  made FSR-RR receive dead signals while every dispatch reported success. If the inputs
  look impossible, suspect another mod before suspecting the binding.
- **OptiScaler rewrites `OptiScaler.ini` when settings are saved.** Explicit values can
  come back as `auto`. For `[DlssNr] Enabled` that silently means *off*, which reads
  exactly like a regression on the next launch.
- **`dlssnr_amd_pass1-3.dll` are passes, not slots.** The ceiling of three is physical:
  `std::array<HMODULE, 3> runtime` in `AmdPreSr.cpp`.
- **The AMD menu branch returns early.** Anything added to the shared DLSS-NR menu below
  that point is dead code while the AMD backend is installed.
- **Test a release in a clean game folder.** The 0.1.0 packaging bug survived a full test
  session because the development game folder still had FidelityFX DLLs at its root from an
  earlier manual install. Extract the zip into a folder holding only the game's own files, run
  `Setup.bat`, and read `OptiScaler.log` for where each `amd_fidelityfx_*` module loaded from.
- **Every SR frame offers the pass both seams.** Any gate added to one placement needs its
  counterpart on the other, or both reach the AMD backend on the same evaluate and its settling
  check stalls it (section 5, "One placement switch").
- **A feature can decline a frame on purpose.** `changeBackend` set from inside an
  evaluate means "rebuild me", not "I failed".

---

## 9. Map of the code

| Path | What lives there |
|---|---|
| `OptiScaler/dlssnr/amd/` | AMD bridge: `AmdBridge` (NGX-facing entry), `AmdPreSr` (the backend, slots, passes, guides), `AmdLayout.h` (runtime identification by SHA), `HipRuntimeLoad.h`, `RuntimeHostLoad.h` |
| `OptiScaler/shaders/dlssnr/DlssNr_Dx12.cpp` | The DLSS-NR pass itself. `EvaluateAtSeam` is the core; the AMD gate sits near its top |
| `OptiScaler/dlssnr/DlssNr_Menu.cpp` | The Ins-menu NR page. AMD section first, then an early return, then the NVIDIA-only controls |
| `OptiScaler/upscalers/fsr31/FSRDFeature_Dx12.cpp` | FSR-RR: NGX/Streamline input resolution, denoiser context, dispatch |
| `OptiScaler/shaders/fsrd_preprocess/` | The FSR-RR preprocessor (floor filter, conversion, composition) and its input probe |
| `docs/fsrd_pipeline_contract.md` | The FSR-RR pipeline contract — read before touching the preprocessor |
| `tools/` | Installer, packaging, shader-identity generation. Required at build time |
