#pragma once

namespace neurodragon::vv::capture {

// ---------------------------------------------------------------------------
// B1c: cut capture (docs/specs/vv.md §14.4)
// ---------------------------------------------------------------------------
//
// At the injection boundary the back buffer holds whatever the engine had drawn
// up to that list. Copying it into a readback texture and saving a BMP therefore
// answers, in one session and without touching a single engine texture:
//
//   * what each of the last lists actually contributes (sweep the boundary and
//     look at the pictures: the first frame that shows the HUD tells us where the
//     UI block starts);
//   * whether the cut sees the world before the hand/HUD (the insertion point);
//   * what TAAU's own output looks like in the same frame - the reference image
//     the colour candidate and the tone curve are fitted against offline.
//
// The copy is recorded into the injection's own command list. No engine resource
// is written, and the back buffer is returned to PRESENT before the suffix lists
// run. The back-buffer reference itself is still owned by the injector and
// released in its submitted().
//
// One recording per frame at most, into a small ring: a slot is only reused after
// its fence has passed, so a slow frame skips the copy instead of overwriting the
// previous one.

struct Stats {
    unsigned captured{};  ///< BMPs written since start()
    unsigned skipped{};   ///< frames where no slot was free / the copy failed
    unsigned pending{};   ///< recordings still on the GPU
    unsigned tail{};      ///< tail-relative k of the last capture (0 = counted from the head)
    unsigned split{};     ///< boundary actually used in the last capture
    unsigned listCount{}; ///< engine lists in that frame
    unsigned width{};     ///< back buffer size of the last capture
    unsigned height{};
    int      lastError{}; ///< HRESULT of the last failed step (0 = none)
    wchar_t  lastFile[80]{};
};

/// Resets the counters and the recording state. Safe to call while armed.
void start();

/// Records "copy the current back buffer" into `commandList` and arms a slot.
/// Returns false when nothing can be recorded (no free slot / no device), in which
/// case the caller must skip the whole injection instead of submitting an empty
/// list. `backBuffer` is owned by the caller.
[[nodiscard]] bool record(void* commandList, void* backBuffer);

/// Signals the capture fence after the three-part submission. `split`/`listCount`
/// are the boundary that was actually used; they go into the file name so the
/// offline tool never has to guess which cut a picture belongs to.
void onSubmitted(void* queue, unsigned split, unsigned listCount, bool fromTail);

/// Called once per presented frame: maps finished recordings and writes the BMPs.
void poll();

void release();

[[nodiscard]] Stats stats();

} // namespace neurodragon::vv::capture
