#include "neurodragon/ui/Menu.h"

#include "neurodragon/Config.h"
#include "neurodragon/Test.h"
#include "neurodragon/input/KeyBind.h"
#include "neurodragon/nr/D3D12Bridge.h"
#include "neurodragon/probe/VvInject.h"
#include "neurodragon/probe/VvProbe.h"
#include "neurodragon/sr/SrSharpening.h"
#include "neurodragon/ui/Overlay.h"
#include "neurodragon/ui/Theme.h"
#include "neurodragon/ui/Widgets.h"
#include "neurodragon/utils/Utils.h"
#include "neurodragon/vv/VvBypass.h"
#include "neurodragon/vv/VvCapture.h"
#include "neurodragon/vv/VvDlssSr.h"
#include "neurodragon/vv/VvSr.h"

#include "ll/api/i18n/I18n.h"

#include <imgui.h>

#include <Windows.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

namespace neurodragon::ui::menu {
namespace {

using ll::i18n_literals::operator""_tr;
using namespace theme;

// ---------------------------------------------------------------------------
// Panel state
//
// The panel edits a *draft* copy; the running renderer only sees it after
// "Apply". Nothing is written to disk unless "Save as default" is pressed -
// config/config.json is an import at load, not a live file.
// ---------------------------------------------------------------------------

config::Nr  gDraft{};
config::Sr  gDraftSr{};

// A percentage saved by an older build (75%) is no longer offered. Treat any
// unknown value as "follow the engine" so the dropdown and the live config agree.
int sanitizedSrPercent(int percent) {
    switch (percent) {
    case 0:
    case 33:
    case 50:
    case 58:
    case 66:
    case 100:
        return percent;
    default:
        return 0;
    }
}

config::Sr sanitizedSr(config::Sr sr) {
    sr.scalePercent = sanitizedSrPercent(sr.scalePercent);
    // A hand-edited config can carry anything; the render thread reads the strength
    // every frame, so keep one sanitised copy at the panel boundary as well.
    if (!std::isfinite(sr.sharpness) || sr.sharpness < 0.0f || sr.sharpness > 1.0f) {
        sr.sharpness = std::clamp(std::isfinite(sr.sharpness) ? sr.sharpness : 0.672f, 0.0f, 1.0f);
    }
    return sr;
}

// Hotkey drafts. Combinations, like everything the overlay matches: "apply" writes
// these back into the config, and the overlay re-reads the config from there.
input::KeyBind gDraftPanelKeys{};
input::KeyBind gDraftNrKeys{};
input::KeyBind gDraftResetKeys{};
input::KeyBind gDraftScreenshotKeys{};
// Which row started the capture (index into the hotkey table); -1 = none. The
// capture itself lives in the overlay because the window thread feeds it.
int gPendingKeySlot{-1};
// Model builds listed from the mod directory. Scanned once per panel open instead of
// per frame: the pick only matters on the next start anyway.
std::vector<std::string> gModelFiles;
int         gActiveTab{};
bool        gDirty{};
std::string gStatus;

// Window position. The ImGui window is not movable (NoMove): dragging is handled
// by the title strip only, exactly like a Win32 window, so a click anywhere else in
// the panel never drags it.
ImVec2 gPos{-1.0f, -1.0f};
bool   gDragging{};

void markDirty() {
    gDirty = true;
    gStatus.clear();
}

void setStatus(std::string text) { gStatus = std::move(text); }

void at(ImVec2 position) { ImGui::SetCursorScreenPos(position); }

// Staging starts from what is configured (the overlay runs the same parse, so the
// draft starts out identical to the live bindings). An empty stored array is a real
// value: "unbound", not "no record".
void reloadKeyDrafts() {
    gDraftPanelKeys      = parsePanelKeys(config::get().ui.openKeys); // never empty: no other way in
    gDraftNrKeys         = parseKeys(config::get().ui.nrKeys, config::kDefaultNrKey);
    gDraftResetKeys      = parseKeys(config::get().ui.resetKeys, config::kDefaultResetKey);
    gDraftScreenshotKeys = parseKeys(config::get().ui.screenshotKeys, config::kDefaultScreenshotKey);
    gPendingKeySlot      = -1;
}

bool mappedCombo(char const* label, int* value, char const* const* items, int const* values, int count) {
    int index = 0;
    for (int candidate = 0; candidate < count; ++candidate) {
        if (values[candidate] == *value) {
            index = candidate;
            break;
        }
    }
    if (!widgets::combo(label, &index, items, count)) return false;
    if (values[index] == *value) return false;
    *value = values[index];
    return true;
}

std::string replaceName(int mode) {
    switch (mode) {
    case 0:
        return "ui.replace.0"_tr();
    case 5:
        return "ui.replace.5"_tr();
    case 6:
        return "ui.replace.6"_tr();
    default:
        return "ui.replace.other"_tr();
    }
}

std::string replaceHint(int mode) {
    switch (mode) {
    case 5:
        return "ui.replace.hint5"_tr();
    case 6:
        return "ui.replace.hint6"_tr();
    default:
        return "ui.replace.hint6"_tr();
    }
}

// DLSS quality slot (the engine writes PerfQualityValue at create) and the network
// preset hint. Values are the measured ones from docs/I §10: 0..5 = MaxPerf /
// Balanced / MaxQuality / UltraPerf / UltraQuality (rejected by NGX) / DLAA, and
// the preset hint carries 0x80000000 | (5=E, 6=F, 10=J, 11=K, 12=L, 13=M).
std::string qualityName(unsigned quality) {
    switch (quality) {
    case 0:
        return "MaxPerf";
    case 1:
        return "Balanced";
    case 2:
        return "MaxQuality";
    case 3:
        return "UltraPerf";
    case 4:
        return "UltraQuality";
    case 5:
        return "DLAA";
    default:
        return "-";
    }
}

std::string presetName(unsigned hint) {
    unsigned const preset = hint & 0x7FFFFFFFu;
    switch (preset) {
    case 0:
        return "ui.sr.presetDefault"_tr();
    case 5:
        return "E";
    case 6:
        return "F";
    case 10:
        return "J";
    case 11:
        return "K";
    case 12:
        return "L";
    case 13:
        return "M";
    default: {
        char buffer[16]{};
        std::snprintf(buffer, sizeof(buffer), "#%u", preset);
        return buffer;
    }
    }
}

// DXGI format of the engine's Color, as a name when it is one we have actually
// seen (docs/D §2: R16G16B16A16_FLOAT) and as a number otherwise - the panel's job
// here is to show a change, not to catalogue every format.
std::string formatName(unsigned format) {
    switch (format) {
    case 2:
        return "R32G32B32A32_FLOAT";
    case 10:
        return "R16G16B16A16_FLOAT";
    case 11:
        return "R16G16B16A16_UNORM";
    case 24:
        return "R10G10B10A2_UNORM";
    case 28:
        return "R8G8B8A8_UNORM";
    case 87:
        return "B8G8R8A8_UNORM";
    case 91:
        return "B8G8R8A8_UNORM_SRGB";
    default:
        break;
    }
    char buffer[32]{};
    std::snprintf(buffer, sizeof(buffer), "DXGI %u", format);
    return buffer;
}

// Frame-time graph: the y axis follows the data instead of a fixed 0-50 ms, with
// a small smoothing so the scale does not flicker frame to frame.
float autoGraphMax(std::vector<float> const& values) {
    static float shown = 33.3f;

    float peak = 8.0f;
    for (float const value : values) peak = std::max(peak, value);
    float const target = peak * 1.2f;

    float const steps[] = {16.7f, 25.0f, 33.3f, 50.0f, 66.7f, 100.0f, 150.0f, 250.0f};
    float       bound   = steps[static_cast<int>(sizeof(steps) / sizeof(steps[0])) - 1];
    for (float const step : steps) {
        if (target <= step) {
            bound = step;
            break;
        }
    }
    shown += (bound - shown) * 0.2f;
    return shown;
}

// Layout --------------------------------------------------------------------

struct Layout {
    ImVec2 pos{};
    ImVec2 size{};
    float  scale{1.0f};
    float  pad{};
    float  headerHeight{};
    float  pipelineHeight{};
    float  tabsHeight{};
    float  footerHeight{};
    float  bodyTop{};
    float  bodyHeight{};
    float  leftWidth{};
    float  gutter{};
};

Layout computeLayout(ImVec2 pos, ImVec2 size) {
    Layout layout;
    layout.pos   = pos;
    layout.size  = size;
    layout.scale = theme::scale();

    float const s         = layout.scale;
    layout.pad            = 18.0f * s;
    layout.headerHeight   = 54.0f * s; // title strip, also the drag handle
    layout.pipelineHeight = 90.0f * s;
    layout.tabsHeight     = 34.0f * s;
    layout.footerHeight   = 50.0f * s;

    layout.bodyTop    = pos.y + layout.pad + layout.headerHeight + layout.pipelineHeight + layout.tabsHeight;
    layout.bodyHeight = pos.y + size.y - layout.pad - layout.footerHeight - layout.bodyTop;
    layout.bodyHeight = std::max(layout.bodyHeight, 70.0f * s);

    layout.gutter = 18.0f * s;
    // Fixed columns: the window is sized from them, and the stats column has to be
    // wide enough for "label ... value" on one line at this font size.
    layout.leftWidth = std::min(330.0f * s, size.x - layout.pad * 2.0f - 240.0f * s);
    return layout;
}

// Header (title strip; the whole strip is the drag handle) -------------------

void drawHeader(Layout const& layout, nr::PanelStats const& stats) {
    auto*       drawList = ImGui::GetWindowDrawList();
    auto const  s        = layout.scale;
    auto const& live     = config::get().nr;

    ImVec2 const titlePos{layout.pos.x + layout.pad, layout.pos.y + layout.pad * 0.42f};

    at(titlePos);
    ImGui::PushFont(theme::titleFont());
    ImGui::TextUnformatted("NeuroDragon");
    ImGui::PopFont();

    drawList->AddText(ImVec2(titlePos.x + 1.0f * s, titlePos.y + 26.0f * s), kTextFaint, "ui.subtitle"_tr().c_str());

    float const right = layout.pos.x + layout.size.x - layout.pad;

    char zoomText[32]{};
    std::snprintf(zoomText, sizeof(zoomText), "%d%%", static_cast<int>(std::lround(theme::userScale() * 100.0f)));

    float const button = 20.0f * s;
    float const gap    = 5.0f * s;

    std::string stateText;
    ImU32       stateColor = kGreen;
    if (!stats.featureCreated) {
        stateText  = "ui.state.notReady"_tr();
        stateColor = stats.lastError ? kRed : kAmber;
    } else if (!live.enabled) {
        stateText  = "ui.state.off"_tr();
        stateColor = kTextDim;
    } else {
        // Every reachable mode now composites (the "evaluate only" mode is gone), so a
        // created feature that is enabled is always on screen.
        stateText = "ui.state.healthy"_tr();
    }

    auto const  stateSize  = ImGui::CalcTextSize(stateText.c_str());
    auto const  stateWidth = stateSize.x + 20.0f * s;
    float const stateX     = right - stateWidth;
    float const dotRadius  = 4.0f * s;

    float const zoomWidth = ImGui::CalcTextSize(zoomText).x + 10.0f * s;
    float const zoomX     = stateX - 18.0f * s - zoomWidth - button * 2.0f - gap * 2.0f;
    float const zoomY     = layout.pos.y + layout.pad * 0.42f;

    // The label is a real ImGui item, so its rect is exactly where its glyphs are
    // centred; it is vertically centred inside the taller zoom buttons, and the dot
    // is placed on that same centre. Using the AddText position instead put the dot
    // on the line-box top, which is visibly above the text.
    at(ImVec2(stateX + dotRadius * 2.0f + 8.0f * s, zoomY + (button - stateSize.y) * 0.5f));
    ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(kText), "%s", stateText.c_str());
    {
        ImVec2 const itemMin = ImGui::GetItemRectMin();
        ImVec2 const itemMax = ImGui::GetItemRectMax();
        drawList->AddCircleFilled(ImVec2(stateX + dotRadius, (itemMin.y + itemMax.y) * 0.5f), dotRadius, stateColor);
    }

    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(4.0f * s, 1.0f * s));
    at(ImVec2(zoomX, zoomY));
    if (ImGui::Button("-##zoomOut", ImVec2(button, button))) theme::setUserScale(theme::userScale() - 0.05f);
    at(ImVec2(zoomX + button + gap, zoomY + 2.0f * s));
    ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(kTextDim), "%s", zoomText);
    at(ImVec2(zoomX + button + gap + zoomWidth + gap, zoomY));
    if (ImGui::Button("+##zoomIn", ImVec2(button, button))) theme::setUserScale(theme::userScale() + 0.05f);
    ImGui::PopStyleVar();

    float const ruleY = layout.pos.y + layout.pad + layout.headerHeight - 10.0f * s;
    drawList->AddLine(
        ImVec2(layout.pos.x + layout.pad, ruleY),
        ImVec2(right, ruleY),
        kBorderSoft,
        std::max(1.0f, 1.0f * s)
    );
}

