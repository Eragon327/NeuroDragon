#pragma once

namespace neurodragon::vv::bypass {

// ---------------------------------------------------------------------------
// B1b-1: colour bypass at the tail k=4 insertion point (TEST build)
// ---------------------------------------------------------------------------
//
// Reads the newest render-resolution R11G11B10 colour from the probe inventory,
// upscales it to the back buffer with a compute pass and writes it through the
// measured display encode (VvTone.h, B1c), then copies the RGBA8 result over the
// back buffer. It is a deliberately blurry "does the insertion point, the colour
// input and the transfer function work" step: it replaces TAAU's output for one
// frame, nothing else. The engine's UI and hand are drawn after the cut, so they
// stay.
struct Stats {
    bool     pipelineReady{};
    bool     recorded{};
    unsigned colorFormat{};
    unsigned srcWidth{};
    unsigned srcHeight{};
    unsigned dstWidth{};
    unsigned dstHeight{};
    int      lastError{}; // HRESULT of the last failed step (0 = none)
    int      colorChoice{};
    unsigned colorCandidates{};
    int      tonemap{};
    void*    colorResource{};
};

// Records the bypass into `commandList` and writes it over `backBuffer`.
// Both are void* so this header stays free of d3d12.h; the implementation casts.
[[nodiscard]] bool record(void* commandList, void* backBuffer);

[[nodiscard]] Stats stats();

// Diagnostics for the tone question (2026-10-25): the render-size fmt26 group has
// several candidates; cycle them, and cycle the tone curve, until the bypass
// matches the engine's picture. The next record resolves the new choice.
void cycleColor();
void cycleTonemap();

// Releases pipelines/resources. Safe when nothing was created.
void release();

} // namespace neurodragon::vv::bypass
