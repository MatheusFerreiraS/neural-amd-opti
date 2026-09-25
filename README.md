# OptiScaler AMD: neural rendering and FSR ray reconstruction

An OptiScaler fork for AMD graphics cards. Games that ask for DLSS get DLSS 5 neural rendering
(DLSS-NR) through AMD runtimes, or AMD's own FidelityFX denoiser as Ray Reconstruction.
Super Resolution stays FSR.

This README covers only what this fork changes. For OptiScaler itself (upscalers, frame
generation, spoofing, the full ini), see [optiscaler/OptiScaler](https://github.com/optiscaler/OptiScaler).

## What it adds

**AMD neural rendering, three runtimes.** Pick one with **NR runtime** in the menu (applies on the
next launch) or `NrBackend` in `OptiScaler.ini`.

| | danielblnc | lmxxf | mochizuki (experimental) |
|---|---|---|---|
| Runtime | [DLSS-NR-on-AMD](https://github.com/danielblnc/DLSS-NR-on-AMD) 0.3.1 or 0.3.0, as `dlssnr_amd_pass1-3.dll` | [lmxxf's open-source HIP port](https://github.com/lmxxf/dlss5-on-amd-9070xt-porting), `LmxxfNrRuntime.dll` built here | [mochizuki0323's Vulkan port](https://github.com/mochizuki0323/DLSSNR-AMD), `MochizukiNrRuntime.dll` built here, not in the package |
| Weights | `dlssnr_on_amd_weights.bin` | `native-game-tiled-assets\` | `dlssnr-amd\dlssnr.bin`, made from your own `nvngx_dlssnr.dll` 310.8.0 |
| GPUs | RDNA3 and RDNA4 with HIP 7 | RDNA4 (gfx1201 modules) | RDNA4 (Vulkan FP8) |
| Where it runs | Before Super Resolution, or on the finished frame (the only choice for Ray Reconstruction titles) | Before Super Resolution, render resolution up to 1080p | Before Super Resolution, on a Vulkan device of its own beside the game's DirectX 12 |
| Controls | Passes 1-3, NR slots, wait mode, encoding (sRGB default), NR resolution with dynamic scaling, effect strength, lighting and structure | Passes 1-3, temporal history and smoothing, detail and colour strength, debug view | Passes 1-3, NR resolution, dynamic resolution, temporal history, detail and colour strength, style, intensity, tone, structure, skin, highlight guard |

No runtime's weights are included; all three are derived from NVIDIA's model.

**mochizuki is experimental.** This package has only its OptiScaler side: `NrBackend=mochizuki`, the
`Mochizuki*` keys and its section in the Neural tab. The runtime (`MochizukiNrRuntime.dll` and its
`dlssnr-amd\` folder) is not in it, and neither is the model, `dlssnr.bin`. The AMD-NR installer
sets up both. To do it by hand, build the runtime with `tools\build-mochizuki-runtime.cmd` and make
the model as `third_party/mochizuki/UPSTREAM.md` ("The model") describes. On an RX 9070 XT the
network takes about 9 ms per frame at 1080p. The first start in each game compiles its pipelines,
and frames pass through without NR until it is done. With a prewarm manifest
(`dlssnr-amd\prewarm\manifest.txt`) they compile in parallel. The build above does not make one:
without it they compile one after another (about 22 s in the test harness), and the runtime then
writes it.

**On danielblnc and lmxxf:** a neural pass meter (GPU milliseconds per frame and the fps the model
alone could reach), a **Colour grade** option with NVIDIA's Model B (Natural) and Model C
(Cinematic) grading, and a menu organised in tabs with status indicators.

**FSR-RR.** AMD's FidelityFX denoiser as the Ray Reconstruction provider, with its own
signal and responsivity controls. Set `Dx12Upscaler=fsr-rr` under `[Upscalers]`. The two denoising
routes are alternatives: pick one per game.

**XeSS multi frame generation.** Intel's XeFG generates up to 7 frames per rendered one (8X) on
AMD cards too. Intel only allows it with its own driver build, so `libxess_fg.dll` and
`libxell.dll` are patched in memory; the files on disk stay as shipped. Set `FGOutput=xefg`, and
`FGInput=dlssg` in games with DLSS frame generation. The **MFG** combo in the XeFG section follows
the game's multiplier (**Auto**) or takes 2X to 8X. Above 4X, turn on VSync or cap the frame rate.
When a game ships an older `libxell.dll` of its own (Cyberpunk 2077 does), XeFG is pointed at
OptiScaler's copy.

## Installing

1. Extract the release next to the game's executable (for Cyberpunk 2077, `bin\x64`).
2. For danielblnc, put its `version.dll` or `dlssnr_on_amd_setup.exe` next to `Setup.bat`, plus
   `dlssnr_on_amd_weights.bin` if you have it. For lmxxf, put `native-game-tiled-assets\` next to
   `Setup.bat` or in the game folder; the runtime, modules and shaders ship in the package. For
   mochizuki, copy `MochizukiNrRuntime.dll` and its `dlssnr-amd\` folder (`shaders\`, your
   `dlssnr.bin`, and `prewarm\manifest.txt` if you have one) into the game folder after Setup,
   and set `NrBackend=mochizuki`.
3. Close the game and run `Setup.bat`. Choose the game folder and a proxy name (`dxgi.dll` is the
   usual one). `Uninstall_OptiScaler_NR.bat` in the game folder removes it again.
4. In game, press **Ins**, open the **Neural** tab and turn on **Enable NR**. In a game that uses
   Ray Reconstruction, set **Processing point** to **After the finished frame** (danielblnc only).

The [AMD-NR ReShade Installer](https://github.com/zmodelerlover/AMD-NR-ReShade-Installer) (v0.5.0
or later) does all of this in one step for DirectX 12 games, lmxxf weights included, and lets you
pick which release goes in. It does not set up mochizuki yet.

## Settings

All in `OptiScaler.ini`, section `[DlssNr]`, and in the **Neural** tab of the overlay.

| Key | What it does |
|---|---|
| `NrBackend` | `daniel`, `lmxxf`, `mochizuki`, `off` or `auto` (danielblnc when both danielblnc and lmxxf are present; never mochizuki) |
| `ApplyAfterRR` | Run on the finished frame instead of before Super Resolution (danielblnc; lmxxf and mochizuki always run before) |
| `AmdEncoding` | 1 Linear, 2 sRGB (default), 3 Gamma 2.2 |
| `AmdEffectStrength` | Share of the network's effect, 0 to 1 (danielblnc 0.3.1) |
| `AmdColourGrade` | 0 none, 1 natural, 2 cinematic |
| `AmdSlots`, `AmdGraphicsWait` | Frames in flight and wait mode (danielblnc) |
| `AmdDynamicScale`, `AmdDynamicTargetFps` | Lower the NR resolution while under the target frame rate |
| `LmxxfFitLarge` | Let lmxxf take a render resolution above 1080p (can hitch; off by default) |
| `LmxxfTemporal` | Each lmxxf pass reads its own result from the previous frame, moved by the game's motion vectors (on by default) |
| `LmxxfSmoothStrength`, `LmxxfSmoothThreshold` | With temporal history, blend the result toward the previous frame's where they differ by less than the threshold (in 1/255): 0.8 and 10 by default, strength 0 is off |
| `MochizukiTemporal`, `MochizukiHistoryStrength` | mochizuki's history from the previous frame, moved by the game's motion vectors, and how much of it is blended in (0 to 1). On and 1 by default; without the key, `LmxxfTemporal` decides |
| `MochizukiDetailStrength`, `MochizukiColourStrength` | How much of the network's detail (0 to 2) and colour change (0 to 4) reaches the frame, exaggerated above 1. Detail 1 and colour 0 by default: colour 0 keeps the game's own colour at the network's brightness |
| `MochizukiPasses`, `MochizukiModelScale` | Network runs per frame (1 to 3) and its resolution as a share of the render resolution (0.25 to 1, in steps of 0.05); 1 and 1 by default. A change rebuilds the network |
| `MochizukiIntensity`, `MochizukiStyle`, `MochizukiLocalTone`, `MochizukiLocalStructure`, `MochizukiSkinStructure`, `MochizukiAutoMask` | mochizuki's model controls: intensity 0 to 2 (1), style 0 standard, 1 natural, 2 cinematic (0), local tone and structure 0 to 2 (1), skin structure -1 to 2 (-1 follows local structure), automatic skin mask (on) |
| `MochizukiMaxRatio` | Highlight guard: the most the result may multiply or divide a pixel's brightness by, 1 to 8 (2) |
| `MochizukiWhitePoint`, `MochizukiLinearInput` | For linear colour, the brightness treated as white (0.01 to 100, 1); whether the colour is linear: 0 auto (float formats), 1 on, 2 off. A `LinearInput` change rebuilds the network |
| `MochizukiApplyModel` | `false` runs the network at full cost but shows the original frame (on by default) |
| `MochizukiDynamicResolution` | For games that change their render resolution as they run: `auto` (default) keeps one network, built for the largest resolution seen, once the render resolution drops below the colour buffer, and the frame moves inside it; `exact` rebuilds the network for every resolution (a second or two without NR each time); `always` is `auto` from the first frame, also across buffer reallocations |
| `MochizukiPass2*`, `MochizukiPass3*` | `Style`, `Intensity`, `LocalTone`, `LocalStructure`, `SkinStructure` and `AutoMask` for passes 2 and 3; `auto` inherits pass 1, with local tone 0 |

mochizuki's tuning comes only from its own `Mochizuki*` keys, so it and the other runtimes' stay apart.
One leftover: without `MochizukiTemporal`, `LmxxfTemporal` decides.

Multi frame generation reads `[XeFG]`: `InterpolationCount` (`auto` follows the game, 1 to 7 is
2X to 8X), `UnlockMFG`, `MaxInterpolatedFrames` (default 7) and `ExtraPacing`.

## Building

Visual Studio with the v143 toolset, `Release|x64`, after `git submodule update --init`. Keep
`tools/` beside the solution: a pre-build step runs from it. `tools\build-lmxxf-runtime.cmd
exports\lmxxf-runtime` builds the lmxxf runtime, `tools\build-mochizuki-runtime.cmd
exports\mochizuki-runtime` the mochizuki one (from an MSVC prompt, with the Vulkan SDK 1.4.357 or
newer and Python), and `tools/PACKAGE_RELEASE.ps1` makes the release zip. CI checks formatting with
clang-format 20 under `OptiScaler/`.

## Credits

- [OptiScaler](https://github.com/optiscaler/OptiScaler) by cdozdil and contributors, the base of
  everything here. This project uses [FreeType](https://gitlab.freedesktop.org/freetype/freetype)
  under the [FTL](https://gitlab.freedesktop.org/freetype/freetype/-/blob/master/docs/FTL.TXT).
- [Dagherbou/OptiScaler_DLSSNR](https://github.com/Dagherbou/OptiScaler_DLSSNR), which first put
  DLSS-NR into OptiScaler, and
  [wilsjo2/OptiScaler-DLSSNR-PreSR-Multipass](https://github.com/wilsjo2/OptiScaler-DLSSNR-PreSR-Multipass),
  which added the pre-SR multipass pipeline.
- [MatheusGViana/dlss-5-amd-project](https://github.com/MatheusGViana/dlss-5-amd-project) and
  [TheAutomatic/dlss-5-amd-project](https://github.com/TheAutomatic/dlss-5-amd-project): the AMD
  bridge into the runtime, multi-slot scheduling, the D3D12 state freeze and restore, the lmxxf
  integration and its same-frame submission layer.
- [burak113/OptiScaler](https://github.com/burak113/OptiScaler/tree/ffx-denoise-experimental): FSR-RR.
- [Coldwood1026/OptiScalerDp4aUnlock](https://github.com/Coldwood1026/OptiScalerDp4aUnlock): the
  XeFG multi frame generation unlock, its frame pacing, the `libxell` ceiling patch and the
  multiplier that follows the game. YiBoF contributed its Intel Arc support.
- [danielblnc/DLSS-NR-on-AMD](https://github.com/danielblnc/DLSS-NR-on-AMD): the AMD neural runtime
  this bridges into. It is not reimplemented here.
- [lmxxf/dlss5-on-amd-9070xt-porting](https://github.com/lmxxf/dlss5-on-amd-9070xt-porting) (MIT):
  the open-source HIP port, vendored under `third_party/lmxxf`.
- [mochizuki0323/DLSSNR-AMD](https://github.com/mochizuki0323/DLSSNR-AMD) (MIT): the Vulkan port,
  vendored under `third_party/mochizuki`.
- [RenoDX](https://github.com/clshortfuse/renodx) by clshortfuse: the colour composition in
  `dlssnr.hlsl`.

NVIDIA's DLSS binaries and model weights are not redistributed. FidelityFX, XeSS and the DirectX
Agility SDK keep their own licences, in `Licenses/`.

## License

GPL-3.0, as OptiScaler. See `LICENSE`.
