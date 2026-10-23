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

} // namespace

Config& get() { return gConfig; }

bool loadOrCreate() {
    auto const path = configPath();

    // A malformed file makes loadConfig throw (it parses json eagerly); keep the
    // defaults and rewrite in that case instead of taking the mod down.
    try {
        if (!ll::config::loadConfig(gConfig, path)) {
            utils::logger().warn("config.json is incomplete, saving defaults");
            return ll::config::saveConfig(gConfig, path);
        }
        return true;
    } catch (std::exception const& e) {
        utils::logger().error("config.json is malformed ({}), rewriting defaults", e.what());
    } catch (...) {
        utils::logger().error("config.json is malformed, rewriting defaults");
    }
    return ll::config::saveConfig(gConfig, path);
}

void reload() {
    try {
        ll::config::loadConfig(gConfig, configPath());
    } catch (...) {
        // Keep the last good values.
    }
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
