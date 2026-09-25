#include "nr_runtime.hpp"
#include "nr_native_plan.hpp"
#include "nr_runtime_dispatch.hpp"
#define NR_NO_MAIN 1
#include "nr_graph.cpp"
#include <filesystem>
#include <string>
#include <mutex>

// Arena reuse: byte-identical to one value one slot at 16 extents, first and fourth
// frame, model scale 1 and 0.5.
constexpr bool kArenaReuseDefault = true;

namespace nr {
namespace {
std::mutex build_mutex;

void validate(const Controls& c) {
    auto range = [](float v, float lo, float hi) { return std::isfinite(v) && v >= lo && v <= hi; };
    if (!range(c.intensity, 0, 2) || !range(c.detail_strength, 0, 2) || !range(c.colour_strength, 0, 4) ||
        !range(c.max_ratio, 1, 8) || !range(c.local_tone, 0, 2) ||
        !range(c.local_structure, 0, 2) || !range(c.skin_structure, -1, 2) ||
        c.style < 0 || c.style > 2 || c.passes < 1)
        throw std::invalid_argument("invalid NR controls");
}

void barrier(VkCommandBuffer cmd, VkImage image, VkImageLayout old_layout,
             VkImageLayout new_layout, VkPipelineStageFlags src_stage,
             VkAccessFlags src_access, VkPipelineStageFlags dst_stage, VkAccessFlags dst_access) {
    VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    b.srcAccessMask = src_access; b.dstAccessMask = dst_access;
    b.oldLayout = old_layout; b.newLayout = new_layout;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = image; b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdPipelineBarrier(cmd, src_stage, dst_stage, 0, 0, nullptr, 0, nullptr, 1, &b);
}

// `exec_only` drops the memory half. That is correct only between two network
// steps whose pipelines were built with `NR_COHERENT_ACT=1` - every activation
// load and store then carries device scope and reaches L2 itself - which is
// what `s.runner.exec_barrier` says after `build()` read `coherent-act.txt`
// beside the SPVs. Measured in `nr_graph` at -3.7% of the 1080p frame, byte-
// identical; the passes around the network keep the full barrier.
//
// `inv_only` drops the *source* half instead, which is what ships:
// the destination invalidate stays and the availability operation goes, because
// on gfx1201 there is nothing to make available - every cache below the device
// coherence point is write-through. See `runner.inv_barrier` in nr_graph.cpp for
// the argument and the numbers (1080p 7.947 -> 7.829 ms, 4K 29.457 -> 28.939).
void compute_barrier(VkCommandBuffer cmd, bool exec_only = false, bool inv_only = false) {
    VkMemoryBarrier b{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    b.srcAccessMask = inv_only ? 0u : VK_ACCESS_SHADER_WRITE_BIT;
    b.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, exec_only ? 0u : 1u, &b,
                         0, nullptr, 0, nullptr);
}

void dispatch(VkCommandBuffer cmd, const nrvk::Kernel& k, uint32_t x, uint32_t y,
              uint32_t z, const void* push, uint32_t bytes) {
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, k.pipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, k.layout, 0, 1, &k.set, 0, nullptr);
    if (bytes) vkCmdPushConstants(cmd, k.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, bytes, push);
    vkCmdDispatch(cmd, x, y, z);
}

// How a caller's colour format reaches the network's RGBA32F image.
//
//   identical   - RGBA32F: a plain copy, no conversion anywhere.
//   through a   - the four 8-bit formats: the bits are copied into a private
//   transfer      UNORM image and blitted from there, so an *_SRGB view never
//   image         applies its transfer function to values the network was
//                 trained on as plain [0,1]. This is the existing path.
//   direct blit - the wide formats a Wayland surface offers first: FP16, 16-bit
//                 UNORM and the two 10-bit packed ones, plus the packed 11/11/10
//                 float an engine renders into before its upscaler. None of them
//                 carries an sRGB transfer function, so there is no hazard to
//                 route around and `vkCmdBlitImage` converts directly. The
//                 swapchain layer only accepts these with SRGB_NONLINEAR; the
//                 engine-data path hands over linear scene-referred values and
//                 nothing here rescales or encodes them.
enum class Transfer { Identical, Encoded, DirectBlit };

Transfer transfer_mode(VkFormat format) {
    switch(format) {
        case VK_FORMAT_R32G32B32A32_SFLOAT: return Transfer::Identical;
        case VK_FORMAT_R8G8B8A8_UNORM: case VK_FORMAT_R8G8B8A8_SRGB:
        case VK_FORMAT_B8G8R8A8_UNORM: case VK_FORMAT_B8G8R8A8_SRGB: return Transfer::Encoded;
        case VK_FORMAT_R16G16B16A16_SFLOAT: case VK_FORMAT_R16G16B16A16_UNORM:
        case VK_FORMAT_A2B10G10R10_UNORM_PACK32: case VK_FORMAT_A2R10G10B10_UNORM_PACK32:
        // The engine-data path's usual colour buffer (DXGI R11G11B10_FLOAT):
        // unsigned floats, no alpha, no transfer function, values above 1.0
        // allowed. The blit in carries them unchanged and the blit out clamps
        // anything negative to zero, which is what the format can hold.
        case VK_FORMAT_B10G11R11_UFLOAT_PACK32:
            return Transfer::DirectBlit;
        default:
            throw std::invalid_argument("unsupported NR colour format, VkFormat " +
                                        std::to_string(unsigned(format)));
    }
}

VkFormat encoded_format(VkFormat format) {
    switch(format) {
        case VK_FORMAT_R8G8B8A8_UNORM: case VK_FORMAT_R8G8B8A8_SRGB: return VK_FORMAT_R8G8B8A8_UNORM;
        case VK_FORMAT_B8G8R8A8_UNORM: case VK_FORMAT_B8G8R8A8_SRGB: return VK_FORMAT_B8G8R8A8_UNORM;
        default: return format;
    }
}
}  // namespace

// The fallback motion estimator's pyramid, as measured:
// base /4 with four levels, and a per-level
// search radius that a /2 base was shown to be *worse* than, not cheaper than.
constexpr unsigned kTemporalLevels = 4;
constexpr unsigned kTemporalBase = 4;
constexpr int kTemporalRadius[kTemporalLevels] = {2, 3, 3, 4};
constexpr float kTemporalReject = 0.50f;

struct Temporal {
    struct LumaPush { uint32_t dst_w, dst_h, src_w, src_h, mode; };
    struct FlowPush {
        uint32_t level_w, level_h, coarse_w, coarse_h;
        int32_t radius; uint32_t first, last; float reject;
    };

    bool enabled{};
    uint32_t lw[kTemporalLevels]{}, lh[kTemporalLevels]{};
    // Ping-pong: this frame's pyramid becomes next frame's previous one. The
    // descriptor sets are baked at creation, so each parity needs its own
    // pipelines; they are four tiny dispatches each and cost nothing to hold.
    nrvk::Context::Image luma[2][kTemporalLevels]{};
    nrvk::Context::Image flow[kTemporalLevels]{};
    nrvk::Context::Image flow0_sampled{};   // the same image, sampled alias
    VkSampler motion_sampler{};             // owned here; the alias does not own its image
    nrvk::Context::Image history{};
    // Where the engine's depth is put, so the consumer has one descriptor
    // whether a game supplies depth or not. Allocated always and left cleared
    // when it is not; the shader is told which by the parameter buffer rather
    // than by reading the contents, because an all-zero depth is a legitimate
    // depth.
    nrvk::Context::Image depth{};
    nrvk::Buffer params;
    nrvk::Kernel luma_kernel[2][kTemporalLevels];
    nrvk::Kernel flow_kernel[2][kTemporalLevels];
    nrvk::Kernel pre, post;
    const nrvk::Kernel* original_pre{};
    const nrvk::Kernel* original_post{};
    float history_strength{1.0f};
    uint32_t parity{};
    // The first-frame latch, the original's plan +0x68. Set after any recorded
    // frame, never cleared by `reset` - reset suppresses consumption, it does
    // not un-write the history buffer.
    bool latch{};

    void destroy(nrvk::Context& ctx) {
        for (unsigned p = 0; p < 2; ++p)
            for (unsigned k = 0; k < kTemporalLevels; ++k) {
                if (luma_kernel[p][k].device) luma_kernel[p][k].destroy();
                if (flow_kernel[p][k].device) flow_kernel[p][k].destroy();
                ctx.destroy(luma[p][k]);
            }
        for (unsigned k = 0; k < kTemporalLevels; ++k) ctx.destroy(flow[k]);
        ctx.destroy(depth);
        if (pre.device) pre.destroy();
        if (post.device) post.destroy();
        // flow0_sampled aliases flow[0]; only its sampler is ours to free.
        if (motion_sampler) vkDestroySampler(ctx.device, motion_sampler, nullptr);
        ctx.destroy(history);
        ctx.destroy(params);
    }
};

