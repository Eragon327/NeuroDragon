#pragma once

#include <string>

struct ID3D11Device;
struct ID3D11DeviceContext;
struct ID3D11Texture2D;

// "Finished frame" screenshot for the SR/NR work (docs/K §2.2 item ②, TRP 0.5.3
// replaces Skyrim's own screenshot with the upscaled frame; Bedrock has no such
// function to hook, so this is our own capture instead).
//
// It runs inside the Present hook *before* the panel is drawn, so the file is
// exactly what the screen shows - post-DLSS, post-NR, post-HUD, without the
// overlay - at the output resolution. That makes it the A/B evidence tool for
// sharpening/scale changes as much as a user feature.
namespace neurodragon::ui::screenshot {

// Copies the frame in `buffer` into a PNG under `directory` and returns the path
// written (empty on failure). Present thread only; the GPU readback is
// synchronous, so one capture stalls that frame (the cost is logged).
[[nodiscard]] std::wstring save(
    ID3D11Device*          device,
    ID3D11DeviceContext*   context,
    ID3D11Texture2D*       buffer,
    std::wstring const&    directory
);

// Drops the cached staging texture (called on overlay uninstall).
void releaseCache();

} // namespace neurodragon::ui::screenshot
