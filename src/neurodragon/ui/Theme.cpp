#include "neurodragon/ui/Theme.h"

#include "neurodragon/ui/Glyphs.h"

#include <algorithm>
#include <cmath>
#include <filesystem>

namespace neurodragon::ui {
namespace {

ImVec4 rgba(float r, float g, float b, float a = 1.0f) { return {r, g, b, a}; }

} // namespace

UiMetrics calculateMetrics(ImVec2 viewport, float uiScale) {
    UiMetrics metrics;
    metrics.viewport = viewport;
    metrics.scale    = std::clamp(uiScale, 0.7f, 3.5f);
    metrics.gap      = 10.0f * metrics.scale;
    metrics.pad      = 16.0f * metrics.scale;
    metrics.rounding = 4.0f * metrics.scale;
    return metrics;
}

} // namespace neurodragon::ui

namespace neurodragon::ui::theme {
namespace {

constexpr float kDensity = 0.9f; // extra text density multiplier on top of `scale`

bool       gBaseStyleReady{};
ImGuiStyle gBaseStyle{};
float      gLastScale{-1.0f};
float      gUserScale{1.0f};
ImVec2     gLastViewport{-1.0f, -1.0f};
ImFont*    gTitleFont{};

} // namespace

void loadFonts() {
    auto& io = ImGui::GetIO();
    io.Fonts->Clear();
    gTitleFont = nullptr;

    // Latin/symbols plus exactly the CJK characters this panel draws (see
    // Glyphs.h). A full CJK set would rasterise ~20k glyphs: tens of megabytes of
    // atlas and a visible build hitch.
    ImFontGlyphRangesBuilder ranges{};
    ranges.AddRanges(io.Fonts->GetGlyphRangesDefault());
    ranges.AddText(kUiGlyphs);
    static ImVector<ImWchar> glyphRanges;
    glyphRanges.clear();
    ranges.BuildRanges(&glyphRanges);

    constexpr char const* kFontPath = "C:\\Windows\\Fonts\\msyh.ttc";
    if (std::filesystem::exists(kFontPath)) {
        ImFontConfig body{};
        body.OversampleH = 2;
        body.OversampleV = 2;
        if (io.Fonts->AddFontFromFileTTF(kFontPath, 32.0f, &body, glyphRanges.Data)) {
            // The title is ASCII ("NeuroDragon"): keep its atlas footprint tiny.
            ImFontConfig title = body;
            title.OversampleH  = 1;
            title.OversampleV  = 1;
            gTitleFont = io.Fonts->AddFontFromFileTTF(kFontPath, 44.0f, &title, io.Fonts->GetGlyphRangesDefault());
            return;
        }
    }

    // No system font: the panel still works, Chinese labels become boxes. Keep the
    // panel up rather than refusing to draw.
    io.Fonts->AddFontDefault();
}

void apply(UiMetrics const& metrics) {
    if (!gBaseStyleReady) {
        gBaseStyle      = ImGui::GetStyle();
        gBaseStyleReady = true;
    }

    auto const viewportChanged =
        std::abs(gLastViewport.x - metrics.viewport.x) > 0.5f || std::abs(gLastViewport.y - metrics.viewport.y) > 0.5f;
    if (std::abs(gLastScale - metrics.scale) < 0.001f && !viewportChanged) return;

    auto const s = metrics.scale;

    auto style = gBaseStyle;
    style.ScaleAllSizes(s);
    // The software cursor is drawn at the current font size, so it shrank together
    // with the text. 1.3 keeps it clearly visible on top of the picture at every
    // scale without changing the cursor shape ImGui picks.
    style.MouseCursorScale = 1.3f;

    style.WindowPadding           = {0.0f, 0.0f};
    style.FramePadding            = {10.0f * s, 6.0f * s};
    style.CellPadding             = {6.0f * s, 4.0f * s};
    style.ItemSpacing             = {10.0f * s, 8.0f * s};
    style.ItemInnerSpacing        = {8.0f * s, 6.0f * s};
    style.WindowRounding          = 6.0f * s;
    style.ChildRounding           = 5.0f * s;
    style.FrameRounding           = 4.0f * s;
    style.PopupRounding           = 4.0f * s;
    style.GrabRounding            = 4.0f * s;
    style.ScrollbarRounding       = 4.0f * s;
    style.TabRounding             = 3.0f * s;
    style.WindowBorderSize        = 0.0f;
    style.ChildBorderSize         = 0.0f;
    style.FrameBorderSize         = 0.0f;
    style.PopupBorderSize         = 1.0f;
    style.ScrollbarSize           = 7.0f * s;
    style.GrabMinSize             = 10.0f * s;
    style.SeparatorTextBorderSize = std::max(1.0f, std::round(1.0f * s));
    style.TabBarOverlineSize      = std::max(2.0f, std::round(2.0f * s));

    auto& c                               = style.Colors;
    c[ImGuiCol_Text]                      = rgba(0.914f, 0.914f, 0.914f);
    c[ImGuiCol_TextDisabled]              = rgba(0.540f, 0.545f, 0.553f);
    c[ImGuiCol_WindowBg]                  = rgba(0.129f, 0.133f, 0.141f);
    c[ImGuiCol_ChildBg]                   = rgba(0.0f, 0.0f, 0.0f, 0.0f);
    c[ImGuiCol_PopupBg]                   = rgba(0.118f, 0.122f, 0.129f, 0.98f);
    c[ImGuiCol_Border]                    = rgba(0.235f, 0.239f, 0.251f);
    c[ImGuiCol_BorderShadow]              = rgba(0.0f, 0.0f, 0.0f, 0.0f);
    c[ImGuiCol_FrameBg]                   = rgba(0.180f, 0.184f, 0.196f);
    c[ImGuiCol_FrameBgHovered]            = rgba(0.220f, 0.224f, 0.239f);
    c[ImGuiCol_FrameBgActive]             = rgba(0.255f, 0.259f, 0.275f);
    c[ImGuiCol_TitleBg]                   = rgba(0.129f, 0.133f, 0.141f);
    c[ImGuiCol_TitleBgActive]             = rgba(0.129f, 0.133f, 0.141f);
    c[ImGuiCol_MenuBarBg]                 = rgba(0.118f, 0.122f, 0.129f);
    c[ImGuiCol_ScrollbarBg]               = rgba(0.0f, 0.0f, 0.0f, 0.0f);
    c[ImGuiCol_ScrollbarGrab]             = rgba(0.290f, 0.294f, 0.306f, 0.85f);
    c[ImGuiCol_ScrollbarGrabHovered]      = rgba(0.360f, 0.365f, 0.376f, 0.95f);
    c[ImGuiCol_ScrollbarGrabActive]       = rgba(0.940f, 0.639f, 0.235f, 0.95f);
    c[ImGuiCol_CheckMark]                 = rgba(0.941f, 0.639f, 0.235f);
    c[ImGuiCol_SliderGrab]                = rgba(0.941f, 0.639f, 0.235f);
    c[ImGuiCol_SliderGrabActive]          = rgba(1.0f, 0.72f, 0.35f);
    c[ImGuiCol_Button]                    = rgba(0.180f, 0.184f, 0.196f);
    c[ImGuiCol_ButtonHovered]             = rgba(0.235f, 0.239f, 0.251f);
    c[ImGuiCol_ButtonActive]              = rgba(0.275f, 0.278f, 0.290f);
    c[ImGuiCol_Header]                    = rgba(0.180f, 0.184f, 0.196f);
    c[ImGuiCol_HeaderHovered]             = rgba(0.235f, 0.239f, 0.251f);
    c[ImGuiCol_HeaderActive]              = rgba(0.180f, 0.184f, 0.196f);
    c[ImGuiCol_Separator]                 = rgba(0.200f, 0.204f, 0.216f);
    c[ImGuiCol_SeparatorHovered]          = rgba(0.941f, 0.639f, 0.235f, 0.6f);
    c[ImGuiCol_SeparatorActive]           = rgba(0.941f, 0.639f, 0.235f);
    c[ImGuiCol_ResizeGrip]                = rgba(0.0f, 0.0f, 0.0f, 0.0f);
    c[ImGuiCol_Tab]                       = rgba(0.0f, 0.0f, 0.0f, 0.0f);
    c[ImGuiCol_TabHovered]                = rgba(0.235f, 0.239f, 0.251f, 0.55f);
    c[ImGuiCol_TabSelected]               = rgba(0.0f, 0.0f, 0.0f, 0.0f);
    c[ImGuiCol_TabSelectedOverline]       = rgba(0.941f, 0.639f, 0.235f);
    c[ImGuiCol_TabDimmed]                 = rgba(0.0f, 0.0f, 0.0f, 0.0f);
    c[ImGuiCol_TabDimmedSelected]         = rgba(0.0f, 0.0f, 0.0f, 0.0f);
    c[ImGuiCol_TabDimmedSelectedOverline] = rgba(0.941f, 0.639f, 0.235f, 0.45f);
    c[ImGuiCol_PlotLines]                 = rgba(0.941f, 0.639f, 0.235f);
    c[ImGuiCol_PlotLinesHovered]          = rgba(1.0f, 0.75f, 0.4f);
    c[ImGuiCol_NavCursor]                 = rgba(0.941f, 0.639f, 0.235f);
    c[ImGuiCol_ModalWindowDimBg]          = rgba(0.0f, 0.0f, 0.0f, 0.72f);

    ImGui::GetStyle() = style;
    // Atlas is built at 2x, so half the scale is the logical text size.
    ImGui::GetIO().FontGlobalScale = s * 0.5f * kDensity;

    gLastScale    = metrics.scale;
    gLastViewport = metrics.viewport;
}

void apply(ImVec2 viewport) {
    if (viewport.x <= 0.0f || viewport.y <= 0.0f) return;
    // Never shrink below the 1080p layout. At 720p the old 0.7x panel made the text
    // unreadable and the ImGui software cursor (it follows the font size) a ~10 px
    // speck that disappeared against the picture, so the UI now stays at 1.0 there
    // and only grows on larger viewports (2026-10-23).
    apply(calculateMetrics(viewport, std::max(1.0f, viewport.y / 1080.0f) * gUserScale));
}

float scale() { return gLastScale > 0.0f ? gLastScale : 1.0f; }

float userScale() { return gUserScale; }

void setUserScale(float value) {
    auto const clamped = std::clamp(value, 0.75f, 1.75f);
    if (std::abs(clamped - gUserScale) < 0.001f) return;
    gUserScale    = clamped;
    gLastScale    = -1.0f; // force the next apply() to rebuild the style
    gLastViewport = {-1.0f, -1.0f};
}

ImFont* titleFont() { return gTitleFont; }

void reset() {
    gBaseStyleReady = false;
    gLastScale      = -1.0f;
    gLastViewport   = {-1.0f, -1.0f};
    gTitleFont      = nullptr;
    if (ImGui::GetCurrentContext()) ImGui::GetIO().FontGlobalScale = 1.0f;
}

} // namespace neurodragon::ui::theme
