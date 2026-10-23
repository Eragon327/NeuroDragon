#pragma once

namespace neurodragon::ui::menu {

// Draw one frame of the panel. Called from the overlay's drawFrame(), after
// ImGui::NewFrame() and after theme::apply().
void render();

// Lifecycle hooks from the overlay: opening takes a fresh disk read (so the panel
// starts from what is actually saved), closing writes changes back.
void onOpened();
void onClosed();

} // namespace neurodragon::ui::menu
