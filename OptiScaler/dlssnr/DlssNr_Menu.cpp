#include "pch.h"
#include "amd/PresentExperimental.h"
#include "amd/AmdBridge.h"
#include "DlssNrFeature_Vk.h"

#include "DlssNr.h"
#include "DlssNr_ExposureScan.h"

#include <Config.h>
#include <hooks/D3D12_Hooks.h>
#include <menu/menu_common.h>

#include <imgui/imgui.h>

#include <string>
#include <unordered_map>
#include <algorithm>
#include <cmath>
#include <cstdio>

namespace DlssNr
{

// The "(?)" marker every control carries, matching the rest of the menu.
static void HelpMarker(const char* tip)
{
    ImGui::SameLine();
    ImGui::TextDisabled("(?)");

    if (ImGui::IsItemHovered())
    {
        ImGui::BeginTooltip();
        ImGui::PushTextWrapPos(ImGui::GetFontSize() * 40.0f);
        ImGui::TextUnformatted(tip);
        ImGui::PopTextWrapPos();
        ImGui::EndTooltip();
    }
}

// Horizontal air between same-row controls. Checkbox labels already include
// ItemSpacing; these gaps keep version / combo / checkbox from colliding.
static void HGap(float em)
{
    ImGui::SameLine();
    ImGui::Dummy(ImVec2(ImGui::GetFontSize() * em, 0.0f));
    ImGui::SameLine();
}

// A slider that only writes its value when the handle is released.
//
// Some controls -- intensity, the structure and tone strengths -- are read by the model once, when
// the feature is built, so changing one rebuilds the whole feature. Writing on every pixel of a drag
// meant a rebuild per frame, felt as the picture hitching while you scrub. The slider still tracks
// live under the cursor; only the commit that triggers the rebuild waits for release. Cheap controls
// that are just shader constants (detail, colour, paper white) do not use this -- they can afford to
// apply live.
template <typename Option>
static bool DeferredSlider(const char* label, Option* opt, float mn, float mx, float def, const char* fmt = "%.2f",
                           bool inheritReset = false)
{
    static std::unordered_map<ImGuiID, float> pending;
    const ImGuiID id = ImGui::GetID(label);

    auto it = pending.find(id);
    float value = it != pending.end() ? it->second : (opt->has_value() ? opt->value() : def);
    bool changed = false;

    if (ImGui::SliderFloat(label, &value, mn, mx, fmt))
        pending[id] = value;

    if (ImGui::IsItemDeactivatedAfterEdit())
    {
        auto committed = pending.find(id);

        if (committed != pending.end())
        {
            *opt = std::clamp(committed->second, mn, mx);
            pending.erase(committed);
            changed = true;
        }
    }

    ImGui::SameLine();

    const std::string resetId = std::string("Reset##") + label;
    if (ImGui::SmallButton(resetId.c_str()))
    {
        if (inheritReset)
            *opt = std::optional<float> {};
        else
            *opt = def;
        pending.erase(id);
        changed = true;
    }

    return changed;
}

// An absent later-pass setting inherits pass 1. The first combo item represents that absence; the
// remaining items map directly to the model's zero-based profile values.
static bool InheritedProfileCombo(const char* label, CustomOptional<uint32_t, NoDefault>* opt, const char* const* names,
                                  int nameCount)
{
    int selected = 0;

    if (opt->has_value())
        selected = std::clamp((int) opt->value(), 0, nameCount - 2) + 1;

    if (!ImGui::Combo(label, &selected, names, nameCount))
        return false;

    if (selected == 0)
        *opt = std::optional<uint32_t> {};
    else
        *opt = (uint32_t) (selected - 1);

    return true;
}

// One per-pass control: a checkbox that decides whether this pass has an opinion, and the slider it
// enables. Unchecked follows the global setting, which is what an untouched pass does.
static bool PassOverrideSlider(const char* label, std::optional<float>* own, float global, float mn, float mx, int pass)
{
    bool changed = false;
    bool has = own->has_value();

    const std::string useId = std::string("##use") + label + std::to_string(pass);

    if (ImGui::Checkbox(useId.c_str(), &has))
    {
        if (has)
            *own = global;
        else
            own->reset();

        changed = true;
    }

    ImGui::SameLine();
    ImGui::BeginDisabled(!has);

    float value = own->value_or(global);
    const std::string sliderId = std::string(label) + "##" + std::to_string(pass);

    if (ImGui::SliderFloat(sliderId.c_str(), &value, mn, mx, "%.2f") && has)
    {
        *own = value;
        changed = true;
    }

    ImGui::EndDisabled();

    if (!has)
    {
        ImGui::SameLine();
        ImGui::TextDisabled("global");
    }

    return changed;
}

void RenderMenu(Config* config, float menuResScale)
{

    // DLSS Neural Rendering -----------------------------
    ImGui::Spacing();
    // Open from the start: it is the whole of the Neural tab.
    if (auto ch = ScopedCollapsingHeader("DLSS Neural Rendering", ImGuiTreeNodeFlags_DefaultOpen); ch.IsHeaderOpen())
    {
        ScopedIndent indent {};
        ImGui::Spacing();

        bool enabled = config->DlssNrEnabled.value_or_default();
        if (ImGui::Checkbox("Enable NR", &enabled))
            config->DlssNrEnabled = enabled;

        if (DlssNr::AmdBridge::HasFiles())
        {
            // Runtime name belongs with Enable NR — tight pair, not a separate group.
            const char* ver = DlssNr::AmdBridge::RuntimeName();
            const bool haveVer = ver && *ver;
            HGap(0.12f);
            ImGui::TextDisabled("%s", haveVer ? ver : "pass1?");
            HelpMarker(haveVer ? "AMD NR runtime (original project / original author)."
                               : "AMD NR runtime: pass1 not identified yet.");

            // Stored as [DlssNr] AmdEveryFrame, the key's original name, so existing INIs keep working.
            // It switches off the model's temporal history.
            bool noTemporal = config->AmdEveryFrame.value_or_default();
            if (ImGui::Checkbox("Disable temporal stabilization", &noTemporal))
                config->AmdEveryFrame = noTemporal;
            HelpMarker("Off (default): the model keeps its temporal history, building each frame"
                       "\non the ones before it, which steadies the image. The upscaler accumulates"
                       "\ntoo, so some ghosting is possible."
                       "\n\nOn: every frame is processed on its own, with no history. Less ghosting,"
                       "\nmore flicker and shimmer. With the INI-only single slot (AmdSlots=1) the"
                       "\nrender thread also waits for the model every frame."
                       "\n\nChanging it restarts the model's history.");

            // Slots first, then New wait — quantity next to the enable row, wait
            // mode after it. Combo is a narrow digit control, not a full-width bar.
            // Menu offers 2-5 only; the ini also accepts 1 (old single-slot path).
            const int stored = std::clamp(config->AmdSlots.value_or_default(), 1, 5);
            const int shown = std::clamp(stored, 2, 5);
            char slotPreview[8] {};
            std::snprintf(slotPreview, sizeof(slotPreview), "%d", shown);

            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted("NR slots");
            HGap(0.15f);
            // Width fits one digit plus the arrow, with padding — not a full-width
            // bar, and not so tight that the arrow covers the number.
            {
                const float digitW = ImGui::CalcTextSize(slotPreview).x;
                const float arrowW = ImGui::GetFrameHeight();
                const float padX = ImGui::GetStyle().FramePadding.x;
                const float comboW = digitW + arrowW + padX * 4.0f;
                ImGui::SetNextItemWidth(std::max(comboW, ImGui::GetFontSize() * 3.2f));
            }
            if (ImGui::BeginCombo("##AmdSlots", slotPreview))
            {
                for (int s = 2; s <= 5; ++s)
                {
                    char item[8] {};
                    std::snprintf(item, sizeof(item), "%d", s);
                    if (ImGui::Selectable(item, shown == s))
                        config->AmdSlots = s;
                }
                ImGui::EndCombo();
            }
            HelpMarker("How many frames may be running denoise at once, 2-5. A frame that gets"
                       "\na buffer waits for its own denoise; one that finds all buffers busy is"
                       "\nrecorded with NO denoise at all - faster, with possible quality loss.\n"
                       "\n3 (default): on Onimusha no difference from 2 was detected. In one Where"
                       "\nWinds Meet A/B session, the skip counter rose by about 1200-1440 per"
                       "\ntwo-slot segment and stayed flat with 3; that log window does not yield"
                       "\na skip percentage.\n"
                       "\n2: in that Where Winds Meet session, display latency was 47.6-47.9 ms"
                       "\nversus 62.9-63.2 ms with 3, but many frames skipped denoise.\n"
                       "\n4-5: measured in a separate sweep and no faster than 3 in that scene. A"
                       "\nscene that actually requires a fourth or fifth slot has not been tested.\n"
                       "\nEach buffer is one FP16 target at the RENDER size (the DLSS input): about"
                       "\n29 MB when a 4K output renders at 1440p, 66 MB only at a native 4K render."
                       "\nOnly the selected number is allocated. No restart needed.\n"
                       "\nThe ini also accepts 1 (the old single-slot path); this menu does not.");
            if (stored < 2)
            {
                // Own line under the slot control; New wait starts below it.
                ImGui::TextDisabled("(ini has NR slots = 1: single-slot mode, not selectable here)");
            }
            else
            {
                HGap(0.65f);
            }

            bool newWait = config->AmdGraphicsWait.value_or_default() != 0;
            const bool hooksArmed = D3D12Hooks::IsAmdGraphicsTrackerArmed();
            const bool restartToTryNewWait =
                !hooksArmed ||
                DlssNr::AmdBridge::GraphicsRestartNeeded(std::clamp(config->DlssNrPasses.value_or_default(), 1u, 3u));
            const bool restartNeeded = newWait && restartToTryNewWait;
            if (ImGui::Checkbox(restartNeeded ? "New wait mode (restart)" : "New wait mode", &newWait))
            {
                config->AmdGraphicsWait = newWait ? 1 : 0;
                if (newWait && restartToTryNewWait)
                    ImGui::OpenPopup("New wait restart");
            }
            HelpMarker("On: New wait mode (0.3.1 1-pixel draw). Still being tested."
                       "\nOff: Original wait mode (switches immediately)."
                       "\nRestart if prompted: hooks or a pass may not be ready for new wait mode."
                       "\nFrames that cannot use new wait mode still fall back to original wait.");
            if (ImGui::BeginPopupModal("New wait restart", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
            {
                ImGui::TextUnformatted("Some NR processing still uses original wait.");
                ImGui::TextUnformatted("Restart the game to retry new-wait initialization.");
                if (ImGui::Button("OK"))
                    ImGui::CloseCurrentPopup();
                ImGui::EndPopup();
            }
        }

        if (AmdPresentExperimental::IsTarget())
        {
            ImGui::TextWrapped(
                "Experimental final-image neural: synthetic motion/depth, no temporal history. Includes game HUD.");
            ImGui::TextUnformatted("One pass, 100% image resolution. Restart after resizing the output.");
            float strength = config->AmdNeuralLightingStrength.value_or_default();
            if (ImGui::SliderFloat("Lightning Strength", &strength, 0.f, 1.f))
                config->AmdNeuralLightingStrength = strength;
            ImGui::TextWrapped("%s", AmdPresentExperimental::Status().c_str());
            return;
        }

        if (DlssNr::AmdBridge::HasFiles())
        {
            if (const float ms = DlssNr::AmdBridge::NeuralMs(); ms > 0 && config->DlssNrEnabled.value_or_default())
            {
                ImGui::TextColored(ImVec4(0.4f, 0.9f, 0.5f, 1.0f),
                                   "Neural pass: %.1f ms per frame (%.0f fps on its own)", ms, 1000.f / ms);
                HelpMarker("GPU time the game's queue spends on the model each frame, all passes"
                           "\ntogether: copying its inputs, waiting for its result and applying it."
                           "\n\nThe fps is 1000 divided by that time: how many frames per second the"
                           "\nmodel alone could keep up with. The game runs slower than that, because"
                           "\nthe rest of the frame takes time too.");
            }

            // Where the model sits, and what its resolution is a percentage OF. This line used to be
            // fixed text claiming "before Super Resolution" whichever placement was running, while
            // the switch that chose it sat in a branch this backend never reaches -- so the
            // percentage below had no stated frame of reference at all.
            //
            // ApplyAfterRR alone decides it, the same key EvaluateAtSeam reads. RunBeforeSR is not
            // consulted on this backend.
            int placement = config->DlssNrApplyAfterRR.value_or_default() ? 1 : 0;

            if (ImGui::Combo("Processing point", &placement, "Before Super Resolution\0After the finished frame\0"))
                config->DlssNrApplyAfterRR = (placement != 0);

            HelpMarker(placement == 0 ? "The model is handed the render-resolution frame the upscaler is about to"
                                        "\nread, so the percentage below is of that render resolution. The upscaler"
                                        "\nthen enlarges the model\'s work along with everything else."
                                        "\n\nA game driving Ray Reconstruction never reaches this point: RR denoises"
                                        "\nand enlarges in one dispatch, leaving no seam before it. Pick the other"
                                        "\nplacement there, or the model will not run at all."
                                      : "The model is handed the finished frame, and the percentage below is of"
                                        "\nthat frame: at 50% a 4K picture is reduced to 1080p, the model runs on"
                                        "\nit, and its answer is enlarged back to fill the frame."
                                        "\n\nThis is the placement Ray Reconstruction titles need, and the only one"
                                        "\nthey can reach.");

            // A Ray Reconstruction title has no seam before the upscaler, and the pass declines the
            // one after it unless this switch picks it, so in this placement the model does not run.
            if (placement == 0)
            {
                if (auto feature = State::Instance().currentFeature; feature != nullptr)
                {
                    const auto kind = feature->GetUpscalerType();
                    if (kind == Upscaler::DLSSD || kind == Upscaler::FSR_RR)
                        ImGui::TextColored(ImVec4(0.95f, 0.70f, 0.20f, 1.0f),
                                           "This title drives Ray Reconstruction, which has no seam before it:"
                                           "\nthe model is not running. Choose \"After the finished frame\".");
                }
            }

            // The stored value is 1 Linear, 2 sRGB, 3 Gamma 2.2, and the combo index is one below it.
            // An old INI's 0 (Auto, which converted nothing) shows as Linear.
            int encoding = std::clamp(config->AmdEncoding.value_or_default(), 1, 3) - 1;
            if (ImGui::Combo("Encoding", &encoding, "Linear\0sRGB (default)\0Gamma 2.2\0"))
                config->AmdEncoding = encoding + 1;
            HelpMarker("sRGB and Gamma 2.2 decode the frame to linear light before the model and"
                       "\nencode its answer back afterwards. Linear hands it over unchanged."
                       "\n\nsRGB is the default: the steadiest in testing, and the one that held"
                       "\nhighlights best. Some games may look better with another.");

            // One slider, bound to whichever placement is live. The two keep separate values, so
            // switching back and forth does not make you retune each time.
            auto& modelScale = placement == 0 ? config->AmdNrScale : config->DlssNrRRWorkingScale;
            static float scale = 100.f;
            static bool editingScale = false;
            if (!editingScale)
                scale = modelScale.value_or_default() * 100.f;
            // With dynamic resolution on, this is the ceiling it steps down from.
            const bool dynamicScale = config->AmdDynamicScale.value_or_default();
            ImGui::SliderFloat(
                placement == 0 ? (dynamicScale ? "NR resolution, maximum (% of render)" : "NR resolution (% of render)")
                               : (dynamicScale ? "NR resolution, maximum (% of frame)" : "NR resolution (% of frame)"),
                &scale, 25, 100, "%.0f%%");
            editingScale = ImGui::IsItemActive();
            // Commit once after dragging or text entry, not one model rebuild per mouse move.
            if (ImGui::IsItemDeactivatedAfterEdit())
                modelScale = scale / 100.f;

            bool dynamic = dynamicScale;
            if (ImGui::Checkbox("Dynamic NR resolution", &dynamic))
                config->AmdDynamicScale = dynamic;
            HelpMarker("Lowers the model's resolution while the game runs under the target frame"
                       "\nrate, in steps of 85%, 70%, 55% and 40% of the resolution above."
                       "\n\nIt steps down after 2 s under the target and back up after 5 s with 15%"
                       "\nheadroom, and waits 10 s after every change (60 s after a step down"
                       "\nbefore it tries to go back up). Each change rebuilds the model, so the"
                       "\neffect is off for one to five seconds and its history restarts."
                       "\n\nThe AMD runtime keeps a little more VRAM after every rebuild, so it makes"
                       "\nat most 8 changes per session and then stays where it is. Turning this"
                       "\noff and on again resets the count."
                       "\n\nWith a frame cap, set the target a little below the cap, or it never"
                       "\nsees the headroom to step back up.");
            if (dynamic)
            {
                int target = config->AmdDynamicTargetFps.value_or_default();
                if (ImGui::SliderInt("Target FPS (rendered)", &target, 30, 240))
                    config->AmdDynamicTargetFps = target;
                if (const auto status = DlssNr::AmdBridge::DynamicStatus(); !status.empty())
                    ImGui::TextDisabled("%s", status.c_str());
            }
            // Stage costly neural parameter edits in ImGui state. Keep rendering
            // with the committed parameters until release/text-edit completion.
            auto neuralSlider = [](const char* label, auto& option, float lo, float hi)
            {
                auto storage = ImGui::GetStateStorage();
                const ImGuiID id = ImGui::GetID(label);
                const ImGuiID activeId = id ^ 0x6e72534cu;
                float value = storage->GetBool(activeId, false) ? storage->GetFloat(id) : option.value_or_default();
                ImGui::SliderFloat(label, &value, lo, hi);
                const bool active = ImGui::IsItemActive();
                const bool commit = ImGui::IsItemDeactivatedAfterEdit();
                storage->SetFloat(id, value);
                storage->SetBool(activeId, active);
                if (commit)
                    option = value;
            };
            static int passes = 1;
            static bool editingPasses = false;
            if (!editingPasses)
                passes = int(config->DlssNrPasses.value_or_default());
            ImGui::SliderInt("AMD neural passes", &passes, 1, 3);
            editingPasses = ImGui::IsItemActive();
            if (ImGui::IsItemDeactivatedAfterEdit())
                config->DlssNrPasses = uint32_t(passes);
            neuralSlider("Lightning Strength", config->AmdNeuralLightingStrength, 0, 1);
            neuralSlider("AMD structure", config->DlssNrLocalStructure, 0, 2);
            neuralSlider("AMD character structure", config->DlssNrSkinStructure, 0, 2);
            if (ImGui::TreeNode("Experimental"))
            {
                ImGui::PushID("RTGI");
                bool enabled = config->AmdRtgiEnabled.value_or_default();
                if (ImGui::Checkbox("Enable effect", &enabled))
                    config->AmdRtgiEnabled = enabled;
                auto slider = [](const char* label, auto& option, float lo, float hi)
                {
                    float value = option.value_or_default();
                    if (ImGui::SliderFloat(label, &value, lo, hi))
                        option = value;
                };
                int quality = config->AmdRtgiQuality.value_or_default();
                if (ImGui::Combo("Quality", &quality, "Very low\0Low\0Medium\0High\0Ultra\0"))
                    config->AmdRtgiQuality = uint32_t(quality);
                int denoiser = config->AmdRtgiDenoiser.value_or_default();
                if (ImGui::Combo("Denoiser", &denoiser, "Low\0Medium\0High\0"))
                    config->AmdRtgiDenoiser = uint32_t(denoiser);
                slider("Effect mix", config->AmdRtgiMix, 0, 1);
                slider("Contact shading", config->AmdRtgiContact, 0, 2);
                slider("Bounce saturation", config->AmdRtgiSaturation, 0, 2);
                slider("Sample radius", config->AmdRtgiRadius, .25f, 3);
                slider("Bounce lighting", config->AmdRtgiLighting, 0, 10);
                slider("Ambient occlusion", config->AmdRtgiOcclusion, 0, 10);
                slider("Ambient level", config->AmdRtgiAmbient, .25f, 1);
                slider("Object thickness", config->AmdRtgiThickness, 0, 1);
                slider("Smoothness", config->AmdRtgiSmoothness, 0, 1);
                slider("Fade range", config->AmdRtgiFade, .001f, 1);
                slider("Camera FOV", config->AmdRtgiFov, 20, 140);
                slider("Depth range", config->AmdRtgiFarPlane, 10, 10000);
                int inspect = config->AmdRtgiInspect.value_or_default();
                if (ImGui::Combo("Inspect", &inspect, "Final image\0Lighting\0"))
                    config->AmdRtgiInspect = uint32_t(inspect);
                if (ImGui::Button("Reset to defaults"))
                {
                    config->AmdRtgiEnabled = false;
                    config->AmdRtgiQuality = 2u;
                    config->AmdRtgiDenoiser = 1u;
                    config->AmdRtgiInspect = 0u;
                    config->AmdRtgiContact = 0.0f;
                    config->AmdRtgiSaturation = 1.0f;
                    config->AmdRtgiRadius = 1.0f;
                    config->AmdRtgiMix = 1.0f;
                    config->AmdRtgiLighting = 5.0f;
                    config->AmdRtgiOcclusion = 1.0f;
                    config->AmdRtgiAmbient = 1.0f;
                    config->AmdRtgiThickness = .1f;
                    config->AmdRtgiSmoothness = .5f;
                    config->AmdRtgiFade = .3f;
                    config->AmdRtgiFov = 60.0f;
                    config->AmdRtgiFarPlane = 600.0f;
                }
                ImGui::PopID();
                ImGui::TreePop();
            }
            if (ImGui::TreeNode("Appearance and tonemap"))
            {
                bool lookEnabled = config->AmdLookEnabled.value_or_default();
                if (ImGui::Checkbox("Enable appearance filter", &lookEnabled))
                    config->AmdLookEnabled = lookEnabled;
                ImGui::BeginDisabled(!lookEnabled);
                auto slider = [](const char* label, auto& option, float lo, float hi)
                {
                    float value = option.value_or_default();
                    if (ImGui::SliderFloat(label, &value, lo, hi))
                        option = value;
                };
                slider("Effect mix", config->AmdLookMix, 0, 1);
                slider("Material detail", config->AmdLookMaterialDetail, 0, 2);
                slider("Shape definition", config->AmdLookShapeDefinition, 0, 2);
                slider("Local lighting", config->AmdLookLocalLighting, 0, 2);
                slider("Skin microstructure", config->AmdLookSkinDetail, 0, 2);
                slider("Skin highlight softness", config->AmdLookSkinSoftness, 0, 1);
                slider("Plastic/specular reduction", config->AmdLookSpecularControl, 0, 1);
                slider("Highlight roll-off", config->AmdLookHighlightRollOff, 0, 1);
                slider("Material colour separation", config->AmdLookColourSeparation, 0, 1);
                slider("Contact-shadow impression", config->AmdLookShadowDepth, 0, 1);
                slider("Edge/halo protection", config->AmdLookAntiHalo, 0, 1);
                slider("Flat/noisy area protection", config->AmdLookFlatAreaProtection, 0, 1);
                bool detectSkin = config->AmdLookDetectSkin.value_or_default();
                if (ImGui::Checkbox("Automatic skin mask", &detectSkin))
                    config->AmdLookDetectSkin = detectSkin;
                ImGui::Separator();
                ImGui::TextUnformatted("Tonemap");
                slider("Tone strength", config->AmdLookTone, 0, 1);
                slider("Exposure (EV)", config->AmdLookExposureEV, -3, 3);
                slider("Contrast", config->AmdLookContrast, 0.5, 1.5);
                slider("Saturation", config->AmdLookSaturation, 0, 2);
                slider("Highlight compression", config->AmdLookHighlightCompression, 0, 1);
                int inspect = static_cast<int>(config->AmdLookInspect.value_or_default());
                if (ImGui::Combo("Inspect", &inspect, "Final image\0Skin mask\0Material residual\0Local lighting\0"))
                    config->AmdLookInspect = static_cast<uint32_t>(inspect);
                ImGui::EndDisabled();
                if (ImGui::Button("Reset to defaults"))
                {
                    config->AmdLookEnabled = false;
                    config->AmdLookAppearance = 2u;
                    config->AmdLookMix = 1.0f;
                    config->AmdLookMaterialDetail = 1.15f;
                    config->AmdLookShapeDefinition = 1.2f;
                    config->AmdLookLocalLighting = 1.15f;
                    config->AmdLookSkinDetail = 1.1f;
                    config->AmdLookSkinSoftness = .486f;
                    config->AmdLookDetectSkin = true;
                    config->AmdLookSpecularControl = .58f;
                    config->AmdLookHighlightRollOff = .9f;
                    config->AmdLookColourSeparation = 0.0f;
                    config->AmdLookShadowDepth = .2f;
                    config->AmdLookAntiHalo = .901f;
                    config->AmdLookFlatAreaProtection = 0.0f;
                    config->AmdLookInspect = 0u;
                    config->AmdLookTone = 0.0f;
                    config->AmdLookExposureEV = 1.0f;
                    config->AmdLookContrast = 1.0f;
                    config->AmdLookSaturation = 1.0f;
                    config->AmdLookHighlightCompression = 0.0f;
                    config->DlssNrPasses = 1u;
                    config->DlssNrLocalStructure = 1.0f;
                    config->DlssNrSkinStructure = 1.0f;
                    config->DlssNrApplyAfterRR = false; // before Super Resolution
                    config->AmdNrScale = 1.0f;
                    config->AmdDynamicScale = false;
                    config->AmdDynamicTargetFps = 60;
                    config->AmdNeuralLighting = true;
                    config->AmdEncoding = 2;
                    config->AmdNeuralLightingStrength = .5f;
                    DlssNr::AmdBridge::InvalidateHistory();
                }
                ImGui::TreePop();
            }
            ImGui::TextWrapped("%s", DlssNr::AmdBridge::Status().c_str());
            ImGui::TextWrapped("AMD HIP backend. Each pass owns independent temporal history. More passes increase GPU "
                               "time and memory. Restart the game after a backend failure.");
            return;
        }

        HelpMarker("Synthesises detail in the upscaler's frame, before frame generation sees it."
                   "\n\nNeeds two similarly named files beside OptiScaler, one character apart:"
                   "\n  nvngx_dlssnr.dll       NVIDIA's model (~165 MB) -- you supply it"
                   "\n  nvngx.dll_dlssnr.dll   the forwarder (~13 KB) -- ships in this package"
                   "\nUndocumented and driven directly, so none of this is officially supported.");

        bool beforeSr = config->DlssNrRunBeforeSr.value_or_default();
        if (ImGui::Checkbox("Apply before Super Resolution", &beforeSr))
            config->DlssNrRunBeforeSr = beforeSr;

        HelpMarker("Runs Neural Rendering on the render-resolution colour input immediately before"
                   "\nSuper Resolution, so SR temporally accumulates and upscales the enhanced frame."
                   "\n\nRay Reconstruction is deliberately excluded: its input contract differs and"
                   "\nuses the separate Apply after Ray Reconstruction option. Origin-zero padded"
                   "\ninputs use their active render size; offset or invalid inputs fall back post-SR."
                   "\n\nThis placement control currently applies to the Direct3D 12 path and its"
                   "\nDirect3D 11/Vulkan bridges; native Vulkan keeps the post-upscale path.");

        // The AMD backend reaches this placement too: FSR-RR denoises and enlarges in one
        // dispatch, so running the model over its finished frame is the only way to follow it.
        const bool amdBackend = DlssNr::AmdBridge::HasFiles();

        bool afterRR = config->DlssNrApplyAfterRR.value_or_default();
        if (ImGui::Checkbox("Apply after Ray Reconstruction (DX12)", &afterRR))
            config->DlssNrApplyAfterRR = afterRR;
        HelpMarker(amdBackend ? "Runs the AMD neural model over the finished frame instead of over the"
                                "\nupscaler's input. This is the placement for FSR-RR, whose denoise and"
                                "\nenlargement are one dispatch and cannot be split."
                                "\n\nWith it off the model stays pre-SR and declines any frame that reaches"
                                "\nit after Ray Reconstruction."
                                "\n\nThe model history is rebuilt when the placement changes, so expect a few"
                                "\nframes of warm-up after toggling it."
                              : "Requires the game's native Ray Reconstruction option. RR denoises and upscales"
                                "\nfirst; NR then processes its output before frame generation."
                                "\nThis does not add RR to games without the required rendering buffers."
                                "\nIndependent controls below prevent inheriting the cost of the SR configuration."
                                "\nNative Vulkan does not use these DX12 controls.");
        // The AMD backend has a single pass chain whatever the placement, driven by Passes above.
        // A second count here would be a control that changes nothing.
        if (!amdBackend)
        {
            int rrPasses = (int) config->DlssNrRRPasses.value_or_default();
            if (ImGui::SliderInt("NR passes after RR", &rrPasses, 1, (int) MaxPassCount))
                config->DlssNrRRPasses = (unsigned int) rrPasses;
        }
        // Above 1.0 the model supersamples, which the AMD runtime does not do -- the slider
        // would offer a number the backend then clamps away.
        const float rrScaleMax = amdBackend ? 1.0f : 2.0f;
        float rrScale = std::min(config->DlssNrRRWorkingScale.value_or_default(), rrScaleMax);
        if (ImGui::SliderFloat("NR model scale after RR", &rrScale, 0.25f, rrScaleMax, "%.2fx"))
            config->DlssNrRRWorkingScale = rrScale;
        HelpMarker(amdBackend ? "Relative to the finished frame: 1.00x runs the model over every pixel of"
                                "\nit, 0.50x at 4K runs it at 1920x1080 and enlarges the answer back."
                                "\nCost falls with the square of this, so it is the first control to reach"
                                "\nfor when the full-size pass is too expensive."
                                "\n\nThe AMD runtime does not supersample, so this stops at 1.00x."
                              : "Relative to RR's OUTPUT resolution: 0.50x at 4K runs NR at 1920x1080."
                                "\nRR itself remains full quality. The NR edit is resized for final composition."
                                "\nPasses 2 and 3 use the same per-pass model profiles as the SR path.");

        // The toggle can be bound to a key, and nobody would think to look for it under Keybinds
        // unless told. Dimmed, because it is a note rather than a setting.
        ImGui::TextDisabled("Can be toggled with a key -- bind it under Keybinds, \"Neural Rendering\".");

        bool applyModel = config->DlssNrApplyModel.value_or_default();
        if (ImGui::Checkbox("Apply the model", &applyModel))
            config->DlssNrApplyModel = applyModel;

        HelpMarker("Whether the model's edit is applied. Off shows the clean upscaler frame while the"
                   "\npass keeps running -- so with Hold frame (under Compare) you can freeze a"
                   "\nframe and toggle this to see the same frozen frame with and without Neural"
                   "\nRendering. Leave it on for normal use.");

        // Either backend. The two keep separate state, and on a native Vulkan game the D3D12 side
        // is never touched -- so asking only that one reports "waiting for the upscaler" over a pass
        // that is demonstrably running.
        const bool vulkan = DlssNr::IsRunningVk();

        // Turning the pass off does not release the model, so the feature handle stays alive and
        // IsRunning keeps answering yes. Reporting a cost from that was wrong in the way that matters
        // most: the toggle is how anyone A/Bs this, so the one moment the number is read is the one
        // moment it describes the frame before last.
        if (!enabled)
        {
            ImGui::TextDisabled("Off. The model stays loaded, so turning this back on is immediate.");
        }
        else if (!DlssNr::IsRunning() && !vulkan)
        {
            const char* reason = DlssNr::FailureReason();

            if (reason[0] != 0)
            {
                ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.35f, 1.0f), "Off for this session: %s.", reason);
                ImGui::SameLine();

                if (ImGui::SmallButton("Retry"))
                    DlssNr::RetryAfterFailure();
            }
            // The model is D3D12 and Vulkan only. A native D3D11 upscaler creates no D3D12 device,
            // so nothing ever arrives and the wait below would never end.
            else if (auto feature = State::Instance().currentFeature;
                     feature != nullptr && feature->Api() == API::DX11 && !feature->IsWithDx12())
            {
                ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.3f, 1.0f),
                                   "%s runs natively on D3D11, which the model has no path for.",
                                   feature->Name().c_str());
                ImGui::TextDisabled("Pick an upscaler marked w/Dx12 above, then restart the game.");
            }
            else if (enabled)
                ImGui::TextUnformatted("Waiting for the upscaler to run.");
        }
        else
        {
            // The cost belongs here rather than only in the upscaler's breakdown: that tooltip needs
            // OptiScaler's own upscaler to have run, and with native DLSS passing through there is
            // nothing in it to hang this off.
            // Either backend's timer. They measure the same thing by different means, and only one
            // of them is running.
            const auto ms = vulkan ? DlssNr::LastGpuTimeVk() : DlssNr::LastGpuTime();

            // With "Apply the model" off the pass STILL RUNS (so Hold-frame A/B can toggle its edit on
            // a frozen frame) -- it only outputs the clean frame. So the cost is real, and saying so
            // stops the reading looking like a bug. Enable NR off is what zeroes it.
            const char* runSuffix =
                !config->DlssNrApplyModel.value_or_default() ? "  (model running, edit hidden)" : "";

            if (ms.has_value())
                ImGui::TextColored(ImVec4(0.4f, 0.9f, 0.5f, 1.0f), "Running%s - %.2f ms per frame%s",
                                   vulkan ? " natively on Vulkan" : "", ms.value(), runSuffix);
            else if (vulkan)
                // Measured but not yet read: the first few frames are still in the query ring.
                ImGui::TextColored(ImVec4(0.4f, 0.9f, 0.5f, 1.0f), "Running natively on Vulkan - %llu frames%s",
                                   DlssNr::FramesVk(), runSuffix);
            else
                ImGui::TextColored(ImVec4(0.4f, 0.9f, 0.5f, 1.0f), "Running.%s", runSuffix);

            ImGui::SameLine();
            ImGui::TextDisabled("(?)");
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                ImGui::SetTooltip("The whole pass: the staging copies and the resolve as well as the"
                                  "\nmodel. Timing only the model would flatter the number."
                                  "\n\nCompare it against the frame time at the bottom of this window to"
                                  "\nsee what it is costing you.");
        }

