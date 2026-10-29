#include "neurodragon/probe/VvInject.h"
#include "neurodragon/probe/VvProbe.h"
#include "neurodragon/vv/VvSr.h"

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

namespace neurodragon::probe::vv::inject {
namespace {

// Why the last attempt did nothing. Codes instead of strings because the reason is
// written from the render thread and read from the panel; the panel turns the code
// into text (stats()).

struct Injector {
    ID3D12Device*              device{};
    ID3D12CommandQueue*        queue{};
    ID3D12CommandAllocator*    allocator{};
    ID3D12GraphicsCommandList* list{};
    ID3D12Fence*               fence{};
    UINT64                     fenceValue{};
    IDXGISwapChain*            swapChain{};
    ID3D12Resource*            pending{}; ///< back buffer handed to the split submission
    std::atomic<bool>          pipelineReady{false};
};

Injector g;

std::atomic<int>      gMode{static_cast<int>(Mode::Off)};
std::atomic<int>      gAction{static_cast<int>(Action::DlssArg10)};
std::atomic<bool>     gUseArg10{false};
std::atomic<unsigned> gContentLogs{0};
std::atomic<unsigned> gLists{0};
std::atomic<unsigned> gInjected{0};
std::atomic<unsigned> gSkipped{0};
std::atomic<unsigned> gReason{static_cast<unsigned>(SkipReason::None)};
// Log throttles: a broken frame would otherwise write a line per frame forever, and
// the first few attempts are the only ones worth reading one by one.
std::atomic<unsigned> gSkips{0};
std::atomic<unsigned> gLastLoggedReason{static_cast<unsigned>(SkipReason::None) + 1};
std::atomic<bool>     gRecordingView{false};
std::atomic<unsigned> gSplit{0};

void setReason(SkipReason reason) {
    // The stage D draw-level suppress only skips TAAU after a frame that really
    // injected; a skip here makes the next frame restore TAAU by itself.
    probe::vv::noteInjectionResult(false);
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
    case SkipReason::RecordFailed:   text = "recording the injected work failed"; break;
    case SkipReason::NoBoundary:     text = "no arg10 writer in this submission; TAAU restored"; break;
    default: break;
    }
    // First three, then only when the reason changes: a frame that keeps failing for
    // the same cause is one fact, not one line per frame.
    unsigned const last = gLastLoggedReason.exchange(static_cast<unsigned>(reason) + 1, std::memory_order_relaxed);
    if (text && (n <= 3 || last != static_cast<unsigned>(reason) + 1)) {
        utils::logger().warn("[vv] inject skipped ({}): {}", n, text);
    }
}

void noteListCount(unsigned listCount) { gLists.store(listCount, std::memory_order_relaxed); }

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
    g.pipelineReady.store(true, std::memory_order_release);
    ND_DEBUG("[vv] inject pipeline ready");
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

// Closes the list and reports whether the frame did its work. Every path out of
// record() must go through here: EXP-0064 left the list in the recording state when a
// sub-record failed, which kept its allocator busy and made every later Reset fail -
// injection died for the rest of the session with "record failed", no device error.
bool finish(bool ok) {
    HRESULT const closed = g.list->Close();
    if (FAILED(closed)) {
        static std::atomic<unsigned> failures{0};
        unsigned const               n = failures.fetch_add(1, std::memory_order_relaxed) + 1;
        if (n <= 3 || n % 600 == 0) {
            utils::logger().warn(
                "[vv] inject: list Close failed hr=0x{:08X}",
                static_cast<unsigned>(closed)
            );
        }
        return false;
    }
    return ok;
}

bool record() {
    // A list left recording by an earlier failed frame has to be closed before its
    // allocator can be reset again; without this the first failure is permanent.
    g.list->Close();
    if (FAILED(g.allocator->Reset()) || FAILED(g.list->Reset(g.allocator, nullptr))) {
        return false;
    }
    if (gAction.load(std::memory_order_relaxed) != static_cast<int>(Action::DlssArg10)) {
        return finish(false);
    }
    // prep + DLSS evaluate, then a PS that writes the DLSS output into the engine's
    // arg10. No white point, no encode, no back-buffer copy.
    void* const target = probe::vv::arg10Target();
    if (!target || !neurodragon::vv::sr::recordArg10(g.list, target)) {
        return finish(false);
    }
    return finish(true);
}

} // namespace

void setMode(Mode mode) {
    int const value    = static_cast<int>(mode);
    int const previous = gMode.exchange(value, std::memory_order_relaxed);
    if (value == static_cast<int>(Mode::Off)) {
        // Stopping injection while the product stays armed is exactly the freeze: the
        // draw gate would keep skipping TAAU over a picture nothing writes. One choke
        // point - whoever turns injection off turns the product off.
        probe::vv::setProductArmed(false);
    }
    if (previous == value) {
        return;
    }
    gContentLogs.store(0, std::memory_order_relaxed);
}

Mode mode() { return static_cast<Mode>(gMode.load(std::memory_order_relaxed)); }

void setAction(Action action) { gAction.store(static_cast<int>(action), std::memory_order_relaxed); }
Action action() { return static_cast<Action>(gAction.load(std::memory_order_relaxed)); }

void setUseArg10(bool use) {
    gUseArg10.store(use, std::memory_order_relaxed);
    if (use) {
        // The panel's injected/skipped numbers must describe this arm, not the
        // previous one (EXP-0059's cumulative skips hid the world-frame rate).
        gInjected.store(0, std::memory_order_relaxed);
        gSkipped.store(0, std::memory_order_relaxed);
    }
}
bool useArg10() { return gUseArg10.load(std::memory_order_relaxed); }

void setDevice(void* device) { g.device = static_cast<ID3D12Device*>(device); }

