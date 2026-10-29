#include "neurodragon/probe/RuntimeOptions.h"

#include "neurodragon/utils/Utils.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include "ll/api/service/TargetedBedrock.h"

#include "mc/client/game/ClientInstance.h"
#include "mc/client/options/IOptionRegistry.h"
#include "mc/options/option_types/Option.h"
#include "mc/options/option_types/OptionID.h"

namespace neurodragon::probe::options {

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

// Reads one option by id, with the same tag check the write uses: a number alone would
// happily report whatever unrelated setting moved into that slot.
int readInt(IOptionRegistry& registry, Known option, int fallback) {
    char const* const tag = tagOf(option);
    auto              found = registry.getIfValid(static_cast<OptionID>(static_cast<int>(option)));
    if (!found || !*found) {
        return fallback;
    }
    std::string const value = savedValue(**found, tag);
    if (value.empty()) {
        return fallback;
    }
    char*     end = nullptr;
    long const parsed = std::strtol(value.c_str(), &end, 10);
    if (!end || end == value.c_str() || *end != '\0') {
        return fallback;
    }
    return static_cast<int>(parsed);
}

struct Cached {
    std::atomic<int>      graphicsMode{-1};
    std::atomic<int>      percent{-1};
    std::atomic<bool>     valid{false};
    std::atomic<uint64_t> lastMs{0};
};

Cached& cache() {
    static Cached instance;
    return instance;
}

std::uint64_t nowMs() {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()
        )
            .count()
    );
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
    if (result.ok) {
        ND_DEBUG("[vv] options: write ok {}", result.text);
    } else {
        utils::logger().warn("[vv] options: write failed {}", result.text);
    }
    {
        auto& state = pending();
        std::lock_guard lock(state.mutex);
        state.last = result;
    }
    // The panel's "configured" reading comes from the same registry; refresh it now so
    // the page does not keep reporting the pre-write value for up to two seconds.
    if (result.ok) {
        refreshKnownValues(true);
    }
}

WriteResult lastWrite() {
    auto& state = pending();
    std::lock_guard lock(state.mutex);
    return state.last;
}

KnownValues knownValues() {
    auto& c = cache();
    KnownValues values{};
    values.valid            = c.valid.load(std::memory_order_relaxed);
    values.graphicsMode     = c.graphicsMode.load(std::memory_order_relaxed);
    values.upscalingPercent = c.percent.load(std::memory_order_relaxed);
    return values;
}

void refreshKnownValues(bool force) {
    constexpr std::uint64_t kIntervalMs = 2000;
    auto&                   c           = cache();
    std::uint64_t const     now         = nowMs();
    if (!force) {
        std::uint64_t const last = c.lastMs.load(std::memory_order_relaxed);
        if (last && now - last < kIntervalMs) {
            return;
        }
    }
    c.lastMs.store(now, std::memory_order_relaxed);

    auto client = ll::service::getClientInstance();
    if (!client) {
        return;
    }
    auto& registry = client->getOptions();
    int const mode = readInt(registry, Known::GraphicsMode, -1);
    int const pct  = readInt(registry, Known::UpscalingPercentage, -1);
    if (mode < 0 && pct < 0) {
        return;
    }
    c.graphicsMode.store(mode, std::memory_order_relaxed);
    c.percent.store(pct, std::memory_order_relaxed);
    c.valid.store(true, std::memory_order_relaxed);
}

void requestBilinear() {
    ND_DEBUG("[vv] options: bilinear switch requested at load time");
    requestWrite(Known::UpscalingMode, "1");
}

} // namespace neurodragon::probe::options
