#include "neurodragon/vv/VvDlssSr.h"

#include "neurodragon/Config.h"
#include "neurodragon/probe/VvProbe.h"

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
constexpr unsigned long long kGenericCmsId = 0x0876232Cu;
constexpr int                kApiVersion   = 0x15;
constexpr char const*        kProjectId    = "f1b2e5d8-9c4a-4e7b-8a36-5d2e90c47a11";
constexpr char const*        kEngineVersion = "0.1.0-neural-rendering-source";

using InitExtFn     = int (*)(unsigned long long, wchar_t const*, void*, int, void const*);
using InitFn        = int (*)(unsigned long long, wchar_t const*, void*, int);
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

std::mutex gMutex;
ProbeState gState{};
std::atomic<bool> gRequested{false};
std::atomic<bool> gRunning{false};

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

template <class F>
void setState(F&& fn) {
    std::lock_guard lock(gMutex);
    fn(gState);
}

std::string narrow(wchar_t const* text) {
    if (!text || !*text) {
        return {};
    }
    int const needed = ::WideCharToMultiByte(CP_UTF8, 0, text, -1, nullptr, 0, nullptr, nullptr);
    if (needed <= 1) {
        return {};
    }
    std::string out(static_cast<std::size_t>(needed), '\0');
    ::WideCharToMultiByte(CP_UTF8, 0, text, -1, out.data(), needed, nullptr, nullptr);
    out.resize(static_cast<std::size_t>(needed - 1));
    return out;
}

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
    utils::logger().info("[vvsr] {} ret=0x{:08X}", what, static_cast<unsigned>(ret));
}

} // namespace

void requestProbe() { gRequested.store(true, std::memory_order_release); }

ProbeState probeState() {
    std::lock_guard lock(gMutex);
    return gState;
}

// NGX's own logging channel. The logging callback is part of FeatureCommonInfo
// (nvsdk_ngx_defs.h: void (*)(char const* message, NVSDK_NGX_Logging_Level,
// NVSDK_NGX_Feature)); handing it to Init_ProjectID is the supported way to hear why
// a parameter or a feature is rejected, instead of guessing at names and formats.
void __cdecl ngxLog(char const* message, int level, int feature) {
    static std::atomic<unsigned> lines{0};
    unsigned const n = lines.fetch_add(1, std::memory_order_relaxed);
    if (n < 240) {
        utils::logger().info("[ngx] lv={} feat={} {}", level, feature, message ? message : "");
    }
}