        ImGui::Spacing();
        ImGui::PushItemWidth(220.0f * menuResScale);

        ImGui::SeparatorText("Cost");

        {
            // Coloured by what it costs, because the number alone does not say. The model is 98% of
            // this pass's expense and every run pays it again, so the scale is linear and brutal:
            // four passes is four times the model, not four percent more.
            //
            // Green at 1, what the model was trained for. Amber at 2 and 3, where it is being asked
            // to enhance its own output. Red from 4, where it usually stops looking rendered.
            //
            // Applied when the handle is let go: every distinct value is a feature to build, and the
            // build is spaced so the driver's latches survive it.
            static int pendingPasses = -1;

            int passes = pendingPasses >= 0 ? pendingPasses : (int) config->DlssNrPasses.value_or_default();

            if (passes < 1)
                passes = 1;

            const ImVec4 colour = passes <= 1                                 ? ImVec4(0.35f, 0.88f, 0.38f, 1.0f)
                                  : passes <= 3                               ? ImVec4(0.95f, 0.70f, 0.20f, 1.0f)
                                  : passes <= (int) DlssNr::kDefaultMaxPasses ? ImVec4(0.92f, 0.30f, 0.25f, 1.0f)
                                                                              : ImVec4(1.00f, 0.25f, 0.85f, 1.0f);

            ImGui::PushStyleColor(ImGuiCol_Text, colour);
            ImGui::PushStyleColor(ImGuiCol_SliderGrab, colour);

            // The AMD backend loads one runtime module per pass (dlssnr_amd_pass1-3.dll beside the
            // proxy) and holds exactly three, so three is a hard ceiling there rather than a
            // suggestion -- and the lift below, which reaches thirty, would only be silently clamped.
            const bool amdPasses = DlssNr::AmdBridge::HasFiles();

            const bool unlocked = !amdPasses && config->DlssNrUnlockPasses.value_or_default();
            const int passLimit = amdPasses ? 3 : (int) (unlocked ? DlssNr::kMaxPasses : DlssNr::kDefaultMaxPasses);

            if (ImGui::SliderInt("Passes", &passes, 1, passLimit, passes == 1 ? "%d (native)" : "%dx model cost"))
                pendingPasses = passes;

            ImGui::PopStyleColor(2);

            if (ImGui::IsItemDeactivatedAfterEdit() && pendingPasses >= 0)
            {
                config->DlssNrPasses = (uint32_t) std::clamp(pendingPasses, 1, passLimit);
                pendingPasses = -1;
            }

            if (amdPasses)
                ImGui::TextDisabled("Three is the ceiling: one runtime module per pass.");

            // The lift, its tooltip and its running-cost line all describe the NVIDIA feature
            // chain, which reaches thirty. None of it applies while the AMD backend is the one
            // running the model, so the whole group goes rather than sitting there lying.
            if (!amdPasses)
            {
                if (bool lift = unlocked; ImGui::Checkbox("Lift the pass limit", &lift))
                {
                    config->DlssNrUnlockPasses = lift;

                    // Dropping the ceiling under a larger count would leave the file asking for passes
                    // the slider can no longer show.
                    if (!lift && config->DlssNrPasses.value_or_default() > DlssNr::kDefaultMaxPasses)
                        config->DlssNrPasses = DlssNr::kDefaultMaxPasses;
                }

                const std::string liftTip =
                    "Raises the slider above to " + std::to_string(DlssNr::kMaxPasses) +
                    ", which is far past what this pass"
                    "\nwas built for. Expect the frame time to scale with it and the game to stop being"
                    "\nplayable well before the top."
                    "\n\nCost is exactly linear and the model is nearly all of it, so ten passes is ten"
                    "\nmodel runs in one frame. Each also holds an NGX feature with its own history,"
                    "\nsized by the driver, and they are built one at a time with a settle between --"
                    "\nreaching a large count takes a while and the frames spent building show nothing."
                    "\n\nThe ratio guard under Colour has to rise with the count or the extra passes"
                    "\nspend their contribution against the clamp.";

                HelpMarker(liftTip.c_str());

                // The tooltip is not enough for a slider that now reaches thirty. Say the cost on screen,
                // and keep saying it while the count is past what the slider offers by default.
                if (unlocked)
                {
                    const int live = (int) config->DlssNrPasses.value_or_default();

                    if (live > (int) DlssNr::kDefaultMaxPasses)
                        ImGui::TextColored(ImVec4(1.00f, 0.25f, 0.85f, 1.0f),
                                           "%d passes: %dx the model's cost, every frame.", live, live);
                    else
                        ImGui::TextColored(ImVec4(0.95f, 0.70f, 0.20f, 1.0f),
                                           "Unlocked. Each pass past this point is another whole model run.");
                }
            }

            // Per-pass settings, one node each, only for the passes that are running.
            //
            // Written back as the sparse "2:intensity=0.5;3:style=1" the pass reads. A pass whose
            // controls all sit at the global value contributes nothing, so the string stays empty
            // until something is actually different and the default costs nothing to carry.
            const auto liveCount = (int) config->DlssNrPasses.value_or_default();

            // The AMD backend reads the global structure/skin values, never the per-pass table.
            if (!amdPasses && liveCount > 1)
            {
                if (ImGui::TreeNode("Per pass"))
                {
                    auto overrides = DlssNr::ParsePassOverridesForMenu(config->DlssNrPassOverrides.value_or_default());

                    bool edited = false;

                    for (int pass = 0; pass < liveCount; ++pass)
                    {
                        const std::string label = "Pass " + std::to_string(pass + 1);

                        if (!ImGui::TreeNode(label.c_str()))
                            continue;

                        auto& own = overrides[pass];

                        edited |= PassOverrideSlider("Intensity", &own.Intensity,
                                                     config->DlssNrIntensity.value_or_default(), 0.0f, 4.0f, pass);
                        edited |= PassOverrideSlider("Detail strength", &own.LocalStructure,
                                                     config->DlssNrLocalStructure.value_or_default(), 0.0f, 4.0f, pass);
                        edited |= PassOverrideSlider("Local tone", &own.LocalTone,
                                                     config->DlssNrLocalTone.value_or_default(), 0.0f, 4.0f, pass);
                        edited |= PassOverrideSlider("Skin structure", &own.SkinStructure,
                                                     config->DlssNrSkinStructure.value_or_default(), -1.0f, 4.0f, pass);

                        ImGui::TreePop();
                    }

                    if (edited)
                        config->DlssNrPassOverrides = DlssNr::SerializePassOverrides(overrides);

                    HelpMarker("What each pass is told, where it should differ from the values above."
                               "\n\nA control left on \"global\" follows the setting above it, so a pass you"
                               "\nhave not touched behaves exactly as it did before this existed."
                               "\n\nThe passes compound: a later pass sees what the one before it produced."
                               "\nEasing intensity down the chain keeps the last passes refining rather than"
                               "\nre-amplifying what is already there.");

                    ImGui::TreePop();
                }
            }

            HelpMarker("How many times the model runs over the frame, each pass shown the last one's"
                       "\nanswer."
                       "\n\nThe most expensive control here. Cost is exactly linear: five passes is"
                       "\nfive model runs, and the model is nearly all of what this pass costs."
                       "\n\nWhat it buys that nothing else can is the model re-deciding where detail"
                       "\ngoes, what hue it is, and how saturated. Detail strength amplifies the map"
                       "\nthe first pass drew; it cannot redraw it."
                       "\n\nWhat it does not buy is raw magnitude. Detail strength and Intensity are"
                       "\nfree and do that. Try both, and raise Model resolution, before this."
                       "\n\nPast 3, raise the ratio guard under Colour with it. The passes compound"
                       "\nthe luminance ratio and the guard clamps it, so beyond its limit the extra"
                       "\nruns are paid for and thrown away."
                       "\n\nEach pass is its own model, with its own memory, built on a frame of its"
                       "\nown -- so raising this takes a couple of seconds to arrive, and VRAM grows"
                       "\nwith it."
                       "\n\nNo effect on native Vulkan, or with the proxy path switched on.");
        }

