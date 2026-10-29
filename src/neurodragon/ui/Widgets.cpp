#include "neurodragon/ui/Widgets.h"

#include "neurodragon/ui/Theme.h"
#include "neurodragon/utils/Utils.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

namespace neurodragon::ui::widgets {
namespace {

using namespace theme;

float gRowLeft{};
float gRowRight{};

ImVec4 toColor(ImU32 value) { return ImGui::ColorConvertU32ToFloat4(value); }

// Blend two palette entries. ImGui wants one colour per interaction state and
// Theme.h names a single accent, so hover/active steps are mixed from entries that
// are already in the palette rather than introducing new ones.
ImU32 mix(ImU32 from, ImU32 to, float amount) {
    ImVec4 const a = ImGui::ColorConvertU32ToFloat4(from);
    ImVec4 const b = ImGui::ColorConvertU32ToFloat4(to);
    ImVec4 const out{
        a.x + (b.x - a.x) * amount,
        a.y + (b.y - a.y) * amount,
        a.z + (b.z - a.z) * amount,
        a.w + (b.w - a.w) * amount,
    };
    return ImGui::ColorConvertFloat4ToU32(out);
}

float lineHeight() { return ImGui::GetTextLineHeight(); }

float scaled(float value) { return value * theme::scale(); }

// Rows stop growing at this width even when the column is wider: a slider that
// spans a whole 1440p panel is impossible to aim at.
float rowLimit() { return scaled(440.0f); }

// Button and marker metrics, logical units like every other size in this file.
constexpr float kButtonPadX     = 12.0f;  // horizontal padding of an auto-width button
constexpr float kButtonPadY     = 6.0f;   // vertical padding; one line + 2x this is the default height
constexpr float kButtonInsetX   = 2.0f;   // padding of an explicitly sized button: clip inset only
constexpr float kButtonRounding = 4.0f;   // same radius as the panel's boxes and frames
constexpr float kTipGap         = 6.0f;   // space between an element and its "!" marker
constexpr float kTipMinSize     = 14.0f;  // hit box of the marker when the line is shorter
constexpr float kTipWrap        = 280.0f; // text width of the hint box; the box adds kTipPadX on both sides
constexpr float kTipPadX        = 10.0f;  // hint box padding: the 10 / 8 the first version drew with
constexpr float kTipPadY        = 8.0f;
constexpr float kTipMargin      = 4.0f;  // least distance between the hint box and the display edge
constexpr float kTipOffsetX     = 12.0f; // the box sits this far right of the cursor ...
constexpr float kTipOffsetY     = 16.0f; // ... and this far below it
constexpr float kValueEditPadX  = 4.0f;  // padding of the number box the value text turns into
constexpr float kValueEditMinW  = 28.0f; // narrowest number box, so a single digit stays clickable
constexpr float kValueNudgeY    = 1.0f;
// Extra drop for a slider's number, on top of the line's text base offset (FramePadding.y).
// 0 puts the number exactly on the label's baseline; positive pushes it down, in logical
// units, and it moves the number, its hit box and the number box together - they are one
// placement, so tuning them separately would only let them drift apart again.

// The one open number box. ImGui can focus a single text field, so one slot serves
// every slider instead of a per-widget map; `id` is 0 when nothing is being edited.
struct ValueEdit {
    ImGuiID id{};           // slider that owns the box
    char    text[64]{};     // exactly what the box shows - the buffer the focus lives on
    int     lastFrame{};    // last frame its owner was drawn, to notice it disappearing
    bool    focusPending{}; // the box was opened this frame and still needs the keyboard
};

ValueEdit gValueEdit;

// Full parse of a number field. strtof alone would accept "1.5abc" (it stops at the
// 'a'), "inf" and "nan", so validity means: something was parsed, it is finite, and
// nothing but whitespace is left over. Anything else - a partial "-", "1e", "" - is
// rejected and the caller keeps its previous value.
bool parseNumber(char const* text, float* out) {
    if (!text || !out) return false;
    while (*text && std::isspace(static_cast<unsigned char>(*text))) ++text;

    char*       end   = nullptr;
    float const value = std::strtof(text, &end);
    if (end == text || !std::isfinite(value)) return false;

    while (*end && std::isspace(static_cast<unsigned char>(*end))) ++end;
    if (*end != '\0') return false;

    *out = value;
    return true;
}

// The visible part of an ImGui label: everything before "##". TEST-only, because a
// release build's ND_DEBUG does not evaluate its arguments.
#ifdef TEST
std::string_view visibleLabel(char const* label) {
    std::string_view const text = label ? label : "";
    auto const             hash = text.find("##");
    return hash == std::string_view::npos ? text : text.substr(0, hash);
}
#endif

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
        drawList->AddRectFilled(boxMin, boxMax, ImGui::GetColorU32(toColor(kAccent)), scaled(3.0f));
        // Hand-drawn tick: ImGui::RenderCheckMark lives in imgui_internal.h, and the
        // panel has no reason to depend on internal headers.
        float const  stroke = std::max(1.5f, s * 2.0f);
        ImVec2 const a{boxMin.x + box * 0.26f, boxMin.y + box * 0.52f};
        ImVec2 const b{boxMin.x + box * 0.44f, boxMin.y + box * 0.72f};
        ImVec2 const c{boxMin.x + box * 0.76f, boxMin.y + box * 0.28f};
        auto const   tick = ImGui::GetColorU32(toColor(kAccentText));
        drawList->AddLine(a, b, tick, stroke);
        drawList->AddLine(b, c, tick, stroke);
    } else {
        drawList->AddRectFilled(boxMin, boxMax, ImGui::GetColorU32(toColor(kFrame)), scaled(3.0f));
        drawList->AddRect(boxMin, boxMax, ImGui::GetColorU32(toColor(kBorder)), scaled(3.0f), 0, std::max(1.0f, s));
    }

