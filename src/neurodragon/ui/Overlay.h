#pragma once

#include "neurodragon/input/KeyBind.h"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace neurodragon::ui {

// Present-hook frame timing, published for the panel.
struct FrameStats {
    double        fps{};
    double        frameMs{};
    std::uint64_t frames{};
};

// The game's D3D12 direct queue, captured by the present hook's
// ExecuteCommandLists detour (the same queue D3D11On12 renders through). Null until
// the first submitted frame and again after uninstall(). NR reads it once per frame
// and uses it only to wait for the GPU before releasing objects a resolution change
// replaced.
[[nodiscard]] void* overlayGameQueue();

// ---------------------------------------------------------------------------
// Hotkeys
// ---------------------------------------------------------------------------
//
// All four hotkeys are combinations, not single keys (input::KeyBind), and a mouse
// button is just another key. They are matched in the window procedure because that
// is where this overlay already owns the input path: the panel is modal and blocks
// the engine's own input while it is open, so an event-bus based manager would go
// blind exactly when the panel hotkey still has to work.

enum class Hotkey {
    Panel,      // open/close the panel; works in both panel states
    NrToggle,   // NR on/off; only while the panel is closed
    NrReset,    // wipe NR + engine DLSS history once; only while the panel is closed
    Screenshot, // save the finished frame; works in both panel states
};

// Code default of @p hotkey, i.e. what "reset to default" restores.
[[nodiscard]] input::KeyBind defaultKeys(Hotkey hotkey);

// Parse one stored key array (config ui.openKeys / nrKeys / ...).
//   - empty              -> deliberately unbound; a real value, not "no record";
//   - illegal or garbage -> the code default, because a hand-edited config must not
//                           leave behind a binding that can never fire.
[[nodiscard]] input::KeyBind parseKeys(std::vector<int> const& keys, int fallback);

// Same for the panel hotkey, which is the only way into the panel (there is no
// command fallback): an empty value is repaired to the default instead of honoured,
// because an "unbound" panel key locks the player out of the UI.
[[nodiscard]] input::KeyBind parsePanelKeys(std::vector<int> const& keys);

// Top-layer ImGui overlay.
//
// A client mod cannot draw into Minecraft's own UI stack (ll::form / DDUI are
// server-driven), so the panel is drawn the way Mangrove and LHolo already do it:
//
//   - rendering: hook DXGI Present / Present1 / ResizeBuffers*, acquire the game's
//     D3D12 device + direct queue, create a D3D11On12 device and a D3D11 immediate
//     context, wrap the current back buffer, draw ImGui's DX11 backend into it;
//   - input: subclass the window procedure (the only layer that sees absolute
//     cursor messages) and block the engine's own HID hooks via ui::InputGuard.
//
// Everything below is deliberately the proven shape of those two projects: the
// swap-chain teardown on resize/F11, the 11on12 acquire/release pair, the
// "ReleaseWrappedResources then Flush" ordering and the cursor hand-back message
// passed to the window thread are all details that took them real debugging.
class Overlay {
public:
    static Overlay& getInstance();
    Overlay(Overlay const&)            = delete;
    Overlay& operator=(Overlay const&) = delete;

    // Hook DXGI, the game window and the engine input path. False when the game
    // window does not exist yet (safe to retry).
    bool install();

    void uninstall();

    [[nodiscard]] bool isInstalled() const;
    [[nodiscard]] bool isVisible() const;

    void setVisible(bool visible);
    void toggle();

    // Short transient line at the bottom of the screen. Only the panel's own
    // feedback uses it now (a discarded edit); player-facing notices - the
    // screenshot key - go through ui::notice::chat instead, because at 2.2 s and
    // one small line they were too easy to miss.
    void notify(std::string message);

    // -----------------------------------------------------------------------
    // Hotkeys
    // -----------------------------------------------------------------------

    // The key that opens/closes the panel (config ui.openKeys) and where the
    // screenshot PNGs go (<version>/screenshots, the game root). The screenshot runs
    // at the next Present, before the panel is drawn, so the file holds the finished
    // frame without the overlay.
    void setScreenshotDirectory(std::wstring directory);

    // Re-read the four bindings from config.json. The window thread reads a snapshot
    // per key event; the panel's Advanced tab calls this when a change is applied.
    void refreshKeys();

    // Rebinding capture. Modal: while it is active the window thread consumes every
    // key and mouse button, collects them in press order and commits once all of them
    // are released again. Esc commits an EMPTY combination (= unbind), it is not a
    // cancel; closing the panel drops the capture.
    void               beginKeyCapture();
    void               cancelKeyCapture();
    [[nodiscard]] bool isCapturingKey() const;

    // Keys of the combination being captured, in press order, for the live preview.
    // Empty while nothing is being captured.
    [[nodiscard]] std::vector<int> capturePreview() const;

    // Tri-state result, consumed once:
    //   - std::nullopt       nothing committed (still capturing, or cancelled);
    //   - engaged and empty  the player pressed Esc -> unbind that slot;
    //   - engaged, not empty the new combination.
    [[nodiscard]] std::optional<input::KeyBind> takeCapturedKeys();

    [[nodiscard]] FrameStats         frameStats() const;
    [[nodiscard]] std::vector<float> recentFrameMs() const;

private:
    Overlay() = default;
};

} // namespace neurodragon::ui
