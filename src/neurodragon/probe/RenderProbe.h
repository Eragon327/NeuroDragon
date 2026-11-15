#pragma once

#include <cstdint>

namespace neurodragon::probe {

/// Locate dragon::framerenderer::modules::taau without any PDB and hook it.
/// @return True if the function was located and the hook was installed.
bool install();

/// How many NGX EvaluateFeature calls have been seen so far. Zero means the NR
/// detour has never run; in VV that is "VV super resolution is off or never
/// injected", because the engine only loads NGX in RT mode (EXP-0118).
[[nodiscard]] std::uint64_t nrEvaluateSerial();

struct NrDiagnostic {
    std::uint64_t evaluateSerial{};
    unsigned      gateMask{};
    bool          paramsValid{};
    bool          enabled{};
    bool          startTried{};
};

// State captured by the last Evaluate detour, for the temporary VV heartbeat.
[[nodiscard]] NrDiagnostic nrDiagnostic();

// VV can load NGX after the watcher's timed wait has ended. Make hook installation
// available to that path as well; repeated calls are harmless.
bool ensureNgxHooks();

#ifdef TEST
// Capture base / encoded input / raw NR output / resolved colour on the next
// replacement frame. The screenshot hotkey requests this alongside its normal F2.
void requestNrToneDump();
#endif

void uninstall();

[[nodiscard]] bool isInstalled();

} // namespace neurodragon::probe
