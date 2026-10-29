#pragma once

// ---------------------------------------------------------------------------
// One answer to "which SR backend is live right now" (docs/specs/ui.md §3)
// ---------------------------------------------------------------------------
// The panel used to read the RT path's stats directly, which under Vibrant Visuals
// is simply not running: every row read zero, every control wrote into a hook that
// never fires. The two backends are mutually exclusive by measurement (the TAAU
// anchors do not fire at all under ray tracing - docs/specs/vv.md §1), so one
// detector is enough, and the panel branches on its answer instead of on config.
//
// Detection is observation, not settings: `upscaling_mode = TAAU` with
// `upscaling_percentage = 100` looks enabled in options.txt and still never runs
// TAAU (EXP-0081), so only "did the per-frame call happen" counts.
//
// The header is engine-free and D3D12-free: only the .cpp reaches into the probe.
namespace neurodragon::sr {

enum class Backend : int {
    None       = 0, ///< neither path is running: no upscaler our SR could sit on
    EngineDlss = 1, ///< ray tracing: the engine's own DLSS feature 1
    VvDlss     = 2, ///< Vibrant Visuals: our DLSS replacing the engine's TAAU
};

struct Status {
    Backend  backend{Backend::None};
    bool     live{false};    ///< the backend was observed inside the freshness window
    bool     vvSeen{false};  ///< sticky for the session, so "never" and "paused" differ
    bool     engineSeen{false};
    unsigned ageMs{0};       ///< since the last observed frame (0 when live)
    unsigned renderWidth{0};
    unsigned renderHeight{0};
    unsigned displayWidth{0};
    unsigned displayHeight{0};
    /// RT: the engine created its feature for the current sizes.
    /// VV: our own NGX feature exists for the current sizes.
    bool     featureReady{false};
    /// The engine is configured for Vibrant Visuals (graphics_mode 2). This is not
    /// detection - it is what keeps the ratio control reachable in the one state
    /// observation cannot see: `upscaling_percentage` 100, where nothing runs at all.
    bool     vvConfigured{false};
    int      configuredPercent{-1}; ///< the engine's upscaling_percentage, -1 = unknown
};

/// Samples the per-frame counters and returns the current answer. Cheap (a few
/// atomics and one clock read); meant to be called once per panel frame.
///
/// Only the panel calls it, so the session's "ever seen" and the hysteresis both
/// run while the panel is open - which is exactly when the answer is read.
[[nodiscard]] Status status();

} // namespace neurodragon::sr
