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
2. `windows/shaders/rdna4/fswin_t.comp`: word views for the e4m3 weight reads and for the persistent runs'
   stores. It speeds up the Windows (LLPC) build, the output is byte-identical, and it is a candidate upstream PR.
   - Weights: `NR_C32_WEIGHT`, `NR_WEIGHT_PAIR` and the packed expand read the packed records through a `uvec2`
     view of binding 4 (`wexpand_u2`) instead of `fe4m3vec4`.
     - The loads were already dword-wide. But LLPC put an identity `v_perm_b32` (a register copy) on every weight
       fragment: 3281 in the 32 pipelines the runtime creates, 611 of them in `g_fswinpds256`. Read as words,
       218 are left, 158 of them in the two pre blocks that keep the old read.
     - The index is counted in words (`base/8`). That is exact because every weight matrix starts on a 256-byte
       boundary (`put(..., 256)` in `nr_graph.cpp`). One `uvec4` per pair record keeps the copies.
     - `NR_C32_WORDS` defaults to 1, and to 0 in the fused image-input pre block (`NR_FUSED_IMAGE_INPUT`). There
       the view takes 192 -> 201 VGPRs (8 -> 7 waves per SIMD) and the block ran 5% slower.
   - Stores: a persistent run (`NR_PERSIST`, coherent arena) stored one byte at a time. Two stores now go through
     uint views:
     - the window epilogue's fragment store (`NR_STORE_ACC_COL_ACT`, binding 0): 64 `buffer_store_b8` become 8
       `buffer_store_b64` per pipeline;
     - the DS projection's store (`act_x4v`, binding 5): 32 `buffer_store_b8` become 8 `buffer_store_b32`.
   - Unchanged: the other e4m3 loads (the `coopMatLoad` A fragments, the post-blend read, `attn.comp`,
     `ffwd3_t.comp`) already compile to dword loads with no instruction before the WMMA.
   - Changed SPIR-V:
     - every `fswin_t.comp` network pipeline except `fswinimagepreds32`: 22 pipelines, which
       `build_network.py --check` lists;
     - `temporal/temporal_post_fp32`.
   - The output is byte-identical. Checked on 2026-09-25 (RX 9070 XT, driver 26.8.1) by running the old and the new
     SPIR-V on the same input. These checks run 18 of the 22 changed network pipelines and `temporal_post_fp32`:
     - M0's EQUIV set (the runtime at 720p, 1080p and 1440p) and `nr_graph --out-image` at 1080p and 1440p, 3 runs
       each. They run the persistent path: `fswin32`, `fswindsp32/64/128`, `fswinfusedup32/64/128`,
       `fswinimagepost32`, `fswinp64/128`, `fswinpds256`, `fswinpup256` and `temporal_post_fp32`.
     - 3840x2160, where the C=256 persistent runs fold in neither the downsample nor the upsample layer:
       `nr_graph --out-image` (3 runs each) and the runtime (`mz_bench` dumps). They add `fswinp256`, `fswindsp256`
       and `fswinfusedup256`.
     - 1080p without persistent runs: `nr_graph --no-persist` (3 runs each) and the runtime with `NR_NO_PERSIST=1`.
       They add `fswin64`, `fswin128` and `fswin256`.
     - `fswinds32/64/128/256` are compiled but never dispatched. `NR_DS_FUSE=15` turns every downsample layer into
       `fswindsp<C>`. The one switch that keeps `fswinds<C>`, `nr_graph --ds-tok-raster`, also needs a `gemmds`
       pipeline, which this build does not make. The only part of this change that `fswinds<C>` compiles is the
       weight read, and `fswindsp<C>`, checked at every width, compiles the same code.
   - Timing, against the previous build:
     - the runtime's D3D12-queue gap (`mz_timing`, 5 interleaved pairs) went from 10.39 to 9.96 ms at 1080p and
       from 17.40 to 16.86 ms at 1440p;
     - standalone `nr_graph` went from 9.88 to 9.50 ms at 1080p, from 16.00 to 15.45 ms at 1440p and from 35.92
       to 35.24 ms at 3840x2160;
     - `fswinpup256` got 30% faster and `fswinpds256` 12% faster.
