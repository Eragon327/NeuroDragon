#include "neurodragon/probe/RuntimeOptions.h"

#include "neurodragon/utils/Utils.h"

#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#ifdef TEST
#include "ll/api/service/TargetedBedrock.h"

#include "mc/client/game/ClientInstance.h"
#include "mc/client/options/IOptionRegistry.h"
#include "mc/options/option_types/Option.h"
#include "mc/options/option_types/OptionID.h"
#endif

namespace neurodragon::probe::options {

#ifdef TEST
namespace {

// One slot, not a queue: the panel only ever asks for "the value I just picked", and
// a pile-up of stale writes would apply in an order nobody asked for.
struct Pending {
    std::mutex  mutex;
    bool        has{};
    Known       option{};
    std::string value;
    WriteResult last;
};

Pending& pending() {
    static Pending instance;
    return instance;
}

char const* tagOf(Known option) {
    switch (option) {
    case Known::UpscalingPercentage:
        return "upscaling_percentage";
    case Known::UpscalingMode:
        return "upscaling_mode";
    case Known::AdvancedUpscaling:
        return "gfx_upscaling";
    case Known::TargetResolution:
        return "target_resolution";
    case Known::GraphicsMode:
        return "graphics_mode";
    }
    return "";
}

// The option's own answer for `tag`: empty means the id no longer points at the
// option we mean (a game update shifted the enum), which is the refusal condition.
std::string savedValue(Option& option, char const* tag) {
    std::vector<std::pair<std::string, std::string>> pairs;
    option.save(pairs);
    for (auto const& [key, value] : pairs) {
        if (key == tag) {
            return value;
        }
    }
    return {};
}

// `load` is the engine's own string parser, so what it stores is not guaranteed to be
// byte-identical to what went in ("50" vs "50.0"). Compare numerically when both sides
// parse, by string otherwise.
bool sameValue(std::string const& before, std::string const& wanted) {
    if (before == wanted) {
        return true;
    }
    char* endBefore = nullptr;
    char* endWanted = nullptr;
    double const a  = std::strtod(before.c_str(), &endBefore);
    double const b  = std::strtod(wanted.c_str(), &endWanted);
    bool const aOk  = endBefore && *endBefore == '\0' && endBefore != before.c_str();
    bool const bOk  = endWanted && *endWanted == '\0' && endWanted != wanted.c_str();
    return aOk && bOk && a == b;
}

WriteResult write(IOptionRegistry& registry, Known option, std::string const& value) {
    WriteResult result{};
    char const* const tag = tagOf(option);
    int const         id  = static_cast<int>(option);

    auto  found      = registry.getIfValid(static_cast<OptionID>(id));
    result.attempted = true;
    if (!found || !*found) {
        std::snprintf(result.text, sizeof(result.text), "%d %s: no option at that id", id, tag);
        return result;
    }

    Option&           target = **found;
    std::string const before = savedValue(target, tag);
    if (before.empty()) {
        std::snprintf(
            result.text,
            sizeof(result.text),
            "%d is not %s; write refused (ids moved?)",
            id,
            tag
        );
        return result;
    }

    target.load(value);
    std::string const after = savedValue(target, tag);
    result.ok = sameValue(after, value);
    std::snprintf(
        result.text,
        sizeof(result.text),
        "%d %s %s -> %s%s",
        id,
        tag,
        before.c_str(),
        after.c_str(),
        result.ok ? "" : " (requested was not stored)"
    );
    return result;
}

} // namespace

void requestWrite(Known option, std::string value) {
    auto& state = pending();
    std::lock_guard lock(state.mutex);
    if (state.has) {
        utils::logger().warn(
            "[vv] options: dropping the pending write {} -> {}, replacing it with {} -> {}",
            static_cast<int>(state.option),
            state.value,
            static_cast<int>(option),
            value
        );
    }
    state.has    = true;
    state.option = option;
    state.value  = std::move(value);
}

void applyPending() {
    {
        auto& state = pending();
        std::lock_guard lock(state.mutex);
        if (!state.has) {
            return;
        }
    }
    // Checked before consuming: on the title screen there is no client instance, and
    // the request must survive to the first level tick instead of being lost.
    auto client = ll::service::getClientInstance();
    if (!client) {
        return;
    }

    Known       option{};
    std::string value;
    {
        auto& state = pending();
        std::lock_guard lock(state.mutex);
        option = state.option;
        value  = std::move(state.value);
        state.has = false;
    }

    WriteResult const result = write(client->getOptions(), option, value);
    utils::logger().info(
        "[vv] options: write {} {}",
        result.ok ? "ok" : "failed",
        result.text
    );
    auto& state = pending();
    std::lock_guard lock(state.mutex);
    state.last = result;
}

WriteResult lastWrite() {
    auto& state = pending();
    std::lock_guard lock(state.mutex);
    return state.last;
}

void requestBilinear() {
    utils::logger().info("[vv] options: bilinear switch requested at load time");
    requestWrite(Known::UpscalingMode, "1");
}

#else
void requestWrite(Known, std::string) {}
void applyPending() {}
WriteResult lastWrite() { return {}; }
void requestBilinear() {}
#endif

} // namespace neurodragon::probe::options
