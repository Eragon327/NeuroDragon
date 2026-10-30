#pragma once

namespace neurodragon::ui::menu {

// Draw one frame of the panel. Called from the overlay's drawFrame(), after
// ImGui::NewFrame() and after theme::apply().
void render();

// Opening stages the current running values; closing discards unapplied edits.
void onOpened();
void onClosed();

} // namespace neurodragon::ui::menu
