#include "neurodragon/fg/SwapchainProbe.h"
#include "neurodragon/utils/Utils.h"

#include "ll/api/memory/Hook.h"
#include "mc/external/bgfx/RendererContextD3D12.h"

#include <atomic>

#ifdef TEST

// F0-a support. The first flip is the frame boundary the factory probe's timestamps
// are compared against: if it happens before the probe is installed, rendering had
// already started and a factory created earlier cannot be observed. The target is
// the SDK's $flip thunk rather than DXGI Present, because F1 replaces the DXGI
// swap-chain vtable with Streamline's proxy and Present stops being a stable
// once-per-frame point.
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
    origin();
}

#endif
