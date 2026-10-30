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

void uninstall();

[[nodiscard]] bool isInstalled();

} // namespace neurodragon::probe
