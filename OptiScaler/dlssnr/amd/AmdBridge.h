#pragma once
#include <d3d12.h>
#include <nvsdk_ngx.h>
#include <string>
namespace DlssNr::AmdBridge
{
bool HasFiles();
bool Before(ID3D12GraphicsCommandList*, NVSDK_NGX_Parameter*, ID3D12CommandQueue*);
// Post-upscale placement: runs the model over the frame the upscaler (or Ray Reconstruction) has
// already finished, and hands its answer back instead of substituting the upscaler's input. The
// caller owns writing it into the frame, because only it knows that surface's format and state.
// Returns true when the backend owns this frame (result may still be null on a skipped frame).
bool After(ID3D12GraphicsCommandList*, NVSDK_NGX_Parameter*, ID3D12CommandQueue*, ID3D12Resource** outResult);
void Restore(NVSDK_NGX_Parameter*);
bool HasReplacement(NVSDK_NGX_Parameter*);
void InvalidateHistory();
void TraceContextRelease(unsigned int handle, bool after);
std::string Status();
// Smoothed GPU time the neural work takes on the game's queue per frame, all passes; 0 before the
// first reading.
float NeuralMs();
// Dynamic NR resolution: the scale in use and the rendered frame rate, or empty while it is off.
std::string DynamicStatus();
bool GraphicsRestartNeeded(UINT activePasses);
// pass1 SHA name ("0.3.0" / "0.3.1" / …) or nullptr if missing/unknown.
// Cached for menu display until the DLL path, size, or write time changes.
const char* RuntimeName();
} // namespace DlssNr::AmdBridge