        // Any percentage, rather than a handful of steps somebody chose in advance. The lower bound
        // is 25%: below that the model is working on so little of the picture that its answer no
        // longer survives being enlarged onto it.
        // Applied when the handle is let go, not while it is moving.
        //
        // Every distinct value here is a different working size, and a different working size tears
        // down the scratch textures and rebuilds the model. Writing it on each pixel of a drag meant
        // dozens of rebuilds in a second, which is felt as the whole frame hitching. The slider still
        // reads live; only the commit waits.
        static int pendingScale = -1;

        int scalePercent =
            pendingScale >= 0 ? pendingScale : (int) lroundf(config->DlssNrWorkingScale.value_or_default() * 100.0f);

        if (ImGui::SliderInt("Model resolution", &scalePercent, 25, 200, "%d%%"))
            pendingScale = scalePercent;

        if (ImGui::IsItemDeactivatedAfterEdit() && pendingScale >= 0)
        {
            config->DlssNrWorkingScale = std::clamp(pendingScale, 25, 200) / 100.0f;
            pendingScale = -1;
        }

        if (scalePercent > 100)
            ImGui::TextDisabled("Supersampling %.2fx: the model runs ABOVE native, then\n"
                                "is sampled back down. Experimental, and costly -- time grows with the area.",
                                scalePercent / 100.0f);

