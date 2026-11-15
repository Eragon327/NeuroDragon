#include "neurodragon/fg/FrameInputs.h"
#include "neurodragon/fg/Streamline.h"
#include "neurodragon/fg/SwapchainProbe.h"
#include "neurodragon/utils/Utils.h"

#include "ll/api/memory/Hook.h"
#include "mc/external/bgfx/RendererContextD3D12.h"

#include <atomic>

// The frame boundary. The target is the SDK's $flip thunk rather than DXGI Present:
// F1 hands the chain to Streamline's proxy, so the Present the overlay sees is *inside*
// SL's own present path - by the time it runs, SL has already accounted for the frame,
// and a tag submitted there describes a frame SL has decided about. $flip runs once per
// frame, before SL touches the Present, which is the boundary the DLSS-G contract wants
// for slSetConstants/slSetTagForFrame (EXP-0150 review). This hook is unconditional: the
// inputs exist in release too, only the F0 diagnosis around it is TEST-only.
LL_AUTO_TYPE_INSTANCE_HOOK(
    RendererContextFlipHook,
    ll::memory::HookPriority::Normal,
    bgfx::d3d12::RendererContextD3D12,
    &bgfx::d3d12::RendererContextD3D12::$flip,
    void
) {
    static std::atomic_bool logged{};
    if (!logged.exchange(true, std::memory_order_relaxed)) {
        ND_DEBUG(
            "[fg] first flip t={:.1f}ms context={:p} swapChain={:p}",
            ::neurodragon::fg::probe::msSinceProcessStart(),
            static_cast<void*>(this),
            static_cast<void*>(this->m_swapChain)
        );
    }
    // TEST only inside (a release build's version is a no-op): the F0 factory/chain probe.
    ::neurodragon::fg::probe::installSwapChainHook(this->m_swapChain);
    // F1 bookkeeping: flip counter + the DLSS-G state readback every 600 flips.
    ::neurodragon::fg::streamline::presentFrame();
    // F2: constants + the three tags, once per frame, before the original flip (and thus
    // before Streamline's proxy Present).
    ::neurodragon::fg::inputs::presentFrame(this->m_swapChain);
    origin();
    ::neurodragon::fg::streamline::logDeviceRemovedReason("flip");
}