    drawList->AddText(ImVec2(boxMax.x + gap, centerY - lineHeight() * 0.5f), ImGui::GetColorU32(toColor(kText)), label);

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

    auto const s = theme::scale();

    // A Text() line is drawn at CursorPos.y + CurrLineTextBaseOffset, and beginRow's
    // AlignTextToFramePadding put FramePadding.y into that offset (SameLine() then carried
    // it over from the label). DrawList::AddText() and item rects only ever see CursorPos,
    // so the number has to be dropped by exactly this much to land on the label's baseline
    // instead of one frame padding above it. kValueNudgeY is the by-eye correction on top.
    float const textBase = ImGui::GetStyle().FramePadding.y + scaled(kValueNudgeY);

    ImGui::PushID(label);
    ImGuiID const valueId = ImGui::GetID("##valueBox");

    // The box survives across frames - that is where the focus and the typed text live -
    // so a slot whose owner stopped being drawn (tab switched, panel closed) is dropped
    // instead of resuming when the row comes back. Two frames of slack cover a row that
    // is skipped for one frame (a tab transition).
    if (gValueEdit.id != 0 && ImGui::GetFrameCount() - gValueEdit.lastFrame > 2) gValueEdit = ValueEdit{};
    bool const editing = (gValueEdit.id == valueId);

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
    bool       changed    = false;

