#pragma once

#include "ll/api/io/Logger.h"
#include "ll/api/mod/NativeMod.h"

#include <Windows.h>

#include <filesystem>
#include <string>
#include <vector>

namespace neurodragon::utils {

// The only output path of this mod: LeviLamina's logger for this mod. Nothing
// writes to stdout, a file or the debugger directly.
ll::io::Logger& logger();

// Debug line in fmt style ("{}"), compiled out when TEST is not defined
// (src/neurodragon/Test.h). A dropped macro does not evaluate its arguments, so
// a release build pays neither formatting nor the expression's own cost.
#ifdef TEST
#    define ND_DEBUG(...) ::neurodragon::utils::logger().debug(__VA_ARGS__)
#else
#    define ND_DEBUG(...) ((void) 0)
#endif

std::wstring applicationDirectory();

std::string fileSizeText(std::filesystem::path const& path);

// Materialises the NR model next to the game executable under the name the NGX core
// looks for (`nvngx_dlssnr.dll`). 'sourceName' is the file in the mod directory (the
// panel can pick among several builds); 'replaceExisting' replaces an already
// materialised copy so a different pick takes effect on the next start.
bool ensureApplicationModel(
    ll::mod::NativeMod&          mod,
    std::filesystem::path const& sourceName,
    bool                         replaceExisting
);

// File names in the mod directory that look like NR model builds
// (`nvngx_dlssnr*.dll`), UTF-8, sorted. Used by the panel's runtime picker.
std::vector<std::string> listNrModelFiles();

} // namespace neurodragon::utils
