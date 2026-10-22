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

std::wstring applicationDirectory();

std::string fileSizeText(std::filesystem::path const& path);

bool ensureApplicationModel(ll::mod::NativeMod& mod, wchar_t const* fileName);

} // namespace neurodragon::utils