// Pipeline strip ------------------------------------------------------------

void drawPipeline(Layout const& layout, nr::PanelStats const& stats) {
    auto const  s    = layout.scale;
    auto const& live = config::get().nr;

    float const top    = layout.pos.y + layout.pad + layout.headerHeight;
    float const height = layout.pipelineHeight - 18.0f * s;
    float const left   = layout.pos.x + layout.pad;
    float const width  = layout.size.x - layout.pad * 2.0f;

    constexpr int kArrows    = 3;
    float const   arrowWidth = 30.0f * s;
    float const   nodeWidth  = (width - static_cast<float>(kArrows) * arrowWidth) / 4.0f;

    char render[48]{};
    char output[48]{};
    std::snprintf(render, sizeof(render), "%u x %u", stats.renderWidth, stats.renderHeight);
    std::snprintf(output, sizeof(output), "%u x %u", stats.outputWidth, stats.outputHeight);

    char nrLine[64]{};
    if (!live.enabled) {
        std::snprintf(nrLine, sizeof(nrLine), "%s", "ui.pipeline.off"_tr().c_str());
    } else if (!stats.stampsActive) {
        std::snprintf(nrLine, sizeof(nrLine), "%s", "ui.diag.timingOff"_tr().c_str());
    } else if (stats.hasNr) {
        std::snprintf(nrLine, sizeof(nrLine), "%.2f ms", stats.nrMs);
    } else {
        std::snprintf(nrLine, sizeof(nrLine), "%s", "ui.pipeline.waiting"_tr().c_str());
    }

    char dlssLine[64]{};
    if (!stats.stampsActive) {
        std::snprintf(dlssLine, sizeof(dlssLine), "%s", "ui.diag.timingOff"_tr().c_str());
    } else if (stats.hasEngine) {
        std::snprintf(dlssLine, sizeof(dlssLine), "%.2f ms", stats.engineMs);
    } else {
        std::snprintf(dlssLine, sizeof(dlssLine), "%s", "ui.pipeline.waiting"_tr().c_str());
    }

    bool const nrActive   = live.enabled;
    // The upscaler is part of the engine and keeps running with NR off, so its node
    // lights up whenever its cost is actually being measured.
    bool const dlssActive = stats.stampsActive || stats.hasEngine;

    std::string const titles[4] = {
        "ui.pipeline.world"_tr(),
        "ui.pipeline.nr"_tr(),
        "ui.pipeline.upscale"_tr(),
        "ui.pipeline.output"_tr(),
    };
    std::string const nrTitle    = live.enabled ? "ui.pipeline.nrRunning"_tr() : "ui.pipeline.nrOff"_tr();
    std::string const outSub     = "ui.pipeline.outputSub"_tr();
    std::string const upscaleSub = "ui.pipeline.upscaleSub"_tr();
    std::string const rasterSub  = "ui.pipeline.raster"_tr();

    struct Node {
        char const* title{};
        char const* line1{};
        char const* line2{};
        bool        active{};
    };
    Node const nodes[4] = {
        {titles[0].c_str(), render,             rasterSub.c_str(), true      },
        {titles[1].c_str(), nrTitle.c_str(),    nrLine,            nrActive  },
        {titles[2].c_str(), upscaleSub.c_str(), dlssLine,          dlssActive},
        {titles[3].c_str(), output,             outSub.c_str(),    true      },
    };

    for (int index = 0; index < 4; ++index) {
        float const x = left + static_cast<float>(index) * (nodeWidth + arrowWidth);
        widgets::nodeBox(
            ImVec2(x, top),
            ImVec2(nodeWidth, height),
            nodes[index].title,
            nodes[index].line1,
            nodes[index].line2,
            nodes[index].active
        );

        if (index < 3) {
            float const fromX = x + nodeWidth + 5.0f * s;
            float const toX   = x + nodeWidth + arrowWidth - 2.0f * s;
            float const midY  = top + height * 0.5f;
            widgets::nodeArrow(ImVec2(fromX, midY), ImVec2(toX, midY), nodes[index + 1].active);
        }
    }
}

// Tabs ----------------------------------------------------------------------

void drawTabs(Layout const& layout) {
    float const y = layout.pos.y + layout.pad + layout.headerHeight + layout.pipelineHeight;

    std::string const tabs[4] = {
        "ui.tab.neural"_tr(),
        "ui.tab.sr"_tr(),
        "ui.tab.diagnostics"_tr(),
        "ui.tab.advanced"_tr(),
    };

    at(ImVec2(layout.pos.x + layout.pad, y));
    if (ImGui::BeginTabBar("##neurodragonTabs")) {
        for (int index = 0; index < 4; ++index) {
            ImGui::PushID(index);
            if (ImGui::BeginTabItem(tabs[index].c_str())) {
                gActiveTab = index;
                ImGui::EndTabItem();
            }
            ImGui::PopID();
        }
        ImGui::EndTabBar();
    }
}

// Stats column --------------------------------------------------------------

void drawStatsColumn(Layout const& layout, nr::PanelStats const& stats) {
    auto const  s       = layout.scale;
    auto const  frame   = Overlay::getInstance().frameStats();
    auto const  history = Overlay::getInstance().recentFrameMs();
    auto const& live    = config::get().nr;

    widgets::sectionHeader("ui.stats.frames"_tr().c_str());

    char buffer[64]{};
    std::snprintf(buffer, sizeof(buffer), "%.1f fps", frame.fps);
    widgets::statRow("ui.stats.fps"_tr().c_str(), buffer);
    std::snprintf(buffer, sizeof(buffer), "%.2f ms", frame.frameMs);
    widgets::statRow("ui.stats.frameTime"_tr().c_str(), buffer);

    ImVec2 const graphSize{ImGui::GetContentRegionAvail().x, 56.0f * s};
    auto const   graphPos = ImGui::GetCursorScreenPos();
    widgets::frameGraph(graphPos, graphSize, history, autoGraphMax(history));
    ImGui::Dummy(ImVec2(graphSize.x, graphSize.y + 4.0f * s));

    widgets::sectionHeader("ui.tab.neural"_tr().c_str());

    std::snprintf(buffer, sizeof(buffer), "%u x %u", stats.renderWidth, stats.renderHeight);
    widgets::statRow("ui.stats.renderSize"_tr().c_str(), buffer);
    std::snprintf(buffer, sizeof(buffer), "%u x %u", stats.outputWidth, stats.outputHeight);
    widgets::statRow("ui.stats.outputSize"_tr().c_str(), buffer);

    // "Off" is a deliberate state, not "not ready yet": with the feature unloaded the
    // sizes above still come from the engine, so only the NR-specific rows change.
    std::string const statusText = !live.enabled ? "ui.stats.disabled"_tr()
                                  : stats.featureCreated ? "ui.stats.created"_tr()
                                                         : "ui.stats.notCreated"_tr();
    ImU32 const statusColor = !live.enabled ? kTextDim : (stats.featureCreated ? kGreen : kRed);
    widgets::statRow("ui.stats.status"_tr().c_str(), statusText.c_str(), statusColor);
    widgets::statRow("ui.stats.replace"_tr().c_str(), replaceName(live.replace).c_str());

    if (!stats.stampsActive) {
        widgets::statRow("ui.stats.nrGpu"_tr().c_str(), "ui.diag.timingOff"_tr().c_str(), kTextDim);
        widgets::statRow("ui.stats.dlssGpu"_tr().c_str(), "ui.diag.timingOff"_tr().c_str(), kTextDim);
    } else {
        if (!live.enabled) {
            widgets::statRow("ui.stats.nrGpu"_tr().c_str(), "ui.stats.disabled"_tr().c_str(), kTextDim);
        } else if (stats.hasNr) {
            std::snprintf(buffer, sizeof(buffer), "%.2f ms", stats.nrMs);
            widgets::statRow("ui.stats.nrGpu"_tr().c_str(), buffer, kAccent);
        } else {
            widgets::statRow("ui.stats.nrGpu"_tr().c_str(), "ui.stats.noSamples"_tr().c_str(), kTextDim);
        }
        if (stats.hasEngine) {
            std::snprintf(buffer, sizeof(buffer), "%.2f ms", stats.engineMs);
            widgets::statRow("ui.stats.dlssGpu"_tr().c_str(), buffer);
        } else {
            widgets::statRow("ui.stats.dlssGpu"_tr().c_str(), "ui.stats.noSamples"_tr().c_str(), kTextDim);
        }
    }
}

// Settings columns ----------------------------------------------------------

