#include "neurodragon/Config.h"
#include "neurodragon/probe/RenderProbe.h"
#include "neurodragon/probe/VvInject.h"
#include "neurodragon/probe/VvProbe.h"
#include "neurodragon/nr/D3D12Bridge.h"
#include "neurodragon/vv/VvDlssSr.h"
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

// The fence only guards allocator reuse, but under Streamline's proxy queue it can
// still be unsignalled on the next world frame (EXP-0142: ~42 fence-busy skips/s
// against ~81 presents/s), and a skipped frame after an injected one presents the
// previous arg10 unchanged. Three slots let one rest for two frames.
constexpr unsigned kSlots = 3;

struct Slot {
    ID3D12CommandAllocator*    allocator{};
    ID3D12GraphicsCommandList* list{};
    ID3D12Fence*               fence{};
    UINT64                     fenceValue{};
    unsigned                   busySubmits{}; ///< consecutive submissions that found this slot's fence unfinished
};

struct Injector {
    ID3D12Device*              device{};
    ID3D12CommandQueue*        queue{};
    Slot                       slots[kSlots];
    unsigned                   slot{};
    unsigned                   rebuilds{};    ///< stall recoveries in this arm (capped)
    HRESULT                    lastSignal{S_OK};
    IDXGISwapChain*            swapChain{};
    ID3D12Resource*            pending{}; ///< back buffer handed to the split submission
    std::atomic<bool>          pipelineReady{false};
};

// A slot has to be re-recordable within a few frames: VvSr and NR rewrite shared
// descriptor heaps while recording, so a list still executing is why prepare() rotates
// instead of reusing the last slot. Three slots give one two frames, which is why a
// fence that is merely late never has to be waited on. A fence that stays unfinished
// for this many attempts is not latency but a stall (EXP-0118): the Signal never
// reached the queue, or the queue meant to run it has been retired.
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
#if ND_VV_HEARTBEAT
std::atomic<unsigned> gQueueChanges{0}; ///< direct-queue rebinds (heartbeat only)
#endif
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

bool anySlotBusy() {
    for (Slot const& slot : g.slots) {
        if (slot.fence && slot.fence->GetCompletedValue() < slot.fenceValue) {
            return true;
        }
    }
    return false;
}

// Retire without resetting the allocators: their lists may still be executing. D3D12
// and the queue keep the submitted objects alive after we drop our references.
void retire() {
    for (Slot& slot : g.slots) {
        if (slot.list) {
            slot.list->Close();
            slot.list->Release();
            slot.list = nullptr;
        }
        if (slot.allocator) {
            slot.allocator->Release();
            slot.allocator = nullptr;
        }
        if (slot.fence) {
            slot.fence->Release();
            slot.fence = nullptr;
        }
        slot.fenceValue  = 0;
        slot.busySubmits = 0;
    }
    if (g.pending) {
        g.pending->Release();
        g.pending = nullptr;
    }
    g.pipelineReady.store(false, std::memory_order_release);
}

bool buildPipeline() {
    if (!g.device) {
        return false;
    }
    for (Slot& slot : g.slots) {
        if (FAILED(g.device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&slot.allocator)))
            || FAILED(g.device->CreateCommandList(
                0,
                D3D12_COMMAND_LIST_TYPE_DIRECT,
                slot.allocator,
                nullptr,
                IID_PPV_ARGS(&slot.list)
            ))
            || FAILED(g.device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&slot.fence)))
            // The list is handed to us in the recording state; a fresh list that has never
            // been closed cannot be Reset on every platform, so close it once here.
            || FAILED(slot.list->Close())) {
            retire();
            return false;
        }
        slot.fenceValue  = 0;
        slot.busySubmits = 0;
    }
    g.slot = kSlots - 1; // the first prepare() advances to slot 0
    g.pipelineReady.store(true, std::memory_order_release);
    ND_DEBUG("[vv] inject pipeline ready slots={}", kSlots);
    return true;
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

