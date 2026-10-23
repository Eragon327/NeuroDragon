#include "neurodragon/ui/Widgets.h"

#include "neurodragon/ui/Theme.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>

namespace neurodragon::ui::widgets {
namespace {

using namespace theme;

float gRowLeft{};
float gRowRight{};

ImVec4 toColor(ImU32 value) { return ImGui::ColorConvertU32ToFloat4(value); }

float lineHeight() { return ImGui::GetTextLineHeight(); }

float scaled(float value) { return value * theme::scale(); }

// Rows stop growing at this width even when the column is wider: a slider that
// spans a whole 1440p panel is impossible to aim at.
float rowLimit() { return scaled(440.0f); }

// Shorten a label so it can never run into the right-aligned value.
//
// The obvious implementation (PushClipRect around the label) cuts the glyphs in
// half: RenderText() draws at CursorPos.y + CurrLineTextBaseOffset, so a clip rect
// built from GetCursorScreenPos() starts above the actual glyphs. Cutting the
// string is also what a normal UI does - the text stays whole, it just ends with an
// ellipsis.
std::string ellipsize(char const* text, float maxWidth) {
    if (!text || maxWidth <= 0.0f) return {};
    if (ImGui::CalcTextSize(text).x <= maxWidth) return text;

    char const* const end          = text + std::strlen(text);
    float const       ellipsisSize = ImGui::CalcTextSize("\xE2\x80\xA6").x;
    char const*       best         = text;

    for (char const* it = text; it < end;) {
        unsigned char const lead   = static_cast<unsigned char>(*it);
        int const           bytes  = lead < 0x80         ? 1
                                   : (lead >> 5) == 0x6  ? 2
                                   : (lead >> 4) == 0xE  ? 3
                                   : (lead >> 3) == 0x1E ? 4
                                                         : 1;
        it                        += bytes;
        if (it > end) break;
        if (ImGui::CalcTextSize(text, it).x + ellipsisSize > maxWidth) break;
        best = it;
    }

    std::string out(text, static_cast<size_t>(best - text));
    out += "\xE2\x80\xA6";
    return out;
}

} // namespace

void beginRow() {
    ImGui::AlignTextToFramePadding();
    gRowLeft  = ImGui::GetCursorPosX();
    gRowRight = gRowLeft + std::min(ImGui::GetContentRegionAvail().x, rowLimit());
    ImGui::SetCursorPosX(gRowLeft);
}

float rowRight() { return gRowRight; }

void text(ImVec2 pos, ImU32 color, char const* value) { ImGui::GetWindowDrawList()->AddText(pos, color, value); }

float textWidth(char const* value) { return ImGui::CalcTextSize(value).x; }

void labelRow(std::string_view label) {
    beginRow();
    ImGui::TextColored(toColor(kText), "%.*s", static_cast<int>(label.size()), label.data());
    ImGui::SameLine();
}

void sectionHeader(char const* label) {
    ImGui::PushStyleColor(ImGuiCol_Text, toColor(kTextDim));
    ImGui::SeparatorText(label);
    ImGui::PopStyleColor();
}

void statRow(char const* label, char const* value, ImU32 valueColor) {
    beginRow();

    auto const  valueWidth = ImGui::CalcTextSize(value).x;
    float const gap        = scaled(10.0f);
    float const labelRoom  = std::max(0.0f, (gRowRight - gRowLeft) - valueWidth - gap);

    std::string const labelText = ellipsize(label, labelRoom);
    ImGui::TextColored(toColor(kTextDim), "%s", labelText.c_str());

    ImGui::SameLine();
    ImGui::SetCursorPosX(gRowRight - valueWidth);
    ImGui::TextColored(toColor(valueColor ? valueColor : kText), "%s", value);
}

void hint(char const* value) {
    ImGui::PushStyleColor(ImGuiCol_Text, toColor(kTextFaint));
    ImGui::TextWrapped("%s", value);
    ImGui::PopStyleColor();
}

bool toggle(char const* label, bool* value) {
    auto const s = theme::scale();

    ImGui::PushID(label);
    float const box       = scaled(17.0f);
    float const gap       = scaled(9.0f);
    auto const  labelSize = ImGui::CalcTextSize(label);
    float const rowHeight = std::max(box, lineHeight());

    ImVec2 const pos = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton("##toggle", ImVec2(box + gap + labelSize.x, rowHeight));

    bool changed = false;
    if (ImGui::IsItemClicked()) {
        *value  = !*value;
        changed = true;
    }
    if (ImGui::IsItemHovered()) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);

    auto*        drawList = ImGui::GetWindowDrawList();
    float const  centerY  = pos.y + rowHeight * 0.5f;
    ImVec2 const boxMin{pos.x, centerY - box * 0.5f};
    ImVec2 const boxMax{pos.x + box, centerY + box * 0.5f};