    ImGui::SameLine();
    if (!editing) {
        ImGui::SetCursorPosX(gRowRight - valueWidth);
        if (!enabled) {
            ImGui::TextColored(toColor(kTextFaint), "%s", shown.c_str());
        } else {
            // The number is a plain text line, but it is also the handle that opens the
            // box: an invisible button the size of the glyphs with the text drawn into
            // it, so the hit box and the glyphs cannot drift apart. Both are dropped onto
            // the label's baseline (textBase) and the cursor is put back afterwards, so a
            // row with an editable number is no taller than one without.
            ImGui::SetCursorPosY(ImGui::GetCursorPosY() + textBase);
            ImVec2 const valuePos = ImGui::GetCursorScreenPos();
            ImGui::InvisibleButton("##valueHit", ImVec2(std::max(valueWidth, scaled(10.0f)), lineHeight()));
            bool const hovered = ImGui::IsItemHovered();
            ImGui::SetCursorPosY(ImGui::GetCursorPosY() - textBase);
            if (hovered) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);

            auto* drawList = ImGui::GetWindowDrawList();
            drawList->AddText(valuePos, ImGui::GetColorU32(toColor(kText)), shown.c_str());
            // A hairline under the number while hovered - the only affordance that says
            // "this can be typed into", drawn in the palette's existing border grey.
            if (hovered) {
                float const underlineY = valuePos.y + lineHeight() - 1.0f;
                drawList->AddLine(
                    ImVec2(valuePos.x, underlineY),
                    ImVec2(valuePos.x + valueWidth, underlineY),
                    ImGui::GetColorU32(toColor(kBorder)),
                    std::max(1.0f, std::round(s))
                );
            }

            if (ImGui::IsItemClicked()) {
                // The box is drawn on the next frame, not this one: submitting it here
                // would put a second item on the number's line, and ImGui would size the
                // next line from the box's top instead of from the line's height - the
                // track would jump for that one frame.
                gValueEdit.id           = valueId;
                gValueEdit.focusPending = true;
                gValueEdit.lastFrame    = ImGui::GetFrameCount();
                std::snprintf(gValueEdit.text, sizeof(gValueEdit.text), "%s", buffer);
            }
        }
    }

    if (editing) {
        gValueEdit.lastFrame = ImGui::GetFrameCount();

        // FramePadding.y = 0 makes the box exactly one text line high, so opening it
        // does not push the track (or anything below) down for as long as it is open.
        float const padX = scaled(kValueEditPadX);
        float const boxW = std::max(valueWidth, scaled(kValueEditMinW)) + 2.0f * padX;

        ImGui::SetCursorPosX(gRowRight - boxW);
        ImGui::SetNextItemWidth(boxW);
        if (gValueEdit.focusPending) ImGui::SetKeyboardFocusHere();

        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(padX, 0.0f));
        ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, scaled(3.0f));
        ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, std::max(1.0f, std::round(s)));
        ImGui::PushStyleColor(ImGuiCol_FrameBg, toColor(kFrame));
        ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, toColor(kFrameHover));
        ImGui::PushStyleColor(ImGuiCol_FrameBgActive, toColor(kFrameHover));
        ImGui::PushStyleColor(ImGuiCol_Border, toColor(kBorder));
        ImGui::PushStyleColor(ImGuiCol_Text, toColor(kText));

        ImGuiInputTextFlags flags = ImGuiInputTextFlags_EnterReturnsTrue;
        if (gValueEdit.focusPending) flags |= ImGuiInputTextFlags_AutoSelectAll;
        gValueEdit.focusPending = false;

        // Escape is read before the box is submitted, so nothing in the widget can have
        // consumed the key yet. ImGui's own Escape handling reverts the text; this
        // reverts the value, which is the part ImGui does not know about.
        bool const escapePressed = ImGui::IsKeyPressed(ImGuiKey_Escape, false);

        // The box replaces the number, so it starts on the same baseline (textBase). The
        // cursor is put back right after it: ItemSize() measures a line from the item's
        // top, so without that correction the track would drop for as long as the box is
        // open. The box therefore must not share its line with the number's hit box -
        // hence it opens on the frame after the click (see the click branch above).
        ImGui::SetCursorPosY(ImGui::GetCursorPosY() + textBase);

        bool const enter = ImGui::InputText("##valueEdit", gValueEdit.text, sizeof(gValueEdit.text), flags);

        bool const edited      = ImGui::IsItemDeactivatedAfterEdit();
        bool const deactivated = ImGui::IsItemDeactivated() || enter;

        ImGui::SetCursorPosY(ImGui::GetCursorPosY() - textBase);

        ImGui::PopStyleColor(5);
        ImGui::PopStyleVar(3);

        // Enter on text that still reads exactly like the prefilled number is a no-op:
        // re-parsing it would only move the value to the rounded display text.
        bool const submit = edited || (enter && std::strcmp(gValueEdit.text, buffer) != 0);

        if (escapePressed) {
            gValueEdit = ValueEdit{}; // discarded; *value never saw the text
        } else if (deactivated) {
            float parsed = 0.0f;
            if (submit && parseNumber(gValueEdit.text, &parsed)) {
                float const next = std::clamp(parsed, min, max);
                if (next != *value) {
                    *value  = next;
                    changed = true;
                }
            }
            gValueEdit = ValueEdit{}; // committed, rolled back, or nothing to commit
        }
    }

    float const trackHeight  = scaled(5.0f);
    float const handleWidth  = scaled(9.0f);
    float const handleHeight = scaled(17.0f);
    // The value text above moved the cursor to the far right; the track must start
    // at the row's left edge and span the whole row.
    float const width = gRowRight - gRowLeft;

    ImGui::SetCursorPosX(gRowLeft);
    ImVec2 const pos = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton("##track", ImVec2(std::max(width, 1.0f), handleHeight + scaled(4.0f)));

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
        ImGui::GetColorU32(toColor(enabled ? kFrame : kBorderSoft)),
        trackHeight * 0.5f
    );
    if (fraction > 0.0f) {
        drawList->AddRectFilled(
            ImVec2(pos.x, centerY - trackHeight * 0.5f),
            ImVec2(pos.x + width * fraction, centerY + trackHeight * 0.5f),
            ImGui::GetColorU32(toColor(enabled ? kAccentSoft : kBorder)),
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
        ImGui::GetColorU32(toColor(enabled ? (ImGui::IsItemActive() ? kText : kAccent) : kTextFaint)),
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

float buttonWidth(char const* label) {
    // Mirrors what ImGui::Button computes for an automatic width: the text before
    // "##" plus this button's own horizontal padding.
    return ImGui::CalcTextSize(label ? label : "", nullptr, true).x + 2.0f * scaled(kButtonPadX);
}

bool button(char const* label, ImVec2 size, bool primary) {
    if (!label) return false;

    auto const  s      = theme::scale();
    float const height = size.y > 0.0f ? size.y : lineHeight() + 2.0f * scaled(kButtonPadY);

    // The label is centred inside (rect - FramePadding). A button shorter than one
    // line of text - the title strip's 20 px zoom boxes are - would then get its
    // glyphs clipped top and bottom, so the vertical padding follows the button and
    // never the style.
    float const padY = std::clamp((height - lineHeight()) * 0.5f, 0.0f, scaled(kButtonPadY));
    // An explicit width means the caller sized the box; a wide inset would only eat
    // the label, so there it shrinks to the border's breathing room.
    float const padX     = size.x > 0.0f ? scaled(kButtonInsetX) : scaled(kButtonPadX);
    float const border   = std::max(1.0f, std::round(s));
    float const rounding = scaled(kButtonRounding);

    ImU32 fill{};
    ImU32 hovered{};
    ImU32 active{};
    ImU32 labelColor{};
    ImU32 edge{};
    if (primary) {
        // Filled accent. Hover brightens toward the label colour, press darkens
        // toward the panel: 1.4:1 of movement in either direction is enough to read
        // at a glance without leaving the accent's hue.
        fill       = kAccent;
        hovered    = mix(kAccent, kText, 0.16f);
        active     = mix(kAccent, kPanelDeep, 0.22f);
        labelColor = kAccentText;
        edge       = mix(kAccent, kPanelDeep, 0.30f);
    } else {
        // Outlined secondary: recessed fill plus a 1 px grey border. Hover lifts the
        // fill one step, press washes it with the accent at low alpha - a state the
        // palette can express without a second accent colour.
        fill       = kPanelDeep;
        hovered    = kFrame;
        active     = kAccentSoft;
        labelColor = kText;
        edge       = kBorder;
    }

    ImGui::PushStyleColor(ImGuiCol_Button, toColor(fill));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, toColor(hovered));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, toColor(active));
    ImGui::PushStyleColor(ImGuiCol_Text, toColor(labelColor));
    ImGui::PushStyleColor(ImGuiCol_Border, toColor(edge));
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(padX, padY));
    ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, border);
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, rounding);

    bool const pressed = ImGui::Button(label, ImVec2(size.x, height));

    ImGui::PopStyleVar(3);
    ImGui::PopStyleColor(5);

    // ImGui has one border colour for every state, so the "this is live" edge is
    // stroked on top while hovered. A disabled item does not report hover, which is
    // exactly the behaviour wanted: no accent edge on something that cannot be used.
    if (ImGui::IsItemHovered()) {
        ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
        ImGui::GetWindowDrawList()->AddRect(
            ImGui::GetItemRectMin(),
            ImGui::GetItemRectMax(),
            primary ? mix(kAccent, kText, 0.45f) : kAccent,
            rounding,
            0,
            border
        );
    }

    // TEST: one line per activation, whether it came from the mouse or from a key.
    if (pressed) ND_DEBUG("ui: button \"{}\" {}", visibleLabel(label), primary ? "primary" : "secondary");
    return pressed;
}