void drawTabNeural() {
    widgets::sectionHeader("ui.neural.enableSection"_tr().c_str());

    if (widgets::toggle("ui.neural.enable"_tr().c_str(), &gDraft.enabled)) markDirty();
    if (!gDraft.enabled) widgets::hint("ui.neural.disabledHint"_tr().c_str());

    if (!gDraft.enabled) ImGui::BeginDisabled();

    // Position axis (docs/N): the two sides are exclusive. The hint carries the cost
    // difference so nobody mistakes the after side's ~9.6 ms for a regression.
    widgets::sectionHeader("ui.neural.placementSection"_tr().c_str());
    gDraft.placement = gDraft.placement == 1 ? 1 : 0;
    {
        std::string const placementItems[2] = {
            "ui.neural.placementBefore"_tr(),
            "ui.neural.placementAfter"_tr(),
        };
        char const* itemPointers[2]  = {placementItems[0].c_str(), placementItems[1].c_str()};
        int const   placementValues[2] = {0, 1};
        if (mappedCombo(
                "ui.neural.placement"_tr().c_str(),
                &gDraft.placement,
                itemPointers,
                placementValues,
                2
            )) {
            markDirty();
        }
    }
    widgets::hint(
        gDraft.placement == 1 ? "ui.neural.placementAfterHint"_tr().c_str() : "ui.neural.placementHint"_tr().c_str()
    );
    if (gDraft.placement == 1 && nr::nrAfterFallback()) {
        widgets::hint("ui.neural.afterFallbackHint"_tr().c_str());
    }

    // Only the two on-screen composites are offered: "evaluate only" (0) is a
    // diagnostic mode with no entrance any more (docs/L), and the config loader maps a
    // stale 0 to 6. The panel is a staging area, so a draft can never be 0 either.
    std::string const replaceItems[2] = {
        "ui.replace.item6"_tr(),
        "ui.replace.item5"_tr(),
    };
    char const* itemPointers[2]  = {replaceItems[0].c_str(), replaceItems[1].c_str()};
    int const   replaceValues[2] = {6, 5};
    if (gDraft.replace != 5 && gDraft.replace != 6) gDraft.replace = 6;
    if (mappedCombo("ui.neural.replace"_tr().c_str(), &gDraft.replace, itemPointers, replaceValues, 2)) markDirty();
    widgets::hint(replaceHint(gDraft.replace).c_str());

    // The gain boost only exists in the bounded-gain composite; hiding it in the ratio
    // mode keeps the default screen free of knobs that do nothing.
    if (gDraft.replace == 5) {
        if (widgets::slider("ui.neural.gain"_tr().c_str(), &gDraft.gainBoost, 0.5f, 8.0f, "%.2f", "x")) markDirty();
    }

    if (!gDraft.enabled) ImGui::EndDisabled();

    ImGui::Spacing();
    widgets::sectionHeader("ui.style.section"_tr().c_str());

    // A dropdown, not a slider: three discrete styles and no meaningful order.
    static char const* const kStyleItems[]  = {"0", "1", "2"};
    static int const         kStyleValues[] = {0, 1, 2};
    if (mappedCombo("ui.style.style"_tr().c_str(), &gDraft.style, kStyleItems, kStyleValues, 3)) markDirty();
    if (widgets::slider("ui.style.intensity"_tr().c_str(), &gDraft.intensity, 0.0f, 2.0f, "%.2f")) markDirty();
    if (widgets::slider("ui.style.localTone"_tr().c_str(), &gDraft.localToneStrength, 0.0f, 2.0f, "%.2f")) markDirty();
    if (widgets::slider("ui.style.localStructure"_tr().c_str(), &gDraft.localStructureStrength, 0.0f, 2.0f, "%.2f")) {
        markDirty();
    }
#ifdef TEST
    // Probe only: no visible difference was measurable at -1/0/1 on Minecraft content
    // (docs/L P1), and TRP ties this knob to its automatic skin mask, which needs skin
    // classification our inputs cannot carry. A release build does not offer a knob
    // that does nothing; the parameter keeps its old default of 1.0.
    if (widgets::slider("ui.style.skin"_tr().c_str(), &gDraft.skinStructure, -1.0f, 2.0f, "%.2f")) markDirty();
    widgets::hint("ui.style.skinHint"_tr().c_str());
#endif

    ImGui::Spacing();
    widgets::sectionHeader("ui.neural.input"_tr().c_str());

    std::string const depthItems[3] = {
        "ui.neural.depthFollow"_tr(),
        "ui.neural.depthNormal"_tr(),
        "ui.neural.depthInverted"_tr(),
    };
    char const* depthPointers[3] = {depthItems[0].c_str(), depthItems[1].c_str(), depthItems[2].c_str()};
    int const   depthValues[3]   = {-1, 0, 1};
    if (mappedCombo("ui.neural.depth"_tr().c_str(), &gDraft.depthInverted, depthPointers, depthValues, 3)) markDirty();

    bool autoMvec = gDraft.mvecScale < 0.0f;
    if (widgets::toggle("ui.neural.autoMv"_tr().c_str(), &autoMvec)) {
        gDraft.mvecScale = autoMvec ? -1.0f : 0.0f;
        markDirty();
    }
    widgets::hint("ui.neural.mvHint"_tr().c_str());
    if (!autoMvec) {
        if (widgets::slider("ui.neural.mv"_tr().c_str(), &gDraft.mvecScale, 1.0f, 2048.0f, "%.0f")) markDirty();
    }

    if (gDraft.placement == 0) {
        ImGui::Spacing();
        widgets::sectionHeader("ui.neural.scale"_tr().c_str());
        // Only the ratios that keep the feature inside the engine's native slot sizes and
        // are worth the image cost (docs/L 2.1); a refused percentage falls back to 100%
        // with a log line, so the panel cannot leave NR broken.
        std::string const scaleItems[3] = {
            "ui.neural.scale100"_tr(),
            "ui.neural.scale66"_tr(),
            "ui.neural.scale50"_tr(),
        };
        char const* scalePointers[3] = {scaleItems[0].c_str(), scaleItems[1].c_str(), scaleItems[2].c_str()};
        int const   scaleValues[3]    = {100, 66, 50};
        if (mappedCombo("ui.neural.scalePercent"_tr().c_str(), &gDraft.inputPercent, scalePointers, scaleValues, 3)) {
            markDirty();
        }
        widgets::hint("ui.neural.scaleHint"_tr().c_str());
    } else {
        widgets::hint("ui.neural.scaleAfterHint"_tr().c_str());
    }

    // Runtime picker (docs/L 3.1): the build in the mod directory that is materialised
    // next to the game executable at load. Read at load, so it says "restart".
    ImGui::Spacing();
    widgets::sectionHeader("ui.neural.model"_tr().c_str());
    {
        std::vector<std::string> items;
        std::vector<std::string> values;
        items.push_back("ui.neural.modelDefault"_tr());
        values.emplace_back();
        for (auto const& name : gModelFiles) {
            items.push_back(name);
            values.push_back(name);
        }
        int index = 0;
        for (std::size_t i = 0; i < values.size(); ++i) {
            if (values[i] == gDraft.modelFile) {
                index = static_cast<int>(i);
                break;
            }
        }
        std::vector<char const*> pointers;
        pointers.reserve(items.size());
        for (auto const& item : items) {
            pointers.push_back(item.c_str());
        }
        if (widgets::combo(
                "ui.neural.modelFile"_tr().c_str(),
                &index,
                pointers.data(),
                static_cast<int>(pointers.size())
            )) {
            gDraft.modelFile = values[static_cast<std::size_t>(index)];
            markDirty();
        }
        widgets::hint("ui.neural.modelHint"_tr().c_str());
    }

    ImGui::Spacing();
    widgets::sectionHeader("ui.neural.encode"_tr().c_str());

    std::string const encodeItems[3] = {
        "ui.neural.encodeAuto"_tr(),
        "ui.neural.encodeReinhard"_tr(),
        "ui.neural.encodeTrp"_tr(),
    };
    char const* encodePointers[3] = {encodeItems[0].c_str(), encodeItems[1].c_str(), encodeItems[2].c_str()};
    int const   encodeValues[3]   = {-1, 0, 3};
    if (mappedCombo("ui.neural.encode"_tr().c_str(), &gDraft.nrEncode, encodePointers, encodeValues, 3)) markDirty();
    widgets::hint("ui.neural.encodeHint"_tr().c_str());

    // Network preset is read at CreateFeature only, so the hint has to say that a
    // change needs a restart - offering a switch that silently does nothing is the
    // class of bug this batch exists to remove (docs/L 1.4).
    std::string const networkItems[2] = {
        "ui.neural.networkDefault"_tr(),
        "ui.neural.networkPrevious"_tr(),
    };
    char const* networkPointers[2] = {networkItems[0].c_str(), networkItems[1].c_str()};
    int const   networkValues[2]   = {0, 1};
    if (mappedCombo("ui.neural.network"_tr().c_str(), &gDraft.networkPreset, networkPointers, networkValues, 2)) {
        markDirty();
    }
    widgets::hint("ui.neural.networkHint"_tr().c_str());

    ImGui::Spacing();
    widgets::sectionHeader("ui.neural.ratio"_tr().c_str());
    if (widgets::slider("ui.neural.white"_tr().c_str(), &gDraft.whitePoint, 0.05f, 4.0f, "%.3f")) markDirty();
    if (widgets::slider("ui.neural.transfer"_tr().c_str(), &gDraft.transferStrength, 0.0f, 2.0f, "%.2f")) markDirty();
    if (widgets::slider("ui.neural.colour"_tr().c_str(), &gDraft.colourStrength, 0.0f, 2.0f, "%.2f")) markDirty();
    if (widgets::slider("ui.neural.maxRatio"_tr().c_str(), &gDraft.maxRatio, 1.0f, 8.0f, "%.2f")) markDirty();
    // TRP's broad/detail split of NR's change: 1.0 / 1.0 leaves the result unchanged.
    if (widgets::slider("ui.neural.broad"_tr().c_str(), &gDraft.broad, 0.0f, 2.0f, "%.2f")) markDirty();
    if (widgets::slider("ui.neural.detail"_tr().c_str(), &gDraft.detail, 0.0f, 2.0f, "%.2f")) markDirty();
    widgets::hint("ui.neural.toneHint"_tr().c_str());

    // Adaptive exposure (docs/L 3.2): the white point comes from the scene instead of
    // the constant above. Only Replace=6 normalises against it in the resolve; both
    // encoders read it.
    if (widgets::toggle("ui.neural.adaptiveExposure"_tr().c_str(), &gDraft.adaptiveExposure)) markDirty();
    if (gDraft.adaptiveExposure) {
        if (widgets::slider("ui.neural.exposureBias"_tr().c_str(), &gDraft.exposureBias, -2.0f, 2.0f, "%+.2f", "EV")) {
            markDirty();
        }
    }
    widgets::hint("ui.neural.exposureHint"_tr().c_str());

    ImGui::Spacing();
    widgets::sectionHeader("ui.neural.compare"_tr().c_str());
    bool compare = gDraft.compare != 0;
    if (widgets::toggle("ui.neural.compareToggle"_tr().c_str(), &compare)) {
        gDraft.compare = compare ? 1 : 0;
        markDirty();
    }
    if (gDraft.compare != 0) {
        if (widgets::slider("ui.neural.compareSplit"_tr().c_str(), &gDraft.compareSplit, 0.05f, 0.95f, "%.2f")) {
            markDirty();
        }
    }
    widgets::hint("ui.neural.compareHint"_tr().c_str());
}

