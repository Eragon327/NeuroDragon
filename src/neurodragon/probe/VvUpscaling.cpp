#include "neurodragon/probe/VvUpscaling.h"

#include "neurodragon/Config.h"
#include "neurodragon/probe/VvProbe.h"
#include "neurodragon/utils/Utils.h"
#include "neurodragon/vv/VvSr.h"

#include "ll/api/memory/Hook.h"

#include <Windows.h>

#include <atomic>
#include <cstddef>
#include <cstdint>

namespace neurodragon::probe::vv {
namespace {

// Canon: bgfxbridge_createdeferredrenderingcameras (win-26.51, rva 0x8c7c2f0),
// signature length 32, unique. docs/specs/vv.md §17.9:
//   member function: this=rcx, vector=rdx, shared_ptr<UpscalingData>=r8,
//   ShadowParameters=r9; UpscalingData::mUpscalingParameters starts at +0
//   (Mode int +0, factor float +4, mEnableDynamicResolution bool +8).
ll::memory::Signature const kSigCreateDeferredCameras = ll::memory::Signature::parse(
    "55 41 57 41 56 41 55 41 54 56 57 53 48 81 EC 48 06 00 00 48 8D AC 24 ?? ?? ?? ?? 44 0F 29 85"
);

// A shared_ptr passed by value is a pointer to its 16-byte copy (object, control).
struct SharedPtrLayout {
    void* object;
    void* control;
};

float factorForPercent(int percent) {
    switch (percent) {
    case 33:
        return 0.3334f;
    case 50:
        return 0.5f;
    case 58:
        return 0.58f;
    case 66:
        return 0.6667f;
    case 75:
        return 0.75f;
    case 100:
        return 1.0f;
    default:
        return 0.0f;
    }
}

bool readable(void const* p, std::size_t bytes) {
    if (!p) {
        return false;
    }
    MEMORY_BASIC_INFORMATION info{};
    if (!::VirtualQuery(p, &info, sizeof(info)) || info.State != MEM_COMMIT) {
        return false;
    }
    if ((info.Protect & (PAGE_NOACCESS | PAGE_GUARD)) != 0) {
        return false;
    }
    auto const begin = reinterpret_cast<std::uintptr_t>(p);
    auto const end   = reinterpret_cast<std::uintptr_t>(info.BaseAddress) + info.RegionSize;
    return begin + bytes <= end;
}

std::atomic<float>    gLastFactor{0.0f};
std::atomic<unsigned> gHits{0};
std::atomic<bool>     gNativeRender{false};

// Below 100% the hook only rewrites the factor. At 100% it also has to keep the
// engine's upscaling branch alive: the camera is built at 1.0, then the factor is
// restored below 1 so frameImpl still declares TAAU/arg10. The TAAU draw itself is
// skipped by the existing draw-level suppress, and the probe reports display size as
// the render size while gNativeRender is set.
constexpr float kBranchKeepaliveFactor = 0.6667f;

} // namespace

bool nativeRender() {
    return gNativeRender.load(std::memory_order_relaxed);
}

LL_AUTO_STATIC_HOOK(
    CreateDeferredCamerasHook,
    HookPriority::Normal,
    ::ll::memory::SignatureView{kSigCreateDeferredCameras},
    void*,
    void* self,
    void* views,
    void* shared,
    void* shadow
) {
    auto const& cfg             = config::get();
    int const   percent         = cfg.sr.vvPercent;
    bool const  nativeRequested = percent == 100;
    bool const  inWorld         = probe::vv::worldLoaded();
    bool        native          = false;
    bool        applied         = false;
    float       before          = 0.0f;
    std::uint8_t* data          = nullptr;

    // The menu panorama is composited outside the R3 arg10 path. Applying the native
    // keepalive there builds the camera at 1.0 but leaves the menu's sampler on the
    // sub-1 factor, which zooms the background to its top-left quarter (and survives
    // into the world's first frames). Leave the engine's own factor alone in the
    // menu; the keepalive starts only once a world is present.
    // The native keepalive is only safe once the SR pipeline can replace TAAU's
    // output. On the first world frames (NGX session/model still loading) the TAAU
    // would run at the branch factor on a display-sized colour and zoom the picture
    // to its top-left quarter. Defer 100% until a frame actually wrote arg10.
    bool const pipelineLive = neurodragon::vv::sr::live();
    bool const mayOverride  = shared && cfg.sr.vvEnabled && percent > 0
                              && (!nativeRequested || (inWorld && pipelineLive));
    if (mayOverride && readable(shared, sizeof(SharedPtrLayout))) {
        auto* sp = static_cast<SharedPtrLayout*>(shared);
        // Validate the object before touching it: if the Windows ABI ever moves the
        // shared_ptr out of r8, the check refuses instead of writing into a vector.
        if (readable(sp->object, 0x40)) {
            data                  = static_cast<std::uint8_t*>(sp->object);
            int const   mode      = *reinterpret_cast<int*>(data);
            float const current   = *reinterpret_cast<float*>(data + 0x4);
            float const wanted    = factorForPercent(percent);
            if (mode >= 0 && mode <= 3 && current > 0.0f && current <= 4.0f && wanted > 0.0f) {
                before = current;
                if (percent == 100) {
                    native = true;
                    *reinterpret_cast<float*>(data + 0x4) = 1.0f;
                } else {
                    *reinterpret_cast<float*>(data + 0x4) = wanted;
                }
                // The engine multiplies by the dynamic-resolution scale; pin it so
                // the requested ratio is what the camera and TAAU actually see.
                *reinterpret_cast<std::uint8_t*>(data + 0x8) = 0;
                applied                                       = true;
            }
        }
    }

    bool const nativeNow = native && applied;
    gNativeRender.store(nativeNow, std::memory_order_relaxed);
    auto const result = origin(self, views, shared, shadow);

    if (nativeNow && readable(data, 0x40)) {
        // frameImpl reads +0x4 after this call to decide whether the upscaling branch
        // exists. A factor of 1.0 removes TAAU/arg10 entirely (EXP-0089), so restore a
        // sub-1 value for the branch; the camera/resources were already built at 1.0.
        float const branch = (before > 0.0f && before < 1.0f) ? before : kBranchKeepaliveFactor;
        *reinterpret_cast<float*>(data + 0x4) = branch;
        static std::atomic<float> loggedBranch{-1.0f};
        if (loggedBranch.exchange(branch, std::memory_order_relaxed) != branch) {
            utils::logger().info(
                "[vv] native render: camera 1.000, branch {:.4f} (dynamic off, option untouched)",
                branch
            );
        }
    } else if (applied) {
        float const target = factorForPercent(percent);
        gLastFactor.store(target, std::memory_order_relaxed);
        gHits.fetch_add(1, std::memory_order_relaxed);
        static std::atomic<float> logged{-1.0f};
        if (logged.exchange(target, std::memory_order_relaxed) != target) {
            utils::logger().info(
                "[vv] direct factor {:.4f} -> {:.4f} (dynamic off, option untouched)",
                before,
                target
            );
        }
    }
    // 100% and below-100% are two different branches, and the direct branch's own log
    // is keyed on the target factor: a 66 -> 100 -> 66 round trip returns to the same
    // 0.6667, so that line stays silent. The transition itself is what shows the
    // downswitch rebuilt the camera at the sub-1 factor (EXP-0103).
    static std::atomic<int> lastNative{-1};
    int const               now      = nativeNow ? 1 : 0;
    int const               previous = lastNative.exchange(now, std::memory_order_relaxed);
    if (applied && previous != -1 && previous != now) {
        if (now == 1) {
            utils::logger().info("[vv] percent: {}% -> native 100% (camera rebuilt at 1.000)", percent);
        } else {
            utils::logger().info(
                "[vv] percent: native 100% -> {}% (camera rebuilt at {:.4f})",
                percent,
                factorForPercent(percent)
            );
        }
    }
    return result;
}

} // namespace neurodragon::probe::vv