3. `windows/shaders/rdna4/pipelines.json`: two work-split knobs retuned for LLPC. Each changes only how the work
   is split between waves, so the output is byte-identical. Neither knob is in `shader-constants.txt`, so the host
   is unchanged.
   - `fswin32`: `NR_FWAVES` 1 -> 2, so two waves share a window.
     - It is the one pipeline where `NR_FWAVES` is live. `NR_HWAVES=1` makes it inert at C >= 64, and the DS,
       fused-upsample and `NR_K_REGS` bodies need 1 at C=32.
     - Upstream ships 1 (tuned on RADV; see the `NR_FWAVES` notes in `fswin_t.comp`).
     - On LLPC, 2 waves take the pipeline from 181 to 125 VGPRs and from 8 to 10 waves per SIMD. Its 6 dispatches
       went from 1021 to 952 us at 1080p (-6.8%) and from 1767 to 1707 us at 1440p (-3.4%).
     - 4 waves (91 VGPRs) is faster than 1 but slower than 2: -4.9%.
   - `fswinfusedup128`: `NR_EXPAND_GROUP` 4 -> 2, the value the persistent C=128 body `fswinp128` already uses.
     VGPRs stay at 244. Changed alone, the dispatch went from 197.6 to 193.6 us at 1080p (-2.0%, 5 runs; -2.1% and
     -2.9% in two other batches) and from 336.2 to 317.9 us at 1440p (-5.4%). Beside the `fswin32` change its
     1080p gain moved between sessions, from -0.6% to -9.6%; it was -4.5% and -7.6% at 1440p and -17.4% at
     3840x2160.
   - Rule: a knob was adopted only when it was byte-identical and at least 2% faster on its own dispatches at both
     1080p and 1440p. The sweep used `nr_graph --per-layer`, run interleaved with the unchanged build. The tool is
     `tools/sweep_mochizuki_knobs.py`.
   - Swept on 2026-09-25 (RX 9070 XT, driver 26.8.1) and not adopted. Every variant that built was byte-identical;
     the times are at 1080p.
     - `fswinpds256`, `NR_EXPAND_GROUP` 4 -> 2: 256 -> 222 VGPRs, the 16 B of scratch gone, and 5 -> 6 waves per
       SIMD. It is still 12.7% slower: each B fragment out of LDS now feeds 2 MMAs instead of 4 (`ds_load`
       298 -> 362).
     - `NR_EXPAND_GROUP` 4 -> 2 on `fswinfusedup64` (+0.6%) and `fswindsp128` (+0.8%).
     - `NR_EXPAND_GROUP` 2 -> 4 on:
       - `fswinp64`: -1.3%, and -0.3% alone over 5 runs, below the bar;
       - `fswinp128`: +30%, at 219 -> 255 VGPRs;
       - `fswindsp64`: +0.6%;
       - `fswinpup256`: +46%, at 192 -> 213 VGPRs.
     - `fswinp64` with the token split (`NR_HWAVES=0`, `NR_FWAVES=2`) does not compile. The paired weights exist
       only on the head-split path at C >= 64 (`fswin_t.comp`, "paired weights require the head-split path"), and
       the host packs every C >= 64 fswin matrix in pairs (`weight_layout 3`).
   - Checked by running the old and the new SPIR-V on the same input:
     - `nr_graph --out-image` at 1080p, 1440p and 3840x2160, 3 runs each;
     - the runtime against M0's EQUIV set (720p, 1080p and 1440p).
   - Timing, against the previous build. The gain moved between sessions, so each figure is the range over the
     sessions run:
     - the runtime's D3D12-queue gap (`mz_timing`, median of 5 interleaved pairs, 3 sessions): -0.04 to -0.09 ms
       at 1080p (9.69 -> 9.60 ms in the first) and -0.06 to -0.08 ms at 1440p (16.51 -> 16.35 ms in the first);
     - standalone `nr_graph --per-layer`, interleaved: -0.043 to -0.065 ms at 1080p (3 sessions; 8.963 -> 8.898
       ms in the first), -0.058 to -0.061 ms at 1440p (2 sessions; 15.208 -> 15.150 ms) and -0.116 ms at
       3840x2160 (1 session; 33.694 -> 33.578 ms);
     - cold pipeline compilation is unchanged: network ready after 22.2 and 22.0 s, both before and after.
