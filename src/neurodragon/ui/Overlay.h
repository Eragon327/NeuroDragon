#pragma once

#include <cstdint>
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
    // Hotkey
    // -----------------------------------------------------------------------

    // The key that opens/closes the panel. Comes from config.json (ui.openKey),
    // imported at load; the panel's Advanced tab stages a new one and calls
    // refreshHotkey() when it is applied.
    [[nodiscard]] int hotkey() const;
    void              refreshHotkey();

    // Screenshot key (config ui.screenshotKey, F2 by default, rebindable in the
    // panel's Advanced tab) and where the PNGs go (<version>/screenshots, the game
    // root). The capture runs at the next Present, before the panel is drawn, so the
    // file holds the finished frame without the overlay.
    [[nodiscard]] int  screenshotKey() const;
    void               setScreenshotDirectory(std::wstring directory);

    // One-shot rebinding: the next key press (Esc cancels) becomes the new key. The
    // capture is global (one key at a time); the panel decides which field the
    // result lands in, so the same mechanism serves every rebindable key.
    void               beginHotkeyCapture();
    void               cancelHotkeyCapture();
    [[nodiscard]] bool isCapturingHotkey() const;
    // Returns the captured code once, then 0.
    int takeCapturedHotkey();

    [[nodiscard]] FrameStats         frameStats() const;
    [[nodiscard]] std::vector<float> recentFrameMs() const;

private:
    Overlay() = default;
};

} // namespace neurodragon::ui
