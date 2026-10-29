#pragma once

#include <cstdint>

namespace neurodragon::probe::vv {

// ---------------------------------------------------------------------------
// Vibrant Visuals: TAAU observation and replacement
// ---------------------------------------------------------------------------
//
// Vibrant Visuals (graphics_mode: deferred, no ray tracing) is the path whose
// upscaler is `dragon::framerenderer::modules::taau`: one framegraph pass that
// reads the render-resolution colour + motion vectors + last frame's history and
// writes the display-resolution image that bloom / tone mapping / UI then use
// (docs/A §2, §3; docs/I §5 M4).
//
// Making DLSS NR + SR replace it needs three things this probe provides:
//
//   1. is the pass alive at all (it is skipped entirely when upscaling_mode is
//      Bilinear - docs/A §7.4), and what numbers does it run with
//      (viewport / mode / factor / jitter);
//   2. which resources the pass reads and writes, because they are the only handle
//      to our input colour, motion vectors and to TAAU's output (arg10);
//   3. how many command lists the game submits per presented frame, which decides
//      where our work is injected between two passes.
//
// The detours only copy POD fields and forward. The explicitly armed DLSS
// replacement path suppresses just the original TAAU draw; all other engine
// callbacks and command lists continue unchanged.
//
// The two addresses are file offsets established offline from the shipped
// executable (no PDB): the strings "InputFinalColor" / "InputBufferMotionVectors"
// / "InputTAAHistory" have exactly one RIP-relative reference each and both land
// in the same .pdata function. See docs/O-vv-path.md §2 for the tool.
bool install();

void uninstall();

[[nodiscard]] bool isInstalled();

// Called from the overlay's ExecuteCommandLists detour (ui/Overlay.cpp): one call
// per submission, with the list array and its size.
void onExecuteCommandLists(void* const* lists, unsigned listCount);

// Called from the overlay's Present detours: one call per presented frame. Applies
// the product switch (vv::sr::syncConfig) and keeps the per-frame counters.
void onPresent();

// Called when the game reports that a world was joined (PlayerJoinEvent). Bumps the
// render generation so the DLSS history restarts across a level change.
void noteWorldEntered();

// Called once from the overlay's present hook: the swap chain is the only way to
// learn the display size and which render targets are back buffers.
void noteSwapChain(void* swapChain);

// Called from the overlay's ResizeBuffers / ResizeBuffers1 hooks *before* the
// original runs. Releases the probe's references to the old render graph: a live
// reference to a swap-chain back buffer (or the whole old graph) makes
// ResizeBuffers fail, and the engine answers that with abort() - the 2026-10-25
// 03:14 F11 crash. Safe to call when nothing is recorded.
void onSwapChainResize();

// The game's D3D12 direct queue, captured by the overlay's ExecuteCommandLists hook.
void setGameQueue(void* queue);

// Frame facts for the SR pipeline. `engineDevice`/`gameQueue` are the objects the
// renderer submits through, latched by the existing hooks; the sizes are the render
// resolution (viewport x factor) and the display viewport.
[[nodiscard]] void*    engineDevice();
[[nodiscard]] void*    gameQueue();
[[nodiscard]] unsigned renderWidth();
[[nodiscard]] unsigned renderHeight();
[[nodiscard]] unsigned displayWidth();
[[nodiscard]] unsigned displayHeight();

// TAAU-less render size (upscaling_mode Bilinear): the newest fmt=26 render target
// that is smaller than the display and matches its aspect ratio. False when the
// inventory holds none (no F11 rebuild yet, or the graph is display-sized).
bool renderSizeHint(unsigned displayWidth, unsigned displayHeight, unsigned* width, unsigned* height);

// Store a derived size as the render size once; a no-op when taauDetour already set
// one. onSwapChainResize() drops it again so the next record re-derives.
void latchRenderSize(unsigned width, unsigned height);

// The render-resolution textures the RTV inventory is holding references to, newest
// first. Only resources whose size matches the current render resolution are
// returned. `format` is the DXGI_FORMAT number (26 = R11G11B10, 44 = R24G8,
// 41 = R32F, ...). The references are owned by the probe inventory, so the caller
// must not release them.
struct RenderTargetInfo {
    void*    resource;
    unsigned width;
    unsigned height;
    unsigned format;
};
[[nodiscard]] RenderTargetInfo inputFinalColorTarget();
[[nodiscard]] unsigned renderTargets(RenderTargetInfo* out, unsigned max, unsigned tolerance = 2);

// The same shape for depth-stencil targets (they are created through
// CreateDepthStencilView, so renderTargets() never holds them). `format` is the
// resource's own DXGI format - 43 (R24G8_TYPELESS) or 44 in practice. Newest first,
// references owned by the probe.
[[nodiscard]] unsigned depthTargets(RenderTargetInfo* out, unsigned max, unsigned tolerance = 2);

// Bumped whenever the render graph may have been replaced (swap-chain resize,
// world enter). The SR record uses it to reset the DLSS history instead of
// reprojecting across a rebuild.
[[nodiscard]] unsigned renderGeneration();

// Whether a level is loaded, maintained by LeviLamina's client join/exit events
// (the same pair the RT path uses). `ll::service::getLevel()` was measured useless
// here: non-empty in the first world, still empty after leaving and re-entering
// (2026-10-25 23:36). Event-driven, so it survives world changes.
[[nodiscard]] bool worldLoaded();

// The per-frame numbers the motion-vector pass needs: current/previous
// view-projection (16 floats each), camera origins (6 floats at +0x280) and sub-pixel
// jitter (2 floats at +0x1e4). Read from the snapshot taken inside the TAAU draw
// callback. The callback only reads this context; the engine advances the pair upstream.
// Returns false when no draw has been seen yet. Read-only.
bool taaMatrices(float* current, float* previous, float* camera, float* jitter);

struct TaauDrawStats {
    std::uint64_t declared{};  ///< the engine declared the pass (once per frame)
    std::uint64_t callbacks{}; ///< the pass' draw callback ran
    std::uint64_t forwarded{}; ///< the original draw callback was allowed through
};

[[nodiscard]] TaauDrawStats taauDrawStats();

// The product switch, and the only one: "our DLSS writes arg10 and the engine's TAAU
// draw is skipped". Everything that drives the product reads this flag, and every
// path that stops injecting clears it (see inject::setMode(Off)), so the draw gate
// can never stay armed over a picture nothing is writing.
//
// Alongside it, `arg10Target`/`arg10WriterSplit` describe where the current frame's
// arg10 is (1-based; 0 when the writer list was not identified). onExecuteCommandLists
// refreshes those, so inject::prepare reads them from the submission it is about to
// split. Arming forces one fresh RTV capture per list; it is off by default and does
// not change any engine state by itself.
[[nodiscard]] void*    arg10Target();
[[nodiscard]] unsigned arg10WriterSplit();
[[nodiscard]] void*    arg10WriterList();
void                   setProductArmed(bool armed);
[[nodiscard]] bool     productArmed();

// True when the submission just observed followed a new arg10 declaration (i.e. it
// is the world frame, not a UI submission reusing the previous native).
[[nodiscard]] bool arg10FreshSubmission();
// A world submission missed its writer: the next injected frame must reset the DLSS
// history rather than reproject across the gap.
void noteArg10Gap();
[[nodiscard]] bool takeArg10Gap();

// R3 stage C (docs/specs/vv.md §35.3): the engine's own motion-vector texture, arg11
// (InputBufferMotionVectors). Captured where the engine actually resolves it: inside the
// TAAU draw's material binding (SetTextureHook, role = render-size non-fmt26 target), not
// from the declarator - the ExternalImage wrapper has no holder chain there (EXP-0062).
// Null until a forwarded draw bound it. The caller reads the resource's own desc,
// because the shader binds `.zw` per the material and the format decides the SRV family.
[[nodiscard]] void* motionVectorResource();

// R3 stage D draw-level suppress (docs/specs/vv.md §35.4): skip only the TAAU quad
// (first DrawIndexedInstanced(6,1) of the list that already bound this frame's arg10),
// keeping the callback, the material binds (MV role channel) and the arg10 writer
// binding.
//
// The invariant that keeps it from freezing the picture: TAAU is skipped only while
// `productArmed()` and the *previous engine submission* really wrote arg10. That
// latch is written exactly once per engine submission by inject::prepare - including
// the paths that decide not to inject - so "nobody is writing arg10" always reaches
// the gate as a plain false, instead of leaving the last true in place (the 2026-10-29
// 14:40 freeze: injection had stopped, the draw kept being skipped).
void               setDrawSuppress(bool armed);
[[nodiscard]] bool drawSuppressArmed();
[[nodiscard]] unsigned drawSuppressSkips();
void               noteInjectionResult(bool ok);

} // namespace neurodragon::probe::vv
