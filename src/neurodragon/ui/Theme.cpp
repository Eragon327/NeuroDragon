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

bool       gBaseStyleReady{};
ImGuiStyle gBaseStyle{};
float      gLastScale{-1.0f};
float      gUserScale{1.0f};
ImVec2     gLastViewport{-1.0f, -1.0f};
ImFont*    gTitleFont{};
ImFont*    gSectionFont{};

} // namespace

void loadFonts() {
    auto& io = ImGui::GetIO();
    io.Fonts->Clear();
    gTitleFont   = nullptr;
    gSectionFont = nullptr;

    ImFontGlyphRangesBuilder ranges{};
    ranges.AddRanges(io.Fonts->GetGlyphRangesDefault());
    ranges.AddText(kUiGlyphs);
    static ImVector<ImWchar> glyphRanges;
    glyphRanges.clear();
    ranges.BuildRanges(&glyphRanges);

    constexpr char const* kFontPath     = "C:\\Windows\\Fonts\\msyh.ttc";
    constexpr char const* kFontBoldPath = "C:\\Windows\\Fonts\\msyhbd.ttc";
    if (std::filesystem::exists(kFontPath)) {
        ImFontConfig body{};
        body.OversampleH = 2;
        body.OversampleV = 2;
        if (io.Fonts->AddFontFromFileTTF(kFontPath, 32.0f, &body, glyphRanges.Data)) {
            ImFontConfig title = body;
            title.OversampleH  = 1;
            title.OversampleV  = 1;
            gTitleFont = io.Fonts->AddFontFromFileTTF(kFontPath, 44.0f, &title, io.Fonts->GetGlyphRangesDefault());
            if (std::filesystem::exists(kFontBoldPath)) {
                gSectionFont = io.Fonts->AddFontFromFileTTF(kFontBoldPath, 32.0f, &body, glyphRanges.Data);
            }
            return;
        }
    }

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
    style.MouseCursorScale = 1.3f;

    style.WindowPadding           = {0.0f, 0.0f};
    style.FramePadding            = {10.0f * s, 7.0f * s};
    style.CellPadding             = {8.0f * s, 5.0f * s};
    style.ItemSpacing             = {12.0f * s, 10.0f * s};
    style.ItemInnerSpacing        = {8.0f * s, 6.0f * s};
    style.WindowRounding          = 10.0f * s;
    style.ChildRounding           = 8.0f * s;
    style.FrameRounding           = 6.0f * s;
    style.PopupRounding           = 6.0f * s;
    style.GrabRounding            = 6.0f * s;
    style.ScrollbarRounding       = 6.0f * s;
    style.TabRounding             = 6.0f * s;
    style.WindowBorderSize        = 0.0f;
    style.ChildBorderSize         = 0.0f;
    style.FrameBorderSize         = 0.0f;
    style.PopupBorderSize         = 1.0f;
    style.ScrollbarSize           = 8.0f * s;
    style.GrabMinSize             = 12.0f * s;
    style.SeparatorTextBorderSize = 0.0f;
    style.TabBarOverlineSize      = 0.0f;

    auto& c                               = style.Colors;
    c[ImGuiCol_Text]                      = rgba(0.922f, 0.929f, 0.941f);
    c[ImGuiCol_TextDisabled]              = rgba(0.471f, 0.494f, 0.533f);
    c[ImGuiCol_WindowBg]                  = rgba(0.110f, 0.118f, 0.133f);
    c[ImGuiCol_ChildBg]                   = rgba(0.0f, 0.0f, 0.0f, 0.0f);
    c[ImGuiCol_PopupBg]                   = rgba(0.110f, 0.118f, 0.133f, 0.99f);
    c[ImGuiCol_Border]                    = rgba(0.243f, 0.267f, 0.306f);
    c[ImGuiCol_BorderShadow]              = rgba(0.0f, 0.0f, 0.0f, 0.0f);
    c[ImGuiCol_FrameBg]                   = rgba(0.169f, 0.184f, 0.212f);
    c[ImGuiCol_FrameBgHovered]            = rgba(0.212f, 0.231f, 0.263f);
    c[ImGuiCol_FrameBgActive]             = rgba(0.255f, 0.275f, 0.306f);
    c[ImGuiCol_TitleBg]                   = rgba(0.110f, 0.118f, 0.133f);
    c[ImGuiCol_TitleBgActive]             = rgba(0.110f, 0.118f, 0.133f);
    c[ImGuiCol_MenuBarBg]                 = rgba(0.110f, 0.118f, 0.133f);
    c[ImGuiCol_ScrollbarBg]               = rgba(0.0f, 0.0f, 0.0f, 0.0f);
    c[ImGuiCol_ScrollbarGrab]             = rgba(0.290f, 0.310f, 0.345f, 0.85f);
    c[ImGuiCol_ScrollbarGrabHovered]      = rgba(0.360f, 0.380f, 0.415f, 0.95f);
    c[ImGuiCol_ScrollbarGrabActive]       = rgba(0.940f, 0.639f, 0.235f, 0.95f);
    c[ImGuiCol_CheckMark]                 = rgba(0.941f, 0.639f, 0.235f);
    c[ImGuiCol_SliderGrab]                = rgba(0.941f, 0.639f, 0.235f);
    c[ImGuiCol_SliderGrabActive]          = rgba(1.0f, 0.72f, 0.35f);
    c[ImGuiCol_Button]                    = rgba(0.169f, 0.184f, 0.212f);
    c[ImGuiCol_ButtonHovered]             = rgba(0.212f, 0.231f, 0.263f);
    c[ImGuiCol_ButtonActive]              = rgba(0.255f, 0.275f, 0.306f);
    c[ImGuiCol_Header]                    = rgba(0.0f, 0.0f, 0.0f, 0.0f);
    c[ImGuiCol_HeaderHovered]             = rgba(0.169f, 0.184f, 0.212f, 0.65f);
    c[ImGuiCol_HeaderActive]              = rgba(0.169f, 0.184f, 0.212f);
    c[ImGuiCol_Separator]                 = rgba(0.169f, 0.184f, 0.212f);
    c[ImGuiCol_SeparatorHovered]          = rgba(0.941f, 0.639f, 0.235f, 0.6f);
    c[ImGuiCol_SeparatorActive]           = rgba(0.941f, 0.639f, 0.235f);
    c[ImGuiCol_ResizeGrip]                = rgba(0.0f, 0.0f, 0.0f, 0.0f);
    c[ImGuiCol_Tab]                       = rgba(0.0f, 0.0f, 0.0f, 0.0f);
    c[ImGuiCol_TabHovered]                = rgba(0.212f, 0.231f, 0.263f, 0.75f);
    c[ImGuiCol_TabSelected]               = rgba(0.169f, 0.184f, 0.212f);
    c[ImGuiCol_TabSelectedOverline]       = rgba(0.0f, 0.0f, 0.0f, 0.0f);
    c[ImGuiCol_TabDimmed]                 = rgba(0.0f, 0.0f, 0.0f, 0.0f);
    c[ImGuiCol_TabDimmedSelected]         = rgba(0.129f, 0.141f, 0.165f);
    c[ImGuiCol_TabDimmedSelectedOverline] = rgba(0.0f, 0.0f, 0.0f, 0.0f);
    c[ImGuiCol_PlotLines]                 = rgba(0.941f, 0.639f, 0.235f);
    c[ImGuiCol_PlotLinesHovered]          = rgba(1.0f, 0.75f, 0.4f);
    c[ImGuiCol_NavCursor]                 = rgba(0.941f, 0.639f, 0.235f);
    c[ImGuiCol_ModalWindowDimBg]          = rgba(0.0f, 0.0f, 0.0f, 0.72f);

    ImGui::GetStyle() = style;
    ImGui::GetIO().FontGlobalScale = s * 0.5f;

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
ImFont* sectionFont() { return gSectionFont; }

void reset() {
    gBaseStyleReady = false;
    gLastScale      = -1.0f;
    gLastViewport   = {-1.0f, -1.0f};
    gTitleFont      = nullptr;
    gSectionFont    = nullptr;
    if (ImGui::GetCurrentContext()) ImGui::GetIO().FontGlobalScale = 1.0f;
}

} // namespace neurodragon::ui::theme