struct Runtime::Impl {
    NrSession session;
    nrvk::Kernel alpha, mask_pre, mask_resolve;
    // The linear-light path: the proxy is made in place on tex_in and the
    // original kept here for the decode. Only when RuntimeConfig::linear_input.
    nrvk::Kernel encode, transfer_pass;
    nrvk::Context::Image keep{};
    bool linear{};
    float white_point{1.0f};
    // Model Resolution: the network runs at mw x mh, the frame is width x
    // height. When they differ, `keep_full` holds the frame as handed over
    // (linear or sRGB, whichever it is) and `full_out` is where the transfer
    // pass writes the full-resolution answer; both are absent otherwise.
    uint32_t mw{}, mh{};
    bool scaled{};
    bool native_compose{};   // RuntimeConfig::native_compose
    VkFilter downscale_filter{VK_FILTER_LINEAR};
    nrvk::Context::Image keep_full{}, full_out{};
    // Multi-pass: what the first pass was shown, kept for the transfer pass
    // because later passes overwrite tex_in; and one history per pass.
    uint32_t max_passes{1};
    nrvk::Context::Image shown_keep{};
    // The detail-only cascade (RuntimeConfig::cascade_detail_only): the log
    // luminance ratio, its separable blur (through tmp), and the pass that
    // writes the next input. Model-sized, GENERAL for life.
    bool cascade_detail{};
    int cascade_radius{4};
    nrvk::Context::Image lograt{}, cascade_tmp{}, lowpass{};
    nrvk::Kernel cascade_lograt, cascade_blur_h, cascade_blur_v, cascade_feed;
    std::vector<nrvk::Context::Image> history_store;
    // ---- what the pass costs, measured on the GPU ----------------------------
    //
    // A timestamp pair around everything `record_all` puts in the caller's
    // command buffer. A **ring** of them, read several recordings behind and
    // never with the WAIT bit: the point of the number is to be free to ask for,
    // and a query that is still in flight must not stall the render thread that
    // is asking. `timing_ok` is false on a queue family with no timestamp bits,
    // and then every accessor answers zero rather than a wrong number.
    static constexpr uint32_t kTimingSlots = 4;
    VkQueryPool timing{};
    bool timing_ok{};
    double timestamp_ns{};       // VkPhysicalDeviceLimits::timestampPeriod
    uint32_t timing_slot{};      // the slot the next recording writes
    uint64_t timings_recorded{};
    float gpu_ms{}, gpu_ms_avg{};
    // Read the oldest slot in the ring, if its recording has finished. Called
    // once per recording, from the thread that records.
    void read_timing() {
        if (!timing_ok || timings_recorded <= kTimingSlots) return;
        const uint32_t oldest = timing_slot;   // the next to be overwritten
        uint64_t stamps[2] = {};
        if (vkGetQueryPoolResults(session.ctx.device, timing, oldest * 2, 2, sizeof stamps, stamps,
                                  sizeof(uint64_t), VK_QUERY_RESULT_64_BIT) != VK_SUCCESS)
            return;   // VK_NOT_READY: still running, ask again next frame
        if (stamps[1] <= stamps[0]) return;
        gpu_ms = float(double(stamps[1] - stamps[0]) * timestamp_ns / 1e6);
        // A plain exponential average. Ten frames is enough to stop the last
        // digit dancing and short enough that a resolution change shows up
        // immediately rather than being dragged in from before it.
        gpu_ms_avg = gpu_ms_avg > 0.0f ? gpu_ms_avg + (gpu_ms - gpu_ms_avg) * 0.1f : gpu_ms;
    }
    // ---- one temporal state per feature -------------------------------------
    //
    // NVIDIA's DLL gives every NGX feature its own object, and OptiScaler builds
    // on that: one feature per model pass, features at two extents alive at
    // once, features destroyed and rebuilt whenever a control moves. What makes
    // a feature independent is its temporal state and nothing else - the
    // weights, the pipelines and the activation arena are properties of the
    // network and the extent, not of the handle.
    //
    // So a feature here is exactly this: one history image, the first-frame
    // latch and the estimator's parity. `temporal.history` stays the image the
    // pre/post descriptor sets are baked against - it is the *bound* history -
    // and a feature's own image is copied into it on the way in and back out on
    // the way out, but only when the bound feature actually changes. With one
    // feature, which is every caller that does not use this, nothing is copied
    // and the recording is byte-identical to what it always was.
    struct FeatureState {
        nrvk::Context::Image history{};
        bool latch{};
        uint32_t parity{};
        bool cleared{};
    };
    std::map<uint64_t, FeatureState> features;
    uint64_t bound_feature{};
    // Released features, and the recording count at which their images may be
    // destroyed. Never a wait: the render thread that releases a feature must
    // not block on the GPU, and OptiScaler has already parked the feature for 32
    // evaluates before it gets here (DlssNr_Dx12.cpp ParkNrFeature).
    static constexpr uint64_t kRetireAfter = 64;
    std::vector<std::pair<nrvk::Context::Image, uint64_t>> retiring;
    uint64_t recordings{};
    // **The tone control is a frame edit and belongs to one pass.** NVIDIA's own
    // description of `LocalToneStrength` is "low-frequency details such as
    // broader lighting and colour response", and it enters the network as five
    // constant features on the pre block - the same constant on every pixel.
    // Running the network again on its own answer therefore applies that whole
    // low-frequency edit a second time, and it compounds: measured on the
    // TR2013 frame against NVIDIA's own output, the block-mean luminance ratio
    // runs 1.052 -> 1.102 -> 1.136 over three passes with the tone control on
    // each time, and 1.052 -> 1.055 -> 1.056 when only the first pass carries
    // it. That drift is the red skin the user sees at more than one pass.
    // Structure is what a later pass can legitimately add more of, so only the
    // tone feature is dropped. One alternate push blob per dispatch, parallel to
    // `session.disp`, empty for every dispatch that is not the pre block.
    //
    // **Not under `cascade_detail_only`.** That feed hands the next pass the
    // first pass's *tone* with this pass's structure on it, so the tone edit has
    // been taken back out of the picture and the network has to make it again;
    // dropping the control there moves the two-pass answer away from the
    // one-pass one instead of towards it (block drift mean 0.0163 -> 0.0273 in
    // `linear_frame_test`). The two are alternative ways to stop the same
    // stacking, so exactly one of them is active at a time.
    // [pass][dispatch]; empty means "use pass 1's push". Pass 0's entry is
    // unused - that pass writes session.disp[i].push in place.
    std::vector<std::vector<std::vector<uint8_t>>> pass_push;
    nrvk::Context::Image mask_image;
    nrvk::Buffer mask_rect;
    const nrvk::Kernel* original_pre{};
    nrvk::Context::Image encoded;
    Transfer transfer{Transfer::Identical};
    Temporal temporal;
    VkFormat colour_format{};
    uint32_t width{}, height{};

    ~Impl() {
        // Caller has already completed its submitted command buffers.
        auto& s = session;
        if (!s.ctx.device) return;
        if (alpha.device) alpha.destroy();
        if (encode.device) encode.destroy();
        if (transfer_pass.device) transfer_pass.destroy();
        if (keep.handle) session.ctx.destroy(keep);
        if (keep_full.handle) session.ctx.destroy(keep_full);
        if (full_out.handle) session.ctx.destroy(full_out);
        if (shown_keep.handle) session.ctx.destroy(shown_keep);
        if (lograt.handle) session.ctx.destroy(lograt);
        if (cascade_tmp.handle) session.ctx.destroy(cascade_tmp);
        if (lowpass.handle) session.ctx.destroy(lowpass);
        if (cascade_lograt.device) cascade_lograt.destroy();
        if (cascade_blur_h.device) cascade_blur_h.destroy();
        if (cascade_blur_v.device) cascade_blur_v.destroy();
        if (cascade_feed.device) cascade_feed.destroy();
        for (auto& h : history_store) session.ctx.destroy(h);
        for (auto& f : features) if (f.second.history.handle) session.ctx.destroy(f.second.history);
        for (auto& r : retiring) if (r.first.handle) session.ctx.destroy(r.first);
        if (mask_pre.device) mask_pre.destroy();
        if (mask_resolve.device) mask_resolve.destroy();
        if (timing) { vkDestroyQueryPool(s.ctx.device, timing, nullptr); timing = VK_NULL_HANDLE; }
        temporal.destroy(s.ctx);
        if (s.ctx.pipeline_cache) {
            vkDestroyPipelineCache(s.ctx.device, s.ctx.pipeline_cache, nullptr);
            s.ctx.pipeline_cache = VK_NULL_HANDLE;
        }
        s.ctx.destroy(mask_image);
        s.ctx.destroy(mask_rect);
        if (s.runner.ctx) s.runner.destroy();
        for (auto& entry : s.kern) if (entry.second.device) entry.second.destroy();
        s.ctx.destroy(s.tex_in); s.ctx.destroy(s.surf0); s.ctx.destroy(s.surf1);
        s.ctx.destroy(encoded);
        s.ctx.destroy(s.act); s.ctx.destroy(s.wgt);
        // Do not destroy the host's instance, device or queue.
    }

    // One pass's resolved model controls. A later pass inherits pass 1 with the
    // tone zeroed - that is what the original's own hosts do, and re-applying
    // the tone control every pass compounds it (x1.052 -> x1.102 -> x1.136 over
    // three) - unless the caller gave that pass its own set.
    struct Resolved { int style; float intensity, tone, structure, skin_in; bool mask; };

    Resolved resolve_pass(const Controls& c, uint32_t pass) const {
        Resolved r{c.style, c.intensity, c.local_tone, c.local_structure, c.skin_structure,
                   c.automatic_mask};
        if (pass == 0) return r;
        if (!cascade_detail) r.tone = 0.0f;
        const size_t k = size_t(pass) - 1;
        if (k < c.per_pass.size() && c.per_pass[k].used) {
            const PassControls& o = c.per_pass[k];
            r.style = o.style; r.intensity = o.intensity; r.tone = o.local_tone;
            r.structure = o.local_structure; r.skin_in = o.skin_structure; r.mask = o.automatic_mask;
        }
        return r;
    }

    // Which dispatches carry a control at all. Everything else uses pass 1's
    // push, so a later pass costs four small blobs rather than a copy of the
    // whole table.
    static bool carries_controls(const std::string& kern) {
        return kern == "fswinimagepreds32" || kern.rfind("imgin", 0) == 0 ||
               kern == "fswinimagepost32" || kern.rfind("imgout", 0) == 0;
    }

