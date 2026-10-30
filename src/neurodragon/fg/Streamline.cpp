#include "neurodragon/fg/Streamline.h"

#include "neurodragon/fg/streamline-sdk/sl.h"
#include "neurodragon/utils/Utils.h"

#include <Windows.h>

// The engine headers (utils/Utils.h -> mc) stub three d3d12 structs; rename the
// d3d12.h copies so both can live in one TU (same trick as ui/Overlay.cpp).
#define D3D12_FEATURE_DATA_D3D12_OPTIONS D3D12_FEATURE_DATA_D3D12_OPTIONS_LEGACY
#define D3D12_FEATURE_DATA_ARCHITECTURE  D3D12_FEATURE_DATA_ARCHITECTURE_LEGACY
#define D3D12_RAYTRACING_GEOMETRY_DESC   D3D12_RAYTRACING_GEOMETRY_DESC_LEGACY
#include <d3d12.h>
#undef D3D12_FEATURE_DATA_D3D12_OPTIONS
#undef D3D12_FEATURE_DATA_ARCHITECTURE
#undef D3D12_RAYTRACING_GEOMETRY_DESC

#include <softpub.h>
#include <wintrust.h>

#include <atomic>
#include <filesystem>
#include <string>
#include <vector>

namespace neurodragon::fg::streamline {
namespace {

struct State {
    HMODULE                        module{};
    PFun_slInit*                   init{};
    PFun_slShutdown*               shutdown{};
    PFun_slIsFeatureLoaded*        isLoaded{};
    PFun_slGetFeatureVersion*      featureVersion{};
    PFun_slGetFeatureRequirements* featureRequirements{};
    PFun_slUpgradeInterface*       upgrade{};
    PFun_slSetD3DDevice*           setDevice{};
    PFun_slSetFeatureLoaded*       setFeatureLoaded{};
    std::vector<sl::Feature>       features{sl::kFeatureReflex, sl::kFeaturePCL};
    std::vector<std::wstring>      paths;
    std::vector<wchar_t const*>    pathPtrs;
    std::wstring                   logDir;
    bool                           ready{};
    bool                           attempted{};
    bool                           deviceSet{};
    void*                          pendingDevice{};
};

State g;

void logMessage(sl::LogType type, char const* msg) {
    ND_DEBUG("[sl] lv={} {}", static_cast<int>(type), msg ? msg : "");
}

bool verifySignature(std::wstring const& path) {
    // Loaded dynamically for the same reason slInit is deferred: a new static import
    // in a DLL that LeviLamina loads is one more thing that can deadlock under the
    // loader lock. wintrust is only needed on the first factory call.
    using WinVerifyTrustFn = LONG(WINAPI*)(HWND, GUID*, LPVOID);
    HMODULE const wintrust = ::LoadLibraryW(L"wintrust.dll");
    if (!wintrust) {
        ND_DEBUG("[fg] streamline: wintrust.dll not available");
        return false;
    }
    auto const verify = reinterpret_cast<WinVerifyTrustFn>(::GetProcAddress(wintrust, "WinVerifyTrust"));
    if (!verify) {
        ND_DEBUG("[fg] streamline: WinVerifyTrust export missing");
        return false;
    }

    WINTRUST_FILE_INFO file{};
    file.cbStruct       = sizeof(file);
    file.pcwszFilePath  = path.c_str();

    WINTRUST_DATA data{};
    data.cbStruct            = sizeof(data);
    data.dwUIChoice          = WTD_UI_NONE;
    data.fdwRevocationChecks = WTD_REVOKE_NONE;
    data.dwUnionChoice       = WTD_CHOICE_FILE;
    data.pFile               = &file;
    data.dwStateAction       = WTD_STATEACTION_VERIFY;

    GUID        action = WINTRUST_ACTION_GENERIC_VERIFY_V2;
    LONG const  status = verify(nullptr, &action, &data);
    data.dwStateAction = WTD_STATEACTION_CLOSE;
    verify(nullptr, &action, &data);

    ND_DEBUG(
        "[fg] streamline: signature {} status=0x{:08x}",
        std::string(path.begin(), path.end()),
        static_cast<unsigned>(status)
    );
    return status == ERROR_SUCCESS;
}

std::wstring findNvngxDirectory() {
    wchar_t systemDir[MAX_PATH]{};
    if (!::GetSystemDirectoryW(systemDir, MAX_PATH)) {
        return {};
    }
    std::wstring const base    = systemDir;
    std::wstring const pattern = base + L"\\DriverStore\\FileRepository\\nv*.inf_amd64_*";

    WIN32_FIND_DATAW data{};
    HANDLE const     find = ::FindFirstFileW(pattern.c_str(), &data);
    if (find == INVALID_HANDLE_VALUE) {
        return {};
    }
    std::wstring best;
    FILETIME     bestTime{};
    do {
        std::wstring const dir  = base + L"\\DriverStore\\FileRepository\\" + data.cFileName;
        std::wstring const file = dir + L"\\nvngx_dlssg.dll";
        WIN32_FILE_ATTRIBUTE_DATA info{};
        if (!::GetFileAttributesExW(file.c_str(), GetFileExInfoStandard, &info)) {
            continue;
        }
        if (best.empty() || ::CompareFileTime(&info.ftLastWriteTime, &bestTime) > 0) {
            best     = dir;
            bestTime = info.ftLastWriteTime;
        }
    } while (::FindNextFileW(find, &data));
    ::FindClose(find);
    return best;
}

template <class T>
bool resolve(char const* name, T& out) {
    out = reinterpret_cast<T>(::GetProcAddress(g.module, name));
    if (!out) {
        ND_DEBUG("[fg] streamline: missing export {}", name);
        return false;
    }
    return true;
}

} // namespace

bool init(std::wstring const& modDir) {
    if (g.ready) {
        return true;
    }
    if (g.attempted) {
        return false;
    }
    g.attempted = true;
    ND_DEBUG("[fg] streamline: init attempt");

    // getModDir() is relative ("./mods/NeuroDragon"); LoadLibraryEx with
    // LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR rejects relative paths with error 87.
    std::filesystem::path modPath(modDir);
    std::error_code       pathError;
    if (modPath.is_relative()) {
        modPath = std::filesystem::absolute(modPath, pathError);
    }
    std::wstring const modRoot = modPath.wstring();
    std::wstring const runtime = modRoot + L"\\fg-runtime";
    std::wstring const interposer = runtime + L"\\sl.interposer.dll";
    if (::GetFileAttributesW(interposer.c_str()) == INVALID_FILE_ATTRIBUTES) {
        ND_DEBUG("[fg] streamline: runtime not found at {}", std::string(runtime.begin(), runtime.end()));
        return false;
    }
    if (!verifySignature(interposer)) {
        ND_DEBUG("[fg] streamline: refusing to load an unsigned sl.interposer.dll");
        return false;
    }

    ::SetDefaultDllDirectories(LOAD_LIBRARY_SEARCH_DEFAULT_DIRS | LOAD_LIBRARY_SEARCH_USER_DIRS);
    ::SetDllDirectoryW(runtime.c_str());
    ::AddDllDirectory(runtime.c_str());

    g.module = ::LoadLibraryExW(
        interposer.c_str(),
        nullptr,
        LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_USER_DIRS | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS
    );
    if (!g.module) {
        ND_DEBUG("[fg] streamline: LoadLibrary failed {}", ::GetLastError());
        return false;
    }
    if (!resolve("slInit", g.init) || !resolve("slShutdown", g.shutdown)
        || !resolve("slIsFeatureLoaded", g.isLoaded) || !resolve("slGetFeatureVersion", g.featureVersion)
        || !resolve("slGetFeatureRequirements", g.featureRequirements)
        || !resolve("slUpgradeInterface", g.upgrade) || !resolve("slSetD3DDevice", g.setDevice)
        || !resolve("slSetFeatureLoaded", g.setFeatureLoaded)) {
        return false;
    }

    g.paths.push_back(runtime);
    std::wstring const driver = findNvngxDirectory();
    if (!driver.empty()) {
        g.paths.push_back(driver);
        ND_DEBUG("[fg] streamline: nvngx search {}", std::string(driver.begin(), driver.end()));
    }
    for (auto const& path : g.paths) {
        g.pathPtrs.push_back(path.c_str());
    }

    g.logDir = modRoot + L"\\logs";
    std::error_code error;
    std::filesystem::create_directories(std::filesystem::path(g.logDir), error);

    sl::Preferences pref{};
    pref.flags            = sl::PreferenceFlags::eUseManualHooking
                          | sl::PreferenceFlags::eUseFrameBasedResourceTagging;
    pref.logLevel          = sl::LogLevel::eVerbose;
    pref.pathsToPlugins    = g.pathPtrs.data();
    pref.numPathsToPlugins = static_cast<uint32_t>(g.pathPtrs.size());
    pref.pathToLogsAndData = g.logDir.c_str();
    pref.logMessageCallback = &logMessage;
    pref.featuresToLoad    = g.features.data();
    pref.numFeaturesToLoad = static_cast<uint32_t>(g.features.size());
    pref.engine            = sl::EngineType::eCustom;
    pref.engineVersion     = "0.1.0-neurodragon-fg";
    pref.projectId         = "a0f57b54-1daf-4934-90ae-c4035c19df04";
    pref.renderAPI         = sl::RenderAPI::eD3D12;

    sl::Result const result = g.init(pref, sl::kSDKVersion);
    ND_DEBUG(
        "[fg] streamline: slInit {} (runtime={})",
        static_cast<int>(result),
        std::string(runtime.begin(), runtime.end())
    );
    if (result != sl::Result::eOk) {
        return false;
    }

    bool       loaded     = false;
    sl::Result loadResult = g.isLoaded(sl::kFeatureDLSS_G, loaded);
    ND_DEBUG(
        "[fg] streamline: DLSS_G loaded={} result={} (feature load deferred until the device exists)",
        loaded ? 1 : 0,
        static_cast<int>(loadResult)
    );

    g.ready = true;
    if (g.pendingDevice) {
        ensureDevice(g.pendingDevice);
    }
    return g.ready;
}

bool upgradeFactory(void** factory) {
    if (!g.ready || !g.upgrade || !factory || !*factory) {
        return false;
    }
    void* const       before  = *factory;
    sl::Result const  result  = g.upgrade(factory);
    ND_DEBUG(
        "[fg] streamline: slUpgradeInterface 0x{:x} -> 0x{:x} result={}",
        reinterpret_cast<std::uintptr_t>(before),
        reinterpret_cast<std::uintptr_t>(*factory),
        static_cast<int>(result)
    );
    return result == sl::Result::eOk && *factory;
}

bool ensureDevice(void* device) {
    if (!g.ready || !g.setDevice || !device) {
        return false;
    }
    if (g.deviceSet) {
        return true;
    }
    sl::Result const result = g.setDevice(device);
    ND_DEBUG(
        "[fg] streamline: slSetD3DDevice 0x{:x} result={}",
        reinterpret_cast<std::uintptr_t>(device),
        static_cast<int>(result)
    );
    g.deviceSet = result == sl::Result::eOk;
    if (g.deviceSet && g.setFeatureLoaded) {
        // DLSS-G is loaded only now: its plugin creates an internal pacer command
        // queue, and activating it before the device existed produced
        // "createCommandQueue nv.sl.dlss_g.cmdQ.pacer" + a failed proxy chain
        // (EXP-0126/0127).
        sl::Result const loadResult = g.setFeatureLoaded(sl::kFeatureDLSS_G, true);
        bool              loaded     = false;
        g.isLoaded(sl::kFeatureDLSS_G, loaded);
        ND_DEBUG(
            "[fg] streamline: DLSS_G load-after-device result={} loaded={}",
            static_cast<int>(loadResult),
            loaded ? 1 : 0
        );
        if (loaded && g.featureVersion) {
            sl::FeatureVersion version{};
            if (g.featureVersion(sl::kFeatureDLSS_G, version) == sl::Result::eOk) {
                ND_DEBUG(
                    "[fg] streamline: version SL={}.{}.{} NGX={}.{}.{}",
                    version.versionSL.major,
                    version.versionSL.minor,
                    version.versionSL.build,
                    version.versionNGX.major,
                    version.versionNGX.minor,
                    version.versionNGX.build
                );
            }
        }
    }
    return g.deviceSet;
}

void noteDevice(void* device) {
    if (!device) {
        return;
    }
    g.pendingDevice = device;
    if (g.ready) {
        ensureDevice(device);
    }
}

bool ensureDeviceFromQueue(void* queue) {
    if (!queue) {
        return false;
    }
    auto*         commandQueue = static_cast<ID3D12CommandQueue*>(queue);
    ID3D12Device* device       = nullptr;
    if (FAILED(commandQueue->GetDevice(IID_PPV_ARGS(&device))) || !device) {
        ND_DEBUG("[fg] streamline: queue GetDevice failed");
        return false;
    }
    bool const ok = ensureDevice(device);
    device->Release();
    return ok;
}

void shutdown() {
    if (g.shutdown) {
        g.shutdown();
    }
    if (g.module) {
        ::FreeLibrary(g.module);
        g.module = nullptr;
    }
    g.ready = false;
}

bool ready() { return g.ready; }

} // namespace neurodragon::fg::streamline
