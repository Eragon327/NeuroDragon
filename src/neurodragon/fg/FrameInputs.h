#pragma once

namespace neurodragon::fg::inputs {

/// F2/F3: mark Depth / MotionVectors for one frame and let DLSS-G intercept the final
/// backbuffer for colour. F3 switches DLSS-G and Reflex only after a successful tag,
/// never on an invalid frame and never repeatedly.
/// setEnabled changes the request for the next flip; it does not alter this frame's tags.
void setEnabled(bool enabled);
[[nodiscard]] bool enabled();

/// True only while the current flip's constants/tag succeeded and FG was requested.
/// The proxy Present hook uses this to select generation (interval 0) or pass-through.
[[nodiscard]] bool frameReady();
///
/// Frame identity: one token per DXGI Present, with SL owning the frame index (EXP-0189).
/// It is deliberately NOT taken per ExecuteCommandLists call (the engine submits several
/// times per frame; a token each would trade frames).
void install();
void uninstall();

/// Called once per frame from the engine's `$flip` entry (fg/Flip.cpp), *before* the
/// original flip - i.e. before Streamline's proxy Present, which is the boundary DLSS-G
/// wants for constants and tags. The overlay's Present hook is inside that proxy and is
/// therefore too late; it no longer calls this.
///
/// Acquires the frame token, refreshes the cached DLSS-G input fence, reads the chain's
/// current back-buffer index (DLSS-G requires that call), and submits constants + the
/// Depth/MV tags when this frame's inputs are fresh. Every early return leaves the
/// original flip untouched.
void presentFrame(void* swapChain);

/// EXP-0190: bracket the engine's flip (and therefore Streamline's proxy Present)
/// with the PCL present markers. DLSS-G matches the common constants to the
/// presented frame by the frame index carried in these markers; without them the
/// plugin reports the constants as missing. Uses the token acquired by the same
/// flip's presentFrame; a no-op when PCL or the token is unavailable.
void presentMarker(bool start);

/// EXP-0192: call Reflex sleep once per presented frame after Present returns. No-op
/// unless FG is active and a token exists.
void reflexSleep();

/// Called from the overlay's ExecuteCommandLists hook when the submission is about to
/// be cut and the SR prep pass will rewrite the shared Depth/MV textures. A no-op when
/// DLSS-G never reported a fence or its value is 0.
void awaitInputsFence(void* queue);

/// resize / disable: drop the cached fence and any pending input *before* the
/// resources they name are released.
void invalidate(char const* why);

} // namespace neurodragon::fg::inputs
