#include "neurodragon/ui/Menu.h"

#include "neurodragon/Config.h"
#include "neurodragon/Log.h"
#include "neurodragon/Test.h"
#include "neurodragon/input/KeyBind.h"
#include "neurodragon/nr/D3D12Bridge.h"
#include "neurodragon/sr/SrSharpening.h"
#include "neurodragon/sr/SrStatus.h"
#include "neurodragon/ui/Overlay.h"
#include "neurodragon/ui/Theme.h"
#include "neurodragon/ui/Widgets.h"
#include "neurodragon/utils/Utils.h"
#include "neurodragon/vv/VvDlssSr.h"
#include "neurodragon/vv/VvSr.h"

#include "ll/api/i18n/I18n.h"

#include <imgui.h>

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
// SR jitter is staged like every other control in the right column, but it has no
// config entry: it is an A/B switch, not a preference. So Apply is the only thing that
// writes it into the running pipeline, "Save as default" leaves it out, and closing
// the panel without applying leaves the running value alone. Default Zero - the world
// frame carries no TAA sub-pixel offset (EXP-0098).
int gDraftJitter{static_cast<int>(vv::sr::JitterSource::Zero)};

// The combo offers all four sources; an unknown value is staged as Zero, which is both
// the default and the only one that is a shipping setting (the other three are the
// discriminators from the jitter contract, kept for diagnosis).
int stagedJitter(vv::sr::JitterSource running) {
    switch (running) {
    case vv::sr::JitterSource::Engine:
    case vv::sr::JitterSource::Negated:
    case vv::sr::JitterSource::Previous:
        return static_cast<int>(running);
    case vv::sr::JitterSource::Zero:
    default:
        return static_cast<int>(vv::sr::JitterSource::Zero);
    }
}

// Re-reads the running value: the panel's "start from what is running" rule, used when
// it opens and when Discard is pressed.
void stageJitterDraft() { gDraftJitter = stagedJitter(vv::sr::jitterSource()); }

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
    sr.vvPercent    = sanitizedSrPercent(sr.vvPercent);
    // A hand-edited config can carry anything; the render thread reads the strength
    // every frame, so keep one sanitised copy at the panel boundary as well.
    if (!std::isfinite(sr.sharpness) || sr.sharpness < 0.0f || sr.sharpness > 1.0f) {
        sr.sharpness = std::clamp(std::isfinite(sr.sharpness) ? sr.sharpness : 0.672f, 0.0f, 1.0f);
    }
    return sr;
}

// Only TRP's Ratio composite ships (2026-10-29): the gain composite's controls are
// gone, so a config (or a hand edit) that still says Replace=5 is staged as 6. Keeping
// this at the panel boundary means Apply can never write the removed mode back.
config::Nr sanitizedNr(config::Nr nr) {
    nr.replace = 6;
    return nr;
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

// NR/SR slot swap in the pipeline strip (after-upscaling placement): 0 = NR then SR,
// 1 = SR then NR. The first frame after the panel opens snaps to the target; later
// changes animate over kSwapSeconds, so a placement change reads as a swap rather than
// a jump. Driven from drawPipeline().
constexpr float kSwapSeconds = 0.25f;
float           gSwapProgress{};
bool            gSwapPrimed{};

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

// Column caption. Plain text, not a section rule: a rule directly above the first
// real section rule read as a doubled header.
void columnCaption(char const* text) {
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(kTextDim));
    ImGui::TextUnformatted(text);
    ImGui::PopStyleColor();
}

// ---------------------------------------------------------------------------
// Buttons
//
// Every button in the panel goes through here. The shared widget owns the look and
// the TEST activation log, so a press produces exactly one line no matter which
// panel surface it came from.
// ---------------------------------------------------------------------------
bool menuButton(char const* label, ImVec2 size = ImVec2(0.0f, 0.0f), bool primary = false) {
    return widgets::button(label, size, primary);
}

// ---------------------------------------------------------------------------
// Live observation (docs/specs/ui.md §3, §6)
//
// The pipeline strip and the left column answer one question: what is the renderer
// doing *right now*. A config flag says what was asked for, `featureCreated` says
// something was built - neither says a frame went through it. Every state shown on
// those two surfaces is therefore derived from counters that only move when the
// renderer actually runs: PanelStats::evalCount for NR, SrStats::evalCount for the
// engine's DLSS, the recorded frame number for our own VV pipeline. `sr::status()`
// answers "which upscaler is live" the same way, from observation.
//
// GPU timestamps being unavailable (stampsActive false) does not remove the
// evidence: a counter delta that keeps growing is a running feature, and the panel
// shows that delta instead of the milliseconds it cannot measure.
// ---------------------------------------------------------------------------

// Off = the setting says disabled (a request); Configured = enabled but nothing
// built; Created = built but no frame observed yet; Running = a counter moved
// inside the freshness window. Only the pipeline strip's NR box still needs all
// four; the header dot answers for the whole pipeline instead (see Overall).
enum class Stage { Off, Configured, Created, Running };

// The header dot's conclusion (2026-10-29 request). It used to report NR alone, so a
// deliberately disabled NR read as "everything is off" while SR was upscaling, and a
// dead upscaler looked healthy as long as NR ran. The pipeline as a whole is in one
// of three states:
//   Running - some stage of it really produced a frame;
//   Error   - an actual error code was recorded;
//   Waiting - nothing has run a frame yet (menu, level load, feature not created).
// NR being off is not an error and not a state of its own: SR can carry the frame.
enum class Overall { Waiting, Running, Error };

// Two seconds: two frames of staleness is a hitch, and a pause menu or a level load
// must not read as "the feature died".
constexpr double kLiveWindow = 2.0;

double liveNow() { return static_cast<double>(ImGui::GetTime()); }

// Watches a render-thread counter and answers one question: did it move inside the
// freshness window? Primed on the first read, so merely opening the panel never counts
// as movement.
struct CounterWatch {
    bool               primed{};
    unsigned long long last{};
    double             movedAt{};
    bool               moved{};

    void observe(unsigned long long value, double now) {
        if (!primed) {
            primed = true;
            last   = value;
            return;
        }
        // A counter that went backwards was reset (new session, new world): start over
        // from there instead of treating it as movement or as an underflow.
        if (value < last) {
            last  = value;
            moved = false;
            return;
        }
        if (value == last) return;
        last    = value;
        movedAt = now;
        moved   = true;
    }