void runPending() {
    if (!gRequested.exchange(false, std::memory_order_acq_rel)) {
        return;
    }
    if (gRunning.exchange(true, std::memory_order_acq_rel)) {
        return;
    }
    struct Guard {
        ~Guard() { gRunning.store(false, std::memory_order_release); }
    } guard;

    unsigned const rw = probe::vv::renderWidth();
    unsigned const rh = probe::vv::renderHeight();
    unsigned const dw = probe::vv::displayWidth();
    unsigned const dh = probe::vv::displayHeight();
    auto* const device = static_cast<ID3D12Device*>(probe::vv::engineDevice());

    setState([&](ProbeState& s) {
        s = ProbeState{};
        s.renderWidth  = rw;
        s.renderHeight = rh;
        s.displayWidth = dw;
        s.displayHeight = dh;
        s.coreLoaded   = gCore != nullptr;
        s.initialized  = gInitialized;
        s.params       = gParams;
    });

    if (!device) {
        utils::logger().warn("[vvsr] probe skipped: engine device not latched yet");
        setState([](ProbeState& s) { s.done = true; });
        return;
    }
    if (!rw || !rh || !dw || !dh) {
        utils::logger().warn("[vvsr] probe skipped: sizes unknown ({}x{} -> {}x{})", rw, rh, dw, dh);
        setState([](ProbeState& s) { s.done = true; });
        return;
    }

    // --- core module --------------------------------------------------------
    if (!gCore) {
        if (HMODULE const loaded = ::GetModuleHandleW(L"_nvngx.dll")) {
            gCore = loaded;
        } else {
            std::wstring const path = findCorePath();
            if (path.empty()) {
                utils::logger().error("[vvsr] no _nvngx.dll found (System32 or DriverStore)");
                setState([](ProbeState& s) { s.done = true; });
                return;
            }
            utils::logger().info("[vvsr] loading core: {}", narrow(path.c_str()));
            gCore = ::LoadLibraryW(path.c_str());
        }
        if (!gCore) {
            utils::logger().error("[vvsr] LoadLibrary(_nvngx.dll) failed, GetLastError={}", ::GetLastError());
            setState([](ProbeState& s) { s.done = true; });
            return;
        }
    }
    setState([](ProbeState& s) { s.coreLoaded = true; });

    auto const caps = reinterpret_cast<GetCapsFn>(::GetProcAddress(gCore, "NVSDK_NGX_D3D12_GetCapabilityParameters"));
    gCreate = reinterpret_cast<CreateFn>(::GetProcAddress(gCore, "NVSDK_NGX_D3D12_CreateFeature"));
    gEvaluate = reinterpret_cast<EvaluateFn>(::GetProcAddress(gCore, "NVSDK_NGX_D3D12_EvaluateFeature"));
    gRelease = reinterpret_cast<ReleaseFn>(::GetProcAddress(gCore, "NVSDK_NGX_D3D12_ReleaseFeature"));
    if (!caps || !gCreate || !gRelease) {
        utils::logger().error("[vvsr] core is missing GetCapabilityParameters/CreateFeature/ReleaseFeature");
        setState([](ProbeState& s) { s.done = true; });
        return;
    }

    // --- session -----------------------------------------------------------
    if (!gInitialized) {
        wchar_t temp[MAX_PATH]{};
        ::GetTempPathW(MAX_PATH, temp);
        gAppData = std::wstring(temp) + L"NeuroDragon";
        ::CreateDirectoryW(gAppData.c_str(), nullptr);
        gAppData += L"\\NGX";
        ::CreateDirectoryW(gAppData.c_str(), nullptr);

        auto const initExt = reinterpret_cast<InitExtFn>(::GetProcAddress(gCore, "NVSDK_NGX_D3D12_Init_Ext"));
        auto const init    = reinterpret_cast<InitFn>(::GetProcAddress(gCore, "NVSDK_NGX_D3D12_Init"));
        auto const initProject =
            reinterpret_cast<InitProjectFn>(::GetProcAddress(gCore, "NVSDK_NGX_D3D12_Init_ProjectID"));

        // The path list is the whole point of this shape: the core loads the DLSS
        // model by name from these directories. Without it, Init succeeds but
        // CreateFeature(1) answers 0xBAD0000B (UnableToInitializeFeature) - the
        // exact result of the first B1a run on 2026-10-25 04:04. `nvngx_dlss.dll`
        // sits next to the game executable, so that directory goes in the list.
        gGameDir     = utils::applicationDirectory();
        gPathList[0] = gGameDir.c_str();
        gCommon      = FeatureCommonInfo{};
        gCommon.pathList.path          = gPathList;
        gCommon.pathList.length        = gGameDir.empty() ? 0 : 1;
        gCommon.logging.callback       = reinterpret_cast<void*>(&ngxLog);
        gCommon.logging.minimumLevel   = 2; // verbose
        gCommon.logging.disableOtherSinks = false;

        int ret = 0xBAD00001;
        if (initProject) {
            ret = initProject(kProjectId, 0, kEngineVersion, gAppData.c_str(), device, kApiVersion, &gCommon);
            logState("Init_ProjectID(pathlist)", ret);
        }
        if (ret != 1 && initExt) {
            ret = initExt(kGenericCmsId, gAppData.c_str(), device, kApiVersion, &gCommon);
            logState("Init_Ext(pathlist)", ret);
        }
        if (ret != 1 && init) {
            ret = init(kGenericCmsId, gAppData.c_str(), device, kApiVersion);
            logState("Init", ret);
        }
        setState([&](ProbeState& s) { s.initRet = ret; });
        if (ret != 1) {
            utils::logger().error("[vvsr] NGX session init failed; not creating a feature");
            setState([](ProbeState& s) { s.done = true; });
            return;
        }
        gInitialized = true;
        setState([](ProbeState& s) { s.initialized = true; });
    }

    // --- capability parameters ---------------------------------------------
    void* params = nullptr;
    int const capsRet = caps(&params);
    logState("GetCapabilityParameters", capsRet);
    setState([&](ProbeState& s) { s.capsRet = capsRet; s.params = params; });
    if (capsRet != 1 || !params) {
        utils::logger().error("[vvsr] no capability parameter object; cannot create a feature");
        setState([](ProbeState& s) { s.done = true; });
        return;
    }
    gParams = params;

    // Parameter object writes use the measured NGX slots (docs/specs/nr.md §3.3):
    // 3 = unsigned, 4 = int, 1 = pointer.
    auto** const vt = *reinterpret_cast<void***>(params);
    using SetU32Fn = int (*)(void*, char const*, unsigned);
    using SetI32Fn = int (*)(void*, char const*, int);
    auto const setU32 = reinterpret_cast<SetU32Fn>(vt[3]);
    auto const setI32 = reinterpret_cast<SetI32Fn>(vt[4]);
    setU32(params, "Width", rw);
    setU32(params, "Height", rh);
    setU32(params, "OutWidth", dw);
    setU32(params, "OutHeight", dh);
    setI32(params, "PerfQualityValue", 3);
    setU32(params, "CreationNodeMask", 1);
    setU32(params, "VisibilityNodeMask", 1);
    setI32(params, "DLSS.Feature.Create.Flags", 11); // IsHDR | MVLowRes | AutoExposure

    // --- one temporary command list, execute it, wait ----------------------
    ID3D12CommandAllocator*    allocator = nullptr;
    ID3D12GraphicsCommandList* list      = nullptr;
    ID3D12CommandQueue*        queue     = nullptr;
    ID3D12Fence*               fence     = nullptr;
    HANDLE                     event     = ::CreateEventW(nullptr, FALSE, FALSE, nullptr);
    HRESULT                    hr        = device->CreateCommandAllocator(
        D3D12_COMMAND_LIST_TYPE_DIRECT,
        IID_PPV_ARGS(&allocator)
    );
    if (SUCCEEDED(hr)) {
        hr = device->CreateCommandList(
            0,
            D3D12_COMMAND_LIST_TYPE_DIRECT,
            allocator,
            nullptr,
            IID_PPV_ARGS(&list)
        );
    }
    if (SUCCEEDED(hr)) {
        D3D12_COMMAND_QUEUE_DESC queueDesc{};
        queueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        hr = device->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&queue));
    }
    if (SUCCEEDED(hr)) {
        hr = device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence));
    }
    if (FAILED(hr) || !list || !allocator || !queue || !fence || !event) {
        utils::logger().error("[vvsr] temporary list/queue/fence creation failed hr=0x{:08X}", static_cast<unsigned>(hr));
        if (list) list->Release();
        if (allocator) allocator->Release();
        if (queue) queue->Release();
        if (fence) fence->Release();
        if (event) ::CloseHandle(event);
        setState([](ProbeState& s) { s.done = true; });
        return;
    }

    void*     feature = nullptr;
    int const createRet = gCreate(list, 1, params, &feature);
    logState("CreateFeature(1)", createRet);
    setState([&](ProbeState& s) { s.createRet = createRet; s.feature = feature; });

    list->Close();
    ID3D12CommandList* lists[1] = {list};
    queue->ExecuteCommandLists(1, lists);
    queue->Signal(fence, 1);
    if (fence->GetCompletedValue() < 1) {
        fence->SetEventOnCompletion(1, event);
        ::WaitForSingleObject(event, 3000);
    }

    if (feature && gRelease) {
        int const releaseRet = gRelease(feature);
        logState("ReleaseFeature", releaseRet);
        setState([](ProbeState& s) { s.feature = nullptr; });
    }
    if (createRet == 1 && feature) {
        utils::logger().info("[vvsr] B1a OK: feature 1 created and released ({}x{} -> {}x{})", rw, rh, dw, dh);
    } else {
        utils::logger().warn("[vvsr] B1a FAILED: CreateFeature(1) ret=0x{:08X} handle={:#x}",
                             static_cast<unsigned>(createRet),
                             reinterpret_cast<std::uintptr_t>(feature));
    }

    ::CloseHandle(event);
    fence->Release();
    list->Release();
    allocator->Release();
    queue->Release();
    setState([](ProbeState& s) { s.done = true; });
}