void noteSwapChain(void* swapChain) { g.swapChain = static_cast<IDXGISwapChain*>(swapChain); }

unsigned prepare(void* queueVoid, unsigned listCount, void* const* lists) {
    // ---- is this an engine frame at all? -------------------------------------
    // The decision below may only touch the inject/suppress state for submissions that
    // could be a frame; the overlay's own D3D11On12 list and any async/copy queue are
    // forwarded untouched (docs/specs/vv.md §3).
    if (!queueVoid || listCount < 8) {
        return 0;
    }
    auto* const queue = static_cast<ID3D12CommandQueue*>(queueVoid);
    // The queue hook sits on the shared vtable, so it sees *every* command queue in the
    // process, compute and copy included. A direct command list cannot be submitted to
    // a compute queue at all, so only the engine's direct queue is ever used.
    if (queue->GetDesc().Type != D3D12_COMMAND_LIST_TYPE_DIRECT) {
        return 0;
    }
    if (g.queue && g.queue != queue) {
        // A frame-graph rebuild can move the engine to a new direct queue (the 100%
        // native switch does this). Refusing silently used to leave the draw suppress
        // armed while nothing wrote arg10: the picture froze on the last injected
        // frame (EXP-0092). Wait for the old queue's last injection to finish, then
        // rebind. While it is still pending, report a failed injection so the draw
        // suppress releases TAAU next frame instead of freezing.
        if (g.fence && g.fence->GetCompletedValue() < g.fenceValue) {
            setReason(SkipReason::FenceBusy);
            return 0;
        }
        ND_DEBUG("[vv] inject: queue changed; rebinding");
        g.queue = queue;
    }
    noteListCount(listCount);

    // ---- from here on: one engine frame, so write the latch exactly once ------
    // The draw gate reads "did the previous engine frame write arg10". A frame that
    // decides *not* to inject has to say so too, otherwise the last `true` stays and
    // the engine's TAAU draw keeps being skipped over a picture nobody updates (the
    // 2026-10-29 freeze: injection stopped, suppress stayed). Every exit below either
    // calls setReason (which reports false) or noteInjectionResult(false) here.
    if (gMode.load(std::memory_order_relaxed) == static_cast<int>(Mode::Off)) {
        noteInjectionResult(false);
        return 0;
    }
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
    if (!gUseArg10.load(std::memory_order_relaxed) || !lists) {
        // The arg10 writer is the only cut point; without it there is nothing to
        // inject into.
        setReason(SkipReason::NoBoundary);
        return 0;
    }
    unsigned const split = probe::vv::arg10WriterSplit();
    if (split == 0 || split > listCount) {
        // Do not record/evaluate DLSS on a submission that will not be injected:
        // advancing the feature's history with a UI/other frame shows up as a
        // one-frame corruption on the next world frame (EXP-0060's black flash).
        if (probe::vv::arg10FreshSubmission()) {
            probe::vv::noteArg10Gap();
        }
        static std::atomic<unsigned> misses{0};
        unsigned const               n = misses.fetch_add(1, std::memory_order_relaxed);
        if (n < 4 || n % 600 == 0) {
            utils::logger().warn(
                "[vv] inject: arg10 writer not found (split={}/{})",
                split,
                listCount
            );
        }
        setReason(SkipReason::NoBoundary);
        return 0;
    }
    ID3D12Resource* buffer = currentBackBuffer();
    if (!buffer) {
        setReason(SkipReason::NoBackBuffer);
        return 0;
    }
    if (!record()) {
        buffer->Release();
        setReason(SkipReason::RecordFailed);
        return 0;
    }
    g.queue   = queue;
    g.pending = buffer;

    gSplit.store(split, std::memory_order_relaxed);
    gReason.store(static_cast<unsigned>(SkipReason::None), std::memory_order_relaxed);
    gInjected.fetch_add(1, std::memory_order_relaxed);
    probe::vv::noteInjectionResult(true);
    unsigned const n = gContentLogs.fetch_add(1, std::memory_order_relaxed);
    if (n < 4) {
        ND_DEBUG("[vv] inject: arg10 boundary split={}/{}", split, listCount);
    }
    return split;
}

void* list() { return g.list; }

bool isOwnList(void* list) { return list && list == g.list; }

bool recordingOwnView() { return gRecordingView.load(std::memory_order_relaxed); }
void setRecordingOwnView(bool recording) { gRecordingView.store(recording, std::memory_order_relaxed); }

void submitted() {
    if (g.queue && g.fence) {
        g.queue->Signal(g.fence, ++g.fenceValue);
    }
    // The reference has to go now: ResizeBuffers fails while any reference to a back
    // buffer is outstanding, and that failure ends in abort() (docs/specs/vv.md §9).
    if (g.pending) {
        g.pending->Release();
        g.pending = nullptr;
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
    if (g.fence) {
        g.fence->Release();
        g.fence = nullptr;
    }
    if (g.pending) {
        g.pending->Release();
        g.pending = nullptr;
    }
    g.pipelineReady.store(false, std::memory_order_release);
    g.device        = nullptr;
    g.queue         = nullptr;
    g.swapChain     = nullptr;
    gMode.store(static_cast<int>(Mode::Off), std::memory_order_relaxed);
}

Stats stats() {
    Stats out{};
    out.listsPerSubmit = gLists.load(std::memory_order_relaxed);
    out.split          = gSplit.load(std::memory_order_relaxed);
    out.injected       = gInjected.load(std::memory_order_relaxed);
    out.skipped        = gSkipped.load(std::memory_order_relaxed);
    out.ready          = g.pipelineReady.load(std::memory_order_acquire);
    out.reason         = static_cast<SkipReason>(gReason.load(std::memory_order_relaxed));
    return out;
}

} // namespace neurodragon::probe::vv::inject