        if (scalePercent > 100)
        {
            static const char* dsNames[] = { "FSR1",     "Bicubic", "Catmull-Rom", "Lanczos2",
                                             "Lanczos3", "Kaiser2", "Kaiser3",     "MAGIC" };
            int ds = (int) config->DlssNrScalingDownscaler.value_or_default();
            if (ds < 0 || ds >= IM_ARRAYSIZE(dsNames))
                ds = (int) Scaler::Lanczos3;

            if (ImGui::Combo("Downscaler (NR)", &ds, dsNames, IM_ARRAYSIZE(dsNames)))
                config->DlssNrScalingDownscaler = (Scaler) ds;

            HelpMarker("The filter that averages the model's above-native answer back to display size --"
                       "\nthis is what turns supersampling into LESS noise rather than more. Sharper"
                       "\nfilters (Lanczos3, Kaiser3) keep the most detail; softer ones (Bicubic,"
                       "\nCatmull-Rom) are gentler on ringing. Independent of the Output Scaling"
                       "\ndownscaler, so the two can differ and run at the same time.");
        }

        HelpMarker("What fraction of the frame the model works at. Cost falls with the square of"
                   "\nthis, so half resolution is roughly a quarter of the time."
                   "\n\nThe frame is never reduced. Only the model's contribution is computed small"
                   "\nand enlarged, so the picture underneath is untouched whatever this says."
                   "\n\nWhat it trades: the shading the model adds is broad and survives enlargement;"
                   "\nthe fine structure it synthesises does not, and softens. Worth having when the"
                   "\npass costs more than you want to pay for the detail it returns."
                   "\n\nThe frame itself stays at full detail whatever this says -- only the"
                   "\nmodel's own work is done small.");

