#pragma once

#include <cstdint>

// ---------------------------------------------------------------------------
// B1a: self-built NGX session probe (docs/specs/vv.md §13.6)
// ---------------------------------------------------------------------------
//
// VV never loads NGX: `_nvngx.dll` / `nvngx_dlss.dll` are absent from the
// process, so the engine's session cannot be reused. The offline `ngx_probe
// --sr` run proved the alternative on this machine: load the driver's core,
// `Init_ProjectID`/`Init_Ext`, `GetCapabilityParameters`, then
// `CreateFeature(1)` with explicit render/display sizes.
//
// This module repeats exactly that on the game's own D3D12 device, on explicit
// request only (panel button). It creates a feature and releases it again; it
// never evaluates, never touches an engine texture, and does not change a pixel.
// Everything is logged with a `[vvsr]` tag and exposed as POD for the panel.
namespace neurodragon::vv::dlss {

struct ProbeState {
    int      initRet{};   ///< NVSDK_NGX_D3D12_Init_Ext return code (1 = OK)
    int      capsRet{};   ///< GetCapabilityParameters return code
    int      createRet{}; ///< CreateFeature(1) return code
    void*    feature{};    ///< handle returned by CreateFeature (released after the probe)
    void*    params{};     ///< capability parameter object
    unsigned renderWidth{};
    unsigned renderHeight{};
    unsigned displayWidth{};
    unsigned displayHeight{};
    bool     coreLoaded{};
    bool     initialized{};
    bool     done{};
};

// Queues one probe. Safe to call from the panel (render thread); the work runs at
// the beginning of the next Present, where the game queue/device are known.
void requestProbe();

// Called from the overlay's Present hook. No-op unless a probe was requested.
void runPending();

[[nodiscard]] ProbeState probeState();

// Releases the feature/session this module owns. Safe to call when nothing was
// created; used by the mod's uninstall path.
void release();

// ---------------------------------------------------------------------------
// B2: the persistent SR feature (docs/specs/vv.md §15)
// ---------------------------------------------------------------------------
//
// The probe above creates a feature, releases it and keeps only the session. The SR
// pipeline needs the opposite: one feature alive for the session, recreated when the
// sizes change, evaluated every frame on our command list.
struct SrState {
    bool     ready{};
    int      createRet{};
    int      evaluateRet{};
    unsigned renderWidth{};
    unsigned renderHeight{};
    unsigned displayWidth{};
    unsigned displayHeight{};
    /// DLSS.Hint.Render.Preset.* value the feature was created with (0 = driver
    /// default). Read back into the panel so a preset that did not take is visible
    /// instead of silently echoing the config.
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
