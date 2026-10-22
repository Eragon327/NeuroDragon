// RT dispatch size probe.
//
// bgfx::EncoderImpl::doAllRayTracing is an MCAPI symbol declared in LL's own
// headers, so the hook is written with LL's macro: no byte scanning, no manual
// address resolution, and the hook installs itself at load. Its last two
// arguments are the size the ray tracing actually runs at, which is what decides
// whether NR belongs at 1485x835 or 2560x1440.
#include <mc/external/bgfx/EncoderImpl.h>

#include "ll/api/memory/Hook.h"

#include "neurodragon/NeuroDragon.h"

#include <atomic>
#include <cstdint>

// 文档: LeviLamina/docs/main/contents/developer_guides/how_to_guides/hook_guide.zh.md
LL_AUTO_TYPE_INSTANCE_HOOK(
    RtDispatchProbe,
    HookPriority::Normal,
    bgfx::EncoderImpl,
    &bgfx::EncoderImpl::doAllRayTracing,
    void,
    ushort                                 _id,
    ::bgfx::RayTracingConfiguration const& rtConfig,
    ::bgfx::RayTracingResources const&     rtResources,
    uint                                   _width,
    uint                                   _height
) {
    static std::atomic<std::uint64_t> calls{0};
    auto const                        n = calls.fetch_add(1, std::memory_order_relaxed) + 1;
    if (n <= 3 || n % 600 == 0) {
        neurodragon::NeuroDragon::getInstance().getSelf().getLogger().info(
            "[rt] #{} id={} {}x{}",
            n,
            _id,
            _width,
            _height
        );
    }
    origin(_id, rtConfig, rtResources, _width, _height);
}