    if (*value) {
        drawList->AddRectFilled(boxMin, boxMax, kAccent, scaled(3.0f));
        // Hand-drawn tick: ImGui::RenderCheckMark lives in imgui_internal.h, and the
        // panel has no reason to depend on internal headers.
        float const  stroke = std::max(1.5f, s * 2.0f);
        ImVec2 const a{boxMin.x + box * 0.26f, boxMin.y + box * 0.52f};
        ImVec2 const b{boxMin.x + box * 0.44f, boxMin.y + box * 0.72f};
        ImVec2 const c{boxMin.x + box * 0.76f, boxMin.y + box * 0.28f};
        drawList->AddLine(a, b, kAccentText, stroke);
        drawList->AddLine(b, c, kAccentText, stroke);
    } else {
        drawList->AddRectFilled(boxMin, boxMax, kFrame, scaled(3.0f));
        drawList->AddRect(boxMin, boxMax, kBorder, scaled(3.0f), 0, std::max(1.0f, s));
    }

    drawList->AddText(ImVec2(boxMax.x + gap, centerY - lineHeight() * 0.5f), kText, label);

    ImGui::PopID();
    return changed;
}

bool slider(
    char const* label,
    float*      value,
    float       min,
    float       max,
    char const* format,
    char const* suffix,
    bool        enabled
) {
    if (!value) return false;

    ImGui::PushID(label);

    char buffer[64]{};
    std::snprintf(buffer, sizeof(buffer), format, static_cast<double>(*value));

    std::string shown = buffer;
    if (suffix && *suffix) {
        shown += ' ';
        shown += suffix;
    }

    beginRow();
    ImGui::TextColored(toColor(enabled ? kTextDim : kTextFaint), "%s", label);

    auto const valueWidth = ImGui::CalcTextSize(shown.c_str()).x;
    ImGui::SameLine();
    ImGui::SetCursorPosX(gRowRight - valueWidth);
    ImGui::TextColored(toColor(enabled ? kText : kTextFaint), "%s", shown.c_str());

    float const trackHeight  = scaled(5.0f);
    float const handleWidth  = scaled(9.0f);
    float const handleHeight = scaled(17.0f);
    // The value text above moved the cursor to the far right; the track must start
    // at the row's left edge and span the whole row.
    float const width = gRowRight - gRowLeft;

    ImGui::SetCursorPosX(gRowLeft);
    ImVec2 const pos = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton("##track", ImVec2(std::max(width, 1.0f), handleHeight + scaled(4.0f)));

    bool changed = false;
    if (enabled && ImGui::IsItemActive()) {
        auto const  relative = std::clamp((ImGui::GetIO().MousePos.x - pos.x) / std::max(1.0f, width), 0.0f, 1.0f);
        float const next     = min + relative * (max - min);
        if (std::abs(next - *value) > 1e-6f) {
            *value  = next;
            changed = true;
        }
    }
    if (enabled && ImGui::IsItemHovered()) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);

    auto*       drawList = ImGui::GetWindowDrawList();
    float const fraction = (max > min) ? std::clamp((*value - min) / (max - min), 0.0f, 1.0f) : 0.0f;
    float const centerY  = pos.y + handleHeight * 0.5f + scaled(2.0f);

    drawList->AddRectFilled(
        ImVec2(pos.x, centerY - trackHeight * 0.5f),
        ImVec2(pos.x + width, centerY + trackHeight * 0.5f),
        enabled ? kFrame : kBorderSoft,
        trackHeight * 0.5f
    );
    if (fraction > 0.0f) {
        drawList->AddRectFilled(
            ImVec2(pos.x, centerY - trackHeight * 0.5f),
            ImVec2(pos.x + width * fraction, centerY + trackHeight * 0.5f),
            enabled ? kAccentSoft : kBorder,
            trackHeight * 0.5f
        );
    }

    // The handle is inset by half its width at both ends: at fraction 0 or 1 a
    // centred handle would stick out of the track and get clipped by the column.
    float const handleX =
        std::clamp(pos.x + width * fraction, pos.x + handleWidth * 0.5f, pos.x + width - handleWidth * 0.5f);
    drawList->AddRectFilled(
        ImVec2(handleX - handleWidth * 0.5f, centerY - handleHeight * 0.5f),
        ImVec2(handleX + handleWidth * 0.5f, centerY + handleHeight * 0.5f),
        enabled ? (ImGui::IsItemActive() ? kText : kAccent) : kTextFaint,
        scaled(3.0f)
    );

    ImGui::PopID();
    return changed;
}

bool sliderInt(char const* label, int* value, int min, int max, char const* suffix) {
    if (!value) return false;
    float asFloat = static_cast<float>(*value);
    bool  changed = slider(label, &asFloat, static_cast<float>(min), static_cast<float>(max), "%.0f", suffix);
    auto  rounded = static_cast<int>(std::lround(asFloat));
    if (changed && rounded != *value) {
        *value = rounded;
        return true;
    }
    return false;
}