4. `windows/shaders/rdna4/fswin_t.comp`, `include/coopmm.glsl` and `pipelines.json`: two codegen knobs for LLPC, set
   per pipeline in `pipelines.json`. Both leave the output byte-identical; both default to 0, which compiles the
   previous code (the SPIR-V of a build without them is unchanged).
   - `NR_QUAD`: the fswin e4m3 quantisation sites convert a fragment's eight components as two `fe4m3vec4` instead of
     four `fe4m3vec2` (`nr_quant_quad`/`nr_quant_quad32` in `coopmm.glsl`; the Q/K scalings as `nr_qscale_fast4`/
     `nr_kscale_fast4`).
     - Why: LLPC writes `s_setreg_imm32_b32 hwreg(HW_REG_WAVE_MODE, 23, 1), 0` in front of every e4m3 OpFConvert and
       never merges or hoists the writes. That is one MODE write per `v_cvt_pk_fp8_f32` for a pair, and a scalar
       conversion is a MODE write plus a whole convert of its own. A `fe4m3vec4` is one write for two converts, and
       the second convert writes the high half of the dword itself (op_sel) where two pairs also need a shift and a
       merge. Found with RGA's offline `amdllpc` on a probe shader, then confirmed on the driver's own ISA.
     - Each quad is the two pairs of the old code, component for component; only the grouping changes.
     - The bitmask covers ten sites (`NR_QD_*` in `fswin_t.comp`). Which sites pay is a register-allocation matter:
       all ten take 1.6-28% off the C >= 64 bodies, but the expand activation or the post-MLP requantisation alone
       make `fswinimagepost32` 5-8% slower.
     - Masks: 1023 (every site) on 16 pipelines; `fswindsp32` 1021 (not the expand activation); `fswinimagepost32`
       1013 (neither the expand activation nor the post-MLP requantisation), which `temporal_post_fp32` inherits.
       `fswin32` keeps the pairs: its best mask (178) was within 1% of the paired code at every resolution. The
       `fswinds<C>` pipelines keep them too: they are never dispatched in this build (see entry 2), so a change there
       could not be checked.
     - `s_setreg` in the 32 pipelines the runtime creates at 1080p: 7134 -> 4158. Instructions: 118423 -> 114538.
   - `NR_V_ROW=1` on the head-split pipelines (`NR_HWAVES=1`) except `fswindsp256`: V's tiles in `lds_y` are stored
     and read back RowMajor instead of ColumnMajor.
     - The context product reads each V tile as an A operand, eight contiguous columns of one row per lane. From the
       ColumnMajor tile LLPC gathered those bytes 16 apart: 64 `ds_load_u8` per pipeline plus the `v_perm_b32` and
       `v_lshl_or_b32` that assemble them, all in front of the MMA. RowMajor moves the byte scatter to the store
       (`ds_store_b8`, which nothing waits on) and the read becomes `ds_load_b64`. The same element reaches the same
       A component.
     - `ds_load_u8` 512 -> 0 in the 8 pipelines that had them; `fswinfusedup128` goes from 244 to 240 VGPRs, i.e.
       from 5 to 6 waves per SIMD.
     - `fswindsp256` is left out: there `NR_V_ROW` made the dispatch 42-49% slower at 3840x2160 and 16% slower in
       the 1080p `--no-persist` run.
     - Against `NR_QUAD` alone (runtime `mz_timing`, 5 interleaved pairs): -0.046 ms at 1080p and -0.069 ms at 1440p.
   - Changed SPIR-V: 18 of the 23 `fswin_t.comp` network pipelines (not `fswin32` and the four `fswinds<C>`), and
     `temporal/temporal_pre_fp32` and `temporal_post_fp32`.
   - Checked on 2026-09-25 (RX 9070 XT, driver 26.8.1) by running the old and the new SPIR-V on the same input:
     - `nr_graph --out-image` at 1080p, 1440p and 3840x2160, and at 1080p with `--no-persist`, 3 runs each: every
       image byte-identical. Together they dispatch all 18 changed network pipelines.
     - M0's EQUIV set against the goldens (`mz_equiv.py`), with the rebuilt runtime and with the previous runtime
       DLL beside the new shaders: EQUIV PASS both times. That covers `temporal_pre_fp32` and
       `temporal_post_fp32`.
     - the runtime at 1080p with `NR_NO_PERSIST=1` (`mz_bench` dumps, old against new shaders): identical.
   - Timing, against the previous build:
     - the runtime's D3D12-queue gap (`mz_timing`, median of 5 interleaved pairs): 9.76 -> 9.43 ms at 1080p (-0.35)
       and 16.97 -> 16.39 ms at 1440p (-0.58), same runtime DLL, only the shaders changed;
     - standalone `nr_graph --per-layer`, interleaved: 9.218 -> 8.879 ms at 1080p, 15.111 -> 14.602 ms at 1440p,
       34.422 -> 32.881 ms at 3840x2160, and 9.812 -> 9.498 ms at 1080p `--no-persist`;
     - the largest single changes at 1080p: `fswinfusedup128` 183 -> 108 us, `fswinp64` 819 -> 729 us a frame,
       `fswinp128` 923 -> 863 us a frame;
     - cold pipeline compilation: network ready after 22.2 and 21.8 s, against 23.1 and 22.3 s before.
   - Tried and not adopted (the details are in the P9 work notes):
     - a `uvec2` view for the persistent runs' activation loads: identical ISA, so the 8 identity `v_perm_b32` a
       persistent pipeline has left in its prologue stay;
     - `NR_C32_WORDS=1` in `fswinimagepreds32` (the 79 identity copies P4 left there): no gain beside `NR_QUAD`;
     - `vit_attn.comp`'s 128 `buffer_load_u8`: they gather V^T out of the [token][channel] tiles, 16 bytes apart.
       LLPC lowers a ColumnMajor e4m3 `coopMatLoad` to byte loads from any typed view and never to a transposing
       load, and a dword view would need the same number of loads. The kernel is 0.25 ms of the 1080p frame.
