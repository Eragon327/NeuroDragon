#pragma once

namespace neurodragon::vv::sr {

// ---------------------------------------------------------------------------
// R3: the SR pipeline's own buffers and the DLSS evaluate (docs/specs/vv.md §35)
// ---------------------------------------------------------------------------
//
//   engine colour (R11G11B10, render res) --\
//   engine depth  (R24G8,     render res) ---> prep compute -> RGBA16F / R32F / RG16F
//   engine arg11  (MV, render res) ---------/          |
//                                                      v
//                                             NGX DLSS-SR evaluate
//                                                      |
//                                        PS + RTV into the engine's arg10
//
// There is no tonemap, no encode, no white point and no back-buffer copy: arg10 is
// TAAU's own output, so the engine's downstream display chain applies to our result
// exactly as it does to TAAU's (decisions/0005).
struct Stats {
    bool     pipelineReady{};
    bool     recorded{};     ///< the last frame recorded the pipeline
    unsigned frame{};        ///< in-world SR frame number of that record (0 = none yet)
    int      lastError{};    ///< HRESULT of the last failed step
    unsigned renderWidth{};
    unsigned renderHeight{};
    unsigned displayWidth{};
    unsigned displayHeight{};
    void*    colorSource{};  ///< engine texture read as colour this frame
    void*    depthSource{};  ///< engine texture read as depth this frame
    unsigned colorFormat{};
    unsigned depthFormat{};
    void*    mvSource{};   ///< engine arg11 this frame (null = none; the frame is skipped)
    unsigned mvFormat{};   ///< its DXGI format, 0 when there was none
    unsigned colorCandidates{}; ///< how many colour textures the inventory offered
    unsigned depthCandidates{};
    int      colorChoice{};     ///< which one is used (0 = newest)
    int      depthChoice{};
    bool     dlssReady{};       ///< the NGX feature exists for the current sizes
    int      dlssCreateRet{};
    int      dlssEvaluateRet{};
};

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

/// F2: copy the next injected frame's TAAU output (arg10, before the DLSS write) into
/// our own texture, so `dumpInputs()` can save `sr_taauref_*.bin` next to
/// `sr_dlssout_*.bin`. Same frame, same inputs - the offline comparison then has no
/// exposure/motion gap. One shot; the capture frame is logged.
void captureTaaRef();

/// R3 stage B: prep + DLSS evaluate, then a PS writes the DLSS output into the
/// engine's arg10 with an RTV (no tonemap, no encode, no back-buffer copy).
/// The resource is left in RENDER_TARGET; a missing DLSS evaluation fails instead
/// of falling back to anything.
[[nodiscard]] bool recordArg10(void* commandList, void* arg10);

[[nodiscard]] Stats stats();

void release();

} // namespace neurodragon::vv::sr
