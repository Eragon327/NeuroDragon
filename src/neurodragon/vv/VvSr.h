#pragma once

namespace neurodragon::vv::sr {

// ---------------------------------------------------------------------------
// B2: the SR pipeline's own buffers and shaders (docs/specs/vv.md §15)
// ---------------------------------------------------------------------------
//
// This is the plumbing between the engine's frame and a super-resolution pass:
//
//   engine colour (R11G11B10, render res) --\
//   engine depth  (R24G8,     render res) ---> prep compute -> RGBA16F / R32F / RG32F
//                                              |
//                            (next step: NGX DLSS-SR evaluate)
//                                              v
//                                        final compute: VvTone encode -> RGBA8
//                                              |
//                                     CopyTextureRegion onto the back buffer
//
// Until the NGX feature is wired in, the final pass can show any of the prepared
// buffers (colour / depth / motion), which is what makes the plumbing verifiable
// on screen before a single DLSS parameter is written.
enum class View : int {
    Bypass = 0, ///< prepared colour through the measured encode (should match the game)
    Depth  = 1, ///< R32F depth as greyscale (near = white, the engine is reversed-Z)
    Motion = 2, ///< RG32F motion vectors, scaled for visibility
    Dlss   = 3, ///< the NGX output texture (falls back to Bypass while there is none)
    Raw    = 4, ///< prepared colour straight out, no encode (isolates read vs curve)
};

struct Stats {
    bool     pipelineReady{};
    bool     recorded{};     ///< the last frame recorded the pipeline
    unsigned frame{};        ///< in-world SR frame number of that record (0 = none yet)
    int      view{};
    int      lastError{};    ///< HRESULT of the last failed step
    unsigned renderWidth{};
    unsigned renderHeight{};
    unsigned displayWidth{};
    unsigned displayHeight{};
    void*    colorSource{};  ///< engine texture read as colour this frame
    void*    depthSource{};  ///< engine texture read as depth this frame
    unsigned colorFormat{};
    unsigned depthFormat{};
    unsigned colorCandidates{}; ///< how many colour textures the inventory offered
    unsigned depthCandidates{};
    int      colorChoice{};     ///< which one is used (0 = newest)
    int      depthChoice{};
    bool     dlssReady{};       ///< the NGX feature exists for the current sizes
    int      dlssCreateRet{};
    int      dlssEvaluateRet{};
};

void setView(View view);
[[nodiscard]] View view();

/// The inventory order is creation order, not "current frame" - the B1c capture found
/// three fmt=26 textures and only one of them is the colour TAAU reads. Cycling is
/// how that gets settled on screen instead of by guessing.
void cycleColor();
void cycleDepth();

/// Writes the colour and depth textures the SR pass actually read to the game
/// directory (`sr_colour_<run>_p<ptr>_<w>x<h>_fmt<f>.bin`, `sr_depth_...`), plus one
/// `sr_cand_<run>_colour_<i>_p<ptr>_<w>x<h>_fmt<f>.bin` per render-size fmt=26
/// candidate (and the same for the depth candidates), in the NDRW format the B0/B1c
/// tools already read. The run number is per dump, so two dumps taken from two camera
/// positions never overwrite each other. Explicit trigger only: the copy has to happen
/// while the engine is not rebuilding (the 2026-10-24 22:58 lesson).
[[nodiscard]] bool dumpInputs();

/// How many input dumps have been started this session - the `#N` in the dump lines.
/// Only the TEST hotkey path needs it, so it can name the run it just triggered.
[[nodiscard]] unsigned dumpRuns();

/// Records the prep + final passes into `commandList` and copies the result over
/// `backBuffer`. Both are void* so this header stays free of d3d12.h.
[[nodiscard]] bool record(void* commandList, void* backBuffer);

/// R3 stage B: the same prep + DLSS evaluate, but the output is written into the
/// engine's arg10 with a PS + RTV (no tonemap, no encode, no back-buffer copy).
/// The resource is left in RENDER_TARGET; a missing DLSS evaluation fails instead
/// of falling back to anything.
[[nodiscard]] bool recordArg10(void* commandList, void* arg10);

[[nodiscard]] Stats stats();

void release();

} // namespace neurodragon::vv::sr
