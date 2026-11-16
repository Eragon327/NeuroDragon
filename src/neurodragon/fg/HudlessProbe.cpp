#include "neurodragon/fg/HudlessProbe.h"

#include "neurodragon/probe/VvCalibrate.h"
#include "neurodragon/probe/VvProbe.h"

#include <Windows.h>

// utils/Utils.h pulls the engine headers, which stub three d3d12 structs. Rename the
// d3d12.h copies (the same trick ui/Overlay.cpp and fg/Streamline.cpp use) so both can
// live in one translation unit.
#define D3D12_FEATURE_DATA_D3D12_OPTIONS D3D12_FEATURE_DATA_D3D12_OPTIONS_LEGACY
#define D3D12_FEATURE_DATA_ARCHITECTURE  D3D12_FEATURE_DATA_ARCHITECTURE_LEGACY
#define D3D12_RAYTRACING_GEOMETRY_DESC   D3D12_RAYTRACING_GEOMETRY_DESC_LEGACY
#include <d3d12.h>
#include <dxgi1_4.h>
#undef D3D12_FEATURE_DATA_D3D12_OPTIONS
#undef D3D12_FEATURE_DATA_ARCHITECTURE
#undef D3D12_RAYTRACING_GEOMETRY_DESC

#include "neurodragon/utils/Utils.h"

#include <atomic>
#include <cstdint>
#include <mutex>

