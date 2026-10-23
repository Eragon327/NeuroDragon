#include "neurodragon/probe/RenderProbe.h"

#include "neurodragon/Config.h"
#include "neurodragon/Log.h"
#include "neurodragon/utils/Utils.h"

#include "neurodragon/NeuroDragon.h"
#include "neurodragon/nr/D3D12Bridge.h"
#include "neurodragon/ui/Overlay.h"

#include "ll/api/i18n/I18n.h"
#include "ll/api/memory/Hook.h"
#include "ll/api/event/EventBus.h"
#include "ll/api/event/client/ClientExitLevelEvent.h"
#include "ll/api/event/client/ClientJoinLevelEvent.h"

#include <Windows.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <map>
#include <string>
#include <thread>

// Read-only NGX/DLSS probe for the RT pipeline.
//
// Everything inside the game module is hooked with LeviLamina's macros: signature
// bytes are a valid IDENTIFIER, so there is no manual scanning and no
// ll::memory::hook. Only the two NGX exports need a manual address, because they
// live in _nvngx.dll which loads after this module.
//
// Log tags: [param] SetPtr  [param4] slot-4 unsigned Set  [eval] EvaluateFeature
//           [create] CreateFeature  [res] first sighting  [desc] sampled GetDesc
//           [dump] colour readback written

using namespace ll::i18n_literals;

namespace neurodragon::probe {
namespace {

using namespace std::chrono_literals;

std::atomic<std::uint64_t> gFrame{0};
std::atomic_bool           gWatcherStop{false};

// Manual hooks are not owned by a LeviLamina macro, so they must be recorded and
// removed on disable: a detour left behind points into an unloaded module and
// faults the next time the engine calls it (0xC0000005 at -1, 2026-10-21).
struct HookRecord {
    void* target = nullptr;
    void* detour = nullptr;
};
HookRecord gHookRecords[4];
int        gHookCount   = 0;
std::thread gWatcherThread;
std::atomic_bool           gNgxHooked{false};
std::atomic_bool           gActive{true};

// ---------------------------------------------------------------------------
// Parameter object helpers. The vtable slot table is measured, not copied from
// the SDK header: Set 1 = pointer/resource, 3 = unsigned, 6 = float; Get 8/9
// return resources, 11 = unsigned. See docs/D-rt-dlss-nr-plan.md §4.
// ---------------------------------------------------------------------------

bool readable(void const* p, std::size_t bytes) {
    if (!p) {
        return false;
    }
    MEMORY_BASIC_INFORMATION mbi{};
    if (!VirtualQuery(p, &mbi, sizeof(mbi)) || mbi.State != MEM_COMMIT) {
        return false;
    }
    if (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) {
        return false;
    }
    auto const start = reinterpret_cast<std::uintptr_t>(p);
    auto const begin = reinterpret_cast<std::uintptr_t>(mbi.BaseAddress);
    return start + bytes <= begin + mbi.RegionSize;
}

struct NgxParamProbe {
    void* self = nullptr;

    template <class Fn>
    [[nodiscard]] Fn slot(int index) const {
        auto** vt = *reinterpret_cast<void***>(self);
        return reinterpret_cast<Fn>(vt[index]);
    }

    [[nodiscard]] unsigned u32(char const* name) const {
        unsigned value = 0;
        using Get      = int (*)(void*, char const*, unsigned int*);
        slot<Get>(11)(self, name, &value);
        return value;
    }

