#pragma once

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
// Nothing here writes to the engine, and no resource is read as pixels: the
// detours copy a few POD fields out of the argument block and forward.
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

} // namespace neurodragon::probe::vv
