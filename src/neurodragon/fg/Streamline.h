#pragma once

#include <string>

namespace neurodragon::fg::streamline {

/// F1 step 1: load Streamline from <mod>/fg-runtime, verify sl.interposer.dll's
/// signature, and call slInit in manual-hooking mode with DLSS-G / Reflex / PCL
/// requested. No factory or swap chain is touched - wrapping is step 2.
///
/// The runtime DLLs are the ones shipped next to the mod (sl.interposer/common/
/// dlss_g/reflex/pcl); nvngx_dlssg.dll is found in the driver store through
/// pathsToPlugins, so the driver owns it.
bool init(std::wstring const& modDir);

/// F1 step 2: replace a native IDXGIFactory with Streamline's proxy (manual hooking).
/// The caller keeps returning the proxy to the game; the native factory is still where
/// the swap-chain methods are hooked.
bool upgradeFactory(void** factory);

/// Replace the game's native D3D12 device with SL's proxy so CreateCommandQueue
/// reaches Streamline; slSetD3DDevice still receives the native device.
bool upgradeDevice(void** device);

/// Return the native swap-chain behind an SL proxy, or the input on failure. Used by
/// the F3 proxy Present path because the fake buffer is not ready for panel drawing yet.
void* nativeInterface(void* proxy);

/// Give Streamline the native device captured by D3D12CreateDevice.
bool ensureDevice(void* device);

/// The D3D12CreateDevice hook reports the game's device here (the swap-chain `device`
/// argument is the command queue, not the device - passing it to slSetD3DDevice
/// returned eErrorUnsupportedInterface).
void noteDevice(void* device);

/// Count flips and log the DLSS-G chain state at 1/600/1200… frames. The eOff
/// options are set once before the chain is created; repeating them on every flip
/// raced SL's own Present bookkeeping and brought the exit hang back (EXP-0143).
void presentFrame();

/// F2: the per-frame exports a frame-input submission needs, resolved here and cast back
/// by fg/FrameInputs.cpp. Opaque on purpose - this header is included by files that must
/// not pull the Streamline SDK (the engine headers stub three d3d12 structs, so sl.h's
/// conversion operators do not compile there).
struct FrameFunctions {
    void* newFrameToken{};  ///< PFun_slGetNewFrameToken*
    void* setConstants{};   ///< PFun_slSetConstants*
    void* setTagForFrame{}; ///< PFun_slSetTagForFrame*
    void* dlssgGetState{};  ///< PFun_slDLSSGGetState*
    void* pclSetMarker{};   ///< PFun_slPCLSetMarker*
    void* reflexSleep{};    ///< PFun_slReflexSleep*
};

/// True once the three per-frame exports exist. They arrive in two steps - the core
/// three at slInit, the DLSS-G state query only after slSetD3DDevice - so a caller must
/// ask each frame instead of latching a false. `dlssgGetState` may stay null; the
/// input fence is then simply not tracked.
[[nodiscard]] bool frameFunctions(FrameFunctions* out);

/// True when the last accepted DLSS-G mode is `eOff`.
[[nodiscard]] bool dlssgOff();

/// True only when DLSS-G and Reflex were both accepted with fixed-2x generation on.
[[nodiscard]] bool dlssgOn();

/// F3: change DLSS-G and Reflex modes only when the requested state differs from the
/// last accepted one. This never repeats options frame-by-frame (EXP-0143).
bool setDlssgEnabled(bool enabled);

/// Log a non-S_OK removed reason for the native game device.
void logDeviceRemovedReason(char const* when);

/// Fallback for the swap-chain path: the queue's device via ID3D12CommandQueue::GetDevice.
bool ensureDeviceFromQueue(void* queue);

/// Unload DLSS-G, then shut down Streamline while its NGX instance manager is still
/// alive. The interposer stays loaded for the proxy swapchain's remaining lifetime.
bool unloadDlssg();

/// slShutdown + unload. Processes that are exiting do not need it (the project keeps
/// GPU-side objects for the process lifetime on purpose).
void shutdown();

[[nodiscard]] bool ready();

} // namespace neurodragon::fg::streamline
