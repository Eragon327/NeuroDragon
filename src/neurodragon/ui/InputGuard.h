#pragma once

namespace neurodragon::ui {

// The panel is drawn on top of the game but the game keeps running underneath.
// Two independent paths feed it input - the window procedure (what our overlay
// subclass sees first) and the engine's own HID controllers - so blocking just one
// of them leaves the player walking around while dragging a slider. These hooks
// block the second path; the window procedure blocking is done in Overlay.cpp.
//
// Proven in Mangrove (src/mangrove/ui/InputGuard.cpp), same hooks, same idea.

struct InputGuardStatus {
    bool mouseHookInstalled{};
    bool keyDownHookInstalled{};
    bool keyUpHookInstalled{};
};

// True while the panel owns the keyboard/mouse, plus a short tail after closing so
// the release that closed it never reaches the game.
[[nodiscard]] bool overlayOwnsGameInput();

// Turn the takeover on/off. Called from Overlay::setVisible.
void setOverlayInputCapture(bool capture);

[[nodiscard]] InputGuardStatus installInputGuard();
void                       uninstallInputGuard();

// Re-injecting synthetic events (the "release everything the game thinks is held"
// pass) must not be eaten by our own hooks. Instantiate this in that scope.
class InputHandoffScope {
public:
    InputHandoffScope();
    ~InputHandoffScope();
    InputHandoffScope(InputHandoffScope const&)            = delete;
    InputHandoffScope& operator=(InputHandoffScope const&) = delete;
};

} // namespace neurodragon::ui
