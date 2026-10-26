#include "neurodragon/vv/VvCapture.h"

#include "neurodragon/probe/VvCalibrate.h"
#include "neurodragon/probe/VvProbe.h"

#include <Windows.h>
#include <d3d12.h>
#include <dxgi1_4.h>

#include "neurodragon/Macro.h"

#include "neurodragon/utils/Utils.h"

#include <cstdio>
#include <cstring>
#include <cwchar>
#include <mutex>
#include <string>

namespace neurodragon::vv::capture {
namespace {

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

// Four recordings in flight is more than a 60 Hz frame needs: the fence of a slot
// is normally already passed by the next frame, and the extra slots only absorb a
// hitch. More slots would only cost system memory (each is a full display-res
// buffer).
constexpr unsigned kSlots = 4;

struct Slot {
    ID3D12Resource*                     readback{};
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT  footprint{};
    UINT64                              fenceValue{};
    bool                                armed{};   ///< recorded, fence not signalled yet
    bool                                pending{}; ///< on the GPU, waiting for the map
    unsigned                            split{};
    unsigned                            listCount{};
    bool                                fromTail{};
    unsigned                            width{};
    unsigned                            height{};
};

struct State {
    ID3D12Device* device{};
    ID3D12Fence*  fence{};
    UINT64        fenceValue{};
    Slot          slots[kSlots]{};
    int           current{-1};
    unsigned      sequence{};
    Stats         stats{};
};

std::mutex gMutex;
State      g;

void moveToPresent(ID3D12GraphicsCommandList* list, ID3D12Resource* resource, D3D12_RESOURCE_STATES before) {
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource   = resource;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barrier.Transition.StateBefore = before;
    barrier.Transition.StateAfter  = D3D12_RESOURCE_STATE_PRESENT;
    list->ResourceBarrier(1, &barrier);
}

bool ensureReadback(ID3D12Device* device, Slot& slot, D3D12_RESOURCE_DESC const& source, unsigned width, unsigned height) {
    if (slot.readback && slot.width == width && slot.height == height) {
        return true;
    }
    if (slot.readback) {
        slot.readback->Release();
        slot.readback = nullptr;
    }
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
    UINT                              rows    = 0;
    UINT64                            rowSize = 0;
    UINT64                            total   = 0;
    device->GetCopyableFootprints(&source, 0, 1, 0, &footprint, &rows, &rowSize, &total);
    if (!total) {
        return false;
    }
    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_READBACK;
    D3D12_RESOURCE_DESC buffer{};
    buffer.Dimension        = D3D12_RESOURCE_DIMENSION_BUFFER;
    buffer.Width            = total;
    buffer.Height           = 1;
    buffer.DepthOrArraySize = 1;
    buffer.MipLevels        = 1;
    buffer.Format           = DXGI_FORMAT_UNKNOWN;
    buffer.SampleDesc.Count = 1;
    buffer.Layout           = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    HRESULT const hr        = device->CreateCommittedResource(
        &heap,
        D3D12_HEAP_FLAG_NONE,
        &buffer,
        D3D12_RESOURCE_STATE_COPY_DEST,
        nullptr,
        IID_PPV_ARGS(&slot.readback)
    );
    if (FAILED(hr)) {
        g.stats.lastError = static_cast<int>(hr);
        utils::logger().warn("[vvc] readback {:#x} failed hr=0x{:08X}", width, height, static_cast<unsigned>(hr));
        return false;
    }
    slot.footprint = footprint;
    slot.width     = width;
    slot.height    = height;
    return true;
}

unsigned pendingCount() {
    unsigned count = 0;
    for (auto const& slot : g.slots) {
        if (slot.pending || slot.armed) {
            ++count;
        }
    }
    return count;
}

} // namespace

void start() {
    std::lock_guard lock(gMutex);
    g.stats.captured = 0;
    g.stats.skipped  = 0;
    g.stats.pending  = 0;
    g.stats.tail     = 0;
    g.stats.split    = 0;
    g.stats.listCount = 0;
    g.stats.lastError = 0;
    g.stats.lastFile[0] = 0;
    g.current        = -1;
    g.sequence       = 0;
    // Slots that are still on the GPU are left alone; their completion is picked up
    // by poll() and they go back into the ring on their own.
}

bool record(void* commandListVoid, void* backBufferVoid) {
    auto* const list = static_cast<ID3D12GraphicsCommandList*>(commandListVoid);
    auto* const back = static_cast<ID3D12Resource*>(backBufferVoid);
    auto* const device = static_cast<ID3D12Device*>(probe::vv::engineDevice());
    if (!list || !back || !device) {
        return false;
    }
    std::lock_guard lock(gMutex);
    if (!g.fence) {
        HRESULT const hr = device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&g.fence));
        if (FAILED(hr)) {
            g.stats.lastError = static_cast<int>(hr);
            utils::logger().warn("[vvc] CreateFence failed hr=0x{:08X}", static_cast<unsigned>(hr));
            return false;
        }
    }
    g.device = device;

    D3D12_RESOURCE_DESC const desc   = back->GetDesc();
    unsigned const            width  = static_cast<unsigned>(desc.Width);
    unsigned const            height = desc.Height;
    if (!width || !height || desc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D) {
        ++g.stats.skipped;
        return false;
    }
    // Belt and braces: 64 pictures are more than any sweep needs, and a diagnostic
    // that keeps writing 11 MB per frame is a disk-full bug, not a measurement.
    if (g.stats.captured >= 64) {
        ++g.stats.skipped;
        return false;
    }

    int slot = -1;
    for (unsigned i = 0; i < kSlots; ++i) {
        auto& candidate = g.slots[i];
        // `pending` means "recorded and not written to disk yet" - poll() clears it at
        // the next Present. Reusing such a slot here would silently lose the picture,
        // so only a slot that has already been saved (or never used) is taken.
        if (candidate.armed || candidate.pending) {
            continue;
        }
        slot = static_cast<int>(i);
        break;
    }
    if (slot < 0) {
        // Every slot is still on the GPU. One frame without a capture is better than
        // overwriting a picture that has not been read back yet.
        ++g.stats.skipped;
        return false;
    }
    auto& target = g.slots[slot];
    if (!ensureReadback(device, target, desc, width, height)) {
        ++g.stats.skipped;
        return false;
    }

    // PRESENT -> COPY_SOURCE -> copy -> PRESENT. The engine leaves the chain in
    // PRESENT between frames (the injection's clear experiment relies on the same
    // thing), and the suffix lists must find it exactly as they left it.
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource   = back;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
    barrier.Transition.StateAfter  = D3D12_RESOURCE_STATE_COPY_SOURCE;
    list->ResourceBarrier(1, &barrier);

    D3D12_TEXTURE_COPY_LOCATION dst{};
    dst.pResource       = target.readback;
    dst.Type            = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    dst.PlacedFootprint = target.footprint;
    D3D12_TEXTURE_COPY_LOCATION src{};
    src.pResource        = back;
    src.Type             = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    src.SubresourceIndex = 0;
    list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);

    moveToPresent(list, back, D3D12_RESOURCE_STATE_COPY_SOURCE);

    target.armed  = true;
    target.width  = width;
    target.height = height;
    g.current     = slot;
    g.stats.width  = width;
    g.stats.height = height;
    return true;
}

