#pragma once

#include <cstdint>

namespace neurodragon::probe::vv {

// ---------------------------------------------------------------------------
// Vibrant Visuals reconnaissance (read-only)
// ---------------------------------------------------------------------------
//
// Vibrant Visuals (graphics_mode: deferred, no ray tracing) is the path whose
// upscaler is `dragon::framerenderer::modules::taau`: one framegraph pass that
// reads the render-resolution colour + motion vectors + last frame's history and
// writes the display-resolution image that bloom / tone mapping / UI then use
// (docs/A §2, §3; docs/I §5 M4).
//
// Making DLSS NR + SR replace it needs three things this probe starts to answer
// without changing a single pixel:
//
//   1. is the pass alive at all (it is skipped entirely when upscaling_mode is
//      Bilinear - docs/A §7.4), and what numbers does it run with
//      (viewport / mode / factor / dynamic-resolution scale / jitter index);
//   2. what the four framebuffer dependencies look like, because they are the
//      only handle to the colour, motion-vector, history and output textures;
//   3. how many command lists the game submits per presented frame, which decides
//      whether work can be injected between two passes at all.
//
// By default the detours only copy POD fields and forward. The explicitly armed
// DLSS replacement path suppresses just the original TAAU draw callback; all other
// engine callbacks and command lists continue unchanged.
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

// Called from the overlay's Present detours: one call per presented frame. Used
// only to turn the counters above into "per frame" numbers.
void onPresent();

// Called when the game reports that a world was joined (PlayerJoinEvent). One semantic
// dump is taken a few seconds later, which is the only point where the render-resolution
// textures hold a real rendered world instead of the main-menu panorama.
void noteWorldEntered();

// Called once from the overlay's present hook: the swap chain is the only way to
// learn which render targets are back buffers, and those must not be transitioned by
// the BMP dump (they live in PRESENT, not COMMON).
void noteSwapChain(void* swapChain);

// Called from the overlay's ResizeBuffers / ResizeBuffers1 hooks *before* the
// original runs. Releases the probe's references to the old render graph: a live
// reference to a swap-chain back buffer (or the whole old graph) makes
// ResizeBuffers fail, and the engine answers that with abort() - the 2026-10-25
// 03:14 F11 crash. Safe to call when nothing is recorded.
void onSwapChainResize();

// The game's D3D12 direct queue, captured by the overlay's ExecuteCommandLists hook.
// Only used by the one-shot BMP dump.
void setGameQueue(void* queue);

// B0 diagnostic triggers (docs/specs/vv.md §13.3). TEST builds act on them; a
// release build compiles the calls out, so the panel can stay wired either way.
// `frames` / `delayMs` describe a context sample that starts after the delay -
// the player presses once, then walks during the wait.
void requestContextSample(unsigned frames, unsigned delayMs);

// Runs one render-resolution dump synchronously (the same path F9 uses).
void requestDumpNow();

// Frame facts for the B1a NGX-session probe. `engineDevice`/`gameQueue` are the
// objects the renderer submits through, latched by the existing hooks; the sizes
// are the render resolution (viewport x factor) and the display viewport.
[[nodiscard]] void*    engineDevice();
[[nodiscard]] void*    gameQueue();
// Latest native resource resolved from InputFinalColor's bgfx handle. Borrowed pointer
// for the explicit SR dump; null until the replacement path has sampled a declarator.
[[nodiscard]] void*    inputFinalColorResource();
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

// B1b: the render-resolution textures the RTV inventory is holding references to,
// newest first. Only resources whose size matches the current render resolution
// are returned. `format` is the DXGI_FORMAT number (26 = R11G11B10, 44 = R24G8,
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
// world enter). The B1b bypass caches the selected colour texture and uses this
// to drop the cache instead of holding a stale generation across a rebuild.
[[nodiscard]] unsigned renderGeneration();

// Content boundary (B1b, 2026-10-25): the engine command list that was recording
// when the TAAU draw callback ran, if the draw-side scanner could prove it from
// an engine-list vtable match. Valid only for the current frame; null when the
// scan found nothing. This is what replaces "tail k=4" once it works.
[[nodiscard]] void* taauListForFrame();

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
    std::uint64_t declared{};
    std::uint64_t callbacks{};
    std::uint64_t forwarded{};
    std::uint64_t suppressed{};
    /// Times the gate was armed, the engine's TAAU draw was suppressed, and the frame
    /// that should have come with it never injected (no boundary, no back buffer,
    /// record failure). Those frames are restored on the next one; a non-zero, growing
    /// number means suppression is outrunning the replacement instead of replacing it.
    std::uint64_t missed{};
};

// Arms replacement only after the injector has produced a valid DLSS frame and a
// pre-HUD boundary. While armed, drawDetour still snapshots the context but does not
// call the engine's draw callback. Disarming restores the original path directly.
void setTaauDrawReplacement(bool armed);
[[nodiscard]] bool taauDrawReplacementArmed();
[[nodiscard]] TaauDrawStats taauDrawStats();

// Called by the injector only when this submission *is* the engine's frame and the
// armed gate could not be honoured (so it is about to be disarmed again). The overlay's
// own small submissions and every non-engine queue must never call this: they do not
// own the frame the suppression applies to.
void noteTaauReplacementMiss();

// TEST only: dumps the 0x300-byte float windows of the TAAU draw's context, the
// execution context and the camera object, so the view-projection packing can be
// settled offline from one labelled in-world frame (0x1e0..0x2a0 covers jitter,
// the current/previous pair and the camera origins).
void dumpTaaContextWindows();

// Asks for a deeper scan of the draw callback's object graph on the next frame
// (up to three pointer hops, bounded). Only runs on request: the first shallow
// scan (2026-10-25) found nothing in the execution context's first level.
void requestTaauListScan();

// Content boundary (B1d): the index of the list in the current frame after which
// the world composite has just finished, i.e. the first list that bound a back
// buffer and drew (1-based; 0 when no such list was seen). This is the anchor that
// does not move with the frame's list count - see docs/specs/vv.md §14.5. The value
// describes the submission that just arrived, so it must be read from the same
// ExecuteCommandLists hook that computes it (inject::prepare does).
[[nodiscard]] unsigned backBufferWriterSplit();
[[nodiscard]] void*    backBufferWriterList();

} // namespace neurodragon::probe::vv
