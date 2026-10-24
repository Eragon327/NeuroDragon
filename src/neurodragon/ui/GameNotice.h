#pragma once

#include <string>

// Messages the player is meant to read.
//
// The overlay's own toast (Overlay::notify) is a single small line at the bottom
// of the screen that lives for 2.2 s - in practice a screenshot taken with F2 and
// one that failed looked the same, and the key got a reputation for doing nothing.
// The game's own chat is the readable channel: native HUD size, about ten seconds
// on screen, and still there to scroll back to afterwards.
namespace neurodragon::ui::notice {

// Appends `message` to the local chat as a client-side system line.
//
// The line never goes to the server: no packet is sent and no chat filter sees it,
// so it behaves identically in single player and on any server. The text is
// already localised by the caller ("key"_tr, docs/README.md 代码约定).
//
// Safe to call from any thread - the message is handed to the client thread,
// because GuiData belongs to the UI. Without a client instance (no world yet, or
// shutting down) it falls back to the overlay toast instead of dropping the line.
void chat(std::string message);

} // namespace neurodragon::ui::notice
