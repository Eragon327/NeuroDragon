#include "neurodragon/fg/SwapchainProbe.h"

#include "neurodragon/utils/Utils.h"

#include <Windows.h>

#include <cstdint>

namespace neurodragon::fg::probe {

double msSinceProcessStart() {
    static ULONGLONG const start = [] {
        FILETIME creation{}, exit{}, kernel{}, user{};
        if (!::GetProcessTimes(::GetCurrentProcess(), &creation, &exit, &kernel, &user)) {
            return ULONGLONG{};
        }
        ULARGE_INTEGER value{};
        value.LowPart  = creation.dwLowDateTime;
        value.HighPart = creation.dwHighDateTime;
        return value.QuadPart;
    }();
    if (!start) return 0.0;

    FILETIME nowFileTime{};
    ::GetSystemTimeAsFileTime(&nowFileTime);
    ULARGE_INTEGER now{};
    now.LowPart  = nowFileTime.dwLowDateTime;
    now.HighPart = nowFileTime.dwHighDateTime;
    return now.QuadPart < start ? 0.0 : static_cast<double>(now.QuadPart - start) / 10000.0;
}

} // namespace neurodragon::fg::probe

#ifdef TEST

#include <MinHook.h>
#include <dxgi1_2.h>

#include <atomic>
#include <mutex>
#include <string>