void drawTabSr() {
    auto const sr = nr::srPanelStats();
    auto const ps = nr::nrPanelStats();

    char buffer[128]{};

    // The old "engine (read only)" block lived here. It duplicated the measured size
    // chain, flashed the temporary quality slot for a few frames after every forced
    // rebuild, and mixed "what we asked for" with "what the engine did". What survives
    // is only the part that verifies a write (docs/L §0.7): the size chain next to the
    // control that moves it, plus the quality slot and preset the engine last created
    // its feature with - both blanked while a rebuild is in flight, because the
    // temporary slot is not a result (docs/J §8).
    widgets::sectionHeader("ui.sr.internal"_tr().c_str());
    // Only the ratios DLSS actually has a slot for: each one maps 1:1 to the
    // engine's own quality slot, so the DLSS size is the engine's exact answer and
    // the scene factor table is patched to the same ratio. "Follow" leaves both
    // alone and restores the table.
    std::string const scaleItems[6] = {
        "ui.sr.scaleAuto"_tr(),
        "33%",
        "50%",
        "58%",
        "66%",
        "100%",
    };
    char const* scalePointers[6] = {
        scaleItems[0].c_str(),
        scaleItems[1].c_str(),
        scaleItems[2].c_str(),
        scaleItems[3].c_str(),
        scaleItems[4].c_str(),
        scaleItems[5].c_str(),
    };
    int const scaleValues[6] = {0, 33, 50, 58, 66, 100};
    if (mappedCombo("ui.sr.scale"_tr().c_str(), &gDraftSr.scalePercent, scalePointers, scaleValues, 6)) {
        markDirty();
    }
    widgets::hint("ui.sr.scaleHint"_tr().c_str());
    // What the two writes actually produced: scene render rect -> engine output.
    // This is the pair that proves the override landed (docs/J §2), so it belongs
    // right under the control that moves it - not in a separate read-only block.
    if (ps.renderWidth != 0 && ps.outputWidth != 0) {
        std::snprintf(
            buffer,
            sizeof(buffer),
            "%u x %u → %u x %u（%.1f%%）",
            ps.renderWidth,
            ps.renderHeight,
            ps.outputWidth,
            ps.outputHeight,
            100.0 * static_cast<double>(ps.renderWidth) / static_cast<double>(ps.outputWidth)
        );
        widgets::statRow("ui.sr.currentSize"_tr().c_str(), buffer);
    }
    {
        // Read back from the engine's CreateFeature parameter object, not from the
        // draft: this is how a "the preset did not take" report is distinguished from
        // "the panel did not save it" without reading the log.
        std::string const slot =
            sr.rebuilding ? "ui.sr.rebuilding"_tr() : qualityName(sr.quality);
        widgets::statRow("ui.sr.quality"_tr().c_str(), slot.c_str(), sr.rebuilding ? kTextDim : 0);
    }

    ImGui::Spacing();
    widgets::sectionHeader("ui.sr.model"_tr().c_str());
    std::string const modelItems[7] = {
        "ui.sr.presetAuto"_tr(),
        "ui.sr.presetE"_tr(),
        "ui.sr.presetF"_tr(),
        "ui.sr.presetJ"_tr(),
        "ui.sr.presetK"_tr(),
        "ui.sr.presetL"_tr(),
        "ui.sr.presetM"_tr(),
    };
    char const* modelPointers[7] = {
        modelItems[0].c_str(),
        modelItems[1].c_str(),
        modelItems[2].c_str(),
        modelItems[3].c_str(),
        modelItems[4].c_str(),
        modelItems[5].c_str(),
        modelItems[6].c_str(),
    };
    int const modelValues[7] = {0, 5, 6, 10, 11, 12, 13};
    if (mappedCombo("ui.sr.model"_tr().c_str(), &gDraftSr.preset, modelPointers, modelValues, 7)) {
        markDirty();
    }
    widgets::hint("ui.sr.modelHint"_tr().c_str());
    {
        std::string const preset = sr.rebuilding ? "ui.sr.rebuilding"_tr() : presetName(sr.preset);
        widgets::statRow("ui.sr.preset"_tr().c_str(), preset.c_str(), sr.rebuilding ? kTextDim : 0);
    }

    ImGui::Spacing();
    widgets::sectionHeader("ui.sr.sharpSection"_tr().c_str());
    // The DLSS "Sharpness" parameter is dead (docs/J §5), so sharpening is our own
    // RCAS pass after the engine's upscale. Live: the render thread reads the config
    // every evaluation, so Apply is enough - no feature rebuild, no restart.
    {
        auto const sharp = neurodragon::sr::sharpStats();
        if (widgets::toggle("ui.sr.sharpEnable"_tr().c_str(), &gDraftSr.sharpening)) {
            markDirty();
        }
        if (!gDraftSr.sharpening) {
            ImGui::BeginDisabled();
        }
        if (widgets::slider("ui.sr.sharpStrength"_tr().c_str(), &gDraftSr.sharpness, 0.0f, 1.0f, "%.3f")) {
            markDirty();
        }
        if (!gDraftSr.sharpening) {
            ImGui::EndDisabled();
        }
        widgets::hint("ui.sr.sharpHint"_tr().c_str());

        // Read-back (docs/L §0.7): what the render thread actually recorded, not
        // what the draft says. "Applied" is dim until a frame has gone through.
        if (sharp.enabled && sharp.strength > 0.0f) {
            std::snprintf(buffer, sizeof(buffer), "%.3f", static_cast<double>(sharp.strength));
            widgets::statRow("ui.sr.sharpApplied"_tr().c_str(), buffer, sharp.active ? kAccent : kTextDim);
            std::snprintf(buffer, sizeof(buffer), "%u", sharp.frames);
            widgets::statRow("ui.sr.sharpFrames"_tr().c_str(), buffer);
            if (ps.hasRcas) {
                std::snprintf(buffer, sizeof(buffer), "%.3f ms", ps.rcasMs);
                widgets::statRow("ui.sr.sharpGpu"_tr().c_str(), buffer);
            } else {
                widgets::statRow("ui.sr.sharpGpu"_tr().c_str(), "ui.stats.noSamples"_tr().c_str(), kTextDim);
            }
            if (sharp.failures != 0) {
                std::snprintf(buffer, sizeof(buffer), "%u", sharp.failures);
                widgets::statRow("ui.sr.sharpFailures"_tr().c_str(), buffer, kRed);
            }
        } else {
            widgets::hint(sharp.enabled ? "ui.sr.sharpZeroHint"_tr().c_str() : "ui.sr.sharpOffHint"_tr().c_str());
        }
    }

    ImGui::Spacing();
    widgets::sectionHeader("ui.sr.ctx"_tr().c_str());
    auto const eng = nr::srEngineStats();
    if (eng.valid) {
        std::snprintf(
            buffer,
            sizeof(buffer),
            "%u / %.3f",
            eng.ctxQuality,
            static_cast<double>(eng.ctxFactor)
        );
        widgets::statRow("ui.sr.ctxCtx"_tr().c_str(), buffer);
        std::snprintf(
            buffer,
            sizeof(buffer),
            "%u / %.3f",
            eng.requestedQ,
            static_cast<double>(eng.requestedF)
        );
        widgets::statRow("ui.sr.ctxEngine"_tr().c_str(), buffer);
        std::snprintf(buffer, sizeof(buffer), "%u x %u", eng.renderW, eng.renderH);
        widgets::statRow("ui.sr.ctxRender"_tr().c_str(), buffer);
        std::snprintf(buffer, sizeof(buffer), "%.3f", static_cast<double>(eng.ctxExtra));
        widgets::statRow("ui.sr.ctxExtra"_tr().c_str(), buffer);
    } else {
        widgets::hint("ui.sr.ctxNone"_tr().c_str());
    }

    ImGui::Spacing();
    widgets::sectionHeader("ui.sr.callback"_tr().c_str());

    std::snprintf(buffer, sizeof(buffer), "%u", sr.callbackCount);
    widgets::statRow("ui.sr.cbCount"_tr().c_str(), buffer);
    if (sr.callbackCount != 0) {
        std::snprintf(buffer, sizeof(buffer), "%u x %u", sr.cbDisplayW, sr.cbDisplayH);
        widgets::statRow("ui.sr.cbDisplay"_tr().c_str(), buffer);
        std::snprintf(buffer, sizeof(buffer), "%u x %u", sr.cbRenderW, sr.cbRenderH);
        widgets::statRow("ui.sr.cbRender"_tr().c_str(), buffer);
        std::snprintf(buffer, sizeof(buffer), "%.2f", static_cast<double>(sr.cbSharpness));
        widgets::statRow("ui.sr.cbSharpness"_tr().c_str(), buffer);
        if (sr.dynMinW != 0) {
            std::snprintf(
                buffer,
                sizeof(buffer),
                "%u x %u .. %u x %u",
                sr.dynMinW,
                sr.dynMinH,
                sr.dynMaxW,
                sr.dynMaxH
            );
            widgets::statRow("ui.sr.dynamic"_tr().c_str(), buffer);
        }
    } else {
        widgets::hint("ui.sr.cbNone"_tr().c_str());
    }
    // Create flags (IsHDR|MVLowRes = 0x3) come from the same parameter object as the
    // optimal-settings answer, so they live here rather than in a separate block.
    std::snprintf(buffer, sizeof(buffer), "0x%08X", sr.flags);
    widgets::statRow("ui.sr.flags"_tr().c_str(), buffer);
    widgets::hint("ui.sr.cbHint"_tr().c_str());

    // Self-check + telemetry (docs/I §7 risk register, docs/K §2.2 ③). The four
    // check rows answer "is SR wired up at all" after a game update; the rows below
    // answer "is the engine actually running it" without reading the log.
    ImGui::Spacing();
    widgets::sectionHeader("ui.sr.selfCheck"_tr().c_str());
    auto const checkRow = [](char const* label, bool ok) {
        widgets::statRow(
            label,
            ok ? "ui.sr.checkOk"_tr().c_str() : "ui.sr.checkFailed"_tr().c_str(),
            ok ? 0 : kRed
        );
    };
    checkRow("ui.sr.checkCallback"_tr().c_str(), sr.callbackWrapped);
    checkRow("ui.sr.checkFrameCtx"_tr().c_str(), sr.frameContextHook);
    checkRow("ui.sr.checkFactorTable"_tr().c_str(), sr.factorTable);
    checkRow("ui.sr.checkCreate"_tr().c_str(), sr.createOverridesRan);
    widgets::hint("ui.sr.checkHint"_tr().c_str());

    std::snprintf(buffer, sizeof(buffer), "%u", sr.evalCount);
    widgets::statRow("ui.sr.evalCount"_tr().c_str(), buffer);
    std::snprintf(buffer, sizeof(buffer), "%u", sr.evalFailures);
    widgets::statRow("ui.sr.evalFailures"_tr().c_str(), buffer, sr.evalFailures != 0 ? kRed : 0);
    widgets::statRow(
        "ui.sr.inputFormat"_tr().c_str(),
        sr.inputFormatValid ? formatName(sr.inputFormat).c_str() : "-"
    );
    widgets::statRow(
        "ui.sr.inputHdr"_tr().c_str(),
        sr.flags == 0 ? "-" : ((sr.flags & 1u) != 0u ? "HDR" : "SDR")
    );
    std::snprintf(
        buffer,
        sizeof(buffer),
        "%.2f / %.2f",
        static_cast<double>(sr.jitterX),
        static_cast<double>(sr.jitterY)
    );
    widgets::statRow("ui.sr.jitter"_tr().c_str(), buffer);

    ImGui::Spacing();
    widgets::sectionHeader("ui.sr.actions"_tr().c_str());
    std::string resetLabel = "ui.sr.reset"_tr();
    resetLabel += "##srReset";
    if (ImGui::Button(resetLabel.c_str())) {
        nr::srRequestReset();
    }
    widgets::hint("ui.sr.resetHint"_tr().c_str());
}