    [[nodiscard]] bool fresh(double now) const { return moved && now - movedAt <= kLiveWindow; }
};

CounterWatch gNrEvals;
CounterWatch gSrEvals;
// Failure counters exist to explain a *current* problem, so they are observed rather
// than read as a latch: a cumulative count that went up once must stop colouring the
// header a minute later (2026-10-29 review).
CounterWatch gNrFailures;
CounterWatch gSrFailures;

struct Live {
    nr::PanelStats      stats{};
    sr::Status          backend{};
    nr::SrStats         srStats{};
    vv::sr::Stats       vvStats{};
    vv::dlss::SrState   dlss{};
    Stage               nr{Stage::Off};
    Stage               upscale{Stage::Off};
    /// A failure counter moved inside the freshness window: an error *now*, not a
    /// historical one.
    bool                failureRecent{};
    /// The feature could not be created at all (`lastError` recorded while NR is still
    /// Configured). Only consulted when nothing runs, so a stale code from an earlier
    /// attempt cannot pin the header red once frames flow again.
    bool                createFailed{};
};

// Sampled once per frame: sr::status() carries hysteresis, so calling it twice in one
// frame would advance its agreement counter twice.
Live sampleLive() {
    double const now = liveNow();

    Live live;
    live.stats   = nr::nrPanelStats();
    live.backend = sr::status();
    live.srStats = nr::srPanelStats();
    live.vvStats = vv::sr::stats();
    live.dlss    = vv::dlss::srState();

    gNrEvals.observe(live.stats.evalCount, now);
    gSrEvals.observe(live.srStats.evalCount, now);
    gNrFailures.observe(live.stats.evalFailures, now);
    gSrFailures.observe(live.srStats.evalFailures, now);

    if (!config::get().nr.enabled) {
        live.nr = Stage::Off;
    } else if (!live.stats.featureCreated) {
        live.nr = Stage::Configured;
    } else {
        live.nr = gNrEvals.fresh(now) ? Stage::Running : Stage::Created;
    }

    switch (live.backend.backend) {
    case sr::Backend::EngineDlss:
        // The engine's feature existing is not the same as it evaluating: its
        // evaluation counter (or a measured frame) is what says it runs.
        live.upscale = (gSrEvals.fresh(now) || live.stats.hasEngine) ? Stage::Running : Stage::Created;
        break;
    case sr::Backend::VvDlss:
        // The backend label is sticky across a short transition; only its live
        // observation may light the box. Otherwise it remains visibly configured.
        live.upscale = live.backend.live ? Stage::Running : Stage::Configured;
        break;
    default:
        // Nothing is upscaling.
        live.upscale = (live.backend.vvConfigured || live.backend.vvSeen) ? Stage::Configured : Stage::Off;
        break;
    }

    // A failure *now* is a counter that grew while the panel watched. The engine path's
    // counter is only meaningful while the engine owns the upscaler.
    live.failureRecent = gNrFailures.fresh(now)
                      || (live.backend.backend == sr::Backend::EngineDlss && gSrFailures.fresh(now));
    // "Creation failed" is a state, not an event: it only says something while the
    // feature still does not exist.
    live.createFailed = live.nr == Stage::Configured && live.stats.lastError != 0;
    return live;
}

// Running > Error > Waiting. A frame that went through wins over any recorded failure
// (a retried evaluation must not keep the pipeline red), and both error signals are
// bounded: a counter movement inside the window, or "still not created after a failure".
Overall overallState(Live const& live) {
    if (live.nr == Stage::Running || live.upscale == Stage::Running) return Overall::Running;
    if (live.failureRecent || live.createFailed) return Overall::Error;
    return Overall::Waiting;
}

std::string stageName(Stage stage) {
    switch (stage) {
    case Stage::Running:
        return "ui.state.running"_tr();
    case Stage::Created:
        return "ui.state.waiting"_tr();
    case Stage::Configured:
        return "ui.state.configured"_tr();
    case Stage::Off:
    default:
        return "ui.state.off"_tr();
    }
}

std::string overallName(Overall state) {
    switch (state) {
    case Overall::Running:
        return "ui.state.running"_tr();
    case Overall::Error:
        return "ui.state.error"_tr();
    case Overall::Waiting:
    default:
        return "ui.state.waitingFrame"_tr();
    }
}

ImU32 overallColor(Overall state) {
    switch (state) {
    case Overall::Running:
        return kGreen;
    case Overall::Error:
        return kRed;
    case Overall::Waiting:
    default:
        return kAmber;
    }
}

// What is upscaling the picture this second (docs/specs/ui.md §3). Shared by the
// pipeline strip, the left column and the SR page so the three cannot disagree.
// Only a backend that really carries the frame gets the accent; TAAU is the engine
// doing its own thing, so it reads dim rather than competing with the substitute.
std::string backendName(Live const& live, ImU32* tint = nullptr) {
    bool const substituting =
        live.backend.backend == sr::Backend::VvDlss && vv::sr::enabled() && live.vvStats.recorded;

    std::string name  = "ui.sr.backendNone"_tr();
    // No upscaler is a normal state (main menu, 100% native), not an error.
    ImU32       color = kTextDim;
    if (live.backend.backend == sr::Backend::EngineDlss) {
        name  = "ui.sr.backendEngine"_tr();
        color = kAccent;
    } else if (substituting) {
        name  = "ui.sr.backendVv"_tr();
        color = kAccent;
    } else if (live.backend.backend == sr::Backend::VvDlss || live.backend.vvConfigured || live.backend.vvSeen) {
        // The engine's own TAAU: a fact, not an achievement of ours.
        name  = "ui.sr.backendTauu"_tr();
        color = kTextDim;
    }
    if (tint) *tint = color;
    return name;
}

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

// DLSS model-preset naming. Values are the measured ones from docs/I §10: the preset
// hint carries 0x80000000 | (5=E, 6=F, 10=J, 11=K, 12=L, 13=M).
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