5. `windows/src/core/nr_runtime.hpp` and `nr_graph.cpp`: a host's stop flag for a network build.
   - `nr::build_cancel` is a thread-local pointer to an `std::atomic<bool>`; a host sets it around the `Runtime`
     constructor with `nr::BuildCancelScope`. The graph build's pipeline loop checks it before each pipeline. Once the
     flag is set, the loop writes the pipeline cache it has so far (`save_pipeline_cache`) and throws
     `NR build cancelled`.
   - Why: `MochizukiNrRuntime.dll`'s `Destroy` during a build (a game that ends the session during its first, cold
     build) stops waiting out a 20 s compile, and the next start keeps what was compiled.
   - It is thread-local rather than a `RuntimeConfig` field so that the constructor in `nr_runtime.cpp` stays as it
     is. It is null by default, so the standalone `nr_graph` and every other caller are unchanged. No shader, SPIR-V
     or output changes.
   - Checked on 2026-09-25 (RX 9070 XT, driver 26.8.1): a cold build (a freshly named exe, no `pipeline.cache`, no
     prewarm manifest) destroyed 3 s or 8 s into the core's serial compile. `Destroy` returned in 0.37-0.63 s (4 runs),
     and the build stopped at its next pipeline and wrote `pipeline.cache` (0.14 MB at 3 s, 0.37 MB at 8 s). Before
     the change `Destroy` waited for the whole build, 22.2 s. M0's EQUIV set is unchanged.

## Build