void release() {
    // The feature is released at the end of each probe; only the session remains.
    // Releasing the core while it is initialized is not required and risks the
    // driver, so it stays loaded until process exit.
    std::lock_guard lock(gMutex);
    gState.feature = nullptr;
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

// Read a parameter back through the measured getter slots: proves a write landed
// rather than assuming it did.
void logParamReadback(void* params, char const* name) {
    auto** const vt = *reinterpret_cast<void***>(params);
    unsigned    u  = 0;
    int const   ru = reinterpret_cast<GetU32Fn>(vt[11])(params, name, &u);
    float       f  = 0.0f;
    int const   rf = reinterpret_cast<GetF32Fn>(vt[14])(params, name, &f);
    utils::logger().info("[vvsr] sr readback {}: u32 ret={} val={} | f32 ret={} val={:.3f}", name, ru, u, rf, f);
}

// Session bootstrap, shared shape with the B1a probe: core from the driver store,
// Init_ProjectID with the game directory as the model search path (the missing path
// list was what made the very first CreateFeature answer 0xBAD0000B).
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
        utils::logger().info("[vvsr] sr feature released for a rebuild (preset change)");
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
    // choice does not survive a return to "default".
    {
        int const preset = config::get().sr.preset;
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
        gSrState.preset = preset;
    }
    void*     feature = nullptr;
    int const ret     = gCreate(commandList, 1, params, &feature);
    gSrState.createRet     = ret;
    gSrState.renderWidth   = renderWidth;
    gSrState.renderHeight  = renderHeight;
    gSrState.displayWidth  = displayWidth;
    gSrState.displayHeight = displayHeight;
    utils::logger().info(
        "[vvsr] sr CreateFeature(1) ret=0x{:08X} {}x{} -> {}x{}",
        static_cast<unsigned>(ret),
        renderWidth,
        renderHeight,
        displayWidth,
        displayHeight
    );
    if (ret != 1 || !feature) {
        gSrState.ready = false;
        return false;
    }
    gSrFeature = feature;
    logParamReadback(params, "Width");
    logParamReadback(params, "OutWidth");
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
    int const colorRet = paramPtr(params, "Color", color);
    int const depthRet = paramPtr(params, "Depth", depth);
    int const mvRet    = paramPtr(params, "MotionVectors", motionVectors);
    int const outRet   = paramPtr(params, "Output", output);
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
        utils::logger().info(
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
    int const ret = gEvaluate(commandList, gSrFeature, params, nullptr, nullptr);
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
