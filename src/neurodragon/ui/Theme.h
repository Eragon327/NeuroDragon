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

// Neutral blue-grey surfaces; amber is reserved for selection, edits and actions.
constexpr ImU32 kAccent      = IM_COL32(240, 163, 60, 255);
constexpr ImU32 kAccentSoft  = IM_COL32(240, 163, 60, 70);
constexpr ImU32 kAccentText  = IM_COL32(28, 24, 18, 255);
constexpr ImU32 kPanel       = IM_COL32(28, 30, 34, 250);
constexpr ImU32 kPanelDeep   = IM_COL32(22, 24, 28, 252);
constexpr ImU32 kPanelRaised = IM_COL32(36, 39, 45, 255);
constexpr ImU32 kFrame       = IM_COL32(43, 47, 54, 255);
constexpr ImU32 kFrameHover  = IM_COL32(54, 59, 67, 255);
constexpr ImU32 kBorder      = IM_COL32(62, 68, 78, 255);
constexpr ImU32 kBorderSoft  = IM_COL32(43, 47, 54, 255);
constexpr ImU32 kText        = IM_COL32(235, 237, 240, 255);
constexpr ImU32 kTextDim     = IM_COL32(164, 169, 177, 255);
constexpr ImU32 kTextFaint   = IM_COL32(120, 126, 136, 255);
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

// Larger title and medium section faces; either may be null when the system font
// is unavailable, so callers must fall back to the default font.
[[nodiscard]] ImFont* titleFont();
[[nodiscard]] ImFont* sectionFont();

// Drop the cached base style (call before destroying the ImGui context).
void reset();

} // namespace neurodragon::ui::theme
