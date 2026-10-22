#pragma once

#include <fmt/format.h>

#include <string>
#include <string_view>

namespace neurodragon::log {

// 0 = info, 1 = warn, 2 = error, 3 = debug. Every line ends up in LeviLamina's
// logger for this mod; declared without any ll/engine include because
// D3D12Bridge.cpp must stay free of those headers (<d3d12.h> conflicts with
// mc/_HeaderOutputPredefine.h, docs/D §4 pitfall 5 - the i18n header pulls those
// same engine headers in, so even "_tr" cannot be used there).
void write(int level, std::string_view message);

// Release-build text from inside the D3D12 translation unit. Same lookup and
// formatting as `"key"_tr(args...)`; a missing key logs the key itself so a
// broken catalogue is visible instead of printing nothing.
void writeKeyArgs(int level, std::string_view key, fmt::format_args args);

template <class... Args>
void writeKey(int level, std::string_view key, Args&&... args) {
    writeKeyArgs(level, key, fmt::make_format_args(args...));
}

// Localised text without arguments (e.g. one NGX result code -> its name).
std::string text(std::string_view key);

} // namespace neurodragon::log
