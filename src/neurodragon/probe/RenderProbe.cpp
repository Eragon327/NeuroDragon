#include "neurodragon/probe/RenderProbe.h"

#include "neurodragon/NeuroDragon.h"
#include "neurodragon/nr/D3D12Bridge.h"

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
    NeuroDragon::getInstance().getSelf().getLogger().info(
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
    auto&      log   = NeuroDragon::getInstance().getSelf().getLogger();
    auto const frame = gFrame.load(std::memory_order_relaxed);
    for (auto& w : gResourceWatch) {
        if (!w.name || std::strcmp(w.name, name) != 0) {
            continue;
        }
        if (!w.described && value) {
            w.described = true;
            auto const info = nr::describe(value);
            log.info(
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
            log.info("[res] {} f={} -> {}", name, frame, isNull ? "null" : "set");
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
    NeuroDragon::getInstance().getSelf().getLogger().info(
        "[init] Init appId={:#x} api={:#x}", appId, static_cast<unsigned>(apiVersion));
    nr::nrSetEngineInit(appId, dataPath);
    return gInitOriginal ? gInitOriginal(appId, dataPath, device, apiVersion) : 0xBAD00007;
}

int ngxInitExtDetour(unsigned long long appId, wchar_t const* dataPath, void* device,
                     int apiVersion, void const* params) {
    NeuroDragon::getInstance().getSelf().getLogger().info(
        "[init] Init_Ext appId={:#x} api={:#x}", appId, static_cast<unsigned>(apiVersion));
    nr::nrSetEngineInit(appId, dataPath);
    return gInitExtOriginal ? gInitExtOriginal(appId, dataPath, device, apiVersion, params)
                            : 0xBAD00007;
}

int ngxInitProjectDetour(char const* projectId, int engineType, char const* engineVersion,
                         wchar_t const* dataPath, void* device, void const* info, int apiVersion) {
    NeuroDragon::getInstance().getSelf().getLogger().info(
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
    auto& log = NeuroDragon::getInstance().getSelf().getLogger();
    for (auto const& e : entries) {
        auto* target = reinterpret_cast<void*>(GetProcAddress(module, e.name));
        if (!target) {
            continue;
        }
        if (ll::memory::hook(target, e.detour, e.original, ll::memory::HookPriority::Normal) == 0) {
            log.info("[init] hooked {} on {}", e.name, which);
        }
    }
    gInitHooked.store(true, std::memory_order_relaxed);
}

// ---------------------------------------------------------------------------
// Stage 2: NR feature (NGX 0x12) at the engine's render resolution.
//
// Off unless "nr_swap.txt" sits next to the game executable. When on, one
// feature is created on the first frame whose colour/depth/MV are all bound,
// evaluated for a 60-frame window, timed with GPU timestamps and dumped twice
// (our output + the engine's Color) so "it ran" can be checked by eye.
// The engine's DLSS keeps its own inputs: nothing is swapped in this stage.
// ---------------------------------------------------------------------------

bool          gNrEnabled      = false;
bool          gNrStartTried   = false;
bool          gNrWindowOpen   = false;
int           gNrWindowIndex  = -1;
bool          gNrSizeWarned   = false;
unsigned      gNrCreateWidth  = 0;
unsigned      gNrCreateHeight = 0;
std::uint64_t gNrBaseFrame    = 0;
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
std::atomic_bool gInWorld{false};
std::atomic_bool gRestartWindows{false};
ll::event::ListenerPtr gJoinLevelListener;
ll::event::ListenerPtr gExitLevelListener;

void registerLevelListeners() {
    auto& log = NeuroDragon::getInstance().getSelf().getLogger();
    auto& bus = ll::event::EventBus::getInstance();
    gJoinLevelListener = bus.emplaceListener<ll::event::client::ClientJoinLevelEvent>([](auto&) {
        gInWorld.store(true, std::memory_order_relaxed);
        gRestartWindows.store(true, std::memory_order_relaxed);
    });
    gExitLevelListener = bus.emplaceListener<ll::event::client::ClientExitLevelEvent>([](auto&) {
        gInWorld.store(false, std::memory_order_relaxed);
        gRestartWindows.store(true, std::memory_order_relaxed);
    });
    log.info(
        "[nr] level 监听 join={} exit={}",
        gJoinLevelListener != nullptr,
        gExitLevelListener != nullptr
    );
}

bool fileNextToExe(char const* name) {
    wchar_t path[MAX_PATH]{};
    if (GetModuleFileNameW(nullptr, path, MAX_PATH) == 0) {
        return false;
    }
    std::wstring file(path);
    auto const   slash = file.find_last_of(L"\\/");
    if (slash == std::wstring::npos) {
        return false;
    }
    file.resize(slash + 1);
    for (char const* p = name; *p; ++p) {
        file.push_back(static_cast<wchar_t>(*p));
    }
    return GetFileAttributesW(file.c_str()) != INVALID_FILE_ATTRIBUTES;
}

void nrLogSink(int level, char const* message) {
    auto& log = NeuroDragon::getInstance().getSelf().getLogger();
    if (level <= 0) {
        log.info("{}", message);
    } else if (level == 1) {
        log.warn("{}", message);
    } else {
        log.error("{}", message);
    }
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
    // The engine rewrites Color/Output every frame (Output alternates between two
    // textures), so log the first frames only and then sparsely.
    static std::atomic<std::uint64_t> calls{0};
    auto const                        n     = calls.fetch_add(1, std::memory_order_relaxed) + 1;
    auto const                        frame = gFrame.load(std::memory_order_relaxed);
    if (frame >= 32 && n % 600 != 0) {
        return;
    }
    NeuroDragon::getInstance().getSelf().getLogger().info(
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
    static std::atomic<std::uint64_t> calls{0};
    auto const                        n = calls.fetch_add(1, std::memory_order_relaxed) + 1;
    if (n <= 200) {
        NeuroDragon::getInstance().getSelf().getLogger().info(
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
    auto&     log    = NeuroDragon::getInstance().getSelf().getLogger();
    log.info(
        "[create] feature={} params={:#x} ret={:#x} handle={:#x}",
        feature,
        reinterpret_cast<std::uintptr_t>(params),
        static_cast<unsigned>(result),
        handle ? reinterpret_cast<std::uintptr_t>(*handle) : std::uintptr_t{0}
    );
    if (result == 1 && validParamObj(params)) {
        NgxParamProbe probe{params};
        log.info("[create] w={} h={}", probe.u32("Width"), probe.u32("Height"));
    }
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
    auto&      log    = NeuroDragon::getInstance().getSelf().getLogger();
    auto const n      = gFrame.fetch_add(1, std::memory_order_relaxed) + 1;
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
                log.info("[dump] color_dump.bmp written max={} nz={}", peak, nz);
            }
        } else if (n == 2800) {
            nr::queueColorDump(cmdList, out, "output_dump.bmp");
        } else if (n == 3000) {
            float         peak = -1.0f;
            std::uint32_t nz   = 0;
            if (nr::flushColorDump(&peak, &nz)) {
                log.info("[dump] output_dump.bmp written max={} nz={}", peak, nz);
            }
        }
    }

    // ---- stage 2: NR in front of the engine's DLSS -------------------------
    bool inWindow = false;
    if (gNrEnabled && validParamObj(const_cast<void*>(params))) {
        // Entering/leaving a world re-anchors the window schedule: the menu's
        // frames must not consume the ten slots.
        if (gRestartWindows.exchange(false, std::memory_order_relaxed)) {
            gNrBaseFrame   = n;
            gNrWindowIndex = -1;
            gNrWindowOpen  = false;
            gNrReportAt    = 0;
            gNrSizeWarned  = false;
            log.info(
                "[nr] 窗口重新锚定 f={} inWorld={}（此前窗口全部作废）",
                n,
                gInWorld.load(std::memory_order_relaxed)
            );
        }

        bool const inWorld = gInWorld.load(std::memory_order_relaxed);
        if (!gNrStartTried && inWorld && n >= gNrBaseFrame + 2 && color && depth && mv && out
            && width && height) {
            gNrStartTried   = true;
            gNrCreateWidth  = width;
            gNrCreateHeight = height;
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
            if (inWorld && !gNrWindowOpen && gNrWindowIndex + 1 < kNrWindows
                && n >= gNrBaseFrame + 2 +
                        static_cast<std::uint64_t>(gNrWindowIndex + 1) * kNrWindowSpacing) {
                ++gNrWindowIndex;
                gNrWindowOpen  = true;
                gNrWindowStart = n;
                gNrWindowEnd   = n + kNrWindowLength;
                gNrDumpFrame   = n + 30;
                nr::nrSetWindowIndex(gNrWindowIndex);
                log.info(
                    "[nr] 窗口 #{} 开始 f={}..{}，dump 于 {}（不替换 Color，画面应保持不变）",
                    gNrWindowIndex,
                    gNrWindowStart,
                    gNrWindowEnd - 1,
                    gNrDumpFrame
                );
            }

            inWindow = gNrWindowOpen && n >= gNrWindowStart && n < gNrWindowEnd;
            if (inWindow && nr::nrIsCreated()) {
                if (width != gNrCreateWidth || height != gNrCreateHeight) {
                    if (!gNrSizeWarned) {
                        gNrSizeWarned = true;
                        log.warn(
                            "[nr] 尺寸变了：建 feature={}x{} 现值={}x{}，本窗口跳过记录（NR 不重建）",
                            gNrCreateWidth,
                            gNrCreateHeight,
                            width,
                            height
                        );
                    }
                } else {
                    auto const     mvInfo = nr::describe(mv);
                    unsigned const guideWidth =
                        mvInfo.width ? static_cast<unsigned>(mvInfo.width) : width;
                    unsigned const guideHeight =
                        mvInfo.height ? static_cast<unsigned>(mvInfo.height) : height;
                    bool const reset = n == gNrWindowStart;
                    nr::nrStampBegin(cmdList, n, 0);
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
                        reset
                    );
                    nr::nrStampEnd(cmdList, n, 0);
                    if (reset) {
                        log.info(
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
                    log.info("[nr] dump {} written max={} nz={}", name, peak, nz);
                }
                nrDumpName(name, sizeof(name), "color_ref", gNrWindowIndex);
                nr::queueColorDumpEncoded(cmdList, nr::nrEncodedColorResource(), name);
            } else if (inWindow && n == gNrDumpFrame + 4) {
                char          name[64];
                float         peak = -1.0f;
                std::uint32_t nz   = 0;
                if (nr::flushColorDump(&peak, &nz)) {
                    nrDumpName(name, sizeof(name), "color_ref", gNrWindowIndex);
                    log.info("[nr] dump {} written max={} nz={}", name, peak, nz);
                }
            }

            if (gNrWindowOpen && n >= gNrWindowEnd && !gNrReportAt) {
                gNrReportAt = n + kNrReportTail;
            }
            if (gNrReportAt && n >= gNrReportAt) {
                gNrReportAt   = 0;
                gNrWindowOpen = false;
                nr::nrStampTick(n);
                log.info("[nr] 窗口 #{} 统计：", gNrWindowIndex);
                nr::nrStampReport();
                nr::nrStampReset();
            }
        }
    }

    bool const measureBaseline = inWindow;
    if (measureBaseline) {
        nr::nrStampBegin(cmdList, n, 1);
    }
    int const result = gNgxEvalOriginal ? gNgxEvalOriginal(cmdList, handle, params, callback, userData) : 0;
    if (measureBaseline) {
        nr::nrStampEnd(cmdList, n, 1);
    }

    if (gNrEnabled) {
        nr::nrStampTick(n);
    }

    if (n <= 3 || n % 600 == 0) {
        log.info(
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
    auto&     log    = NeuroDragon::getInstance().getSelf().getLogger();
    log.info(
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
    auto& log  = NeuroDragon::getInstance().getSelf().getLogger();
    auto* core = GetModuleHandleW(L"_nvngx.dll");
    if (!core) {
        log.warn("[ngx] 未找到 _nvngx.dll");
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
            log.warn("[ngx] 找不到导出 {}", e.name);
            continue;
        }
        if (ll::memory::hook(target, e.detour, e.original, ll::memory::HookPriority::Normal) == 0) {
            if (gHookCount < 4) {
                gHookRecords[gHookCount].target = target;
                gHookRecords[gHookCount].detour = e.detour;
                ++gHookCount;
            }
            log.info("[ngx] hooked {} @ {:#x}", e.name, reinterpret_cast<std::uintptr_t>(target));
        } else {
            log.error("[ngx] {} 挂钩失败", e.name);
        }
    }
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
                return;
            }
            if (GetModuleHandleW(L"_nvngx.dll") && GetModuleHandleW(L"nvngx_dlss.dll")) {
                std::this_thread::sleep_for(150ms);
                if (gNgxHooked.exchange(true)) {
                    return;
                }
                hookNgxExports();
                return;
            }
            std::this_thread::sleep_for(20ms);
        }
    });
}

} // namespace

bool install() {
    auto& log    = NeuroDragon::getInstance().getSelf().getLogger();
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
            log.info("[probe] 模块已 PIN（禁用后仍需重启游戏才能彻底卸载）");
        }
    }
    gActive.store(true, std::memory_order_relaxed);
    gNrEnabled   = fileNextToExe("nr_swap.txt");
    nr::nrSetLogger(&nrLogSink);
    auto const   modDir = NeuroDragon::getInstance().getSelf().getModDir();
    std::error_code modDirError;
    auto const   modDirAbs = std::filesystem::absolute(modDir, modDirError);
    std::wstring modDirFull = (modDirError ? modDir : modDirAbs).wstring();
    nr::nrSetModDirectory(modDirFull.c_str());
    log.info(
        "[probe] install (nr_swap={}, modDir={} -> {})",
        gNrEnabled ? "on" : "off",
        modDir.string(),
        std::filesystem::path(modDirFull).string()
    );
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