    void patch_push(std::vector<uint8_t>& blob, const std::string& kern, const Resolved& r) const {
        const float skin = r.mask ? (r.skin_in < 0 ? r.structure : r.skin_in) : -1.0f;
        const float background = r.mask ? r.structure : -1.0f;
        if (kern == "fswinimagepreds32") {
            PushPreImage p{};
            std::memcpy(&p, blob.data() + sizeof(PushFSwin), sizeof p);
            p.style = float(r.style) / 128; p.tone = r.tone;
            p.structure = r.mask ? 1 : r.structure;
            p.skin = skin; p.other = background;
            std::memcpy(blob.data() + sizeof(PushFSwin), &p, sizeof p);
        } else if (kern.rfind("imgin", 0) == 0) {
            PushImgIn p{}; std::memcpy(&p, blob.data(), sizeof p);
            p.s434 = float(r.style) / 128; p.aux0 = r.tone;
            p.aux1 = r.mask ? 1 : r.structure;
            p.gate0 = skin; p.gate1 = background;
            std::memcpy(blob.data(), &p, sizeof p);
        } else if (kern == "fswinimagepost32") {
            constexpr size_t offset = sizeof(PushFSwin) + sizeof(PushUps);
            PushImageTail p{}; std::memcpy(&p, blob.data() + offset, sizeof p);
            // Native compose (the OptiScaler and ReShade routes): the fused
            // tail's own blend IS the DLL's post-block Intensity. **Not clamped
            // to 1**: that clamp came from one post variant's `sat(Intensity)`
            // in the PTX, which governs that variant's lerp and not the
            // control - the DLL copies Intensity out of its settings
            // unsaturated (18001cf07) and the Toolkit documents 0..2. The tail
            // takes 0..1 as the lerp and past it as a luminance-ratio power;
            // see windows/shaders/rdna4/fswin_t.comp.
            // Every route, every pass: each pass carries its own Intensity
            // into this blend, as each OptiScaler DLSS-NR pass is its own DLL
            // feature with its own Intensity.
            p.intensity = std::clamp(r.intensity, 0.0f, 2.0f);
            std::memcpy(blob.data() + offset, &p, sizeof p);
        } else if (kern.rfind("imgout", 0) == 0) {
            PushImgOut p{}; std::memcpy(&p, blob.data(), sizeof p);
            p.nr_intensity = std::clamp(r.intensity, 0.0f, 2.0f);
            std::memcpy(blob.data(), &p, sizeof p);
        }
    }

    void controls(const Controls& c) {
        const uint32_t npass = std::max<uint32_t>(max_passes, 1u);
        pass_push.assign(npass, {});
        for (auto& d : session.disp) patch_push(d.push, d.kern, resolve_pass(c, 0));
        for (uint32_t p = 1; p < npass; ++p) {
            const Resolved r = resolve_pass(c, p);
            pass_push[p].assign(session.disp.size(), {});
            for (size_t i = 0; i < session.disp.size(); ++i) {
                if (!carries_controls(session.disp[i].kern)) continue;
                pass_push[p][i] = session.disp[i].push;
                patch_push(pass_push[p][i], session.disp[i].kern, r);
            }
        }
    }
};

Runtime::Runtime(const HostDevice& host, const RuntimeConfig& config) : Runtime(host,config,ControlMaskConfig{}) {}
Runtime::Runtime(const HostDevice& host, const RuntimeConfig& config, const ControlMaskConfig& mask_config)
    : Runtime(host,config,mask_config,TemporalConfig{}) {}
