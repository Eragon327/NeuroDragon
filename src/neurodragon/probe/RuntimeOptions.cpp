#include "neurodragon/probe/RuntimeOptions.h"

#include "neurodragon/utils/Utils.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#ifdef TEST
#include "ll/api/service/TargetedBedrock.h"
#include "ll/api/thread/ClientThreadExecutor.h"

#include "mc/client/game/ClientInstance.h"
#include "mc/client/options/IOptionRegistry.h"
#include "mc/options/option_types/Option.h"
#include "mc/options/option_types/OptionID.h"
#endif

namespace neurodragon::probe::options {
namespace {

std::atomic<bool> gRequested{false};
std::atomic<bool> gDone{false};

} // namespace

#ifdef TEST
namespace {

using ll::thread::ClientThreadExecutor;

// The engine registers every saved option under a string tag (the key in
// options.txt). `OptionID` is shipped empty, so the tag is the only stable name in
// these headers; isValidOption() is the bounds-checked query, which is what makes a
// scan over the low id range safe even when the real enum has holes.
constexpr int kFirstOptionId = 0;
constexpr int kLastOptionId  = 1023;

std::atomic<std::uint64_t> gLastPostMs{0};

std::uint64_t nowMs() {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()
        )
            .count()
    );
}

void scanAndSwitch(int attempt) {
    utils::logger().info("[vv] options: client-thread task running (attempt {})", attempt);
    auto client = ll::service::getClientInstance();
    if (!client) {
        utils::logger().warn("[vv] options: no client instance on this thread; switch not attempted");
        return;
    }
    auto& registry = client->getOptions();
    utils::logger().info("[vv] options: registry reached (attempt {})", attempt);

    int         valid       = 0;
    int         found       = -1;
    int         interesting = 0;
    std::string before;
    for (int i = kFirstOptionId; i <= kLastOptionId; ++i) {
        auto const id = static_cast<OptionID>(i);
        if (!registry.isValidOption(id)) {
            continue;
        }
        ++valid;
        Option* option = registry.get(id);
        if (!option) {
            continue;
        }
        std::vector<std::pair<std::string, std::string>> pairs;
        option->save(pairs);
        for (auto const& [key, value] : pairs) {
            bool const relevant = key.find("upscal") != std::string::npos
                || key == "graphics_mode" || key == "target_resolution";
            if (relevant && interesting < 24) {
                ++interesting;
                utils::logger().info("[vv] options: id={} {}={}", i, key, value);
            }
            if (key == "upscaling_mode") {
                found  = i;
                before = value;
            }
        }
    }
    utils::logger().info(
        "[vv] options: scan done valid={} upscaling_mode id={} value={}",
        valid,
        found,
        before
    );
    if (found < 0) {
        utils::logger().warn("[vv] options: upscaling_mode not found by save tag; runtime switch not attempted");
        return;
    }
    Option* const option = registry.get(static_cast<OptionID>(found));
    if (!option) {
        utils::logger().warn("[vv] options: upscaling_mode id={} vanished between scan and get", found);
        return;
    }
    option->load("1");
    std::vector<std::pair<std::string, std::string>> after;
    option->save(after);
    std::string value;
    for (auto const& [key, v] : after) {
        if (key == "upscaling_mode") {
            value = v;
        }
    }
    utils::logger().info("[vv] options: load upscaling_mode {} -> {}", before, value);
    utils::logger().info("[vv] options: reread upscaling_mode = {}", value);
}

// Runs at most once: the first route to get here wins, the rest are no-ops.
void switchOnce(int attempt) {
    if (gDone.exchange(true, std::memory_order_acq_rel)) {
        return;
    }
    scanAndSwitch(attempt);
}

} // namespace

void requestBilinear() {
    utils::logger().info("[vv] options: bilinear switch requested at load time");
    gRequested.store(true, std::memory_order_relaxed);
}

void applyRequested() {
    if (!gRequested.load(std::memory_order_relaxed) || gDone.load(std::memory_order_relaxed)) {
        return;
    }
    // The executor path did not deliver during startup (EXP-0018/0019), so retry at
    // a slow rate instead of once: the client thread processing the queue is what
    // this waits for, and the join-event route is the one expected to win.
    std::uint64_t const now  = nowMs();
    std::uint64_t const last = gLastPostMs.load(std::memory_order_relaxed);
    if (last && now - last < 2000) {
        return;
    }
    gLastPostMs.store(now, std::memory_order_relaxed);
    utils::logger().info("[vv] options: handing the switch request to the client thread");
    ClientThreadExecutor::getDefault().execute([] { switchOnce(0); });
}

void applyNow() {
    if (!gRequested.load(std::memory_order_relaxed) || gDone.load(std::memory_order_relaxed)) {
        return;
    }
    utils::logger().info("[vv] options: applying the switch from the client level event");
    switchOnce(1);
}

#else
void requestBilinear() {}
void applyRequested() {}
void applyNow() {}
#endif

} // namespace neurodragon::probe::options
