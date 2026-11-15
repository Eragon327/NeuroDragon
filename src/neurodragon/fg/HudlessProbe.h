#pragma once

namespace neurodragon::fg::hudless {

/// F2 (EXP-0150): the frame's pre-UI LDR image, copied into a texture we own every
/// world frame. This replaces the F0-b BMP probe: no readback, no mapping, no BMP, and
/// no reference to a swap-chain back buffer held past the submission that records the
/// copy (a held back buffer makes the engine's ResizeBuffers fail, EXP-0139).
///
/// The copy is recorded as its own command list and submitted between two of the
/// engine's lists, so no engine list is modified. The cut comes from the submission
/// itself (probe::vv::preUiSplit()); when the shape is not recognised nothing is
/// recorded and the frame simply has no hudless input.
void install();
void uninstall();

/// Called from the overlay's ExecuteCommandLists hook, after the submission has been
/// analysed. Returns the number of lists that must be forwarded before the copy list
/// (1-based), 0 = forward the submission unchanged.
unsigned prepare(void* queue, unsigned listCount, void* const* lists);
[[nodiscard]] void* list();
void submitted();

/// The copy of the most recent frame. `serial` changes once per recorded copy, so a
/// reader can tell "this frame" from "the previous one"; `state` is the texture's
/// current D3D12_RESOURCE_STATES (ALL_SHADER_RESOURCE once a copy has completed).
struct Frame {
    void*    texture{};
    unsigned serial{};
    unsigned width{};
    unsigned height{};
    unsigned format{};
    unsigned state{};
};

[[nodiscard]] bool frame(Frame* out);

/// resize/disable: make the published copy unusable *before* anything it refers to is
/// released. The next prepare() re-allocates the target at the new size.
void invalidate(char const* why);

} // namespace neurodragon::fg::hudless
