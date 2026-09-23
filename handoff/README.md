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

The merge and everything after it sit **staged and uncommitted** on
`dlss-neural-rendering`. Zero commits were created. `git diff --cached` against
`origin/dlss-neural-rendering` is the whole body of work.

Staged content survives `git checkout -- .` but **not** `git reset --hard`.

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
ApplyAfterRR=true          ; REQUIRED for any title driving Ray Reconstruction
RunBeforeSR=false
RRWorkingScale=1.0         ; 1.0 = model over every pixel of the finished frame
```

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

- **Nothing is committed.** See section 1.
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