// Render size as a percentage of the presented size: the one number that says
// whether the ratio control actually landed in the frame. "-" until the engine has
// published a pair of extents (no world frame yet).
std::string ratioName(unsigned renderWidth, unsigned displayWidth) {
    if (renderWidth == 0 || displayWidth == 0) return "-";
    char buffer[16]{};
    std::snprintf(
        buffer,
        sizeof(buffer),
        "%d%%",
        static_cast<int>(std::lround(100.0 * static_cast<double>(renderWidth) / static_cast<double>(displayWidth)))
    );
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
//
// The dot is a conclusion about the pipeline as a whole, and it is built from what
// actually ran: a feature that exists but has not produced a frame reads "waiting",
// not "healthy", and a deliberately disabled NR does not read as "off".

void drawHeader(Layout const& layout, Live const& live) {
    auto*      drawList = ImGui::GetWindowDrawList();
    auto const s        = layout.scale;

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

    // One dot for the whole pipeline, not for NR alone (see Overall): an NR the user
    // turned off on purpose cannot turn this into "off" or "error" while SR runs.
    std::string const stateText  = overallName(overallState(live));
    ImU32 const       stateColor = overallColor(overallState(live));

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

    // The zoom buttons are the same widget as every other button; only their size is
    // smaller, so the padding is dropped instead of being pushed by hand.
    at(ImVec2(zoomX, zoomY));
    if (menuButton("-##zoomOut", ImVec2(button, button))) theme::setUserScale(theme::userScale() - 0.05f);
    at(ImVec2(zoomX + button + gap, zoomY + 2.0f * s));
    ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(kTextDim), "%s", zoomText);
    at(ImVec2(zoomX + button + gap + zoomWidth + gap, zoomY));
    if (menuButton("+##zoomIn", ImVec2(button, button))) theme::setUserScale(theme::userScale() + 0.05f);

    float const ruleY = layout.pos.y + layout.pad + layout.headerHeight - 10.0f * s;
    drawList->AddLine(
        ImVec2(layout.pos.x + layout.pad, ruleY),
        ImVec2(right, ruleY),
        kBorderSoft,
        std::max(1.0f, 1.0f * s)
    );
}

// Pipeline strip ------------------------------------------------------------
//
// Four boxes, and each one only claims what its own read-back proves: the world and
// output boxes need published sizes, the NR box needs its evaluation counter to
// move, the upscaler box needs a live backend. A box that is merely configured is
// drawn dim with the state spelled out - that is the difference between "it is on in
// config.json" and "it ran a frame".

