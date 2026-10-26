#pragma once

namespace neurodragon::probe::vv::inject {

// ---------------------------------------------------------------------------
// Boundary / injection experiment (docs/specs/vv.md §12 A)
// ---------------------------------------------------------------------------
//
// The engine submits a whole frame in ONE ExecuteCommandLists call that carries
// ~48 command lists (docs/specs/vv.md §3). This module proves that such a
// submission can be cut into "prefix + our own list + suffix" on the same queue,
// and - by clearing the back buffer at the cut and looking at what survives - where
// the TAAU draw and the UI sit inside that frame.
//
// It is an experiment, not a feature: with Mode::Off nothing is created, recorded
// or submitted, and no engine resource is touched. Everything it writes goes into a
// command list of its own; the only engine-owned object it ever holds a reference to
// is the current back buffer, and that reference is released immediately after the
// split submission (holding one makes the engine's ResizeBuffers fail, and the
// engine answers that with abort() - docs/specs/vv.md §9).
//
// What the three outcomes mean (docs/specs/vv.md §12 A):
//
//   * screen turns the clear colour, UI still drawn  -> the cut is before the UI;
//   * the whole screen turns the clear colour         -> the cut is after the UI;
//   * nothing changes                                 -> a later list overwrote the
//                                                       back buffer, i.e. the cut is
//                                                       too early (raise k).
//
// The boundary is 1-based and counts *lists*, so `k` means "submit the first k
// engine lists, then our list, then the rest". k == listCount puts our work at the
// very end of the frame.

enum class Mode : int {
    Off   = 0, ///< forward every submission unchanged
    Fixed = 1, ///< always cut at setK()
    Sweep = 2, ///< advance k every setHoldFrames() frames, one pass at a time
};

/// What our inserted command list does. Clear is the M5 boundary experiment;
/// Bypass is B1b-1 (read the render-resolution colour, ACES it over the back
/// buffer); Capture is B1c (copy the back buffer into a readback and save a BMP,
/// changing no pixel). All write only our own resources and are off unless a Mode
/// is active.
enum class Action : int {
    Clear   = 0,
    Bypass  = 1,
    Capture = 2,
    Sr      = 3,
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
};

struct Stats {
    unsigned   listsPerSubmit{}; ///< size of the last engine submission seen
    unsigned   minLists{};       ///< smallest / largest engine submission since the mode
    unsigned   maxLists{};       ///< was switched on (the per-frame count is not constant)
    unsigned   k{};              ///< boundary setting (from the head, or from the tail)
    unsigned   split{};          ///< boundary actually used by the last injection
    unsigned   injected{};       ///< frames that really were split
    unsigned   skipped{};        ///< attempts that did nothing (see `reason`)
    unsigned   sweeps{};         ///< completed Sweep passes
    bool       ready{};          ///< device + swap chain seen and pipeline built
    bool       fromTail{};       ///< k counts back from the last list
    SkipReason reason{SkipReason::None};
};

void setMode(Mode mode);
[[nodiscard]] Mode mode();

void setAction(Action action);
[[nodiscard]] Action action();

/// When set, `prepare()` first tries to cut immediately after the engine list that
/// was recording during the TAAU draw (probe::vv::taauListForFrame()). This is the
/// content boundary; if the list was not captured it falls back to k.
void setUseTaauList(bool use);
[[nodiscard]] bool useTaauList();

/// When set, `prepare()` cuts immediately after the list that just finished the
/// world composite (probe::vv::backBufferWriterSplit()): the first list that bound a
/// back buffer and drew. That anchor is independent of how many lists the frame has,
/// which a tail count is not (docs/specs/vv.md §14.5). Falls back to k when the probe
/// did not see a back-buffer writer.
void setUseBackBuffer(bool use);
[[nodiscard]] bool useBackBuffer();

/// Which end `k` is measured from. From the head (default) k is "after the k-th list";
/// from the tail k = 1 means "after the whole frame", k = 2 "before the last list", and
/// so on. The engine's list count is NOT constant (69 in the main-menu dressing room,
/// 74/81/107 on individual frames, 48-51 in a world), so a tail-relative boundary is
/// the only one that can stay put across frames - and it answers the question the next
/// milestone needs: is the UI always the last list?
void setFromTail(bool fromTail);
[[nodiscard]] bool fromTail();

void setK(unsigned k);
void setHoldFrames(unsigned frames);
/// How many boundaries one Sweep pass visits (0 = the whole frame). The B1c cut
/// capture only needs the tail: tail k=1..8 is "the last eight lists", and a full
/// pass would write one 13 MB BMP per list.
void setSweepDepth(unsigned depth);
[[nodiscard]] unsigned sweepDepth();

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

/// Whether `list` is this module's own command list. The probe hooks Close on the
/// shared list vtable, so recording our own list would otherwise show up in the
/// engine's per-frame boundary count and quietly corrupt that measurement.
[[nodiscard]] bool isOwnList(void* list);

/// True while this module creates the back-buffer view inside record(). The probe
/// hooks CreateRenderTargetView on the engine's device, and the back buffer is not
/// part of the frame graph it is inventorying: its views must not enter that list.
[[nodiscard]] bool recordingOwnView();

/// Called after the three-part submission: signals our fence, releases the back
/// buffer reference and advances a Sweep.
void submitted(unsigned splitAt, unsigned listCount);

/// Drops everything this module created. Safe to call when nothing was built, and
/// safe to call from the mod's uninstall path.
void release();

[[nodiscard]] Stats stats();

} // namespace neurodragon::probe::vv::inject