namespace neurodragon::fg::hudless {
namespace {

// Three targets, not one: the copy of frame N is still readable at frame N's Present
// (eValidUntilPresent), so frame N+1 must not write the same texture. A slot is reused
// only after its own fence completed - the render thread never waits for the GPU, it
// skips the frame instead.
constexpr unsigned kSlots = 3;

// The hudless path does one more GPU-visible thing per round, so a failure can be pinned
// to the step that added it. Exactly one mode is compiled in; the others stay below,
// unreachable, so that restoring one is a one-line change and never a rewrite:
//   ProbeOnly   - read the back buffer and log it, submit nothing of ours (EXP-0171
//                 control round: stable on the real client).
//   BarrierOnly - submit *only* the back-buffer RENDER_TARGET -> COPY_SOURCE ->
//                 RENDER_TARGET pair in a list of our own. No target allocation, no
//                 CopyResource, no published Frame.
//   TrackedBarrierOnly - EXP-0179: use the display resource whose own engine trace
//                 proves RT -> PRESENT at or after the cut. No GetBuffer, no copy.
//   TrackedFullCopy - EXP-0180: the same proven source and states, plus CopyResource
//                 into our own texture and a published Frame. Still no GetBuffer.
//   FullCopy    - the old production path: allocate the copy target and CopyResource
//                 from GetBuffer (disproved by EXP-0178).
enum class Mode {
    ProbeOnly,
    BarrierOnly,
    TrackedBarrierOnly,
    TrackedFullCopy,
    FullCopy,
};

// EXP-0180 selects TrackedFullCopy. EXP-0179 proved the tracked source's barrier pair
// safe, so CopyResource into our own target and Frame publication are this round's only
// additions. The old GetBuffer FullCopy remains unreachable.
constexpr Mode kMode = Mode::TrackedFullCopy;

constexpr char const* modeName() {
    switch (kMode) {
    case Mode::ProbeOnly:
        return "probe-only";
    case Mode::BarrierOnly:
        return "barrier-only";
    case Mode::TrackedBarrierOnly:
        return "tracked-barrier-only";
    case Mode::TrackedFullCopy:
        return "tracked-full-copy";
    case Mode::FullCopy:
        return "full-copy";
    }
    return "probe-only";
}

struct Slot {
    ID3D12CommandAllocator*    allocator{};
    ID3D12GraphicsCommandList* list{};
    ID3D12Resource*            texture{};
    D3D12_RESOURCE_STATES      state{D3D12_RESOURCE_STATE_COMMON};
    UINT                       width{};
    UINT                       height{};
    DXGI_FORMAT                format{DXGI_FORMAT_UNKNOWN};
    UINT64                     fenceValue{}; ///< 0 = never submitted
    unsigned                   generation{}; ///< the invalidate() epoch of `texture`
};

struct State {
    ID3D12Device*       device{};
    ID3D12Fence*        fence{};
    ID3D12CommandQueue* queue{}; ///< not owned; latched by prepare() for submitted()
    UINT64              fenceNext{};
    unsigned            nextSlot{};
    unsigned            activeSlot{};
    bool                active{};
    unsigned            serial{};
    std::atomic<unsigned> generation{1}; ///< invalidate() epoch; new targets per epoch
    unsigned            skips{};
};

Slot  gSlots[kSlots]{};
State g;

std::mutex gFrameMutex;
Frame      gFrame{};

void transition(ID3D12GraphicsCommandList* list, ID3D12Resource* resource, D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after) {
    if (!resource || before == after) {
        return;
    }
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource   = resource;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barrier.Transition.StateBefore = before;
    barrier.Transition.StateAfter  = after;
    list->ResourceBarrier(1, &barrier);
}

// The copy target takes the back buffer's own format: DLSS-G compares the hudless
// input against the presented colour, and a converted copy would change the pixels'
// domain. Typeless formats are mapped to their typed sibling, which is what a copy
// destination and a shader read both accept.
[[maybe_unused]] DXGI_FORMAT copyFormat(DXGI_FORMAT format) {
    switch (format) {
    case DXGI_FORMAT_R10G10B10A2_TYPELESS:
        return DXGI_FORMAT_R10G10B10A2_UNORM;
    case DXGI_FORMAT_R8G8B8A8_TYPELESS:
        return DXGI_FORMAT_R8G8B8A8_UNORM;
    case DXGI_FORMAT_B8G8R8A8_TYPELESS:
        return DXGI_FORMAT_B8G8R8A8_UNORM;
    case DXGI_FORMAT_R16G16B16A16_TYPELESS:
        return DXGI_FORMAT_R16G16B16A16_FLOAT;
    default:
        return format;
    }
}

[[maybe_unused]] bool ensureDevice(ID3D12CommandQueue* queue) {
    if (!queue) {
        ND_DEBUG("[fg] hudless: gate no queue");
        return false;
    }
    D3D12_COMMAND_QUEUE_DESC const queueDesc = queue->GetDesc();
    if (queueDesc.Type != D3D12_COMMAND_LIST_TYPE_DIRECT) {
        ND_DEBUG("[fg] hudless: gate queue type={} is not DIRECT", static_cast<unsigned>(queueDesc.Type));
        return false;
    }
    if (g.device) {
        return true;
    }
    ID3D12Device* device = nullptr;
    if (FAILED(queue->GetDevice(IID_PPV_ARGS(&device))) || !device) {
        ND_DEBUG("[fg] hudless: queue has no device");
        return false;
    }
    bool ok = SUCCEEDED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&g.fence)));
    for (unsigned i = 0; ok && i < kSlots; ++i) {
        Slot& slot = gSlots[i];
        ok = ok && SUCCEEDED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&slot.allocator)));
        ok = ok && SUCCEEDED(device->CreateCommandList(
                     0,
                     D3D12_COMMAND_LIST_TYPE_DIRECT,
                     slot.allocator,
                     nullptr,
                     IID_PPV_ARGS(&slot.list)
                 ));
        // A list that is created but never closed cannot be Reset; closing it once here
        // puts every slot in the state record()/recordBarrierOnly() expect.
        if (ok) {
            slot.list->Close();
        }
    }
    if (!ok) {
        ND_DEBUG("[fg] hudless: device objects unavailable");
        device->Release();
        return false;
    }
    g.device = device;
    ND_DEBUG("[fg] hudless: device objects ready ({}, {} slots)", modeName(), kSlots);
    return true;
}

[[maybe_unused]] bool ensureTarget(Slot& slot, UINT width, UINT height, DXGI_FORMAT format) {
    if (slot.texture && slot.width == width && slot.height == height && slot.format == format
        && slot.generation == g.generation.load(std::memory_order_relaxed)) {
        return true;
    }
    if (slot.texture) {
        slot.texture->Release();
        slot.texture = nullptr;
        slot.state   = D3D12_RESOURCE_STATE_COMMON;
    }
    if (!width || !height) {
        return false;
    }
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension        = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width            = width;
    desc.Height           = height;
    desc.DepthOrArraySize = 1;
    desc.MipLevels        = 1;
    desc.Format           = format;
    desc.SampleDesc       = {1, 0};
    desc.Layout           = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    desc.Flags            = D3D12_RESOURCE_FLAG_NONE;
    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    if (FAILED(g.device->CreateCommittedResource(
            &heap,
            D3D12_HEAP_FLAG_NONE,
            &desc,
            D3D12_RESOURCE_STATE_COMMON,
            nullptr,
            IID_PPV_ARGS(&slot.texture)
        ))) {
        ND_DEBUG("[fg] hudless: target allocation failed {}x{} fmt={}", width, height, static_cast<unsigned>(format));
        return false;
    }
    slot.width      = width;
    slot.height     = height;
    slot.format     = format;
    slot.state      = D3D12_RESOURCE_STATE_COMMON;
    slot.generation = g.generation.load(std::memory_order_relaxed);
    return true;
}

