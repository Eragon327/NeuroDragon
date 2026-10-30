#pragma once

#include <string>

namespace neurodragon::fg::probe {

/// Milliseconds since process creation (0 when the process times are unavailable).
/// Used by the F0 diagnostics to put factory/swap-chain creation, probe install and
/// the first flip on one timeline.
double msSinceProcessStart();

/// F0-a: record when dxgi factories and swap chains are created relative to mod
/// load, who created them (module + offset) and with which description. TEST only;
/// a release build installs nothing.
///
/// `modDir` is only stored: Streamline's slInit runs on the first *game* factory
/// call, not here. Calling LoadLibrary/WinVerifyTrust from load() deadlocked the
/// client (EXP-0122/0123: 0-byte log, no window), which is the loader-lock shape.
void install(std::wstring const& modDir);
void uninstall();

} // namespace neurodragon::fg::probe
