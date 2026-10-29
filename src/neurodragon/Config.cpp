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
//
// The same pass folds values that no longer have a meaning to the one that does:
// `replace` is the single composite now (TRP Ratio resolve = 6), and `nrEncode`
// only has TRP's encode (3) and the two Reinhard A/B encodes (0/1).
void normalizeOnLoad() {
    gConfig.nr.compare  = 0;
    gConfig.nr.replace  = 6;
    if (gConfig.nr.nrEncode != 0 && gConfig.nr.nrEncode != 1 && gConfig.nr.nrEncode != 3) {
        gConfig.nr.nrEncode = 3;
    }
}

} // namespace

Config& get() { return gConfig; }

bool loadOrCreate() {
    auto const path = configPath();

    // A malformed file makes loadConfig throw (it parses json eagerly); keep the
    // defaults and rewrite in that case instead of taking the mod down. Values that no
    // longer exist (the gain composite's 5, an old encode id) are folded by
    // normalizeOnLoad() before anything reads them, so a hand-edited file cannot put
    // the renderer into a mode that no longer has code behind it.
    try {
        if (!ll::config::loadConfig(gConfig, path)) {
            utils::logger().warn("config.json is incomplete, saving defaults");
            normalizeOnLoad();
            return ll::config::saveConfig(gConfig, path);
        }
        normalizeOnLoad();
        return true;
    } catch (std::exception const& e) {
        utils::logger().error("config.json is malformed ({}), rewriting defaults", e.what());
    } catch (...) {
        utils::logger().error("config.json is malformed, rewriting defaults");
    }
    normalizeOnLoad();
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