// `indexOut` receives the index the resource was actually taken from (the chain's own
// GetCurrentBackBufferIndex when it is an IDXGISwapChain3, 0 otherwise). Diagnostic only:
// the returned reference belongs to the caller, and holding it past the submission makes
// the engine's ResizeBuffers fail (EXP-0139), so it is released in the same frame.
ID3D12Resource* currentBackBuffer(UINT* indexOut = nullptr) {
    auto* chain = static_cast<IDXGISwapChain*>(probe::vv::swapChain());
    if (!chain) {
        return nullptr;
    }
    UINT index = 0;
    if (IDXGISwapChain3* chain3 = nullptr;
        SUCCEEDED(chain->QueryInterface(IID_PPV_ARGS(&chain3))) && chain3) {
        index = chain3->GetCurrentBackBufferIndex();
        chain3->Release();
    }
    if (indexOut) {
        *indexOut = index;
    }
    ID3D12Resource* buffer = nullptr;
    if (FAILED(chain->GetBuffer(index, IID_PPV_ARGS(&buffer)))) {
        return nullptr;
    }
    return buffer;
}

// One list: back buffer -> COPY_SOURCE, full copy, back buffer back to
// RENDER_TARGET (the UI lists that follow still render into it), copy target ends in
// ALL_SHADER_RESOURCE - the state the FG tag declares for it.
[[maybe_unused]] bool record(Slot& slot, ID3D12Resource* backBuffer) {
    if (FAILED(slot.allocator->Reset()) || FAILED(slot.list->Reset(slot.allocator, nullptr))) {
        return false;
    }
    transition(slot.list, backBuffer, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
    transition(slot.list, slot.texture, slot.state, D3D12_RESOURCE_STATE_COPY_DEST);
    slot.list->CopyResource(slot.texture, backBuffer);
    transition(slot.list, backBuffer, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
    transition(slot.list, slot.texture, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);
    // The tracked state may only advance once the list is really closed: a failed Close
    // discards everything recorded above, so the texture is still in the state it had on
    // entry and the next attempt must transition from there.
    if (FAILED(slot.list->Close())) {
        return false;
    }
    slot.state = D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE;
    return true;
}

// BarrierOnly: exactly the two transitions the copy would have made on the engine's back
// buffer, and nothing else. The pair leaves the buffer in RENDER_TARGET before anyone else
// submits, so both the engine lists before the cut and the UI lists after it see the state
// they expect. This is the step EXP-0176 isolates: no target is created or converted, no
// CopyResource is recorded, and `slot.texture`/`slot.state` are untouched - they stay ready
// for a later FullCopy round.
[[maybe_unused]] bool recordBarrierOnly(Slot& slot, ID3D12Resource* backBuffer) {
    if (FAILED(slot.allocator->Reset()) || FAILED(slot.list->Reset(slot.allocator, nullptr))) {
        return false;
    }
    transition(slot.list, backBuffer, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
    transition(slot.list, backBuffer, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
    // A list that failed to close was not submitted: prepare() declines and the slot's
    // fence stays 0, so the next frame may reuse it immediately.
    return SUCCEEDED(slot.list->Close());
}

// Round-robin over the three slots, taking the first whose own fence already completed.
// A busy GPU means a skipped frame, never a wait (see kSlots).
[[maybe_unused]] Slot* takeSlot() {
    for (unsigned i = 0; i < kSlots; ++i) {
        unsigned const index     = (g.nextSlot + i) % kSlots;
        Slot&          candidate = gSlots[index];
        if (candidate.fenceValue == 0 || g.fence->GetCompletedValue() >= candidate.fenceValue) {
            g.nextSlot = (index + 1) % kSlots;
            return &candidate;
        }
    }
    ++g.skips;
    return nullptr;
}

// EXP-0177: what the engine itself recorded for this resource, from the barrier trace
// (probe::vv::barrierTrace, vtable slot 26). The two lines answer two different
// questions and are deliberately separate:
//
//   barrier-stats calls=0        -> the slot was never called: the detour is not
//                                   installed (a rejected/failed hook logs its own
//                                   warning), so the absence of a state line says
//                                   nothing about the buffer.
//   barrier-stats calls=N, none  -> the detour runs and this submission carries no
//                                   transition for that resource, i.e. the engine did
//                                   not move it in the lists being forwarded.
//
// `world` marks the record that belongs to lists[split-1]: the last list before the
// pre-UI cut, i.e. the world's display write. A transition recorded anywhere else in
// the submission is reported with world=0 instead of being hidden.
void logBarrierState(unsigned split, unsigned listCount, void* const* lists, ID3D12Resource const* buffer) {
    auto const stats = probe::vv::barrierStats();
    ND_DEBUG("[fg] hudless: barrier-stats calls={} records={} ticket={}", stats.calls, stats.records, stats.ticket);
    ID3D12Resource* const bufferNative = const_cast<ID3D12Resource*>(buffer);
    void* const worldList    = split && split <= listCount ? lists[split - 1] : nullptr;
    auto const ptr = [](void const* value) {
        return static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(value));
    };

    probe::vv::BarrierTrace trace{};
    bool const inSubmission = probe::vv::barrierTrace(buffer, lists, listCount, &trace, 1) != 0;
    if (inSubmission) {
        ND_DEBUG(
            "[fg] hudless: barrier-state buffer=0x{:x} list=0x{:x} world={} flags={} "
            "sub={} before=0x{:x} after=0x{:x} seq={}",
            ptr(buffer),
            ptr(trace.list),
            trace.list == worldList ? 1u : 0u,
            trace.flags,
            trace.subresource,
            trace.before,
            trace.after,
            trace.seq
        );
    } else {
        ND_DEBUG("[fg] hudless: barrier-state buffer=0x{:x} none", ptr(buffer));
        probe::vv::BarrierTrace any{};
        if (probe::vv::barrierTrace(buffer, nullptr, 0, &any, 1)) {
            ND_DEBUG(
                "[fg] hudless: barrier-state-any buffer=0x{:x} list=0x{:x} flags={} sub={} "
                "before=0x{:x} after=0x{:x} seq={}",
                ptr(buffer),
                ptr(any.list),
                any.flags,
                any.subresource,
                any.before,
                any.after,
                any.seq
            );
        } else {
            ND_DEBUG("[fg] hudless: barrier-state-any buffer=0x{:x} none", ptr(buffer));
        }
    }

    // EXP-0178: enumerate the submission's display-sized transitions as well. The
    // GetBuffer pointer is not necessarily the resource the engine rendered into on
    // an SL fake chain; `same` and `world` make that distinction directly checkable.
    D3D12_RESOURCE_DESC const bufferDesc = bufferNative->GetDesc();
    probe::vv::BarrierTrace   traces[32]{};
    unsigned const count = probe::vv::barrierTrace(nullptr, lists, listCount, traces, 32);
    unsigned       displays = 0;
    for (unsigned i = 0; i < count; ++i) {
        auto const& current = traces[i];
        if (!current.resource) {
            continue;
        }
        auto* const resource = static_cast<ID3D12Resource*>(current.resource);
        D3D12_RESOURCE_DESC const desc = resource->GetDesc();
        if (desc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || desc.Width != bufferDesc.Width
            || desc.Height != bufferDesc.Height || desc.Format != bufferDesc.Format) {
            continue;
        }
        ++displays;
        ND_DEBUG(
            "[fg] hudless: barrier-display resource=0x{:x} list=0x{:x} same={} world={} "
            "flags={} sub={} before=0x{:x} after=0x{:x} seq={}",
            ptr(current.resource),
            ptr(current.list),
            current.resource == bufferNative ? 1u : 0u,
            current.list == worldList ? 1u : 0u,
            current.flags,
            current.subresource,
            current.before,
            current.after,
            current.seq
        );
    }
    if (!displays) {
        ND_DEBUG("[fg] hudless: barrier-display none");
    }
}

// EXP-0171 probe-only: exactly the part of the copy path that reads the engine's back
// buffer, and nothing that touches the GPU. The resource is queried (index, pointer,
// desc) and released inside the same frame; the first three world frames whose cut is
// valid log the numbers so the rounds can be compared line by line. `swapchain` says
// whether the pointer GetBuffer answered with is one of the chain's own buffers (1) or
// not (0), which is the cheapest available statement about whose surface the copy would
// have read. Nothing here allocates, transitions, copies or signals - a probe-only frame
// submits no list of ours. EXP-0177 adds the barrier trace below: still reads only, and
// only on the three logged frames.
[[maybe_unused]] void recordProbeOnly(unsigned split, unsigned listCount, void* const* lists) {
    static std::atomic<unsigned> logged{0};
    UINT                         index  = 0;
    ID3D12Resource*              buffer = currentBackBuffer(&index);
    if (!buffer) {
        return;
    }
    D3D12_RESOURCE_DESC const desc = buffer->GetDesc();
    unsigned const            seen = logged.fetch_add(1, std::memory_order_relaxed);
    if (seen < 3) {
        // Only the logged frames pay for this: swapChainBuffers() takes a reference per
        // buffer, and every one of them has to go back (releaseComObject()).
        void*            chainBuffers[4]{};
        unsigned const   chainCount = probe::vv::swapChainBuffers(probe::vv::swapChain(), chainBuffers, 4);
        unsigned         onChain    = 0;
        for (unsigned i = 0; i < chainCount; ++i) {
            if (chainBuffers[i] == static_cast<void*>(buffer)) {
                ++onChain;
            }
            probe::vv::releaseComObject(chainBuffers[i]);
        }
        ND_DEBUG(
            "[fg] hudless: probe-only split={}/{} buffer=0x{:x} index={} size={}x{} fmt={} swapchain={}",
            split,
            listCount,
            static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(buffer)),
            index,
            desc.Width,
            desc.Height,
            static_cast<unsigned>(desc.Format),
            onChain ? 1u : 0u
        );
        logBarrierState(split, listCount, lists, buffer);
    }
    // Must go before prepare() returns: see currentBackBuffer().
    buffer->Release();
}

// The first three barrier-only frames log the same fields the copy line carries, so a
// later FullCopy round can be compared against them field by field. Gated like the other
// two modes: three lines per process, not three per second.
[[maybe_unused]] void logBarrierOnly(unsigned split, unsigned listCount, ID3D12Resource const* buffer, D3D12_RESOURCE_DESC const& desc) {
    static std::atomic<unsigned> logged{0};
    if (logged.fetch_add(1, std::memory_order_relaxed) >= 3) {
        return;
    }
    ND_DEBUG(
        "[fg] hudless: barrier-only split={}/{} buffer=0x{:x} size={}x{} fmt={}",
        split,
        listCount,
        static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(buffer)),
        desc.Width,
        desc.Height,
        static_cast<unsigned>(desc.Format)
    );
}

// EXP-0179: select only a resource whose own engine records prove the state at the
// pre-UI cut. The chain desc supplies size/format without GetBuffer; the transition
// must be RT -> PRESENT in a list that has not executed yet at the cut, so the source
// is still RT there. The engine owns the pointer for the whole submission; no reference
// is acquired and nothing is released by the caller.
[[maybe_unused]] ID3D12Resource* trackedDisplayResource(
    unsigned split,
    unsigned listCount,
    void* const* lists,
    unsigned* transitionIndex
) {
    auto* chain = static_cast<IDXGISwapChain*>(probe::vv::swapChain());
    DXGI_SWAP_CHAIN_DESC chainDesc{};
    if (!chain || FAILED(chain->GetDesc(&chainDesc))) {
        return nullptr;
    }
    probe::vv::BarrierTrace traces[64]{};
    unsigned const count = probe::vv::barrierTrace(nullptr, lists, listCount, traces, 64);
    for (unsigned i = 0; i < count; ++i) {
        auto const& trace = traces[i];
        if (!trace.resource || trace.before != D3D12_RESOURCE_STATE_RENDER_TARGET
            || trace.after != D3D12_RESOURCE_STATE_PRESENT) {
            continue;
        }
        auto* const resource = static_cast<ID3D12Resource*>(trace.resource);
        D3D12_RESOURCE_DESC const desc = resource->GetDesc();
        if (desc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D
            || desc.Width != chainDesc.BufferDesc.Width || desc.Height != chainDesc.BufferDesc.Height
            || desc.Format != chainDesc.BufferDesc.Format) {
            continue;
        }
        unsigned index = 0;
        for (; index < listCount && lists[index] != trace.list; ++index) {
        }
        if (index == listCount || index < split) {
            continue;
        }
        if (transitionIndex) {
            *transitionIndex = index;
        }
        return resource;
    }
    return nullptr;
}

} // namespace