Runtime::Runtime(const HostDevice& host, const RuntimeConfig& config, const ControlMaskConfig& mask_config,
                 const TemporalConfig& temporal_config)
    : impl_(std::make_unique<Impl>()) {
    runtime_dispatch::Scope physical_scope(host.instance, host.physical_dispatch);
    if (!host.instance || !host.physical || !host.device || !host.queue || !config.width || !config.height)
        throw std::invalid_argument("NR requires complete host device and source extent");
    if (config.accumulation != "fp32" && config.accumulation != "round-fp16")
        throw std::invalid_argument("unsupported accumulation policy");
    const Transfer transfer = transfer_mode(config.colour_format);
    const VkFormat unorm = encoded_format(config.colour_format);
    impl_->transfer = transfer;
    impl_->colour_format = config.colour_format;
    const auto root = std::filesystem::canonical(config.root);
    // An installed copy keeps everything it loads in dlssnr-amd/ beside the DLL:
    // the model pack and shaders/ (network, runtime/ passes, temporal/). A
    // development tree has build/ and artifacts/ at its root instead.
    const auto data = std::filesystem::is_directory(root / "dlssnr-amd") ? root / "dlssnr-amd" : root;
    const bool installed = data != root;
    const auto network_shaders = installed ? data / "shaders" : root / "build";
    if (!std::isfinite(config.model_scale) || config.model_scale <= 0.f || config.model_scale > 1.f)
        throw std::invalid_argument("model_scale must be in (0, 1]");
    if (config.max_passes < 1 || config.max_passes > 16)
        throw std::invalid_argument("max_passes must be 1..16");
    // The model extent. At scale 1 it is the frame's own, unaligned, as every
    // game has run so far; below 1 it is rounded to a multiple of 8 (the
    // network's tile) and never above the frame.
    auto model_extent = [&](uint32_t v) {
        if (config.model_scale == 1.0f) return v;
        uint32_t m = uint32_t(std::lround(double(v) * config.model_scale));
        m = (m + 4) / 8 * 8;
        return std::min(std::max(m, 8u), v);
    };
    const uint32_t mw = model_extent(config.width), mh = model_extent(config.height);
    impl_->mw = mw; impl_->mh = mh; impl_->scaled = mw != config.width || mh != config.height;
    impl_->native_compose = config.native_compose;
    if (config.native_compose && impl_->scaled)
        throw std::runtime_error("native_compose needs model_scale 1: the DLL has no transfer pass to resample through");
    impl_->max_passes = config.max_passes;
    const auto native = make_native_plan(mw, mh);
    std::string plan_name = "compiled native descriptor";
    if (!config.plan.empty()) {
        const auto path = std::filesystem::canonical(config.plan);
        const auto rel = path.lexically_relative(root);
        if (rel.empty() || *rel.begin() == "..") throw std::invalid_argument("plan must be inside NR root");
        auto tokens = [](std::istream& input) {
            std::vector<std::string> result; std::string line, word;
            while (std::getline(input, line)) {
                if (line.empty() || line[0] == '#') continue;
                std::istringstream words(line); while (words >> word) result.push_back(word);
            }
            return result;
        };
        std::ifstream provided(path); std::istringstream expected(native.text);
        if (!provided || tokens(provided) != tokens(expected))
            throw std::invalid_argument("explicit plan does not match native source-sized descriptor plan");
        plan_name = path.string();
    }
    std::istringstream plan_stream(native.text);
    const auto prepared_plan = parse_plan(plan_stream);
    nr::PhaseTimer timer;
    std::lock_guard<std::mutex> lock(build_mutex);
    auto& s = impl_->session;
    uint32_t count = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(host.physical, &count, nullptr);
    std::vector<VkQueueFamilyProperties> props(count);
    vkGetPhysicalDeviceQueueFamilyProperties(host.physical, &count, props.data());
    if (host.queue_family >= count || !(props[host.queue_family].queueFlags & VK_QUEUE_COMPUTE_BIT))
        throw std::invalid_argument("NR host queue must support compute");
    if (transfer != Transfer::Identical && !(props[host.queue_family].queueFlags & VK_QUEUE_GRAPHICS_BIT))
        throw std::invalid_argument("NR colour format conversion requires a graphics-capable queue");
    std::string why;
    if (!s.ctx.adopt(host.instance, host.physical, host.device, host.queue, host.queue_family, &why))
        throw std::runtime_error("NR device unsupported: " + why);
    s.ctx.require_matrix_config();
    // Where the pass's own cost is measured. Two queries per ring slot. A queue
    // family is allowed to report no timestamp bits - then there is no pool, and
    // every accessor answers zero rather than a number made up from a clock.
    if (props[host.queue_family].timestampValidBits && s.ctx.timestamp_period > 0.0f) {
        VkQueryPoolCreateInfo qi{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
        qi.queryType = VK_QUERY_TYPE_TIMESTAMP;
        qi.queryCount = Impl::kTimingSlots * 2;
        if (vkCreateQueryPool(s.ctx.device, &qi, nullptr, &impl_->timing) == VK_SUCCESS) {
            impl_->timing_ok = true;
            impl_->timestamp_ns = s.ctx.timestamp_period;
        }
    }
    // Values share arena memory by lifetime (nr_graph.cpp, `reuse`): same
    // arithmetic, same bytes out, a fraction of the memory. NR_ARENA_REUSE=0
    // gives every value its own bytes again, for an A/B.
    auto arena_reuse = [] {
        const char* e = std::getenv("NR_ARENA_REUSE");
        return e && *e ? std::atoi(e) != 0 : kArenaReuseDefault;
    };
    std::vector<std::string> arguments = {"nr_runtime", "--plan", plan_name,
        "--unpacked", (installed ? data / "model" : root / "artifacts").string(),
        "--spv-dir", network_shaders.string(),
        "--host-boundary", arena_reuse() ? "--reuse" : "--no-reuse", "--source-width", std::to_string(mw),
        "--source-height", std::to_string(mh), "--accumulation", config.accumulation,
        "--pipeline-cache", (installed ? data / "pipeline.cache" : root / "dlssnr-amd.pipelinecache").string()};
    // The weights as one file (linux/package/model-tools/pack_model.py); the names inside are
    // the relative paths the graph asks for under --unpacked.
    if (installed) {
        const auto pack = data / "dlssnr.bin";
        if (!std::filesystem::is_regular_file(pack))
            throw std::runtime_error("missing " + pack.string() + " (run install.sh again)");
        arguments.push_back("--model-pack"); arguments.push_back(pack.string());
    }
    std::vector<char*> argv;
    for (auto& a : arguments) argv.push_back(a.data());
    timer.mark("device");
    if (s.build(int(argv.size()), argv.data(), &prepared_plan) != 0 || !s.missing.empty())
        throw std::runtime_error("NR graph initialization failed");
    timer.mark("graph");
    impl_->width = config.width; impl_->height = config.height;
    const auto adapters = std::filesystem::canonical(config.adapter_shaders.empty()
        ? network_shaders / "runtime" : std::filesystem::path(config.adapter_shaders));
    const auto adapter_rel = adapters.lexically_relative(root);
    if (adapter_rel.empty() || *adapter_rel.begin() == "..")
        throw std::invalid_argument("adapter shaders must be inside NR root");
    if (impl_->scaled) {
        if (mask_config.width) throw std::invalid_argument("the control mask path does not support model_scale below 1");
        impl_->keep_full = s.ctx.image(config.width, config.height, VK_FORMAT_R32G32B32A32_SFLOAT, true);
        s.ctx.transition(impl_->keep_full, VK_IMAGE_LAYOUT_GENERAL);
        impl_->full_out = s.ctx.image(config.width, config.height, VK_FORMAT_R32G32B32A32_SFLOAT, false);
        s.ctx.transition(impl_->full_out, VK_IMAGE_LAYOUT_GENERAL);
        // The downscale is a blit from the RGBA32F keep; linear filtering on a
        // 32-bit float format is a feature bit, not a given.
        VkFormatProperties fp{}; vkGetPhysicalDeviceFormatProperties(host.physical, VK_FORMAT_R32G32B32A32_SFLOAT, &fp);
        impl_->downscale_filter = (fp.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT)
            ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
    }
    if (config.max_passes > 1) {
        if (mask_config.width) throw std::invalid_argument("the control mask path does not support more than one pass");
        impl_->shown_keep = s.ctx.image(mw, mh, VK_FORMAT_R32G32B32A32_SFLOAT, true);
        s.ctx.transition(impl_->shown_keep, VK_IMAGE_LAYOUT_GENERAL);
        if (config.cascade_detail_only) {
            impl_->cascade_detail = true;
            impl_->cascade_radius = std::max(4, int(mh / 60));
            for (auto* im : {&impl_->lograt, &impl_->cascade_tmp, &impl_->lowpass}) {
                *im = s.ctx.image(mw, mh, VK_FORMAT_R32_SFLOAT, false);
                s.ctx.transition(*im, VK_IMAGE_LAYOUT_GENERAL);
            }
            nrvk::Context::Image tex_in_storage = s.tex_in;
            tex_in_storage.sampler = VK_NULL_HANDLE;
            tex_in_storage.layout = VK_IMAGE_LAYOUT_GENERAL;
            impl_->cascade_lograt.create(s.ctx, (adapters / "cascade_lograt.spv").string(), {}, 12,
                                         {&s.surf0, &impl_->shown_keep, &impl_->lograt});
            impl_->cascade_blur_h.create(s.ctx, (adapters / "cascade_blur.spv").string(), {}, 16,
                                         {&impl_->lograt, &impl_->cascade_tmp});
            impl_->cascade_blur_v.create(s.ctx, (adapters / "cascade_blur.spv").string(), {}, 16,
                                         {&impl_->cascade_tmp, &impl_->lowpass});
            impl_->cascade_feed.create(s.ctx, (adapters / "cascade_feed.spv").string(), {}, 12,
                                       {&impl_->shown_keep, &impl_->lograt, &impl_->lowpass, &tex_in_storage});
        }
    }
    // The alpha pass restores the frame's own alpha onto the answer: the answer
    // is full_out when scaled, and the frame's alpha lives wherever the frame
    // was kept.
    nrvk::Context::Image* answer = impl_->scaled ? &impl_->full_out : &s.surf0;
    nrvk::Context::Image* frame_alpha = impl_->scaled ? &impl_->keep_full
                                        : (config.max_passes > 1 ? &impl_->shown_keep : &s.tex_in);
    impl_->alpha.create(s.ctx, (adapters / "runtime_alpha.spv").string(), {}, 12, {answer, frame_alpha});
    if (config.linear_input) {
        impl_->linear = true;
        impl_->white_point = config.white_point;
        // Sampled, because the transfer reads it; the encode writes it through
        // a storage alias (same image and view, no sampler, GENERAL).
        impl_->keep = s.ctx.image(mw, mh, VK_FORMAT_R32G32B32A32_SFLOAT, true);
        s.ctx.transition(impl_->keep, VK_IMAGE_LAYOUT_GENERAL);
        nrvk::Context::Image keep_storage = impl_->keep;
        keep_storage.sampler = VK_NULL_HANDLE;
        // tex_in as a storage image, in GENERAL for the one pass that writes it.
        // Same VkImage and view as the sampled descriptor every kernel reads;
        // the alias owns nothing and is never destroyed.
        nrvk::Context::Image tex_in_storage = s.tex_in;
        tex_in_storage.sampler = VK_NULL_HANDLE;
        tex_in_storage.layout = VK_IMAGE_LAYOUT_GENERAL;
        impl_->encode.create(s.ctx, (adapters / "runtime_encode.spv").string(), {}, 16,
                             {&tex_in_storage, &keep_storage});
    }
    // The transfer runs on every frame at the FRAME extent: `result` and
    // `shown` are model-sized (sampled bilinearly when scaled), `keep` is the
    // frame as handed over - the full-resolution copy when scaled, the
    // untouched linear original on the linear path, the frame itself
    // otherwise - and `out` is the full-resolution answer.
    {
        nrvk::Context::Image* shown = config.max_passes > 1 ? &impl_->shown_keep : &s.tex_in;
        nrvk::Context::Image* keep = impl_->scaled ? &impl_->keep_full
                                     : impl_->linear ? &impl_->keep : shown;
        impl_->transfer_pass.create(s.ctx, (adapters / "runtime_transfer.spv").string(), {}, 36,
                               {&s.surf0, shown, keep, answer});
    }
    auto require_variant_profile = [&](const std::filesystem::path& directory) {
        std::ifstream manifest(directory / "shader-constants.txt");
        uint32_t profile = 0;
        if (manifest) {
            const auto values = nr::detail::read_shader_manifest(manifest);
            if (values.count("math_profile")) profile = values.at("math_profile");
        } else if (s.math_profile != 0) {
            throw std::runtime_error("fast graph requires a versioned image-variant manifest");
        }
        if (profile != s.math_profile)
            throw std::runtime_error("image variant and graph math profiles differ");
    };
    if (bool(mask_config.width) != bool(mask_config.height))
        throw std::invalid_argument("both control mask dimensions are required");
    if (mask_config.width) {
        if (config.accumulation == "fp32") require_variant_profile(adapters);
        impl_->mask_image=s.ctx.image(mask_config.width,mask_config.height,VK_FORMAT_R32G32B32A32_SFLOAT,true);
        impl_->mask_rect=s.ctx.buffer(24);
        auto& k=impl_->mask_pre;
        if (s.kern.count("fswinimagepreds32")) {
            impl_->original_pre=&s.kern.at("fswinimagepreds32");
            k.create(s.ctx,(adapters/"mask_pre_fp32.spv").string(),
                {s.act.handle,s.act.handle,s.wgt.handle,s.wgt.handle,s.wgt.handle,s.act.handle,impl_->mask_rect.handle},
                sizeof(PushFSwin)+sizeof(PushPreImage),{&s.tex_in,&impl_->mask_image});
        } else if (s.kern.count("imgin16fast")) {
            impl_->original_pre=&s.kern.at("imgin16fast");
            k.create(s.ctx,(adapters/"mask_pre_round.spv").string(),
                {s.act.handle,s.wgt.handle,impl_->mask_rect.handle},sizeof(PushImgIn),{&s.tex_in,&impl_->mask_image});
        } else throw std::runtime_error("control mask pre-input variant unavailable");
        impl_->mask_resolve.create(s.ctx,(adapters/"runtime_control_mask.spv").string(),
            {impl_->mask_rect.handle},12,{&s.surf0,&s.tex_in,&impl_->mask_image});
    }
    if (temporal_config.enable) {
        auto& t = impl_->temporal;
        const auto shaders = std::filesystem::canonical(temporal_config.shaders.empty()
            ? (installed ? data / "shaders/temporal" : root / "build/windows/rdna4/network/temporal")
            : std::filesystem::path(temporal_config.shaders));
        require_variant_profile(shaders);
        const auto temporal_rel = shaders.lexically_relative(root);
        if (temporal_rel.empty() || *temporal_rel.begin() == "..")
            throw std::invalid_argument("temporal shaders must be inside NR root");
        if (!s.kern.count("fswinimagepreds32") || !s.kern.count("fswinimagepost32"))
            throw std::runtime_error("the temporal path requires the fused FP32 image kernels");
        t.original_pre = &s.kern.at("fswinimagepreds32");
        t.original_post = &s.kern.at("fswinimagepost32");
        t.lw[0] = (mw + kTemporalBase - 1) / kTemporalBase;
        t.lh[0] = (mh + kTemporalBase - 1) / kTemporalBase;
        for (unsigned k = 1; k < kTemporalLevels; ++k) {
            t.lw[k] = (t.lw[k - 1] + 1) / 2;
            t.lh[k] = (t.lh[k - 1] + 1) / 2;
        }
        for (unsigned p = 0; p < 2; ++p)
            for (unsigned k = 0; k < kTemporalLevels; ++k)
                t.luma[p][k] = s.ctx.image(t.lw[k], t.lh[k], VK_FORMAT_R32_SFLOAT, false);
        for (unsigned k = 0; k < kTemporalLevels; ++k)
            t.flow[k] = s.ctx.image(t.lw[k], t.lh[k], VK_FORMAT_R32G32_SFLOAT, false);
        // The five-tap reconstruction's merged centre tap is only valid under
        // linear filtering, and the motion field wants the same. nrvk creates
        // NEAREST + MIRRORED_REPEAT because that is the shipping colour
        // kernel's addressing; replace the sampler here rather than change a
        // production header.
        auto linear_sampler = [&] {
            VkSamplerCreateInfo si{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
            si.magFilter = si.minFilter = VK_FILTER_LINEAR;
            si.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
            si.addressModeU = si.addressModeV = si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
            VkSampler sampler{};
            NRVK_CHECK(vkCreateSampler(s.ctx.device, &si, nullptr, &sampler));
            return sampler;
        };
        t.history = s.ctx.image(mw, mh, VK_FORMAT_R32G32B32A32_SFLOAT, true);
        vkDestroySampler(s.ctx.device, t.history.sampler, nullptr);
        t.history.sampler = linear_sampler();
        // Kept in GENERAL for its whole life: it is a transfer destination once
        // a frame and a sampled source once a frame, and GENERAL is valid for
        // both. The alternative is two layout transitions per frame to save
        // nothing measurable on an image this size.
        s.ctx.transition(t.history, VK_IMAGE_LAYOUT_GENERAL);
        // The finest flow level is written as a storage image and read as a
        // sampled one. Same VkImage and view, two descriptors; the alias does
        // not own the image, so it must never be handed to ctx.destroy.
        t.motion_sampler = linear_sampler();
        t.flow0_sampled = t.flow[0];
        t.flow0_sampled.sampler = t.motion_sampler;
        // Single channel and float: a depth buffer's value is all this reads, and
        // the blit converts whatever the game hands over.
        t.depth = s.ctx.image(mw, mh, VK_FORMAT_R32_SFLOAT, true);
        vkDestroySampler(s.ctx.device, t.depth.sampler, nullptr);
        t.depth.sampler = linear_sampler();
        s.ctx.transition(t.depth, VK_IMAGE_LAYOUT_GENERAL);
        t.params = s.ctx.buffer(32);

        for (unsigned p = 0; p < 2; ++p) {
            for (unsigned k = 0; k < kTemporalLevels; ++k) {
                // Binding 1 is the finer level this one halves; in mode 0 it is
                // not read, and the coarsest level is bound there as a valid
                // descriptor that nothing touches.
                auto* src = k ? &t.luma[p][k - 1] : &t.luma[p][kTemporalLevels - 1];
                t.luma_kernel[p][k].create(s.ctx, (shaders / "motion_luma.spv").string(), {},
                                           sizeof(Temporal::LumaPush),
                                           {&s.tex_in, src, &t.luma[p][k]});
                // Likewise, the coarsest flow pass runs with first=1 and never
                // reads flow_in, so it may point at its own output.
                const bool first = k == kTemporalLevels - 1;
                t.flow_kernel[p][k].create(s.ctx, (shaders / "motion_estimate.spv").string(), {},
                                           sizeof(Temporal::FlowPush),
                                           {&t.luma[p][k], &t.luma[1 - p][k],
                                            first ? &t.flow[k] : &t.flow[k + 1], &t.flow[k]});
            }
        }
        t.pre.create(s.ctx, (shaders / "temporal_pre_fp32.spv").string(),
                     {s.act.handle, s.act.handle, s.wgt.handle, s.wgt.handle, s.wgt.handle,
                      s.act.handle, t.params.handle},
                     sizeof(PushFSwin) + sizeof(PushPreImage),
                     {&s.tex_in, &t.flow0_sampled, &t.history, &t.depth});
        // The post variant reads the same parameter buffer, the motion field and
        // the history the pre variant does: its blend is gated by the same term.
        t.post.create(s.ctx, (shaders / "temporal_post_fp32.spv").string(),
                      {s.act.handle, s.act.handle, s.wgt.handle, s.wgt.handle, s.wgt.handle,
                       t.params.handle},
                      sizeof(PushFSwin) + sizeof(PushUps) + sizeof(PushImageTail),
                      {&s.surf0, &s.surf1, &s.tex_in, &t.flow0_sampled, &t.history});
        t.history_strength = temporal_config.history_strength;
        t.enabled = true;
        // One history per pass. Pass k reads and writes store[k] through the
        // bound history image; the copies are model-sized and cost nothing
        // next to a pass.
        if (config.max_passes > 1) {
            impl_->history_store.resize(config.max_passes);
            for (auto& h : impl_->history_store) {
                h = s.ctx.image(mw, mh, VK_FORMAT_R32G32B32A32_SFLOAT, false);
                s.ctx.transition(h, VK_IMAGE_LAYOUT_GENERAL);
            }
        }
    }
    if(transfer != Transfer::Identical) {
        for(VkFormat format : {transfer == Transfer::Encoded ? unorm : config.colour_format,
                               VK_FORMAT_R32G32B32A32_SFLOAT}) {
            VkFormatProperties properties{};vkGetPhysicalDeviceFormatProperties(host.physical,format,&properties);
            const VkFormatFeatureFlags need=VK_FORMAT_FEATURE_BLIT_SRC_BIT|VK_FORMAT_FEATURE_BLIT_DST_BIT;
            if((properties.optimalTilingFeatures&need)!=need)
                throw std::runtime_error("NR format conversion requires source/destination blit support");
        }
    }
    // Only the 8-bit path needs the private transfer image: it exists to keep an
    // *_SRGB view from applying its transfer function to the bits, not to
    // convert anything. The wide Wayland formats blit straight through.
    if(transfer == Transfer::Encoded) {
        auto& im=impl_->encoded;im.w=config.width;im.h=config.height;im.format=unorm;
        VkImageCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        info.imageType=VK_IMAGE_TYPE_2D;info.format=unorm;info.extent={im.w,im.h,1};
        info.mipLevels=info.arrayLayers=1;info.samples=VK_SAMPLE_COUNT_1_BIT;
        info.tiling=VK_IMAGE_TILING_OPTIMAL;
        info.usage=VK_IMAGE_USAGE_TRANSFER_SRC_BIT|VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        NRVK_CHECK(vkCreateImage(host.device,&info,nullptr,&im.handle));
        VkMemoryRequirements req{};vkGetImageMemoryRequirements(host.device,im.handle,&req);
        uint32_t type=s.ctx.mem.memoryTypeCount;
        for(uint32_t i=0;i<s.ctx.mem.memoryTypeCount;++i)
            if((req.memoryTypeBits&(1u<<i))&&(s.ctx.mem.memoryTypes[i].propertyFlags&VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) {type=i;break;}
        if(type==s.ctx.mem.memoryTypeCount)throw std::runtime_error("no device-local transfer image memory");
        VkMemoryAllocateInfo allocation{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        allocation.allocationSize=req.size;allocation.memoryTypeIndex=type;
        NRVK_CHECK(vkAllocateMemory(host.device,&allocation,nullptr,&im.memory));
        NRVK_CHECK(vkBindImageMemory(host.device,im.handle,im.memory,0));
        s.ctx.transition(im,VK_IMAGE_LAYOUT_GENERAL);
    }
    timer.mark("adapters");
    nr::logf("[nr] runtime %ux%u built in %.2fs: %s", config.width, config.height, timer.total(),
             timer.text().c_str());
}

Runtime::~Runtime() = default;

uint32_t Runtime::model_width() const { return impl_->mw; }
uint32_t Runtime::model_height() const { return impl_->mh; }

void Runtime::set_white_point(float v) {
    if (impl_->linear) impl_->white_point = v > 1e-4f ? v : 1e-4f;
}

void Runtime::set_history_strength(float v) {
    if (impl_->temporal.enabled) impl_->temporal.history_strength = v < 0.f ? 0.f : (v > 1.f ? 1.f : v);
}

void Runtime::release_feature(uint64_t feature) {
    if (!feature) return;
    auto it = impl_->features.find(feature);
    if (it == impl_->features.end()) return;
    if (it->second.history.handle)
        impl_->retiring.emplace_back(it->second.history,
                                     impl_->recordings + Impl::kRetireAfter);
    impl_->features.erase(it);
    // The bound history keeps whatever this feature left in it; the next
    // feature to record copies its own state over the top, and a feature that
    // is gone has no state to write back.
    if (impl_->bound_feature == feature) impl_->bound_feature = 0;
}

uint32_t Runtime::live_features() const { return uint32_t(impl_->features.size()); }

Runtime::TemporalHistoryView Runtime::temporal_history() const {
    TemporalHistoryView v{};
    const auto& t = impl_->temporal;
    if (!t.enabled) return v;
    v.image = t.history.handle; v.view = t.history.view;
    v.width = t.history.w; v.height = t.history.h;
    v.format = t.history.format; v.layout = t.history.layout;
    return v;
}

RecordResult Runtime::record(VkCommandBuffer cmd, const ColourFrame& frame, const Controls& c) {
    return record(cmd,frame,c,nullptr).frame;
}

ControlMaskResult Runtime::record(VkCommandBuffer cmd, const ColourFrame& frame, const Controls& c,
                             const ControlMaskFrame* mask) {
    return record_all(cmd,frame,c,mask,nullptr,nullptr,nullptr);
}

TemporalResult Runtime::record_temporal(VkCommandBuffer cmd, const ColourFrame& frame,
                                        const Controls& c, const TemporalFrame& temporal) {
    if (!impl_->temporal.enabled)
        throw std::invalid_argument("this NR runtime was not built with the temporal path");
    TemporalResult result{};
    bool consumed = false;
    result.frame = record_all(cmd,frame,c,nullptr,&temporal,&consumed,nullptr).frame;
    result.history_consumed = consumed;
    // The only provider that exists today. When the upscaler interception lands
    // it fills the same texture and this becomes "engine" - an estimate must
    // never be reported as engine data.
    result.motion_provider = "estimated";
    return result;
}

EngineResult Runtime::record_engine(VkCommandBuffer cmd, const EngineFrame& frame,
                                    const Controls& c) {
    auto& t = impl_->temporal;
    if (!t.enabled)
        throw std::invalid_argument("this NR runtime was not built with the temporal path");
    if (!frame.motion.image)
        throw std::invalid_argument("the engine provider requires a motion-vector image");
    EngineResult result{};
    bool consumed = false;
    // The engine's motion texture is blitted into the same image the fallback
    // estimator writes, so exactly one pipeline and one descriptor set serve
    // both providers. It costs one blit and it means the consumer cannot drift
    // between the two paths - which was the point of splitting them.
    TemporalFrame temporal{frame.reset, frame.feature};
    result.frame = record_all(cmd, frame.colour, c, nullptr, &temporal, &consumed, &frame).frame;
    result.history_consumed = consumed;
    result.motion_provider = "engine";
    return result;
}

ControlMaskResult Runtime::record_all(VkCommandBuffer cmd, const ColourFrame& frame, const Controls& c,
                             const ControlMaskFrame* mask, const TemporalFrame* temporal,
                             bool* history_consumed, const EngineFrame* engine) {
    validate(c);
    if (!cmd || !frame.image || frame.format != impl_->colour_format ||
        frame.width != impl_->width || frame.height != impl_->height ||
        !frame.before_stage || !frame.after_stage || frame.before == VK_IMAGE_LAYOUT_UNDEFINED ||
        frame.before == VK_IMAGE_LAYOUT_PREINITIALIZED || frame.after == VK_IMAGE_LAYOUT_UNDEFINED ||
        frame.after == VK_IMAGE_LAYOUT_PREINITIALIZED)
        throw std::invalid_argument("invalid NR colour frame");
    uint32_t mw=0,mh=0;
    if (mask) {
        const auto& t=mask->texture;
        mw=mask->width?mask->width:t.width;mh=mask->height?mask->height:t.height;
        if (!impl_->original_pre || !t.image || t.image==frame.image ||
            t.format!=VK_FORMAT_R32G32B32A32_SFLOAT ||
            t.width!=impl_->mask_image.w || t.height!=impl_->mask_image.h ||
            mask->x>=t.width || mask->y>=t.height || mw>t.width-mask->x || mh>t.height-mask->y ||
            !t.before_stage || !t.after_stage || t.before==VK_IMAGE_LAYOUT_UNDEFINED ||
            t.before==VK_IMAGE_LAYOUT_PREINITIALIZED || t.after==VK_IMAGE_LAYOUT_UNDEFINED ||
            t.after==VK_IMAGE_LAYOUT_PREINITIALIZED)
            throw std::invalid_argument("invalid/unconfigured external control mask");
    }
    ControlMaskResult result{};
    auto& info=result.frame;
    info.effective_skin = c.automatic_mask ? (c.skin_structure < 0 ? c.local_structure : c.skin_structure) : -1;
    info.effective_background = c.automatic_mask ? c.local_structure : -1;
    if (!c.enabled) {
        if (mask && mask->texture.before!=mask->texture.after) {
            const auto& t=mask->texture;
            barrier(cmd,t.image,t.before,t.after,t.before_stage,t.before_access,t.after_stage,t.after_access);
        }
        if (frame.before != frame.after)
            barrier(cmd, frame.image, frame.before, frame.after, frame.before_stage,
                    frame.before_access, frame.after_stage, frame.after_access);
        return result;
    }
    // Timed from here: a disabled pass records nothing and is not a cost.
    const uint32_t timing_slot = impl_->timing_slot;
    if (impl_->timing_ok) {
        vkCmdResetQueryPool(cmd, impl_->timing, timing_slot * 2, 2);
        vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, impl_->timing, timing_slot * 2);
    }
    auto& s = impl_->session;
    Controls effective=c;
    if (mask) {
        effective.automatic_mask=false;effective.intensity=1;
        info.effective_skin=info.effective_background=-1;
        result.consumed=true;
        const auto& t=mask->texture;
        auto& im=impl_->mask_image;
        const float rect[]={float(mask->x),float(mask->y),float(mw),float(mh),1.f/t.width,1.f/t.height};
        VkBufferMemoryBarrier b{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
        b.srcAccessMask=VK_ACCESS_SHADER_READ_BIT;b.dstAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;
        b.srcQueueFamilyIndex=b.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;
        b.buffer=impl_->mask_rect.handle;b.size=VK_WHOLE_SIZE;
        vkCmdPipelineBarrier(cmd,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,0,0,nullptr,1,&b,0,nullptr);
        vkCmdUpdateBuffer(cmd,impl_->mask_rect.handle,0,sizeof rect,rect);
        b.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;b.dstAccessMask=VK_ACCESS_SHADER_READ_BIT;
        vkCmdPipelineBarrier(cmd,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,0,0,nullptr,1,&b,0,nullptr);
        barrier(cmd,t.image,t.before,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,t.before_stage,t.before_access,
                VK_PIPELINE_STAGE_TRANSFER_BIT,VK_ACCESS_TRANSFER_READ_BIT);
        barrier(cmd,im.handle,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,VK_ACCESS_SHADER_READ_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_ACCESS_TRANSFER_WRITE_BIT);
        VkImageCopy copy{};copy.srcSubresource=copy.dstSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1};
        copy.extent={t.width,t.height,1};
        vkCmdCopyImage(cmd,t.image,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,im.handle,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,1,&copy);
        barrier(cmd,im.handle,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                VK_PIPELINE_STAGE_TRANSFER_BIT,VK_ACCESS_TRANSFER_WRITE_BIT,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,VK_ACCESS_SHADER_READ_BIT);
        barrier(cmd,t.image,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,t.after,VK_PIPELINE_STAGE_TRANSFER_BIT,
                VK_ACCESS_TRANSFER_READ_BIT,t.after_stage,t.after_access);
    }
    impl_->controls(effective);
    // The network extent (Model Resolution); the frame is frame.width x frame.height.
    const uint32_t nw = impl_->mw, nh = impl_->mh;
    const uint32_t passes = std::min<uint32_t>(std::max(effective.passes, 1), impl_->max_passes);
    barrier(cmd, frame.image, frame.before, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            frame.before_stage, frame.before_access, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_READ_BIT);
    barrier(cmd, s.tex_in.handle, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT,
            VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);
    VkImageCopy region{};
    region.srcSubresource = region.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.extent = {frame.width, frame.height, 1};
    VkImageBlit blit{};
    blit.srcSubresource=blit.dstSubresource=region.srcSubresource;
    blit.srcOffsets[1]=blit.dstOffsets[1]={int32_t(frame.width),int32_t(frame.height),1};
    auto& encoded=impl_->encoded;
    // Model Resolution: the frame lands in keep_full at its own size first,
    // then is filtered down into tex_in. Whatever the frame's format, the
    // downscale is one RGBA32F -> RGBA32F blit.
    VkImage colour_dst = s.tex_in.handle;
    if (impl_->scaled) {
        colour_dst = impl_->keep_full.handle;
        barrier(cmd, impl_->keep_full.handle, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT,
                VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);
    }
    if(encoded.handle) {
        barrier(cmd,encoded.handle,VK_IMAGE_LAYOUT_GENERAL,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                VK_PIPELINE_STAGE_TRANSFER_BIT,VK_ACCESS_TRANSFER_READ_BIT,
                VK_PIPELINE_STAGE_TRANSFER_BIT,VK_ACCESS_TRANSFER_WRITE_BIT);
        vkCmdCopyImage(cmd,frame.image,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                       encoded.handle,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,1,&region);
        barrier(cmd,encoded.handle,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                VK_PIPELINE_STAGE_TRANSFER_BIT,VK_ACCESS_TRANSFER_WRITE_BIT,
                VK_PIPELINE_STAGE_TRANSFER_BIT,VK_ACCESS_TRANSFER_READ_BIT);
        vkCmdBlitImage(cmd,encoded.handle,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                       colour_dst,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,1,&blit,VK_FILTER_NEAREST);
        barrier(cmd,encoded.handle,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,VK_IMAGE_LAYOUT_GENERAL,
                VK_PIPELINE_STAGE_TRANSFER_BIT,VK_ACCESS_TRANSFER_READ_BIT,
                VK_PIPELINE_STAGE_TRANSFER_BIT,VK_ACCESS_TRANSFER_READ_BIT|VK_ACCESS_TRANSFER_WRITE_BIT);
    } else if (impl_->transfer == Transfer::DirectBlit) {
        // FP16 / 16-bit UNORM / 10-bit packed: no sRGB transfer function to route
        // around, so the blit's own format conversion is the whole of it.
        vkCmdBlitImage(cmd, frame.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                       colour_dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit, VK_FILTER_NEAREST);
    } else {
        vkCmdCopyImage(cmd, frame.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                       colour_dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
    }
    if (impl_->scaled) {
        barrier(cmd, impl_->keep_full.handle, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
                VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_READ_BIT);
        VkImageBlit down{};
        down.srcSubresource = down.dstSubresource = region.srcSubresource;
        down.srcOffsets[1] = {int32_t(frame.width), int32_t(frame.height), 1};
        down.dstOffsets[1] = {int32_t(nw), int32_t(nh), 1};
        vkCmdBlitImage(cmd, impl_->keep_full.handle, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                       s.tex_in.handle, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &down, impl_->downscale_filter);
        barrier(cmd, impl_->keep_full.handle, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL,
                VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_READ_BIT,
                VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT);
    }
    if (impl_->linear) {
        // Linear light: make the proxy in place and keep the original. The
        // network reads tex_in in SHADER_READ_ONLY like every other frame.
        barrier(cmd, s.tex_in.handle, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL,
                VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
                VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT);
        struct { uint32_t w, h; float white, knee; } push{nw, nh, impl_->white_point, 0.75f};
        dispatch(cmd, impl_->encode, (nw + 7) / 8, (nh + 7) / 8, 1, &push, sizeof push);
        barrier(cmd, s.tex_in.handle, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT,
                VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT);
    } else {
        barrier(cmd, s.tex_in.handle, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
                VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT);
    }
    // A GENERAL -> GENERAL copy between two of our own images, fenced for the
    // compute work on either side. Model-sized, used by the multi-pass loop.
    auto copy_general = [&](const nrvk::Context::Image& src, const nrvk::Context::Image& dst) {
        barrier(cmd, src.handle, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL,
                VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT,
                VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_READ_BIT);
        barrier(cmd, dst.handle, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL,
                VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT,
                VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);
        VkImageCopy c{};
        c.srcSubresource = c.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        c.extent = {src.w, src.h, 1};
        vkCmdCopyImage(cmd, src.handle, VK_IMAGE_LAYOUT_GENERAL, dst.handle, VK_IMAGE_LAYOUT_GENERAL, 1, &c);
        barrier(cmd, dst.handle, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL,
                VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
                VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT);
        barrier(cmd, src.handle, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL,
                VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_READ_BIT,
                VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT);
    };
    if (impl_->max_passes > 1) {
        // What the first pass sees, for the transfer pass and the alpha
        // restore; tex_in is overwritten by every pass after the first.
        barrier(cmd, s.tex_in.handle, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT,
                VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_READ_BIT);
        barrier(cmd, impl_->shown_keep.handle, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL,
                VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT,
                VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);
        VkImageCopy c{};
        c.srcSubresource = c.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        c.extent = {nw, nh, 1};
        vkCmdCopyImage(cmd, s.tex_in.handle, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                       impl_->shown_keep.handle, VK_IMAGE_LAYOUT_GENERAL, 1, &c);
        barrier(cmd, impl_->shown_keep.handle, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL,
                VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
                VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT);
        barrier(cmd, s.tex_in.handle, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_READ_BIT,
                VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT);
    }
    compute_barrier(cmd);
    auto& t = impl_->temporal;
    // This frame's feature, if the caller named one. Its state is host-side
    // bookkeeping plus one image; nothing is recorded yet.
    Impl::FeatureState* fs = nullptr;
    if (temporal && t.enabled && temporal->feature) {
        auto it = impl_->features.find(temporal->feature);
        if (it == impl_->features.end()) {
            Impl::FeatureState made{};
            // No settling transition: that is a submit and a fence wait, and
            // this is the render thread inside a recording. The barrier below
            // does it in the command buffer instead.
            made.history = s.ctx.image(t.history.w, t.history.h, t.history.format, false, false);
            it = impl_->features.emplace(temporal->feature, made).first;
        }
        fs = &it->second;
    }
    bool gate = false;
    if (temporal) {
        // The original's gate, all three terms, resolved here and nowhere else:
        // the first-frame latch, DLSSNR.Reset, and a motion source existing at
        // all.
        gate = (fs ? fs->latch : t.latch) && !temporal->reset;
        const uint32_t p = fs ? fs->parity : t.parity;
        if (engine) {
            // Engine motion: one blit into the shared motion image, converting
            // whatever format the game handed us. No pyramid, no search - this
            // is the whole difference between the two providers.
            barrier(cmd, engine->motion.image, engine->motion.before,
                    VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, engine->motion.before_stage,
                    engine->motion.before_access, VK_PIPELINE_STAGE_TRANSFER_BIT,
                    VK_ACCESS_TRANSFER_READ_BIT);
            barrier(cmd, t.flow[0].handle, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL,
                    VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT,
                    VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);
            VkImageBlit mv{};
            mv.srcSubresource = mv.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            mv.srcOffsets[1] = {int32_t(engine->motion.width), int32_t(engine->motion.height), 1};
            mv.dstOffsets[1] = {int32_t(t.lw[0]), int32_t(t.lh[0]), 1};
            vkCmdBlitImage(cmd, engine->motion.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                           t.flow[0].handle, VK_IMAGE_LAYOUT_GENERAL, 1, &mv, VK_FILTER_LINEAR);
            barrier(cmd, t.flow[0].handle, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL,
                    VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
                    VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT);
            barrier(cmd, engine->motion.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                    engine->motion.after, VK_PIPELINE_STAGE_TRANSFER_BIT,
                    VK_ACCESS_TRANSFER_READ_BIT, engine->motion.after_stage,
                    engine->motion.after_access);
            // Depth, the same way and for the same reason: one blit into one
            // image, so the consumer has a single descriptor whether or not the
            // game supplies depth this frame.
            if (engine->depth.image) {
                barrier(cmd, engine->depth.image, engine->depth.before,
                        VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, engine->depth.before_stage,
                        engine->depth.before_access, VK_PIPELINE_STAGE_TRANSFER_BIT,
                        VK_ACCESS_TRANSFER_READ_BIT);
                barrier(cmd, t.depth.handle, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL,
                        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT,
                        VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);
                VkImageBlit d{};
                // A depth image's aspect is DEPTH, not COLOR, and blitting with
                // the wrong one is a validation error rather than a wrong
                // picture. The game tells us which by the format it handed over.
                const bool depth_aspect =
                    engine->depth.format == VK_FORMAT_D32_SFLOAT ||
                    engine->depth.format == VK_FORMAT_D32_SFLOAT_S8_UINT ||
                    engine->depth.format == VK_FORMAT_D24_UNORM_S8_UINT ||
                    engine->depth.format == VK_FORMAT_D16_UNORM ||
                    engine->depth.format == VK_FORMAT_X8_D24_UNORM_PACK32;
                d.srcSubresource = {VkImageAspectFlags(depth_aspect ? VK_IMAGE_ASPECT_DEPTH_BIT
                                                                    : VK_IMAGE_ASPECT_COLOR_BIT), 0, 0, 1};
                d.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
                d.srcOffsets[1] = {int32_t(engine->depth.width), int32_t(engine->depth.height), 1};
                d.dstOffsets[1] = {int32_t(t.depth.w), int32_t(t.depth.h), 1};
                // NEAREST, not LINEAR: interpolating across a silhouette invents
                // a depth that is on neither surface, which is precisely the
                // edge this tap exists to get right. Vulkan also forbids linear
                // filtering on a depth aspect.
                vkCmdBlitImage(cmd, engine->depth.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                               t.depth.handle, VK_IMAGE_LAYOUT_GENERAL, 1, &d, VK_FILTER_NEAREST);
                barrier(cmd, t.depth.handle, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL,
                        VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
                        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT);
                barrier(cmd, engine->depth.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                        engine->depth.after, VK_PIPELINE_STAGE_TRANSFER_BIT,
                        VK_ACCESS_TRANSFER_READ_BIT, engine->depth.after_stage,
                        engine->depth.after_access);
            }
        }
        for (unsigned k = 0; !engine && k < kTemporalLevels; ++k) {
            Temporal::LumaPush lp{t.lw[k], t.lh[k],
                                  k ? t.lw[k-1] : nw, k ? t.lh[k-1] : nh,
                                  k ? 1u : 0u};
            dispatch(cmd, t.luma_kernel[p][k], (t.lw[k]+7)/8, (t.lh[k]+7)/8, 1, &lp, sizeof lp);
            compute_barrier(cmd);
        }
        if (gate && !engine) {
            for (int k = int(kTemporalLevels) - 1; k >= 0; --k) {
                const bool first = unsigned(k) == kTemporalLevels - 1, last = k == 0;
                Temporal::FlowPush fp{t.lw[k], t.lh[k],
                                      first ? 1u : t.lw[k+1], first ? 1u : t.lh[k+1],
                                      kTemporalRadius[k], first ? 1u : 0u, last ? 1u : 0u,
                                      kTemporalReject};
                dispatch(cmd, t.flow_kernel[p][k], (t.lw[k]+7)/8, (t.lh[k]+7)/8, 1, &fp, sizeof fp);
                compute_barrier(cmd);
            }
        }
        // The estimator writes normalized backward motion, so the consumer's
        // MVecScale pair is 1.0.
        const bool have_depth = engine && engine->depth.image;
        const float params[8] = {gate ? 1.0f : 0.0f,
                                 engine ? engine->motion_scale_x : 1.0f,
                                 engine ? engine->motion_scale_y : 1.0f,
                                 t.history_strength,
                                 float(t.history.w), float(t.history.h),
                                 have_depth ? 1.0f : 0.0f,
                                 have_depth && engine->depth_inverted ? 1.0f : 0.0f};
        VkBufferMemoryBarrier b{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
        b.srcAccessMask=VK_ACCESS_SHADER_READ_BIT;b.dstAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;
        b.srcQueueFamilyIndex=b.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;
        b.buffer=t.params.handle;b.size=VK_WHOLE_SIZE;
        vkCmdPipelineBarrier(cmd,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,0,0,nullptr,1,&b,0,nullptr);
        vkCmdUpdateBuffer(cmd,t.params.handle,0,sizeof params,params);
        b.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;b.dstAccessMask=VK_ACCESS_SHADER_READ_BIT;
        vkCmdPipelineBarrier(cmd,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,0,0,nullptr,1,&b,0,nullptr);
    }
    // Bind this feature's history: copy the previously bound feature's state out
    // and this one's in. Only on a change, so the single-feature case records
    // nothing at all here.
    if (fs && impl_->bound_feature != temporal->feature) {
        if (impl_->bound_feature) {
            auto prev = impl_->features.find(impl_->bound_feature);
            if (prev != impl_->features.end()) copy_general(t.history, prev->second.history);
        }
        if (!fs->cleared) {
            // A fresh feature's image is whatever the allocator left there. The
            // gate is closed on its first frame so nothing reads it, but an
            // unwritten float image can hold NaN and a NaN multiplied by a zero
            // gate is still NaN. This is also where it reaches GENERAL, which
            // it will stay in for the rest of its life.
            barrier(cmd, fs->history.handle, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL,
                    VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, 0,
                    VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);
            VkClearColorValue zero{};
            VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
            vkCmdClearColorImage(cmd, fs->history.handle, VK_IMAGE_LAYOUT_GENERAL, &zero, 1, &range);
            barrier(cmd, fs->history.handle, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL,
                    VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
                    VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_READ_BIT);
            fs->history.layout = VK_IMAGE_LAYOUT_GENERAL;
            fs->cleared = true;
        }
        copy_general(fs->history, t.history);
        impl_->bound_feature = temporal->feature;
    }
    // The passes. Pass k > 0 takes pass k-1's answer (surf0, the blended
    // output) as its colour, the same motion and depth, and its own history:
    // store[k] is copied into the bound history image before the pass and
    // the model's write (surf1) copied back after it. With one pass the
    // history round trip is the single copy it always was.
    for (uint32_t pass = 0; pass < passes; ++pass) {
        if (pass > 0 && impl_->cascade_detail) {
            // Detail only: the first pass's input with the previous pass's
            // local structure on it (windows/shaders/passes/cascade_feed.comp).
            barrier(cmd, s.tex_in.handle, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL,
                    VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT,
                    VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT);
            struct { uint32_t w, h; float floor_; } lp{nw, nh, 1.0f / 512.0f};
            dispatch(cmd, impl_->cascade_lograt, (nw + 7) / 8, (nh + 7) / 8, 1, &lp, sizeof lp);
            compute_barrier(cmd);
            struct { uint32_t w, h; int32_t radius; uint32_t dir; } bp{nw, nh, impl_->cascade_radius, 0u};
            dispatch(cmd, impl_->cascade_blur_h, (nw + 7) / 8, (nh + 7) / 8, 1, &bp, sizeof bp);
            compute_barrier(cmd);
            bp.dir = 1u;
            dispatch(cmd, impl_->cascade_blur_v, (nw + 7) / 8, (nh + 7) / 8, 1, &bp, sizeof bp);
            compute_barrier(cmd);
            struct { uint32_t w, h; float guard; } fp{nw, nh, 2.0f};
            dispatch(cmd, impl_->cascade_feed, (nw + 7) / 8, (nh + 7) / 8, 1, &fp, sizeof fp);
            barrier(cmd, s.tex_in.handle, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                    VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT,
                    VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT);
            compute_barrier(cmd);
            if (temporal) copy_general(impl_->history_store[pass], t.history);
        } else if (pass > 0) {
            barrier(cmd, s.surf0.handle, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                    VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT,
                    VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_READ_BIT);
            barrier(cmd, s.tex_in.handle, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                    VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT,
                    VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);
            VkImageCopy c{};
            c.srcSubresource = c.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            c.extent = {nw, nh, 1};
            vkCmdCopyImage(cmd, s.surf0.handle, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                           s.tex_in.handle, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &c);
            barrier(cmd, s.tex_in.handle, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                    VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
                    VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT);
            barrier(cmd, s.surf0.handle, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL,
                    VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_READ_BIT,
                    VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT);
            if (temporal) copy_general(impl_->history_store[pass], t.history);
        }
        for (size_t i = 0; i < s.steps.size(); ++i) {
            const auto& step = s.steps[i];
            const nrvk::Kernel* use = step.k;
            if (mask && step.k==impl_->original_pre) use = &impl_->mask_pre;
            else if (temporal && step.k==t.original_pre) use = &t.pre;
            else if (temporal && step.k==t.original_post) use = &t.post;
            // Each pass has its own controls; see `pass_push`. An empty entry
            // means that dispatch carries none, so pass 1's push stands.
            const void* push = step.push;
            if (pass > 0 && pass < impl_->pass_push.size() &&
                i < impl_->pass_push[pass].size() && !impl_->pass_push[pass][i].empty())
                push = impl_->pass_push[pass][i].data();
            dispatch(cmd, *use, step.gx, step.gy, step.gz, push, step.push_bytes);
            // Execution-only between network steps when the arena is coherent,
            // invalidate-only when it is not; the barrier after the last step
            // stays full, because the passes that follow it are not part of
            // either contract - they were not built with device-scoped loads
            // and the arena is not the only thing they read.
            const bool last = i + 1 >= s.steps.size();
            compute_barrier(cmd, s.runner.exec_barrier && !last,
                            s.runner.inv_barrier && !last);
        }
        if (temporal) {
            // History is a copy of what the MODEL wrote - the temporal post variant
            // puts that on the second surface, before the application strength -
            // and it is taken independently of whatever composition follows.
            const nrvk::Context::Image& dst = passes > 1 ? impl_->history_store[pass] : t.history;
            barrier(cmd, s.surf1.handle, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                    VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT,
                    VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_READ_BIT);
            barrier(cmd, dst.handle, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL,
                    VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT,
                    VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);
            VkImageCopy history_copy{};
            history_copy.srcSubresource = history_copy.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT,0,0,1};
            history_copy.extent = {nw, nh, 1};
            vkCmdCopyImage(cmd, s.surf1.handle, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                           dst.handle, VK_IMAGE_LAYOUT_GENERAL, 1, &history_copy);
            barrier(cmd, dst.handle, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL,
                    VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
                    VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT);
            barrier(cmd, s.surf1.handle, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL,
                    VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_READ_BIT,
                    VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                    VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT);
        }
    }
    if (temporal) {
        // Leave the bound history holding the FIRST pass's, which is what the
        // next frame's first pass reads.
        if (passes > 1) copy_general(impl_->history_store[0], t.history);
        // The latch is set after every recorded frame and `reset` never clears
        // it: reset suppresses consumption, it does not un-write the buffer.
        if (fs) { fs->latch = true; fs->parity ^= 1u; }
        else { t.latch = true; t.parity ^= 1u; }
        if (history_consumed) *history_consumed = gate;
    }
    // Retired features, destroyed once enough recordings have gone by that the
    // submission holding their last use cannot still be executing.
    ++impl_->recordings;
    for (size_t i = impl_->retiring.size(); i-- > 0;) {
        if (impl_->recordings < impl_->retiring[i].second) continue;
        s.ctx.destroy(impl_->retiring[i].first);
        impl_->retiring.erase(impl_->retiring.begin() + long(i));
    }
    info.network_dispatches = uint32_t(s.steps.size()) * passes;
    if (c.apply_model) {
        if (mask) {
            struct { uint32_t w,h;float intensity; } push{frame.width,frame.height,c.intensity};
            dispatch(cmd,impl_->mask_resolve,(frame.width+7)/8,(frame.height+7)/8,1,&push,sizeof push);
            compute_barrier(cmd);
        }
        if (!mask && !impl_->native_compose) {
            // The model's answer onto the frame, as OptiScaler DLSS-NR's
            // resolve; see the shader. At the frame extent; the model-sized
            // answer is sampled.
            struct { uint32_t w, h, model_w, model_h, passthrough; float detail, colour, max_ratio, white; }
                push{frame.width, frame.height, nw, nh, impl_->linear ? 0u : 1u,
                     c.detail_strength, c.colour_strength, c.max_ratio, impl_->white_point};
            dispatch(cmd, impl_->transfer_pass, (frame.width + 7) / 8, (frame.height + 7) / 8, 1, &push, sizeof push);
            compute_barrier(cmd);
        }
        const VkImage out = impl_->scaled ? impl_->full_out.handle : s.surf0.handle;
        const uint32_t extent[] = {frame.width, frame.height, uint32_t(encoded.handle != VK_NULL_HANDLE)};
        dispatch(cmd, impl_->alpha, (frame.width + 7) / 8, (frame.height + 7) / 8, 1, extent, sizeof extent);
        barrier(cmd, out, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT,
                VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_READ_BIT);
        barrier(cmd, frame.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_READ_BIT,
                VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);
        if(encoded.handle) {
            barrier(cmd,encoded.handle,VK_IMAGE_LAYOUT_GENERAL,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                    VK_PIPELINE_STAGE_TRANSFER_BIT,VK_ACCESS_TRANSFER_READ_BIT,
                    VK_PIPELINE_STAGE_TRANSFER_BIT,VK_ACCESS_TRANSFER_WRITE_BIT);
            vkCmdBlitImage(cmd,out,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                           encoded.handle,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,1,&blit,VK_FILTER_NEAREST);
            barrier(cmd,encoded.handle,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                    VK_PIPELINE_STAGE_TRANSFER_BIT,VK_ACCESS_TRANSFER_WRITE_BIT,
                    VK_PIPELINE_STAGE_TRANSFER_BIT,VK_ACCESS_TRANSFER_READ_BIT);
            vkCmdCopyImage(cmd,encoded.handle,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                           frame.image,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,1,&region);
            barrier(cmd,encoded.handle,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,VK_IMAGE_LAYOUT_GENERAL,
                    VK_PIPELINE_STAGE_TRANSFER_BIT,VK_ACCESS_TRANSFER_READ_BIT,
                    VK_PIPELINE_STAGE_TRANSFER_BIT,VK_ACCESS_TRANSFER_READ_BIT|VK_ACCESS_TRANSFER_WRITE_BIT);
        } else if (impl_->transfer == Transfer::DirectBlit) {
            vkCmdBlitImage(cmd, out, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                           frame.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit, VK_FILTER_NEAREST);
        } else {
            vkCmdCopyImage(cmd, out, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                           frame.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
        }
        barrier(cmd, out, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL,
                VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_READ_BIT,
                VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT);
        info.applied = true;
    }
    barrier(cmd, frame.image, c.apply_model ? VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL : VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            frame.after, VK_PIPELINE_STAGE_TRANSFER_BIT,
            c.apply_model ? VK_ACCESS_TRANSFER_WRITE_BIT : VK_ACCESS_TRANSFER_READ_BIT,
            frame.after_stage, frame.after_access);
    if (impl_->timing_ok) {
        vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, impl_->timing,
                            timing_slot * 2 + 1);
        ++impl_->timings_recorded;
        impl_->timing_slot = (timing_slot + 1) % Impl::kTimingSlots;
        // Collect whatever has finished. Never waits; the caller is a render
        // thread and this is a diagnostic.
        impl_->read_timing();
    }
    return result;
}

float Runtime::last_gpu_ms() const { return impl_->gpu_ms; }
float Runtime::average_gpu_ms() const { return impl_->gpu_ms_avg; }
}  // namespace nr