        {
            bool dual = config->DlssNrDualFeature.value_or_default();

            if (ImGui::Checkbox("Run inside the upscaler", &dual))
                config->DlssNrDualFeature = dual;

            ImGui::SameLine();
            ImGui::TextColored(ImVec4(0.95f, 0.70f, 0.20f, 1.0f), "(experimental, restart)");

            HelpMarker("Splits the upscaler in two and puts the model between the halves. The upscaler"
                       "\nwrites at render resolution, the model runs on that, and the enlargement to"
                       "\ndisplay resolution happens afterwards."
                       "\n\nWith ray reconstruction this makes the first half a denoiser and nothing else,"
                       "\nwhich is the arrangement worth having: the frame the model sees is clean, and it"
                       "\nis a quarter of the pixels at Performance."
                       "\n\nUnlike 'Run before the upscaler', the frame here has already been through"
                       "\ntemporal accumulation, so the subpixel jitter the model cannot be told about is"
                       "\nresolved before it sees anything."
                       "\n\nThe enlargement is the output scaler, not the upscaler's own. Takes effect when"
                       "\nthe upscaler is next built, so restart or change quality after ticking it."
                       "\n\nDoes nothing when render resolution already equals display resolution -- at"
                       "\nDLAA there is no smaller frame to run on.");

            if (dual)
            {
                // The same names the upscaler list uses, resolved through the same provider, so a
                // machine without DLSS is handed FSR here exactly as it is anywhere else.
                static const char* enlargerNames[] = { "Spatial (no motion vectors)", "DLSS", "FSR 2.2", "FSR 3.1",
                                                       "XeSS" };
                static const std::optional<Upscaler> enlargerValues[] = { std::nullopt, Upscaler::DLSS, Upscaler::FSR22,
                                                                          Upscaler::FFX, Upscaler::XeSS };

                const auto current = config->DlssNrDualEnlarger.value_for_config();

                int index = 0;
                for (int i = 1; i < IM_ARRAYSIZE(enlargerNames); ++i)
                {
                    if (current == enlargerValues[i])
                    {
                        index = i;
                        break;
                    }
                }

                if (ImGui::Combo("Enlarged by", &index, enlargerNames, IM_ARRAYSIZE(enlargerNames)))
                {
                    if (enlargerValues[index].has_value())
                        config->DlssNrDualEnlarger = enlargerValues[index].value();
                    else
                        config->DlssNrDualEnlarger.reset();
                }

                HelpMarker("What enlarges the frame once the model has edited it."
                           "\n\nSpatial needs no motion vectors, no depth and no jitter, so it cannot be"
                           "\nwrong about any of them -- and it is the softest, having nothing temporal to"
                           "\nwork from."
                           "\n\nThe upscalers are sharper and use the game's own per-frame data. An"
                           "\nupscaler this machine cannot run is replaced with one it can, the same way"
                           "\nthe main upscaler list behaves."
                           "\n\nTakes effect when the upscaler is next built.");
            }

            bool preUpscale = config->DlssNrPreUpscale.value_or_default();

            if (dual)
                ImGui::BeginDisabled();

            if (ImGui::Checkbox("Run before the upscaler", &preUpscale))
                config->DlssNrPreUpscale = preUpscale;

            if (dual)
                ImGui::EndDisabled();

            ImGui::SameLine();
            ImGui::TextColored(ImVec4(0.95f, 0.70f, 0.20f, 1.0f), "(experimental)");

            HelpMarker("Shows the model the frame the upscaler is about to read, instead of the one it"
                       "\nwrote. The model runs at render resolution, so at Performance it costs about a"
                       "\nquarter of what it costs after the upscaler, and it sees rendered pixels rather"
                       "\nthan reconstructed ones."
                       "\n\nUnlike Model resolution this does not soften what the model returns: the model"
                       "\nruns 1:1 on a smaller frame rather than small on a large one, and the upscaler"
                       "\nenlarges its work along with everything else."
                       "\n\nUntested territory. Colour at this point is jittered by a different subpixel"
                       "\noffset every frame and the model is given no way to know that, so its history"
                       "\nmay reproject against an offset it cannot see. Look for shimmer and swimming on"
                       "\nfine detail while the camera moves.");
        }

        // Meaningful only when the model runs BELOW the frame's size. At 100% -- and above, where
        // supersampling composites its down-legged answer at native -- the residual collapses to the
        // model's own picture and the two modes are identical, so the control says so by going grey.
        {
            const bool reduced = config->DlssNrWorkingScale.value_or_default() < 0.999f;

            if (!reduced)
                ImGui::BeginDisabled();

            static const char* enlargeNames[] = { "Classic", "Matched residual" };
            int enlarge = config->DlssNrTransfer.value_or_default() == 1 ? 1 : 0;

            if (ImGui::Combo("Enlargement", &enlarge, enlargeNames, IM_ARRAYSIZE(enlargeNames)))
                config->DlssNrTransfer = (uint32_t) enlarge;

            if (!reduced)
                ImGui::EndDisabled();

            HelpMarker("How the model's work is brought back up when it ran below the frame's size."
                       "\n\nClassic composes the model's small picture directly against the full-size"
                       "\nframe. Those two disagree by the shrink's blur as well as by the model's edit,"
                       "\nand the composition cannot tell them apart -- it reads the blur as brightness"
                       "\nthe frame has and the model never saw. The lower the model resolution the"
                       "\nlarger that error, and it is the colour shift that shows up at 50%."
                       "\n\nMatched residual carries up only the model's difference and lays it on the"
                       "\nframe's own proxy, so both pictures being compared are full size and the only"
                       "\nthing that came from the small raster is the edit itself."
                       "\n\nNo effect at 100% or above: there is no residual to carry and the two are"
                       "\nidentical (supersampling brings its answer down to frame size before this)."
                       "\n\nFrom hhkbble's multi-pass work on this fork.");
        }

        ImGui::SeparatorText("How much of it lands");

        float transfer = config->DlssNrTransferStrength.value_or_default();
        if (ImGui::SliderFloat("Detail strength", &transfer, 0.0f, 2.0f, "%.2f"))
            config->DlssNrTransferStrength = transfer;

        ImGui::SameLine();
        if (ImGui::SmallButton("Reset##detail"))
            config->DlssNrTransferStrength = 1.0f;

        HelpMarker("How far the frame moves toward the model's picture."
                   "\n\nThe model's answer is not added to the frame -- it is a complete picture of its"
                   "\nown, rescaled so its luminance sits where the original says it should. This"
                   "\nblends between the two, so both ends are real pictures and everything between"
                   "\nthem is one too."
                   "\n\n0 gives back exactly what the upscaler produced. 1 is the model's picture."
                   "\n\nAbove 1 carries on past it in the same direction, which is not something the"
                   "\nmodel asked for -- use it to see what it is doing, then come back down. This"
                   "\nis the control to push if you want more effect: Intensity belongs to the model"
                   "\nand it decides what to do with it.");

        float colour = config->DlssNrColourStrength.value_or_default();
        if (ImGui::SliderFloat("Colour strength", &colour, 0.0f, 4.0f, "%.2f"))
            config->DlssNrColourStrength = colour;

        ImGui::SameLine();
        if (ImGui::SmallButton("Reset##colour"))
            config->DlssNrColourStrength = 1.0f;

        HelpMarker("Whether the model's colour arrives with its light."
                   "\n\n0 keeps the game's own hue exactly -- every pixel is the original colour with"
                   "\nonly its brightness carrying the model's verdict. Game-accurate colour, with"
                   "\nthe detail. 1 brings the model's colour as well, in its own hue, clamped into"
                   "\nAP1 so nothing unreachable is asked for."
                   "\n\nThis cannot shift hue on its own: it interpolates between two finished"
                   "\npictures rather than adding a colour difference to one, which is what used to"
                   "\nlet a warm subject come back green."
                   "\n\nAbove 1 it OVER-SATURATES: the colour keeps its hue but grows more vivid,"
                   "\nand rolls off at the edge of what the display can show rather than clipping"
                   "\ninto a flat blown patch. 1 is the model's own colour; push past it for punch.");

        // Experimental. 0 off (soft knee), 1 Neutwo + our composition, 2 Neutwo + pure-inverse replace,
        // 3 hybrid+composed, 4 hybrid+replace (identity midtones + unclipped highlights). Always shown.
        static const char* reversibleNames[] = { "Off (soft knee)", "Neutwo proxy + composed", "Neutwo proxy + replace",
                                                 "Hybrid proxy + composed", "Hybrid proxy + replace" };
        int reversible = (int) config->DlssNrReversibleMode.value_or_default();
        if (reversible < 0 || reversible > 4)
            reversible = 0;
        if (ImGui::Combo("Reversible proxy (experimental)", &reversible, reversibleNames,
                         IM_ARRAYSIZE(reversibleNames)))
            config->DlssNrReversibleMode = (uint32_t) reversible;

        HelpMarker("What the model is shown, and how its answer comes back."
                   "\n\nOff (soft knee): the default. It rolls highlights off so hard the model"
                   "\ncannot resolve detail in them -- fine in soft-lit scenes, weak in bright ones."
                   "\n\nNeutwo composed: an unclipped curve so the model sees highlight detail, then"
                   "\neverything above (Detail/Colour strength, highlight guard, palette). It wins in"
                   "\nbright scenes, but the curve compresses MIDTONES too, so in soft-lit content it"
                   "\ncan be worse than Off. It also shifts paper white -- re-check it when you switch."
                   "\n\nHybrid composed: the best of both, and the one to use. Identity in the"
                   "\nmidtones -- as good as Off there -- and the unclipped roll only in the"
                   "\nhighlights, so it recovers the detail Off crushes without giving up the"
                   "\nmidtones Neutwo does. It barely shifts paper white."
                   "\n\nReplace: the raw model straight back through the exact inverse, none of the"
                   "\ncomposition -- no guard, no palette, no strengths. Gorgeous where there are no"
                   "\nbright lights, but they FLASH in motion. A reference, not a daily setting."
                   "\n\nHybrid replace: the raw model like Replace, but on the hybrid curve -- the"
                   "\ndecode is identity in the midtones, so the flashing is confined to genuine"
                   "\nbright highlights instead of everywhere. Most of Replace's detail, far more"
                   "\nstable. If you love the Replace look but the flicker bothers you, use this."
                   "\n\nOff is byte-identical to before.");

        ImGui::SeparatorText("Model passes");
        ImGui::TextWrapped("Each pass has its own style and model strengths. Changes apply when you release a slider.");
        static const char* styles[] = { "Standard", "Natural", "Cinematic" };
        static const char* inheritedStyles[] = { "Auto (inherit pass 1)", "Standard", "Natural", "Cinematic" };

        if (ImGui::TreeNodeEx("Pass 1", ImGuiTreeNodeFlags_DefaultOpen))
        {
            int style = (int) std::min(config->DlssNrStyle.value_or_default(), 2u);
            if (ImGui::Combo("Style", &style, styles, IM_ARRAYSIZE(styles)))
                config->DlssNrStyle = (uint32_t) style;
            DeferredSlider("Intensity", &config->DlssNrIntensity, 0.0f, 2.0f, 1.0f);
            DeferredSlider("Local structure", &config->DlssNrLocalStructure, 0.0f, 2.0f, 1.0f);
            DeferredSlider("Local tone", &config->DlssNrLocalTone, 0.0f, 2.0f, 1.0f);
            DeferredSlider("Skin structure", &config->DlssNrSkinStructure, -1.0f, 2.0f, -1.0f);
            HelpMarker(
                "-1 follows local structure; 0 reduces skin structure independently."
                "\nThis is not a skin-colour/tone off switch. Use the final skin/scene controls below for that.");
            bool mask = config->DlssNrAutoMask.value_or_default();
            if (ImGui::Checkbox("Auto skin mask", &mask))
                config->DlssNrAutoMask = mask;
            HelpMarker("NVIDIA's internal automatic mask, not the colour-based preview below."
                       "\nWith Skin structure=-1 it follows general structure, so the difference may be subtle."
                       "\nTry Skin structure=0 versus Local structure=1 to compare. Mask accuracy is model-dependent.");
            ImGui::TreePop();
        }

        if (ImGui::TreeNodeEx("Pass 2", ImGuiTreeNodeFlags_DefaultOpen))
        {
            ImGui::TextWrapped("Unset controls inherit pass 1, except local tone which defaults to 0. Reset restores "
                               "this behavior. Only active passes run.");
            InheritedProfileCombo("Style", &config->DlssNrPass2Style, inheritedStyles, IM_ARRAYSIZE(inheritedStyles));
            DeferredSlider("Intensity", &config->DlssNrPass2Intensity, 0.0f, 2.0f,
                           config->DlssNrIntensity.value_or_default(), "%.2f", true);
            DeferredSlider("Local structure", &config->DlssNrPass2LocalStructure, 0.0f, 2.0f,
                           config->DlssNrLocalStructure.value_or_default(), "%.2f", true);
            DeferredSlider("Local tone", &config->DlssNrPass2LocalTone, 0.0f, 2.0f, 0.0f, "%.2f", true);
            DeferredSlider("Skin structure", &config->DlssNrPass2SkinStructure, -1.0f, 2.0f,
                           config->DlssNrSkinStructure.value_or_default(), "%.2f", true);
            bool mask = config->DlssNrPass2AutoMask.has_value() ? config->DlssNrPass2AutoMask.value()
                                                                : config->DlssNrAutoMask.value_or_default();
            if (ImGui::Checkbox("Auto skin mask", &mask))
                config->DlssNrPass2AutoMask = mask;
            ImGui::SameLine();
            if (ImGui::SmallButton("Reset##mask"))
                config->DlssNrPass2AutoMask = std::optional<bool> {};
            ImGui::TreePop();
        }

