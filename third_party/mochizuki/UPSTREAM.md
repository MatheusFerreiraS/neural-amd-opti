# mochizuki0323/DLSSNR-AMD

The Vulkan network behind `MochizukiNrRuntime.dll` (`NrBackend=mochizuki`).

- Source: https://github.com/mochizuki0323/DLSSNR-AMD
- Pinned commit: `82560c4b` (2026-10-01, after v0.0.2.4): `windows/` as upstream has it, apart from the local patches
  below
- `linux/package/model-tools/` is still from `791b046` (v0.0.1); the model it makes has not changed
- Licence: MIT, `LICENSE` beside this file

## What is here

Only what the runtime build reads, at the upstream paths:

| Path | Use |
|---|---|
| `windows/src/core/` | the network runtime (`nr::Runtime`), compiled into the DLL |
| `windows/shaders/` | the network and the passes around it, GLSL |
| `windows/build/build_network.py`, `unroll_glsl.py`, `quad_quant_glsl.py` | GLSL to SPIR-V, the upstream way |
| `windows/build/arch/rdna4.sh` | the constants host and shaders share; `tools/build-mochizuki-runtime.cmd` repeats them |
| `linux/package/model-tools/` | makes `dlssnr.bin` from a user's `nvngx_dlssnr.dll` 310.8.0 |

Upstream's `windows/src/pe/` (its own NGX and ReShade hosts, which need the game to run on vkd3d-proton),
`windows/package/` and `windows/vulkan-loader/` are not used. `OptiScaler/dlssnr/backend/mochizuki_runtime/
MochizukiNrRuntime.cpp` hosts the runtime instead, on a Vulkan device of its own, behind the lmxxf C ABI.

## Local patches

1. `windows/src/core/nr_runtime.hpp` and `nr_graph.cpp`: a host's stop flag for a network build. `nr::build_cancel`
   is a thread-local pointer to an `std::atomic<bool>`, set around the `Runtime` constructor with
   `nr::BuildCancelScope`; the graph build's pipeline loop checks it before each pipeline and, once set, writes the
   pipeline cache it has so far and throws `NR build cancelled`. `MochizukiNrRuntime.dll`'s `Destroy` during a cold
   build then returns in under a second instead of waiting out the compile, and the next start keeps what was
   compiled. Null by default.
2. `windows/shaders/rdna4/pipelines.json`: `NR_EDGE_BODIES=0` on the eight pipelines upstream builds with 1
   (`fswin32` and the six folded persistent runs, `fswinp256`). An edge body is a copy of the window body for the
   windows at the image's edge; LLPC compiles each copy, and the folded runs' SPIR-V grows 5 to 7 times.
   - Output byte-identical with and without (1080p runtime dumps).
   - Cold network build with no prewarm list and no driver cache (a fresh exe name): 109 s with them, 23.4 s
     without (164 s the first time ever, before the AMD driver had any of it). With the prewarm list: 49 s and 6.1 s.
   - Network time, three interleaved runs each (runtime's own GPU time): 1080p 8.48-8.52 ms with them,
     8.51-8.70 ms without; 1440p 14.95-15.01 and 14.91-15.15 ms. Within the noise.

Host side (not in this tree): `MochizukiNrRuntime.cpp` enables `bufferDeviceAddress` when the driver offers it and
sets `HostDevice::buffer_device_address`; marks its colour and motion images `SAMPLED`
(`ColourFrame::usage`), so the core reads them in place; and sets `RuntimeConfig::preprocess_unknee` on the linear
path. `nr_runtime.cpp` is compiled with the defines of `rdna4.sh`.

### Dropped on 2026-10-02

The previous pin was `791b046` (v0.0.1) with sixteen patches, most of them parts of upstream's `linux/` tree
ported by hand and tuned for LLPC. Upstream's `windows/` now carries all of it, rebuilt for the AMD Windows driver
(228d3a6, b1419b0) and with the motion and history work of d1185d2:
- the history ping-pong, the preprocess, the colour formats and the post block's 0.7397 weight (old 12, 16) are
  upstream's own;
- the frame kept in its own format and read in place (old 13) is upstream's `ColourFrame::usage` path;
- the alpha pass skipped where it changes nothing (old 14) is upstream's `NR_POST_ALPHA`;
- the pre block's round-to-nearest colour input (old 15) is upstream's `NR_HALF_RTE`, which on LLPC gives the same
  bytes as ours did (RGBA32F frames with values between two halves, compared byte for byte);
- word-wide e4m3 reads, quad quantisation, the V tiles in rows and the work-split knobs (old 2 to 4, 7 to 11) have
  upstream equivalents (`NR_WLOAD_U32`, `NR_WPAIR_U32`, `quad_quant_glsl.py`, `NR_V_LDS_ROW`), tuned by upstream;
- the MSVC cast (old 1) is fixed upstream.

Measured against the previous build (RX 9070 XT, driver of 2026-08-18, the runtime's own GPU time):

| Case | Previous | Now |
|---|---|---|
| 1080p, 1 pass | 9.18-9.34 ms | 8.45-8.52 ms |
| 1080p, 2 / 3 passes | 29.0 / 42.3 ms | 25.8 / 38.3 ms |
| 1707x961 / 1280x720 | 12.7 / 8.6 ms | 11.0 / 7.6 ms |
| 3840x2160 | 55.6 ms | 48.1 ms |
| Cold build with the prewarm list | 7.6 s | 6.1 s |

The output is 49-56 dB from the previous build at 1080p and above (upstream's per-frame noise and the changes
above). A pan with exact motion vectors at 1707x961 and 1280x720 is about ten times steadier frame to frame than
before (mean |out_n - shift(out_n-1)| 0.0007 against 0.0102 and 0.0075), the motion-vector work of d1185d2.

## Build

`tools\build-mochizuki-runtime.cmd [out]` from an MSVC developer prompt, with the Vulkan SDK 1.4.357 or newer
(`VULKAN_SDK`) and Python. It downloads glslang 16.5.0, the version the upstream SPIR-V is pinned to, into
`toolchain\` here, and writes `MochizukiNrRuntime.dll` and `dlssnr-amd\shaders\` to `out`
(default `exports\mochizuki-runtime`).

The shader folder has 48 network, 4 temporal and 8 runtime pipelines and a `noise_field` pass. The prewarm list
(`dlssnr-amd\prewarm\manifest.txt`) must be made again whenever the shaders change: run the runtime once at 720p,
1080p, 1440p and 3840x2160 (the larger ones add `ffwd3w`) and take the list it writes.

## The model

Never shipped with the code. `dlssnr-amd\dlssnr.bin` beside the DLL is made from `nvngx_dlssnr.dll` 310.8.0
(SHA-256 `e16bcf15…6e1fc8e`) by the scripts in `linux/package/model-tools/`: `inspect_nr.py --extract`, the
five `unpack_*.py`, then `pack_model.py --verify model-files.sha256`, as `extract_model.sh` runs them. The result
is 599 entries, 147,756,560 bytes.

## Updating

Copy the paths above from a newer upstream commit, reapply the patches, rebuild, and run the runtime against a
D3D12 frame before shipping it.