void install() {
    std::lock_guard lock(gFrameMutex);
    g.active = false;
    gFrame   = Frame{};
}

void uninstall() {
    // Deliberately does not release the GPU objects: the process is going away and
    // freeing in-flight D3D12 resources is what hung the exit before (EXP-0121 for this
    // probe, EXP-0085 for the SR pipeline). Only the published reference goes.
    invalidate("uninstall");
}

unsigned prepare(void* queueVoid, unsigned listCount, void* const* lists, unsigned split) {
    static std::atomic<unsigned> entryLogs{0};
    if (listCount >= 4 && probe::vv::worldLoaded() && entryLogs.fetch_add(1, std::memory_order_relaxed) < 20) {
        ND_DEBUG(
            "[fg] prepare split={}/{} world={} queue=0x{:x}",
            split,
            listCount,
            probe::vv::worldLoaded() ? 1 : 0,
            static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(queueVoid))
        );
    }
    if (!queueVoid || listCount < 4) {
        return 0;
    }
    // A prepare that ends up returning 0 must not leave a previous frame's "active" copy
    // behind: submitted() would then signal and publish a list that was not submitted.
    g.active = false;
    // Outside a world the display write is a panorama/title frame, not a world image.
    if (!probe::vv::worldLoaded()) {
        return 0;
    }
    // EXP-0183: this split belongs to this exact submission. The old global could be
    // cleared by another thread's short submission between analysis and this call.
    if (!split || split >= listCount) {
        return 0;
    }
    static std::atomic<unsigned> gateLogged{0};
    if (gateLogged.fetch_add(1, std::memory_order_relaxed) < 3) {
        ND_DEBUG("[fg] hudless: gate split={}/{} mode={}", split, listCount, modeName());
    }
    if constexpr (kMode == Mode::ProbeOnly) {
        // The cut is recognised and the back buffer is read, then prepare() declines: no
        // list of ours reaches the queue, so the overlay forwards the submission unchanged.
        // EXP-0177 keeps this the *only* compiled path: no device objects, no list, no
        // transition, no CopyResource, no Signal - the barrier trace reads what the
        // engine recorded and nothing is submitted.
        recordProbeOnly(split, listCount, lists);
        return 0;
    } else {
        auto* const queue = static_cast<ID3D12CommandQueue*>(queueVoid);
        if (!ensureDevice(queue)) {
            return 0;
        }
        Slot* slot = takeSlot();
        if (!slot) {
            static std::atomic<unsigned> busyLogged{0};
            if (busyLogged.fetch_add(1, std::memory_order_relaxed) < 3) {
                ND_DEBUG("[fg] hudless: gate all slots busy");
            }
            return 0;
        }
        if constexpr (kMode == Mode::TrackedBarrierOnly || kMode == Mode::TrackedFullCopy) {
            unsigned transitionIndex = 0;
            ID3D12Resource* const resource = trackedDisplayResource(split, listCount, lists, &transitionIndex);
            static std::atomic<unsigned> logged{0};
            unsigned const logIndex = logged.fetch_add(1, std::memory_order_relaxed);
            if (!resource) {
                if (logIndex < 3) {
                    ND_DEBUG("[fg] hudless: tracked-copy split={}/{} none", split, listCount);
                }
                return 0;
            }
            bool recorded = false;
            if constexpr (kMode == Mode::TrackedBarrierOnly) {
                recorded = recordBarrierOnly(*slot, resource);
            } else {
                D3D12_RESOURCE_DESC const desc = resource->GetDesc();
                recorded = ensureTarget(*slot, static_cast<UINT>(desc.Width), desc.Height, copyFormat(desc.Format))
                        && record(*slot, resource);
            }
            if (!recorded) {
                static std::atomic<unsigned> failureLogged{0};
                if (failureLogged.fetch_add(1, std::memory_order_relaxed) < 3) {
                    ND_DEBUG(
                        "[fg] hudless: gate record failed split={}/{} resource=0x{:x}",
                        split,
                        listCount,
                        static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(resource))
                    );
                }
                return 0;
            }
            if (logIndex < 3) {
                if constexpr (kMode == Mode::TrackedBarrierOnly) {
                    ND_DEBUG(
                        "[fg] hudless: tracked-barrier split={}/{} resource=0x{:x} listIndex={} "
                        "before=0x{:x} after=0x{:x}",
                        split,
                        listCount,
                        static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(resource)),
                        transitionIndex,
                        static_cast<unsigned>(D3D12_RESOURCE_STATE_RENDER_TARGET),
                        static_cast<unsigned>(D3D12_RESOURCE_STATE_PRESENT)
                    );
                } else {
                    ND_DEBUG(
                        "[fg] hudless: tracked-copy split={}/{} resource=0x{:x} listIndex={} size={}x{} fmt={}",
                        split,
                        listCount,
                        static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(resource)),
                        transitionIndex,
                        slot->width,
                        slot->height,
                        static_cast<unsigned>(slot->format)
                    );
                }
            }
        } else {
            ID3D12Resource* buffer = currentBackBuffer();
            if (!buffer) {
                return 0;
            }
            D3D12_RESOURCE_DESC const desc     = buffer->GetDesc();
            bool                      recorded = false;
            if constexpr (kMode == Mode::BarrierOnly) {
                recorded = recordBarrierOnly(*slot, buffer);
            } else {
                recorded = ensureTarget(*slot, static_cast<UINT>(desc.Width), desc.Height, copyFormat(desc.Format))
                        && record(*slot, buffer);
            }
            if (!recorded) {
                // The back buffer reference must go before anything else: holding it makes
                // the engine's ResizeBuffers fail (EXP-0139/EXP-0078).
                buffer->Release();
                return 0;
            }
            if constexpr (kMode == Mode::BarrierOnly) {
                logBarrierOnly(split, listCount, buffer, desc);
            } else {
                static std::atomic<unsigned> logged{0};
                if (logged.fetch_add(1, std::memory_order_relaxed) < 3) {
                    ND_DEBUG(
                        "[fg] hudless: copy split={}/{} {}x{} fmt={}",
                        split,
                        listCount,
                        slot->width,
                        slot->height,
                        static_cast<unsigned>(slot->format)
                    );
                }
            }
            buffer->Release();
        }

        g.queue      = queue;
        g.activeSlot = static_cast<unsigned>(slot - gSlots);
        g.active     = true;
        return split;
    }
}

