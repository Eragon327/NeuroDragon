#include "neurodragon/vv/VvDlssSr.h"

#include "neurodragon/Config.h"
#include "neurodragon/probe/VvProbe.h"
#include "neurodragon/sr/SrSharpening.h"

#include <Windows.h>
#include <d3d12.h>
#include <dxgi1_4.h>

// <d3d12.h> first, then the engine-header shim, then anything that reaches mc/.
#include "neurodragon/Macro.h"

#include "neurodragon/utils/Utils.h"

#include <atomic>
#include <cstdio>
#include <filesystem>
#include <mutex>
#include <string>

namespace neurodragon::vv::dlss {
namespace {

// Same values the offline ngx_probe uses (docs/specs/nr.md §2.1 / ngx_probe.cpp).
constexpr int         kApiVersion   = 0x15;
constexpr char const* kProjectId    = "f1b2e5d8-9c4a-4e7b-8a36-5d2e90c47a11";
constexpr char const* kEngineVersion = "0.1.0-neural-rendering-source";

// This core's Init_ProjectID puts the api version BEFORE the FeatureCommonInfo
// pointer (the SDK header says the opposite; ngx_probe.cpp line 33 has the same
// warning). Passing the header order dereferences 0x15 inside the core.
using InitProjectFn = int (*)(char const*, int, char const*, wchar_t const*, void*, int, void const*);

// Mirrors nvsdk_ngx_defs.h; the SDK headers stay out of this TU.
struct PathList {
    wchar_t const* const* path;
    unsigned              length;
};
struct LoggingInfo {
    void* callback;
    int   minimumLevel;
    bool  disableOtherSinks;
};
struct FeatureCommonInfo {
    PathList    pathList;
    void*       internalData;
    LoggingInfo logging;
};
using GetCapsFn     = int (*)(void**);
using CreateFn      = int (*)(void*, int, void*, void**);
using EvaluateFn    = int (*)(void*, void*, void const*, void*, void*);
using ReleaseFn     = int (*)(void*);

HMODULE   gCore{};
void*     gParams{};
CreateFn  gCreate{};
EvaluateFn gEvaluate{};
ReleaseFn gRelease{};
bool      gInitialized{};

// The core may keep the FeatureCommonInfo path list for the whole session (it loads
// the model lazily at CreateFeature), so these must outlive the init call - a local
// std::wstring here would dangle by the time CreateFeature runs.
std::wstring       gAppData;
std::wstring       gGameDir;
wchar_t const*     gPathList[1]{};
FeatureCommonInfo  gCommon{};

// _nvngx.dll lives in the driver store on this machine (the engine loads it from
// there in RT mode). System32 may hold a copy on other installs, so try both, then
// the DriverStore repository glob. Returns the first readable file.
std::wstring findCorePath() {
    wchar_t systemDir[MAX_PATH]{};
    if (!::GetSystemDirectoryW(systemDir, MAX_PATH)) {
        return {};
    }
    std::wstring const base = systemDir;
    for (wchar_t const* name : {L"\\_nvngx.dll", L"\\nvngx.dll"}) {
        std::wstring candidate = base + name;
        if (::GetFileAttributesW(candidate.c_str()) != INVALID_FILE_ATTRIBUTES) {
            return candidate;
        }
    }
    std::wstring const pattern = base + L"\\DriverStore\\FileRepository\\nvami.inf_amd64_*";
    WIN32_FIND_DATAW   data{};
    HANDLE const       find = ::FindFirstFileW(pattern.c_str(), &data);
    if (find == INVALID_HANDLE_VALUE) {
        return {};
    }
    std::wstring found;
    do {
        std::wstring const candidate =
            base + L"\\DriverStore\\FileRepository\\" + data.cFileName + L"\\_nvngx.dll";
        if (::GetFileAttributesW(candidate.c_str()) != INVALID_FILE_ATTRIBUTES) {
            found = candidate;
            break;
        }
    } while (::FindNextFileW(find, &data));
    ::FindClose(find);
    return found;
}

void logState(char const* what, int ret) {
    ND_DEBUG("[vvsr] {} ret=0x{:08X}", what, static_cast<unsigned>(ret));
    (void)what;
    (void)ret;
}

} // namespace

// NGX's own logging channel. The logging callback is part of FeatureCommonInfo
// (nvsdk_ngx_defs.h: void (*)(char const* message, NVSDK_NGX_Logging_Level,
// NVSDK_NGX_Feature)); handing it to Init_ProjectID is the supported way to hear why
// a parameter or a feature is rejected, instead of guessing at names and formats.
// Debug level and TEST-only: the core is verbose, and a release log must stay readable.
void __cdecl ngxLog(char const* message, int level, int feature) {
    ND_DEBUG("[ngx] lv={} feat={} {}", level, feature, message ? message : "");
    (void)message;
    (void)level;
    (void)feature;
}

namespace {

using SetPtrFn = int (*)(void*, char const*, void*);
using SetU32Fn = int (*)(void*, char const*, unsigned);
using SetI32Fn = int (*)(void*, char const*, int);
using SetF32Fn = int (*)(void*, char const*, float);

// Same vtable slots the NR bridge measured: 1 = pointer, 3 = unsigned, 4 = int,
// 6 = float. All writes go through these; no direct struct knowledge.
int paramPtr(void* params, char const* name, void* value) {
    auto** const vt = *reinterpret_cast<void***>(params);
    return reinterpret_cast<SetPtrFn>(vt[1])(params, name, value);
}
int paramU32(void* params, char const* name, unsigned value) {
    auto** const vt = *reinterpret_cast<void***>(params);
    return reinterpret_cast<SetU32Fn>(vt[3])(params, name, value);
}
int paramI32(void* params, char const* name, int value) {
    auto** const vt = *reinterpret_cast<void***>(params);
    return reinterpret_cast<SetI32Fn>(vt[4])(params, name, value);
}
int paramF32(void* params, char const* name, float value) {
    auto** const vt = *reinterpret_cast<void***>(params);
    return reinterpret_cast<SetF32Fn>(vt[6])(params, name, value);
}

void*     gSrFeature{};
unsigned  gSrRenderW{};
unsigned  gSrRenderH{};
unsigned  gSrDisplayW{};
unsigned  gSrDisplayH{};
SrState   gSrState{};
std::mutex gSrMutex;
// Set by requestFeatureRebuild(): the model preset is only read at CreateFeature, so
// changing it means dropping the feature and letting the next frame build a new one.
std::atomic<bool> gSrRebuild{false};

using GetU32Fn = int (*)(void*, char const*, unsigned*);
using GetF32Fn = int (*)(void*, char const*, float*);

// Read one unsigned parameter back through the measured getter slot (11 = uint).
// Returns false when the object has no such getter/key - the caller must then report
// "not applied" instead of the value it asked for.
bool readBackU32(void* params, char const* name, unsigned* out) {
    if (!params || !name || !out) {
        return false;
    }
    auto** const vt = *reinterpret_cast<void***>(params);
    if (!vt || !vt[11]) {
        return false;
    }
    unsigned  u  = 0;
    int const ru = reinterpret_cast<GetU32Fn>(vt[11])(params, name, &u);
    if (ru != 1) {
        return false;
    }
    *out = u;
    return true;
}

// Read a parameter back through the measured getter slots: proves a write landed
// rather than assuming it did.
void logParamReadback(void* params, char const* name) {
#ifdef TEST
    unsigned    u  = 0;
    int const   ru = readBackU32(params, name, &u) ? 1 : 0;
    auto** const vt = *reinterpret_cast<void***>(params);
    float       f  = 0.0f;
    int const   rf = reinterpret_cast<GetF32Fn>(vt[14])(params, name, &f);
    utils::logger().debug("[vvsr] sr readback {}: u32 ret={} val={} | f32 ret={} val={:.3f}", name, ru, u, rf, f);
#else
    (void)params;
    (void)name;
#endif
}

// Session bootstrap: core from the driver store, Init_ProjectID with the game
// directory as the model search path (the missing path list was what made the very
// first CreateFeature answer 0xBAD0000B).
bool ensureSession(ID3D12Device* device) {
    if (gInitialized) {
        return gParams != nullptr;
    }
    if (!gCore) {
        if (HMODULE const loaded = ::GetModuleHandleW(L"_nvngx.dll")) {
            gCore = loaded;
        } else {
            std::wstring const path = findCorePath();
            if (path.empty()) {
                utils::logger().error("[vvsr] sr: no _nvngx.dll found");
                return false;
            }
            gCore = ::LoadLibraryW(path.c_str());
        }
        if (!gCore) {
            return false;
        }
    }
    auto const caps = reinterpret_cast<GetCapsFn>(::GetProcAddress(gCore, "NVSDK_NGX_D3D12_GetCapabilityParameters"));
    gCreate   = reinterpret_cast<CreateFn>(::GetProcAddress(gCore, "NVSDK_NGX_D3D12_CreateFeature"));
    gEvaluate = reinterpret_cast<EvaluateFn>(::GetProcAddress(gCore, "NVSDK_NGX_D3D12_EvaluateFeature"));
    gRelease  = reinterpret_cast<ReleaseFn>(::GetProcAddress(gCore, "NVSDK_NGX_D3D12_ReleaseFeature"));
    if (!caps || !gCreate || !gEvaluate || !gRelease) {
        utils::logger().error("[vvsr] sr: core is missing feature entry points");
        return false;
    }
    if (!gInitialized) {
        wchar_t temp[MAX_PATH]{};
        ::GetTempPathW(MAX_PATH, temp);
        gAppData = std::wstring(temp) + L"NeuroDragon";
        ::CreateDirectoryW(gAppData.c_str(), nullptr);
        gAppData += L"\\NGX";
        ::CreateDirectoryW(gAppData.c_str(), nullptr);
        gGameDir     = utils::applicationDirectory();
        gPathList[0] = gGameDir.c_str();
        gCommon      = FeatureCommonInfo{};
        gCommon.pathList.path          = gPathList;
        gCommon.pathList.length        = gGameDir.empty() ? 0 : 1;
        gCommon.logging.callback       = reinterpret_cast<void*>(&ngxLog);
        gCommon.logging.minimumLevel   = 2; // verbose
        gCommon.logging.disableOtherSinks = false;
        auto const initProject = reinterpret_cast<InitProjectFn>(::GetProcAddress(gCore, "NVSDK_NGX_D3D12_Init_ProjectID"));
        int const  ret = initProject
            ? initProject(kProjectId, 0, kEngineVersion, gAppData.c_str(), device, kApiVersion, &gCommon)
            : 0;
        logState("sr Init_ProjectID", ret);
        if (ret != 1) {
            return false;
        }
        gInitialized = true;
    }
    void* params = nullptr;
    int const capsRet = caps(&params);
    logState("sr GetCapabilityParameters", capsRet);
    if (capsRet != 1 || !params) {
        return false;
    }
    gParams = params;
    return true;
}

} // namespace

bool ensureSrFeature(void* deviceVoid, void* commandList, unsigned renderWidth, unsigned renderHeight, unsigned displayWidth, unsigned displayHeight) {
    auto* const device = static_cast<ID3D12Device*>(deviceVoid);
    if (!device || !commandList) {
        return false;
    }
    std::lock_guard lock(gSrMutex);
    if (!ensureSession(device)) {
        gSrState.ready = false;
        return false;
    }
    // A pending rebuild request drops the feature even when the sizes still match: the
    // model preset is a CreateFeature-time parameter, so there is no other way to apply
    // it (the RT path's quality-slot nudge exists for the same reason).
    bool const rebuild = gSrRebuild.exchange(false, std::memory_order_acq_rel);
    if (rebuild && gSrFeature) {
        gRelease(gSrFeature);
        gSrFeature = nullptr;
        ND_DEBUG("[vvsr] sr feature released for a rebuild (preset change)");
    }
    if (gSrFeature && gSrRenderW == renderWidth && gSrRenderH == renderHeight
        && gSrDisplayW == displayWidth && gSrDisplayH == displayHeight) {
        gSrState.ready = true;
        return true;
    }
    if (gSrFeature) {
        gRelease(gSrFeature);
        gSrFeature = nullptr;
    }
    void* params = gParams;
    paramU32(params, "Width", renderWidth);
    paramU32(params, "Height", renderHeight);
    paramU32(params, "OutWidth", displayWidth);
    paramU32(params, "OutHeight", displayHeight);
    paramI32(params, "PerfQualityValue", 3);
    paramU32(params, "CreationNodeMask", 1);
    paramU32(params, "VisibilityNodeMask", 1);
    paramI32(params, "DLSS.Feature.Create.Flags", 11); // IsHDR | MVLowRes | AutoExposure
    // Model preset, the same six keys and the same values the engine path writes
    // (src/neurodragon/nr/D3D12Bridge.cpp, srApplyCreateOverrides): 0 = driver default,
    // 5/6 = E/F, 10/11/12/13 = J/K/L/M. Our session's parameter object is our own, so
    // there is no engine 0x80000000 bit to preserve; 0 is still written so a previous
    // choice does not survive a return to "default". `gSrState.preset` is NOT set here:
    // it is the read-back below, because a runtime that refuses the key would otherwise
    // show the request as applied.
    int const preset = config::get().sr.preset;
    {
        static char const* const keys[] = {
            "DLSS.Hint.Render.Preset.DLAA",
            "DLSS.Hint.Render.Preset.Quality",
            "DLSS.Hint.Render.Preset.Balanced",
            "DLSS.Hint.Render.Preset.Performance",
            "DLSS.Hint.Render.Preset.UltraPerformance",
            "DLSS.Hint.Render.Preset.UltraQuality",
        };
        for (char const* key : keys) {
            paramU32(params, key, static_cast<unsigned>(preset));
        }
    }
    void*     feature = nullptr;
    int const ret     = gCreate(commandList, 1, params, &feature);
    gSrState.createRet     = ret;
    gSrState.renderWidth   = renderWidth;
    gSrState.renderHeight  = renderHeight;
    gSrState.displayWidth  = displayWidth;
    gSrState.displayHeight = displayHeight;
    if (ret != 1 || !feature) {
        // A feature that cannot be created is a user-visible failure: our DLSS never
        // runs and the engine's TAAU stays. Say it once per attempt sequence, with the
        // runtime's return code, instead of a per-frame line.
        static std::atomic<unsigned> failures{0};
        unsigned const               n = failures.fetch_add(1, std::memory_order_relaxed) + 1;
        if (n <= 3) {
            utils::logger().warn(
                "[vvsr] sr CreateFeature(1) failed ret=0x{:08X} ({}x{} -> {}x{})",
                static_cast<unsigned>(ret),
                renderWidth,
                renderHeight,
                displayWidth,
                displayHeight
            );
        }
        gSrState.ready = false;
        return false;
    }
    ND_DEBUG(
        "[vvsr] sr CreateFeature(1) ret=0x{:08X} {}x{} -> {}x{}",
        static_cast<unsigned>(ret),
        renderWidth,
        renderHeight,
        displayWidth,
        displayHeight
    );
    gSrFeature = feature;
    // Preset read-back, not the request: the panel's "preset" row must show what the
    // runtime actually holds for the quality slot. A failed read (no getter, no key)
    // reports 0 = driver default, which is also what the keys above ask for when the
    // user picked "default" - never the value we merely asked for.
    unsigned appliedPreset = 0;
    if (readBackU32(params, "DLSS.Hint.Render.Preset.Quality", &appliedPreset)) {
        gSrState.preset = static_cast<int>(appliedPreset);
    } else {
        gSrState.preset = 0;
        ND_DEBUG("[vvsr] sr preset read-back unavailable; reporting the driver default");
    }
    logParamReadback(params, "Width");
    logParamReadback(params, "OutWidth");
    // Same key, raw getter return next to the value: a TEST build can tell "the runtime
    // answered 0" from "the key is not there" (both end up as preset = 0).
    logParamReadback(params, "DLSS.Hint.Render.Preset.Quality");
    gSrRenderW = renderWidth;
    gSrRenderH = renderHeight;
    gSrDisplayW = displayWidth;
    gSrDisplayH = displayHeight;
    gSrState.ready = true;
    return true;
}

bool evaluateSr(void* commandList, void* color, void* depth, void* motionVectors, void* output, float jitterX, float jitterY, bool reset) {
    std::lock_guard lock(gSrMutex);
    if (!gSrFeature || !gParams || !gEvaluate) {
        return false;
    }
    void* params = gParams;
    // Key names are the runtime's, not the header's: the dots are part of the name
    // ("Jitter.Offset.X", not "JitterOffsetX"). 2026-10-25 this function wrote the
    // header spelling, every EvaluateFeature answered 0xBAD00005, and the feature's
    // own log said "could not find Jitter.Offset.X parameter". Slot 6 is Set(float):
    // the NR bridge in this same process reads the same dotted keys from the same
    // parameter object through the same vtable.
    [[maybe_unused]] int const colorRet = paramPtr(params, "Color", color);
    [[maybe_unused]] int const depthRet = paramPtr(params, "Depth", depth);
    [[maybe_unused]] int const mvRet    = paramPtr(params, "MotionVectors", motionVectors);
    [[maybe_unused]] int const outRet   = paramPtr(params, "Output", output);
    // The TAA-context projection has ndc.z = 1 - near/viewDepth: far/sky is 1,
    // near is 0, so DLSS must treat the input as inverted depth.
    paramI32(params, "DepthInverted", 1);
    paramI32(params, "Reset", reset ? 1 : 0);
    paramF32(params, "Jitter.Offset.X", jitterX);
    paramF32(params, "Jitter.Offset.Y", jitterY);
    // The motion vectors are produced and consumed in pixels, so the scale is 1.
    paramF32(params, "MV.Scale.X", 1.0f);
    paramF32(params, "MV.Scale.Y", 1.0f);
    static std::atomic<unsigned> logged{0};
    if (logged.fetch_add(1, std::memory_order_relaxed) < 2) {
        ND_DEBUG(
            "[vvsr] sr params colour={} depth={} mv={} out={} jitter={:.2f},{:.2f} reset={}",
            colorRet,
            depthRet,
            mvRet,
            outRet,
            jitterX,
            jitterY,
            reset ? 1 : 0
        );
        // Read back the float pair the NGX log complained about: proves the write
        // landed rather than assuming it did.
        logParamReadback(params, "Jitter.Offset.X");
        logParamReadback(params, "MV.Scale.X");
    }
    // This call goes through the RT probe's EvaluateFeature hook (that is how NR is
    // spliced in), and that hook also records the engine's RCAS. The engine pass
    // assumes the engine output's resource state; this output is ours, so mark the
    // call and let VvSr record the sharpening with the states it owns.
    neurodragon::sr::OwnEvaluateScope const ownEvaluate;
    int const                              ret = gEvaluate(commandList, gSrFeature, params, nullptr, nullptr);
    // The NR detour runs inside this call and (on a shared parameter object) used to
    // overwrite MV.Scale with the render size. Read it back once after the evaluate:
    // VV's pixel-space vectors must still see 1.0 (EXP-0088).
    static std::atomic<bool> loggedScaleAfter{false};
    if (!loggedScaleAfter.exchange(true, std::memory_order_relaxed)) {
        logParamReadback(params, "MV.Scale.X");
    }
    gSrState.evaluateRet = ret;
    return ret == 1;
}

SrState srState() {
    std::lock_guard lock(gSrMutex);
    return gSrState;
}

void requestFeatureRebuild() {
    gSrRebuild.store(true, std::memory_order_release);
}

void releaseSr() {
    std::lock_guard lock(gSrMutex);
    if (gSrFeature && gRelease) {
        gRelease(gSrFeature);
    }
    gSrFeature = nullptr;
    gSrState   = SrState{};
}

} // namespace neurodragon::vv::dlss