        if (ImGui::TreeNodeEx("Pass 3", ImGuiTreeNodeFlags_DefaultOpen))
        {
            ImGui::TextWrapped("Unset controls inherit pass 1, except local tone which defaults to 0. Reset restores "
                               "this behavior. Only active passes run.");
            InheritedProfileCombo("Style", &config->DlssNrPass3Style, inheritedStyles, IM_ARRAYSIZE(inheritedStyles));
            DeferredSlider("Intensity", &config->DlssNrPass3Intensity, 0.0f, 2.0f,
                           config->DlssNrIntensity.value_or_default(), "%.2f", true);
            DeferredSlider("Local structure", &config->DlssNrPass3LocalStructure, 0.0f, 2.0f,
                           config->DlssNrLocalStructure.value_or_default(), "%.2f", true);
            DeferredSlider("Local tone", &config->DlssNrPass3LocalTone, 0.0f, 2.0f, 0.0f, "%.2f", true);
            DeferredSlider("Skin structure", &config->DlssNrPass3SkinStructure, -1.0f, 2.0f,
                           config->DlssNrSkinStructure.value_or_default(), "%.2f", true);
            bool mask = config->DlssNrPass3AutoMask.has_value() ? config->DlssNrPass3AutoMask.value()
                                                                : config->DlssNrAutoMask.value_or_default();
            if (ImGui::Checkbox("Auto skin mask", &mask))
                config->DlssNrPass3AutoMask = mask;
            ImGui::SameLine();
            if (ImGui::SmallButton("Reset##mask"))
                config->DlssNrPass3AutoMask = std::optional<bool> {};
            ImGui::TreePop();
        }

        if (ImGui::TreeNode("Advanced preset hints (effect unverified)"))
        {
            ImGui::TextWrapped("These hints are passed to NVIDIA at creation, but their visual effect is unverified. "
                               "Use Style for model profile selection. Existing INI hints are preserved.");
            static const char* presets[] = { "Default", "Preset 1", "Preset 2", "Preset 3" };
            static const char* inheritedPresets[] = { "Auto (inherit pass 1)", "Default", "Preset 1", "Preset 2",
                                                      "Preset 3" };
            int preset = (int) std::min(config->DlssNrPreset.value_or_default(), 3u);
            if (ImGui::Combo("Pass 1 preset hint", &preset, presets, IM_ARRAYSIZE(presets)))
                config->DlssNrPreset = (uint32_t) preset;
            InheritedProfileCombo("Pass 2 preset hint", &config->DlssNrPass2Preset, inheritedPresets,
                                  IM_ARRAYSIZE(inheritedPresets));
            InheritedProfileCombo("Pass 3 preset hint", &config->DlssNrPass3Preset, inheritedPresets,
                                  IM_ARRAYSIZE(inheritedPresets));
            ImGui::TreePop();
        }
        ImGui::TextWrapped("Per-pass overrides apply to the DX12 multipass path, including NR after RR. Native Vulkan "
                           "and the driver-proxy backend remain single-pass.");

        ImGui::SeparatorText("Colour");

        if (ImGui::TreeNode("Skin and environment (final edit)"))
        {
            ImGui::TextWrapped("Optional colour-based selection, NOT NVIDIA's automatic skin mask. Warm scenery can be "
                               "selected and coloured lighting can hide skin. Check the preview. These controls affect "
                               "the combined result of all passes.");
            bool filter = config->DlssNrSkinProtection.value_or_default();
            if (ImGui::Checkbox("Separate skin / environment controls", &filter))
                config->DlssNrSkinProtection = filter;
            ImGui::BeginDisabled(!filter);
            bool tone = config->DlssNrSkinToneEnabled.value_or_default();
            if (ImGui::Checkbox("Allow skin tone / colour changes", &tone))
                config->DlssNrSkinToneEnabled = tone;
            HelpMarker("Off preserves colour in selected pixels; lighting/detail can still change."
                       "\nAlso set Skin detail / lighting to 0 to suppress both.");
            const auto slider = [](const char* label, auto& option)
            {
                float v = option.value_or_default();
                if (ImGui::SliderFloat(label, &v, 0.0f, 1.0f, "%.2f"))
                    option = v;
            };
            slider("Skin detail / lighting", config->DlssNrSkinDetail);
            ImGui::BeginDisabled(!tone);
            slider("Skin colour", config->DlssNrSkinColour);
            ImGui::EndDisabled();
            slider("Environment detail / lighting", config->DlssNrEnvironmentDetail);
            slider("Environment colour", config->DlssNrEnvironmentColour);
            bool preview = config->DlssNrShowSkinMask.value_or_default();
            if (ImGui::Checkbox("Preview colour-based mask", &preview))
                config->DlssNrShowSkinMask = preview;
            ImGui::EndDisabled();
            ImGui::TreePop();
        }

        ImGui::TextDisabled("The model was trained on finished, sRGB-encoded frames. The upscaler's\n"
                            "output is not one: it is linear and open-ended. These decide how it is\n"
                            "mapped into something the model recognises. A frame the game reports as\n"
                            "already tone-mapped is passed over untouched and none of this applies.");

