#include "neurodragon/fg/HudlessProbe.h"

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
DXGI_FORMAT copyFormat(DXGI_FORMAT format) {
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

bool ensureDevice(ID3D12CommandQueue* queue) {
    if (!queue || queue->GetDesc().Type != D3D12_COMMAND_LIST_TYPE_DIRECT) {
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
        // puts every slot in the state record() expects.
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
    ND_DEBUG("[fg] hudless: copy path ready ({} slots)", kSlots);
    return true;
}

bool ensureTarget(Slot& slot, UINT width, UINT height, DXGI_FORMAT format) {
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

ID3D12Resource* currentBackBuffer() {
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
    ID3D12Resource* buffer = nullptr;
    if (FAILED(chain->GetBuffer(index, IID_PPV_ARGS(&buffer)))) {
        return nullptr;
    }
    return buffer;
}

// One list: back buffer -> COPY_SOURCE, full copy, back buffer back to
// RENDER_TARGET (the UI lists that follow still render into it), copy target ends in
// ALL_SHADER_RESOURCE - the state the FG tag declares for it.
bool record(Slot& slot, ID3D12Resource* backBuffer) {
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

unsigned prepare(void* queueVoid, unsigned listCount, void* const* lists) {
    (void)lists;
    auto* const queue = static_cast<ID3D12CommandQueue*>(queueVoid);
    if (!queue || listCount < 4) {
        return 0;
    }
    // A prepare that ends up returning 0 must not leave a previous frame's "active" copy
    // behind: submitted() would then signal and publish a list that was not submitted.
    g.active = false;
    // Outside a world the display write is a panorama/title frame, not a world image.
    if (!probe::vv::worldLoaded()) {
        return 0;
    }
    unsigned const split = probe::vv::preUiSplit();
    if (!split || split >= listCount) {
        return 0;
    }
    if (!ensureDevice(queue)) {
        return 0;
    }

    Slot* slot = nullptr;
    for (unsigned i = 0; i < kSlots; ++i) {
        unsigned const index = (g.nextSlot + i) % kSlots;
        Slot&          candidate = gSlots[index];
        if (candidate.fenceValue == 0 || g.fence->GetCompletedValue() >= candidate.fenceValue) {
            slot      = &candidate;
            g.nextSlot = (index + 1) % kSlots;
            break;
        }
    }
    if (!slot) {
        ++g.skips;
        return 0;
    }

    ID3D12Resource* buffer = currentBackBuffer();
    if (!buffer) {
        return 0;
    }
    D3D12_RESOURCE_DESC const desc = buffer->GetDesc();
    bool const ready = ensureTarget(
        *slot,
        static_cast<UINT>(desc.Width),
        desc.Height,
        copyFormat(desc.Format)
    );
    if (!ready || !record(*slot, buffer)) {
        // The back buffer reference must go before anything else: holding it makes the
        // engine's ResizeBuffers fail (EXP-0139/EXP-0078).
        buffer->Release();
        return 0;
    }
    buffer->Release();

    g.queue      = queue;
    g.activeSlot = static_cast<unsigned>(slot - gSlots);
    g.active     = true;
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
    return split;
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

    Frame published{};
    published.texture = slot.texture;
    published.serial  = ++g.serial;
    published.width   = slot.width;
    published.height  = slot.height;
    published.format  = static_cast<unsigned>(slot.format);
    published.state   = static_cast<unsigned>(slot.state);
    std::lock_guard lock(gFrameMutex);
    gFrame = published;
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