// ---------------------------------------------------------------------------
// VV injection experiment (docs/specs/vv.md §12 A)
//
// The engine submits a whole frame in one ExecuteCommandLists call; the probe can
// cut it at a list boundary and clear the back buffer in between. The panel only
// picks the boundary - everything else happens on the render thread - and the
// answers are read off the screen, not out of a number:
//
//   clear colour + UI still drawn  -> the cut is before the UI
//   the whole screen is clear colour -> the cut is after the UI
//   nothing changed                -> a later list overwrote the buffer (too early)
// ---------------------------------------------------------------------------

std::string skipReasonText(probe::vv::inject::SkipReason reason) {
    switch (reason) {
    case probe::vv::inject::SkipReason::NoDevice:
        return "ui.diag.vvReasonDevice"_tr();
    case probe::vv::inject::SkipReason::NoSwapChain:
        return "ui.diag.vvReasonChain"_tr();
    case probe::vv::inject::SkipReason::FenceBusy:
        return "ui.diag.vvReasonFence"_tr();
    case probe::vv::inject::SkipReason::NoBackBuffer:
        return "ui.diag.vvReasonBuffer"_tr();
    case probe::vv::inject::SkipReason::PipelineFailed:
        return "ui.diag.vvReasonPipeline"_tr();
    case probe::vv::inject::SkipReason::RecordFailed:
        return "ui.diag.vvReasonRecord"_tr();
    case probe::vv::inject::SkipReason::NoBoundary:
        return "no pre-HUD boundary; TAAU restored";
    default:
        return {};
    }
}

#ifdef TEST
// B0 probe controls (docs/specs/vv.md §13.4). TEST-only and deliberately not
// translated: this is a developer switch, not a user feature. The delay lets the
// player click, close the panel and start walking before the 24 frames begin.
void drawB0Probe() {
    widgets::sectionHeader("VV B0 (TEST)");
    widgets::hint("Press F11 once after entering the world so the engine rebuilds its render graph.");
    if (ImGui::Button("Dump now (stand still, then F2)")) {
        probe::vv::requestDumpNow();
    }
    ImGui::SameLine();
    if (ImGui::Button("Sample 24 frames in 2 s (then walk)")) {
        probe::vv::requestContextSample(24, 2000);
    }

    // B1a: self-built NGX session probe. Creates and releases DLSS feature 1 on
    // the game's device; it does not evaluate and does not touch a frame.
    widgets::sectionHeader("VV DLSS B1a (TEST)");
    if (ImGui::Button("Probe self-init NGX + CreateFeature(1)")) {
        vv::dlss::requestProbe();
    }
    auto const ngx = vv::dlss::probeState();
    char       buffer[256]{};
    std::snprintf(buffer, sizeof(buffer), "%s", ngx.done ? "done" : "idle");
    widgets::statRow("B1a state", buffer);
    std::snprintf(
        buffer,
        sizeof(buffer),
        "init=0x%08X caps=0x%08X create=0x%08X",
        static_cast<unsigned>(ngx.initRet),
        static_cast<unsigned>(ngx.capsRet),
        static_cast<unsigned>(ngx.createRet)
    );
    widgets::statRow("B1a NGX returns", buffer);
    std::snprintf(
        buffer,
        sizeof(buffer),
        "%ux%u -> %ux%u  core=%d session=%d",
        ngx.renderWidth,
        ngx.renderHeight,
        ngx.displayWidth,
        ngx.displayHeight,
        ngx.coreLoaded ? 1 : 0,
        ngx.initialized ? 1 : 0
    );
    widgets::statRow("B1a sizes", buffer);

    // B1c: cut capture. One pass over tail k = 1..8, one picture per boundary, no
    // pixel changed. The pictures answer "what does each of the last lists draw"
    // (the first one that shows the HUD is the UI block) and give the offline tool
    // TAAU's own output as the reference for colour / tone fitting.
    widgets::sectionHeader("VV B1c cut capture (TEST)");
    if (ImGui::Button("Capture at content boundary (1 frame)")) {
        // The self-check for the new anchor: with the boundary taken from the first
        // back-buffer writer, the picture must never contain the HUD or the hand -
        // whatever the frame's list count is.
        vv::capture::start();
        probe::vv::inject::setAction(probe::vv::inject::Action::Capture);
        probe::vv::inject::setUseBackBuffer(true);
        // If the probe has not seen a writer yet, fall back to the last measured
        // boundary instead of capturing the top of the frame (k=1 is useless there).
        probe::vv::inject::setFromTail(true);
        probe::vv::inject::setK(4);
        probe::vv::inject::setMode(probe::vv::inject::Mode::Off);
        probe::vv::inject::setMode(probe::vv::inject::Mode::Fixed);
    }
    ImGui::SameLine();
    if (ImGui::Button("Capture sweep: tail k=1..8 (one pass)")) {
        vv::capture::start();
        probe::vv::inject::setAction(probe::vv::inject::Action::Capture);
        probe::vv::inject::setUseBackBuffer(false);
        probe::vv::inject::setFromTail(true);
        // One frame per boundary: the capture is the only action, so a "hold" of one
        // frame gives exactly one picture per tail k instead of three identical ones.
        probe::vv::inject::setHoldFrames(1);
        probe::vv::inject::setSweepDepth(8);
        probe::vv::inject::setK(1);
        probe::vv::inject::setMode(probe::vv::inject::Mode::Off);
        probe::vv::inject::setMode(probe::vv::inject::Mode::Sweep);
    }
    ImGui::SameLine();
    if (ImGui::Button("Stop capture")) {
        probe::vv::inject::setMode(probe::vv::inject::Mode::Off);
        probe::vv::inject::setAction(probe::vv::inject::Action::Clear);
    }
    widgets::hint("Stand still. After a capture, press \"Dump now\" (B0) at once: the pictures and the");
    widgets::hint("candidates must describe the same frame. F11 first, or the dump finds no textures.");
    auto const cut = vv::capture::stats();
    std::snprintf(
        buffer,
        sizeof(buffer),
        "captured=%u skipped=%u pending=%u",
        cut.captured,
        cut.skipped,
        cut.pending
    );
    widgets::statRow("B1c frames", buffer);
    std::snprintf(
        buffer,
        sizeof(buffer),
        "tail=%u split=%u/%u  %ux%u",
        cut.tail,
        cut.split,
        cut.listCount,
        cut.width,
        cut.height
    );
    widgets::statRow("B1c last cut", buffer);
    if (cut.lastFile[0]) {
        int const needed = ::WideCharToMultiByte(CP_UTF8, 0, cut.lastFile, -1, buffer, sizeof(buffer), nullptr, nullptr);
        widgets::statRow("B1c last file", needed > 0 ? buffer : "?");
    }

    // B2: the SR pipeline's own buffers. Same boundary as the bypass, but the world
    // colour and depth are first copied into our render-resolution textures; the
    // final pass encodes and writes the result. The depth / motion buttons are the
    // visual check that those inputs are sane before DLSS is wired in.
    widgets::sectionHeader("VV B2 SR (TEST)");
    auto const startSr = [&](vv::sr::View view) {
        probe::vv::inject::setMode(probe::vv::inject::Mode::Fixed);
        probe::vv::inject::setFromTail(true);
        probe::vv::inject::setK(4);
        probe::vv::inject::setAction(probe::vv::inject::Action::Sr);
        probe::vv::inject::setUseBackBuffer(true);
        vv::sr::setView(view);
    };
    // One row per pair: seven buttons on one line ran off the panel's right edge, so
    // the last ones (Next SR depth / Dump SR inputs) were invisible (2026-10-25).
    if (ImGui::Button("SR colour bypass")) {
        startSr(vv::sr::View::Bypass);
    }
    ImGui::SameLine();
    if (ImGui::Button("SR depth view")) {
        startSr(vv::sr::View::Depth);
    }
    if (ImGui::Button("SR raw colour (no encode)")) {
        startSr(vv::sr::View::Raw);
    }
    if (ImGui::Button("SR + DLSS")) {
        startSr(vv::sr::View::Dlss);
    }
    if (ImGui::Button("SR motion view")) {
        startSr(vv::sr::View::Motion);
    }
    ImGui::SameLine();
    if (ImGui::Button("Stop SR")) {
        probe::vv::inject::setMode(probe::vv::inject::Mode::Off);
        probe::vv::inject::setAction(probe::vv::inject::Action::Clear);
    }
    if (ImGui::Button("Next SR colour")) {
        vv::sr::cycleColor();
    }
    ImGui::SameLine();
    if (ImGui::Button("Next SR depth")) {
        vv::sr::cycleDepth();
    }
    if (ImGui::Button("Dump SR inputs")) {
        vv::sr::dumpInputs();
    }
    // TEST: the hotkey path, so a pair can be taken while the panel is closed and the
    // player keeps turning the camera (EXP-0027).
    widgets::hint("F7 (TEST): two input dumps 2 s apart - keep turning; the menu may stay closed.");
    auto const sr = vv::sr::stats();
    std::snprintf(
        buffer,
        sizeof(buffer),
        "%s  view=%d  %ux%u -> %ux%u",
        sr.pipelineReady ? "ready" : "not built",
        sr.view,
        sr.renderWidth,
        sr.renderHeight,
        sr.displayWidth,
        sr.displayHeight
    );
    widgets::statRow("B2 pipeline", buffer);
    std::snprintf(
        buffer,
        sizeof(buffer),
        "feature=%d create=0x%08X eval=0x%08X",
        sr.dlssReady ? 1 : 0,
        static_cast<unsigned>(sr.dlssCreateRet),
        static_cast<unsigned>(sr.dlssEvaluateRet)
    );
    widgets::statRow("B2 DLSS", buffer);
    auto const taau = probe::vv::taauDrawStats();
    std::snprintf(
        buffer,
        sizeof(buffer),
        "armed=%d declared=%llu callbacks=%llu forwarded=%llu suppressed=%llu missed=%llu",
        probe::vv::taauDrawReplacementArmed() ? 1 : 0,
        static_cast<unsigned long long>(taau.declared),
        static_cast<unsigned long long>(taau.callbacks),
        static_cast<unsigned long long>(taau.forwarded),
        static_cast<unsigned long long>(taau.suppressed),
        static_cast<unsigned long long>(taau.missed)
    );
    widgets::statRow("TAAU replacement", buffer);
    std::snprintf(
        buffer,
        sizeof(buffer),
        "colour=%p/%u (%d/%u)  depth=%p/%u (%d/%u)  rec=%d",
        sr.colorSource,
        sr.colorFormat,
        static_cast<int>(sr.colorChoice) % (sr.colorCandidates ? static_cast<int>(sr.colorCandidates) : 1),
        sr.colorCandidates,
        sr.depthSource,
        sr.depthFormat,
        static_cast<int>(sr.depthChoice) % (sr.depthCandidates ? static_cast<int>(sr.depthCandidates) : 1),
        sr.depthCandidates,
        sr.recorded ? 1 : 0
    );
    widgets::statRow("B2 inputs", buffer);
    if (sr.lastError) {
        std::snprintf(buffer, sizeof(buffer), "0x%08X", static_cast<unsigned>(sr.lastError));
        widgets::statRow("B2 last error", buffer);
    }

    // R3 stage A (docs/specs/vv.md §35.1): write magenta into the engine's TAAU
    // output (arg10) after the list that wrote it. TAAU still runs; only the output
    // is replaced, and a missing writer stays a skip.
    widgets::sectionHeader("VV R3 stage A (TEST)");
    if (ImGui::Button(probe::vv::arg10SolidArmed() ? "Disarm arg10 solid" : "Write solid arg10")) {
        bool const arm = !probe::vv::arg10SolidArmed();
        probe::vv::setArg10Solid(arm);
        probe::vv::inject::setUseArg10(arm);
        probe::vv::inject::setUseBackBuffer(false);
        probe::vv::inject::setFromTail(false);
        probe::vv::inject::setAction(probe::vv::inject::Action::SolidArg10);
        probe::vv::inject::setMode(arm ? probe::vv::inject::Mode::Fixed : probe::vv::inject::Mode::Off);
    }
    std::snprintf(
        buffer,
        sizeof(buffer),
        "armed=%d writer=%u list=%p target=%p",
        probe::vv::arg10SolidArmed() ? 1 : 0,
        probe::vv::arg10WriterSplit(),
        probe::vv::arg10WriterList(),
        probe::vv::arg10Target()
    );
    widgets::statRow("R3 A arg10", buffer);

    // B1b-1: our colour bypass. The boundary is the content anchor now (the first
    // list that bound a back buffer and drew = the world composite), so this follows
    // the frame instead of counting from its end; tail 4 stays as the fallback when
    // the probe did not see a writer.
    widgets::sectionHeader("VV DLSS B1b (TEST)");
    if (ImGui::Button("Colour bypass at content boundary (fallback tail 4)")) {
        probe::vv::setArg10Solid(false);
        probe::vv::inject::setUseArg10(false);
        probe::vv::inject::setMode(probe::vv::inject::Mode::Fixed);
        probe::vv::inject::setFromTail(true);
        probe::vv::inject::setK(4);
        probe::vv::inject::setAction(probe::vv::inject::Action::Bypass);
        probe::vv::inject::setUseBackBuffer(true);
    }
    ImGui::SameLine();
    if (ImGui::Button("Stop injection")) {
        probe::vv::setArg10Solid(false);
        probe::vv::inject::setUseArg10(false);
        probe::vv::inject::setMode(probe::vv::inject::Mode::Off);
        probe::vv::inject::setAction(probe::vv::inject::Action::Clear);
        probe::vv::inject::setUseBackBuffer(false);
    }
    widgets::statRow(
        "B1b boundary",
        probe::vv::inject::useBackBuffer() ? "content: back-buffer writer" : "head/tail k (count)",
        probe::vv::inject::useBackBuffer() ? ImGui::GetColorU32(ImGuiCol_Text) : 0
    );
    std::snprintf(
        buffer,
        sizeof(buffer),
        "split=%u  list=%p",
        probe::vv::backBufferWriterSplit(),
        probe::vv::backBufferWriterList()
    );
    widgets::statRow(
        "B1d writer",
        buffer,
        probe::vv::backBufferWriterSplit() ? ImGui::GetColorU32(ImGuiCol_Text) : 0
    );
    auto const bypass = vv::bypass::stats();
    if (ImGui::Button("Next colour candidate")) {
        vv::bypass::cycleColor();
    }
    ImGui::SameLine();
    if (ImGui::Button("Cycle encode (table / gamma / ACES+g)")) {
        vv::bypass::cycleTonemap();
    }
    widgets::statRow("B1b pipeline", bypass.pipelineReady ? "ready" : "not built",
                     bypass.pipelineReady ? ImGui::GetColorU32(ImGuiCol_Text) : 0);
    std::snprintf(
        buffer,
        sizeof(buffer),
        "choice=%d/%u  tonemap=%d  ptr=%p",
        bypass.colorChoice,
        bypass.colorCandidates,
        bypass.tonemap,
        bypass.colorResource
    );
    widgets::statRow("B1b colour", buffer);
    std::snprintf(
        buffer,
        sizeof(buffer),
        "%ux%u -> %ux%u  fmt=%u  recorded=%d",
        bypass.srcWidth,
        bypass.srcHeight,
        bypass.dstWidth,
        bypass.dstHeight,
        bypass.colorFormat,
        bypass.recorded ? 1 : 0
    );
    widgets::statRow("B1b sizes", buffer);
    if (bypass.lastError) {
        std::snprintf(buffer, sizeof(buffer), "0x%08X", static_cast<unsigned>(bypass.lastError));
        widgets::statRow("B1b last error", buffer);
    }
}
#endif

