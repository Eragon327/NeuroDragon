#include "neurodragon/sr/SrStatus.h"

#include "neurodragon/nr/D3D12Bridge.h"
#include "neurodragon/probe/RuntimeOptions.h"
#include "neurodragon/probe/VvProbe.h"
#include "neurodragon/vv/VvSr.h"

#include <chrono>
#include <cstdint>

namespace neurodragon::sr {
namespace {

// Two frames of staleness is a hitch, three seconds is "the game stopped feeding
// this path". The window also covers the pause menu and a level load, where the
// counters pause without the backend being gone.
constexpr auto kFreshWindow = std::chrono::milliseconds(3000);
// Flipping the panel's layout costs a read; a half-second of agreement keeps a
// single skipped frame from redrawing the tab with all controls disabled.
constexpr int kAgreeFrames = 30;

using Clock = std::chrono::steady_clock;

struct Detect {
    std::uint64_t      vvDeclared{};
    unsigned           engineEvals{};
    Clock::time_point  vvSeen{};
    Clock::time_point  engineSeen{};
    bool               vvEver{false};
    bool               engineEver{false};
    Backend            current{Backend::None};
    Backend            candidate{Backend::None};
    int                agree{0};
};

Detect& detect() {
    static Detect instance;
    return instance;
}

bool freshSince(Clock::time_point now, Clock::time_point seen) {
    return seen.time_since_epoch().count() != 0 && now - seen < kFreshWindow;
}

unsigned ageOf(Clock::time_point now, Clock::time_point seen) {
    if (seen.time_since_epoch().count() == 0) {
        return 0;
    }
    auto const ms = std::chrono::duration_cast<std::chrono::milliseconds>(now - seen).count();
    return ms > 0 ? static_cast<unsigned>(ms) : 0;
}

} // namespace

Status status() {
    auto&      d   = detect();
    auto const now = Clock::now();

    auto const taau   = probe::vv::taauDrawStats();
    auto const engine = nr::srPanelStats();

    // Declarations, not draws: our own draw-level suppress removes TAAU's draw while
    // the pass stays declared (EXP-0067), so a draw-based signal would report "no VV"
    // exactly while the mod is substituting that pass.
    if (taau.declared != d.vvDeclared) {
        d.vvDeclared = taau.declared;
        d.vvSeen     = now;
        d.vvEver     = true;
    }
    if (engine.evalCount != d.engineEvals) {
        d.engineEvals = engine.evalCount;
        d.engineSeen  = now;
        d.engineEver  = true;
    }

    bool const vvFresh     = d.vvEver && freshSince(now, d.vvSeen);
    bool const engineFresh = d.engineEver && freshSince(now, d.engineSeen);

    // The two never run at once (docs/specs/vv.md §1); the tie-break only exists so a
    // renderer switch mid-session has a defined answer instead of a coin flip.
    Backend candidate = Backend::None;
    if (vvFresh && engineFresh) {
        candidate = d.vvSeen >= d.engineSeen ? Backend::VvDlss : Backend::EngineDlss;
    } else if (vvFresh) {
        candidate = Backend::VvDlss;
    } else if (engineFresh) {
        candidate = Backend::EngineDlss;
    }

    if (candidate == d.candidate) {
        if (d.agree < kAgreeFrames) {
            ++d.agree;
        }
    } else {
        d.candidate = candidate;
        d.agree     = 0;
    }
    // Adopting a backend is immediate (the panel is usually opened on a live frame);
    // losing one waits for the agreement window.
    if (d.agree >= kAgreeFrames || (d.current == Backend::None && candidate != Backend::None)) {
        d.current = candidate;
    }

    Status s{};
    s.backend    = d.current;
    s.live       = candidate != Backend::None;
    s.vvSeen     = d.vvEver;
    s.engineSeen = d.engineEver;
    {
        auto const known      = probe::options::knownValues();
        s.vvConfigured        = known.graphicsMode == 2;
        s.configuredPercent   = known.upscalingPercent;
    }
    if (!s.live) {
        Clock::time_point const last = d.vvSeen > d.engineSeen ? d.vvSeen : d.engineSeen;
        s.ageMs = ageOf(now, last);
    }

    if (s.backend == Backend::VvDlss) {
        // The probe's numbers describe the engine (published on every TAAU
        // declaration), so they exist before our own pipeline is armed.
        s.renderWidth  = probe::vv::renderWidth();
        s.renderHeight = probe::vv::renderHeight();
        s.displayWidth = probe::vv::displayWidth();
        s.displayHeight = probe::vv::displayHeight();
        s.featureReady = vv::sr::stats().dlssReady;
    } else if (s.backend == Backend::EngineDlss) {
        auto const panel = nr::nrPanelStats();
        s.renderWidth    = panel.renderWidth;
        s.renderHeight   = panel.renderHeight;
        s.displayWidth   = panel.outputWidth;
        s.displayHeight  = panel.outputHeight;
        s.featureReady   = panel.featureCreated;
    }
    return s;
}

} // namespace neurodragon::sr