namespace neurodragon::fg::probe {
namespace {

using CreateFactoryFn    = HRESULT(WINAPI*)(REFIID, void**);
using CreateFactory2Fn   = HRESULT(WINAPI*)(UINT, REFIID, void**);
using CreateSwapChainFn  = HRESULT(STDMETHODCALLTYPE*)(IDXGIFactory*, IUnknown*, DXGI_SWAP_CHAIN_DESC*, IDXGISwapChain**);
using CreateForHwndFn    = HRESULT(STDMETHODCALLTYPE*)(
    IDXGIFactory2*,
    IUnknown*,
    HWND,
    DXGI_SWAP_CHAIN_DESC1 const*,
    DXGI_SWAP_CHAIN_FULLSCREEN_DESC const*,
    IDXGIOutput*,
    IDXGISwapChain1**
);
using CreateForWindowFn  = HRESULT(STDMETHODCALLTYPE*)(
    IDXGIFactory2*,
    IUnknown*,
    IUnknown*,
    DXGI_SWAP_CHAIN_DESC1 const*,
    IDXGIOutput*,
    IDXGISwapChain1**
);
using CreateForComposeFn = HRESULT(STDMETHODCALLTYPE*)(
    IDXGIFactory2*,
    IUnknown*,
    DXGI_SWAP_CHAIN_DESC1 const*,
    IDXGIOutput*,
    IDXGISwapChain1**
);

// IDXGIFactory / IDXGIFactory2 vtable slots. Every Windows 10+ factory implements
// IDXGIFactory7, so all four slots exist on the object the game actually receives.
constexpr size_t kCreateSwapChainSlot      = 10;
constexpr size_t kCreateForHwndSlot        = 15;
constexpr size_t kCreateForCoreWindowSlot  = 16;
constexpr size_t kCreateForCompositionSlot = 24;

// One original per hooked entry point: MH_CreateHook stores the trampoline through
// the pointer it is given, so sharing one between two targets would leave the first
// detour calling the second target's trampoline.
CreateFactoryFn    gCreateFactory{};
CreateFactoryFn    gCreateFactory1{};
CreateFactory2Fn   gCreateFactory2{};
CreateSwapChainFn  gCreateSwapChain{};
CreateForHwndFn    gCreateForHwnd{};
CreateForWindowFn  gCreateForWindow{};
CreateForComposeFn gCreateForComposition{};

std::atomic_bool gInstalled{};
std::atomic_bool gFactoryHooked{};
std::mutex       gHookMutex;
void*            gHookedTargets[8]{};
int              gHookedCount{};

std::string callerStack() {
    void*       frames[16]{};
    USHORT      count = ::RtlCaptureStackBackTrace(1, 16, frames, nullptr);
    std::string text;
    for (USHORT i = 0; i < count && i < 6; ++i) {
        HMODULE module = nullptr;
        wchar_t path[MAX_PATH]{};
        if (!::GetModuleHandleExW(
                GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                reinterpret_cast<LPCWSTR>(frames[i]),
                &module
            )
            || !module || !::GetModuleFileNameW(module, path, MAX_PATH)) {
            continue;
        }
        std::wstring const full  = path;
        auto const         slash = full.find_last_of(L"\\/");
        std::wstring const file  = slash == std::wstring::npos ? full : full.substr(slash + 1);
        std::string const  name(file.begin(), file.end());
        if (!text.empty()) text += " <- ";
        text += fmt::format(
            "{} +0x{:x}",
            name,
            reinterpret_cast<std::uintptr_t>(frames[i]) - reinterpret_cast<std::uintptr_t>(module)
        );
    }
    return text;
}

bool hookTarget(char const* name, void* target, void* detour, void** original) {
    if (!target || !detour || !original) return false;
    std::lock_guard lock(gHookMutex);
    for (int i = 0; i < gHookedCount; ++i) {
        if (gHookedTargets[i] == target) return true;
    }
    MH_STATUS status = ::MH_CreateHook(target, detour, original);
    if (status != MH_OK) {
        ND_DEBUG("[fg] hook {} failed create status={}", name, static_cast<int>(status));
        return false;
    }
    status = ::MH_EnableHook(target);
    if (status != MH_OK) {
        ::MH_RemoveHook(target);
        ND_DEBUG("[fg] hook {} failed enable status={}", name, static_cast<int>(status));
        return false;
    }
    if (gHookedCount < 8) gHookedTargets[gHookedCount++] = target;
    ND_DEBUG("[fg] hook {} at 0x{:x}", name, reinterpret_cast<std::uintptr_t>(target));
    return true;
}

void logFactory(char const* which, HRESULT hr, void* factory) {
    ND_DEBUG(
        "[fg] factory {} t={:.1f}ms tid={} hr=0x{:08x} factory=0x{:x} caller={}",
        which,
        msSinceProcessStart(),
        ::GetCurrentThreadId(),
        static_cast<unsigned>(hr),
        reinterpret_cast<std::uintptr_t>(factory),
        callerStack()
    );
}

void logRequest(
    char const* which,
    void*       factory,
    void*       device,
    HWND        hwnd,
    unsigned    width,
    unsigned    height,
    unsigned    format,
    unsigned    buffers,
    unsigned    usage,
    unsigned    flags,
    unsigned    effect,
    unsigned    scaling
) {
    ND_DEBUG(
        "[fg] {} req t={:.1f}ms tid={} factory=0x{:x} device=0x{:x} hwnd=0x{:x} {}x{} fmt={} buffers={} usage=0x{:x} flags=0x{:x} effect={} scaling={} caller={}",
        which,
        msSinceProcessStart(),
        ::GetCurrentThreadId(),
        reinterpret_cast<std::uintptr_t>(factory),
        reinterpret_cast<std::uintptr_t>(device),
        reinterpret_cast<std::uintptr_t>(hwnd),
        width,
        height,
        format,
        buffers,
        usage,
        flags,
        effect,
        scaling,
        callerStack()
    );
}

void logChain(char const* which, HRESULT hr, void* chain) {
    ND_DEBUG(
        "[fg] {} done t={:.1f}ms hr=0x{:08x} chain=0x{:x}",
        which,
        msSinceProcessStart(),
        static_cast<unsigned>(hr),
        reinterpret_cast<std::uintptr_t>(chain)
    );
}

// Detours are forward-declared because hookFactory() takes their addresses and is
// called from the factory detours above them.
HRESULT WINAPI createFactoryDetour(REFIID riid, void** out);
HRESULT WINAPI createFactory1Detour(REFIID riid, void** out);
HRESULT WINAPI createFactory2Detour(UINT flags, REFIID riid, void** out);
HRESULT STDMETHODCALLTYPE
createSwapChainDetour(IDXGIFactory* self, IUnknown* device, DXGI_SWAP_CHAIN_DESC* desc, IDXGISwapChain** out);
HRESULT STDMETHODCALLTYPE createForHwndDetour(
    IDXGIFactory2*                        self,
    IUnknown*                             device,
    HWND                                  hwnd,
    DXGI_SWAP_CHAIN_DESC1 const*          desc,
    DXGI_SWAP_CHAIN_FULLSCREEN_DESC const* fullscreen,
    IDXGIOutput*                          restrictToOutput,
    IDXGISwapChain1**                     out
);
HRESULT STDMETHODCALLTYPE createForWindowDetour(
    IDXGIFactory2*               self,
    IUnknown*                    device,
    IUnknown*                    window,
    DXGI_SWAP_CHAIN_DESC1 const* desc,
    IDXGIOutput*                 restrictToOutput,
    IDXGISwapChain1**            out
);
HRESULT STDMETHODCALLTYPE createForCompositionDetour(
    IDXGIFactory2*               self,
    IUnknown*                    device,
    DXGI_SWAP_CHAIN_DESC1 const* desc,
    IDXGIOutput*                 restrictToOutput,
    IDXGISwapChain1**            out
);

// Install the swap-chain entry points on the first factory we see. The vtable slots
// are shared by every factory object of that class, so this is once per process.
void hookFactory(void* factory) {
    bool expected = false;
    if (!gFactoryHooked.compare_exchange_strong(expected, true)) return;

    void** vtbl = *reinterpret_cast<void***>(factory);
    if (!vtbl) {
        ND_DEBUG("[fg] factory 0x{:x} has no vtable", reinterpret_cast<std::uintptr_t>(factory));
        return;
    }

    hookTarget(
        "IDXGIFactory::CreateSwapChain",
        vtbl[kCreateSwapChainSlot],
        reinterpret_cast<void*>(&createSwapChainDetour),
        reinterpret_cast<void**>(&gCreateSwapChain)
    );

    IDXGIFactory2* factory2 = nullptr;
    if (SUCCEEDED(reinterpret_cast<IUnknown*>(factory)->QueryInterface(IID_PPV_ARGS(&factory2))) && factory2) {
        hookTarget(
            "IDXGIFactory2::CreateSwapChainForHwnd",
            vtbl[kCreateForHwndSlot],
            reinterpret_cast<void*>(&createForHwndDetour),
            reinterpret_cast<void**>(&gCreateForHwnd)
        );
        hookTarget(
            "IDXGIFactory2::CreateSwapChainForCoreWindow",
            vtbl[kCreateForCoreWindowSlot],
            reinterpret_cast<void*>(&createForWindowDetour),
            reinterpret_cast<void**>(&gCreateForWindow)
        );
        hookTarget(
            "IDXGIFactory2::CreateSwapChainForComposition",
            vtbl[kCreateForCompositionSlot],
            reinterpret_cast<void*>(&createForCompositionDetour),
            reinterpret_cast<void**>(&gCreateForComposition)
        );
        factory2->Release();
    } else {
        ND_DEBUG("[fg] factory 0x{:x} is not IDXGIFactory2", reinterpret_cast<std::uintptr_t>(factory));
    }
}

HRESULT WINAPI createFactoryDetour(REFIID riid, void** out) {
    HRESULT const hr = gCreateFactory ? gCreateFactory(riid, out) : E_FAIL;
    logFactory("CreateDXGIFactory", hr, out ? *out : nullptr);
    if (SUCCEEDED(hr) && out && *out) hookFactory(*out);
    return hr;
}

HRESULT WINAPI createFactory1Detour(REFIID riid, void** out) {
    HRESULT const hr = gCreateFactory1 ? gCreateFactory1(riid, out) : E_FAIL;
    logFactory("CreateDXGIFactory1", hr, out ? *out : nullptr);
    if (SUCCEEDED(hr) && out && *out) hookFactory(*out);
    return hr;
}

HRESULT WINAPI createFactory2Detour(UINT flags, REFIID riid, void** out) {
    HRESULT const hr = gCreateFactory2 ? gCreateFactory2(flags, riid, out) : E_FAIL;
    logFactory("CreateDXGIFactory2", hr, out ? *out : nullptr);
    if (SUCCEEDED(hr) && out && *out) hookFactory(*out);
    return hr;
}

HRESULT STDMETHODCALLTYPE
createSwapChainDetour(IDXGIFactory* self, IUnknown* device, DXGI_SWAP_CHAIN_DESC* desc, IDXGISwapChain** out) {
    if (desc) {
        logRequest(
            "CreateSwapChain",
            self,
            device,
            desc->OutputWindow,
            desc->BufferDesc.Width,
            desc->BufferDesc.Height,
            static_cast<unsigned>(desc->BufferDesc.Format),
            desc->BufferCount,
            static_cast<unsigned>(desc->BufferUsage),
            desc->Flags,
            static_cast<unsigned>(desc->SwapEffect),
            0
        );
    }
    HRESULT const hr = gCreateSwapChain ? gCreateSwapChain(self, device, desc, out) : E_FAIL;
    logChain("CreateSwapChain", hr, out ? *out : nullptr);
    return hr;
}

HRESULT STDMETHODCALLTYPE createForHwndDetour(
    IDXGIFactory2*                        self,
    IUnknown*                             device,
    HWND                                  hwnd,
    DXGI_SWAP_CHAIN_DESC1 const*          desc,
    DXGI_SWAP_CHAIN_FULLSCREEN_DESC const* fullscreen,
    IDXGIOutput*                          restrictToOutput,
    IDXGISwapChain1**                     out
) {
    if (desc) {
        logRequest(
            "CreateSwapChainForHwnd",
            self,
            device,
            hwnd,
            desc->Width,
            desc->Height,
            static_cast<unsigned>(desc->Format),
            desc->BufferCount,
            static_cast<unsigned>(desc->BufferUsage),
            desc->Flags,
            static_cast<unsigned>(desc->SwapEffect),
            static_cast<unsigned>(desc->Scaling)
        );
    }
    HRESULT const hr =
        gCreateForHwnd ? gCreateForHwnd(self, device, hwnd, desc, fullscreen, restrictToOutput, out) : E_FAIL;
    logChain("CreateSwapChainForHwnd", hr, out ? *out : nullptr);
    return hr;
}

HRESULT STDMETHODCALLTYPE createForWindowDetour(
    IDXGIFactory2*               self,
    IUnknown*                    device,
    IUnknown*                    window,
    DXGI_SWAP_CHAIN_DESC1 const* desc,
    IDXGIOutput*                 restrictToOutput,
    IDXGISwapChain1**            out
) {
    if (desc) {
        logRequest(
            "CreateSwapChainForCoreWindow",
            self,
            device,
            nullptr,
            desc->Width,
            desc->Height,
            static_cast<unsigned>(desc->Format),
            desc->BufferCount,
            static_cast<unsigned>(desc->BufferUsage),
            desc->Flags,
            static_cast<unsigned>(desc->SwapEffect),
            static_cast<unsigned>(desc->Scaling)
        );
    }
    HRESULT const hr =
        gCreateForWindow ? gCreateForWindow(self, device, window, desc, restrictToOutput, out) : E_FAIL;
    logChain("CreateSwapChainForCoreWindow", hr, out ? *out : nullptr);
    return hr;
}

HRESULT STDMETHODCALLTYPE createForCompositionDetour(
    IDXGIFactory2*               self,
    IUnknown*                    device,
    DXGI_SWAP_CHAIN_DESC1 const* desc,
    IDXGIOutput*                 restrictToOutput,
    IDXGISwapChain1**            out
) {
    if (desc) {
        logRequest(
            "CreateSwapChainForComposition",
            self,
            device,
            nullptr,
            desc->Width,
            desc->Height,
            static_cast<unsigned>(desc->Format),
            desc->BufferCount,
            static_cast<unsigned>(desc->BufferUsage),
            desc->Flags,
            static_cast<unsigned>(desc->SwapEffect),
            static_cast<unsigned>(desc->Scaling)
        );
    }
    HRESULT const hr =
        gCreateForComposition ? gCreateForComposition(self, device, desc, restrictToOutput, out) : E_FAIL;
    logChain("CreateSwapChainForComposition", hr, out ? *out : nullptr);
    return hr;
}

} // namespace

void install() {
    bool expected = false;
    if (!gInstalled.compare_exchange_strong(expected, true)) return;

    MH_STATUS status = ::MH_Initialize();
    if (status != MH_OK && status != MH_ERROR_ALREADY_INITIALIZED) {
        ND_DEBUG("[fg] MinHook initialization failed ({})", static_cast<int>(status));
        gInstalled.store(false, std::memory_order_release);
        return;
    }

    HMODULE dxgi = ::GetModuleHandleW(L"dxgi.dll");
    if (!dxgi) dxgi = ::LoadLibraryW(L"dxgi.dll");
    if (!dxgi) {
        ND_DEBUG("[fg] dxgi.dll not loaded");
        gInstalled.store(false, std::memory_order_release);
        return;
    }

    auto const createFactory  = ::GetProcAddress(dxgi, "CreateDXGIFactory");
    auto const createFactory1 = ::GetProcAddress(dxgi, "CreateDXGIFactory1");
    auto const createFactory2 = ::GetProcAddress(dxgi, "CreateDXGIFactory2");

    bool any = false;
    if (createFactory) {
        any |= hookTarget(
            "CreateDXGIFactory",
            reinterpret_cast<void*>(createFactory),
            reinterpret_cast<void*>(&createFactoryDetour),
            reinterpret_cast<void**>(&gCreateFactory)
        );
    }
    if (createFactory1) {
        any |= hookTarget(
            "CreateDXGIFactory1",
            reinterpret_cast<void*>(createFactory1),
            reinterpret_cast<void*>(&createFactory1Detour),
            reinterpret_cast<void**>(&gCreateFactory1)
        );
    }
    if (createFactory2) {
        any |= hookTarget(
            "CreateDXGIFactory2",
            reinterpret_cast<void*>(createFactory2),
            reinterpret_cast<void*>(&createFactory2Detour),
            reinterpret_cast<void**>(&gCreateFactory2)
        );
    }

    ND_DEBUG(
        "[fg] swapchain probe installed t={:.1f}ms hooks={} dxgi=0x{:x}",
        msSinceProcessStart(),
        any ? 1 : 0,
        reinterpret_cast<std::uintptr_t>(dxgi)
    );
    if (!any) gInstalled.store(false, std::memory_order_release);
}

void uninstall() {
    if (!gInstalled.exchange(false)) return;
    std::lock_guard lock(gHookMutex);
    for (int i = 0; i < gHookedCount; ++i) {
        ::MH_DisableHook(gHookedTargets[i]);
        ::MH_RemoveHook(gHookedTargets[i]);
    }
    gHookedCount = 0;
    gFactoryHooked.store(false, std::memory_order_release);
}

} // namespace neurodragon::fg::probe

#else

namespace neurodragon::fg::probe {

void install() {}
void uninstall() {}

} // namespace neurodragon::fg::probe

#endif
