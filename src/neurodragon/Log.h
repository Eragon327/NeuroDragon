#pragma once

#include <fmt/format.h>

#include <chrono>
#include <string>
#include <string_view>

namespace neurodragon::log {

// 0 = info, 1 = warn, 2 = error, 3 = debug. Every line ends up in LeviLamina's
// logger for this mod; declared without any ll/engine include so that this header can be
// included from every translation unit as-is (D3D12Bridge.cpp needs <d3d12.h>).
//
// `"key"_tr(args...)` is the project's one i18n idiom (docs/README.md 代码约定) and every
// call site uses it. The key-taking helpers below are not a second idiom: they exist only
// for Stage, which lives in this header and therefore cannot include the i18n header
// itself. New code calls "key"_tr(...) directly.
void write(int level, std::string_view message);

// Localised log line with arguments, for Stage. A missing key logs the key itself so a
// broken catalogue is visible instead of printing nothing.
void writeKeyArgs(int level, std::string_view key, fmt::format_args args);

template <class... Args>
void writeKey(int level, std::string_view key, Args&&... args) {
    writeKeyArgs(level, key, fmt::make_format_args(args...));
}

// Localised text without arguments, for Stage's name (e.g. one NGX result code -> its
// name). "key"_tr() needs the i18n header, which this header cannot include.
std::string text(std::string_view key);

// ---------------------------------------------------------------------------
// Startup stage markers
// ---------------------------------------------------------------------------
//
// Everything that can freeze the client for a noticeable time - creating a D3D12
// device, building the font atlas, letting the NGX core load its 264 MB model - is
// wrapped in one of these. It writes one info line when the work starts and one
// when it ends, the second one carrying milliseconds, so a frozen picture has a
// last log line that names the step instead of leaving "it hung somewhere during
// start-up" to guesswork.
//
// The start line is written *before* the work begins on purpose: if the client has
// to be killed while stalled, that line is the only evidence left. That is why
// load() raises the log file's flush level to info - without it the line would sit
// in a buffer and be lost together with the process.
class Stage {
public:
    // 'nameKey' is an i18n key under "log.stage.*" (see src/lang). 'enabled' false
    // keeps both lines out of the log: for steps that may retry every frame, where
    // only the first attempt is worth reporting.
    explicit Stage(std::string_view nameKey, bool enabled = true) : enabled_(enabled) {
        start_ = clock::now();
        if (!enabled_) return;
        name_ = text(nameKey);
        writeKey(0, "log.stage.start", name_);
    }

    Stage(Stage const&)            = delete;
    Stage& operator=(Stage const&) = delete;
    Stage(Stage&&)                 = delete;
    Stage& operator=(Stage&&)      = delete;

    ~Stage() { finish(failed_); }

    // Ends the stage now (writing the end line) and returns how long the work took
    // in milliseconds. Used where the duration is also needed as a value. The
    // duration is measured even when the stage body is silent.
    double finish(bool failed = false) {
        if (ended_) return 0.0;
        ended_          = true;
        double const ms = elapsedMs();
        if (enabled_) writeKey(0, failed ? "log.stage.failed" : "log.stage.done", name_, ms);
        return ms;
    }

    // The end line then reads "failed" instead of "done". For scopes that leave
    // early; the adjacent error/warn line already says why.
    void markFailed() { failed_ = true; }

private:
    using clock = std::chrono::steady_clock;

    double elapsedMs() const { return std::chrono::duration<double, std::milli>(clock::now() - start_).count(); }

    std::string       name_;
    clock::time_point start_{};
    bool              enabled_{true};
    bool              ended_{false};
    bool              failed_{false};
};

} // namespace neurodragon::log
