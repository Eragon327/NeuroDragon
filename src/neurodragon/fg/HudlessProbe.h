#pragma once

namespace neurodragon::fg::hudless {

/// F0-b diagnostic (TEST only): copy the current back buffer at a list boundary
/// between the engine's command lists, then write it to a BMP after the GPU finishes.
/// The boundary is relative to the submission tail (count-6, count-3, count-2,
/// rotating), so the three candidates for "after the display write / before UI" are
/// captured in one session. Nothing is captured outside a world.
void install();
void uninstall();

/// Called from the overlay's ExecuteCommandLists hook after the VV injection declined.
/// Returns the 1-based split if a copy list was recorded, 0 = forward unchanged.
unsigned prepare(void* queue, unsigned listCount, void* const* lists);
[[nodiscard]] void* list();
void                 submitted();

/// Called from the overlay's Present hook: maps the readback buffer once the copy's
/// fence has completed and writes the BMP.
void poll();

} // namespace neurodragon::fg::hudless
