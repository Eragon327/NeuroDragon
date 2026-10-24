#include "neurodragon/probe/VvInject.h"

#include <Windows.h>
#include <d3d12.h>
#include <dxgi1_4.h>

// <d3d12.h> is in place, so the engine headers may follow - with Macro.h between them,
// which is what stops mc/_HeaderOutputPredefine.h from re-declaring three empty D3D12
// shells on top of the real ones (docs/decisions/0004 §2).
#include "neurodragon/Macro.h"

#include "neurodragon/utils/Utils.h"

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace neurodragon::probe::vv::inject {
namespace {

// Why the last attempt did nothing. Codes instead of strings because the reason is
// written from the render thread and read from the panel; the panel turns the code
// into text (stats()).

struct Injector {
    ID3D12Device*               device{};
    ID3D12CommandQueue*         queue{};
    ID3D12CommandAllocator*     allocator{};
    ID3D12GraphicsCommandList*  list{};
    ID3D12DescriptorHeap*       heap{};
    D3D12_CPU_DESCRIPTOR_HANDLE rtv{};
    ID3D12Fence*                fence{};
    UINT64                      fenceValue{};
    IDXGISwapChain*             swapChain{};
    ID3D12Resource*             pending{}; ///< back buffer handed to the split submission
    std::atomic<bool>           pipelineReady{false};
};

Injector g;

std::atomic<int>      gMode{static_cast<int>(Mode::Off)};
std::atomic<unsigned> gK{1};
std::atomic<unsigned> gHold{20};
std::atomic<unsigned> gHoldLeft{20};
std::atomic<unsigned> gLists{0};
std::atomic<unsigned> gInjected{0};
std::atomic<unsigned> gSkipped{0};
std::atomic<unsigned> gSweeps{0};
std::atomic<unsigned> gReason{static_cast<unsigned>(SkipReason::None)};
// Log throttles: the first few attempts are never interesting one by one, and a
// broken frame would otherwise write a line per frame forever.
std::atomic<unsigned> gSkips{0};
std::atomic<unsigned> gLastLoggedInjected{0};
std::atomic<unsigned> gKLogCount{0};
std::atomic<bool>     gRecordingView{false};
std::atomic<bool>     gFromTail{false};
std::atomic<unsigned> gSplit{0};
// The engine's per-frame list count is not constant (69 in the main-menu dressing
// room, 48-51 in a world, and single frames at 74/81/107), so the panel needs the
// range, not just the last value: it is the difference between "the boundary moved"
// and "the frame changed shape".
std::atomic<unsigned> gMinLists{0};
std::atomic<unsigned> gMaxLists{0};

void setReason(SkipReason reason) {
    gReason.store(static_cast<unsigned>(reason), std::memory_order_relaxed);
    gSkipped.fetch_add(1, std::memory_order_relaxed);
    unsigned const n    = gSkips.fetch_add(1, std::memory_order_relaxed) + 1;
    char const*    text = nullptr;
    switch (reason) {
    case SkipReason::NoDevice:       text = "no engine device yet"; break;
    case SkipReason::NoSwapChain:    text = "no swap chain yet"; break;
    case SkipReason::FenceBusy:      text = "the previous injection is still on the GPU"; break;
    case SkipReason::NoBackBuffer:   text = "GetBuffer failed"; break;
    case SkipReason::PipelineFailed: text = "could not build the injection pipeline"; break;
    case SkipReason::RecordFailed:   text = "recording the clear failed"; break;
    default: break;
    }
    if (text && (n <= 3 || n % 120 == 0)) {
        utils::logger().warn("[vv] inject skipped ({}): {}", n, text);
    }
}

void noteListCount(unsigned listCount) {
    gLists.store(listCount, std::memory_order_relaxed);
    unsigned previous = gMinLists.load(std::memory_order_relaxed);
    while ((previous == 0 || listCount < previous)
           && !gMinLists.compare_exchange_weak(
               previous,
               listCount,
               std::memory_order_relaxed,
               std::memory_order_relaxed
           )) {
    }
    previous = gMaxLists.load(std::memory_order_relaxed);
    while (listCount > previous
           && !gMaxLists.compare_exchange_weak(
               previous,
               listCount,
               std::memory_order_relaxed,
               std::memory_order_relaxed
           )) {
    }
}

unsigned clampK(unsigned listCount, unsigned k) {
    if (k < 1) {
        k = 1;
    }
    if (k > listCount) {
        k = listCount;
    }
    return k;
}

bool buildPipeline() {
    if (!g.device) {
        return false;
    }
    if (FAILED(g.device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&g.allocator)))
        || FAILED(g.device->CreateCommandList(
            0,
            D3D12_COMMAND_LIST_TYPE_DIRECT,
            g.allocator,
            nullptr,
            IID_PPV_ARGS(&g.list)
        ))
        || FAILED(g.device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&g.fence)))) {
        return false;
    }
    // The list is handed to us in the recording state; a fresh list that has never
    // been closed cannot be Reset on every platform, so close it once here.
    if (FAILED(g.list->Close())) {
        return false;
    }
    D3D12_DESCRIPTOR_HEAP_DESC heapDesc{};
    heapDesc.Type           = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    heapDesc.NumDescriptors = 1;
    heapDesc.Flags          = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
    if (FAILED(g.device->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(&g.heap)))) {
        return false;
    }
    g.rtv           = g.heap->GetCPUDescriptorHandleForHeapStart();
    g.pipelineReady.store(true, std::memory_order_release);
    utils::logger().info(
        "[vv] inject pipeline ready device={:#x} allocator={:#x} list={:#x} heap={:#x}",
        reinterpret_cast<std::uintptr_t>(g.device),
        reinterpret_cast<std::uintptr_t>(g.allocator),
        reinterpret_cast<std::uintptr_t>(g.list),
        reinterpret_cast<std::uintptr_t>(g.heap)
    );
    return true;
}