// Whether slot `index` may be recorded into again. With three slots the target last ran
// two frames ago, so the ordinary answer costs one GetCompletedValue and no wait. An
// unfinished fence means the GPU is behind, and one skipped frame - which presents the
// previous arg10 unchanged - is cheaper than blocking the render thread on an event
// (EXP-0142). A fence that never finishes is the EXP-0118 stall: rather than skipping
// forever with no picture, the injection is rebuilt.
bool slotIdle(unsigned index) {
    Slot& slot = g.slots[index];
    if (!slot.fence) {
        return true;
    }
    UINT64 const done = slot.fence->GetCompletedValue();
    if (done >= slot.fenceValue) {
        slot.busySubmits = 0;
        return true;
    }
    if (++slot.busySubmits < kStallSubmits) {
        return false;
    }
    char text[192]{};
    std::snprintf(
        text,
        sizeof(text),
        "slot %u stayed at %llu of %llu for %u submissions (signal=0x%08X)",
        index,
        static_cast<unsigned long long>(done),
        static_cast<unsigned long long>(slot.fenceValue),
        slot.busySubmits,
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
    HRESULT const closed = g.slots[g.slot].list->Close();
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
    Slot&         slot = g.slots[g.slot];
    // A list left recording by an earlier failed frame has to be closed before its
    // allocator can be reset again; without this the first failure is permanent.
    slot.list->Close();
    if (FAILED(slot.allocator->Reset()) || FAILED(slot.list->Reset(slot.allocator, nullptr))) {
        return false;
    }
    if (gAction.load(std::memory_order_relaxed) != static_cast<int>(Action::DlssArg10)) {
        return finish(false);
    }
    // prep + DLSS evaluate, then a PS that writes the DLSS output into the engine's
    // arg10. No white point, no encode, no back-buffer copy.
    void* const target = probe::vv::arg10Target();
    if (!target || !neurodragon::vv::sr::recordArg10(slot.list, target)) {
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
        // frame (EXP-0092). Waiting for the old queue's fence does not fix that: a queue
        // the frame graph has already retired never advances it, so the wait is forever
        // and the picture stays frozen. Retire instead - the old allocators are never
        // reset (see retire), so the GPU finishes whatever it still holds untouched
        // (EXP-0118). The rebind below then rebuilds the slots on the next frame.
        if (anySlotBusy()) {
            utils::logger().warn("[vv] inject: queue changed with a pending injection; retiring it");
            retire();
        }
#if ND_VV_HEARTBEAT
        gQueueChanges.fetch_add(1, std::memory_order_relaxed);
#endif
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
    // Rotate only here: a submission that will not record must not consume a slot,
    // or two in-flight injections could share one allocator. The target slot's fence
    // is the one that matters; with three slots it normally has two frames to finish.
    unsigned const next = (g.slot + 1) % kSlots;
    if (!slotIdle(next)) {
        setReason(SkipReason::FenceBusy);
        return 0;
    }
    g.slot = next;
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
    unsigned const injected = gInjected.fetch_add(1, std::memory_order_relaxed) + 1;
    if (gRecovered.exchange(false, std::memory_order_relaxed)) {
        utils::logger().info(
            "[vv] inject: recovered after a rebuild (injected={} skipped={} rebuilds={})",
            injected,
            gSkipped.load(std::memory_order_relaxed),
            g.rebuilds
        );
    }
    probe::vv::noteInjectionResult(true);
    if (injected % 600 == 0) {
        // One line per 600 world frames: read it against the present counter to see
        // whether injection kept a 1:1 pace (EXP-0144's acceptance signal).
        utils::logger().info(
            "[vv] inject: {} injected, {} skipped since arm",
            injected,
            gSkipped.load(std::memory_order_relaxed)
        );
    }
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

void* list() { return g.slots[g.slot].list; }

bool isOwnList(void* list) {
    for (Slot const& slot : g.slots) {
        if (list && list == slot.list) {
            return true;
        }
    }
    return false;
}

bool recordingOwnView() { return gRecordingView.load(std::memory_order_relaxed); }
void setRecordingOwnView(bool recording) { gRecordingView.store(recording, std::memory_order_relaxed); }

void submitted() {
    Slot& slot         = g.slots[g.slot];
    bool  signalFailed = false;
    if (g.queue && slot.fence) {
        UINT64 const value      = ++slot.fenceValue;
        bool         skipSignal = false;
#ifdef TEST
        skipSignal = takeSignalFault() != 0;
#endif
        if (skipSignal) {
            // TEST only: the value advances but nothing will ever signal it, which is
            // what a Signal the queue never processed looks like (EXP-0118).
            utils::logger().warn("[vv] inject: TEST fault - dropped Signal({})", value);
        } else {
            HRESULT const hr = g.queue->Signal(slot.fence, value);
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
    retire();
    g.rebuilds   = 0;
    g.lastSignal = S_OK;
    g.device     = nullptr;
    g.queue      = nullptr;
    g.swapChain  = nullptr;
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

#if ND_VV_HEARTBEAT
namespace {

char const* reasonName(SkipReason reason) {
    switch (reason) {
    case SkipReason::None:           return "none";
    case SkipReason::NoDevice:       return "no-device";
    case SkipReason::NoSwapChain:    return "no-swapchain";
    case SkipReason::FenceBusy:      return "fence-busy";
    case SkipReason::NoBackBuffer:   return "no-backbuffer";
    case SkipReason::PipelineFailed: return "pipeline";
    case SkipReason::RecordFailed:   return "record-failed";
    case SkipReason::NoBoundary:     return "no-writer";
    }
    return "?";
}

} // namespace

// EXP-0119: one line per kHeartbeatPresents presents, while the product is armed.
// Release logs only print on a reason change, which is exactly what made the 4K
// machine's stall unreadable; this line is the periodic summary that replaces it.
// Deleting this function (and the macro in VvInject.h) removes the diagnostic.
void notePresentHeartbeat() {
    static unsigned presents     = 0;
    static unsigned lastInjected = 0;
    static unsigned lastSkipped  = 0;
    if (gMode.load(std::memory_order_relaxed) == static_cast<int>(Mode::Off)) {
        return;
    }
    if (++presents % kHeartbeatPresents != 0) {
        return;
    }
    unsigned const injected = gInjected.load(std::memory_order_relaxed);
    unsigned const skipped  = gSkipped.load(std::memory_order_relaxed);
    unsigned const injectedDelta = injected - lastInjected;
    unsigned const skippedDelta  = skipped - lastSkipped;
    lastInjected = injected;
    lastSkipped  = skipped;

    unsigned displayW = 0;
    unsigned displayH = 0;
    if (g.swapChain) {
        DXGI_SWAP_CHAIN_DESC desc{};
        if (SUCCEEDED(g.swapChain->GetDesc(&desc))) {
            displayW = desc.BufferDesc.Width;
            displayH = desc.BufferDesc.Height;
        }
    }
    auto const   diag    = neurodragon::vv::sr::diag();
    auto const   sr      = neurodragon::vv::dlss::srState();
    auto const   nrProbe = probe::nrDiagnostic();
    auto const   nr      = neurodragon::nr::nrPanelStats();
    // The fence columns describe the slot the next injection would use; inFlight is how
    // many of the kSlots still have GPU work outstanding.
    Slot const&  slot     = g.slots[g.slot];
    UINT64 const done     = slot.fence ? slot.fence->GetCompletedValue() : 0;
    UINT64 const signaled = slot.fenceValue;
    unsigned     inFlight = 0;
    for (Slot const& each : g.slots) {
        if (each.fence && each.fence->GetCompletedValue() < each.fenceValue) {
            ++inFlight;
        }
    }
    utils::logger().info(
        "[vv] heartbeat: {} presents, injected +{} skipped +{} | reason={} split={}/{} "
        "render={}x{} display={}x{} | sr colours={} depths={} mv={}x{} fmt={} fail={} sinceOk={} "
        "ready={} create=0x{:08X} eval=0x{:08X} | ngx={} nrOn={} evalNr={} gate=0x{:02X} params={} "
        "tried={} attempted={} created={} nrEval={} err=0x{:08X} | fence={}/{} rebuilds={} queues={} "
        "slots={} inFlight={}",
        presents,
        injectedDelta,
        skippedDelta,
        reasonName(static_cast<SkipReason>(gReason.load(std::memory_order_relaxed))),
        gSplit.load(std::memory_order_relaxed),
        gLists.load(std::memory_order_relaxed),
        probe::vv::renderWidth(),
        probe::vv::renderHeight(),
        displayW,
        displayH,
        diag.colourCandidates,
        diag.depthCandidates,
        diag.mvWidth,
        diag.mvHeight,
        diag.mvFormat,
        diag.lastFail,
        diag.framesSinceOk,
        sr.ready,
        static_cast<unsigned>(sr.createRet),
        static_cast<unsigned>(sr.evaluateRet),
        nrProbe.evaluateSerial,
        config::get().nr.enabled,
        nrProbe.enabled,
        nrProbe.gateMask,
        nrProbe.paramsValid,
        nrProbe.startTried,
        nr.featureAttempted,
        nr.featureCreated,
        nr.evalCount,
        static_cast<unsigned>(nr.lastError),
        done,
        signaled,
        g.rebuilds,
        gQueueChanges.load(std::memory_order_relaxed),
        kSlots,
        inFlight
    );
}
#endif

} // namespace neurodragon::probe::vv::inject
