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
    bool     dlssReady{};       ///< the NGX feature exists for the current sizes
    int      dlssCreateRet{};
    int      dlssEvaluateRet{};
};

/// R3 stage B: prep + DLSS evaluate, then a PS writes the DLSS output into the
/// engine's arg10 with an RTV (no tonemap, no encode, no back-buffer copy).
/// The resource is left in RENDER_TARGET; a missing DLSS evaluation fails instead
/// of falling back to anything.
[[nodiscard]] bool recordArg10(void* commandList, void* arg10);

/// Clears the DLSS temporal history over the next recorded frames. The panel's "reset"
/// button uses it; the pipeline also resets by itself after a world or size change.
void requestReset();

// ---------------------------------------------------------------------------
// The product switch (docs/specs/ui.md §3)
// ---------------------------------------------------------------------------
// On: our DLSS writes arg10 and the engine's TAAU draw is skipped (the draw-level
// suppress, EXP-0067 - not the callback-level one, which also removed the material
// binds the MV channel needs). Off: the engine's own TAAU, nothing of ours runs.
//
// The sequence is kept here, in one place, because it is four switches that only mean
// something together.
void setEnabled(bool enabled);
[[nodiscard]] bool enabled();

/// Reads `sr.vvEnabled` once per present and applies a change. Called from
/// probe::vv::onPresent(); a no-op while the config value has not changed.
void syncConfig();

[[nodiscard]] Stats stats();

// Feature-level fallback ladder (docs/specs/vv.md §38). The RT path degrades
// runtime -> core -> after -> before -> percent; VV has a single feature, so the
// only meaningful reductions are the optional input (NR), the extra pass (RCAS),
// and finally giving the frame back to TAAU. Rungs are cumulative.
enum class Rung : int { Full = 0, NoNr = 1, NoSharp = 2, EngineTaau = 3 };
enum class RungReason : int { None = 0, Create = 1, Eval = 2, Retry = 3 };
#ifdef TEST
enum class Fault : int { None = 0, Create = 1, Eval = 2 }; // session-only, TEST panel
#endif

struct Fallback {
    Rung       rung{};
    RungReason reason{};
    unsigned   createFails{}; ///< consecutive creation failures
    unsigned   evalFails{};   ///< consecutive evaluation failures
    unsigned   sinceFrame{};  ///< sr frame the current rung was entered
};

[[nodiscard]] Fallback fallback();
[[nodiscard]] char const* rungName(Rung rung) noexcept;
[[nodiscard]] char const* rungReasonName(RungReason reason) noexcept;

/// TEST: pretend CreateFeature / EvaluateFeature failed, to walk the ladder on
/// demand instead of waiting for a real failure. Session-only, not a config key;
/// the frame is skipped exactly as a real failure would skip it.
#ifdef TEST
void setFault(Fault fault);
[[nodiscard]] Fault fault();
#endif

/// True after a frame successfully wrote arg10, and latched until disable/release.
/// The direct-factor hook defers the native 100% keepalive until this is true, then
/// keeps the render size stable while the engine rebinds its native-size motion vectors.
[[nodiscard]] bool live();

/// What Jitter.Offset DLSS is told. Zero by default (EXP-0098: the engine value makes
/// the picture wobble, zero is stable, and the value is already in DLSS's space -
/// docs/research/2026-10-29-taau-jitter-dlss-contract.md).
///
/// The other three are that document's §7 discrimination, and the panel offers all
/// four so the A/B can be re-run without a rebuild:
///   Engine   j(n)     - as delivered by the TAA context (the original hand-off)
///   Negated  -j(n)    - amplitude doubles => the frame really carries this jitter and
///                       the value/sign is right; unchanged => the frame is not rendered
///                       with it and only 0 is truthful
///   Previous j(n-1)   - stable => the context value runs one frame ahead of the render
///
/// Only applied inside a world (outside it the panorama is not rendered with the world
/// jitter, EXP-0073); a value outside this enum is folded to Zero on the way in.
enum class JitterSource : int { Zero = 0, Engine = 1, Negated = 2, Previous = 3 };

void setJitterSource(JitterSource source);
[[nodiscard]] JitterSource jitterSource();

void release();

} // namespace neurodragon::vv::sr
