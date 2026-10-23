#pragma once

#include <imgui.h>

namespace neurodragon::ui {

// Panel geometry, all in "logical" units for a 1080p viewport, then multiplied by
// `scale`. Anything code-drawn (box widths, graph sizes) must use theme::scale():
// ImGui's ScaleAllSizes only covers the style values.
struct UiMetrics {
    ImVec2 viewport{};
    float  scale{1.0f};
    float  gap{};
    float  pad{};
    float  rounding{};
};

[[nodiscard]] UiMetrics calculateMetrics(ImVec2 viewport, float uiScale);

} // namespace neurodragon::ui

namespace neurodragon::ui::theme {

// Palette lifted from the reference panel (Theo's Render Pipeline): near-black
// neutral panels, one warm orange accent, everything else grey.
constexpr ImU32 kAccent      = IM_COL32(240, 163, 60, 255);
constexpr ImU32 kAccentSoft  = IM_COL32(240, 163, 60, 70);
constexpr ImU32 kAccentText  = IM_COL32(28, 24, 18, 255); // text drawn on the accent
constexpr ImU32 kPanel       = IM_COL32(33, 34, 36, 250);
constexpr ImU32 kPanelDeep   = IM_COL32(24, 25, 27, 252);
constexpr ImU32 kPanelRaised = IM_COL32(42, 43, 46, 255);
constexpr ImU32 kFrame       = IM_COL32(46, 47, 50, 255);
constexpr ImU32 kFrameHover  = IM_COL32(56, 57, 61, 255);
constexpr ImU32 kBorder      = IM_COL32(60, 61, 64, 255);
constexpr ImU32 kBorderSoft  = IM_COL32(48, 49, 52, 255);
constexpr ImU32 kText        = IM_COL32(233, 233, 233, 255);
constexpr ImU32 kTextDim     = IM_COL32(152, 153, 156, 255);
constexpr ImU32 kTextFaint   = IM_COL32(110, 111, 114, 255);
constexpr ImU32 kGreen       = IM_COL32(95, 198, 110, 255);
constexpr ImU32 kAmber       = IM_COL32(224, 163, 60, 255);
constexpr ImU32 kRed         = IM_COL32(217, 96, 96, 255);

// ImGui's built-in font has no CJK glyphs, so the body font is Microsoft YaHei
// rasterised at 2x and drawn at half size (crisper edges, same trick Mangrove uses).
void loadFonts();

// Apply palette + sizes. No-op when neither the viewport nor the scale changed.
void apply(UiMetrics const& metrics);
void apply(ImVec2 viewport);

// Height-based scale of the current viewport (1.0 at 1080p).
[[nodiscard]] float scale();

// User multiplier from the panel's zoom buttons (0.75 .. 1.75).
[[nodiscard]] float userScale();
void                setUserScale(float value);

// Larger face for the panel title / tab labels; may be null when no system font
// could be loaded, so callers must fall back to the default font.
[[nodiscard]] ImFont* titleFont();

// Drop the cached base style (call before destroying the ImGui context).
void reset();

} // namespace neurodragon::ui::theme
