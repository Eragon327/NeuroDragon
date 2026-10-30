#pragma once

namespace neurodragon::fg::probe {

/// Milliseconds since process creation (0 when the process times are unavailable).
/// Used by the F0 diagnostics to put factory/swap-chain creation, probe install and
/// the first flip on one timeline.
double msSinceProcessStart();

/// F0-a: record when dxgi factories and swap chains are created relative to mod
/// load, who created them (module + offset) and with which description. TEST only;
/// a release build installs nothing.
void install();
void uninstall();

} // namespace neurodragon::fg::probe
