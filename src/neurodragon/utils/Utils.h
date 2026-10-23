#pragma once

#include "ll/api/io/Logger.h"
#include "ll/api/mod/NativeMod.h"

#include <Windows.h>

#include <filesystem>
#include <string>

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

bool ensureApplicationModel(ll::mod::NativeMod& mod, wchar_t const* fileName);

} // namespace neurodragon::utils