`tools\build-mochizuki-runtime.cmd [out]` from an MSVC developer prompt, with the Vulkan SDK 1.4.357 or newer
(`VULKAN_SDK`) and Python. It downloads glslang 16.5.0, the version the upstream SPIR-V is pinned to, into
`toolchain\` here, and writes `MochizukiNrRuntime.dll` and `dlssnr-amd\shaders\` to `out`
(default `exports\mochizuki-runtime`).

`build_network.py` unrolls every constant-trip loop of the `fswin_t.comp` pipelines, as upstream does
(`unroll_glsl.py` with no options). Lighter settings were tried on 2026-09-25 (RX 9070 XT, driver 26.8.1, the build
with patches 1-4) and not adopted. Every one is slower at run time. All but `--max-trip 16`, which saves only 0.1 s
of compilation, also change the output. Measured:
- cold network build: a fresh exe name, no `pipeline.cache`, pipelines created one at a time; median of 2;
- `nr_graph --per-layer` at 1080p, interleaved with the shipped build;
- output: `nr_graph --out-image` at 1080p against the shipped build, 3 runs each, then the runtime (M0's EQUIV set
  at 720p, 1080p and 1440p, or `mz_bench` against its golden for the builds `nr_graph` had already found different).

| `fswin_t` build | Cold network ready | `nr_graph` 1080p | Output | Scratch |
|---|---|---|---|---|
| full unroll (shipped) | 22.5 s | 8.94-9.03 ms | reference | 16 B in `fswinpds256` |
| `unroll_glsl.py --private-index` | 18.6 s | +3.31 ms | differs | 144-272 B in 5 pipelines |
| `unroll_glsl.py --max-trip 16` | 22.4 s | +0.11 ms | identical at 1080p and 3840x2160 | unchanged |
| `--private-index`, then `spirv-opt -O` | 18.2 s | +3.16 ms | differs | 144-272 B in 5 pipelines |
| `spirv-opt -O` | 24.6 s | +0.30 ms | differs after the first frame | 32 B in `fswinpds256` |

- `--private-index` unrolls only the loops whose variable indexes a local array. That cuts the fswin SPIR-V from
  19.5 to 8.3 MB, but:
  - LLPC then spills in `fswindsp32`, `fswinp128`, `fswinpup256` and `fswinfusedup32` too;
  - 12 of the 13 fswin kernels that run at 1080p get 9-83% slower.
- `--max-trip 16` changes only `fswinpup256` and `fswinfusedup256`, the two pipelines with longer loops:
  - the output is identical: `nr_graph --out-image` at 1080p, which dispatches `fswinpup256`, and at 3840x2160, the
    one size that dispatches `fswinfusedup256` (see entry 2), and M0's EQUIV set;
  - `fswinpup256` gets 32% slower at 1080p. At 3840x2160, which does not dispatch it, the frame is 0.04 ms slower.
- `spirv-opt -O` (SPIRV-Tools v2026.3, Vulkan SDK 1.4.357) keeps the network's output identical at 1080p, the one
  size checked (`nr_graph --out-image`, which dispatches 13 of the 23 network pipelines it changes), but:
  - every fswin pipeline takes LLPC longer to compile;
  - `fswinpup256` gets 34% slower and `fswinfusedup128` 45% slower;
  - the temporal variants built from it (`temporal_pre_fp32`, `temporal_post_fp32`) change the output of every
    frame after the first.

## Barriers and `NR_CHAIN` (measured, not changed)

The inter-dispatch barriers were priced again on 2026-09-25 (RX 9070 XT, driver 26.8.1, the build with patches 1-5),
and the ways to remove some of them listed below were measured. None is adopted, and no code changed. `NR_CHAIN`
stays what upstream ships it as: an opt-in diagnostic of `nr_graph` (SPVs built with `-DNR_CHAIN=1` plus
`NR_CHAIN_KERNELS`), which the runtime never turns on. Measured with `nr_graph` (`--warmup 60 --repeats 300`, 3 runs
interleaved with the shipped build) and the runtime's `mz_timing` (5 interleaved pairs):
- The bound. The shipped graph is 0.86 ms slower than the barrier-free graph without persistent runs
  (`NR_DIAG_BARRIERS=1 --no-persist --no-barrier`) at 1080p, 0.92 ms at 1440p and 1.87 ms at 3840x2160. The part
  `NR_CHAIN` or a fusion could reach is the boundaries after the bottleneck's small dispatches (`attn`, `ffwd3`,
  `vitattn` and the `gemm*` kernels; 106 of them, 98 at 3840x2160): dropping all of them (`--no-barrier-after`)
  gains at most 0.60-0.64 ms (two sessions), 0.56 and 0.38 ms.
- `NR_CHAIN` with upstream's kernel list, which includes `vitattn`: every `gemmvqkvnorm` -> `vitattn` wait runs out
  its poll budget, 8 timeouts a frame, and a frame takes 330-380 ms at 1080p and 1440p and 274 ms at 3840x2160. The
  waits return only because the budget is bounded. The argument in `nr_chain.glsl` that a waiting workgroup never
  holds a slot its producer needs does not hold on this driver.
- `NR_CHAIN` without `vitattn`: byte-identical, and slower in every form tried. As shipped it is +0.73-0.78 ms at
  1080p, +0.56 at 1440p and +0.43 at 3840x2160. A backoff LLVM cannot fold (LLPC turns the 64 LCG steps into one
  multiply-add) brings that to +0.21, +0.24 and +0.33 ms in `nr_graph`, and +0.21 / +0.17 ms in the runtime. Built
  with the coherent arena it is +2.5 to +3.5 ms. LLPC lowers every device-scope acquire to `global_inv
  scope:SCOPE_DEV` (RADV emits none; see `coherent_act.glsl`), so the non-coherent chain does get its invalidate:
  one in every poll and one after the wait. Polling with relaxed loads, which leaves only the one after the wait,
  did not change the time.
- The runtime would also need `nr_runtime.cpp` to skip the boundaries `g_chain_nobar` marks. And
  `g_chain_epoch_word` is process-global: after a resolution change the next session reads the first one's epoch
  word, and `mz_phases` hung at its third size.
- C=32 persistent runs (`--persist-levels 32,64,128,256`) need a `fswinp32` pipeline, which neither upstream's
  `pipelines.json` nor this one has. The one measured is `fswin32` as this tree ships it (`NR_FWAVES=2`,
  `NR_HWAVES=0`) plus `NR_PERSIST=1`, `NR_PERSIST_DF=1`, `NR_COHERENT_ACT=1` and `NR_WAVE_UNIFORM=1`. It is
  byte-identical at 1080p and 1440p, but +0.11 and +0.25 ms (+0.12 and +0.24 ms in the runtime). At 3840x2160 both
  C=32 runs report `persist error`, a frame takes 1.06 s, and the picture is wrong (max |d| 0.12). Upstream's
  `NR_FWAVES=1` does the same.
- The cause is the `NR_PERSIST_DF` ready queue in `fswin_t.comp`, which is upstream's code unchanged. A queue entry
  is `nr_tag | c`, a 16-bit epoch tag over a 16-bit item index. At 3840x2160 a C=32 run has 33,017 windows a layer
  and 98,433-98,673 items. An index above 65,535 spills into the tag, so its entry never matches (or names another
  item), and the workgroup that claims it polls until its budget runs out. With a 17-bit index (a scratch build
  only) the frame is byte-identical and 0.51 ms slower. Upstream's +0.067 ms at 4K (the `--persist-levels` note in
  `nr_graph.cpp`) counts 8,349 C=32 windows, which is this plan's count at 1080p.
- The shipped levels (64, 128, 256) stay under that limit: the largest run at 3840x2160 is C=64, with 24,857 items.
  That is exactly the C=32 grid at 1080p (8,349 windows a layer), so C=64 at 7680x4320 has the C=32 grid of
  3840x2160 and would pass 65,535 items (worked out from the window counts, not run). The runtime accepts inputs up
  to 16384x16384.
- Fusing bottleneck kernels. The table gives what the barriers between one pair of kernels are worth at 1080p when
  only those barriers are dropped (a scratch `nr_graph` with a per-pair list; a range covers two sessions):

| Pair | Boundaries | Barrier price |
|---|---|---|
| `gemmvqkvnorm` -> `vitattn` | 8 | 0.16-0.21 ms |
| `gemmproj` -> `gemmvqkvnorm` | 8 | 0.15 ms |
| `vitattn` -> `gemmproj` | 8 | 0.04-0.10 ms |
| `attn` -> `gemmproj` | 15 | 0.06-0.08 ms |
| `ffwd3` -> `gemmproj` | 16 | 0.04-0.07 ms |
| `gemmproj` -> `attn` | 16 | 0.05 ms |
| `gemmvact` -> `gemmproj` | 8 | 0.03-0.05 ms |
| `gemmproj` -> `ffwd3` | 14 | 0.03 ms |
| `gemmproj` -> `gemmvact` | 7 | 0.01 ms |

These are only the barrier's part. What a fusion saves in launches and in the activation's round trip through
memory, and what it loses in parallelism, was not measured. The three pairs that reach 0.1 ms follow each other in
every ViT block:
- `gemmvqkvnorm` -> `vitattn` cannot be fused: every `vitattn` query reads the K and V of every token. `vitattn`
  with 64 or 128 queries a workgroup (fewer workgroups behind that boundary) is identical and no faster.
- `gemmproj` -> `gemmvqkvnorm` and `vitattn` -> `gemmproj` are open candidates, not built. The consumer is a
  `gemm1x1` GEMM, which sums all 1024 channels of a token in FP32 and rounds only after the whole sum. So a
  byte-identical fusion needs the producer's whole row in one workgroup, one workgroup per 32-token tile: 20 at
  1080p, where `gemmproj` runs 160 and `vitattn` 640 (a tile's 32 heads are 32 workgroups). Adding partial sums
  across workgroups with atomics would not keep the order.

## The model

Never shipped with the code. `dlssnr-amd\dlssnr.bin` beside the DLL is made from `nvngx_dlssnr.dll` 310.8.0
(SHA-256 `e16bcf15…6e1fc8e`) by the scripts in `linux/package/model-tools/`: `inspect_nr.py --extract`, the
five `unpack_*.py`, then `pack_model.py --verify model-files.sha256`, as `extract_model.sh` runs them. The result
is 599 entries, 147,756,560 bytes.

## Updating

Copy the paths above from a newer upstream commit, reapply the patches, rebuild, and run the runtime against a
D3D12 frame before shipping it.