void drawPipeline(Layout const& layout, Live const& live) {
    auto const s = layout.scale;

    float const top    = layout.pos.y + layout.pad + layout.headerHeight;
    float const height = layout.pipelineHeight - 18.0f * s;
    float const left   = layout.pos.x + layout.pad;
    float const width  = layout.size.x - layout.pad * 2.0f;

    constexpr int kArrows    = 3;
    float const   arrowWidth = 30.0f * s;
    float const   nodeWidth  = (width - static_cast<float>(kArrows) * arrowWidth) / 4.0f;

    // Sizes come from `sr::status()`, not from the RT-only PanelStats: it normalises per
    // backend (the ray-traced branch maps the same PanelStats, the VV branch reports the
    // engine's TAAU declaration sizes), so the strip shows real numbers under Vibrant
    // Visuals too (2026-10-29 report: VV had GPU time but no sizes).
    char render[48]{};
    char output[48]{};
    std::snprintf(render, sizeof(render), "%u x %u", live.backend.renderWidth, live.backend.renderHeight);
    std::snprintf(output, sizeof(output), "%u x %u", live.backend.displayWidth, live.backend.displayHeight);

    // NR box: where the feature actually runs, and what state it is in. The placement is
    // read back from the render thread's own record (`PanelStats.placement`) - an after
    // side that was refused reports the before side it really ran. A feature that was
    // never created has no placement, so the first line stays empty instead of inventing
    // one, and the state line carries the "not created yet" answer. Numbers (GPU ms,
    // counters) live in the LIVE column; this box answers "where" and "in what state".
    std::string nrPlacement;
    if (live.nr != Stage::Off && live.nr != Stage::Configured) {
        bool const after = live.stats.placement == 1 && !live.stats.afterFallback;
        nrPlacement      = after ? "ui.pipeline.nrAfter"_tr() : "ui.pipeline.nrBefore"_tr();
    }
    std::string const nrState = stageName(live.nr);

    // Upscaler: who does it, which preset its feature was created with, and the ratio
    // the frame is *actually* rendered at (render / display, not the setting). The
    // preset is read back from the feature itself - the engine's parameter object on
    // RT, our own CreateFeature value on VV - so a control that did not take shows up
    // here instead of echoing the config. TAAU has no preset and says so. No frame
    // counts: the box answers "what" and "how much", not "how many".
    std::string upName;
    std::string upLine;
    bool        upActive = live.upscale == Stage::Running;
    switch (live.backend.backend) {
    case sr::Backend::EngineDlss:
        upName = "ui.pipeline.upEngine"_tr();
        upLine = presetName(live.srStats.preset) + " · "
               + ratioName(live.backend.renderWidth, live.backend.displayWidth);
        break;
    case sr::Backend::VvDlss: {
        bool const substituting = vv::sr::enabled() && live.vvStats.recorded;
        upName                  = substituting ? "ui.pipeline.upVv"_tr() : "ui.pipeline.upTauu"_tr();
        if (substituting) {
            upLine = presetName(static_cast<unsigned>(live.dlss.preset)) + " · "
                   + ratioName(live.backend.renderWidth, live.backend.displayWidth);
        } else {
            upLine = "ui.pipeline.noPreset"_tr() + " · "
                   + ratioName(live.backend.renderWidth, live.backend.displayWidth);
        }
        break;
    }
    default:
        // Nothing is upscaling. A VV-configured engine at 100% is real but invisible
        // to observation, so the left column carries the explanation.
        upName   = "ui.pipeline.upNone"_tr();
        upActive = false;
        break;
    }

    std::string const titles[4] = {
        "ui.pipeline.world"_tr(),
        "ui.pipeline.nr"_tr(),
        "ui.pipeline.upscale"_tr(),
        "ui.pipeline.output"_tr(),
    };
    std::string const outSub    = "ui.pipeline.outputSub"_tr();
    std::string const rasterSub = "ui.pipeline.raster"_tr();

    // A size is the evidence for the two outer boxes: no extent for the live backend
    // means it has not handed us a frame yet (same source as the text above).
    bool const worldActive = live.backend.renderWidth != 0 && live.backend.renderHeight != 0;
    bool const outActive   = live.backend.displayWidth != 0 && live.backend.displayHeight != 0;

    struct Node {
        char const* title{};
        char const* line1{};
        char const* line2{};
        bool        active{};
    };
    Node const worldNode{titles[0].c_str(), render,              rasterSub.c_str(), worldActive               };
    Node const nrNode   {titles[1].c_str(), nrPlacement.c_str(), nrState.c_str(),   live.nr == Stage::Running };
    Node const srNode   {titles[2].c_str(), upName.c_str(),      upLine.c_str(),    upActive                  };
    Node const outNode  {titles[3].c_str(), output,              outSub.c_str(),    outActive                 };

    // The two middle boxes swap places with the actual placement: before upscaling the
    // picture goes World -> NR -> SR -> Output, after upscaling it goes World -> SR ->
    // NR -> Output. A refused after side is the before side the render thread really
    // ran, so it takes the before order (`placement` is already 0 then, the explicit
    // check keeps the intent readable). The first frame after the panel opens snaps to
    // the current order instead of replaying the move; later changes animate (~0.25 s)
    // so a placement change reads as a swap rather than a jump.
    float const target = (live.stats.placement == 1 && !live.stats.afterFallback) ? 1.0f : 0.0f;
    if (!gSwapPrimed) {
        gSwapProgress = target;
        gSwapPrimed   = true;
    } else {
        // A hitch must not make the boxes jump: cap the step at the frame delta.
        float const dt   = std::min(ImGui::GetIO().DeltaTime, 0.1f);
        float const step = dt / kSwapSeconds;
        gSwapProgress    = std::clamp(gSwapProgress + std::clamp(target - gSwapProgress, -step, step), 0.0f, 1.0f);
    }
    // smoothstep: ease out of one slot and into the other.
    float const t = gSwapProgress * gSwapProgress * (3.0f - 2.0f * gSwapProgress);

    auto const  slotX   = [&](int index) { return left + static_cast<float>(index) * (nodeWidth + arrowWidth); };
    float const nrX     = slotX(1) + (slotX(2) - slotX(1)) * t;
    float const srX     = slotX(2) + (slotX(1) - slotX(2)) * t;
    bool const  nrFirst = t < 0.5f; // which of the two middle slots holds NR right now

    // Arrows first, so a sliding box covers a gap arrow instead of being covered by it.
    // They always point right: the middle arrow connects whatever sits in the left slot
    // to whatever sits in the right slot, which is the real flow in both orders - the
    // swap therefore never shows a reversed arrow.
    float const midY = top + height * 0.5f;
    for (int index = 0; index < 3; ++index) {
        bool active = outNode.active;
        if (index == 0) active = nrFirst ? nrNode.active : srNode.active;
        else if (index == 1) active = nrFirst ? srNode.active : nrNode.active;
        float const fromX = slotX(index) + nodeWidth + 5.0f * s;
        float const toX   = slotX(index) + nodeWidth + arrowWidth - 2.0f * s;
        widgets::nodeArrow(ImVec2(fromX, midY), ImVec2(toX, midY), active);
    }

    auto const drawNode = [&](Node const& node, float x) {
        widgets::nodeBox(ImVec2(x, top), ImVec2(nodeWidth, height), node.title, node.line1, node.line2, node.active);
    };
    drawNode(worldNode, slotX(0));
    drawNode(nrNode, nrX);
    drawNode(srNode, srX);
    drawNode(outNode, slotX(3));
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
//
// Read-back only: nothing here may read gDraft (the panel's staging copy) or a config
// flag as if it were a result. What is left is what a player asks the panel: is it
// running, what is upscaling, at which sizes, at what frame rate and GPU cost. The
// per-feature counters moved to the diagnostics page (and most of them were deleted,
// 2026-10-29).

void drawStatsColumn(Layout const& layout, Live const& live) {
    auto const s       = layout.scale;
    auto const frame   = Overlay::getInstance().frameStats();
    auto const history = Overlay::getInstance().recentFrameMs();

    columnCaption("ui.live.header"_tr().c_str());
    ImGui::Spacing();

    widgets::sectionHeader("ui.stats.frames"_tr().c_str());

    char buffer[96]{};
    std::snprintf(buffer, sizeof(buffer), "%.1f fps", frame.fps);
    widgets::statRow("ui.stats.fps"_tr().c_str(), buffer);
    std::snprintf(buffer, sizeof(buffer), "%.2f ms", frame.frameMs);
    widgets::statRow("ui.stats.frameTime"_tr().c_str(), buffer);

    ImVec2 const graphSize{ImGui::GetContentRegionAvail().x, 48.0f * s};
    auto const   graphPos = ImGui::GetCursorScreenPos();
    widgets::frameGraph(graphPos, graphSize, history, autoGraphMax(history));
    ImGui::Dummy(ImVec2(graphSize.x, graphSize.y + 4.0f * s));

    widgets::sectionHeader("ui.stats.pipeline"_tr().c_str());

    // The column's one conclusion: the pipeline as a whole (see Overall), so NR being
    // off on purpose does not read as "the mod is off".
    Overall const state = overallState(live);
    widgets::statRow("ui.stats.status"_tr().c_str(), overallName(state).c_str(), overallColor(state));

    // Which stage is upscaling the picture right now - the same answer the strip and
    // the SR page use.
    ImU32             tint = 0;
    std::string const up   = backendName(live, &tint);
    widgets::statRow("ui.sr.backendNow"_tr().c_str(), up.c_str(), tint);

    // Sizes come from the normalised backend snapshot (same as the strip): the RT branch
    // maps PanelStats, the VV branch reports the engine's TAAU declaration sizes.
    std::snprintf(buffer, sizeof(buffer), "%u x %u", live.backend.renderWidth, live.backend.renderHeight);
    widgets::statRow("ui.stats.renderSize"_tr().c_str(), buffer);
    std::snprintf(buffer, sizeof(buffer), "%u x %u", live.backend.displayWidth, live.backend.displayHeight);
    widgets::statRow("ui.stats.outputSize"_tr().c_str(), buffer);

    // What the two stages cost, in the panel's normal text colour. A measured time is
    // a measurement, not an alarm: NR and SR read the same white, and only "no
    // sample" is dimmed.
    widgets::sectionHeader("ui.stats.gpu"_tr().c_str());
    if (!live.stats.stampsActive) {
        widgets::statRow("ui.stats.nrGpu"_tr().c_str(), "ui.diag.timingOff"_tr().c_str(), kTextDim);
        widgets::statRow("ui.stats.srGpu"_tr().c_str(), "ui.diag.timingOff"_tr().c_str(), kTextDim);
    } else {
        if (live.stats.hasNr) {
            std::snprintf(buffer, sizeof(buffer), "%.2f ms", live.stats.nrMs);
            widgets::statRow("ui.stats.nrGpu"_tr().c_str(), buffer);
        } else {
            widgets::statRow("ui.stats.nrGpu"_tr().c_str(), "ui.stats.noSamples"_tr().c_str(), kTextDim);
        }
        if (live.stats.hasEngine) {
            std::snprintf(buffer, sizeof(buffer), "%.2f ms", live.stats.engineMs);
            widgets::statRow("ui.stats.srGpu"_tr().c_str(), buffer);
        } else {
            widgets::statRow("ui.stats.srGpu"_tr().c_str(), "ui.stats.noSamples"_tr().c_str(), kTextDim);
        }
    }
}

// Settings columns ----------------------------------------------------------

void drawTabNeural() {
    // One composite, no choice: TRP's Ratio resolve is the only one that ships
    // (2026-10-29). The gain composite's controls are gone with it, so the draft is
    // pinned here instead of in applyDraft - everything else applyDraft writes keeps
    // its old meaning.
    gDraft.replace = 6;

    widgets::sectionHeader("ui.neural.enableSection"_tr().c_str());

    if (widgets::toggle("ui.neural.enable"_tr().c_str(), &gDraft.enabled)) markDirty();
    // Asked for explicitly (2026-10-29): the two consequences a user cannot see - the
    // history restart and what "save as default" does while it is off.
    widgets::tip("ui.neural.enableTip"_tr().c_str());
    // A state, not an explanation: while this is off everything below is grey and the
    // reason has to be visible without hovering anything.
    if (!gDraft.enabled) widgets::hint("ui.neural.disabledHint"_tr().c_str());

    if (!gDraft.enabled) ImGui::BeginDisabled();

    // Position axis (docs/N): the two sides are exclusive, and the cost difference
    // belongs in the hint (the after side's ~9.6 ms is not a regression).
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
        // Asked for explicitly: the after side's cost and the rebuild/reset a switch costs.
        widgets::tip("ui.neural.placementTip"_tr().c_str());
    }
    widgets::hint(
        gDraft.placement == 1 ? "ui.neural.placementAfterHint"_tr().c_str() : "ui.neural.placementHint"_tr().c_str()
    );
    // The fallback is a fact about this session, so it stays inline next to it.
    if (gDraft.placement == 1 && nr::nrAfterFallback()) {
        widgets::hint("ui.neural.afterFallbackHint"_tr().c_str());
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
    // the constant above. Only Replace=6 normalises against it in the resolve.
    if (widgets::toggle("ui.neural.adaptiveExposure"_tr().c_str(), &gDraft.adaptiveExposure)) markDirty();
    if (gDraft.adaptiveExposure) {
        if (widgets::slider("ui.neural.exposureBias"_tr().c_str(), &gDraft.exposureBias, -2.0f, 2.0f, "%+.2f", "EV")) {
            markDirty();
        }
    }
}

void drawTabSr(Live const& live) {
    // Which upscaler is actually running decides what this page can offer: the RT path
    // owns the engine's DLSS feature, the VV path owns ours, and with neither there is
    // nothing to control (docs/specs/ui.md §3). Detection is observation - a settings
    // value cannot tell "TAAU at 100%" from "TAAU running" (EXP-0081). The status comes
    // from the frame sample, so this page and the left column can never disagree.
    auto const& backend = live.backend;
    // Not `rt`/`vv`: `vv` is a namespace here (`vv::sr`, `vv::dlss`), and a local of
    // that name hides it in this scope.
    bool const enginePath = backend.backend == sr::Backend::EngineDlss;
    bool const vvPath     = backend.backend == sr::Backend::VvDlss;
    bool const any        = enginePath || vvPath;
    // The VV ratio is also the way back from the one state where nothing runs: at
    // 100% the upscaler is gone, so the control must stay reachable - and the engine's
    // own setting is what says this machine is a VV machine even then.
    bool const ratioEditable = any || backend.vvConfigured || backend.vvSeen;
    // Everything that only exists on our own pipeline (the master switch, its ratio,
    // the DLSS-side jitter) shares this gate: on the ray-traced path the engine owns
    // DLSS, so offering them there would be a control with no effect.
    bool const vvMode = vvPath || (!enginePath && (backend.vvConfigured || backend.vvSeen));

    auto const sr     = live.srStats;
    auto const ps     = live.stats;
    auto const vvDlss = live.dlss;

    char buffer[128]{};

    widgets::sectionHeader("ui.sr.backend"_tr().c_str());
    {
        // The badge answers "what is upscaling the picture right now", not "which path
        // could be substituted" - the same shared answer the left column shows.
        ImU32             tint = 0;
        std::string const up   = backendName(live, &tint);
        widgets::statRow("ui.sr.backendNow"_tr().c_str(), up.c_str(), tint);
        if (!any) {
            if (backend.vvConfigured) {
                // Observation cannot explain this one; the engine's own setting can.
                if (backend.configuredPercent >= 0) {
                    std::snprintf(buffer, sizeof(buffer), "%d", backend.configuredPercent);
                } else {
                    std::snprintf(buffer, sizeof(buffer), "?");
                }
                widgets::hint("ui.sr.backendConfigured"_tr(buffer).c_str());
            } else if (!backend.live && backend.ageMs != 0) {
                std::snprintf(buffer, sizeof(buffer), "%u s", backend.ageMs / 1000u);
                widgets::hint("ui.sr.backendLost"_tr(buffer).c_str());
            } else {
                widgets::hint("ui.sr.backendNever"_tr().c_str());
            }
        }
    }

    // The resolution row is deliberately outside the disabled block below: it is the
    // only way back from the state where nothing runs (100% = no upscaler), so it gets
    // its own gate. Everything else is a write into a feature that does not exist
    // without a backend.
    ImGui::Spacing();
    widgets::sectionHeader("ui.sr.internal"_tr().c_str());
    if (vvMode) {
        // The master switch: under VV the engine's upscaler is TAAU, so "enabled" means
        // our DLSS writes TAAU's output and the engine's TAAU draw is skipped.
        if (widgets::toggle("ui.sr.vvEnable"_tr().c_str(), &gDraftSr.vvEnabled)) {
            markDirty();
        }
        ImGui::Spacing();
        // VV: the render thread writes UpscalingData.factor before the camera build
        // (probe/VvUpscaling.cpp), so this is a real control and the engine option is
        // left alone. 100% means native render: the hook restores a <1 branch factor
        // after the camera so TAAU/arg10 stay declared, and DLSS runs 1:1 with NR at
        // native resolution (EXP-0090).
        std::string const vvItems[6] = {
            "ui.sr.scaleAuto"_tr(),
            "33%",
            "50%",
            "58%",
            "66%",
            "100%",
        };
        char const* vvPointers[6] = {
            vvItems[0].c_str(),
            vvItems[1].c_str(),
            vvItems[2].c_str(),
            vvItems[3].c_str(),
            vvItems[4].c_str(),
            vvItems[5].c_str(),
        };
        int const vvValues[6] = {0, 33, 50, 58, 66, 100};
        if (!ratioEditable) {
            ImGui::BeginDisabled();
        }
        if (mappedCombo("ui.sr.scale"_tr().c_str(), &gDraftSr.vvPercent, vvPointers, vvValues, 6)) {
            markDirty();
        }
        if (!ratioEditable) {
            ImGui::EndDisabled();
        }
        widgets::hint("ui.sr.scaleHintVv"_tr().c_str());
    } else {
        // RT: only the ratios DLSS actually has a slot for; each maps 1:1 to the
        // engine's quality slot and the scene factor table is patched to match.
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
        if (!any) {
            ImGui::BeginDisabled();
        }
        if (mappedCombo("ui.sr.scale"_tr().c_str(), &gDraftSr.scalePercent, scalePointers, scaleValues, 6)) {
            markDirty();
        }
        if (!any) {
            ImGui::EndDisabled();
        }
        widgets::hint("ui.sr.scaleHint"_tr().c_str());
    }
    // What the write actually produced: scene render rect -> upscale target, with the
    // ratio those two imply. This is the pair that proves the override landed, so it
    // sits under the control that moves it.
    if (backend.renderWidth != 0 && backend.displayWidth != 0) {
        std::snprintf(
            buffer,
            sizeof(buffer),
            "%u x %u → %u x %u（%s）",
            backend.renderWidth,
            backend.renderHeight,
            backend.displayWidth,
            backend.displayHeight,
            ratioName(backend.renderWidth, backend.displayWidth).c_str()
        );
        widgets::statRow("ui.sr.currentSize"_tr().c_str(), buffer);
    }

    if (!any) {
        ImGui::BeginDisabled();
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
        // The read-back differs per backend: the engine's parameter object on RT, our own
        // feature's CreateFeature value on VV (VvDlssSr.h stores it for exactly this).
        std::string preset;
        if (vvPath) {
            preset = vvDlss.ready ? presetName(static_cast<unsigned>(vvDlss.preset)) : "ui.sr.rebuilding"_tr();
        } else {
            preset = sr.rebuilding ? "ui.sr.rebuilding"_tr() : presetName(sr.preset);
        }
        bool const dim = (vvPath && !vvDlss.ready) || (enginePath && sr.rebuilding);
        widgets::statRow("ui.sr.preset"_tr().c_str(), preset.c_str(), dim ? kTextDim : 0);
    }

    ImGui::Spacing();
    widgets::sectionHeader("ui.sr.jitterSection"_tr().c_str());
    {
        // The sub-pixel offset handed to DLSS. Default Zero: the world frame is not
        // rendered with the TAA context's offset, so feeding it shifted the input every
        // frame (a visible wobble, EXP-0098). Engine follows the engine's current frame
        // - the A/B described in research/2026-10-29-taau-jitter-dlss-contract.md.
        //
        // Only our own DLSS has an offset we can choose; on the ray-traced path the
        // engine feeds its feature, so the control is greyed out there instead of
        // pretending to do something (2026-10-29 review).
        //
        // Staged like every other control: the combo only moves the draft, and
        // applyDraft() is what writes vv::sr. It has no config entry, so Apply affects
        // this run and "Save as default" does not carry it over.
        //
        // All four sources are offered again (2026-10-29): Zero is the shipping default,
        // Engine is the A/B, Negated/Previous are the discriminators from
        // research/2026-10-29-taau-jitter-dlss-contract.md §7 - the tip says so, because
        // neither is a better picture.
        std::string const jitterItems[4] = {
            "ui.sr.jitterZero"_tr(),
            "ui.sr.jitterEngine"_tr(),
            "ui.sr.jitterNegated"_tr(),
            "ui.sr.jitterPrevious"_tr(),
        };
        char const* jitterPointers[4] = {
            jitterItems[0].c_str(),
            jitterItems[1].c_str(),
            jitterItems[2].c_str(),
            jitterItems[3].c_str(),
        };
        int const jitterValues[4] = {
            static_cast<int>(vv::sr::JitterSource::Zero),
            static_cast<int>(vv::sr::JitterSource::Engine),
            static_cast<int>(vv::sr::JitterSource::Negated),
            static_cast<int>(vv::sr::JitterSource::Previous),
        };
        if (!vvMode) ImGui::BeginDisabled();
        if (mappedCombo("ui.sr.jitterSource"_tr().c_str(), &gDraftJitter, jitterPointers, jitterValues, 4)) {
            markDirty();
        }
        widgets::tip("ui.sr.jitterTip"_tr().c_str());
        if (!vvMode) ImGui::EndDisabled();
        widgets::hint(vvMode ? "ui.sr.jitterHint"_tr().c_str() : "ui.sr.jitterRtHint"_tr().c_str());
    }

    ImGui::Spacing();
    widgets::sectionHeader("ui.sr.sharpSection"_tr().c_str());
    // The DLSS "Sharpness" parameter is dead (docs/J §5), so sharpening is our own
    // RCAS pass. Live: the render thread reads the config every frame, so Apply is
    // enough - no feature rebuild, no restart.
    //
    // Both backends run the same pass: on RT it works on the engine's Output, on VV
    // on our DLSS output just before it is written into arg10 (docs/specs/sr.md §8).
    // The section is gated by the backend block above, so "no backend" greys it with
    // everything else.
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
        widgets::hint(enginePath ? "ui.sr.sharpHint"_tr().c_str() : "ui.sr.sharpHintVv"_tr().c_str());

        // Read-back (docs/L §0.7): what the render thread actually recorded, not
        // what the draft says. Its GPU time reads in the same white as every other
        // timing in the panel.
        if (sharp.enabled && sharp.strength > 0.0f) {
            if (ps.hasRcas) {
                std::snprintf(buffer, sizeof(buffer), "%.3f ms", ps.rcasMs);
                widgets::statRow("ui.sr.sharpGpu"_tr().c_str(), buffer);
            } else {
                widgets::statRow("ui.sr.sharpGpu"_tr().c_str(), "ui.stats.noSamples"_tr().c_str(), kTextDim);
            }
        } else {
            widgets::hint(sharp.enabled ? "ui.sr.sharpZeroHint"_tr().c_str() : "ui.sr.sharpOffHint"_tr().c_str());
        }
    }

    ImGui::Spacing();
    widgets::sectionHeader("ui.sr.actions"_tr().c_str());
    std::string resetLabel = "ui.sr.reset"_tr();
    resetLabel += "##srReset";
    if (menuButton(resetLabel.c_str())) {
        if (vvPath) {
            vv::sr::requestReset();
        } else {
            nr::srRequestReset();
        }
    }
    widgets::hint(enginePath ? "ui.sr.resetHint"_tr().c_str() : "ui.sr.resetHintVv"_tr().c_str());

    if (!any) {
        ImGui::EndDisabled();
    }
}

