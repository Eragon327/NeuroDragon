#include "neurodragon/Config.h"

#include "neurodragon/utils/Utils.h"

#include "ll/api/Config.h"

#include <filesystem>
#include <system_error>

namespace neurodragon::config {
namespace {

Config gConfig;

std::filesystem::path configPath() {
    auto       dir = ll::mod::NativeMod::current()->getConfigDir();
    std::error_code error;
    std::filesystem::create_directories(dir, error);
    return dir / "config.json";
}

// Diagnostic switches are session-only. A split seam is saved like any other key
// (the whole struct is reflected), but a forgotten one would then be baked into every
// frame of the next session with nothing on screen explaining it - so it is cleared
// after load, the way TRP keeps its Compare out of its config. The position stays.
void clearSessionOnly() { gConfig.nr.compare = 0; }

} // namespace

Config& get() { return gConfig; }

bool loadOrCreate() {
    auto const path = configPath();

    // A malformed file makes loadConfig throw (it parses json eagerly); keep the
    // defaults and rewrite in that case instead of taking the mod down. There is no
    // migration for old values: the renderer treats everything except 5 as the ratio
    // resolve, so a stale config cannot silently leave NR off-screen.
    try {
        if (!ll::config::loadConfig(gConfig, path)) {
            utils::logger().warn("config.json is incomplete, saving defaults");
            clearSessionOnly();
            return ll::config::saveConfig(gConfig, path);
        }
        clearSessionOnly();
        return true;
    } catch (std::exception const& e) {
        utils::logger().error("config.json is malformed ({}), rewriting defaults", e.what());
    } catch (...) {
        utils::logger().error("config.json is malformed, rewriting defaults");
    }
    clearSessionOnly();
    return ll::config::saveConfig(gConfig, path);
}

bool save() {
    try {
        if (!ll::config::saveConfig(gConfig, configPath())) {
            utils::logger().warn("config.json could not be written");
            return false;
        }
        return true;
    } catch (...) {
        utils::logger().warn("config.json could not be written (exception)");
        return false;
    }
}

} // namespace neurodragon::config