void* list() {
    return g.active ? gSlots[g.activeSlot].list : nullptr;
}

void submitted() {
    if (!g.active || !g.queue || !g.fence) {
        return;
    }
    Slot&      slot  = gSlots[g.activeSlot];
    UINT64 const value = ++g.fenceNext;
    if (FAILED(g.queue->Signal(g.fence, value))) {
        g.active = false;
        return;
    }
    slot.fenceValue = value;
    g.active        = false;

    // Only the copy modes publish. Barrier-only modes leave gFrame empty so fg::inputs
    // keeps skipping instead of describing a copy that never happened.
    if constexpr (kMode == Mode::FullCopy || kMode == Mode::TrackedFullCopy) {
        Frame published{};
        published.texture = slot.texture;
        published.serial  = ++g.serial;
        published.width   = slot.width;
        published.height  = slot.height;
        published.format  = static_cast<unsigned>(slot.format);
        published.state   = static_cast<unsigned>(slot.state);
        {
            std::lock_guard lock(gFrameMutex);
            gFrame = published;
        }
        static std::atomic<unsigned> logged{0};
        if (logged.fetch_add(1, std::memory_order_relaxed) < 3) {
            ND_DEBUG(
                "[fg] hudless: published serial={} size={}x{} fmt={}",
                published.serial,
                published.width,
                published.height,
                published.format
            );
        }
    }
}

bool frame(Frame* out) {
    if (!out) {
        return false;
    }
    std::lock_guard lock(gFrameMutex);
    if (!gFrame.texture || !gFrame.serial) {
        return false;
    }
    *out = gFrame;
    return true;
}

void invalidate(char const* why) {
    {
        std::lock_guard lock(gFrameMutex);
        gFrame = Frame{};
    }
    // A new epoch: every slot's target is re-allocated at the size/format the next frame
    // produces. Nothing is released here, so an in-flight texture is never freed under
    // the GPU; the slot's own fence decides when it may be replaced.
    g.generation.fetch_add(1, std::memory_order_relaxed);
    if (why) {
        ND_DEBUG("[fg] hudless: inputs invalidated ({})", why);
    }
}

} // namespace neurodragon::fg::hudless
