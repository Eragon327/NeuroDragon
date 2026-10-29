#pragma once

#include <cstdint>

namespace neurodragon::vv::dlss {

// ---------------------------------------------------------------------------
// B2: the persistent SR feature (docs/specs/vv.md §15)
// ---------------------------------------------------------------------------
//
// VV never loads NGX itself for the engine's path: `_nvngx.dll` / `nvngx_dlss.dll`
// are absent from the process in that mode, so the engine's session cannot be
// reused. The offline `ngx_probe --sr` run proved the alternative on this machine:
// load the driver's core, `Init_ProjectID`/`Init_Ext`, `GetCapabilityParameters`,
// then `CreateFeature(1)` with explicit render/display sizes. This module does
// exactly that on the game's own D3D12 device and keeps one feature alive for the
// session, recreated when the sizes change, evaluated every frame on our command
// list.
struct SrState {
    bool     ready{};
    int      createRet{};
    int      evaluateRet{};
    unsigned renderWidth{};
    unsigned renderHeight{};
    unsigned displayWidth{};
    unsigned displayHeight{};
    /// What the quality slot's `DLSS.Hint.Render.Preset.Quality` actually holds after
    /// CreateFeature - read back through the measured getter (slot 11), never the value
    /// we asked for. 0 = driver default, and also what a failed read reports, so a
    /// preset the runtime refused cannot look applied in the panel.
    int      preset{};
};

[[nodiscard]] bool ensureSrFeature(void* device, void* commandList, unsigned renderWidth, unsigned renderHeight, unsigned displayWidth, unsigned displayHeight);

/// Drops the feature so the next ensureSrFeature() recreates it. Needed for knobs the
/// runtime only reads at CreateFeature (the model preset). The next frames reset the
/// temporal history themselves, so a rebuild costs one frame of accumulation.
void requestFeatureRebuild();

// Colour / depth / motion / output are ID3D12Resource*. Reset=1 clears TAA history.
[[nodiscard]] bool evaluateSr(
    void* commandList,
    void* color,
    void* depth,
    void* motionVectors,
    void* output,
    float jitterX,
    float jitterY,
    bool  reset
);

[[nodiscard]] SrState srState();
void              releaseSr();

} // namespace neurodragon::vv::dlss