void drawTabDiagnostics() {
    // What the two stages cost, plus the sharpening pass while it is on. Nothing on
    // this page is a conclusion, and the block that used to be here (feature flags,
    // return codes, input formats, the per-frame parameter dump, the VV boundary
    // experiments and the history-reset switches) answered questions only a developer
    // asks (2026-10-29 requests).
    auto const stats = nr::nrPanelStats();
    auto const sharp = neurodragon::sr::sharpStats();

    widgets::sectionHeader("ui.diag.gpu"_tr().c_str());

    char buffer[96]{};
    if (!stats.stampsActive) {
        widgets::statRow("ui.diag.nr"_tr().c_str(), "ui.diag.timingOff"_tr().c_str(), kTextDim);
        widgets::statRow("ui.diag.dlss"_tr().c_str(), "ui.diag.timingOff"_tr().c_str(), kTextDim);
    } else {
        // Same white for NR and SR: a measured time is not an alarm.
        if (stats.hasNr) {
            std::snprintf(buffer, sizeof(buffer), "%.3f ms", stats.nrMs);
            widgets::statRow("ui.diag.nr"_tr().c_str(), buffer);
        } else {
            widgets::statRow("ui.diag.nr"_tr().c_str(), "ui.stats.noSamples"_tr().c_str(), kTextDim);
        }
        if (stats.hasEngine) {
            std::snprintf(buffer, sizeof(buffer), "%.3f ms", stats.engineMs);
            widgets::statRow("ui.diag.dlss"_tr().c_str(), buffer);
        } else {
            widgets::statRow("ui.diag.dlss"_tr().c_str(), "ui.stats.noSamples"_tr().c_str(), kTextDim);
        }
    }
    if (sharp.enabled && sharp.strength > 0.0f) {
        if (stats.hasRcas) {
            std::snprintf(buffer, sizeof(buffer), "%.3f ms", stats.rcasMs);
            widgets::statRow("ui.sr.sharpGpu"_tr().c_str(), buffer);
        } else {
            widgets::statRow("ui.sr.sharpGpu"_tr().c_str(), "ui.stats.noSamples"_tr().c_str(), kTextDim);
        }
    }

    // Clearing the published samples is the one readout control left: it answers "did
    // that number come from now, or from before I changed anything". The two
    // "reset history every frame" switches that used to sit below it are gone: they
    // only existed to break the picture on purpose (2026-10-29 review).
    ImGui::Spacing();
    std::string clearLabel = "ui.diag.clear"_tr();
    clearLabel += "##clearSamples";
    if (menuButton(clearLabel.c_str())) nr::nrStampClearReported();
    widgets::hint("ui.diag.hint"_tr().c_str());
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
        auto const  resetWidth = widgets::buttonWidth(resetText.c_str());
        auto const  keyWidth   = widgets::buttonWidth(text.c_str());
        // No reset button while capturing: the row should stay quiet (the preview
        // text is still growing).
        auto const totalWidth = capturing ? keyWidth : keyWidth + style.ItemInnerSpacing.x + resetWidth;

        ImGui::SameLine();
        ImGui::SetCursorPosX(widgets::rowRight() - totalWidth);

        bool const  conflicted = !capturing && severity != input::ConflictSeverity::None;
        ImU32 const border     = severity == input::ConflictSeverity::Exact ? kRed : kAmber;
        // The visible text cannot be the id: two rows can show the same text (both
        // "-", or the same key twice), and ImGui would then share one id between them.
        std::string const id      = text + "##hotkey" + std::to_string(index);
        bool const        clicked = menuButton(id.c_str());
        if (conflicted) {
            ImGui::GetWindowDrawList()->AddRect(
                ImGui::GetItemRectMin(),
                ImGui::GetItemRectMax(),
                border,
                4.0f * theme::scale(),
                0,
                std::max(1.0f, theme::scale())
            );
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
        bool const reset = menuButton((resetText + "##reset" + std::to_string(index)).c_str());
        if (!canReset) ImGui::EndDisabled();

        if (reset) {
            *row.keys = defaultKeys(row.hotkey);
            markDirty();
#ifdef TEST
            log::write(3, "[ui] hotkey reset: " + row.label + " -> " + row.keys->display());
#endif
        }
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
            ImGui::SetTooltip("%s", "ui.adv.resetHint"_tr().c_str());
        }
    }

    // The capture result arrives from the window thread: nullopt = nothing committed
    // (still capturing, or cancelled); engaged-and-empty = the player pressed Esc, an
    // explicit unbind; engaged-and-non-empty = the new combination.
    if (auto captured = overlay.takeCapturedKeys()) {
#ifdef TEST
        // TEST: every binding the capture path produces, including the explicit unbind,
        // so a "the hotkey does nothing" report can be settled from the log.
        log::write(3, "[ui] hotkey captured: " + captured->display());
#endif
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

    // SR jitter is the one staged value without a config entry, so this write *is* its
    // persistence: it reaches the render thread here and survives only this run.
    // setJitterSource() is a no-op when the value did not change (no history wipe), so
    // every Apply can write it unconditionally.
    vv::sr::setJitterSource(static_cast<vv::sr::JitterSource>(gDraftJitter));

    // A model preset is only read at CreateFeature, so applying a new one has to
    // make the feature rebuild. Each backend owns its own feature: the engine's on the
    // RT path, ours on the VV path. Asking the idle one is a no-op.
    if (config::get().sr.preset != oldPreset) {
        nr::srRequestFeatureRebuild();
        vv::dlss::requestFeatureRebuild();
    }

    // VV's ratio is applied on the render thread by the createDeferredRenderingCameras
    // hook (probe/VvUpscaling.cpp): it writes UpscalingData.factor before the camera
    // viewport is built. The player's upscaling_percentage option is deliberately not
    // touched, so "follow the engine" (0) still means the engine's own setting.

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

    float const      buttonHeight = 28.0f * s;
    std::string const saveLabel   = "ui.footer.saveDefault"_tr();
    float const      buttonWidth  = std::max(96.0f * s, widgets::buttonWidth(saveLabel.c_str()) + 4.0f * s);
    // The three buttons are one group, so the gap is the panel's normal control
    // spacing. It was widened while each button carried a `!` marker; the markers are
    // gone (2026-10-29), and the wider gap then read as three unrelated buttons.
    float const gap      = 12.0f * s;
    float const rightX   = layout.pos.x + layout.size.x - layout.pad;
    float const saveX    = rightX - buttonWidth;
    float const applyX   = saveX - gap - buttonWidth;
    float const discardX = applyX - gap - buttonWidth;

    // Left to right: discard, apply, save-as-default - the order the panel shipped with.
    // "Save as default" is the primary (filled) button on the right: it is the one that
    // writes config/config.json and therefore the strongest promise; "Apply" stays the
    // outlined secondary even though it is pressed more often. Both behaviours are
    // unchanged - only the position and the emphasis follow the original design again.
    at(ImVec2(discardX, top));
    if (menuButton("ui.footer.discard"_tr().c_str(), ImVec2(buttonWidth, buttonHeight))) {
        gDraft   = sanitizedNr(config::get().nr);
        gDraftSr = sanitizedSr(config::get().sr);
        reloadKeyDrafts();
        stageJitterDraft();
        gDirty = false;
        setStatus("ui.footer.discarded"_tr());
    }

    at(ImVec2(applyX, top));
    if (menuButton("ui.footer.apply"_tr().c_str(), ImVec2(buttonWidth, buttonHeight))) {
        applyDraft(false);
    }

    at(ImVec2(saveX, top));
    if (menuButton(saveLabel.c_str(), ImVec2(buttonWidth, buttonHeight), true)) {
        applyDraft(true);
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
        auto const live   = sampleLive();

        auto* drawList = ImGui::GetWindowDrawList();
        drawList->AddRectFilled(gPos, ImVec2(gPos.x + size.x, gPos.y + size.y), kPanel, 6.0f * s);
        drawList->AddRect(gPos, ImVec2(gPos.x + size.x, gPos.y + size.y), kBorder, 6.0f * s, 0, std::max(1.0f, s));

        drawHeader(layout, live);

        // Drag affordance: the title strip, minus anything interactive on it (the
        // zoom buttons). Checked after the header so IsAnyItemHovered() already
        // knows about them.
        ImVec2 const stripMax{gPos.x + size.x, gPos.y + layout.pad + layout.headerHeight};
        bool const   overStrip = ImGui::IsMouseHoveringRect(gPos, stripMax) && !ImGui::IsAnyItemHovered();
        if (overStrip && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) gDragging = true;
        if (overStrip || gDragging) ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeAll);

        drawPipeline(layout, live);
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
            drawStatsColumn(layout, live);
        }
        ImGui::EndChild();

        at(ImVec2(rightX, layout.bodyTop));
        if (ImGui::BeginChild(
                "##settingsColumn",
                ImVec2(rightW, layout.bodyHeight),
                ImGuiChildFlags_AlwaysUseWindowPadding
            )) {
            // The two columns are different things, and the panel says so: the left one
            // reads the renderer, this one stages changes for it.
            columnCaption("ui.controls.header"_tr().c_str());
            widgets::hint("ui.controls.hint"_tr().c_str());
            ImGui::Spacing();

            switch (gActiveTab) {
            case 0:
                drawTabNeural();
                break;
            case 1:
                drawTabSr(live);
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
    // values. Jitter has no config entry to import, so its running value is the only
    // source.
    gDraft   = sanitizedNr(config::get().nr);
    gDraftSr = sanitizedSr(config::get().sr);
    stageJitterDraft();
    reloadKeyDrafts();
    gModelFiles = utils::listNrModelFiles();
    gDirty      = false;
    gStatus.clear();

    // The pipeline strip shows the current NR/SR order immediately; the swap animation is
    // for changes made while the panel is open.
    gSwapPrimed = false;

    // Counters are re-primed so "growth since the panel was opened" starts at zero
    // instead of inheriting the previous session's numbers.
    gNrEvals = CounterWatch{};
    gSrEvals = CounterWatch{};
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
