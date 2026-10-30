#include "neurodragon/Config.h"
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
#include <cstdio>
#include <cstdlib>

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
    unsigned                   busySubmits{}; ///< consecutive submissions that found the fence unfinished
    unsigned                   rebuilds{};    ///< stall recoveries in this arm (capped)
    HRESULT                    lastSignal{S_OK};
    IDXGISwapChain*            swapChain{};
    ID3D12Resource*            pending{}; ///< back buffer handed to the split submission
    std::atomic<bool>          pipelineReady{false};
};

// A pending fence is normal for a submission or two - the GPU is still running the
// injection. Hundreds in a row are a lost signal or a dead queue, not latency:
// 120 submissions is about 2 s at 60 fps and 8 s at 15 fps.
constexpr unsigned kStallSubmits = 120;
// A stall that keeps coming back is not something a rebuild fixes; after this many
// the product turns itself off instead of leaking an allocator per retry.
constexpr unsigned kMaxRebuilds = 8;

Injector g;

std::atomic<int>      gMode{static_cast<int>(Mode::Off)};
std::atomic<int>      gAction{static_cast<int>(Action::DlssArg10)};
std::atomic<bool>     gUseArg10{false};
std::atomic<unsigned> gContentLogs{0};
std::atomic<unsigned> gLists{0};
std::atomic<unsigned> gInjected{0};
std::atomic<unsigned> gSkipped{0};
std::atomic<unsigned> gReason{static_cast<unsigned>(SkipReason::None)};
std::atomic<bool>     gRebuild{false};   ///< re-arm asked for fresh objects
std::atomic<bool>     gRecovered{false}; ///< report the first injection after a rebuild
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

// Drops the objects a pending injection may still be using. The allocator is retired,
// never reset: the GPU can still be executing the list recorded from it, and resetting
// it from under the GPU is what killed injection for a whole session in EXP-0064.
void retire() {
    if (g.list) {
        g.list->Close();
        g.list->Release();
        g.list = nullptr;
    }
    if (g.allocator) {
        g.allocator->Release();
        g.allocator = nullptr;
    }
    if (g.fence) {
        // The queue keeps its own reference until a pending Signal is processed, so
        // dropping ours cannot strand the signal.
        g.fence->Release();
        g.fence = nullptr;
    }
    if (g.pending) {
        g.pending->Release();
        g.pending = nullptr;
    }
    g.fenceValue  = 0;
    g.busySubmits = 0;
    g.pipelineReady.store(false, std::memory_order_release);
}

// Why the fence cannot be trusted, in one line; retires the old objects and builds a
// fresh set. Past kMaxRebuilds the product turns itself off through its own config
// switch, so the panel and syncConfig() agree with the injector instead of showing a
// switch that does nothing (EXP-0118).
void restart(char const* why) {
    unsigned const n = ++g.rebuilds;
    if (n > kMaxRebuilds) {
        utils::logger().error(
            "[vv] inject: {} after {} rebuilds; turning VV super resolution off (restart to try again)",
            why,
            n - 1
        );
        config::get().sr.vvEnabled = false;
        setMode(Mode::Off);
        retire();
        return;
    }
    utils::logger().warn("[vv] inject: {}; rebuilding the injection (rebuild #{})", why, n);
    retire();
    gRecovered.store(true, std::memory_order_relaxed);
    if (!buildPipeline()) {
        utils::logger().error("[vv] inject: rebuilding the injection failed");
    }
}

// True when the previous injection has finished (or there is none). A stall is
// recovered here instead of being reported forever: a Signal that never reaches the
// queue used to leave fenceValue ahead of the fence for the rest of the process, which
// silently killed every later injection - and in VV that also starves NR's create
// gate, because the gate only runs inside our evaluate (EXP-0118).
bool fenceIdle() {
    if (!g.fence) {
        return true;
    }
    UINT64 const done = g.fence->GetCompletedValue();
    if (done >= g.fenceValue) {
        g.busySubmits = 0;
        return true;
    }
    if (++g.busySubmits < kStallSubmits) {
        return false;
    }
    char text[192]{};
    std::snprintf(
        text,
        sizeof(text),
        "the fence stayed at %llu of %llu for %u submissions (signal=0x%08X)",
        static_cast<unsigned long long>(done),
        static_cast<unsigned long long>(g.fenceValue),
        g.busySubmits,
        static_cast<unsigned>(g.lastSignal)
    );
    restart(text);
    return false;
}

#ifdef TEST
// EXP-0118: the deterministic local reproduction. NEURODRAGON_SIGNAL_FAULT=<n> makes
// the next n injections advance the fence value without calling Signal(), which is
// exactly the symptom the tester's log shows. Unset or 0 = off.
unsigned takeSignalFault() {
    static std::atomic<int> left{-1};
    int                     cached = left.load(std::memory_order_relaxed);
    if (cached < 0) {
        wchar_t     text[32]{};
        DWORD const n     = ::GetEnvironmentVariableW(L"NEURODRAGON_SIGNAL_FAULT", text, 32);
        long        value = 0;
        if (n > 0 && n < 32) {
            value = std::wcstol(text, nullptr, 10);
        }
        if (value < 0) {
            value = 0;
        }
        left.store(static_cast<int>(value), std::memory_order_relaxed);
        cached = static_cast<int>(value);
    }
    while (cached > 0) {
        if (left.compare_exchange_weak(cached, cached - 1, std::memory_order_relaxed)) {
            return 1;
        }
    }
    return 0;
}
#endif

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
        // Re-arming is also the user's retry after a stall: fresh objects next frame
        // (EXP-0118).
        gRebuild.store(true, std::memory_order_relaxed);
    }
}
bool useArg10() { return gUseArg10.load(std::memory_order_relaxed); }