// The buffer the engine is drawing into right now. A flip-model chain rotates its
// buffers, so GetBuffer(0) is not "the one on screen" - GetCurrentBackBufferIndex is.
// The reference returned here is owned by the caller and released after submission.
ID3D12Resource* currentBackBuffer() {
    UINT index = 0;
    IDXGISwapChain3* chain3 = nullptr;
    if (SUCCEEDED(g.swapChain->QueryInterface(IID_PPV_ARGS(&chain3))) && chain3) {
        index = chain3->GetCurrentBackBufferIndex();
        chain3->Release();
    }
    ID3D12Resource* buffer = nullptr;
    if (FAILED(g.swapChain->GetBuffer(index, IID_PPV_ARGS(&buffer)))) {
        return nullptr;
    }
    return buffer;
}

// A view description is required when the resource is typeless: a swap chain may be
// created as R8G8B8A8_TYPELESS and then viewed as UNORM or UNORM_SRGB, and creating a
// view with a typeless format is a validation error (the runtime rejects it).
void makeRtvDesc(DXGI_FORMAT format, D3D12_RENDER_TARGET_VIEW_DESC& desc, bool& useDesc) {
    useDesc = true;
    desc    = {};
    desc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
    switch (format) {
    case DXGI_FORMAT_R8G8B8A8_TYPELESS: desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB; break;
    case DXGI_FORMAT_B8G8R8A8_TYPELESS: desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM_SRGB; break;
    case DXGI_FORMAT_R16G16B16A16_TYPELESS: desc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT; break;
    case DXGI_FORMAT_R10G10B10A2_TYPELESS: desc.Format = DXGI_FORMAT_R10G10B10A2_UNORM; break;
    default: useDesc = false; break;
    }
}

bool record(ID3D12Resource* buffer) {
    if (FAILED(g.allocator->Reset()) || FAILED(g.list->Reset(g.allocator, nullptr))) {
        return false;
    }
    D3D12_RENDER_TARGET_VIEW_DESC  desc{};
    bool                           useDesc = false;
    makeRtvDesc(buffer->GetDesc().Format, desc, useDesc);
    // The probe hooks CreateRenderTargetView on this same device to inventory the frame
    // graph; a back-buffer view is not part of that graph, so the flag keeps it out.
    gRecordingView.store(true, std::memory_order_relaxed);
    g.device->CreateRenderTargetView(buffer, useDesc ? &desc : nullptr, g.rtv);
    gRecordingView.store(false, std::memory_order_relaxed);

    // PRESENT -> RENDER_TARGET -> clear -> PRESENT. The clear is the whole point of
    // the experiment, so the state transitions are kept exactly as the runtime
    // demands: the engine leaves the buffer in PRESENT after presenting it, and that
    // is the state we have to leave behind again or the next frame's presentation
    // fails.
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource   = buffer;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
    barrier.Transition.StateAfter  = D3D12_RESOURCE_STATE_RENDER_TARGET;
    g.list->ResourceBarrier(1, &barrier);

    float const magenta[4] = {1.0f, 0.0f, 1.0f, 1.0f};
    g.list->ClearRenderTargetView(g.rtv, magenta, 0, nullptr);

    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
    barrier.Transition.StateAfter  = D3D12_RESOURCE_STATE_PRESENT;
    g.list->ResourceBarrier(1, &barrier);

    return SUCCEEDED(g.list->Close());
}

} // namespace

