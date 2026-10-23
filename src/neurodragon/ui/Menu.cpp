#include "neurodragon/ui/Menu.h"

#include "neurodragon/Config.h"
#include "neurodragon/Test.h"
#include "neurodragon/nr/D3D12Bridge.h"
#include "neurodragon/ui/Overlay.h"
#include "neurodragon/ui/Theme.h"
#include "neurodragon/ui/Widgets.h"

#include "ll/api/i18n/I18n.h"

#include <imgui.h>

#include <Windows.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>

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
    return sr;
}

int         gDraftKey{VK_END};
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

// Readable name for a virtual-key code.
std::string keyLabel(int virtualKey) {
    if (virtualKey >= 'A' && virtualKey <= 'Z') return std::string(1, static_cast<char>(virtualKey));
    if (virtualKey >= '0' && virtualKey <= '9') return std::string(1, static_cast<char>(virtualKey));
    if (virtualKey >= VK_F1 && virtualKey <= VK_F12) return "F" + std::to_string(virtualKey - VK_F1 + 1);
    switch (virtualKey) {
    case VK_END:
        return "End";
    case VK_HOME:
        return "Home";
    case VK_INSERT:
        return "Insert";
    case VK_DELETE:
        return "Delete";
    case VK_PRIOR:
        return "PageUp";
    case VK_NEXT:
        return "PageDown";
    case VK_TAB:
        return "Tab";
    case VK_SPACE:
        return "Space";
    case VK_RETURN:
        return "Enter";
    case VK_ESCAPE:
        return "Esc";
    default:
        return "ui.key.unknown"_tr(virtualKey);
    }
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
    case 0:
        return "ui.replace.hint0"_tr();
    case 5:
        return "ui.replace.hint5"_tr();
    case 6:
        return "ui.replace.hint6"_tr();
    default:
        return "ui.replace.hintOther"_tr();
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
    } else if (live.replace != 5) {
        stateText  = "ui.state.notOnScreen"_tr();
        stateColor = kAmber;
    } else {
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

    bool const nrActive   = live.enabled && live.replace == 5;
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

    std::string const replaceItems[3] = {
        "ui.replace.item0"_tr(),
        "ui.replace.item5"_tr(),
        "ui.replace.item6"_tr(),
    };
    char const* itemPointers[3]  = {replaceItems[0].c_str(), replaceItems[1].c_str(), replaceItems[2].c_str()};
    int const   replaceValues[3] = {0, 5, 6};
    if (mappedCombo("ui.neural.replace"_tr().c_str(), &gDraft.replace, itemPointers, replaceValues, 3)) markDirty();
    widgets::hint(replaceHint(gDraft.replace).c_str());

    if (widgets::slider("ui.neural.gain"_tr().c_str(), &gDraft.gainBoost, 0.5f, 8.0f, "%.2f", "x")) markDirty();

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

    ImGui::Spacing();
    widgets::sectionHeader("ui.neural.ratio"_tr().c_str());
    if (widgets::slider("ui.neural.white"_tr().c_str(), &gDraft.whitePoint, 0.05f, 4.0f, "%.3f")) markDirty();
    if (widgets::slider("ui.neural.transfer"_tr().c_str(), &gDraft.transferStrength, 0.0f, 2.0f, "%.2f")) markDirty();
    if (widgets::slider("ui.neural.colour"_tr().c_str(), &gDraft.colourStrength, 0.0f, 2.0f, "%.2f")) markDirty();
    if (widgets::slider("ui.neural.maxRatio"_tr().c_str(), &gDraft.maxRatio, 1.0f, 8.0f, "%.2f")) markDirty();
}

void drawTabSr() {
    auto const sr = nr::srPanelStats();
    auto const ps = nr::nrPanelStats();

    char buffer[128]{};

    widgets::sectionHeader("ui.sr.engine"_tr().c_str());

    std::snprintf(buffer, sizeof(buffer), "%u x %u", ps.renderWidth, ps.renderHeight);
    widgets::statRow("ui.sr.renderSize"_tr().c_str(), buffer);
    std::snprintf(buffer, sizeof(buffer), "%u x %u", ps.outputWidth, ps.outputHeight);
    widgets::statRow("ui.sr.outputSize"_tr().c_str(), buffer);
    if (ps.outputWidth != 0 && ps.renderWidth != 0) {
        std::snprintf(
            buffer,
            sizeof(buffer),
            "%.1f%%",
            100.0 * static_cast<double>(ps.renderWidth) / static_cast<double>(ps.outputWidth)
        );
        widgets::statRow("ui.sr.ratio"_tr().c_str(), buffer);
    }
    widgets::statRow("ui.sr.quality"_tr().c_str(), qualityName(sr.quality).c_str());
    widgets::statRow("ui.sr.preset"_tr().c_str(), presetName(sr.preset).c_str());
    std::snprintf(buffer, sizeof(buffer), "%.2f", static_cast<double>(sr.sharpness));
    widgets::statRow("ui.sr.sharpness"_tr().c_str(), buffer);
    std::snprintf(buffer, sizeof(buffer), "0x%08X", sr.flags);
    widgets::statRow("ui.sr.flags"_tr().c_str(), buffer);
    widgets::hint("ui.sr.readOnlyHint"_tr().c_str());

    ImGui::Spacing();
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
    widgets::hint("ui.sr.cbHint"_tr().c_str());

    ImGui::Spacing();
    widgets::sectionHeader("ui.sr.actions"_tr().c_str());
    std::string resetLabel = "ui.sr.reset"_tr();
    resetLabel += "##srReset";
    if (ImGui::Button(resetLabel.c_str())) {
        nr::srRequestReset();
    }
    widgets::hint("ui.sr.resetHint"_tr().c_str());
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
    std::string clearLabel  = "ui.diag.clear"_tr();
    clearLabel             += "##clearSamples";
    if (ImGui::Button(clearLabel.c_str())) nr::nrStampClearReported();
    widgets::hint("ui.diag.hint"_tr().c_str());

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

    widgets::beginRow();
    ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(kTextDim), "%s", "ui.adv.open"_tr().c_str());

    std::string const label = overlay.isCapturingHotkey() ? "ui.adv.capturing"_tr() : keyLabel(gDraftKey);
    float const       width = ImGui::CalcTextSize(label.c_str()).x + ImGui::GetStyle().FramePadding.x * 2.0f;
    ImGui::SameLine();
    ImGui::SetCursorPosX(widgets::rowRight() - width);
    if (ImGui::Button((label + "##rebind").c_str())) overlay.beginHotkeyCapture();
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", "ui.adv.rebindHint"_tr().c_str());

    if (int const captured = overlay.takeCapturedHotkey(); captured != 0 && captured != gDraftKey) {
        gDraftKey = captured;
        markDirty();
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
    int const oldPreset    = config::get().sr.preset;
    config::get().nr         = gDraft;
    config::get().sr         = gDraftSr;
    config::get().ui.openKey = gDraftKey;
    Overlay::getInstance().refreshHotkey();
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
        gDraft    = config::get().nr;
        gDraftSr  = sanitizedSr(config::get().sr);
        gDraftKey = config::get().ui.openKey;
        gDirty    = false;
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
    gDraft    = config::get().nr;
    gDraftSr  = sanitizedSr(config::get().sr);
    gDraftKey = config::get().ui.openKey;
    gDirty    = false;
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
