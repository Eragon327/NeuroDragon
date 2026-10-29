#pragma once

#include <string>

namespace neurodragon::probe::options {

// ---------------------------------------------------------------------------
// Engine option writes (docs/specs/vv.md §17)
// ---------------------------------------------------------------------------
// The upscaler is selected by the `upscaling_mode` option and the render ratio by
// `upscaling_percentage`; both live in the engine's own registry, reachable from the
// client instance (`ClientInstance::getOptions()` -> `IOptionRegistry` -> `Option`).
// The registry is client state, so every write has to run on the client thread:
// `applyPending()` is drained by the client level tick and by the level-join event,
// never from Present.
//
// Why the ids are written down instead of resolved by name: this build's headers ship
// an empty `OptionID` enum, and the tag scan over 0..1023 is a one-off diagnostic
// (EXP-0020), not something to repeat per write. A bare number is not trusted though -
// every write re-reads the option's own save tag first and refuses on a mismatch, so
// a game update that shifts the ids turns the write into a logged no-op instead of
// writing into an unrelated setting.

/// The engine options this mod writes, by the id the registry uses on 1.26.51.01./// The values are the ones the EXP-0020 scan printed for `upscaling_percentage`,
/// `upscaling_mode`, `gfx_upscaling`, `target_resolution` and `graphics_mode`; the
/// 26.51.9 LeviLamina headers carry the same numbers (26.51.7 ships the enum empty).
enum class Known : int {
    UpscalingPercentage = 579, ///< upscaling_percentage: render ratio in percent (0 = native)
    UpscalingMode       = 580, ///< upscaling_mode: 0 = TAAU, 1 = Bilinear
    AdvancedUpscaling   = 581, ///< gfx_upscaling
    TargetResolution    = 582, ///< target_resolution: absolute target, DRS turns it into a factor
    GraphicsMode        = 703, ///< graphics_mode: 2 = Vibrant Visuals, 3 = ray tracing
};

/// Outcome of the last write, for the panel and the log. `text` is already formatted.
struct WriteResult {
    bool attempted{};   ///< a request reached the registry this run
    bool ok{};          ///< the tag matched and the option now holds the requested value
    char text[160]{};   ///< e.g. "579 upscaling_percentage 66 -> 50"
};

/// Queues a write. Replaces an older pending request of the same slot (last wins, the
/// replacement is logged). Never touches the registry on the calling thread.
void requestWrite(Known option, std::string value);

/// Drains the pending write on the client thread. No-op when nothing is queued or the
/// client instance does not exist yet (the request stays queued for the next call).
void applyPending();

[[nodiscard]] WriteResult lastWrite();

/// The engine's configured graphics/upscaling state, read from the same registry.
/// The panel needs it for one case observation cannot answer: at `upscaling_percentage
/// = 100` nothing runs, so "is the player configured for VV or for ray tracing" decides
/// whether the ratio control is the way back or has no business being there.
struct KnownValues {
    bool valid{false};
    int  graphicsMode{-1};      ///< 2 = Vibrant Visuals, 3 = ray tracing
    int  upscalingPercent{-1};  ///< 0 = native (no upscaling to replace)
};

[[nodiscard]] KnownValues knownValues();

/// Re-reads the two options above on the client thread. Throttled unless `force`, so a
/// level tick can call it without paying two registry reads every frame.
void refreshKnownValues(bool force = false);

// TEST only: the A2 route that leaves the TAAU branch by switching `upscaling_mode`
// to Bilinear through the same queue. Kept as a named entry point because the
// `force_bilinear.flag` bootstrap calls it before any world exists.
void requestBilinear();

} // namespace neurodragon::probe::options