bool combo(char const* label, int* current, char const* const* items, int count) {
    if (!current || !items || count <= 0) return false;

    ImGui::PushID(label);
    beginRow();
    ImGui::TextColored(toColor(kTextDim), "%s", label);
    ImGui::SetNextItemWidth(gRowRight - gRowLeft);
    bool const changed = ImGui::Combo("##combo", current, items, count);
    ImGui::PopID();
    return changed;
}

void nodeBox(ImVec2 pos, ImVec2 size, char const* title, char const* line1, char const* line2, bool active) {
    auto const s        = theme::scale();
    auto*      drawList = ImGui::GetWindowDrawList();

    drawList
        ->AddRectFilled(pos, ImVec2(pos.x + size.x, pos.y + size.y), active ? kPanelRaised : kPanelDeep, scaled(4.0f));
    drawList->AddRect(
        pos,
        ImVec2(pos.x + size.x, pos.y + size.y),
        active ? kAccent : kBorder,
        scaled(4.0f),
        0,
        std::max(1.0f, s)
    );

    float const padX = scaled(10.0f);
    float       y    = pos.y + scaled(7.0f);

    drawList->AddText(ImVec2(pos.x + padX, y), active ? kText : kTextDim, title);
    y += lineHeight() + scaled(3.0f);

    if (line1 && *line1) {
        drawList->AddText(ImVec2(pos.x + padX, y), active ? kTextDim : kTextFaint, line1);
        y += lineHeight() + scaled(1.0f);
    }
    if (line2 && *line2) {
        drawList->AddText(ImVec2(pos.x + padX, y), active ? kTextDim : kTextFaint, line2);
    }
}

void nodeArrow(ImVec2 from, ImVec2 to, bool accent) {
    auto*       drawList  = ImGui::GetWindowDrawList();
    ImU32 const color     = accent ? kAccent : kBorder;
    float const thickness = std::max(1.0f, theme::scale() * 1.2f);

    drawList->AddLine(from, ImVec2(to.x - scaled(5.0f), to.y), color, thickness);

    float const  head = scaled(5.0f);
    ImVec2 const tip{to.x, to.y};
    drawList->AddTriangleFilled(
        tip,
        ImVec2(tip.x - head * 1.6f, tip.y - head * 0.8f),
        ImVec2(tip.x - head * 1.6f, tip.y + head * 0.8f),
        color
    );
}

void frameGraph(ImVec2 pos, ImVec2 size, std::vector<float> const& values, float maxMs) {
    auto*      drawList = ImGui::GetWindowDrawList();
    auto const s        = theme::scale();

    drawList->AddRectFilled(pos, ImVec2(pos.x + size.x, pos.y + size.y), kPanelDeep, scaled(3.0f));

    // Three hairlines: bottom, middle, top.
    for (int index = 0; index <= 2; ++index) {
        float const y = pos.y + size.y * (static_cast<float>(index) / 2.0f);
        drawList->AddLine(
            ImVec2(pos.x, y),
            ImVec2(pos.x + size.x, y),
            IM_COL32(255, 255, 255, index == 0 ? 26 : 14),
            std::max(1.0f, s * 0.7f)
        );
    }

    char scaleLabel[32]{};
    std::snprintf(scaleLabel, sizeof(scaleLabel), "0 - %.0f ms", static_cast<double>(maxMs));
    drawList->AddText(
        ImVec2(pos.x + size.x * 0.5f - textWidth(scaleLabel) * 0.5f, pos.y + scaled(3.0f)),
        kTextFaint,
        scaleLabel
    );

    if (values.size() < 2) return;

    auto const  count  = static_cast<int>(values.size());
    auto const  step   = size.x / static_cast<float>(count - 1);
    float const usable = size.y - scaled(8.0f);

    ImVector<ImVec2> points;
    points.reserve(count);
    for (int index = 0; index < count; ++index) {
        float const ms       = values[static_cast<size_t>(index)];
        float const fraction = std::clamp(ms / std::max(1.0f, maxMs), 0.0f, 1.0f);
        points.push_back(
            ImVec2(pos.x + step * static_cast<float>(index), pos.y + size.y - scaled(3.0f) - usable * fraction)
        );
    }

    drawList->AddPolyline(points.Data, points.Size, kAccent, 0, std::max(1.0f, s * 1.2f));
}

bool footerButton(char const* label, ImVec2 size, bool primary) {
    auto const& style = ImGui::GetStyle();

    if (primary) {
        ImGui::PushStyleColor(ImGuiCol_Button, toColor(kAccent));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(1.0f, 0.72f, 0.35f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.90f, 0.58f, 0.20f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_Text, toColor(kAccentText));
    } else {
        ImGui::PushStyleColor(ImGuiCol_Button, toColor(kPanelDeep));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, toColor(kFrame));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, toColor(kFrameHover));
        ImGui::PushStyleColor(ImGuiCol_Text, toColor(kText));
    }
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(style.FramePadding.x, (size.y - lineHeight()) * 0.5f));

    bool const pressed = ImGui::Button(label, size);

    ImGui::PopStyleVar();
    ImGui::PopStyleColor(4);
    return pressed;
}

} // namespace neurodragon::ui::widgets