void onSubmitted(void* queueVoid, unsigned split, unsigned listCount, bool fromTail) {
    auto* const queue = static_cast<ID3D12CommandQueue*>(queueVoid);
    std::lock_guard lock(gMutex);
    int const       current = g.current;
    g.current               = -1;
    if (current < 0) {
        return;
    }
    auto& slot = g.slots[current];
    if (!slot.armed) {
        return;
    }
    slot.armed = false;
    if (!queue || !g.fence) {
        ++g.stats.skipped;
        return;
    }
    slot.split      = split;
    slot.listCount  = listCount;
    slot.fromTail   = fromTail;
    slot.fenceValue = ++g.fenceValue;
    if (FAILED(queue->Signal(g.fence, slot.fenceValue))) {
        ++g.stats.skipped;
        g.stats.lastError = E_FAIL;
        return;
    }
    slot.pending     = true;
    g.stats.tail     = fromTail ? (listCount - split + 1) : 0;
    g.stats.split    = split;
    g.stats.listCount = listCount;
    g.stats.pending  = pendingCount();
}

void poll() {
    std::lock_guard lock(gMutex);
    if (!g.fence) {
        return;
    }
    for (unsigned i = 0; i < kSlots; ++i) {
        auto& slot = g.slots[i];
        if (!slot.pending) {
            continue;
        }
        if (g.fence->GetCompletedValue() < slot.fenceValue) {
            continue;
        }
        slot.pending = false;
        if (!slot.readback) {
            ++g.stats.skipped;
            continue;
        }
        void* mapped = nullptr;
        if (FAILED(slot.readback->Map(0, nullptr, &mapped)) || !mapped) {
            ++g.stats.skipped;
            continue;
        }
        wchar_t name[80]{};
        unsigned const tail = slot.fromTail ? (slot.listCount - slot.split + 1) : 0;
        std::swprintf(
            name,
            std::size(name),
            L"vv_cut_%02u_k%u_of%u_tail%u_%ux%u.bmp",
            g.sequence++,
            slot.split,
            slot.listCount,
            tail,
            slot.width,
            slot.height
        );
        std::wstring const path = utils::applicationDirectory() + L"\\" + name;
        bool const ok = probe::vv::writeBmpRgba8(
            path.c_str(),
            static_cast<unsigned char const*>(mapped),
            slot.width,
            slot.height,
            slot.footprint.Footprint.RowPitch
        );
        slot.readback->Unmap(0, nullptr);
        if (ok) {
            ++g.stats.captured;
            std::swprintf(g.stats.lastFile, std::size(g.stats.lastFile), L"%s", name);
            utils::logger().info(
                "[vvc] cut {} tail={} split={}/{} {}x{}",
                narrow(name),
                tail,
                slot.split,
                slot.listCount,
                slot.width,
                slot.height
            );
        } else {
            ++g.stats.skipped;
            utils::logger().warn("[vvc] writing {} failed", narrow(name));
        }
    }
    g.stats.pending = pendingCount();
}

Stats stats() {
    std::lock_guard lock(gMutex);
    Stats copy     = g.stats;
    copy.pending   = pendingCount();
    return copy;
}

void release() {
    std::lock_guard lock(gMutex);
    for (auto& slot : g.slots) {
        if (slot.readback) {
            slot.readback->Release();
        }
        slot = Slot{};
    }
    if (g.fence) {
        g.fence->Release();
    }
    g.fence = nullptr;
    g.fenceValue = 0;
    g.current    = -1;
    g.stats      = Stats{};
}

} // namespace neurodragon::vv::capture