void tip(char const* text) {
    if (!text || !*text) return;

    ImVec2 const prevMin = ImGui::GetItemRectMin();
    ImVec2 const prevMax = ImGui::GetItemRectMax();
    if (prevMax.x <= prevMin.x || prevMax.y <= prevMin.y) return; // nothing submitted before

    auto const  s   = theme::scale();
    float const box = std::max(lineHeight(), scaled(kTipMinSize));

    ImVec2 const cursorBefore = ImGui::GetCursorScreenPos();
    // Screen X of the content region's right edge. The cursor is on the line after the
    // element, so its X plus the width still available is that edge.
    float const contentRight = cursorBefore.x + ImGui::GetContentRegionAvail().x;
    float const centerY      = (prevMin.y + prevMax.y) * 0.5f;

    // There is normally room after the element for the marker. A full-width element -
    // the label-above combo spans the whole column - has none, so the marker moves
    // inside its right end instead. A combo's drop-down arrow lives in the last
    // frame-height square of its frame (ImGui draws it FramePadding.x inside the right
    // edge), so an inside marker stays to the left of that square rather than on it.
    float const arrowRoom = ImGui::GetFrameHeight() + ImGui::GetStyle().FramePadding.x;
    ImVec2      markerPos{};
    if (prevMax.x + scaled(kTipGap) + box <= contentRight) {
        markerPos = ImVec2(prevMax.x + scaled(kTipGap), centerY - box * 0.5f);
    } else {
        markerPos = ImVec2(
            std::max(prevMin.x, prevMax.x - arrowRoom - box),
            // Centred on the element, but never hanging out of it: a marker taller than
            // the element it annotates would otherwise stick out above or below.
            std::clamp(centerY - box * 0.5f, prevMin.y, std::max(prevMin.y, prevMax.y - box))
        );
    }

    // The marker is positioned by hand on the element's line and the caller's cursor is
    // put back afterwards: it is a marker, not a control, so it must not move whatever
    // the caller submits next (a hint after a full-width combo would break the column).
    ImGui::SetCursorScreenPos(markerPos);

    // The id comes from where the marker sits, so several hints in one window cannot
    // collide and each keeps a stable id from frame to frame.
    ImGui::PushID(static_cast<int>(std::lround(prevMin.y)));
    ImGui::PushID(static_cast<int>(std::lround(prevMin.x)));
    ImGui::InvisibleButton("##tip", ImVec2(box, box));
    bool const hovered = ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled);
    ImGui::PopID();
    ImGui::PopID();

    auto*        drawList = ImGui::GetWindowDrawList();
    ImVec2 const center{markerPos.x + box * 0.5f, markerPos.y + box * 0.5f};
    if (hovered) drawList->AddCircleFilled(center, box * 0.5f, kAccentSoft);
    // The glyph comes from the font rather than from hand-built rectangles, so the
    // marker keeps the same weight as the labels around it.
    ImVec2 const glyph = ImGui::CalcTextSize("!");
    drawList->AddText(ImVec2(center.x - glyph.x * 0.5f, center.y - glyph.y * 0.5f), kAccent, "!");

    ImGui::SetCursorScreenPos(cursorBefore);

    if (!hovered) return;

    // Size the box here, before Begin(), from the text itself.
    //
    // Leaving the size to ImGui's auto-resize is what used to squeeze the box into a
    // vertical strip: TextWrapped wraps at the window's content width, an auto-resizing
    // window starts out one item wide, and the narrow result then becomes the next
    // frame's wrap width. The wrapped height is a pure function of the text and the
    // wrap width, so measuring it up front makes the first frame already correct - no
    // frame of auto-resize to learn the width from.
    //
    // The position is fixed here too, for the same reason: moving a window after its
    // content has been appended to (ImGui::SetWindowPos, which the first version did to
    // nudge the box back on screen) smears the text, because the glyphs are already in
    // the draw list at absolute coordinates.
    float const padX     = scaled(kTipPadX);
    float const padY     = scaled(kTipPadY);
    float const textWrap = scaled(kTipWrap);
    // A width that never depends on the text keeps the box from jumping around when a
    // hint quotes a number; only the height follows the paragraph.
    float const width      = std::ceil(textWrap + padX * 2.0f);
    float const textHeight = ImGui::CalcTextSize(text, nullptr, false, textWrap).y;
    // ceil(): a wrapped line can end on a fraction of a pixel and clip its last glyphs.
    float const height = std::ceil(textHeight) + padY * 2.0f;

    // Beside the cursor, flipped to the other side when that would not fit, then kept
    // inside the display - a marker near the right or bottom edge would otherwise put
    // the box outside the screen, where there is nothing to scroll it back.
    ImVec2 const mouse   = ImGui::GetIO().MousePos;
    ImVec2 const display = ImGui::GetIO().DisplaySize;
    float const  margin  = scaled(kTipMargin);

    float x = mouse.x + scaled(kTipOffsetX);
    if (x + width + margin > display.x) x = mouse.x - scaled(kTipOffsetX) - width;
    float y = mouse.y + scaled(kTipOffsetY);
    if (y + height + margin > display.y) y = mouse.y - scaled(kTipOffsetY) - height;

    x = std::clamp(x, margin, std::max(margin, display.x - width - margin));
    y = std::clamp(y, margin, std::max(margin, display.y - height - margin));

    constexpr ImGuiWindowFlags kTipFlags = ImGuiWindowFlags_Tooltip | ImGuiWindowFlags_NoTitleBar
                                         | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove
                                         | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoSavedSettings
                                         | ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoFocusOnAppearing;

    ImGui::SetNextWindowPos(ImVec2(x, y));
    ImGui::SetNextWindowSize(ImVec2(width, height));

    ImGui::PushStyleColor(ImGuiCol_PopupBg, toColor(kPanelDeep));
    ImGui::PushStyleColor(ImGuiCol_Border, toColor(kBorder));
    ImGui::PushStyleColor(ImGuiCol_Text, toColor(kText));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(padX, padY));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, std::max(1.0f, std::round(s)));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, scaled(kButtonRounding));

    if (ImGui::Begin("##widgetTip", nullptr, kTipFlags)) ImGui::TextWrapped("%s", text);
    ImGui::End();

    ImGui::PopStyleVar(3);
    ImGui::PopStyleColor(3);
}

bool footerButton(char const* label, ImVec2 size, bool primary) { return button(label, size, primary); }

} // namespace neurodragon::ui::widgets