    [[nodiscard]] void* resAny(char const* name, int* slotOut = nullptr) const {
        static constexpr int kSlots[] = {8, 9, 14};
        using Get                     = int (*)(void*, char const*, void**);
        for (int const slotIndex : kSlots) {
            void* value = nullptr;
            (void)slot<Get>(slotIndex)(self, name, &value);
            if (value) {
                if (slotOut) {
                    *slotOut = slotIndex;
                }
                return value;
            }
        }
        return nullptr;
    }
};

bool validParamObj(void* p) {
    if (!p || !readable(p, sizeof(void*))) {
        return false;
    }
    auto** vt = *reinterpret_cast<void***>(p);
    return vt && readable(vt, 17 * sizeof(void*));
}

// ---------------------------------------------------------------------------
// Resource reporting
// ---------------------------------------------------------------------------

void describeNow(char const* name, void* value) {
    if (!value) {
        return;
    }
    auto const info = nr::describe(value);
    utils::logger().info(
        "[desc] {} f={} p={:#x} fmt={} {}x{} mips={} flags={:#x}",
        name,
        gFrame.load(std::memory_order_relaxed),
        reinterpret_cast<std::uintptr_t>(value),
        info.format,
        info.width,
        info.height,
        info.mips,
        info.flags
    );
}

// First sighting of each texture gets a full GetDesc; afterwards only
// null <-> set transitions are reported.
struct ResourceWatch {
    char const* name      = nullptr;
    void*       last      = nullptr;
    bool        described = false;
};

std::array<ResourceWatch, 4> gResourceWatch{{
    {"Color"},
    {"Depth"},
    {"MotionVectors"},
    {"Output"},
}};

void watchResource(char const* name, void* value) {
    auto&      log   = utils::logger();
    auto const frame = gFrame.load(std::memory_order_relaxed);
    for (auto& w : gResourceWatch) {
        if (!w.name || std::strcmp(w.name, name) != 0) {
            continue;
        }
        if (!w.described && value) {
            w.described = true;
            auto const info = nr::describe(value);
            log.debug(
                "[res] {} f={} p={:#x} dev={:#x} fmt={} {}x{} arr={} mips={} flags={:#x}",
                name,
                frame,
                reinterpret_cast<std::uintptr_t>(value),
                reinterpret_cast<std::uintptr_t>(info.device),
                info.format,
                info.width,
                info.height,
                info.depthOrArray,
                info.mips,
                info.flags
            );
        }
        bool const wasNull = w.last == nullptr;
        bool const isNull  = value == nullptr;
        if (wasNull != isNull) {
            log.debug("[res] {} f={} -> {}", name, frame, isNull ? "null" : "set");
        }
        w.last = value;
        return;
    }
}

// ---------------------------------------------------------------------------
// Capture the engine's NGX identity.
//
// The engine answers CreateFeature/EvaluateFeature through _nvngx.dll, but its
// Init may go through the app-facing shim "nvngx.dll" (a different module with
// its own exports), which is why hooking only _nvngx.dll never saw an Init. Hook
// both modules as soon as either appears - that is the whole point here.
// ---------------------------------------------------------------------------
using NgxInitFn        = int (*)(unsigned long long, wchar_t const*, void*, int);
using NgxInitExtFn     = int (*)(unsigned long long, wchar_t const*, void*, int, void const*);
using NgxInitProjectFn = int (*)(
    char const*, int, char const*, wchar_t const*, void*, void const*, int);

NgxInitFn        gInitOriginal        = nullptr;
NgxInitExtFn     gInitExtOriginal     = nullptr;
NgxInitProjectFn gInitProjectOriginal = nullptr;
std::atomic_bool gInitHooked{false};

int ngxInitDetour(unsigned long long appId, wchar_t const* dataPath, void* device, int apiVersion) {
    utils::logger().info(
        "[init] Init appId={:#x} api={:#x}", appId, static_cast<unsigned>(apiVersion));
    nr::nrSetEngineInit(appId, dataPath);
    return gInitOriginal ? gInitOriginal(appId, dataPath, device, apiVersion) : 0xBAD00007;
}

int ngxInitExtDetour(unsigned long long appId, wchar_t const* dataPath, void* device,
                     int apiVersion, void const* params) {
    utils::logger().info(
        "[init] Init_Ext appId={:#x} api={:#x}", appId, static_cast<unsigned>(apiVersion));
    nr::nrSetEngineInit(appId, dataPath);
    return gInitExtOriginal ? gInitExtOriginal(appId, dataPath, device, apiVersion, params)
                            : 0xBAD00007;
}

int ngxInitProjectDetour(char const* projectId, int engineType, char const* engineVersion,
                         wchar_t const* dataPath, void* device, void const* info, int apiVersion) {
    utils::logger().info(
        "[init] Init_ProjectID project={} engineType={} engineVersion={} api={:#x}",
        projectId ? projectId : "?", engineType, engineVersion ? engineVersion : "?",
        static_cast<unsigned>(apiVersion));
    nr::nrSetEngineInit(0x0876232Cu, dataPath);
    return gInitProjectOriginal
         ? gInitProjectOriginal(projectId, engineType, engineVersion, dataPath, device, info, apiVersion)
         : 0xBAD00007;
}

void hookInitOn(HMODULE module, char const* which) {
    if (!module) {
        return;
    }
    struct Entry {
        char const* name;
        void*       detour;
        void**      original;
    };
    Entry const entries[] = {
        {"NVSDK_NGX_D3D12_Init", reinterpret_cast<void*>(&ngxInitDetour),
         reinterpret_cast<void**>(&gInitOriginal)},
        {"NVSDK_NGX_D3D12_Init_Ext", reinterpret_cast<void*>(&ngxInitExtDetour),
         reinterpret_cast<void**>(&gInitExtOriginal)},
        {"NVSDK_NGX_D3D12_Init_ProjectID", reinterpret_cast<void*>(&ngxInitProjectDetour),
         reinterpret_cast<void**>(&gInitProjectOriginal)},
        {"NVSDK_NGX_D3D12_Init_with_ProjectID", reinterpret_cast<void*>(&ngxInitProjectDetour),
         reinterpret_cast<void**>(&gInitProjectOriginal)},
    };
    auto& log = utils::logger();
    for (auto const& e : entries) {
        auto* target = reinterpret_cast<void*>(GetProcAddress(module, e.name));
        if (!target) {
            continue;
        }
        // No thread suspension: this runs from a watcher thread and the window in
        // which _nvngx.dll becomes visible is exactly the window in which the game
        // is still inside the loader lock. Suspending the loader thread there
        // deadlocked the client at startup (hangs right after the first
        // [rt] dispatch, where the good runs logged "[init] hooked ...").
        // LeviLamina's own AUTO hooks install with suspendThreads = false too.
        if (ll::memory::hook(
                target,
                e.detour,
                e.original,
                ll::memory::HookPriority::Normal,
                false /* suspendThreads */
            )
            == 0) {
            log.debug("[init] hooked {} on {}", e.name, which);
        }
    }
    gInitHooked.store(true, std::memory_order_relaxed);
}

// ---------------------------------------------------------------------------
// Stage 2: NR feature (NGX 0x12) at the engine's render resolution.
//
// Off unless config.json (nr.enabled) says so. When on, one
// feature is created on the first frame whose colour/depth/MV are all bound,
// evaluated for a 60-frame window, timed with GPU timestamps and dumped twice
// (our output + the engine's Color) so "it ran" can be checked by eye.
// The engine's DLSS keeps its own inputs unless config.json asks for
// Replace=1, which copies the NR result back into Color (docs/D §6.14 route A,
// NR never upscales - the engine's DLSS still does the upscaling).
// ---------------------------------------------------------------------------

bool          gNrEnabled      = false;
bool          gNrStartTried   = false;
bool          gNrWindowOpen   = false;
int           gNrWindowIndex  = -1;
bool          gNrSizeWarned   = false;
unsigned      gNrCreateWidth  = 0;
unsigned      gNrCreateHeight = 0;
std::uint64_t gWindowAnchor   = 0;
std::uint64_t gNrWindowStart  = 0;
std::uint64_t gNrWindowEnd    = 0;
std::uint64_t gNrDumpFrame    = 0;
std::uint64_t gNrReportAt     = 0;

// Short windows spaced far apart: the first ones run in the menu (no ray
// tracing, Color is black), later ones land inside a world and after the world
// finished loading. The dumps carry the window index, so the in-world pair is
// easy to pick out.
constexpr int           kNrWindows       = 10;
constexpr std::uint64_t kNrWindowLength  = 60;
constexpr std::uint64_t kNrWindowSpacing = 900;
constexpr std::uint64_t kNrReportTail    = 4; // let the last timestamp pair retire

void nrDumpName(char* buffer, std::size_t size, char const* stem, int index) {
    std::snprintf(buffer, size, "%s_w%d.bmp", stem, index);
}

// Color is pure black until a world is loaded (and ray tracing is on), so the
// whole NR measurement is gated on the client level actually being there.
// Building the feature in the menu would also bake the menu's frame in before
// the first real frame, and any window spent there would only dump black.
// A level change only resets temporal history and restarts the dump/timing
// window schedule; it never delays NR (that gate was removed on purpose).
std::atomic_bool gRestartWindows{false};
ll::event::ListenerPtr gJoinLevelListener;
ll::event::ListenerPtr gExitLevelListener;

void registerLevelListeners() {
    auto& log = utils::logger();
    auto& bus = ll::event::EventBus::getInstance();
    gJoinLevelListener = bus.emplaceListener<ll::event::client::ClientJoinLevelEvent>([](auto&) {
        nr::nrNotifyLevelLoaded();
        gRestartWindows.store(true, std::memory_order_relaxed);
    });
    gExitLevelListener = bus.emplaceListener<ll::event::client::ClientExitLevelEvent>([](auto&) {
        gRestartWindows.store(true, std::memory_order_relaxed);
    });
    log.debug(
        "[nr] level 监听 join={} exit={}",
        gJoinLevelListener != nullptr,
        gExitLevelListener != nullptr
    );
}



// ---------------------------------------------------------------------------
// Game-module hooks (signature identifiers, auto-registered at load)
// ---------------------------------------------------------------------------

// Signature identifiers are parsed once and handed to the hook macros as
// SignatureView, which is the form resolveIdentifier() accepts.
namespace sig {
inline ll::memory::Signature const kSigSetPtr = ll::memory::Signature::parse("48 89 5C 24 08 48 89 74 24 10 48 89 7C 24 18 41 56 48 83 EC 20 48 8B 01 4C 8B F1 49 8B F8 48 8B F2 48 8B 58 08 48 8B CB");
inline ll::memory::Signature const kSigSetF = ll::memory::Signature::parse("48 89 5C 24 08 48 89 74 24 10 57 48 83 EC 30 48 8B 01 48 8B F1 0F 29 74 24 20 48 8B FA 0F 28 F2");
inline ll::memory::Signature const kSigSetUI = ll::memory::Signature::parse("48 89 5C 24 08 48 89 74 24 10 48 89 7C 24 18 41 56 48 83 EC 20 48 8B 01 4C 8B F1 41 8B F8 48 8B F2 48 8B 58 18 48 8B CB");
inline ll::memory::Signature const kSigSet4 = ll::memory::Signature::parse("48 89 5C 24 08 48 89 74 24 10 48 89 7C 24 18 41 56 48 83 EC 20 48 8B 01 4C 8B F1 41 8B F8 48 8B F2 48 8B 58 20");
inline ll::memory::Signature const kSigCreateWrapper = ll::memory::Signature::parse("48 89 6C 24 10 48 89 74 24 18 48 89 7C 24 20 41 56 48 83 EC 20 48 83 3D ?? ?? ?? ?? 00");
} // namespace sig

// ---------------------------------------------------------------------------
// NGX parameter contract log.
//
// Every Set* the engine performs is recorded by name; value changes are logged
// (budgeted) when config.json sets nr.paramLog. This is how we learn the
// exact parameter set - sizes, subrects, jitter, MV scale, exposure, presets -
// that the engine's own DLSS SR is driven with.
// ---------------------------------------------------------------------------
std::map<std::string, std::string> gParamSeen;
std::atomic<int>                   gParamBudget{800};
std::atomic<bool>                  gParamDumped{false};

void recordParam(char const* name, char const* text) {
    if (!name || !text) {
        return;
    }
    auto it = gParamSeen.find(name);
    if (it != gParamSeen.end() && it->second == text) {
        return;
    }
    gParamSeen[name] = text;
    if (!nr::nrParamLogEnabled()) {
        return;
    }
    // First time logging is on, dump everything recorded so far: the important
    // setup parameters (sizes, subrects, presets) are written once at creation and
    // would otherwise never show up as a "change".
    if (!gParamDumped.exchange(true)) {
        auto& logger = utils::logger();
        for (auto const& [key, value] : gParamSeen) {
            logger.info("[ngxset] {} = {} (initial)", key, value);
        }
    }
    if (gParamBudget.fetch_sub(1, std::memory_order_relaxed) <= 0) {
        return;
    }
    utils::logger().info(
        "[ngxset] {} = {} f={}",
        name,
        text,
        gFrame.load(std::memory_order_relaxed)
    );
}

// SetPtr(param, name, value): the resource overload.
LL_AUTO_STATIC_HOOK(
    ParamSetPtrHook,
    HookPriority::Normal,
    ::ll::memory::SignatureView{sig::kSigSetPtr},
    void,
    void*       param,
    char const* name,
    void*       value
) {
    origin(param, name, value);
    if (!name) {
        return;
    }
    char text[32];
    std::snprintf(text, sizeof(text), "%#llx", static_cast<unsigned long long>(
        reinterpret_cast<std::uintptr_t>(value)));
    recordParam(name, text);
    // The engine rewrites Color/Output every frame (Output alternates between two
    // textures), so log the first frames only and then sparsely.
    static std::atomic<std::uint64_t> calls{0};
    auto const                        n     = calls.fetch_add(1, std::memory_order_relaxed) + 1;
    auto const                        frame = gFrame.load(std::memory_order_relaxed);
    if (frame >= 32 && n % 600 != 0) {
        return;
    }
    utils::logger().info(
        "[param] SetPtr {} f={} tid={} = {:#x}",
        name,
        frame,
        GetCurrentThreadId(),
        reinterpret_cast<std::uintptr_t>(value)
    );
}

// Set(param, name, float).
LL_AUTO_STATIC_HOOK(
    ParamSetFHook,
    HookPriority::Normal,
    ::ll::memory::SignatureView{sig::kSigSetF},
    void,
    void*       param,
    char const* name,
    float       value
) {
    origin(param, name, value);
    char text[32];
    std::snprintf(text, sizeof(text), "%.6g", static_cast<double>(value));
    recordParam(name, text);
}

// Set(param, name, unsigned) - slot 3.
LL_AUTO_STATIC_HOOK(
    ParamSetUIHook,
    HookPriority::Normal,
    ::ll::memory::SignatureView{sig::kSigSetUI},
    void,
    void*       param,
    char const* name,
    unsigned    value
) {
    origin(param, name, value);
    char text[32];
    std::snprintf(text, sizeof(text), "%u", value);
    recordParam(name, text);
}

// Set(param, name, unsigned) - slot 4. The DLSS creation path feeds it the sizes
// (Width/Height, OutWidth/OutHeight, subrect dimensions), so this is the hook that
// reports what resolution the engine believes it renders at.
LL_AUTO_STATIC_HOOK(
    ParamSet4Hook,
    HookPriority::Normal,
    ::ll::memory::SignatureView{sig::kSigSet4},
    void,
    void*       param,
    char const* name,
    unsigned    value
) {
    origin(param, name, value);
    char text[32];
    std::snprintf(text, sizeof(text), "%u", value);
    recordParam(name, text);
    static std::atomic<std::uint64_t> calls{0};
    auto const                        n = calls.fetch_add(1, std::memory_order_relaxed) + 1;
    if (n <= 200) {
        utils::logger().info(
            "[param4] {} f={} = {}",
            name ? name : "?",
            gFrame.load(std::memory_order_relaxed),
            value
        );
    }
}

// Game-side CreateFeature wrapper: catches the very first call regardless of when
// _nvngx.dll finishes loading.
LL_AUTO_STATIC_HOOK(
    CreateWrapperHook,
    HookPriority::Normal,
    ::ll::memory::SignatureView{sig::kSigCreateWrapper},
    int,
    void*  cmdList,
    int    feature,
    void*  params,
    void** handle
) {
    int const result = origin(cmdList, feature, params, handle);
    auto&     log    = utils::logger();
    log.debug(
        "[create] feature={} params={:#x} ret={:#x} handle={:#x}",
        feature,
        reinterpret_cast<std::uintptr_t>(params),
        static_cast<unsigned>(result),
        handle ? reinterpret_cast<std::uintptr_t>(*handle) : std::uintptr_t{0}
    );
    return result;
}

// ---------------------------------------------------------------------------
// NGX exports (_nvngx.dll). Manual address + registration: the module loads after
// this one, and the macros resolve identifiers inside the game module only.
// ---------------------------------------------------------------------------

using NgxEvalFn   = int (*)(void* cmdList, void* handle, void const* params, void* callback, void* userData);
using NgxCreateFn = int (*)(void* cmdList, int feature, void* params, void** handle);

NgxEvalFn   gNgxEvalOriginal   = nullptr;
NgxCreateFn gNgxCreateOriginal = nullptr;
int ngxEvaluateDetour(void* cmdList, void* handle, void const* params, void* callback, void* userData) {
    if (!gActive.load(std::memory_order_relaxed)) {
        return gNgxEvalOriginal ? gNgxEvalOriginal(cmdList, handle, params, callback, userData) : 0;
    }
    auto&      log    = utils::logger();
    auto const n      = gFrame.fetch_add(1, std::memory_order_relaxed) + 1;
    // The panel's "Apply" can flip this without a restart, and the flag gates both
    // NR itself and the diagnostic window schedule, so read it per frame.
    gNrEnabled        = config::get().nr.enabled;
    void*      color  = nullptr;
    void*      depth  = nullptr;
    void*      mv     = nullptr;
    void*      out    = nullptr;
    unsigned   width  = 0;
    unsigned   height = 0;

    if (validParamObj(const_cast<void*>(params))) {
        NgxParamProbe probe{const_cast<void*>(params)};
        color  = probe.resAny("Color");
        depth  = probe.resAny("Depth");
        mv     = probe.resAny("MotionVectors");
        out    = probe.resAny("Output");
        width  = probe.u32("Width");
        height = probe.u32("Height");
        watchResource("Color", color);
        watchResource("Depth", depth);
        watchResource("MotionVectors", mv);
        watchResource("Output", out);

        if (n % 600 == 0) {
            describeNow("Color", color);
            describeNow("Depth", depth);
            describeNow("MotionVectors", mv);
            describeNow("Output", out);
        }

        // Diagnostic readback: dump the DLSS input and output once, then write them
        // as BMP next to the executable. Only meaningful inside a world - RT is off
        // in the main menu, where Color is entirely zero.
        if (n == 2400) {
            nr::queueColorDump(cmdList, color, "color_dump.bmp");
        } else if (n == 2600) {
            float         peak = -1.0f;
            std::uint32_t nz   = 0;
            if (nr::flushColorDump(&peak, &nz)) {
                log.debug("[dump] color_dump.bmp written max={} nz={}", peak, nz);
            }
        } else if (n == 2800) {
            nr::queueColorDump(cmdList, out, "output_dump.bmp");
        } else if (n == 3000) {
            float         peak = -1.0f;
            std::uint32_t nz   = 0;
            if (nr::flushColorDump(&peak, &nz)) {
                log.debug("[dump] output_dump.bmp written max={} nz={}", peak, nz);
            }
        }
    }

    // ---- stage 2: NR in front of the engine's DLSS -------------------------
    bool inWindow    = false;
    bool replaceMode = false;
    if (gNrEnabled && validParamObj(const_cast<void*>(params))) {
        // Entering/leaving a world only restarts the window schedule: the menu's
        // frames must not consume the ten slots.
        if (gRestartWindows.exchange(false, std::memory_order_relaxed)) {
            gWindowAnchor  = n;
            gNrWindowIndex = -1;
            gNrWindowOpen  = false;
            gNrReportAt    = 0;
            gNrSizeWarned  = false;
            log.debug("[nr] 窗口重新锚定 f={}（此前窗口全部作废）", n);
        }

        // No "wait until the level settles" gate: the moment the engine hands over
        // real Color/Depth/MV/Output resources, NR is created.
        if (!gNrStartTried && color && depth && mv && out && width && height) {
            gNrStartTried   = true;
            gNrCreateWidth  = width;
            gNrCreateHeight = height;
            gWindowAnchor   = n;
            nr::nrCreate(
                cmdList,
                color,
                width,
                height,
                const_cast<void*>(params),
                reinterpret_cast<void*>(gNgxCreateOriginal),
                reinterpret_cast<void*>(gNgxEvalOriginal)
            );
        }

        if (gNrStartTried) {
            if (!gNrWindowOpen && gNrWindowIndex + 1 < kNrWindows
                && n >= gWindowAnchor
                        + static_cast<std::uint64_t>(gNrWindowIndex + 1) * kNrWindowSpacing) {
                ++gNrWindowIndex;
                gNrWindowOpen  = true;
                gNrWindowStart = n;
                gNrWindowEnd   = n + kNrWindowLength;
                gNrDumpFrame   = n + 30;
                nr::nrSetWindowIndex(gNrWindowIndex);
                log.debug(
                    "[nr] 窗口 #{} 开始 f={}..{}，dump 于 {}（Replace=1 时画面显示 NR 结果，否则不变）",
                    gNrWindowIndex,
                    gNrWindowStart,
                    gNrWindowEnd - 1,
                    gNrDumpFrame
                );
            }

            inWindow = gNrWindowOpen && n >= gNrWindowStart && n < gNrWindowEnd;
            // Replace=1 is about looking at the picture, not sampling it, so in
            // that mode NR runs on every in-world frame; only the timing/dump
            // machinery stays windowed (a 60-frame burst every 900 frames is
            // useless for judging image quality by eye).
            replaceMode = nr::nrReplaceContinuous();
            if ((inWindow || replaceMode) && nr::nrIsCreated()) {
                if (width != gNrCreateWidth || height != gNrCreateHeight) {
                    if (!gNrSizeWarned) {
                        gNrSizeWarned = true;
                        neurodragon::log::write(
                            1,
                            "nr.warn.sizeChanged"_tr(gNrCreateWidth, gNrCreateHeight, width, height)
                        );
                    }
                } else {
                    auto const     mvInfo = nr::describe(mv);
                    unsigned const guideWidth =
                        mvInfo.width ? static_cast<unsigned>(mvInfo.width) : width;
                    unsigned const guideHeight =
                        mvInfo.height ? static_cast<unsigned>(mvInfo.height) : height;
                    bool const reset     = n == gNrWindowStart;
                    bool const replaceNow = replaceMode;
                    // The panel wants live GPU numbers in the mode people actually
                    // use for tuning, so Replace also stamps outside the windows.
                    bool const timed = inWindow || replaceMode;
                    if (timed) {
                        nr::nrStampBegin(cmdList, n, 0);
                    }
                    bool const recorded = nr::nrRecord(
                        cmdList,
                        color,
                        depth,
                        mv,
                        guideWidth,
                        guideHeight,
                        static_cast<float>(width),
                        static_cast<float>(height),
                        1,
                        reset,
                        replaceNow ? const_cast<void*>(params) : nullptr,
                        const_cast<void*>(out)
                    );
                    if (timed) {
                        nr::nrStampEnd(cmdList, n, 0);
                    }
                    if (reset) {
                        log.debug(
                            "[nr] 窗口 #{} 首帧 f={} 指南={}x{} 特征={}x{} recorded={}（DepthInverted 暂按 1=反向 Z）",
                            gNrWindowIndex,
                            n,
                            guideWidth,
                            guideHeight,
                            width,
                            height,
                            recorded
                        );
                    }
                }
            }

            if (inWindow && n == gNrDumpFrame) {
                char name[64];
                nrDumpName(name, sizeof(name), "nr_dump", gNrWindowIndex);
                if (nr::nrEncodedColorResource()) {
                    // Encoder active: both images are display-referred already.
                    nr::queueColorDumpEncoded(cmdList, nr::nrOutputResource(), name);
                } else {
                    nr::queueColorDumpLinear(cmdList, nr::nrOutputResource(), name);
                }
            } else if (inWindow && n == gNrDumpFrame + 2) {
                char          name[64];
                float         peak = -1.0f;
                std::uint32_t nz   = 0;
                if (nr::flushColorDump(&peak, &nz)) {
                    nrDumpName(name, sizeof(name), "nr_dump", gNrWindowIndex);
                    log.debug("[nr] dump {} written max={} nz={}", name, peak, nz);
                }
                nrDumpName(name, sizeof(name), "color_ref", gNrWindowIndex);
                nr::queueColorDumpEncoded(cmdList, nr::nrEncodedColorResource(), name);
                // Everything the upscaler touches, captured on THIS ONE frame so the
                // comparison does not silently depend on the camera standing still:
                //   0 color_ref (network input)  1 nr2 (network output)
                //   2 fed       (what DLSS reads) 3 base (engine's own Color)
                nrDumpName(name, sizeof(name), "nr2", gNrWindowIndex);
                nr::queueColorDumpN(1, cmdList, nr::nrOutputResource(), name, 1);
                if (void* fed = nr::nrFedResource()) {
                    nrDumpName(name, sizeof(name), "fed", gNrWindowIndex);
                    nr::queueColorDumpN(2, cmdList, fed, name, nr::nrFedDomain() == 1 ? 1 : 0);
                }
                nrDumpName(name, sizeof(name), "base", gNrWindowIndex);
                nr::queueColorDumpN(3, cmdList, const_cast<void*>(color), name, 0);
            } else if (inWindow && n == gNrDumpFrame + 4) {
                char          name[64];
                float         peak = -1.0f;
                std::uint32_t nz   = 0;
                char const*   tags[4] = {"color_ref", "nr2", "fed", "base"};
                for (int slot = 0; slot < 4; ++slot) {
                    peak = -1.0f;
                    nz   = 0;
                    if (nr::flushColorDumpN(slot, &peak, &nz)) {
                        nrDumpName(name, sizeof(name), tags[slot], gNrWindowIndex);
                        log.debug("[nr] dump {} written max={} nz={}", name, peak, nz);
                    }
                }
            } else if (inWindow && n == gNrDumpFrame + 6) {
                // out_after was queued right after the engine's evaluate: if it
                // equals nr_dump the upscaler really ran on the NR result.
                char          name[64];
                float         peak = -1.0f;
                std::uint32_t nz   = 0;
                if (nr::flushColorDump(&peak, &nz)) {
                    nrDumpName(name, sizeof(name), "out_after", gNrWindowIndex);
                    log.debug("[nr] dump {} written max={} nz={}", name, peak, nz);
                }
            } else if (inWindow && n == gNrDumpFrame + 10) {
                char          name[64];
                float         peak = -1.0f;
                std::uint32_t nz   = 0;
                if (nr::flushColorDump(&peak, &nz)) {
                    nrDumpName(name, sizeof(name), "out_b", gNrWindowIndex);
                    log.debug("[nr] dump {} written max={} nz={}", name, peak, nz);
                }
            } else if (inWindow && n == gNrDumpFrame + 14) {
                char          name[64];
                float         peak = -1.0f;
                std::uint32_t nz   = 0;
                if (nr::flushColorDump(&peak, &nz)) {
                    nrDumpName(name, sizeof(name), "sr_dump", gNrWindowIndex);
                    log.debug("[nr] dump {} written max={} nz={}", name, peak, nz);
                }
            } else if (inWindow && n == gNrDumpFrame + 34) {
                // Same two resources as the +2 pair, ~32 frames later. If the encoder
                // output is identical here while the camera moved, the network's input
                // is frozen - which is what "NR is not real time" would look like.
                char name[64];
                nrDumpName(name, sizeof(name), "color_ref2", gNrWindowIndex);
                nr::queueColorDumpN(0, cmdList, nr::nrEncodedColorResource(), name, 1);
                nrDumpName(name, sizeof(name), "nr3", gNrWindowIndex);
                nr::queueColorDumpN(1, cmdList, nr::nrOutputResource(), name, 1);
            } else if (inWindow && n == gNrDumpFrame + 36) {
                char          name[64];
                float         peak = -1.0f;
                std::uint32_t nz   = 0;
                char const*   tags[2] = {"color_ref2", "nr3"};
                for (int slot = 0; slot < 2; ++slot) {
                    peak = -1.0f;
                    nz   = 0;
                    if (nr::flushColorDumpN(slot, &peak, &nz)) {
                        nrDumpName(name, sizeof(name), tags[slot], gNrWindowIndex);
                        log.debug("[nr] dump {} written max={} nz={}", name, peak, nz);
                    }
                }
            }

            if (gNrWindowOpen && n >= gNrWindowEnd && !gNrReportAt) {
                gNrReportAt = n + kNrReportTail;
            }
            if (gNrReportAt && n >= gNrReportAt) {
                gNrReportAt   = 0;
                gNrWindowOpen = false;
                nr::nrStampTick(n);
                log.debug("[nr] 窗口 #{} 统计：", gNrWindowIndex);
                nr::nrStampReport();
                nr::nrStampReset();
            }
        }
    }

    bool const measureBaseline = inWindow || replaceMode;
    if (measureBaseline) {
        nr::nrStampBegin(cmdList, n, 1);
    }
    int const result =
        gNgxEvalOriginal ? gNgxEvalOriginal(cmdList, handle, params, callback, userData) : 0;
    if (gNrEnabled && gNrWindowOpen && gFrame.load(std::memory_order_relaxed) == gNrDumpFrame + 4) {
        // The engine's own Output, right after its DLSS ran: this is the proof
        // that the swap reached the upscaler (route A).
        char name[64];
        nrDumpName(name, sizeof(name), "out_after", gNrWindowIndex);
        nr::queueColorDumpLinear(cmdList, const_cast<void*>(out), name);
    } else if (gNrEnabled && gNrWindowOpen
               && gFrame.load(std::memory_order_relaxed) == gNrDumpFrame + 8) {
        // Second sample a few frames later: if it is identical to out_after the
        // engine stopped refreshing its Output (frozen picture).
        char name[64];
        nrDumpName(name, sizeof(name), "out_b", gNrWindowIndex);
        nr::queueColorDumpLinear(cmdList, const_cast<void*>(out), name);
    }
    if (measureBaseline) {
        nr::nrStampEnd(cmdList, n, 1);
    }

    if (gNrEnabled) {
        nr::nrStampTick(n);
    }

    if (n <= 3 || n % 600 == 0) {
        log.debug(
            "[eval] f={} tid={} cl={:#x} h={:#x} {}x{} c={:#x} d={:#x} mv={:#x} o={:#x} ret={:#x}",
            n,
            GetCurrentThreadId(),
            reinterpret_cast<std::uintptr_t>(cmdList),
            reinterpret_cast<std::uintptr_t>(handle),
            width,
            height,
            reinterpret_cast<std::uintptr_t>(color),
            reinterpret_cast<std::uintptr_t>(depth),
            reinterpret_cast<std::uintptr_t>(mv),
            reinterpret_cast<std::uintptr_t>(out),
            static_cast<unsigned>(result)
        );
    }
    return result;
}

int ngxCreateDetour(void* cmdList, int feature, void* params, void** handle) {
    int const result = gNgxCreateOriginal ? gNgxCreateOriginal(cmdList, feature, params, handle) : 0;
    if (!gActive.load(std::memory_order_relaxed)) {
        return result;
    }
    auto&     log    = utils::logger();
    log.debug(
        "[create-export] feature={} ret={:#x} handle={:#x}",
        feature,
        static_cast<unsigned>(result),
        handle ? reinterpret_cast<std::uintptr_t>(*handle) : std::uintptr_t{0}
    );
    return result;
}

// Hooking must not run inside a LoadLibrary detour (loader lock), so a watcher
// thread installs these once both modules are resident.
void hookNgxExports() {
    auto& log  = utils::logger();
    auto* core = GetModuleHandleW(L"_nvngx.dll");
    if (!core) {
        log.debug("[ngx] core not loaded yet, retrying");
        return;
    }
    struct Entry {
        char const* name;
        void*       detour;
        void**      original;
    };
    Entry const entries[] = {
        {"NVSDK_NGX_D3D12_EvaluateFeature",
         reinterpret_cast<void*>(&ngxEvaluateDetour),
         reinterpret_cast<void**>(&gNgxEvalOriginal)},
        {"NVSDK_NGX_D3D12_CreateFeature",
         reinterpret_cast<void*>(&ngxCreateDetour),
         reinterpret_cast<void**>(&gNgxCreateOriginal)},
    };
    for (auto const& e : entries) {
        auto* target = reinterpret_cast<void*>(GetProcAddress(core, e.name));
        if (!target) {
            neurodragon::log::write(2, "nr.error.missingExport"_tr(e.name));
            continue;
        }
        // Same reasoning as hookInitOn: never suspend threads from the watcher.
        if (ll::memory::hook(
                target,
                e.detour,
                e.original,
                ll::memory::HookPriority::Normal,
                false /* suspendThreads */
            )
            == 0) {
            if (gHookCount < 4) {
                gHookRecords[gHookCount].target = target;
                gHookRecords[gHookCount].detour = e.detour;
                ++gHookCount;
            }
            log.debug("[ngx] hooked {} @ {:#x}", e.name, reinterpret_cast<std::uintptr_t>(target));
        } else {
            neurodragon::log::write(2, "nr.error.hookFailed"_tr(e.name));
        }
    }
    log.debug(
        "[ngx] originals create={:#x} eval={:#x}",
        reinterpret_cast<std::uintptr_t>(gNgxCreateOriginal),
        reinterpret_cast<std::uintptr_t>(gNgxEvalOriginal)
    );
}

void startWatcher() {
    gWatcherThread = std::thread([] {
        // Phase 1: hook the Init entries on whichever NGX module shows up first
        // (the app-facing shim and the core are separate modules).
        for (int i = 0; i < 4000 && !gWatcherStop.load(std::memory_order_relaxed); ++i) {
            if (HMODULE shim = GetModuleHandleW(L"nvngx.dll"); shim) {
                hookInitOn(shim, "nvngx.dll");
            }
            if (HMODULE core = GetModuleHandleW(L"_nvngx.dll"); core) {
                hookInitOn(core, "_nvngx.dll");
            }
            if (gInitHooked.load(std::memory_order_relaxed)) {
                break;
            }
            std::this_thread::sleep_for(1ms);
        }
        for (int i = 0; i < 3000; ++i) {
            if (gWatcherStop.load(std::memory_order_relaxed) || gNgxHooked.load(std::memory_order_relaxed)) {
                break;
            }
            if (GetModuleHandleW(L"_nvngx.dll") && GetModuleHandleW(L"nvngx_dlss.dll")) {
                std::this_thread::sleep_for(150ms);
                if (gNgxHooked.exchange(true)) {
                    break;
                }
                hookNgxExports();
                break;
            }
            std::this_thread::sleep_for(20ms);
        }

        // Phase 3: the UI overlay needs the game window (and, on D3D12, the game's
        // command queue). If enable() ran before either existed, keep trying here
        // instead of leaving the panel unavailable for the whole session.
        for (int i = 0; i < 240 && !gWatcherStop.load(std::memory_order_relaxed); ++i) {
            if (ui::Overlay::getInstance().isInstalled()) return;
            if (ui::Overlay::getInstance().install()) return;
            std::this_thread::sleep_for(250ms);
        }
    });
}

} // namespace

bool install() {
    auto& log    = utils::logger();
    // Pin this module for the process lifetime. Unhooking while another thread is
    // inside one of our detours leaves it returning into freed trampoline memory
    // (0xC000001D, 2026-10-21), and the only safe alternative is to never unload
    // the code the hooks jump to. Disable therefore *soft-disables*: the detours
    // stay installed and pass straight through.
    {
        HMODULE self = nullptr;
        if (GetModuleHandleExW(
                GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
                reinterpret_cast<LPCWSTR>(&install),
                &self
            )) {
            log.debug("[probe] 模块已 PIN（禁用后仍需重启游戏才能彻底卸载）");
        }
    }
    gActive.store(true, std::memory_order_relaxed);
    config::reload();
    gNrEnabled = config::get().nr.enabled;
    auto const   modDir = NeuroDragon::getInstance().getSelf().getModDir();
    std::error_code modDirError;
    auto const   modDirAbs = std::filesystem::absolute(modDir, modDirError);
    std::wstring modDirFull = (modDirError ? modDir : modDirAbs).wstring();
    nr::nrSetModDirectory(modDirFull.c_str());
    neurodragon::log::write(0, gNrEnabled ? "nr.info.enabled"_tr() : "nr.info.disabled"_tr());
    if (gNrEnabled) {
        registerLevelListeners();
    }
    startWatcher();
    return true;
}

void uninstall() {
    // Soft disable: the hooks stay installed (the module is pinned) and turn into
    // pass-throughs, which is the only version that cannot crash the client on
    // disable. A real unload needs a process restart.
    gActive.store(false, std::memory_order_relaxed);
    gWatcherStop.store(true, std::memory_order_relaxed);
    if (gWatcherThread.joinable()) {
        gWatcherThread.join();
    }
    nr::nrShutdown();
    auto& bus = ll::event::EventBus::getInstance();
    if (gJoinLevelListener) {
        bus.removeListener<ll::event::client::ClientJoinLevelEvent>(gJoinLevelListener);
        gJoinLevelListener.reset();
    }
    if (gExitLevelListener) {
        bus.removeListener<ll::event::client::ClientExitLevelEvent>(gExitLevelListener);
        gExitLevelListener.reset();
    }
}

bool isInstalled() { return gNgxHooked.load(std::memory_order_relaxed); }

} // namespace neurodragon::probe
