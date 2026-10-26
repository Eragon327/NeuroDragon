#pragma once

namespace neurodragon::probe::options {

// ---------------------------------------------------------------------------
// Runtime option switch (TEST only) - docs/specs/vv.md §17
// ---------------------------------------------------------------------------
// A2 needs the engine out of the TAAU branch without editing the player's
// options.txt: the upscaler is selected by the `upscaling_mode` option, and the
// engine's own registry is reachable from the client instance
// (`ClientInstance::getOptions()` -> `IOptionRegistry` -> `Option`).
//
// `OptionID` ships empty in the LeviLamina headers, so the option is found by its
// save tag (the options.txt key) over the low id range, which is what
// `isValidOption` exists for. The option objects are client state, so the actual
// registry access must run on the client thread. Two routes exist:
//
//   * `applyRequested()` is called from the present hook (render thread) and hands
//     the request to `ClientThreadExecutor`. Measured 2026-10-26 (EXP-0018/0019):
//     the task was handed over before the main menu finished loading and never ran,
//     so this route alone is not enough.
//   * `applyNow()` is called from the client level-join event, which is already on
//     the client thread; it runs the registry access immediately.
//
// The request is kept until one of the routes actually runs, so the engine can be
// in TAAU for a while and still be switched later.

// TEST only: record that the engine should be switched to Bilinear.
void requestBilinear();

// TEST only: post the pending request to the client thread (no-op when none is
// pending or a switch already ran). Called once per present.
void applyRequested();

// TEST only: run the pending request on the calling thread (the client level-join
// event uses this). Safe to call when nothing is pending.
void applyNow();

} // namespace neurodragon::probe::options
