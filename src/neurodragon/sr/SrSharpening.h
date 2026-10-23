#pragma once

#include <cstdint>

// Engine SR post-process: RCAS sharpening of the frame the engine's own DLSS just
// produced (docs/K §2.2 item ①, docs/J §5 for why the DLSS "Sharpness" parameter
// is not the way).
//
// Placement matters twice over:
//   * it runs after the engine's DLSS evaluate and before its colour grading and
//     UI composition, so the HUD and text are never sharpened;
//   * that image is linear HDR (the engine creates DLSS with IsHDR and grades
//     afterwards - docs/D §2), while RCAS only solves for inputs in [0,1]: above 1
//     `hitMax` turns positive and the lobe weight clamps to zero, so a straight
//     port would sharpen shadows but skip every pixel that straddles a highlight.
//     The pass therefore maps through Reinhard (x/(1+x), the same curve the NR
//     encoder uses), sharpens in that domain and maps back.
//
// The header is engine-free and D3D12-free on purpose: it is included from the
// probe (engine headers) and from the panel (ImGui), while <d3d12.h> stays inside
// the .cpp (docs/D §4 pitfall 5).
namespace neurodragon::sr {

struct SharpStats {
    bool     enabled  = false; // sr.sharpening in config.json
    bool     active   = false; // the last evaluation actually recorded the pass
    bool     ready    = false; // shader, pipeline and scratch textures are built
    unsigned frames   = 0;     // frames sharpened since load
    unsigned failures = 0;     // pipeline/texture creation failures
    float    strength = 0.0f;  // strength used by the last pass
};

// Records the sharpening pass on the engine's command list. Call it once per
// engine DLSS evaluation, after the original EvaluateFeature came back with
// success (a failed evaluation leaves a stale Output, and sharpening that every
// frame would stack artifacts).
//
// No-op when the config switch is off, the strength is zero, the output is not a
// 2D texture, or a window resize is still settling - the scratch pair follows the
// output size, and rebuilding it on every drag frame would burn VRAM for nothing.
//
// The engine's Output is only ever put in COPY_SOURCE / COPY_DEST ('all shader
// resource' outside), never in UNORDERED_ACCESS: the pass reads a copy and writes
// a private target, then copies back. Same pattern the TEST colour dumps already
// use on that resource (docs/D §6.16 rule 2: do not flip engine textures through
// UAV states).
void recordSharpening(void* commandList, void* engineOutput, std::uint64_t frame);

[[nodiscard]] SharpStats sharpStats();

// Called from the mod's unload. The pipeline objects are released; textures still
// referenced by an open command list are deliberately left allocated (a leak costs
// memory once, a release while the GPU is reading costs the session - the same
// rule as NR's retired objects, docs/D §6.16).
void shutdown();

} // namespace neurodragon::sr
