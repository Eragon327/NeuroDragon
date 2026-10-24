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

} // namespace neurodragon::probe::vv