void drawVvInjection() {
    widgets::sectionHeader("ui.diag.vv"_tr().c_str());

    static int  mode     = static_cast<int>(probe::vv::inject::Mode::Off);
    static int  boundary = 1;
    static int  hold     = 20;
    static int  applied  = -1;
    static bool tail     = false;

    std::string const names[3] = {
        "ui.diag.vvOff"_tr(),
        "ui.diag.vvFixed"_tr(),
        "ui.diag.vvSweep"_tr(),
    };
    char const* items[3] = {names[0].c_str(), names[1].c_str(), names[2].c_str()};
    widgets::combo("ui.diag.vvMode"_tr().c_str(), &mode, items, 3);
    if (mode != applied) {
        probe::vv::inject::setMode(static_cast<probe::vv::inject::Mode>(mode));
        applied = mode;
    }

    // The engine's list count is not constant (a world frame is 48-51 lists, the
    // main-menu dressing room 69, and single frames reach 74/81/107), so a boundary
    // counted from the head moves whenever the frame changes shape. Counting from the
    // tail is the only way to ask "is the UI always the last list?" without the number
    // drifting underneath the experiment.
    if (widgets::toggle("ui.diag.vvTail"_tr().c_str(), &tail)) {
        probe::vv::inject::setFromTail(tail);
        boundary = 1;
    }

    auto const stats = probe::vv::inject::stats();
    char       buffer[96]{};

    if (mode == static_cast<int>(probe::vv::inject::Mode::Fixed)) {
        int const limit = tail ? 16 : 128;
        if (widgets::sliderInt("ui.diag.vvK"_tr().c_str(), &boundary, 1, limit)) {
            probe::vv::inject::setK(static_cast<unsigned>(boundary));
        }
    } else if (mode == static_cast<int>(probe::vv::inject::Mode::Sweep)) {
        if (widgets::sliderInt("ui.diag.vvHold"_tr().c_str(), &hold, 1, 60)) {
            probe::vv::inject::setHoldFrames(static_cast<unsigned>(hold));
        }
    }
    if (mode != static_cast<int>(probe::vv::inject::Mode::Off)) {
        std::snprintf(buffer, sizeof(buffer), "%u / %u", stats.split, stats.listsPerSubmit);
        widgets::statRow("ui.diag.vvAt"_tr().c_str(), buffer);
    }
    if (stats.minLists) {
        std::snprintf(buffer, sizeof(buffer), "%u .. %u", stats.minLists, stats.maxLists);
        widgets::statRow("ui.diag.vvRange"_tr().c_str(), buffer);
    }

    if (!stats.ready) {
        widgets::statRow("ui.diag.vvState"_tr().c_str(), "ui.diag.vvNotReady"_tr().c_str(), kTextDim);
    } else if (stats.reason != probe::vv::inject::SkipReason::None) {
        widgets::statRow("ui.diag.vvState"_tr().c_str(), skipReasonText(stats.reason).c_str(), kAmber);
    } else {
        widgets::statRow("ui.diag.vvState"_tr().c_str(), "ui.diag.vvRunning"_tr().c_str(), kAccent);
    }

    std::snprintf(buffer, sizeof(buffer), "%u", stats.listsPerSubmit);
    widgets::statRow("ui.diag.vvLists"_tr().c_str(), buffer);
    std::snprintf(buffer, sizeof(buffer), "%u", stats.injected);
    widgets::statRow("ui.diag.vvInjected"_tr().c_str(), buffer);
    std::snprintf(buffer, sizeof(buffer), "%u", stats.skipped);
    widgets::statRow("ui.diag.vvSkipped"_tr().c_str(), buffer, stats.skipped ? kAmber : kTextDim);
    std::snprintf(buffer, sizeof(buffer), "%u", stats.sweeps);
    widgets::statRow("ui.diag.vvPass"_tr().c_str(), buffer);
    widgets::hint("ui.diag.vvHint"_tr().c_str());
}

