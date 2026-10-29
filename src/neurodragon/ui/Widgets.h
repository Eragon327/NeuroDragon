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

// The panel's one action button. A zero in either axis of `size` means "auto":
// the width then fits the label plus the button's own padding, the height one line
// of text plus padding. `primary` is the filled accent button - at most one per
// screen - and everything else is the outlined secondary.
//
// The label doubles as the ImGui id, so a label that appears twice in a window
// needs a "##suffix". A button inside ImGui::BeginDisabled() keeps its outline and
// a readable label; it just never reports a press.
bool button(char const* label, ImVec2 size = ImVec2(0, 0), bool primary = false);

// Width button(label) takes with an automatic width. Rows that right-align a button
// need the number before the item exists.
[[nodiscard]] float buttonWidth(char const* label);

// Warning marker for the element submitted immediately before: an orange "!" on the
// same line, vertically centred on that element. Hovering it shows `text` in a
// bordered box of fixed width (280 logical px, scaled by theme::scale()) whose height
// follows the wrapped paragraph; the box sits beside the cursor, flips to the other
// side when it would not fit and stays on screen near an edge. It is a marker, not a
// control: it stays hoverable while that element is disabled, which is when a hint is
// needed.
//
// The panel ships without hints - this is kept for the ones the user asks for.
void tip(char const* text);

// Footer action button: kept as the name the footer used; identical to button().
bool footerButton(char const* label, ImVec2 size, bool primary);

// Text helper for the draw-list laid-out parts.
void                text(ImVec2 pos, ImU32 color, char const* value);
[[nodiscard]] float textWidth(char const* value);

} // namespace neurodragon::ui::widgets