void setMode(Mode mode) {
    int const value = static_cast<int>(mode);
    int const previous = gMode.exchange(value, std::memory_order_relaxed);
    if (previous == value) {
        return;
    }
    // A fresh Sweep always starts at the first boundary and counts from zero: the
    // panel's numbers must describe the pass the user just started, not a stale one.
    gK.store(1, std::memory_order_relaxed);
    gHoldLeft.store(gHold.load(std::memory_order_relaxed), std::memory_order_relaxed);
    gSweeps.store(0, std::memory_order_relaxed);
    gKLogCount.store(0, std::memory_order_relaxed);
    gMinLists.store(0, std::memory_order_relaxed);
    gMaxLists.store(0, std::memory_order_relaxed);
}

Mode mode() { return static_cast<Mode>(gMode.load(std::memory_order_relaxed)); }

void setK(unsigned k) { gK.store(k < 1 ? 1 : k, std::memory_order_relaxed); }

void setFromTail(bool fromTail) {
    if (gFromTail.exchange(fromTail, std::memory_order_relaxed) != fromTail) {
        gK.store(1, std::memory_order_relaxed);
        gHoldLeft.store(gHold.load(std::memory_order_relaxed), std::memory_order_relaxed);
        gMinLists.store(0, std::memory_order_relaxed);
        gMaxLists.store(0, std::memory_order_relaxed);
    }
}

bool fromTail() { return gFromTail.load(std::memory_order_relaxed); }

void setHoldFrames(unsigned frames) {
    unsigned const value = frames < 1 ? 1 : (frames > 240 ? 240 : frames);
    gHold.store(value, std::memory_order_relaxed);
    gHoldLeft.store(value, std::memory_order_relaxed);
}

void setDevice(void* device) { g.device = static_cast<ID3D12Device*>(device); }

void noteSwapChain(void* swapChain) { g.swapChain = static_cast<IDXGISwapChain*>(swapChain); }

unsigned prepare(void* queueVoid, unsigned listCount) {
    if (gMode.load(std::memory_order_relaxed) == static_cast<int>(Mode::Off)) {
        return 0;
    }
    // The overlay's own D3D11On12 submission is one list; only the engine's frame is
    // big enough to be worth cutting (docs/specs/vv.md §3).
    if (!queueVoid || listCount < 8) {
        return 0;
    }
    auto* const queue = static_cast<ID3D12CommandQueue*>(queueVoid);
    // The queue hook sits on the shared vtable, so it sees *every* command queue in the
    // process, compute and copy included. A direct command list cannot be submitted to
    // a compute queue at all, and clearing the back buffer from a second queue would
    // race the engine's own writes, so only the engine's direct queue is ever used.
    if (queue->GetDesc().Type != D3D12_COMMAND_LIST_TYPE_DIRECT) {
        return 0;
    }
    if (g.queue && g.queue != queue) {
        return 0;
    }
    noteListCount(listCount);
    if (!g.device) {
        setReason(SkipReason::NoDevice);
        return 0;
    }
    if (!g.swapChain) {
        setReason(SkipReason::NoSwapChain);
        return 0;
    }
    if (!g.pipelineReady.load(std::memory_order_acquire) && !buildPipeline()) {
        setReason(SkipReason::PipelineFailed);
        return 0;
    }
    // An allocator may not be reset while the GPU is still executing a list recorded
    // from it. One frame of delay is invisible here and far safer than waiting.
    if (g.fence && g.fence->GetCompletedValue() < g.fenceValue) {
        setReason(SkipReason::FenceBusy);
        return 0;
    }
    ID3D12Resource* buffer = currentBackBuffer();
    if (!buffer) {
        setReason(SkipReason::NoBackBuffer);
        return 0;
    }
    if (!record(buffer)) {
        buffer->Release();
        setReason(SkipReason::RecordFailed);
        return 0;
    }
    g.queue   = static_cast<ID3D12CommandQueue*>(queueVoid);
    g.pending = buffer;

    unsigned const setting = gK.load(std::memory_order_relaxed);
    unsigned       k       = 0;
    if (gFromTail.load(std::memory_order_relaxed)) {
        // k = 1 means "after the whole frame", k = 2 "before the last list", ...
        unsigned back = setting < 1 ? 1 : setting;
        if (back > listCount) {
            back = listCount;
        }
        k = listCount - back + 1;
        if (gMode.load(std::memory_order_relaxed) == static_cast<int>(Mode::Fixed)) {
            gK.store(back, std::memory_order_relaxed);
        }
    } else {
        k = clampK(listCount, setting);
        if (gMode.load(std::memory_order_relaxed) == static_cast<int>(Mode::Fixed)) {
            // Store the clamped value back so the panel's "boundary now" is the boundary
            // that was actually used, not the slider position that asked for it.
            gK.store(k, std::memory_order_relaxed);
        }
    }
    gSplit.store(k, std::memory_order_relaxed);
    gReason.store(static_cast<unsigned>(SkipReason::None), std::memory_order_relaxed);
    gInjected.fetch_add(1, std::memory_order_relaxed);
    return k;
}