        {
            // Logarithmic, because the useful range is not linear. A quarter to 240: the low end because
            // a frame the game already tone mapped wants roughly 1, the high end because there is no
            // principled ceiling -- this is a divisor on an open-ended linear buffer, and how far up a
            // given game needs to go is a property of that game's exposure, not of anything we can bound.
            // One tester was still improving at 100. A linear slider over that span would spend nine
            // tenths of its travel on values nobody needs and never reach the ones they do.
            // One dropdown, because there is one answer.
            //
            // This was two checkboxes that could both be on, and every attempt to stop that was a patch
            // on a shape that should not have existed. Greying deadlocked -- each disabled the other, so
            // once both were set the only way out was a button the notice never mentioned. Clearing
            // worked but silently undid a setting somebody had made. Both were ways to stop an illegal
            // state being REACHED; a single choice cannot reach it, because there is only one value to
            // be in.
            //
            // Each option also says whether it can actually do anything in THIS game, in colour, so the
            // choice is made on what is available rather than on what sounds best.
            {
                const auto ex = DlssNr::GameExposureStatus();
                const bool vk = DlssNr::IsRunningVk();
                const bool haveExposure = vk ? DlssNr::ExposureOfferedVk() : ex.everOffered;

                const float anchorNow = DlssNr::ExposureScan::BestValue();
                const bool haveAnchor = !DlssNr::ExposureScan::Anchors().empty();

                static const char* sourceNames[] = { "Paper white only", "The game's own exposure",
                                                     "A buffer the scan found" };

                int source = (int) config->DlssNrWhitePointSource.value_or_default();

                if (source < 0 || source > 2)
                    source = 0;

                if (ImGui::Combo("White point from", &source, sourceNames, IM_ARRAYSIZE(sourceNames)))
                {
                    config->DlssNrWhitePointSource = (uint32_t) source;

                    // Nothing else to set. The scan asks the source whether it is wanted, so choosing
                    // it here is the whole of switching it on -- there is no second flag to keep in
                    // step, and so no way for the two to disagree.
                }

                HelpMarker("Where the number that divides the frame comes from."
                           "\n\nPaper white only -- the slider below and nothing else. Right for a"
                           "\ngame whose exposure never moves, wrong the moment it does: one"
                           "\nconstant cannot serve a cave and a field."
                           "\n\nThe game's own exposure -- read from the texture the game hands"
                           "\nthe upscaler. The best source there is, because it is decided"
                           "\nupstream and nothing this pass does can move it. Not every game"
                           "\nsupplies one."
                           "\n\nA buffer the scan found -- for games that compute an exposure and"
                           "\nnever pass it on. A GUESS: candidates are matched by shape, and in"
                           "\nGTA V the best one tracks the real exposure but at its own scale,"
                           "\nwhich the anchor's ratio cancels. Needs anchoring once, and checking"
                           "\nafterwards.");

                // Availability, in colour, for the option currently chosen.
                if (source == 1)
                {
                    if (!vk && ex.seenFrames == 0)
                        ImGui::TextDisabled("Waiting for a frame...");
                    else if (!haveExposure)
                        ImGui::TextColored(ImVec4(0.9f, 0.6f, 0.25f, 1.0f),
                                           "This game supplies no exposure -- paper white is in use. Try "
                                           "the scan instead.");
                    else if (vk)
                        ImGui::TextColored(ImVec4(0.45f, 0.8f, 0.45f, 1.0f),
                                           "This game supplies an exposure and it is being read.");
                    else if (ex.exposure > 1e-6f)
                    {
                        const float trim = std::clamp(config->DlssNrWhitePointTrim.value_or_default(), 0.25f, 4.0f);
                        ImGui::TextColored(
                            ImVec4(0.45f, 0.8f, 0.45f, 1.0f), "Game exposure %.4f  ->  white point %.2f%s", ex.exposure,
                            ex.preExposure / ex.exposure * trim, ex.offeredNow ? "" : "  (held: absent this frame)");
                    }
                    else
                        ImGui::TextDisabled("Reading the exposure...");
                }
                else if (source == 2)
                {
                    // "Nothing found" and "found several, none of them moving" are different states,
                    // and this said the first for both. In GTA V the log carried eight candidates while
                    // the panel claimed there were none, which reads as the scan being broken when what
                    // it actually needs is for the light to change.
                    if (anchorNow <= 0.0f)
                    {
                        const unsigned int watching = (unsigned int) DlssNr::ExposureScan::Report().size();

                        if (watching == 0)
                            ImGui::TextColored(ImVec4(0.9f, 0.6f, 0.25f, 1.0f),
                                               "Nothing in this game is shaped like an exposure.");
                        else
                            ImGui::TextColored(ImVec4(0.9f, 0.6f, 0.25f, 1.0f),
                                               "Watching %u, none moving yet -- go between light and shade.", watching);
                    }
                    else if (!haveAnchor)
                        ImGui::TextColored(ImVec4(0.9f, 0.6f, 0.25f, 1.0f),
                                           "Found one. Set paper white below until the picture looks "
                                           "right, then press Anchor here.");
                    // Once anchored, the scan -> white point readout sits above the sliders below; it is
                    // not repeated up here.
                }
                else if (haveExposure)
                {
                    ImGui::TextColored(ImVec4(0.45f, 0.8f, 0.45f, 1.0f),
                                       "This game supplies an exposure -- the option above would use it.");
                }
            }

            // A measured suggestion for paper white used to sit here and has been withdrawn.
            //
            // It took the 90th percentile of per-tile peak luminance from the untouched frame, which is a
            // statement about scene content rather than about the buffer's scale. In Nioh 3, where the
            // right answer is about 240, it offered 8 -- because most tiles are shadow and the percentile
            // sits wherever most tiles are. The guard meant to catch that compared each tile against the
            // frame's own brightest, which is scale-free and therefore passes on a black screen: the same
            // relative-threshold mistake the white point meter was removed for, made a second time.
            //
            // A wrong number offered confidently is worse than no number, so nothing is offered. What
            // replaces it has to be a measurement of the game's own exposure rather than of its scenery:
            // the exposure texture where a game supplies one, and otherwise the ratio between the
            // scene-referred buffer and the finished frame, which is that exposure by definition.

            // Two controls, not one control with two meanings.
            //
            // These are different quantities. The manual path wants an absolute divisor on an open-ended
            // linear buffer -- Nioh 3 needs about 240 -- and the exposure path wants a multiplier on a
            // number the game already supplied, where 1 is correct and anything far from it says the read
            // is wrong rather than that somebody prefers it.
            //
            // They used to share one stored value, narrowed to 0.25..4 when the toggle was on. That kept
            // a ruinous value unreachable but left two worse problems: moving the slider in one mode
            // silently destroyed the number found in the other, and there was no way back to "just take
            // the game's answer" short of knowing that the number for it was 1. Separate values fix both.
            // Switching modes is now non-destructive in both directions.
            // The trim belongs to both automatic sources, since both end in "the game's number times a
            // little". Only the manual source gets the absolute slider.
            // One slider per source, each remembering its own number.
            //
            // A trim on the game's exposure and a trim on a buffer the scan found are trims on different
            // things, and a value found against one means nothing against the other. Sharing them meant
            // changing source silently carried a number across, so a picture that had been tuned came
            // back wrong for a reason nothing on screen explained.
            //
            // The scan before it is anchored is the exception, and it has to be: anchoring captures an
            // absolute white point, so there must be an absolute slider to set. Showing a trim there
            // asked people to "set paper white below" next to a control that was not paper white.
            const int wpSource = (int) config->DlssNrWhitePointSource.value_or_default();

            // Which anchor row the paper-white slider edits, or -1 for the live unanchored point. Menu-
            // local and not persisted; the anchor block below sets it when a row is clicked. Declared
            // here because both the slider (this block) and the table (below) read it in the same frame.
            static int selectedAnchor = -1;
            auto anchors = DlssNr::ExposureScan::Anchors();
            if (selectedAnchor >= (int) anchors.size())
                selectedAnchor = -1;

            if (wpSource == 2)
            {
                const bool editingRow = selectedAnchor >= 0 && selectedAnchor < (int) anchors.size();

                // The single scan -> white point readout, above the sliders it explains.
                if (!anchors.empty())
                {
                    const float liveScan = DlssNr::ExposureScan::BestValue();

                    if (liveScan > 0.0f)
                    {
                        const float w = DlssNr::ExposureScan::AnchoredWhitePoint(
                            liveScan, config->DlssNrScanInverted.value_or_default(),
                            config->DlssNrScanTrim.value_or_default());

                        ImGui::TextColored(ImVec4(0.45f, 0.8f, 0.45f, 1.0f),
                                           "Scan %.5f  ->  white point %.2f   (%u point%s)", liveScan, w,
                                           (unsigned) anchors.size(), anchors.size() == 1 ? "" : "s");
                    }
                }

                // Paper white shows only when there is a point to set: before the first anchor, or when a
                // row is selected to edit. Once points exist and none is selected, the white point is fixed
                // by the anchors and only the trim adjusts the live picture -- so the trim takes the
                // slider's place, the same shape as the game-exposure source.
                const bool showPaperWhite = anchors.empty() || editingRow;

                if (showPaperWhite)
                {
                    float pw =
                        editingRow ? anchors[selectedAnchor].white : config->DlssNrWhitePointScale.value_or_default();

                    char lbl[48];
                    if (editingRow)
                        snprintf(lbl, sizeof(lbl), "Paper white (editing point %d)", selectedAnchor + 1);
                    else
                        snprintf(lbl, sizeof(lbl), "Paper white");

                    if (ImGui::SliderFloat(lbl, &pw, 0.25f, 2000.0f, "%.2fx", ImGuiSliderFlags_Logarithmic))
                    {
                        if (editingRow)
                        {
                            DlssNr::ExposureScan::AnchorSetWhite(selectedAnchor, pw);
                            config->DlssNrScanAnchors = DlssNr::ExposureScan::SerializeAnchors();
                        }
                        else
                            config->DlssNrWhitePointScale = pw;
                    }

                    HelpMarker("The white point for the selected calibration point, or -- with no row"
                               "\nselected -- the value the next Anchor press captures."
                               "\n\nSet it until the picture looks right here, then Anchor. Move to very"
                               "\ndifferent light and do it again: two points fix the buffer's real"
                               "\nrelationship and the white point holds between them. Click a row below"
                               "\nto come back and adjust that point; click it again to let go.");
                }

                // The trim multiplies the interpolated result, and in the steady state it is the control
                // that stands in for paper white: adjust it until the picture looks right in the current
                // light, then Anchor bakes that trimmed value into a new point and resets the trim to 1.
                if (!anchors.empty())
                {
                    float trim = config->DlssNrScanTrim.value_or_default();

                    if (ImGui::SliderFloat("Trim (x the scan)", &trim, 0.25f, 4.0f, "%.2fx",
                                           ImGuiSliderFlags_Logarithmic))
                        config->DlssNrScanTrim = std::clamp(trim, 0.25f, 4.0f);

                    ImGui::SameLine();

                    if (ImGui::SmallButton("Reset##scantrim"))
                        config->DlssNrScanTrim = 1.0f;

                    HelpMarker("A multiplier on the scan's white point, and the control you adjust between"
                               "\nanchor points: dial it until the picture looks right in the current"
                               "\nlight, then press Anchor here -- it captures the trimmed value as a new"
                               "\npoint and resets the trim to 1.");
                }
            }
            else if (wpSource == 1)
            {
                const bool ofScan = false;

                float trim = ofScan ? config->DlssNrScanTrim.value_or_default()
                                    : config->DlssNrWhitePointTrim.value_or_default();

                if (ImGui::SliderFloat(ofScan ? "Trim (x the scan)" : "Trim (x the game's exposure)", &trim, 0.25f,
                                       4.0f, "%.2fx", ImGuiSliderFlags_Logarithmic))
                {
                    if (ofScan)
                        config->DlssNrScanTrim = std::clamp(trim, 0.25f, 4.0f);
                    else
                        config->DlssNrWhitePointTrim = std::clamp(trim, 0.25f, 4.0f);
                }

                ImGui::SameLine();

                // Deliberately always present rather than greyed at 1. The point of it is that the safe
                // value is one click away without having to know what the safe value is.
                if (ImGui::SmallButton("Reset##wptrim"))
                {
                    if (ofScan)
                        config->DlssNrScanTrim = 1.0f;
                    else
                        config->DlssNrWhitePointTrim = 1.0f;
                }

                HelpMarker("A multiplier on the exposure the game supplied. 1.00x takes its number"
                           "\nexactly, and that is the right answer here."
                           "\n\nThis is not a fudge factor. If a game needs the trim far from 1 to look"
                           "\nright, that is evidence the exposure being read is wrong for that game,"
                           "\nnot that the game wants trimming. Somewhere around 0.8 to 1.25 is honest"
                           "\ntuning; reaching for 4 means something upstream is broken and the trim is"
                           "\nhiding it."
                           "\n\nYour manual paper white is kept separately and comes back untouched if"
                           "\nyou switch the option above off.");
            }
            else
            {
                // Logarithmic, because the useful range is not linear. A quarter to 2000: the low end
                // because a frame the game already tone mapped wants roughly 1, the high end because
                // there is no principled ceiling -- this is a divisor on an open-ended linear buffer, and
                // how far up a given game needs to go is a property of that game's exposure rather than
                // of anything that can be bounded here. One tester was still improving at 100.
                float wpScale = config->DlssNrWhitePointScale.value_or_default();

                if (ImGui::SliderFloat("Paper white", &wpScale, 0.25f, 2000.0f, "%.2fx", ImGuiSliderFlags_Logarithmic))
                    config->DlssNrWhitePointScale = wpScale;

                HelpMarker("What the frame is divided by before the model sees it. There is no other white"
                           "\npoint; this is the whole of it."
                           "\n\nThe model was trained on finished frames where white sits at 1. The"
                           "\nupscaler's output is linear and open-ended, so something has to say where"
                           "\nwhite is -- and where the game's DLSS buffer is linear HDR, that number is"
                           "\nrarely anywhere near 1. Measured in Monster Hunter Wilds it takes 16 or more"
                           "\nbefore the model's detail reaches the frame at all, and the value that suits"
                           "\na shaded camp is still too small for the same game out in daylight."
                           "\n\nToo low and almost every pixel trips the soft knee: the model is shown a"
                           "\nflat near-white picture, its answer is scaled away, and only its hue"
                           "\nsurvives -- which reads as a colour cast rather than as lost detail. Too"
                           "\nhigh and it is shown an underexposed one, its answer degrades, and this same"
                           "\nnumber multiplies that error on the way out."
                           "\n\nRaise it until the picture stops improving. Past that point it does not"
                           "\nplateau, it gets worse in the other direction."
                           "\n\nThis was once a multiplier on a measured white point. The measurement is"
                           "\ngone: it read scene brightness rather than where white belongs, handed the"
                           "\nmodel a picture three times too dark, and left the highlight path nothing to"
                           "\ngive back."
                           "\n\nAt strength zero the frame is still bit-identical whatever this says.");
            }

            // Highlight guard, directly under the white point / trim -- it bounds the model's edit and
            // belongs with the exposure controls it works alongside.
            float maxRatio = config->DlssNrMaxRatio.value_or_default();
            if (ImGui::SliderFloat("Highlight guard", &maxRatio, 1.0f, (float) DlssNr::kMaxPasses, "%.1fx"))
                config->DlssNrMaxRatio = maxRatio;

            ImGui::SameLine();
            if (ImGui::SmallButton("Reset##guard"))
                config->DlssNrMaxRatio = 2.0f;

            HelpMarker("The most the pass may move any pixel, as a multiple of what it already was, in"
                       "\nboth directions -- a pixel may not be brightened past this nor darkened past"
                       "\nits reciprocal. Lights are where the model has least to say and rescaling its"
                       "\nanswer does the most damage; 2x leaves detail intact while stopping a strip"
                       "\nlight turning into a string of coloured cells. Raise it only if bright areas"
                       "\nlook clipped.");

            // Directly under the white point, because that is the number it moves and the number the
            // anchor captures. It used to sit under Inspect, a whole section away from the slider it
            // reads, which left "Anchor here" looking like a control for something else entirely.
            {
                // No checkbox here any more.
                //
                // The dropdown above says whether the scan is the white point's source, and that is
                // the only reason anybody using this would want it running. A second control could
                // only agree with the dropdown or contradict it, and both were on offer: it began as
                // a redundant question and became a way to switch off the thing the chosen source
                // depended on.
                //
                // The ini key survives as a developer override for the one case a user has no reason
                // to want -- running the scan in a game that supplies a REAL exposure, so the log can
                // compare the two. That is validation, and validation does not need a widget.
                //
                // Worth keeping written down, since the panel no longer says it: the scan matches
                // buffers by SHAPE, and shape is a weak filter. In GTA V -- a game that supplies a
                // real exposure, so the right answer sat visible beside it -- the best candidate was
                // a 1x1 R32_FLOAT that climbed in a straight line for seventeen minutes while the
                // true exposure held still. Their ratio moved 14x. That is an accumulator, not an
                // eye adaptation.

                // Only where it means something. The lamp reads the scan, so offering it beside a
                // white point that comes from the game's own exposure is offering a control that
                // cannot light up.
                bool meter = config->DlssNrScanMeter.value_or_default();

                if (config->DlssNrWhitePointSource.value_or_default() == 2 &&
                    ImGui::Checkbox("Show the light meter on screen", &meter))
                    config->DlssNrScanMeter = meter;

                HelpMarker("A lamp in the corner: red for dark, green for full light, and the"
                           "\nshades between, with the reading beside it."
                           "\n\nIt is how you see at a glance that the scan is TRACKING rather"
                           "\nthan merely running. Walk into shade and it should slide toward"
                           "\nred; step out and it should go green. If it moves the wrong way,"
                           "\nthat is what the setting above is for."
                           "\n\nPurely a readout. It changes nothing.");

                // Shown when the scan is actually running, whichever way it got switched on.
                if (DlssNr::ExposureScan::Scanning())
                {
                    // Anchoring: one press, then it never needs touching again.
                    //
                    // The absolute white point cannot come out of a buffer whose units are unknown.
                    // Every value AFTER the first can: only the ratio against the anchor is used, so
                    // whatever the number means, it cancels. That is why this is a button and not a
                    // measurement -- the one thing a person can supply that no amount of cleverness
                    // can is "this looks right to me".
                    int which = 0;
                    float low = 0.0f, high = 0.0f;
                    const float live = DlssNr::ExposureScan::BestValue(&which, &low, &high);

                    const bool isSource = config->DlssNrWhitePointSource.value_or_default() == 2;

                    // Anchor captures (currentScan, currentPaperWhite) and ADDS a row -- it does not
                    // replace. One row is the old single-anchor ratio law; add a second in different
                    // light and the white point is interpolated between the points, so it holds across
                    // the whole range instead of only near one anchor. Greyed unless the scan is the
                    // chosen source and it currently has a value to capture.
                    ImGui::BeginDisabled(live <= 0.0f || !isSource);

                    if (ImGui::Button("Anchor here"))
                    {
                        // What to capture. Before the first point, the paper white above (an absolute value
                        // with the wide range a fresh game needs). After that, the EFFECTIVE white point the
                        // picture is showing right now -- the interpolated value times the Trim the user just
                        // dialed in -- so a second point in different light captures the trimmed look, not a
                        // frozen paper white (which would make two equal whites and a flat, non-tracking
                        // curve). The trim is reset afterwards: the new point, which the picture now passes
                        // through exactly, must not be multiplied by it a second time.
                        const float captureWhite =
                            anchors.empty() ? std::max(0.01f, config->DlssNrWhitePointScale.value_or_default())
                                            : std::max(0.01f, DlssNr::ExposureScan::AnchoredWhitePoint(
                                                                  live, config->DlssNrScanInverted.value_or_default(),
                                                                  config->DlssNrScanTrim.value_or_default()));

                        if (DlssNr::ExposureScan::AnchorAdd(live, captureWhite))
                        {
                            config->DlssNrScanAnchors = DlssNr::ExposureScan::SerializeAnchors();
                            config->DlssNrScanTrim = 1.0f;
                            selectedAnchor = -1;
                        }
                    }

                    ImGui::EndDisabled();

                    HelpMarker("Make the picture look right, then press this -- it captures the current look"
                               "\nas a point. For the first point use the Paper white slider above; for"
                               "\nevery point after, move to different light and use the Trim, which the"
                               "\nAnchor then bakes into a new point."
                               "\n\nThe first press calibrates one point -- the white point then"
                               "\nfollows the scan by ratio from there, as before. Walk into very"
                               "\ndifferent light, set paper white again, and press it again: the"
                               "\nsecond point pins down the buffer's real curve and everything"
                               "\nbetween the two is right, not just near one anchor. Up to eight."
                               "\n\nThe table is per game and shareable: one person calibrates a game"
                               "\nand the numbers are the same for everyone who takes the profile.");

                    if (!isSource)
                        ImGui::TextDisabled("(the scan is only watching -- the white point above comes "
                                            "from somewhere else)");

                    if (!anchors.empty())
                    {
                        // The row nearest the live scan value (in log space) is the one driving the
                        // picture right now; mark it so the user can see which calibration is in effect.
                        int active = 0;
                        float bestDist = 1e30f;
                        const float liveLog = std::log(std::max(live, 1e-6f));

                        for (size_t i = 0; i < anchors.size(); ++i)
                        {
                            const float d = std::fabs(std::log(std::max(anchors[i].scan, 1e-6f)) - liveLog);
                            if (d < bestDist)
                            {
                                bestDist = d;
                                active = (int) i;
                            }
                        }

                        for (size_t i = 0; i < anchors.size(); ++i)
                        {
                            ImGui::PushID((int) i);

                            // Delete first, so its click is never swallowed by the row-wide Selectable.
                            if (ImGui::SmallButton("x"))
                            {
                                DlssNr::ExposureScan::AnchorRemove((int) i);
                                config->DlssNrScanAnchors = DlssNr::ExposureScan::SerializeAnchors();
                                if (selectedAnchor == (int) i)
                                    selectedAnchor = -1;
                                else if (selectedAnchor > (int) i)
                                    --selectedAnchor;
                                ImGui::PopID();
                                continue;
                            }

                            ImGui::SameLine();

                            const bool sel = (int) i == selectedAnchor;
                            char row[96];
                            snprintf(row, sizeof(row), "%s scan %.4f  ->  white %.2f%s",
                                     ((int) i == active && isSource) ? ">" : "  ", anchors[i].scan, anchors[i].white,
                                     sel ? "   [editing]" : "");

                            // Click selects the row (slider edits it); click again deselects (slider
                            // returns to the live unanchored point).
                            if (ImGui::Selectable(row, sel))
                                selectedAnchor = sel ? -1 : (int) i;

                            ImGui::PopID();
                        }

                        ImGui::TextDisabled("Click a row to edit it with the slider above; click it again"
                                            " to control the live point. > is the point in use now.");
                    }

                    // The direction flag only means anything with a single point; with two or more the
                    // direction the white point moves is already fixed by the data.
                    if (anchors.size() == 1)
                    {
                        bool inverted = config->DlssNrScanInverted.value_or_default();
                        if (ImGui::Checkbox("The number runs the other way", &inverted))
                            config->DlssNrScanInverted = inverted;

                        HelpMarker("Flip this if the picture gets worse in the direction it should be"
                                   "\ngetting better. Most engines store an exposure that falls as"
                                   "\nthe scene brightens; some store its reciprocal, and a buffer"
                                   "\nfound by shape does not say which. Add a second anchor point in"
                                   "\ndifferent light and this is decided for you, so it disappears.");
                    }

                    // The scan -> white point readout is shown above the sliders now, not here.

                    // Everything below is read-out rather than control: what the scan is looking at and
                    // how to tell whether it found the right thing. Folded away because the two decisions
                    // that matter -- anchor, and which way the number runs -- are above it.
                    if (ImGui::TreeNode("Advanced"))
                    {

                        const auto found = DlssNr::ExposureScan::Report();
                        const char* why = DlssNr::ExposureScan::Status();

                        if (found.empty())
                        {
                            ImGui::TextDisabled("%s", why != nullptr && why[0] != 0 ? why : "nothing matched yet.");
                        }
                        else
                        {
                            for (size_t i = 0; i < found.size(); ++i)
                            {
                                const auto& c = found[i];

                                if (c.reads == 0)
                                {
                                    ImGui::TextDisabled("%zu. %s -- not read yet", i + 1, c.shape.c_str());
                                    continue;
                                }

                                // Moving is the whole signal, so it is the thing that is coloured.
                                ImGui::TextColored(c.moves ? ImVec4(0.45f, 0.8f, 0.45f, 1.0f)
                                                           : ImVec4(0.6f, 0.6f, 0.6f, 1.0f),
                                                   "%zu. %s = %.5f  (seen %.5f..%.5f) %s", i + 1, c.shape.c_str(),
                                                   c.latest, c.lowest, c.highest, c.moves ? "MOVES" : "flat so far");
                            }

                            ImGui::TextDisabled("Walk from shade into daylight. A real exposure moves.");
                            ImGui::TextDisabled("One that only ever climbs is a counter, not an exposure.");
                        }

                        ImGui::TreePop();
                    }
                }
            }
        }