void setDevice(void* device) { g.device = static_cast<ID3D12Device*>(device); }

void noteSwapChain(void* swapChain) { g.swapChain = static_cast<IDXGISwapChain*>(swapChain); }

// Why there is no arg10 to write, in one clause. The TAAU output is our only cut point:
// if the engine's upscaling is DLSS (its runtime is resident) or there is no upscaling
// pass at all, nothing ever writes arg10. The 2026-10-30 tester log is exactly that -
// 87 lists per submission, split 0 - while the enable line claimed the draw was being
// skipped.
char const* engineUpscalingHint() {
    if (::GetModuleHandleW(L"nvngx_dlss.dll")) {
        return "nvngx_dlss.dll is loaded, so the game's upscaling is DLSS; VV replacement needs TAAU";
    }
    return "the game's upscaling is off or at 100%; VV replacement needs TAAU with a sub-100% ratio";
}

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
        // frame (EXP-0092). That fix waited for the old queue's fence; a queue the
        // frame graph has already retired never advances it, so the wait is forever.
        // Retire instead: the old allocator is never reset, so the GPU finishes it
        // untouched (EXP-0118).
        if (g.fence && g.fence->GetCompletedValue() < g.fenceValue) {
            utils::logger().warn("[vv] inject: queue changed with a pending injection; retiring it");
            retire();
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
    if (gRebuild.exchange(false, std::memory_order_relaxed)) {
        // Re-arming is the user's retry: start from fresh objects so a fence that
        // stalled in the previous arm cannot poison this one (EXP-0118). The pipeline
        // check below builds the new set.
        retire();
        g.rebuilds = 0;
    }
    if (!g.pipelineReady.load(std::memory_order_acquire) && !buildPipeline()) {
        setReason(SkipReason::PipelineFailed);
        return 0;
    }
    // An allocator may not be reset while the GPU is still executing a list recorded
    // from it. One frame of delay is invisible here and far safer than waiting; a
    // stall that lasts kStallSubmits is recovered inside fenceIdle() (EXP-0118).
    if (!fenceIdle()) {
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
        unsigned const               n = misses.fetch_add(1, std::memory_order_relaxed) + 1;
        if (n < 4) {
            // Normal for the first frames after arming: the split is resolved from the
            // lists that bound arg10 in the previous armed frame.
            utils::logger().warn(
                "[vv] inject: arg10 writer not found yet (split={}/{}); TAAU runs this frame",
                split,
                listCount
            );
        } else if (n % 600 == 0) {
            // Ten seconds without a single writer: the premise is wrong, and saying so
            // is the difference between "the mod does nothing" and a diagnosis.
            utils::logger().warn(
                "[vv] inject: no arg10 writer after {} frames (split={}/{}); {}",
                n,
                split,
                listCount,
                engineUpscalingHint()
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
    if (gRecovered.exchange(false, std::memory_order_relaxed)) {
        utils::logger().info(
            "[vv] inject: recovered after a rebuild (injected={} skipped={} rebuilds={})",
            gInjected.load(std::memory_order_relaxed),
            gSkipped.load(std::memory_order_relaxed),
            g.rebuilds
        );
    }
    probe::vv::noteInjectionResult(true);
    unsigned const n = gContentLogs.fetch_add(1, std::memory_order_relaxed);
    if (n < 4) {
        ND_DEBUG("[vv] inject: arg10 boundary split={}/{}", split, listCount);
    }
    if (n == 0) {
        // The "it is really replacing TAAU now" marker, kept in release logs: the enable
        // line only says armed, and this is the first frame that proves otherwise.
        utils::logger().info(
            "[vv] inject: arg10 captured (split={}/{}); the TAAU draw is skipped from here",
            split,
            listCount
        );
    }
    return split;
}

void* list() { return g.list; }

bool isOwnList(void* list) { return list && list == g.list; }

bool recordingOwnView() { return gRecordingView.load(std::memory_order_relaxed); }
void setRecordingOwnView(bool recording) { gRecordingView.store(recording, std::memory_order_relaxed); }

void submitted() {
    bool signalFailed = false;
    if (g.queue && g.fence) {
        UINT64 const value      = ++g.fenceValue;
        bool         skipSignal = false;
#ifdef TEST
        skipSignal = takeSignalFault() != 0;
#endif
        if (skipSignal) {
            // TEST only: the value advances but nothing will ever signal it, which is
            // what a Signal the queue never processed looks like (EXP-0118).
            utils::logger().warn("[vv] inject: TEST fault - dropped Signal({})", value);
        } else {
            HRESULT const hr = g.queue->Signal(g.fence, value);
            if (FAILED(hr)) {
                signalFailed = true;
                g.lastSignal = hr;
                utils::logger().warn(
                    "[vv] inject: Signal({}) failed hr=0x{:08X}",
                    value,
                    static_cast<unsigned>(hr)
                );
            }
        }
    }
    // The reference has to go now: ResizeBuffers fails while any reference to a back
    // buffer is outstanding, and that failure ends in abort() (docs/specs/vv.md §9).
    if (g.pending) {
        g.pending->Release();
        g.pending = nullptr;
    }
    if (signalFailed) {
        // Do not wait for a value nobody will ever signal: retire now and inject again
        // on the next frame (EXP-0118).
        restart("the previous Signal did not reach the queue");
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
    // The next build makes a fence at 0; a value left over from the previous arm would
    // make every later frame read it as busy forever (EXP-0118).
    g.fenceValue  = 0;
    g.busySubmits = 0;
    g.rebuilds    = 0;
    g.lastSignal  = S_OK;
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