void* list() { return g.list; }

bool isOwnList(void* list) { return list && list == g.list; }

bool recordingOwnView() { return gRecordingView.load(std::memory_order_relaxed); }

void submitted(unsigned splitAt, unsigned listCount) {
    if (g.queue && g.fence) {
        g.queue->Signal(g.fence, ++g.fenceValue);
    }
    // The reference has to go now: ResizeBuffers fails while any reference to a back
    // buffer is outstanding, and that failure ends in abort() (docs/specs/vv.md §9).
    if (g.pending) {
        g.pending->Release();
        g.pending = nullptr;
    }

    unsigned const mode = static_cast<unsigned>(gMode.load(std::memory_order_relaxed));
    if (mode == static_cast<unsigned>(Mode::Fixed)) {
        unsigned const n = gInjected.load(std::memory_order_relaxed);
        if (n <= 3 || n % 120 == 0) {
            utils::logger().info(
                "[vv] inject fixed k={}/{} injected={} skipped={}",
                splitAt,
                listCount,
                n,
                gSkipped.load(std::memory_order_relaxed)
            );
        }
        return;
    }

    // Sweep: hold each boundary for `hold` frames. A whole pass then covers the frame
    // in list order, which is what makes "the screen went magenta from here on" a
    // readable answer without touching the log between boundaries.
    unsigned left = gHoldLeft.load(std::memory_order_relaxed);
    if (left > 1) {
        gHoldLeft.store(left - 1, std::memory_order_relaxed);
        return;
    }
    gHoldLeft.store(gHold.load(std::memory_order_relaxed), std::memory_order_relaxed);
    bool     wrapped = false;
    unsigned next    = 0;
    if (gFromTail.load(std::memory_order_relaxed)) {
        next = gK.load(std::memory_order_relaxed) + 1;
        if (next > listCount) {
            next    = 1;
            wrapped = true;
        }
    } else {
        next = splitAt + 1;
        if (next > listCount) {
            next    = 1;
            wrapped = true;
        }
    }
    if (wrapped) {
        unsigned const pass = gSweeps.fetch_add(1, std::memory_order_relaxed) + 1;
        utils::logger().info(
            "[vv] inject sweep pass {} done lists={} injected={} skipped={}",
            pass,
            listCount,
            gInjected.load(std::memory_order_relaxed),
            gSkipped.load(std::memory_order_relaxed)
        );
    }
    gK.store(next, std::memory_order_relaxed);
    unsigned const logged = gKLogCount.fetch_add(1, std::memory_order_relaxed);
    if (logged < 512) {
        utils::logger().info(
            "[vv] inject k={}/{} split={} tail={}",
            next,
            listCount,
            gSplit.load(std::memory_order_relaxed),
            gFromTail.load(std::memory_order_relaxed) ? "yes" : "no"
        );
    }
}

void release() {
    if (g.list) {
        g.list->Release();
        g.list = nullptr;
    }
    if (g.allocator) {
        g.allocator->Release();
        g.allocator = nullptr;
    }
    if (g.heap) {
        g.heap->Release();
        g.heap = nullptr;
    }
    if (g.fence) {
        g.fence->Release();
        g.fence = nullptr;
    }
    if (g.pending) {
        g.pending->Release();
        g.pending = nullptr;
    }
    g.rtv           = {};
    g.pipelineReady.store(false, std::memory_order_release);
    g.device        = nullptr;
    g.queue         = nullptr;
    g.swapChain     = nullptr;
    gMode.store(static_cast<int>(Mode::Off), std::memory_order_relaxed);
}

Stats stats() {
    Stats out{};
    out.listsPerSubmit = gLists.load(std::memory_order_relaxed);
    out.minLists       = gMinLists.load(std::memory_order_relaxed);
    out.maxLists       = gMaxLists.load(std::memory_order_relaxed);
    out.k              = gK.load(std::memory_order_relaxed);
    out.split          = gSplit.load(std::memory_order_relaxed);
    out.injected       = gInjected.load(std::memory_order_relaxed);
    out.skipped        = gSkipped.load(std::memory_order_relaxed);
    out.sweeps         = gSweeps.load(std::memory_order_relaxed);
    out.ready          = g.pipelineReady.load(std::memory_order_acquire);
    out.fromTail       = gFromTail.load(std::memory_order_relaxed);
    out.reason         = static_cast<SkipReason>(gReason.load(std::memory_order_relaxed));
    return out;
}

} // namespace neurodragon::probe::vv::inject