        ImGui::SeparatorText("Compare");

        // Freeze the frame the model works on, so a setting change re-renders it in place -- the only
        // clean way to A/B our own settings (a moving scene confounds every other comparison). See
        // design/frame-hold.md.
        bool held = config->DlssNrHoldFrame.value_or_default();
        if (ImGui::Checkbox("Hold frame", &held))
            config->DlssNrHoldFrame = held;

        HelpMarker("Freezes the frame the model works on. While held, change paper white, the"
                   "\nstrengths, the reversible mode, the model preset -- anything below the"
                   "\nupscaler -- and only that setting moves; the scene does not."
                   "\n\nWhat it CANNOT show: DLSS/FSR/XeSS upscaler presets or anything upstream"
                   "\n(the upscaler is not re-run on a held frame), and the game's own HUD and"
                   "\npost-processing, which run after this pass and keep updating. The white"
                   "\npoint stops being measured and holds its value while frozen, so it cannot"
                   "\ndrift and confound the comparison."
                   "\n\nHide the menu and it stays held. Untoggle to resume.");

        static const char* compareNames[] = { "Off", "Side by side", "Wipe" };
        int compare = (int) config->DlssNrCompare.value_or_default();
        if (ImGui::Combo("Compare", &compare, compareNames, IM_ARRAYSIZE(compareNames)))
            config->DlssNrCompare = (uint32_t) compare;

        HelpMarker("Shows the pass against itself, so the two can be seen at once rather than"
                   "\ntoggled and remembered."
                   "\n\nSide by side puts the whole frame in each half, untouched on the left and"
                   "\nedited on the right. Both halves are squeezed horizontally to fit, so it is"
                   "\nfor looking at rather than playing in."
                   "\n\nWipe cuts a single frame at the split and resamples nothing, so the picture"
                   "\nis the right shape and can be played normally. Drag the split below; it is a"
                   "\nstored setting and stays put once the menu is closed."
                   "\n\nNeither needs the menu open to keep working. A hairline marks the join.");

        if (compare != 0)
        {
            bool swap = config->DlssNrCompareSwap.value_or_default();
            if (ImGui::Checkbox("Swap sides", &swap))
                config->DlssNrCompareSwap = swap;

            bool tags = config->DlssNrCompareTags.value_or_default();
            if (ImGui::Checkbox("Label the sides", &tags))
                config->DlssNrCompareTags = tags;

            HelpMarker("Writes which side is which onto the frame itself, so a screenshot still"
                       "\nsays so after it has left this machine. Drawn into the picture's own"
                       "\nplane: in the wipe the split reveals and hides the label exactly as it"
                       "\ndoes the images, and there is nothing to drag. Swap sides moves the"
                       "\nlabels with their pictures.");

            if (tags)
            {
                float tagScale = config->DlssNrTagScale.value_or_default();
                if (ImGui::SliderFloat("Label size", &tagScale, 0.5f, 5.0f, "%.1fx"))
                    config->DlssNrTagScale = std::clamp(tagScale, 0.5f, 5.0f);
            }

            HelpMarker("Puts the edited frame on the other side."
                       "\n\nWorth doing once you have decided which you prefer: the eye is not"
                       "\neven-handed about left and right, and a difference can read as an"
                       "\nimprovement purely from where it sits. If the same side still wins after"
                       "\nswapping, it is the pass you are seeing and not the placement.");
        }

        if (compare == 1)
        {
            float zoom = config->DlssNrCompareZoom.value_or_default();
            if (ImGui::SliderFloat("Zoom", &zoom, 1.0f, 2.0f, "%.2f"))
                config->DlssNrCompareZoom = std::clamp(zoom, 1.0f, 2.0f);

            HelpMarker("How much of the frame each half shows."
                       "\n\nA half is half as wide as the frame and just as tall, so the frame"
                       "\ncannot fill it and keep its shape."
                       "\n\nAt 1 the whole frame is there at its right proportions, with bars above"
                       "\nand below. At 2 the half is filled and the sides are cropped away"
                       "\ninstead. Anything between trades one for the other.");
        }

        if (compare == 2)
        {
            float split = config->DlssNrCompareSplit.value_or_default();
            if (ImGui::SliderFloat("Split", &split, 0.0f, 1.0f, "%.2f"))
                config->DlssNrCompareSplit = std::clamp(split, 0.0f, 1.0f);

            HelpMarker("Where the wipe cuts. Left of it is the frame as the upscaler produced it,"
                       "\nright of it is the frame the model edited.");
        }

        static const char* debugNames[] = { "Off", "Proxy (what the model sees)", "Model output (raw)",
                                            "Difference (amplified)" };
        int debugView = (int) config->DlssNrDebugView.value_or_default();
        if (ImGui::Combo("Debug view", &debugView, debugNames, IM_ARRAYSIZE(debugNames)))
            config->DlssNrDebugView = (uint32_t) debugView;

        HelpMarker("Proxy is the picture handed to the model -- if that looks wrong, the white point"
                   "\nis wrong and nothing downstream can be judged."
                   "\n\nDifference shows what the model actually changed, amplified twenty times and"
                   "\ncentred on grey. A flat grey frame there means it is doing nothing.");

        ImGui::PopItemWidth();
    }
}

} // namespace DlssNr