void drawTabDiagnostics() {
    auto const stats = nr::nrPanelStats();

    widgets::sectionHeader("ui.diag.gpu"_tr().c_str());

    char buffer[96]{};
    if (!stats.stampsActive) {
        widgets::statRow("ui.diag.nr"_tr().c_str(), "ui.diag.timingOff"_tr().c_str(), kTextDim);
        widgets::statRow("ui.diag.dlss"_tr().c_str(), "ui.diag.timingOff"_tr().c_str(), kTextDim);
    } else {
        if (stats.hasNr) {
            std::snprintf(
                buffer,
                sizeof(buffer),
                "%.3f ms  %s",
                stats.nrMs,
                "ui.diag.sample"_tr(stats.nrSamples).c_str()
            );
            widgets::statRow("ui.diag.nr"_tr().c_str(), buffer, kAccent);
        } else {
            widgets::statRow("ui.diag.nr"_tr().c_str(), "ui.stats.noSamples"_tr().c_str(), kTextDim);
        }
        if (stats.hasEngine) {
            std::snprintf(
                buffer,
                sizeof(buffer),
                "%.3f ms  %s",
                stats.engineMs,
                "ui.diag.sample"_tr(stats.engineSamples).c_str()
            );
            widgets::statRow("ui.diag.dlss"_tr().c_str(), buffer);
        } else {
            widgets::statRow("ui.diag.dlss"_tr().c_str(), "ui.stats.noSamples"_tr().c_str(), kTextDim);
        }
    }
    if (stats.lastError != 0) {
        std::snprintf(buffer, sizeof(buffer), "0x%08X", static_cast<unsigned>(stats.lastError));
        widgets::statRow("ui.diag.lastError"_tr().c_str(), buffer, kRed);
    }
    // Telemetry (docs/L 3.3): a failed evaluation is skipped by the composite, so a
    // rising counter explains "NR stopped affecting some frames" without digging logs.
    std::snprintf(buffer, sizeof(buffer), "%u", stats.evalFailures);
    widgets::statRow("ui.diag.evalFailures"_tr().c_str(), buffer, stats.evalFailures ? kAmber : kTextDim);
    std::string clearLabel  = "ui.diag.clear"_tr();
    clearLabel             += "##clearSamples";
    if (ImGui::Button(clearLabel.c_str())) nr::nrStampClearReported();
    widgets::hint("ui.diag.hint"_tr().c_str());

    // Stage split (docs/L 1.7): measured on the same frames as the NR row above, so
    // encode + evaluate + composite should add up to roughly that number. A stage
    // that reads 0.000 with samples is a real measurement, not "timing off".
    auto stageRow = [&](std::string const& label, bool has, double ms, unsigned samples) {
        char value[96]{};
        if (!stats.stampsActive) {
            widgets::statRow(label.c_str(), "ui.diag.timingOff"_tr().c_str(), kTextDim);
        } else if (has) {
            std::snprintf(value, sizeof(value), "%.3f ms  %s", ms, "ui.diag.sample"_tr(samples).c_str());
            widgets::statRow(label.c_str(), value);
        } else {
            widgets::statRow(label.c_str(), "ui.stats.noSamples"_tr().c_str(), kTextDim);
        }
    };
    stageRow("ui.diag.encode"_tr(), stats.hasEncode, stats.encodeMs, stats.encodeSamples);
    stageRow("ui.diag.evaluate"_tr(), stats.hasEvaluate, stats.evaluateMs, stats.evaluateSamples);
    stageRow("ui.diag.composite"_tr(), stats.hasComposite, stats.compositeMs, stats.compositeSamples);

    ImGui::Spacing();
    widgets::sectionHeader("ui.diag.switches"_tr().c_str());
    if (widgets::toggle("ui.diag.resetNr"_tr().c_str(), &gDraft.resetEveryFrame)) markDirty();
    if (widgets::toggle("ui.diag.resetSr"_tr().c_str(), &gDraft.srResetEveryFrame)) markDirty();
#ifdef TEST
    // The parameter trace only exists in a TEST build (src/neurodragon/Test.h),
    // so the release panel must not offer a switch that would do nothing.
    if (widgets::toggle("ui.diag.paramLog"_tr().c_str(), &gDraft.paramLog)) markDirty();
#endif
    widgets::hint("ui.diag.warnHint"_tr().c_str());

    ImGui::Spacing();
    drawVvInjection();
#ifdef TEST
    ImGui::Spacing();
    drawB0Probe();
#endif

    // What the last evaluation actually wrote (docs/L 1.2). This is the answer to
    // "did my change reach the runtime": the numbers come from the same variables
    // the parameter writes used, not from the draft in the panel.
    ImGui::Spacing();
    widgets::sectionHeader("ui.diag.written"_tr().c_str());
    auto const written = nr::nrFrameParams();
    if (!written.valid) {
        widgets::hint("ui.diag.writtenNone"_tr().c_str());
        return;
    }
    std::snprintf(buffer, sizeof(buffer), "%d", written.replace);
    widgets::statRow("ui.diag.wReplace"_tr().c_str(), buffer);
    std::string placementName;
    if (written.placement == 1) {
        placementName = "ui.diag.placementAfter"_tr();
    } else if (written.afterFallback) {
        placementName = "ui.diag.placementFallback"_tr();
    } else {
        placementName = "ui.diag.placementBefore"_tr();
    }
    widgets::statRow("ui.diag.wPlacement"_tr().c_str(), placementName.c_str());
    std::snprintf(buffer, sizeof(buffer), "%u x %u", written.domainWidth, written.domainHeight);
    widgets::statRow("ui.diag.wDomain"_tr().c_str(), buffer);
    std::snprintf(buffer, sizeof(buffer), "%u x %u", written.guideWidth, written.guideHeight);
    widgets::statRow("ui.diag.wGuide"_tr().c_str(), buffer);
    std::string const compositeName = (written.compositeMode == 4 || written.compositeMode == 6) ? "ui.diag.modeRatio"_tr()
                                      : (written.compositeMode == 2 || written.compositeMode == 5) ? "ui.diag.modeGain"_tr()
                                                                                                    : "ui.diag.modeOff"_tr();
    widgets::statRow("ui.diag.wComposite"_tr().c_str(), compositeName.c_str());
    if (written.encodeMode < 0) {
        widgets::statRow("ui.diag.wEncode"_tr().c_str(), "ui.diag.modeRaw"_tr().c_str());
    } else if (written.encodeMode == 3) {
        widgets::statRow("ui.diag.wEncode"_tr().c_str(), "ui.neural.encodeTrp"_tr().c_str());
    } else if (written.encodeMode == 0) {
        widgets::statRow("ui.diag.wEncode"_tr().c_str(), "ui.neural.encodeReinhard"_tr().c_str());
    } else {
        std::snprintf(buffer, sizeof(buffer), "#%d", written.encodeMode);
        widgets::statRow("ui.diag.wEncode"_tr().c_str(), buffer);
    }
    std::snprintf(buffer, sizeof(buffer), "%d", written.style);
    widgets::statRow("ui.diag.wStyle"_tr().c_str(), buffer);
    std::snprintf(buffer, sizeof(buffer), "%d", written.networkPreset);
    widgets::statRow("ui.diag.wPreset"_tr().c_str(), buffer);
    std::snprintf(buffer, sizeof(buffer), "%.2f", static_cast<double>(written.intensity));
    widgets::statRow("ui.diag.wIntensity"_tr().c_str(), buffer);
    std::snprintf(
        buffer,
        sizeof(buffer),
        "%.2f / %.2f",
        static_cast<double>(written.localTone),
        static_cast<double>(written.localStructure)
    );
    widgets::statRow("ui.diag.wLocal"_tr().c_str(), buffer);
    std::snprintf(buffer, sizeof(buffer), "%.2f", static_cast<double>(written.skinStructure));
    widgets::statRow("ui.diag.wSkin"_tr().c_str(), buffer);
    std::snprintf(buffer, sizeof(buffer), "%d", written.reset);
    widgets::statRow("ui.diag.wReset"_tr().c_str(), buffer);
    std::snprintf(buffer, sizeof(buffer), "%.0f x %.0f", static_cast<double>(written.mvScaleX), static_cast<double>(written.mvScaleY));
    widgets::statRow("ui.diag.wMv"_tr().c_str(), buffer);
    std::snprintf(
        buffer,
        sizeof(buffer),
        "%d%%  %u x %u",
        written.inputPercent,
        written.featureWidth,
        written.featureHeight
    );
    widgets::statRow("ui.diag.wScale"_tr().c_str(), buffer);
    std::snprintf(buffer, sizeof(buffer), "%.2f / %.2f", static_cast<double>(written.broad), static_cast<double>(written.detail));
    widgets::statRow("ui.diag.wTone"_tr().c_str(), buffer);
    if (written.adaptiveExposure) {
        std::snprintf(buffer, sizeof(buffer), "%+.2f EV", static_cast<double>(written.exposureBias));
        widgets::statRow("ui.diag.wExposure"_tr().c_str(), buffer);
    } else {
        widgets::statRow("ui.diag.wExposure"_tr().c_str(), "ui.diag.modeOff"_tr().c_str());
    }
    if (written.compare) {
        std::snprintf(buffer, sizeof(buffer), "%.2f", static_cast<double>(written.compareSplit));
        widgets::statRow("ui.diag.wCompare"_tr().c_str(), buffer);
    } else {
        widgets::statRow("ui.diag.wCompare"_tr().c_str(), "ui.diag.modeOff"_tr().c_str());
    }
}

void drawTabAdvanced() {
    widgets::sectionHeader("ui.adv.ui"_tr().c_str());
    char buffer[64]{};
    std::snprintf(buffer, sizeof(buffer), "%d%%", static_cast<int>(std::lround(theme::userScale() * 100.0f)));
    widgets::statRow("ui.adv.scale"_tr().c_str(), buffer);
    widgets::hint("ui.adv.scaleHint"_tr().c_str());

    ImGui::Spacing();
    widgets::sectionHeader("ui.adv.keys"_tr().c_str());

    auto& overlay = Overlay::getInstance();

    // One row per rebindable hotkey. The capture itself is global (one at a time,
    // fed by the window thread); `gPendingKeySlot` only remembers which row started
    // it so the result lands in the right draft.
    struct Row {
        input::KeyBind* keys;
        Hotkey          hotkey;
        std::string     label;
        std::string     hint;
    };
    std::array<Row, 4> const rows{{
        {&gDraftPanelKeys, Hotkey::Panel, "ui.adv.open"_tr(), "ui.adv.openHint"_tr()},
        {&gDraftNrKeys, Hotkey::NrToggle, "ui.adv.nrKey"_tr(), "ui.adv.nrKeyHint"_tr()},
        {&gDraftResetKeys, Hotkey::NrReset, "ui.adv.resetKey"_tr(), "ui.adv.resetKeyHint"_tr()},
        {&gDraftScreenshotKeys, Hotkey::Screenshot, "ui.adv.screenshot"_tr(), "ui.adv.screenshotHint"_tr()},
    }};

    for (size_t index = 0; index < rows.size(); ++index) {
        Row const& row = rows[index];

        // Worst collision with any *other* row. Severity only depends on binding
        // contents (not on hover/click), so the style pushes below stay paired.
        input::ConflictSeverity severity = input::ConflictSeverity::None;
        std::string             otherLabel;
        for (size_t other = 0; other < rows.size(); ++other) {
            if (other == index) continue;
            auto const candidate = input::classifyConflict(*row.keys, *rows[other].keys);
            if (candidate > severity) {
                severity   = candidate;
                otherLabel = rows[other].label;
            }
        }

        bool const capturing = gPendingKeySlot == static_cast<int>(index) && overlay.isCapturingKey();

        std::string text;
        if (capturing) {
            auto const preview = overlay.capturePreview();
            text = preview.empty() ? "ui.adv.capturing"_tr() : input::formatCombo(preview);
        } else {
            text = row.keys->display();
        }

        widgets::beginRow();
        ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(kTextDim), "%s", row.label.c_str());

        auto const& style      = ImGui::GetStyle();
        auto const  resetText  = "ui.adv.reset"_tr();
        auto const  resetWidth = ImGui::CalcTextSize(resetText.c_str()).x + style.FramePadding.x * 2.0f;
        auto const  keyWidth   = ImGui::CalcTextSize(text.c_str()).x + style.FramePadding.x * 2.0f;
        // No reset button while capturing: the row should stay quiet (the preview
        // text is still growing).
        auto const totalWidth = capturing ? keyWidth : keyWidth + style.ItemInnerSpacing.x + resetWidth;

        ImGui::SameLine();
        ImGui::SetCursorPosX(widgets::rowRight() - totalWidth);

        bool const  conflicted = !capturing && severity != input::ConflictSeverity::None;
        ImU32 const border     = severity == input::ConflictSeverity::Exact ? kRed : kAmber;
        if (conflicted) {
            ImGui::PushStyleColor(ImGuiCol_Border, border);
            ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, std::max(1.0f, theme::scale()));
        }
        // The visible text cannot be the id: two rows can show the same text (both
        // "-", or the same key twice), and ImGui would then share one id between them.
        std::string const id      = text + "##hotkey" + std::to_string(index);
        bool const        clicked = ImGui::Button(id.c_str());
        if (conflicted) {
            ImGui::PopStyleVar();
            ImGui::PopStyleColor();
        }

        if (clicked) {
            gPendingKeySlot = static_cast<int>(index);
            overlay.beginKeyCapture();
        }
        if (ImGui::IsItemHovered()) {
            // The conflict is reported on hover only: an inline line per row pushed
            // everything below it down and broke the column's rhythm.
            if (severity == input::ConflictSeverity::Exact) {
                ImGui::SetTooltip("%s", "ui.adv.conflictExact"_tr(otherLabel).c_str());
            } else if (severity == input::ConflictSeverity::Overlap) {
                ImGui::SetTooltip("%s", "ui.adv.conflictOverlap"_tr(otherLabel).c_str());
            } else {
                ImGui::SetTooltip("%s", row.hint.c_str());
            }
        }

        if (capturing) continue;

        ImGui::SameLine(0.0f, style.ItemInnerSpacing.x);

        // Back to the code default. Grey (but still drawn) when already there, so the
        // row does not change width between the two states.
        bool const canReset = !row.keys->equals(defaultKeys(row.hotkey));
        if (!canReset) ImGui::BeginDisabled();
        bool const reset = ImGui::Button((resetText + "##reset" + std::to_string(index)).c_str());
        if (!canReset) ImGui::EndDisabled();

        if (reset) {
            *row.keys = defaultKeys(row.hotkey);
            markDirty();
        }
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
            ImGui::SetTooltip("%s", "ui.adv.resetHint"_tr().c_str());
        }
    }

    // The capture result arrives from the window thread: nullopt = nothing committed
    // (still capturing, or cancelled); engaged-and-empty = the player pressed Esc, an
    // explicit unbind; engaged-and-non-empty = the new combination.
    if (auto captured = overlay.takeCapturedKeys()) {
        int const slot = gPendingKeySlot;
        if (slot >= 0 && slot < static_cast<int>(rows.size())) {
            auto& keys = *rows[static_cast<size_t>(slot)].keys;
            // The panel key cannot be unbound: Esc on that row keeps the current
            // binding, because nothing else would open the panel again.
            bool const keepsPanelKey = slot == static_cast<int>(Hotkey::Panel) && captured->empty();
            if (!keepsPanelKey && !keys.equals(*captured)) {
                keys = std::move(*captured);
                markDirty();
            }
        }
        gPendingKeySlot = -1;
    } else if (!overlay.isCapturingKey()) {
        gPendingKeySlot = -1;
    }

    widgets::statRow("ui.adv.close"_tr().c_str(), "Esc");

    ImGui::Spacing();
    widgets::sectionHeader("ui.adv.config"_tr().c_str());
    widgets::hint("ui.adv.configHint"_tr().c_str());

    ImGui::Spacing();
    widgets::sectionHeader("ui.adv.about"_tr().c_str());
    widgets::statRow("ui.adv.version"_tr().c_str(), "0.0.0");
}

