#pragma once

namespace neurodragon::fg::inputs {

/// F2 (EXP-0150): mark Depth / MotionVectors / HUDLessColor for one frame, with
/// DLSS-G kept at `eOff`. Nothing is generated here and nothing is presented
/// differently: the only change to the frame is that Streamline is told what the
/// frame's inputs are, once, immediately before the original Present.
///
/// Frame identity: the token is taken once per DXGI Present, with the Present
/// sequence number as its frame index, so "token" and "presented frame" are the same
/// thing by construction. It is deliberately NOT taken per ExecuteCommandLists call
/// (the engine submits several times per frame; a token each would trade frames).
void install();
void uninstall();

/// Called once per frame from the engine's `$flip` entry (fg/Flip.cpp), *before* the
/// original flip - i.e. before Streamline's proxy Present, which is the boundary DLSS-G
/// wants for constants and tags. The overlay's Present hook is inside that proxy and is
/// therefore too late; it no longer calls this.
///
/// Acquires the frame token, refreshes the cached DLSS-G input fence, reads the chain's
/// current back-buffer index (DLSS-G requires that call), and submits constants + the
/// three tags when this frame's inputs are fresh. Every early return leaves the original
/// flip untouched.
void presentFrame(void* swapChain);

/// Called from the overlay's ExecuteCommandLists hook when the submission is about to
/// be cut and the SR prep pass will rewrite the shared Depth/MV textures. A no-op when
/// DLSS-G never reported a fence or its value is 0.
void awaitInputsFence(void* queue);

/// resize / disable: drop the cached fence and any pending input *before* the
/// resources they name are released.
void invalidate(char const* why);

} // namespace neurodragon::fg::inputs
