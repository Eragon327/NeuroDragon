#pragma once

namespace neurodragon::probe {

/// Locate dragon::framerenderer::modules::taau without any PDB and hook it.
/// @return True if the function was located and the hook was installed.
bool install();

void uninstall();

[[nodiscard]] bool isInstalled();

} // namespace neurodragon::probe