// Footer --------------------------------------------------------------------

void applyDraft(bool persist) {
    int const oldPreset = config::get().sr.preset;

    config::get().nr                = gDraft;
    config::get().sr                = gDraftSr;
    config::get().ui.openKeys       = gDraftPanelKeys.keys();
    config::get().ui.nrKeys         = gDraftNrKeys.keys();
    config::get().ui.resetKeys      = gDraftResetKeys.keys();
    config::get().ui.screenshotKeys = gDraftScreenshotKeys.keys();
    Overlay::getInstance().refreshKeys();
    gDirty = false;

    // A model preset is only read at CreateFeature, so applying a new one has to
    // make the engine rebuild its DLSS feature. Other settings are read live.
    if (config::get().sr.preset != oldPreset) {
        nr::srRequestFeatureRebuild();
    }

    if (!persist) {
        setStatus("ui.footer.applied"_tr());
        return;
    }
    if (config::save()) setStatus("ui.footer.saved"_tr());
    else setStatus("ui.footer.writeFailed"_tr());

    // "Save as default" with NR off is the deliberate "done with it" action: ask the
    // render thread to unload the feature and the 264 MB model. "Apply" deliberately
    // does not, so toggling NR while tuning stays instant.
    if (!config::get().nr.enabled) {
        nr::nrRequestUnload();
    }
}

void drawFooter(Layout const& layout) {
    auto*      drawList = ImGui::GetWindowDrawList();
    auto const s        = layout.scale;

    float const ruleY = layout.pos.y + layout.size.y - layout.pad - layout.footerHeight + 2.0f * s;
    drawList->AddLine(
        ImVec2(layout.pos.x + layout.pad, ruleY),
        ImVec2(layout.pos.x + layout.size.x - layout.pad, ruleY),
        kBorderSoft,
        std::max(1.0f, 1.0f * s)
    );

    float const top = ruleY + 14.0f * s;

    std::string status = gStatus;
    if (status.empty()) status = gDirty ? "ui.footer.dirty"_tr() : "ui.footer.clean"_tr();

    at(ImVec2(layout.pos.x + layout.pad, top + 7.0f * s));
    ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(gDirty ? kAccent : kTextDim), "%s", status.c_str());

    float const buttonHeight = 28.0f * s;
    float const buttonWidth  = 96.0f * s;
    float const gap          = 7.0f * s;
    float       x            = layout.pos.x + layout.size.x - layout.pad - buttonWidth;

    at(ImVec2(x, top));
    if (widgets::footerButton("ui.footer.saveDefault"_tr().c_str(), ImVec2(buttonWidth, buttonHeight), true)) {
        applyDraft(true);
    }

    x -= buttonWidth + gap;
    at(ImVec2(x, top));
    if (widgets::footerButton("ui.footer.apply"_tr().c_str(), ImVec2(buttonWidth, buttonHeight), false)) {
        applyDraft(false);
    }

    x -= buttonWidth + gap;
    at(ImVec2(x, top));
    if (widgets::footerButton("ui.footer.discard"_tr().c_str(), ImVec2(buttonWidth, buttonHeight), false)) {
        gDraft   = config::get().nr;
        gDraftSr = sanitizedSr(config::get().sr);
        reloadKeyDrafts();
        gDirty = false;
        setStatus("ui.footer.discarded"_tr());
    }
}

} // namespace

void render() {
    auto const viewport = ImGui::GetIO().DisplaySize;
    if (viewport.x <= 1.0f || viewport.y <= 1.0f) return;

    theme::apply(viewport);

    float const s = theme::scale();
    // Width comes from the two fixed columns (330 + 460 logical) plus the gutter,
    // not from the viewport.
    ImVec2 const defaultSize{
        std::min(850.0f * s, viewport.x - 64.0f * s),
        std::min(700.0f * s, viewport.y - 64.0f * s),
    };
    ImVec2 const defaultPos{26.0f * s, 26.0f * s};
    if (gPos.x < 0.0f) gPos = defaultPos;

    // Drag is applied *before* Begin: the window, its background and both child
    // columns are then all laid out at the same position. Applying it after Begin
    // (SetWindowPos mid-frame) moved the frame but not the already-positioned
    // children, which is exactly the "elements lag behind the window" symptom.
    auto const& io   = ImGui::GetIO();
    float const minX = 120.0f * s;
    float const minY = 40.0f * s;
    if (gDragging) {
        gPos.x += io.MouseDelta.x;
        gPos.y += io.MouseDelta.y;
        if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) gDragging = false;
    }
    gPos = ImVec2(
        std::clamp(gPos.x, -(defaultSize.x - minX), viewport.x - minX),
        std::clamp(gPos.y, 0.0f, viewport.y - minY)
    );

    ImGui::SetNextWindowPos(gPos, ImGuiCond_Always);
    ImGui::SetNextWindowSize(defaultSize, ImGuiCond_Always);

    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 6.0f * s);

    // NoMove: dragging is our own state machine over the title strip only.
    constexpr ImGuiWindowFlags kWindowFlags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove
                                            | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoSavedSettings
                                            | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoNavFocus
                                            | ImGuiWindowFlags_NoBackground;

    if (ImGui::Begin("##neurodragonPanel", nullptr, kWindowFlags)) {
        ImVec2 const size = ImGui::GetWindowSize();

        auto const layout = computeLayout(gPos, size);
        auto const stats  = nr::nrPanelStats();

        auto* drawList = ImGui::GetWindowDrawList();
        drawList->AddRectFilled(gPos, ImVec2(gPos.x + size.x, gPos.y + size.y), kPanel, 6.0f * s);
        drawList->AddRect(gPos, ImVec2(gPos.x + size.x, gPos.y + size.y), kBorder, 6.0f * s, 0, std::max(1.0f, s));

        drawHeader(layout, stats);

        // Drag affordance: the title strip, minus anything interactive on it (the
        // zoom buttons). Checked after the header so IsAnyItemHovered() already
        // knows about them.
        ImVec2 const stripMax{gPos.x + size.x, gPos.y + layout.pad + layout.headerHeight};
        bool const   overStrip = ImGui::IsMouseHoveringRect(gPos, stripMax) && !ImGui::IsAnyItemHovered();
        if (overStrip && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) gDragging = true;
        if (overStrip || gDragging) ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeAll);

        drawPipeline(layout, stats);
        drawTabs(layout);

        float const leftX  = gPos.x + layout.pad;
        float const rightX = leftX + layout.leftWidth + layout.gutter;
        float const rightW = gPos.x + size.x - layout.pad - rightX;

        drawList->AddRectFilled(
            ImVec2(leftX, layout.bodyTop),
            ImVec2(leftX + layout.leftWidth, layout.bodyTop + layout.bodyHeight),
            kPanelDeep,
            4.0f * s
        );

        float const padding = 14.0f * s;

        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(padding, padding));
        at(ImVec2(leftX, layout.bodyTop));
        // A borderless child window gets no padding unless asked for, which is why
        // the columns looked like the text was glued to their edges.
        if (ImGui::BeginChild(
                "##statsColumn",
                ImVec2(layout.leftWidth, layout.bodyHeight),
                ImGuiChildFlags_AlwaysUseWindowPadding
            )) {
            drawStatsColumn(layout, stats);
        }
        ImGui::EndChild();

        at(ImVec2(rightX, layout.bodyTop));
        if (ImGui::BeginChild(
                "##settingsColumn",
                ImVec2(rightW, layout.bodyHeight),
                ImGuiChildFlags_AlwaysUseWindowPadding
            )) {
            switch (gActiveTab) {
            case 0:
                drawTabNeural();
                break;
            case 1:
                drawTabSr();
                break;
            case 2:
                drawTabDiagnostics();
                break;
            default:
                drawTabAdvanced();
                break;
            }
        }
        ImGui::EndChild();
        ImGui::PopStyleVar();

        drawFooter(layout);
    }
    ImGui::End();
    ImGui::PopStyleVar(3);
}

void onOpened() {
    // Start from what is running right now. Nothing is read from disk here: the
    // file is an import at load, and the panel is a staging area over the live
    // values.
    gDraft   = config::get().nr;
    gDraftSr = sanitizedSr(config::get().sr);
    reloadKeyDrafts();
    gModelFiles = utils::listNrModelFiles();
    gDirty      = false;
    gStatus.clear();
}

void onClosed() {
    // Unapplied edits die with the panel - same contract as the reference UI:
    // Apply is the only way a change reaches the renderer.
    gDragging = false;
    if (gDirty) {
        gDirty = false;
        Overlay::getInstance().notify("ui.footer.discarded"_tr());
    }
}

} // namespace neurodragon::ui::menu
