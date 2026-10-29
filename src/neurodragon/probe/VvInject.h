#pragma once

namespace neurodragon::probe::vv::inject {

// ---------------------------------------------------------------------------
// arg10 injection (docs/specs/vv.md §35.2)
// ---------------------------------------------------------------------------
//
// The engine submits a whole frame in ONE ExecuteCommandLists call that carries
// ~48 command lists (docs/specs/vv.md §3). The product cuts that submission at the
// list that wrote the engine's TAAU output (arg10) and submits a command list of
// our own between the two halves: that list runs the SR pipeline's prep + DLSS
// evaluate and writes the result into arg10.
//
// With Mode::Off nothing is created, recorded or submitted, and no engine resource
// is touched. Everything we write goes into a command list of its own; the only
// engine-owned objects it ever holds a reference to are the current back buffer
// (released immediately after the split submission, because holding one makes the
// engine's ResizeBuffers fail and the engine answers that with abort()) and arg10,
// which is what the product exists to write.
//
// There is exactly one cut point and one action. The boundary is 1-based and counts
// *lists*: "submit the first k engine lists, then our list, then the rest".

enum class Mode : int {
    Off   = 0, ///< forward every submission unchanged; also clears productArmed
    Fixed = 1, ///< inject at the current frame's arg10 writer
};

/// What our inserted command list does. DlssArg10 is the product path: the SR
/// pipeline's prep + DLSS evaluate, then a PS that writes the DLSS output into the
/// engine's arg10. It deliberately writes one engine resource and leaves it in
/// RENDER_TARGET.
enum class Action : int {
    DlssArg10 = 3,
};

/// Why an injection attempt did nothing. The panel turns these into text (the probe
/// itself writes English log lines only).
enum class SkipReason : unsigned {
    None = 0,
    NoDevice,
    NoSwapChain,
    FenceBusy,
    NoBackBuffer,
    PipelineFailed,
    RecordFailed,
    NoBoundary,
};

struct Stats {
    unsigned   listsPerSubmit{}; ///< size of the last engine submission seen
    unsigned   split{};          ///< boundary actually used by the last injection
    unsigned   injected{};       ///< frames that really were split
    unsigned   skipped{};        ///< attempts that did nothing (see `reason`)
    bool       ready{};          ///< device + swap chain seen and pipeline built
    SkipReason reason{SkipReason::None};
};

void setMode(Mode mode);
[[nodiscard]] Mode mode();

void setAction(Action action);
[[nodiscard]] Action action();

/// The boundary comes from probe::vv's arg10 writer resolution: the list whose
/// OMSetRenderTargets bound this frame's arg10. There is no other cut point and no
/// fallback - a frame whose writer is not found is not injected at all, so a missed
/// anchor can never turn into a frame written in the wrong place.
void setUseArg10(bool use);
[[nodiscard]] bool useArg10();

/// The engine's own device (VvProbe passes it once the first engine submission is
/// seen). The pipeline is built lazily on it, at the first injection attempt.
void setDevice(void* device);

/// The swap chain, from the overlay's Present hook. The current back buffer is read
/// from it while injecting and released right after the submission.
void noteSwapChain(void* swapChain);

/// Called from the overlay's ExecuteCommandLists hook, before forwarding.
/// Returns the boundary to cut at (0 = forward the call unchanged). When it returns
/// non-zero, `list()` is a closed command list ready to be submitted between
/// lists[k-1] and lists[k].
unsigned prepare(void* queue, unsigned listCount, void* const* lists);

/// The command list recorded by the last prepare(). Valid until submitted().
[[nodiscard]] void* list();

/// Whether `list` is this module's own command list.
[[nodiscard]] bool isOwnList(void* list);

/// True while this module creates a resource view itself (arg10's RTV). Without it,
/// one view per frame floods the probe's 4096-entry RTV map (EXP-0059: the arg10
/// writer stopped resolving after ~40 s).
[[nodiscard]] bool recordingOwnView();
void setRecordingOwnView(bool recording);

/// Called after the three-part submission: signals our fence and releases the back
/// buffer reference.
void submitted();

/// Drops everything this module created. Safe to call when nothing was built, and
/// safe to call from the mod's uninstall path.
void release();

[[nodiscard]] Stats stats();

} // namespace neurodragon::probe::vv::inject
