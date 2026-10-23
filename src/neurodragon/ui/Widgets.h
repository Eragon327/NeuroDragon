#pragma once

#include <imgui.h>

#include <string>
#include <string_view>
#include <vector>

namespace neurodragon::ui::widgets {

// Row layout: label on the left, control right-aligned inside the same row. The
// row's right edge is captured by beginRow() so callers do not have to repeat the
// available-width maths.
void                beginRow();
void                labelRow(std::string_view label);
[[nodiscard]] float rowRight();

// A section title with a rule, like the reference panel's group headers.
void sectionHeader(char const* label);

// "label ......... value" - the stats-column primitive.
void statRow(char const* label, char const* value, ImU32 valueColor = 0);

// Small dim note (key hints, units).
void hint(char const* text);

// Orange check box + label; the whole row is the hit target.
bool toggle(char const* label, bool* value);

// Label + value on one line, then a full-width track with a small orange handle.
// Returns true on the frame the value changed.
bool slider(
    char const* label,
    float*      value,
    float       min,
    float       max,
    char const* format,
    char const* suffix  = nullptr,
    bool        enabled = true
);
bool sliderInt(char const* label, int* value, int min, int max, char const* suffix = nullptr);

// Label above a full-width dropdown.
bool combo(char const* label, int* current, char const* const* items, int count);

// Pipeline strip primitives, drawn at explicit screen positions so the menu can
// lay them out without fighting ImGui's cursor.
void nodeBox(ImVec2 pos, ImVec2 size, char const* title, char const* line1, char const* line2, bool active);
void nodeArrow(ImVec2 from, ImVec2 to, bool accent);

// Frame-time sparkline with a 0..maxMs scale.
void frameGraph(ImVec2 pos, ImVec2 size, std::vector<float> const& values, float maxMs);

// Footer action button: primary = filled orange, secondary = outlined.
bool footerButton(char const* label, ImVec2 size, bool primary);

// Text helper for the draw-list laid-out parts.
void                text(ImVec2 pos, ImU32 color, char const* value);
[[nodiscard]] float textWidth(char const* value);

} // namespace neurodragon::ui::widgets
