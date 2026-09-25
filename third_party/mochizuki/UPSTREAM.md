# mochizuki0323/DLSSNR-AMD

The Vulkan network behind `MochizukiNrRuntime.dll` (`NrBackend=mochizuki`).

- Source: https://github.com/mochizuki0323/DLSSNR-AMD
- Pinned commit: `791b04620c34f2b8c3552ccd3be17eaa53a67e6d` (v0.0.1)
- Licence: MIT, `LICENSE` beside this file

## What is here

Only what the runtime build reads, at the upstream paths:

| Path | Use |
|---|---|
| `windows/src/core/` | the network runtime (`nr::Runtime`), compiled into the DLL |
| `windows/shaders/` | the network and the passes around it, GLSL |
| `windows/build/build_network.py`, `unroll_glsl.py` | GLSL to SPIR-V, the upstream way |
| `windows/build/arch/rdna4.sh` | the constants host and shaders share; `tools/build-mochizuki-runtime.cmd` repeats them |
| `linux/package/model-tools/` | makes `dlssnr.bin` from a user's `nvngx_dlssnr.dll` 310.8.0 |

Upstream's `windows/src/pe/` (its own NGX and ReShade hosts, which need the game to run on vkd3d-proton) is
not used. `OptiScaler/dlssnr/backend/mochizuki_runtime/MochizukiNrRuntime.cpp` hosts the runtime instead, on a
Vulkan device of its own, behind the lmxxf C ABI.

## Local patches

1. `windows/src/core/nr_runtime.cpp`, the depth blit's `srcSubresource`: the aspect is cast to
   `VkImageAspectFlags`. MSVC rejects the enum-to-flags narrowing in a braced initializer; GCC accepts it.

## Build

`tools\build-mochizuki-runtime.cmd [out]` from an MSVC developer prompt, with the Vulkan SDK 1.4.357 or newer
(`VULKAN_SDK`) and Python. It downloads glslang 16.5.0, the version the upstream SPIR-V is pinned to, into
`toolchain\` here, and writes `MochizukiNrRuntime.dll` and `dlssnr-amd\shaders\` to `out`
(default `exports\mochizuki-runtime`).

## The model

Never shipped with the code. `dlssnr-amd\dlssnr.bin` beside the DLL is made from `nvngx_dlssnr.dll` 310.8.0
(SHA-256 `e16bcf15…6e1fc8e`) by the scripts in `linux/package/model-tools/`: `inspect_nr.py --extract`, the
five `unpack_*.py`, then `pack_model.py --verify model-files.sha256`, as `extract_model.sh` runs them. The result
is 599 entries, 147,756,560 bytes.

## Updating

Copy the paths above from a newer upstream commit, reapply the patches, rebuild, and run the runtime against a
D3D12 frame before shipping it.
