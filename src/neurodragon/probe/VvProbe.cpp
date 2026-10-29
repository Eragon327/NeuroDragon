#include "neurodragon/probe/VvProbe.h"

#include "neurodragon/nr/D3D12Bridge.h"
#include "neurodragon/probe/RuntimeOptions.h"
#include "neurodragon/probe/VvCalibrate.h"
#include "neurodragon/probe/VvCamera.h"
#include "neurodragon/probe/VvInject.h"
#include "neurodragon/utils/Utils.h"
#include "neurodragon/vv/VvSr.h"

#include <MinHook.h>
#include <Windows.h>

#include "ll/api/event/EventBus.h"
#include "ll/api/event/client/ClientExitLevelEvent.h"
#include "ll/api/event/client/ClientJoinLevelEvent.h"
#include "ll/api/event/world/ClientLevelTickEvent.h"
#include "ll/api/memory/Hook.h"


#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <filesystem>
#include <intrin.h>
#include <string>

namespace neurodragon::probe::vv {
namespace {

// ---------------------------------------------------------------------------
// Offline-located entry points (Minecraft.Windows.exe 1.26.5101.0)
// ---------------------------------------------------------------------------
//
// No PDB, so the anchors are the material uniform names the code itself builds:
//
//   "InputFinalColor" / "InputBufferMotionVectors" / "InputTAAHistory"
//       -> exactly one RIP-relative reference each, all three inside ONE .pdata
//          function => dragon::framerenderer::modules::taau (the declarator).
//   "SubPixelJitterAndValidHistory" / "ResolutionRatiosAndFPEpsilon" / ...
//       -> the pass' draw callback (std::function target). Not hooked yet: the
//          declarator already carries every number we need for M4.
//
// RVAs are relative to the main module, so they survive ASLR. The prologue is
// checked at install time: a patch that moves the function must fail loudly here
// instead of silently missing (or worse, patching half of another function).
constexpr std::uintptr_t kTaauRva = 0xb74f860;

// The pass' draw callback: the std::function target that runs once per frame with
// the resolved textures. Counted only - if the declarator fires but this does not,
// the pass is declared and then skipped, which is a different bug than "no VV".
constexpr std::uintptr_t kTaauDrawRva = 0xb752820;

// push rbp; push r15; push r14; push r13; push r12; push rsi; push rdi; push rbx;
// sub rsp, imm32
constexpr unsigned char kPrologue[] = {
    0x55, 0x41, 0x57, 0x41, 0x56, 0x41, 0x55, 0x41, 0x54, 0x56, 0x57, 0x53, 0x48, 0x81, 0xEC
};
constexpr std::size_t kPrologueSize = sizeof(kPrologue);

// The 12 parameters are four references in registers and eight references on the
// stack (x64): declarator, allocator, material manager, material service,
// viewport, upscaling data, camera data, mesh filter, then the four
// BindFramebufferColor dependencies (colour / history / motion vectors / output -
// the order is read off the original: docs/A §7.1). Declaring all twelve keeps
// the stack arguments intact for the callee.
using TaauFn = void (*)(void*, void*, void*, void*, void*, void*, void*, void*, void*, void*, void*, void*);
// The callback is invoked through std::function, so the closure pointer comes First
// and the rest is whatever the lambda declared; four pointer-sized slots cover it.
using DrawFn = void (*)(void*, void*, void*, void*);

std::atomic<std::uint64_t> gTaauHits{0};
std::atomic<std::uint64_t> gDrawHits{0};
std::atomic<std::uint64_t> gDrawForwarded{0};
std::atomic<std::uint64_t> gDrawSuppressed{0};
std::atomic<std::uint64_t> gReplacementMissed{0};
std::atomic<bool>          gReplaceTaauDraw{false};
std::atomic<std::uint64_t> gPresents{0};
std::atomic<std::uint64_t> gSubmits{0};
std::atomic<std::uint64_t> gSubmittedLists{0};
std::atomic<unsigned>      gMaxListsPerSubmit{0};
std::atomic<bool>          gInstalled{false};

void*  gTaauTarget{};
TaauFn gOriginalTaau{};
void*  gDrawTarget{};
DrawFn gOriginalDraw{};

// ---------------------------------------------------------------------------
// v4: command lists and render targets
// ---------------------------------------------------------------------------
//
// The frame is ONE ExecuteCommandLists call carrying ~48 command lists, so the only
// question left about "where can our GPU work go" is which list holds the TAAU draw.
// These hooks answer it with resources instead of guesses:

bool        readable(void const* p, std::size_t bytes);
std::string moduleOf(std::uint64_t value);
//
//   * D3D12CreateDevice (d3d12.dll export) -> the engine's device, taken early
//     enough that the render-target views created afterwards are all seen;
//   * ID3D12GraphicsCommandList::OMSetRenderTargets (vtable 46) -> the render target
//     each list writes. The method's `this` IS the list object, so every write is
//     self-labelled;
//   * DrawInstanced / DrawIndexedInstanced (vtable 12 / 13) -> whether that list
//     actually drew, which is what separates the world composite from earlier lists;
//   * ID3D12Device::CreateRenderTargetView (vtable 20) -> CPU descriptor handle to
//     ID3D12Resource, which is what turns a render-target write into a texture.
//
// Every detour is observation only: it forwards immediately. No method is marshalled,
// no resource state is touched.
using CreateDeviceFn = long(WINAPI*)(void* adapter, int minFeatureLevel, void const* riid, void** device);
using CreateRtvFn    = void (*)(void* device, void* resource, void const* desc, std::uintptr_t handle);
using OmSetRtvFn = void (*)(void* list, unsigned count, std::uintptr_t const* rtvs, int single, std::uintptr_t const* dsv);
// Same reason for the draw call: a fullscreen triangle is DrawInstanced(3,1,0,0),
// which is how the TAAU pass itself is issued, and a clean triple proves the two
// neighbouring vtable slots were read correctly.
using CmdDrawFn = void (*)(void* list, unsigned vertexCount, unsigned instanceCount, unsigned startVertex, unsigned startInstance);
using SetViewportsFn = void (*)(void* list, unsigned count, void const* viewports);

struct RtvBinding {
    std::uint64_t handle;
    void*         resource;
};
constexpr unsigned    kRtvSlots = 4096;
RtvBinding            gRtvs[kRtvSlots]{};
std::atomic<unsigned> gRtvCount{0};

// --- content boundary (B1d) ------------------------------------------------
// The frame's world composite is ONE list that writes the whole back buffer at
// display resolution; everything before it leaves the back buffer untouched and
// everything after it is the hand / HUD / UI. That was measured directly by the
// B1c cut capture (2026-10-25: only tail 5 -> 4 changes a pixel) - so the stable
// anchor is "the first list in the frame that binds a back buffer and draws",
// not a tail count (docs/specs/vv.md §14.5).
//
// Finding it needs two pure-forward detours: OMSetRenderTargets (slot 46) to see
// the binding, DrawInstanced / DrawIndexedInstanced (12 / 13) to see that the list
// actually drew. The handles are compared against the back buffer's own view
// handles, which are refreshed once per present.
constexpr unsigned kBackBufferHandles = 32;
std::atomic<std::uint64_t> gBackBufferHandles[kBackBufferHandles]{};
std::atomic<unsigned>      gBackBufferHandleCount{0};
std::atomic<unsigned>      gDescriptorIncrement{0};

struct ListMark {
    void* list;
    bool  bound;
    bool  drew;
};
constexpr unsigned kMarkSlots = 16;
ListMark              gMarks[kMarkSlots]{};
std::atomic<unsigned> gMarkCount{0};
std::atomic<unsigned> gBoundarySplit{0}; ///< 1-based: cut AFTER this many lists
std::atomic<void*>    gBoundaryList{nullptr};
std::atomic<unsigned> gBoundaryLogs{0};

// Diagnostic for the content boundary's fallback anchor: one entry per list that
// did anything observable this frame - viewport, depth binding, first draw shape.
// The world composite and the UI set a display-sized viewport; the world passes
// render at the render resolution. One line per frame for the first frames only.
struct ListSig {
    void*           list;
    unsigned        width;
    unsigned        height;
    unsigned        dsv;          ///< bound a depth-stencil view in this list
    unsigned        draws;        ///< DrawInstanced + DrawIndexedInstanced in this list
    unsigned        firstVerts;   ///< first DrawInstanced vertex count (0 = none)
    unsigned        firstInst;    ///< first DrawInstanced instance count
    unsigned        firstIndices; ///< first DrawIndexedInstanced index count (0 = none)
    std::uintptr_t  rtvHandle;
    void*           rtvResource;
    unsigned        rtvCalls;
    unsigned        rtvGen;       ///< arm generation the capture belongs to
    unsigned        arg10CopyMask;
    void*           arg10CopyPeer;
    // Every RTV handle this list bound during the current arming, not just its
    // first call: a list may bind a small target first and arg10 later, and the
    // writer is "any binding", not "the first one" (EXP-0057 postmortem). 16, not 8:
    // the 108/109-list shapes bind a longer mip chain before arg10, and when the
    // eight slots filled the writer stopped resolving after the shape changed.
    std::uintptr_t  rtvSeen[16];
    unsigned        rtvSeenCount;
    unsigned        rtvSeenGen;
    // Direct evidence recorded at bind time: this list bound the *current* arg10
    // native while the capture was armed, tagged with the declaration serial so a
    // stale binding from the previous frame cannot match the alternating buffer.
    void*           arg10Bound;
    unsigned        arg10BoundSerial;
};
// 512, not 128: UI frames (panel open) bring new list objects into the table, and
// when it filled the arg10 writer stopped resolving after the panel closed (EXP-0058).
constexpr unsigned kSigSlots = 512;
ListSig               gSigs[kSigSlots]{};
std::atomic<unsigned> gSigCount{0};
std::atomic<unsigned> gSigFrames{0};

ListSig* sigFor(void* list) {
    if (!list) {
        return nullptr;
    }
    unsigned const n = gSigCount.load(std::memory_order_relaxed);
    for (unsigned i = 0; i < n && i < kSigSlots; ++i) {
        if (gSigs[i].list == list) {
            return &gSigs[i];
        }
    }
    unsigned const slot = gSigCount.fetch_add(1, std::memory_order_relaxed);
    if (slot < kSigSlots) {
        gSigs[slot] = ListSig{list, 0, 0, 0, 0, 0, 0, 0, 0, nullptr, 0, 0, 0, nullptr, {}, 0, ~0u, nullptr, 0};
        return &gSigs[slot];
    }
    static std::atomic<bool> warned{false};
    if (!warned.exchange(true, std::memory_order_relaxed)) {
        utils::logger().warn("[vv] list signature table full ({} slots); new lists are not tracked", kSigSlots);
    }
    return nullptr;
}

void noteViewport(void* list, unsigned width, unsigned height) {
    if (auto* const sig = sigFor(list)) {
        sig->width  = width;
        sig->height = height;
    }
}

std::atomic<void*>    gResourceVtable{nullptr};
// Every distinct CreateRenderTargetView implementation we have hooked (one per
// device class): the overlay's throw-away device and the engine's own are not the
// same function.
constexpr unsigned kHookedVtables = 4;
void*              gHookedRtvTargets[kHookedVtables]{};
unsigned           gHookCount = 0;

CreateDeviceFn gOriginalCreateDevice{};
CreateRtvFn    gOriginalCreateRtv{};
CreateRtvFn    gOriginalCreateDsv{};
OmSetRtvFn     gOriginalOmSetRtv{};
CmdDrawFn      gOriginalDrawInstanced{};
std::atomic<unsigned> gDrawInstancedCalls{0};
SetViewportsFn gOriginalSetViewports{};
using CmdDrawIndexedFn = void (*)(void* list, unsigned indexCount, unsigned instanceCount, unsigned startIndex, int baseVertex, unsigned startInstance);
CmdDrawIndexedFn      gOriginalDrawIndexed{};
std::atomic<unsigned> gDrawIndexedCalls{0};
using CopyTextureRegionFn = void (*)(
    void* list,
    void const* dst,
    unsigned dstX,
    unsigned dstY,
    unsigned dstZ,
    void const* src,
    void const* srcBox
);
using CopyResourceFn = void (*)(void* list, void* dst, void* src);
CopyTextureRegionFn gOriginalCopyTextureRegion{};
CopyResourceFn      gOriginalCopyResource{};
void copyTextureRegionDetour(void*, void const*, unsigned, unsigned, unsigned, void const*, void const*);
void copyResourceDetour(void*, void*, void*);

std::atomic<bool> gDeviceHooked{false};
std::atomic<bool> gListHooked{false};
// The 120-frame sampler that used to arm here (`gDumpFrame` / `gCloseInFrame` /
// `gBigFrames`) was removed on 2026-10-25 22:46: it cost a render-thread burst every
// ~1.2 s and its only reader was its own log line.

// ---------------------------------------------------------------------------
// B0 probes (docs/specs/vv.md §13.1) - diagnostics build only
// ---------------------------------------------------------------------------
//
// P0.4 needs the two objects the TAAU pass actually owns, sampled together:
//   * the draw callback's TAAContext (its third argument), where the matrices
//     already read out at +0x200/+0x240 live - and, per P0.4, maybe only the
//     PROJECTION there, so the whole window is sampled rather than one offset;
//   * UpscalingCameraData (the declarator's `camera` argument), which is the
//     natural home of the view/projection pair the Mac decompile calls
//     Current/PreviousViewProjectionMatrixUniform.
// A marker file (`vv_ctx_now.txt`) arms 12 consecutive samples; logs are parsed
// offline (tools/vv_ctx.py). Nothing is written and no state is touched.
#ifdef TEST
std::atomic<unsigned> gJitterIndex{0};
std::atomic<void*>    gCameraData{nullptr};
std::atomic<unsigned> gCtxSamplesLeft{0};
std::atomic<unsigned> gCtxSampleIndex{0};
std::atomic<unsigned> gCtxBatch{0};
// F8 asks for a sample and only starts after a delay, so the player can press it
// while standing and then walk: a file marker (which takes two hands and a
// keyboard) is no longer needed for the moving sample.
std::atomic<unsigned long long> gCtxStartMs{0};
// One-shot P0.1 state: the core is only queried after the engine's own Init was
// observed, and the answer is logged once per state change.
std::atomic<bool> gSessionReady{false};
#endif

// Cached engine device and queue, filled when the first engine submission is seen.
std::atomic<void*> gEngineDevice{nullptr};
// Level presence, from the client join/exit event pair the RT path already uses.
// Polling ll::service::getLevel() is not an option: it was non-empty in the first
// world and stayed empty after leaving and entering another one (2026-10-25 23:36),
// so an SR gate built on it never reopened.
std::atomic<bool>      gWorldLoaded{false};
ll::event::ListenerPtr gJoinLevelListener{};
ll::event::ListenerPtr gExitLevelListener{};
ll::event::ListenerPtr gLevelTickListener{};
// The TAAContext handed to the TAAU draw callback, kept per frame. It holds the
// current/previous view-projection pair (+0x200/+0x240), the camera origins
// (+0x280..) and the sub-pixel jitter (+0x1e4 ..) that the motion-vector
// reconstruction needs (docs/specs/vv.md §6).
std::atomic<void*> gTaaContext{nullptr};
// The draw callback's second argument (execution context). The +0x200 matrices turned
// out to be unchanged under pure forward movement (2026-10-25 03:23), which is only
// right for a camera-relative pair, so this object is dumped next to it.
std::atomic<void*> gExecContext{nullptr};std::atomic<void*> gGameQueue{nullptr};
// EXP-0028: the InputFinalColor dependency (declarator arg 10) of the last *armed*
// frame. The declarator-phase scan sees the pass only declared; the draw callback is
// where the engine has resolved it, so the same flat scan runs there too, on this
// pointer. Only ever read (each level guarded by readable()), never dereferenced
// through an unmatched value - a stale pointer costs comparisons, not a fault.
std::atomic<void*> gInputColorDependency{nullptr};
// The draw callback runs *before* the engine's own draw, and that draw updates the
// TAA history: by the time our injection records (ExecuteCommandLists, after the whole
// frame) the previous pair has already been collapsed onto the current one. Measured
// 2026-10-26: samples taken inside the callback satisfy prev(n)=cur(n-1), while samples
// taken after the frame are prev==cur - so the pair has to be copied here, not read
// later. Offsets: +0x200 current VP, +0x240 previous VP, +0x1e4 jitter, +0x280 origins.
struct TaaSnapshot {
    float current[16]{};
    float previous[16]{};
    float camera[6]{};
    float jitter[2]{};
};
TaaSnapshot       gTaaSnapshot{};
std::atomic<bool> gTaaSnapshotValid{false};
void*              gExcluded[4]{};
bool               gLoggedExclusion = false;
unsigned           gExcludedCount = 0;
unsigned           gLastSeenCount = 0;
std::uint64_t      gSeenStableSince = 0;
unsigned           gDumpPasses = 0;
// Every resource that has ever been given a render-target view, so the one-shot dump
// knows what to look at without depending on any unverified vtable slot.
// Raw-pointer render-target inventory. No references are held (see
// createRtvDetour), and it is deliberately NOT cleared on resize: the engine
// creates the new render graph's views *before* our ResizeBuffers hook runs
// (2026-10-25 03:28:50.510 vs .518), so clearing there would discard exactly the
// textures the next dump needs. 4096 entries cover a whole session.
constexpr unsigned kSeenSlots = 4096;
void*              gSeen[kSeenSlots]{};
unsigned           gSeenCount = 0;

// Reference-counted subset of the RTV inventory, safe to describe at dump time.
// Only small, non-swap-chain resources enter it (see createRtvDetour), it is a
// ring so the newest 128 survive, and it is NOT cleared on resize: the new
// graph's views are created just before our ResizeBuffers hook runs. This is the
// list dumpSemantics uses; the full gSeen list stays raw because a *stale* raw
// pointer passed to nr::describe() is exactly the 2026-10-25 03:4x AV.
struct HeldRtv {
    void*    resource;
    unsigned width;
    unsigned height;
    unsigned format;
};
constexpr unsigned kHeldSlots = 256;
HeldRtv            gHeld[kHeldSlots]{};
unsigned           gHeldCount = 0;
unsigned           gHeldNext = 0;

// Depth targets are created as *depth-stencil views*, not render-target views, so
// they never entered the inventory above and the SR pipeline could not find the
// main depth (2026-10-25: 1689x903 fmt=26 RTVs were there, no 43/44 anywhere).
// CreateDepthStencilView is device vtable slot 21 (CreateRenderTargetView is 20).
// 64 slots, not 8: the engine creates small helper depths continuously (a 128x128
// one every ~6 s in the 2026-10-25 session), and with 8 slots the main depth was
// evicted before the next re-pick. Small depths are filtered out below as well.
struct HeldDepth {
    void*    resource;
    unsigned width;
    unsigned height;
    unsigned format;
};
constexpr unsigned kHeldDepthSlots = 64;
HeldDepth          gHeldDepth[kHeldDepthSlots]{};
unsigned           gHeldDepthCount = 0;
unsigned           gHeldDepthNext = 0;

// The render resolution the TAAU pass upscales FROM: display viewport x the engine's
// factor. It is the key that turns the render-target inventory into named inputs
// (docs/specs/vv.md §6): 373x239 fmt=10 is the HDR colour, fmt=12 the motion vectors,
// fmt=54 the depth. Filled by taauDetour, which is the only place both numbers exist.
std::atomic<unsigned> gRenderWidth{0};
std::atomic<unsigned> gRenderHeight{0};
std::atomic<unsigned> gDisplayWidth{0};
std::atomic<unsigned> gDisplayHeight{0};
// True when gRenderWidth/Height came from the inventory fallback instead of taauDetour.
// A resize invalidates them; a taauDetour write takes over again on its own.
std::atomic<bool> gRenderDerived{false};
// Bumped on resize / world enter; the B1b bypass drops its cached colour then.
std::atomic<unsigned> gRenderGeneration{1};
// Bit per candidate that has been dumped successfully: the inventory is not always
// complete (a rebuild may create only part of the render-resolution group), so the
// dump retries until every format has been captured once.
std::atomic<unsigned> gSemanticsMask{0};
// Dumps are taken in batches: the main menu also has content, so a single dump
// can easily be the menu instead of a rendered world. Five batches of ~10 s apart
// cover any menu -> world transition; each file carries its batch number.
std::atomic<unsigned> gDumpBatch{0};
// Tick at which a world was entered; the dump waits a few seconds for the world
// to finish loading. Zero means no pending dump.
std::atomic<unsigned> gWorldEnteredTick{0};
// Tick of the last render-target view creation. The engine only creates these when it
// (re)builds the render graph - a fresh start reuses the previous process' layout and
// the views are created before mods load, so the inventory stays empty until something
// changes the window. The dump only runs a few seconds after the last creation, which
// is exactly the "quiet window" the crash of 2026-10-24 22:58 was missing.
std::atomic<unsigned> gLastRtvTick{0};
// Kept only to dump one back buffer as a control: its state is known (PRESENT after
// the flip), so if that dump has content and a render-resolution one does not, the
// problem is the guessed StateBefore, not the copy path.
std::atomic<void*> gSwapChain{nullptr};

void hookDevice(void* device);
void hookCommandList(void* list);
class ID3D12GraphicsCommandList;
void calibrateSlots(ID3D12GraphicsCommandList* list);

// Direct-mapped handle -> resource cache in front of the append-only ring: the
// bind-time arg10 proof needs one lookup per bound handle per call, and the ring
// scan is O(4096). The cache is write-through on every creation, so a descriptor
// reused for a new resource cannot serve a stale mapping.
constexpr unsigned kRtvCacheSlots = 1024;
struct RtvCacheEntry {
    std::uintptr_t handle;
    void*          resource;
};
RtvCacheEntry gRtvCache[kRtvCacheSlots]{};

void recordRtv(std::uintptr_t handle, void* resource) {
    // Ring, not a cap: the engine creates views for transient targets continuously,
    // and once the old "stop after 4096" version filled, every newer handle became
    // unresolvable - which is exactly how the arg10 writer anchor died mid-session.
    unsigned const n = gRtvCount.fetch_add(1, std::memory_order_relaxed);
    gRtvs[n % kRtvSlots] = RtvBinding{handle, resource};
    gRtvCache[(handle >> 4) % kRtvCacheSlots] = RtvCacheEntry{handle, resource};
}

void* lookupRtv(std::uintptr_t handle) {
    auto const& cached = gRtvCache[(handle >> 4) % kRtvCacheSlots];
    if (cached.handle == handle) {
        return cached.resource;
    }
    unsigned const n     = gRtvCount.load(std::memory_order_relaxed);
    unsigned const count = n < kRtvSlots ? n : kRtvSlots;
    for (unsigned k = 0; k < count; ++k) {
        unsigned const i = (n - 1 - k) % kRtvSlots;
        if (gRtvs[i].handle == handle) {
            gRtvCache[(handle >> 4) % kRtvCacheSlots] = RtvCacheEntry{handle, gRtvs[i].resource};
            return gRtvs[i].resource;
        }
    }
    return nullptr;
}

// Does this list bind `resource` in any of its OMSetRenderTargets calls? Resolving
// here, at submission time, keeps the render-thread detour to storing handles.
bool sigBindsResource(ListSig const* sig, void* resource) {
    if (!sig || !resource) {
        return false;
    }
    if (sig->rtvResource == resource) {
        return true;
    }
    for (unsigned i = 0; i < sig->rtvSeenCount; ++i) {
        if (lookupRtv(sig->rtvSeen[i]) == resource) {
            return true;
        }
    }
    return false;
}

// Stage C writer anchor: prefer the bind-time proof tagged with this frame's
// declaration serial; fall back to the lifetime handle set for frames recorded
// before the tag existed. Shape-independent, because it does not need the handle
// to stay resolvable until submission.
bool sigBindsArg10(ListSig const* sig, void* native, unsigned serial) {
    if (!sig || !native) {
        return false;
    }
    if (sig->arg10Bound == native && sig->arg10BoundSerial == serial) {
        return true;
    }
    return sigBindsResource(sig, native);
}

// --- content boundary helpers ----------------------------------------------
// The back buffers' view handles, so the hot OMSetRenderTargets detour only has to
// compare a handful of integers. The pointers come from noteSwapChain (gExcluded),
// the handles from the RTV inventory; both are refreshed once per present.
bool isBackBufferResource(void* resource) {
    if (!resource) {
        return false;
    }
    for (unsigned i = 0; i < gExcludedCount && i < 4; ++i) {
        if (gExcluded[i] == resource) {
            return true;
        }
    }
    return false;
}

void refreshBackBufferHandles() {
    // Two things move: the chain's buffers (resize) and the RTV inventory (the engine
    // re-creates the back buffers' views when it rebuilds its render graph - 2026-10-25
    // 16:59 showed two 2560x1440 fmt=28 views appearing mid-session). The first scan
    // therefore has to be retried whenever a new view shows up, not only on resize.
    static void*         previous[4]{};
    static unsigned      previousCount = 0;
    static unsigned      previousRtvs  = 0;
    unsigned const       rtvs          = gRtvCount.load(std::memory_order_relaxed);
    bool                 changed       = previousCount != gExcludedCount || previousRtvs != rtvs;
    for (unsigned i = 0; i < gExcludedCount && i < 4; ++i) {
        if (previous[i] != gExcluded[i]) {
            changed = true;
        }
        previous[i] = gExcluded[i];
    }
    previousCount = gExcludedCount;
    previousRtvs  = rtvs;
    if (!changed) {
        return;
    }
    unsigned         found = 0;
    unsigned const   limit = rtvs < kRtvSlots ? rtvs : kRtvSlots;
    // Oldest first: the engine's own view of a back buffer is the one the composite
    // binds, and it is created when the render graph is rebuilt. Newer views (the
    // overlay's D3D11On12 wrap among them) must not evict it, which "newest N" did.
    for (unsigned i = 0; i < limit && found < kBackBufferHandles; ++i) {
        if (isBackBufferResource(gRtvs[i].resource)) {
            gBackBufferHandles[found++].store(gRtvs[i].handle, std::memory_order_relaxed);
        }
    }
    gBackBufferHandleCount.store(found, std::memory_order_relaxed);
    static unsigned previousFound = 0xFFFFFFFFu;
    if (found != previousFound) {
        previousFound = found;
        utils::logger().info("[vv] back-buffer view handles={} (chain buffers={}, rtv views={})", found, gExcludedCount, rtvs);
    }
}

void markBound(void* list) {
    unsigned const count = gMarkCount.load(std::memory_order_relaxed);
    for (unsigned i = 0; i < count && i < kMarkSlots; ++i) {
        if (gMarks[i].list == list) {
            gMarks[i].bound = true;
            return;
        }
    }
    unsigned const slot = gMarkCount.fetch_add(1, std::memory_order_relaxed);
    if (slot < kMarkSlots) {
        gMarks[slot].list  = list;
        gMarks[slot].bound = true;
        gMarks[slot].drew  = false;
    }
}

void markDrew(void* list) {
    // Only lists that already bound a back buffer can become the boundary; a draw
    // on any other list costs one comparison over at most kMarkSlots entries.
    unsigned const count = gMarkCount.load(std::memory_order_relaxed);
    for (unsigned i = 0; i < count && i < kMarkSlots; ++i) {
        if (gMarks[i].list == list) {
            gMarks[i].drew = true;
            return;
        }
    }
}

// True when the resource is one of the swap chain's current back buffers. Those
// must never be AddRef'd (ResizeBuffers would fail and the engine aborts); the
// size gate below already excludes them, but this is the exact test when the
// chain is known.
//
// The chain here has 2 or 3 buffers: MCBE presents a 3-buffer flip chain since
// 2026-10-28 23:34 (EXP-0078), and asking for only two left the third back buffer
// unrecognised - its RTV then entered gHeld with a reference, ResizeBuffers answered
// DXGI_ERROR_INVALID_CALL and the engine aborted (0xC0000409). Four slots cover the
// chain and leave room for a driver that adds one.
bool isSwapChainBuffer(void* resource) {
    if (!resource) {
        return false;
    }
    auto* const chain = gSwapChain.load(std::memory_order_relaxed);
    if (!chain) {
        return false;
    }
    void*    buffers[4]{};
    unsigned const count = swapChainBuffers(chain, buffers, 4);
    bool     found = false;
    for (unsigned i = 0; i < count; ++i) {
        if (buffers[i] == resource) {
            found = true;
        }
    }
    for (unsigned i = 0; i < count; ++i) {
        releaseComObject(buffers[i]);
    }
    return found;
}

// A texture the SR pipeline can never use, and which must not push the one it needs
// out of a ring inventory: the render graph is at most a few times smaller than the
// display, so anything under display/4 is a mip, a UI atlas or a helper map.
// 2026-10-25 23:05 is the reason this exists: the engine created a fresh 128x128
// depth every ~6 s, the depth ring had 8 slots, and the 1689x903 depth - the SR
// input - was pushed out while the frame still rendered at full size. The next
// re-pick answered "no render-resolution colour/depth in the inventory" and the
// injection stopped for the rest of the session.
bool tooSmallForRenderGraph(unsigned width, unsigned height) {
    unsigned const dw = gDisplayWidth.load(std::memory_order_relaxed);
    unsigned const dh = gDisplayHeight.load(std::memory_order_relaxed);
    if (!dw || !dh) {
        return false; // display size not known yet: keep everything rather than guess
    }
    return width * 4 < dw || height * 4 < dh;
}

void createRtvDetour(void* device, void* resource, void const* desc, std::uintptr_t handle) {
    // The boundary experiment creates a view of the back buffer on this same device;
    // the back buffer is not part of the frame graph this hook inventories.
    if (inject::recordingOwnView()) {
        gOriginalCreateRtv(device, resource, desc, handle);
        return;
    }
    recordRtv(handle, resource);
    // The first render-target view names a real ID3D12Resource, so its vtable is the
    // resource vtable. Matching against it is what makes the texture scan safe: a
    // device, queue or descriptor heap also has its vtable in d3d12, and calling
    // GetDesc on one of those is a crash, not a diagnosis.
    if (!gResourceVtable.load(std::memory_order_relaxed) && resource && readable(resource, sizeof(void*))) {
        gResourceVtable.store(*reinterpret_cast<void**>(resource), std::memory_order_relaxed);
    }
    // Log each *distinct* resource once, unbounded: a render-target view is only
    // ever created when a texture becomes a target, so this is the closest thing to
    // an inventory of the frame's render targets - and it stays readable because the
    // pool is reused and the same textures reappear.
    bool isNew = true;
    for (unsigned i = 0; i < gSeenCount; ++i) {
        if (gSeen[i] == resource) {
            isNew = false;
            break;
        }
    }
    if (isNew && gSeenCount < kSeenSlots) {
        // The raw inventory NEVER holds a reference: it sees every render-target
        // view, including the swap chain's back buffers, and a live reference to
        // a back buffer makes ResizeBuffers fail (engine abort(), 0xC0000409).
        // The reference-counted list dumpSemantics may safely describe is gHeld,
        // restricted to small non-swap-chain textures.
        gSeen[gSeenCount++] = resource;
        gLastRtvTick.store(GetTickCount(), std::memory_order_relaxed);
        auto const info = nr::describe(resource);
        // The render graph is <=2048x1200 at this engine's VV factors (844x475 at
        // 2560x1440, 1689x903 at the user's current 66% setting); display-sized back
        // buffers, 1024x2048 atlases and 1937x333 UI strips fall outside. The
        // swap-chain test is the exact one when the chain is already known.
        bool const renderSized = info.width <= 2048 && info.height <= 1200;
        // Exclude square textures (UI atlases are 64/128/256/512/1024 squares) so
        // the ring is not flooded and the render graph's non-square colour stays in
        // it. The real graph at this engine is 1689x950 / 844x475. Also drop anything
        // below display/4 (mip chains, helper maps): those are what eventually pushed
        // the render-sized colour out of a full ring on 2026-10-25.
        bool const bigEnough = !tooSmallForRenderGraph(static_cast<unsigned>(info.width), static_cast<unsigned>(info.height));
        if (renderSized && bigEnough && info.width != info.height && !isSwapChainBuffer(resource)) {
            if (gHeldCount == kHeldSlots) {
                releaseComObject(gHeld[gHeldNext].resource);
            } else {
                ++gHeldCount;
            }
            addRefComObject(resource);
            gHeld[gHeldNext] = HeldRtv{
                resource,
                static_cast<unsigned>(info.width),
                static_cast<unsigned>(info.height),
                info.format
            };
            gHeldNext = (gHeldNext + 1) % kHeldSlots;
        }
        utils::logger().info(
            "[vv] rtv res={:#x} {}x{} fmt={} flags={:#x}",
            reinterpret_cast<std::uintptr_t>(resource),
            static_cast<unsigned>(info.width),
            static_cast<unsigned>(info.height),
            info.format,
            info.flags
        );
    }
    gOriginalCreateRtv(device, resource, desc, handle);
}

// The depth side of the frame graph: every depth texture is bound through a
// depth-stencil view, so CreateRenderTargetView never sees it. Keeping a small
// reference-counted ring of DSV resources is what makes the main depth reachable
// from the SR pipeline (2026-10-25: the RTV inventory had the colour but no 43/44).
void createDsvDetour(void* device, void* resource, void const* desc, std::uintptr_t handle) {
    if (resource && !inject::recordingOwnView()) {
        bool known = false;
        for (unsigned i = 0; i < gHeldDepthCount; ++i) {
            if (gHeldDepth[i].resource == resource) {
                known = true;
                break;
            }
        }
        if (!known) {
            auto const info = nr::describe(resource);
            if (info.width && info.height && info.width <= 4096 && info.height <= 4096
                && !tooSmallForRenderGraph(static_cast<unsigned>(info.width), static_cast<unsigned>(info.height))) {
                if (gHeldDepthCount == kHeldDepthSlots) {
                    auto const& victim = gHeldDepth[gHeldDepthNext];
                    // A large depth leaving the ring means the SR input can disappear
                    // mid-session; say so instead of failing silently later.
                    if (!tooSmallForRenderGraph(victim.width, victim.height)) {
                        utils::logger().warn(
                            "[vv] depth ring ({} slots) is evicting a {}x{} depth - the SR input may vanish",
                            kHeldDepthSlots,
                            victim.width,
                            victim.height
                        );
                    }
                    releaseComObject(victim.resource);
                } else {
                    ++gHeldDepthCount;
                }
                addRefComObject(resource);
                gHeldDepth[gHeldDepthNext] = HeldDepth{
                    resource,
                    static_cast<unsigned>(info.width),
                    static_cast<unsigned>(info.height),
                    info.format
                };
                gHeldDepthNext = (gHeldDepthNext + 1) % kHeldDepthSlots;
                utils::logger().info(
                    "[vv] dsv res={:#x} {}x{} fmt={}",
                    reinterpret_cast<std::uintptr_t>(resource),
                    static_cast<unsigned>(info.width),
                    static_cast<unsigned>(info.height),
                    info.format
                );
            }
        }
    }
    gOriginalCreateDsv(device, resource, desc, handle);
}

extern std::atomic<unsigned> gArg10ProbeFrames;
extern std::atomic<bool>     gArg10SolidArmed;
extern std::atomic<unsigned> gArg10ArmGeneration;

// Declared below in this translation unit; the bind-time arg10 proof inside the
// OMRT detour needs them before their definitions.
extern std::atomic<void*>    gArg10Native;
extern std::atomic<unsigned> gArg10DeclaredSerial;
extern std::atomic<unsigned> gDrawTagProbeFrames;
extern std::atomic<void*>    gMotionVectorNative;
extern std::atomic<bool>     gDrawSuppressArmed;
extern std::atomic<bool>     gLastInjectionOk;
extern std::atomic<unsigned> gDrawSuppressSkips;
extern std::atomic<unsigned> gInjectedGeneration;

void omSetRtvDetour(void* list, unsigned count, std::uintptr_t const* rtvs, int single, std::uintptr_t const* dsv) {
    // Pure forward: nothing here changes an argument or a state. What it adds is the
    // one fact the frame does not expose anywhere else - which list binds a back
    // buffer. The frame's world composite is the first such list (B1c, 2026-10-25).
    if (list && rtvs && count) {
        static std::atomic<unsigned> calls{0};
        unsigned const               bbCount   = gBackBufferHandleCount.load(std::memory_order_relaxed);
        unsigned const               increment = gDescriptorIncrement.load(std::memory_order_relaxed);
        unsigned const               limit     = count > 8 ? 8 : count;
        bool                         touches   = false;
        for (unsigned i = 0; i < limit && !touches; ++i) {
            std::uintptr_t const handle =
                (single && increment) ? rtvs[0] + static_cast<std::uintptr_t>(i) * increment : rtvs[i];
            for (unsigned b = 0; b < bbCount && b < kBackBufferHandles; ++b) {
                if (handle == gBackBufferHandles[b].load(std::memory_order_relaxed)) {
                    touches = true;
                    break;
                }
            }
        }
        if (touches) {
            markBound(list);
        }
        if (auto* const sig = sigFor(list)) {
            sig->dsv |= dsv ? 1u : 0u;
            bool const     probe = gArg10ProbeFrames.load(std::memory_order_relaxed) > 0;
            bool const     solid = gArg10SolidArmed.load(std::memory_order_relaxed);
            unsigned const gen   = gArg10ArmGeneration.load(std::memory_order_relaxed);
            if (probe || solid) {
                if (sig->rtvSeenGen != gen) {
                    sig->rtvSeenCount = 0;
                    sig->rtvSeenGen   = gen;
                    sig->arg10Bound   = nullptr;
                }
                void* const native = solid ? gArg10Native.load(std::memory_order_acquire) : nullptr;
                unsigned const limit = count > 16 ? 16 : count;
                for (unsigned i = 0; i < limit; ++i) {
                    std::uintptr_t const handle = (single && increment)
                        ? rtvs[0] + static_cast<std::uintptr_t>(i) * increment
                        : rtvs[i];
                    bool known = false;
                    for (unsigned s = 0; s < sig->rtvSeenCount; ++s) {
                        if (sig->rtvSeen[s] == handle) {
                            known = true;
                        }
                    }
                    if (!known && sig->rtvSeenCount < std::size(sig->rtvSeen)) {
                        sig->rtvSeen[sig->rtvSeenCount++] = handle;
                    }
                    // Bind-time proof: this list bound the current arg10 native. Cheap
                    // because lookupRtv is a write-through cache; tagged with the frame's
                    // declaration serial so a stale buffer cannot match next frame.
                    if (native && lookupRtv(handle) == native) {
                        sig->arg10Bound       = native;
                        sig->arg10BoundSerial = gArg10DeclaredSerial.load(std::memory_order_acquire);
                    }
                }
                // The probe summary still reports the first binding (that is what
                // EXP-0055 logged); the writer matcher scans every handle above.
                if (sig->rtvCalls == 0 || (solid && sig->rtvGen != gen)) {
                    sig->rtvHandle   = rtvs[0];
                    sig->rtvResource = lookupRtv(rtvs[0]);
                    sig->rtvGen      = gen;
                }
            }
            ++sig->rtvCalls;
        }
        unsigned const n = calls.fetch_add(1, std::memory_order_relaxed) + 1;
        if (n <= 2) {
            // Shape check for the slot itself: a real OMSetRenderTargets arrives with
            // count 1..8, a small single flag and a valid descriptor handle.
            utils::logger().info(
                "[vv] om rt #{} list={:#x} count={} single={} handle0={:#x} bbHandles={} increment={}",
                n,
                reinterpret_cast<std::uintptr_t>(list),
                count,
                single,
                static_cast<std::uintptr_t>(rtvs[0]),
                bbCount,
                increment
            );
        }
    }
    gOriginalOmSetRtv(list, count, rtvs, single, dsv);
}


// ---------------------------------------------------------------------------
// Resource creation: every texture, not just the render targets
// ---------------------------------------------------------------------------
//
// The render-target inventory was enough for the colour and the normals but not for
// the motion vectors or the depth (2026-10-25): those are bgfx transients and depth
// views, and never get a render-target view. Hooking the two creation entry points
// catches everything the engine (and bgfx) builds - and a resize rebuilds all of it,
// so the inventory is complete right after one.
//
// Both detours forward immediately; the only added work is one GetDesc per created
// texture, filtered to 64x64 and up.

using CreateCommittedFn = long (*)(
    void*       device,
    void const* heapProperties,
    unsigned    heapFlags,
    void const* desc,
    unsigned    initialState,
    void const* optimizedClearValue,
    void const* riidResource,
    void**      resource
);
using CreatePlacedFn = long (*)(
    void*              device,
    void*              heap,
    unsigned long long heapOffset,
    void const*        desc,
    unsigned           initialState,
    void const*        optimizedClearValue,
    void const*        riidResource,
    void**             resource
);

CreateCommittedFn gOriginalCommitted{};
CreatePlacedFn    gOriginalPlaced{};
void*             gHookedCreateTargets[2]{};
unsigned          gHookedCreateCount = 0;

struct TextureRecord {
    void*    resource;
    unsigned width;
    unsigned height;
    unsigned format;
    unsigned source; // 1 = committed, 2 = placed
};
constexpr unsigned kTextureSlots = 96; // retained references; see recordTexture
TextureRecord gTextures[kTextureSlots]{};
unsigned      gTextureCount = 0;

void recordTexture(void* resource, unsigned source) {
    if (!resource) {
        return;
    }
    auto const info = nr::describe(resource);
    if (info.dimension != 3 || info.width < 64 || info.height < 64 || info.mips > 2) {
        return;
    }
    // Only the FIRST few textures after a resize are kept, and a resize clears the
    // list (onSwapChainResize). The engine creates its render graph in one burst
    // right after the swap-chain rebuild; holding a reference to every texture it
    // ever creates (thousands of atlases in a session) is both a VRAM leak and a
    // way to make the next rebuild fail (2026-10-25 03:14 abort).
    for (unsigned i = 0; i < gTextureCount; ++i) {
        if (gTextures[i].resource == resource) {
            return;
        }
    }
    if (gTextureCount >= kTextureSlots) {
        return;
    }
    // Hold a reference so a later rebuild cannot free the texture out from under a
    // dump. Bounded by kTextureSlots and dropped on resize.
    addRefComObject(resource);
    gTextures[gTextureCount++] = TextureRecord{
        resource,
        static_cast<unsigned>(info.width),
        static_cast<unsigned>(info.height),
        info.format,
        source
    };
    utils::logger().info(
        "[vv] tex res={:#x} {}x{} fmt={} src={}",
        reinterpret_cast<std::uintptr_t>(resource),
        static_cast<unsigned>(info.width),
        static_cast<unsigned>(info.height),
        info.format,
        source == 1 ? "committed" : "placed"
    );
}

// A swap-chain resize (F11 / window size) frees the whole render graph. Release
// our references and drop the raw-pointer inventories before ResizeBuffers runs:
// keeping the old generation alive is how the 03:14 abort was produced.
void resetTextureInventory() {
    for (unsigned i = 0; i < gTextureCount; ++i) {
        releaseComObject(gTextures[i].resource);
        gTextures[i] = TextureRecord{};
    }
    gTextureCount = 0;
    // gSeen / gRtvs are raw pointers and are intentionally kept: the new graph's
    // RTVs can be created just before this hook runs (see gSeen).
}

long createCommittedDetour(
    void*       device,
    void const* heapProperties,
    unsigned    heapFlags,
    void const* desc,
    unsigned    initialState,
    void const* optimizedClearValue,
    void const* riidResource,
    void**      resource
) {
    long const result = gOriginalCommitted(
        device,
        heapProperties,
        heapFlags,
        desc,
        initialState,
        optimizedClearValue,
        riidResource,
        resource
    );
    if (result >= 0 && resource && *resource) {
        recordTexture(*resource, 1);
    }
    return result;
}

long createPlacedDetour(
    void*              device,
    void*              heap,
    unsigned long long heapOffset,
    void const*        desc,
    unsigned           initialState,
    void const*        optimizedClearValue,
    void const*        riidResource,
    void**             resource
) {
    long const result = gOriginalPlaced(
        device,
        heap,
        heapOffset,
        desc,
        initialState,
        optimizedClearValue,
        riidResource,
        resource
    );
    if (result >= 0 && resource && *resource) {
        recordTexture(*resource, 2);
    }
    return result;
}

void hookDevice(void* device) {
    if (!device) {
        return;
    }
    // Every device is reported, and every *distinct* CreateRenderTargetView target is
    // hooked: the overlay creates a throw-away device for the swap-chain vtable, and
    // latching on the first device seen (that one) is why the engine's own render
    // targets never showed up.
    utils::logger().info("[vv] device created {:#x}", reinterpret_cast<std::uintptr_t>(device));
    if (!gEngineDevice.load(std::memory_order_relaxed)) {
        gEngineDevice.store(device, std::memory_order_relaxed);
    }
    // Needed when the engine passes a single RTV handle for a contiguous range; the
    // content boundary computes the i-th handle from it.
    gDescriptorIncrement.store(rtvDescriptorIncrement(device), std::memory_order_relaxed);
    auto* const* vtbl = *reinterpret_cast<void* const* const*>(device);
    if (!vtbl || !readable(vtbl, 30 * sizeof(void*))) {
        utils::logger().warn("[vv] device vtable unreadable; render targets will not resolve");
        return;
    }
    auto* const target = const_cast<void*>(vtbl[20]);
    for (unsigned i = 0; i < kHookedVtables; ++i) {
        if (gHookedRtvTargets[i] == target) {
            return;
        }
    }
    if (MH_CreateHook(target, reinterpret_cast<void*>(&createRtvDetour), reinterpret_cast<void**>(&gOriginalCreateRtv))
            == MH_OK
        && MH_EnableHook(target) == MH_OK) {
        if (gHookCount < kHookedVtables) {
            gHookedRtvTargets[gHookCount++] = target;
        }
        utils::logger().info(
            "[vv] CreateRenderTargetView hooked on device {:#x} (target {:#x})",
            reinterpret_cast<std::uintptr_t>(device),
            reinterpret_cast<std::uintptr_t>(target)
        );
    } else {
        utils::logger().warn("[vv] could not hook CreateRenderTargetView for device {:#x}", reinterpret_cast<std::uintptr_t>(device));
    }

    // Depth-stencil views: device vtable slot 21 (CreateRenderTargetView is 20, and
    // both follow the SDK header's declaration order).
    auto* const dsvTarget = const_cast<void*>(vtbl[21]);
    static void* hookedDsv[4]{};
    static unsigned dsvHooks = 0;
    bool dsvHooked = false;
    for (unsigned i = 0; i < dsvHooks; ++i) {
        if (hookedDsv[i] == dsvTarget) {
            dsvHooked = true;
        }
    }
    if (!dsvHooked
        && MH_CreateHook(dsvTarget, reinterpret_cast<void*>(&createDsvDetour), reinterpret_cast<void**>(&gOriginalCreateDsv))
               == MH_OK
        && MH_EnableHook(dsvTarget) == MH_OK) {
        if (dsvHooks < 4) {
            hookedDsv[dsvHooks++] = dsvTarget;
        }
        utils::logger().info(
            "[vv] CreateDepthStencilView hooked on device {:#x} (target {:#x})",
            reinterpret_cast<std::uintptr_t>(device),
            reinterpret_cast<std::uintptr_t>(dsvTarget)
        );
    } else if (!dsvHooked) {
        utils::logger().warn("[vv] could not hook CreateDepthStencilView for device {:#x}", reinterpret_cast<std::uintptr_t>(device));
    }

    // Resource creation: slots 27 (CreateCommittedResource) and 29
    // (CreatePlacedResource). Slot 20 is proven, and these are core device methods that
    // have not moved since D3D12 shipped; if a future runtime reorders them the first
    // call faults, which is why the probe logs the hook result instead of assuming it.
    auto* const committedTarget = const_cast<void*>(vtbl[27]);
    auto* const placedTarget    = const_cast<void*>(vtbl[29]);
    bool        committedHooked = false;
    bool        placedHooked    = false;
    for (unsigned i = 0; i < gHookedCreateCount; ++i) {
        if (gHookedCreateTargets[i] == committedTarget) {
            committedHooked = true;
        }
        if (gHookedCreateTargets[i] == placedTarget) {
            placedHooked = true;
        }
    }
    if (!committedHooked
        && MH_CreateHook(
               committedTarget,
               reinterpret_cast<void*>(&createCommittedDetour),
               reinterpret_cast<void**>(&gOriginalCommitted)
           )
               == MH_OK
        && MH_EnableHook(committedTarget) == MH_OK) {
        if (gHookedCreateCount < 2) {
            gHookedCreateTargets[gHookedCreateCount++] = committedTarget;
        }
        committedHooked = true;
        utils::logger().info("[vv] CreateCommittedResource hooked");
    } else if (!committedHooked) {
        utils::logger().warn("[vv] CreateCommittedResource not hooked; textures stay partial");
    }
    if (!placedHooked
        && MH_CreateHook(
               placedTarget,
               reinterpret_cast<void*>(&createPlacedDetour),
               reinterpret_cast<void**>(&gOriginalPlaced)
           )
               == MH_OK
        && MH_EnableHook(placedTarget) == MH_OK) {
        if (gHookedCreateCount < 2) {
            gHookedCreateTargets[gHookedCreateCount++] = placedTarget;
        }
        placedHooked = true;
        utils::logger().info("[vv] CreatePlacedResource hooked");
    } else if (!placedHooked) {
        utils::logger().warn("[vv] CreatePlacedResource not hooked; textures stay partial");
    }
}

// Stage D dry run: is this draw recorded into the list that bound the current arg10?
// If yes, the fullscreen TAAU triangle can be identified at draw time by the same
// bind-time proof the writer anchor uses, and only that one draw has to be skipped -
// the material binding (the MV role channel) would still run. Logs only; nothing is
// dropped while the probe window is armed.
// Is this list the current frame's arg10 writer, by the bind-time proof? `firstDraw`
// additionally requires that no draw has been recorded into it yet - the TAAU quad is
// the first draw of its list (EXP-0066).
bool inArg10WriterList(void* list, bool firstDraw) {
    auto* const sig    = sigFor(list);
    void* const native = gArg10Native.load(std::memory_order_acquire);
    if (!sig || !native) {
        return false;
    }
    if (firstDraw && sig->draws != 0) {
        return false;
    }
    return sig->arg10Bound == native
        && sig->arg10BoundSerial == gArg10DeclaredSerial.load(std::memory_order_acquire);
}

void probeDrawTag(void* list, char const* kind, unsigned a, unsigned b) {
    if (gDrawTagProbeFrames.load(std::memory_order_relaxed) == 0) {
        return;
    }
    auto* const sig    = sigFor(list);
    void* const native = gArg10Native.load(std::memory_order_acquire);
    if (!sig || !native) {
        return;
    }
    bool const exact = inArg10WriterList(list, false);
    if (!exact && !sigBindsResource(sig, native)) {
        return;
    }
    static std::atomic<unsigned> logs{0};
    unsigned const               n = logs.fetch_add(1, std::memory_order_relaxed) + 1;
    if (n <= 32 || n % 120 == 0) {
        utils::logger().info(
            "[vv] d-probe: draw in arg10 list kind={} a={} b={} exact={} draws={}",
            kind,
            a,
            b,
            exact ? 1 : 0,
            sig->draws
        );
    }
}

void drawInstancedDetour(void* list, unsigned vertexCount, unsigned instanceCount, unsigned startVertex, unsigned startInstance) {
    markDrew(list);
    probeDrawTag(list, "inst", vertexCount, instanceCount);
    if (auto* const sig = sigFor(list)) {
        if (sig->draws++ == 0) {
            sig->firstVerts = vertexCount;
            sig->firstInst  = instanceCount;
        }
    }
    auto const n = gDrawInstancedCalls.fetch_add(1, std::memory_order_relaxed) + 1;
    if (n <= 8) {
        utils::logger().info(
            "[vv] slot12 raw #{} this={:#x} vertices={} instances={} startVertex={} startInstance={}",
            n,
            reinterpret_cast<std::uintptr_t>(list),
            vertexCount,
            instanceCount,
            startVertex,
            startInstance
        );
    }
    gOriginalDrawInstanced(list, vertexCount, instanceCount, startVertex, startInstance);
}

// Pure forward. Only used while the content boundary is being established: the
// composite and the UI are the lists that draw at the display resolution, the world
// passes do not, so the per-list viewport is the signature that survives even when
// the back buffers' render-target views were created before the mod loaded.
void rssSetViewportsDetour(void* list, unsigned count, void const* viewports) {
    if (list && viewports && count) {
        // D3D12_VIEWPORT = { TopLeftX, TopLeftY, Width, Height, MinDepth, MaxDepth }
        auto const* values = static_cast<float const*>(viewports);
        noteViewport(list, static_cast<unsigned>(values[2] + 0.5f), static_cast<unsigned>(values[3] + 0.5f));
    }
    gOriginalSetViewports(list, count, viewports);
}

void drawIndexedInstancedDetour(void* list, unsigned indexCount, unsigned instanceCount, unsigned startIndex, int baseVertex, unsigned startInstance) {
    markDrew(list);
    probeDrawTag(list, "indexed", indexCount, instanceCount);
    // Stage D: skip exactly the TAAU quad (the first 6x1 indexed draw of the list that
    // already bound this frame's arg10), and only while the last engine frame really
    // injected. The callback, its material binds, the arg10 writer binding and the MV
    // capture all stay; only the GPU draw leaves the submission.
    //
    // There is deliberately no world gate: since the user's 2026-10-28 decision the
    // pipeline keeps running after leaving a save, and the menu background is processed
    // by the same DLSS -> arg10 path (EXP-0068 showed what happens when the skip runs
    // but the injection is a no-op: the background freezes on the last world frame).
    // `arg10SolidArmed` makes Stop/Start take effect immediately; `gLastInjectionOk`
    // restores TAAU on the frame after any failed injection; the generation and size
    // checks keep a resize frame on TAAU until a correctly sized injection exists
    // (rapid F11 used to leave a stale/truncated arg10, EXP-0075).
    bool skipped = false;
    if (indexCount == 6 && instanceCount == 1
        && gDrawSuppressArmed.load(std::memory_order_acquire)
        && gArg10SolidArmed.load(std::memory_order_acquire)
        && gLastInjectionOk.load(std::memory_order_acquire)
        && gInjectedGeneration.load(std::memory_order_acquire)
               == gRenderGeneration.load(std::memory_order_relaxed)
        && inArg10WriterList(list, true)
        && gMotionVectorNative.load(std::memory_order_acquire)) {
        unsigned const n = gDrawSuppressSkips.fetch_add(1, std::memory_order_relaxed) + 1;
        if (n <= 8 || n % 600 == 0) {
            utils::logger().info(
                "[vv] d-suppress: skipped TAAU draw #{} list={:#x} mv={:#x}",
                n,
                reinterpret_cast<std::uintptr_t>(list),
                reinterpret_cast<std::uintptr_t>(gMotionVectorNative.load(std::memory_order_acquire))
            );
        }
        skipped = true;
    }
    if (auto* const sig = sigFor(list)) {
        if (sig->draws++ == 0) {
            sig->firstIndices = indexCount;
        }
    }
    if (skipped) {
        return;
    }
    auto const n = gDrawIndexedCalls.fetch_add(1, std::memory_order_relaxed) + 1;
    if (n <= 8) {
        utils::logger().info(
            "[vv] slot13 raw #{} this={:#x} indices={} instances={} startIndex={} baseVertex={} startInstance={}",
            n,
            reinterpret_cast<std::uintptr_t>(list),
            indexCount,
            instanceCount,
            startIndex,
            baseVertex,
            startInstance
        );
    }
    gOriginalDrawIndexed(list, indexCount, instanceCount, startIndex, baseVertex, startInstance);
}

void hookCommandList(void* list) {
    if (!list || gListHooked.exchange(true, std::memory_order_acq_rel)) {
        return;
    }
    auto* const* vtbl = *reinterpret_cast<void* const* const*>(list);
    if (!vtbl || !readable(vtbl, 29 * sizeof(void*))) {
        utils::logger().warn("[vv] command list vtable unreadable; no pass boundaries");
        return;
    }
    // The swap also submits the overlay's own D3D11On12 lists, which have a DIFFERENT
    // vtable: hooking slot 25 on that one crashed the client (forwarding a count that
    // was really a pointer). Only the engine's lists live in the Agility runtime, so
    // that is the gate - and it has to release the latch when it does not match.
    if (moduleOf(reinterpret_cast<std::uint64_t>(vtbl[9])).find("D3D12Core") == std::string::npos) {
        gListHooked.store(false, std::memory_order_release);
        return;
    }
    // Identity, not behaviour: which vtable the engine's lists actually use, and what
    // its slots point at. Pure reads, only logged.
    utils::logger().info(
        "[vv] engine list vtable={:#x} slot9={} slot12={} slot13={} slot25={} slot28={}",
        reinterpret_cast<std::uintptr_t>(vtbl),
        moduleOf(reinterpret_cast<std::uint64_t>(vtbl[9])),
        moduleOf(reinterpret_cast<std::uint64_t>(vtbl[12])),
        moduleOf(reinterpret_cast<std::uint64_t>(vtbl[13])),
        moduleOf(reinterpret_cast<std::uint64_t>(vtbl[25])),
        moduleOf(reinterpret_cast<std::uint64_t>(vtbl[28]))
    );

    // The engine's device, reached through its own command list: it is created before
    // mods load and lives in D3D12Core.dll, so the d3d12.dll export hook above never
    // sees it. Hooking *this* device's CreateRenderTargetView is what finally makes
    // the engine's render targets visible (and a fullscreen toggle then re-creates
    // them, because they are only ever created once, at start-up).
    if (!gDeviceHooked.exchange(true, std::memory_order_acq_rel)) {
        if (void* const device = engineDeviceFromCommandList(list)) {
            utils::logger().info("[vv] engine device {:#x}", reinterpret_cast<std::uintptr_t>(device));
            // This device came from an engine submission, so it is authoritative -
            // the overlay's throw-away device may have been created first.
            gEngineDevice.store(device, std::memory_order_relaxed);
            hookDevice(device);
            // The boundary experiment records on the engine's device too; it only
            // needs to be told which one that is (docs/specs/vv.md §12 A).
            inject::setDevice(device);
        }
    }

    auto hookAt = [vtbl](unsigned index, void* detour, void** original, char const* what) {
        auto* const target = const_cast<void*>(vtbl[index]);
        bool const  ok = target && MH_CreateHook(target, detour, original) == MH_OK && MH_EnableHook(target) == MH_OK;
        utils::logger().info("[vv] command list slot {} ({}) hooked={}", index, what, ok);
        return ok;
    };
    // Every detour forwards every argument unchanged. Slot numbers come from the SDK
    // declaration order, which the working slots above already matched.
    hookAt(12, reinterpret_cast<void*>(&drawInstancedDetour), reinterpret_cast<void**>(&gOriginalDrawInstanced), "DrawInstanced");
    hookAt(13, reinterpret_cast<void*>(&drawIndexedInstancedDetour), reinterpret_cast<void**>(&gOriginalDrawIndexed), "DrawIndexedInstanced");
    hookAt(16, reinterpret_cast<void*>(&copyTextureRegionDetour), reinterpret_cast<void**>(&gOriginalCopyTextureRegion), "CopyTextureRegion");
    hookAt(17, reinterpret_cast<void*>(&copyResourceDetour), reinterpret_cast<void**>(&gOriginalCopyResource), "CopyResource");
    hookAt(46, reinterpret_cast<void*>(&omSetRtvDetour), reinterpret_cast<void**>(&gOriginalOmSetRtv), "OMSetRenderTargets");
    hookAt(21, reinterpret_cast<void*>(&rssSetViewportsDetour), reinterpret_cast<void**>(&gOriginalSetViewports), "RSSetViewports");
}

long WINAPI createDeviceDetour(void* adapter, int minFeatureLevel, void const* riid, void** device) {
    long const result = gOriginalCreateDevice(adapter, minFeatureLevel, riid, device);
    if (result >= 0 && device && *device) {
        hookDevice(*device);
    }
    return result;
}

// Guarded read: a wrong pointer must not fault the render thread. Every access in
// this file goes through it.
bool readable(void const* p, std::size_t bytes) {
    if (!p || bytes == 0) {
        return false;
    }
    MEMORY_BASIC_INFORMATION info{};
    if (VirtualQuery(p, &info, sizeof(info)) == 0) {
        return false;
    }
    if (info.State != MEM_COMMIT || (info.Protect & (PAGE_NOACCESS | PAGE_GUARD)) != 0) {
        return false;
    }
    auto const begin = reinterpret_cast<std::uintptr_t>(p);
    auto const end   = reinterpret_cast<std::uintptr_t>(info.BaseAddress) + info.RegionSize;
    return begin + bytes <= end;
}

template <class T>
bool peek(void const* base, std::size_t offset, T& out) {
    auto const p = static_cast<unsigned char const*>(base) + offset;
    if (!readable(p, sizeof(T))) {
        return false;
    }
    std::memcpy(&out, p, sizeof(T));
    return true;
}

// dragon::framerenderer::modules::UpscalingParameters, 52 bytes, then
// dragon::rendering::UpscalingData adds mCurrentJitterIdx (+52) and
// mDynamicResolutionScale (+56). Layout straight from the SDK headers
// (LeviLamina src-client/mc/external/render_dragon/...), not guessed.
struct UpscalingSnapshot {
    int          mode{-1};        // 0 Taau, 1 Bilinear, 2 Fsr1, 3 Fsr1DebugBilinear
    float        factor{0.0f};    // render/display ratio the engine wants
    bool         dynamicResolution{};
    float        dynamicOverride{};
    float        minFactor{};
    float        targetFramerate{};
    float        pixelScaling{};
    float        smoothingAlpha{};
    float        varianceBiasMin{};
    float        varianceBiasMax{};
    bool         colorClipping{};
    bool         motionVectorFiltering{};
    unsigned     jitterIndex{};
    float        dynamicResolutionScale{1.0f};
};

UpscalingSnapshot readUpscaling(void const* data) {
    UpscalingSnapshot s{};
    if (!readable(data, 60)) {
        return s;
    }
    peek(data, 0, s.mode);
    peek(data, 4, s.factor);
    peek(data, 8, s.dynamicResolution);
    peek(data, 12, s.dynamicOverride);
    peek(data, 16, s.minFactor);
    peek(data, 20, s.targetFramerate);
    peek(data, 24, s.pixelScaling);
    peek(data, 28, s.smoothingAlpha);
    peek(data, 44, s.varianceBiasMin);
    peek(data, 48, s.varianceBiasMax);
    peek(data, 42, s.colorClipping);
    peek(data, 43, s.motionVectorFiltering);
    peek(data, 52, s.jitterIndex);
    peek(data, 56, s.dynamicResolutionScale);
    return s;
}

char const* modeName(int mode) {
    switch (mode) {
    case 0: return "Taau";
    case 1: return "Bilinear";
    case 2: return "Fsr1";
    case 3: return "Fsr1DebugBilinear";
    default: return "?";
    }
}

// What a word in the payload points at, when it points at a module at all: a
// vtable (or any code/data pointer) names the module it came from, which is how a
// frame-graph object gets identified without symbols.
std::string moduleOf(std::uint64_t value) {
    auto* const p = reinterpret_cast<void*>(value);
    if (!p) {
        return {};
    }
    HMODULE module{};
    if (!GetModuleHandleExW(
            GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            reinterpret_cast<LPCWSTR>(p),
            &module
        )
        || !module) {
        return {};
    }
    wchar_t path[MAX_PATH]{};
    if (!GetModuleFileNameW(module, path, MAX_PATH)) {
        return {};
    }
    wchar_t const* name = path;
    for (wchar_t const* c = path; *c; ++c) {
        if (*c == L'\\' || *c == L'/') {
            name = c + 1;
        }
    }
    char narrow[128]{};
    std::size_t n = 0;
    for (; name[n] && n + 1 < sizeof(narrow); ++n) {
        narrow[n] = name[n] < 128 ? static_cast<char>(name[n]) : '?';
    }
    narrow[n] = 0;
    auto const offset = value - reinterpret_cast<std::uintptr_t>(module);
    char       text[192];
    std::snprintf(text, sizeof(text), "%s+%#llx", narrow, static_cast<unsigned long long>(offset));
    return text;
}

// --- EXP-0028: is there a native resource *inside* InputFinalColor? ----------------
//
// The Windows declarator's 9th argument is InputFinalColor (arg10 is Upscaling).
// The original pick took the newest fmt26 RTV at the current render size, so the question was
// upscale the engine's own colour" reduces to: does this dependency carry a native
// resource pointer the inventory already knows?
//
// Deliberately flat, unlike the removed recursive dependency scanner: four fixed regions (dep 0x40, dep+8
// node 0x200, dep+16 object 0x200, node+16 object 0x200), each read only when
// readable() says the whole block is committed. No recursion, no DFS, no budget. Every
// qword is compared *by value* against this frame's renderTargets()/depthTargets()
// (tolerance 64 = the pick path's own) and the raw gSeen list, which createRtvDetour
// appends on this same render thread. A qword that matches nothing is never
// dereferenced, described or handed to a virtual call, so a stale pointer costs a
// comparison and nothing else.
//
// Two phases, each with its own three-call budget: `decl` (the declarator, pass only
// declared) and `draw` (the draw callback, where the engine has resolved it). A
// pointer that is only filled at execution time therefore cannot show up as a false
// nativeNONE.
constexpr unsigned kInputColorNativeCalls = 3;

void scanInputColorNative(char const* phase, void* dep, std::uint64_t sequence, unsigned call) {
    RenderTargetInfo colours[48]{};
    RenderTargetInfo depths[48]{};
    unsigned const   colourCount = dep ? renderTargets(colours, 48, 64) : 0;
    unsigned const   depthCount  = dep ? depthTargets(depths, 48, 64) : 0;
    unsigned const   candidates  = colourCount + depthCount + gSeenCount;

    // Two hops at most, and only through pointers whose target block is committed.
    void* node  = nullptr;
    void* dep16 = nullptr;
    if (readable(dep, 0x18)) {
        peek(dep, 8, node);
        peek(dep, 16, dep16);
    }
    void* node16 = nullptr;
    if (readable(node, 0x18)) {
        peek(node, 16, node16);
    }

    unsigned  scanned = 0;
    unsigned  hits    = 0;
    auto const report = [&](char const* region, std::size_t offset, std::uint64_t value) {
        auto* const candidate = reinterpret_cast<void*>(value);
        for (unsigned i = 0; i < colourCount; ++i) {
            if (colours[i].resource != candidate) {
                continue;
            }
            ++hits;
            utils::logger().info(
                "[vv] in.color nativeHIT phase={} cand=colour idx={} ptr={:#x} at={}+{:#x} fmt={}",
                phase,
                i,
                value,
                region,
                offset,
                colours[i].format
            );
            return;
        }
        for (unsigned i = 0; i < depthCount; ++i) {
            if (depths[i].resource != candidate) {
                continue;
            }
            ++hits;
            utils::logger().info(
                "[vv] in.color nativeHIT phase={} cand=depth idx={} ptr={:#x} at={}+{:#x} fmt={}",
                phase,
                i,
                value,
                region,
                offset,
                depths[i].format
            );
            return;
        }
        for (unsigned i = 0; i < gSeenCount; ++i) {
            if (gSeen[i] != candidate) {
                continue;
            }
            ++hits;
            // gSeen is raw: no format here without a describe() call, and this scan
            // deliberately makes none on anything.
            utils::logger().info(
                "[vv] in.color nativeHIT phase={} cand=seen idx={} ptr={:#x} at={}+{:#x} fmt=?",
                phase,
                i,
                value,
                region,
                offset
            );
            return;
        }
    };
    // Returns the bytes actually scanned (0 = region absent or unreadable).
    auto const scan = [&](char const* region, void const* base, std::size_t bytes) {
        if (!base || !readable(base, bytes)) {
            return 0U;
        }
        for (std::size_t offset = 0; offset + 8 <= bytes; offset += 8) {
            std::uint64_t value = 0;
            if (!peek(base, offset, value)) {
                break;
            }
            ++scanned;
            if (value) {
                report(region, offset, value);
            }
        }
        return static_cast<unsigned>(bytes);
    };

    unsigned const depBytes   = scan("dep", dep, 0x40);
    unsigned const nodeBytes  = scan("node", node, 0x200);
    unsigned const dep16Bytes = scan("dep16", dep16, 0x200);
    unsigned const node16Bytes = scan("node16", node16, 0x200);

    auto const regionText = [](unsigned bytes) {
        if (!bytes) {
            return std::string("none");
        }
        char text[16]{};
        std::snprintf(text, sizeof(text), "0x%X", bytes);
        return std::string(text);
    };
    // One bounded line per call, hit or miss, so "all four regions were actually
    // walked" is readable from the log instead of inferred from the absence of hits.
    utils::logger().info(
        "[vv] in.color {} phase={} dep={} node={} dep16={} node16={} scanned={} candidates={}"
        " hits={} call={}/{} seq={} this={:#x}",
        hits ? "nativeSCAN" : "nativeNONE",
        phase,
        regionText(depBytes),
        regionText(nodeBytes),
        regionText(dep16Bytes),
        regionText(node16Bytes),
        scanned,
        candidates,
        hits,
        call,
        kInputColorNativeCalls,
        sequence,
        reinterpret_cast<std::uintptr_t>(dep)
    );
}

// --- EXP-0030: bgfx handle -> native resource, read from the engine's own table ----
//
// EXP-0029 shadowed this mapping in the probe and hooked taau's "install"; the run
// (docs/agent/evidence/bgfx-handle-native-EXP-0029.log) recorded sets=0 / nonNull=0 /
// contexts=0 while destroy counted 2021. The static pass explains the asymmetry: RVA
// 0xDF9D680 creates nothing - it stores a caller-supplied native pointer into entry+0
// and sets the "not ours to free" bit at entry+0x9B, i.e. it only adopts a texture
// some other owner already created. The entries that matter are filled by 0xDFA4EA0
// (the real create, 9 call sites) and by the two adopt paths.
//
// So the probe stops shadowing and reads the engine's table instead. The layout comes
// from the renderer's own accessors (decoded in 0xDF9D680 / 0xDF9D9F0 / 0xDF9EE00):
//
//   ctx            = *(void**)(base + 0x12006060)  // renderer singleton
//   entries        = ctx + 0xBDE170, 0x1000 inline slots of 0xB8 bytes
//   used bytes     = *(u64*)(ctx + 0xC96178)       // capacity in bytes, multiple of 0xB8
//   page directory = *(void**)(ctx + 0xC96170)     // { first block, last block, bytes }
//   block n        = 0xB8000 bytes of entries; next block in the qword at +0xB8000
//   entry+0        = ID3D12Resource*
//
// Every read goes through peek(), so a wrong ctx or a torn page turns into a reason
// string instead of a fault on the render thread. Nothing here writes engine memory,
// calls a virtual or patches code.
constexpr std::uintptr_t kTextureContextStoreRva  = 0xDFA4214; // `mov [rip+disp], rdi`
constexpr std::uintptr_t kTextureContextGlobalRva = 0x12006060;
constexpr std::uintptr_t kTextureArrayOffset      = 0xBDE170;
constexpr std::uintptr_t kTexturePageDirOffset    = 0xC96170;
constexpr std::uintptr_t kTextureUsedBytesOffset  = 0xC96178;
constexpr std::uint16_t  kHandleInvalid           = 0xFFFF;
constexpr unsigned       kTextureInlineSlots      = 0x1000;
constexpr unsigned       kTextureEntryStride      = 0xB8;
constexpr unsigned       kTextureBlockBytes       = 0xB8000;

// 7 bytes at RVA 0xDFA4214: the only store that publishes the renderer singleton. Its
// displacement is also what pins the global to RVA 0x12006060 (0xDFA421B + 7 +
// 0x04061E45), so matching the bytes validates the address this file reads ctx from.
constexpr unsigned char kTextureContextStore[] = {0x48, 0x89, 0x3D, 0x45, 0x1E, 0x06, 0x04};

// Filled once at install; ctx is resolved on the first sample because LeviLamina loads
// mods before the renderer exists, which leaves the global null at install time.
std::atomic<std::uintptr_t> gTextureModuleBase{0};
std::atomic<std::uintptr_t> gTextureContext{0};
std::atomic<bool>           gTextureLayoutOk{false};
std::atomic<std::uint64_t>  gTextureReads{0};
std::atomic<std::uint64_t>  gTextureHits{0};
std::atomic<void*>          gInputFinalColorNative{nullptr};
std::atomic<unsigned>       gInputFinalColorWidth{0};
std::atomic<unsigned>       gInputFinalColorHeight{0};
std::atomic<unsigned>       gInputFinalColorFormat{0};
std::atomic<unsigned>       gInputFinalColorRecaptureDraws{0};
std::atomic<void*>          gArg10Native{nullptr};
std::atomic<void*>          gArg10PreviousNative{nullptr};
std::atomic<unsigned>       gArg10Handle{kHandleInvalid};
std::atomic<char const*>    gArg10Reason{"notSampled"};
std::atomic<unsigned>       gArg10ProbeFrames{0};
// Stage A: when armed, every frame resolves the current arg10 native and the list
// whose first OMSetRenderTargets binds it. The injection then cuts after that list
// and writes a solid colour into arg10.
std::atomic<bool>           gArg10SolidArmed{false};
std::atomic<unsigned>       gArg10ArmGeneration{0};
std::atomic<void*>          gArg10WriterList{nullptr};
std::atomic<unsigned>       gArg10WriterSplit{0};
// One declaration per world frame. A submission is allowed to look for the arg10
// writer only when a new declaration arrived since the last submission; UI frames
// reuse the previous native, and matching it there produced false cuts at 8/25/40/43
// in 107-list frames (EXP-0059: occasional black flash).
std::atomic<unsigned>       gArg10DeclaredSerial{0};
std::atomic<unsigned>       gArg10ConsumedSerial{0};
std::atomic<bool>           gArg10FreshSubmission{false};
// A world submission that had a new declaration but no writer: the DLSS history must
// not span that gap (the camera moved while no frame was upscaled).
std::atomic<bool>           gArg10Gap{false};
// R3 stage C: arg11 (InputBufferMotionVectors) is an ExternalImage - it has no holder
// chain at dep+0x10 (EXP-0062: that field is 0x3be70000 at declaration and a
// non-pointer at draw), so it is captured where the resolved texture actually appears:
// the TAAU material's own texture binding. The draw binds exactly three textures -
// FinalColor (fmt26, render size), MV (any other render-size form) and History
// (display size, so it never enters the render-size inventory) - which is the role
// split §35.3 asks for.
std::atomic<void*>          gMotionVectorNative{nullptr};

// R3 stage D probe: the two questions that decide whether TAAU can be suppressed at
// all - does the writer list still bind arg10, and does the MV material binding still
// happen. SuppressProbeFrames skips the draw callback for exactly N frames and then
// restores it; nothing else is armed by these.
std::atomic<unsigned> gSuppressProbeFrames{0};
std::atomic<unsigned> gDrawTagProbeFrames{0};
std::atomic<unsigned> gChainProbeFrames{0};
std::atomic<bool>     gProbeFrameSuppressed{false};

// Stage D draw-level suppress: skip only the TAAU draw itself, keep the callback and
// its material binds. `gLastInjectionOk` is written by inject::prepare so a frame
// whose inputs went bad restores TAAU (its draw is not skipped) instead of leaving a
// stale arg10 on screen.
std::atomic<bool>     gDrawSuppressArmed{false};
std::atomic<bool>     gLastInjectionOk{false};
std::atomic<unsigned> gDrawSuppressSkips{0};
// Generation of the last injection that really wrote arg10. A render-graph rebuild
// (resize/F11/world enter) bumps renderGeneration(); until a frame has injected under
// the new generation the draw is NOT skipped, so a resize can never leave a stale or
// wrong-sized arg10 on screen (the rapid-F11 freeze/truncation in EXP-0075).
std::atomic<unsigned> gInjectedGeneration{0};

bool matchesBytes(void* target, unsigned char const* bytes, unsigned size) {
    return target && readable(target, size) && std::memcmp(target, bytes, size) == 0;
}

void* textureContext() {
    if (auto* const cached = reinterpret_cast<void*>(gTextureContext.load(std::memory_order_acquire))) {
        return cached;
    }
    if (!gTextureLayoutOk.load(std::memory_order_acquire)) {
        return nullptr;
    }
    std::uintptr_t const base = gTextureModuleBase.load(std::memory_order_acquire);
    if (!base) {
        return nullptr;
    }
    // The global sits in the zero-filled tail of .data and is published by 0xDFA4214
    // once the renderer singleton has been allocated.
    void* resolved = nullptr;
    if (!peek(reinterpret_cast<void*>(base + kTextureContextGlobalRva), 0, resolved) || !resolved) {
        return nullptr;
    }
    std::uintptr_t const value    = reinterpret_cast<std::uintptr_t>(resolved);
    std::uintptr_t       expected = 0;
    if (gTextureContext.compare_exchange_strong(expected, value, std::memory_order_acq_rel)) {
        utils::logger().info(
            "[vv] texture table ctx={:#x} (global rva {:#x}); handle->native is a plain read",
            value,
            kTextureContextGlobalRva
        );
    }
    return resolved;
}

// One handle -> native lookup. `reason` is the experiment's diagnosis and the only thing
// the caller branches on; the other fields are logged so a miss says *where* the walk
// stopped instead of only that it did.
struct TextureLookup {
    void*         ctx{nullptr};
    std::uint64_t usedBytes{0};
    void*         directory{nullptr};
    void*         block{nullptr};
    void*         entry{nullptr};
    void*         native{nullptr};
    char const*   reason{"noContext"};
};

TextureLookup lookupTextureNative(std::uint16_t handle) {
    TextureLookup out{};
    void* const   ctx = textureContext();
    if (!ctx) {
        return out; // reason stays noContext
    }
    out.ctx = ctx;
    gTextureReads.fetch_add(1, std::memory_order_relaxed);
    auto* const array = static_cast<unsigned char*>(ctx) + kTextureArrayOffset;
    // Read in both branches so every log line can show the table's size; only the paged
    // branch *enforces* it, because the inline block is always present.
    if (!peek(ctx, kTextureUsedBytesOffset, out.usedBytes)) {
        out.usedBytes = 0;
    }
    if (static_cast<unsigned>(handle) < kTextureInlineSlots) {
        out.entry = array + static_cast<std::size_t>(handle) * kTextureEntryStride;
    } else {
        // Paged handle: bound it against the array's used bytes before walking, which is
        // the same check the engine's own reserve() makes, so the walk stays inside the
        // table. Zero, misaligned or too small means the layout is not what we assume.
        if (out.usedBytes == 0 || out.usedBytes % kTextureEntryStride != 0
            || static_cast<std::uint64_t>(handle) * kTextureEntryStride >= out.usedBytes) {
            out.reason = "outOfRange";
            return out;
        }
        if (!peek(ctx, kTexturePageDirOffset, out.directory) || !out.directory) {
            out.reason = "noDirectory";
            return out;
        }
        if (!peek(out.directory, 0, out.block) || !out.block) {
            out.reason = "noBlock";
            return out;
        }
        // Block 0 covers handles 0x1000..0x1FFF; the next block's address is the qword
        // the engine itself follows at 0xDF9D747.
        for (unsigned n = (handle >> 12) - 1; n > 0; --n) {
            void* next = nullptr;
            if (!peek(out.block, kTextureBlockBytes, next) || !next) {
                out.block  = nullptr;
                out.reason = "noBlock";
                return out;
            }
            out.block = next;
        }
        out.entry =
            static_cast<unsigned char*>(out.block) + static_cast<std::size_t>(handle & 0xFFF) * kTextureEntryStride;
    }
    if (!peek(out.entry, 0, out.native) || !out.native) {
        out.native = nullptr;
        out.reason = "notMapped";
        return out;
    }
    out.reason = "notInRtv"; // the caller upgrades this to a hit if it is on screen
    return out;
}

bool isArg10Resource(void* resource) {
    return resource
        && (resource == gArg10Native.load(std::memory_order_acquire)
            || resource == gArg10PreviousNative.load(std::memory_order_acquire));
}

void noteArg10Copy(void* list, unsigned mask, void* peer) {
    if (gArg10ProbeFrames.load(std::memory_order_relaxed) == 0 || inject::isOwnList(list)) {
        return;
    }
    if (auto* const sig = sigFor(list)) {
        sig->arg10CopyMask |= mask;
        sig->arg10CopyPeer = peer;
    }
}

struct ProbeTextureCopyLocation {
    void*    resource;
    unsigned type;
};

void copyTextureRegionDetour(
    void*        list,
    void const*  dst,
    unsigned     dstX,
    unsigned     dstY,
    unsigned     dstZ,
    void const*  src,
    void const*  srcBox
) {
    auto const* const dstLocation = static_cast<ProbeTextureCopyLocation const*>(dst);
    auto const* const srcLocation = static_cast<ProbeTextureCopyLocation const*>(src);
    void* const       dstResource = dstLocation ? dstLocation->resource : nullptr;
    void* const       srcResource = srcLocation ? srcLocation->resource : nullptr;
    if (isArg10Resource(srcResource)) {
        noteArg10Copy(list, 1, dstResource);
    }
    if (isArg10Resource(dstResource)) {
        noteArg10Copy(list, 2, srcResource);
    }
    gOriginalCopyTextureRegion(list, dst, dstX, dstY, dstZ, src, srcBox);
}

void copyResourceDetour(void* list, void* dst, void* src) {
    if (isArg10Resource(src)) {
        noteArg10Copy(list, 4, dst);
    }
    if (isArg10Resource(dst)) {
        noteArg10Copy(list, 8, src);
    }
    gOriginalCopyResource(list, dst, src);
}

// Called from install() once the main module is known. Only the base and the layout
// fingerprint are recorded here; a build whose layout moved logs the mismatch, leaves
// the table unavailable and keeps the rest of the probe working.
void installTextureTable(std::uintptr_t base) {
    if (gTextureModuleBase.load(std::memory_order_acquire)) {
        return;
    }
    auto* const store = reinterpret_cast<void*>(base + kTextureContextStoreRva);
    if (!matchesBytes(store, kTextureContextStore, sizeof(kTextureContextStore))) {
        utils::logger().error(
            "[vv] texture table: ctx store fingerprint mismatch at {:#x} (rva {:#x});"
            " handle->native unavailable",
            reinterpret_cast<std::uintptr_t>(store),
            kTextureContextStoreRva
        );
        return;
    }
    gTextureModuleBase.store(base, std::memory_order_release);
    gTextureLayoutOk.store(true, std::memory_order_release);
    utils::logger().info(
        "[vv] texture table: layout ok (rva {:#x} -> global {:#x}); ctx resolved on first sample",
        kTextureContextStoreRva,
        kTextureContextGlobalRva
    );
}

// Read-only state only: there is no shadow table to clear and no hook to remove, but the
// cached ctx must not outlive the renderer it points into.
void uninstallTextureTable() {
    gTextureLayoutOk.store(false, std::memory_order_release);
    gTextureContext.store(0, std::memory_order_release);
    gTextureModuleBase.store(0, std::memory_order_release);
    utils::logger().info(
        "[vv] texture table released (reads={} hits={})",
        gTextureReads.load(std::memory_order_relaxed),
        gTextureHits.load(std::memory_order_relaxed)
    );
}

// EXP-0038: 0xDFF64E0 is the material tag-3 consumer reached by TAAU's
// 0xDFF5480 call. Its fourth argument is ResolvedTextureResource and 0xDFF6510
// reads the handle from resource+0. Forward all six ABI arguments unchanged.
constexpr std::uintptr_t kSetTextureRva = 0xDFF64E0;
constexpr unsigned char kSetTexturePrologue[] = {
    0x41, 0x56, 0x56, 0x57, 0x53, 0x48, 0x83, 0xEC, 0x48, 0x89, 0xC8, 0x44, 0x0F, 0xB6, 0x94, 0x24,
};
std::uintptr_t    gSetTextureTarget{};
std::atomic<bool> gSetTextureHooked{false};
std::atomic<unsigned> gSetTextureLogs{0};
thread_local bool gInTaauDraw = false;

LL_STATIC_HOOK(
    SetTextureHook,
    ll::memory::HookPriority::Normal,
    gSetTextureTarget,
    bool,
    std::uint32_t  action,
    void*          encoder,
    void*          record,
    void*          resource,
    std::uintptr_t flags,
    std::uintptr_t view
) {
    bool const probing = gArg10ProbeFrames.load(std::memory_order_relaxed) > 0;
    if (resource && (gInTaauDraw || probing)) {
        std::uint16_t handle = kHandleInvalid;
        if (peek(resource, 0, handle) && handle != kHandleInvalid) {
            TextureLookup const lookup = lookupTextureNative(handle);
            if (gInTaauDraw) {
                // Classify the bound texture by its own description instead of by whether
                // it is still in the RTV ring. A rebuild creates several hundred views,
                // and the ring holds 128: the older entries (the ones the TAAU draw binds)
                // are evicted before the draw runs. That is how the motion vectors stopped
                // being captured after the 2026-10-28 23:11 world re-entry - the resize
                // cleared gMotionVectorNative, nothing refilled it, and every SR frame was
                // skipped for the rest of the session ("no usable arg11 ... native=0x0").
                RenderTargetInfo detail{};
                bool             inRtv = false;
                if (lookup.native) {
                    auto const info = nr::describe(lookup.native);
                    detail.resource = lookup.native;
                    detail.width    = static_cast<unsigned>(info.width);
                    detail.height   = static_cast<unsigned>(info.height);
                    detail.format   = static_cast<unsigned>(info.format);
                    RenderTargetInfo ring[48]{};
                    unsigned const   count = renderTargets(ring, 48, 64);
                    for (unsigned i = 0; i < count; ++i) {
                        if (ring[i].resource == lookup.native) {
                            inRtv = true;
                            break;
                        }
                    }
                }
                bool             matched = false;
                RenderTargetInfo mvTarget{};
                // Not named `near`: <Windows.h> still defines that as a macro.
                auto const       closeEnough = [](unsigned a, unsigned b) { return (a > b ? a - b : b - a) <= 64; };
                if (detail.resource) {
                    if (detail.format == 26 && detail.width == renderWidth() && detail.height == renderHeight()) {
                        gInputFinalColorWidth.store(detail.width, std::memory_order_release);
                        gInputFinalColorHeight.store(detail.height, std::memory_order_release);
                        gInputFinalColorFormat.store(detail.format, std::memory_order_release);
                        gInputFinalColorNative.store(lookup.native, std::memory_order_release);
                        gTextureHits.fetch_add(1, std::memory_order_relaxed);
                        matched = true;
                    }
                    // R3 stage C: the TAAU draw binds exactly three textures - FinalColor
                    // (fmt26, render size), InputBufferMotionVectors (render size, any
                    // other form) and InputTAAHistory (display size, so it is never this
                    // close to the render size). The remaining render-sized entry is the MV.
                    else if (closeEnough(detail.width, renderWidth()) && closeEnough(detail.height, renderHeight())) {
                        mvTarget = detail;
                    }
                }
                if (!matched && mvTarget.resource) {
                    gMotionVectorNative.store(mvTarget.resource, std::memory_order_release);
                    static std::atomic<unsigned> mvLogs{0};
                    unsigned const               mvNo = mvLogs.fetch_add(1, std::memory_order_relaxed) + 1;
                    if (mvNo <= 6 || mvNo % 900 == 0) {
                        utils::logger().info(
                            "[vv] arg11 mv material handle=0x{:04X} native={:#x} {}x{} fmt={} "
                            "(role=InputBufferMotionVectors)",
                            handle,
                            reinterpret_cast<std::uintptr_t>(mvTarget.resource),
                            mvTarget.width,
                            mvTarget.height,
                            mvTarget.format
                        );
                    }
                }
                unsigned const n = gSetTextureLogs.fetch_add(1, std::memory_order_relaxed) + 1;
                if (n <= 24) {
                    // Role channel probe: the bind call's `record` is the material-side
                    // per-texture record; if it carries the uniform-name hash, the role
                    // (MV / FinalColor / History) becomes exact instead of inferential.
                    std::uint64_t word[6]{};
                    bool const    recordOk = readable(record, sizeof(word));
                    if (recordOk) {
                        std::memcpy(word, record, sizeof(word));
                    }
                    utils::logger().info(
                        "[vv] in.color material handle=0x{:04X} action={} native={:#x} entry={:#x} match={} rtv={}"
                        " record={:#x} flags={:#x} view={:#x}"
                        " q={:#x},{:#x},{:#x},{:#x},{:#x},{:#x}",
                        handle,
                        action,
                        reinterpret_cast<std::uintptr_t>(lookup.native),
                        reinterpret_cast<std::uintptr_t>(lookup.entry),
                        matched ? 1 : 0,
                        inRtv ? 1 : 0,
                        reinterpret_cast<std::uintptr_t>(record),
                        flags,
                        view,
                        word[0],
                        word[1],
                        word[2],
                        word[3],
                        word[4],
                        word[5]
                    );
                }
            }
            if (probing && isArg10Resource(lookup.native)) {
                utils::logger().info(
                    "[vv] arg10 material caller={:#x} handle=0x{:04X} action={} native={:#x} view={:#x} taau={}",
                    reinterpret_cast<std::uintptr_t>(_ReturnAddress()),
                    handle,
                    action,
                    reinterpret_cast<std::uintptr_t>(lookup.native),
                    view,
                    gInTaauDraw ? 1 : 0
                );
            }
        }
    }
    return origin(action, encoder, record, resource, flags, view);
}

void installSetTextureHook(std::uintptr_t base) {
    if (gSetTextureHooked.load(std::memory_order_acquire)) {
        return;
    }
    auto* const target = reinterpret_cast<void*>(base + kSetTextureRva);
    if (!matchesBytes(target, kSetTexturePrologue, sizeof(kSetTexturePrologue))) {
        utils::logger().error("[vv] material texture prologue mismatch at {:#x}; FinalColor capture disabled", reinterpret_cast<std::uintptr_t>(target));
        return;
    }
    gSetTextureTarget = reinterpret_cast<std::uintptr_t>(target);
    int const status = SetTextureHook::hook();
    if (status != 0) {
        utils::logger().error("[vv] material texture hook failed status={}", status);
        return;
    }
    gSetTextureHooked.store(true, std::memory_order_release);
    utils::logger().info("[vv] material texture hooked at {:#x} (rva {:#x})", gSetTextureTarget, kSetTextureRva);
}

void uninstallSetTextureHook() {
    if (!gSetTextureHooked.exchange(false, std::memory_order_acq_rel)) {
        return;
    }
    SetTextureHook::unhook();
}

// BindFramebufferColor<RenderContext> is opaque (the SDK header is an empty
// struct). Disassembling the shipped `taau` shows exactly what it reads: one qword
// at **+0x8** per dependency, handed to the material as the texture override value
// (docs/A §7.1 has the macOS build reading +0x10 instead - the two layouts differ,
// which is why this offset comes from the Windows disassembly and not from the Mac
// IDB). Windows 26.51 binds the dependencies differently from macOS: arg9 =
// InputFinalColor, arg10 = Upscaling, arg11 = InputBufferMotionVectors, arg12 = InputTAAHistory.
//
// The +0x8 value is a 0x20-byte dependency wrapper: +0x10/+0x18 are a shared holder
// and its control block; the holder's +0x18 points to the resolved resource (EXP-0032).
// That chain is the transient-output one (arg10). EXP-0062 showed it does not apply to
// the ExternalImage dependencies (arg9 colour / arg11 motion): at dep2+0x10 the value
// is 0x3be70000 at declaration and a non-pointer at draw, and the engine only resolves
// those through the material texture binding. The motion vectors are therefore captured
// in SetTextureHook (role split below), not here.

void taauDetour(
    void* declarator,
    void* allocator,
    void* materialManager,
    void* materialService,
    void* viewport,
    void* upscaling,
    void* camera,
    void* meshFilter,
    void* dep0,
    void* dep1,
    void* dep2,
    void* dep3
) {
    auto const index = gTaauHits.fetch_add(1, std::memory_order_relaxed) + 1;

    unsigned x = 0, y = 0, width = 0, height = 0;
    if (readable(viewport, 8)) {
        auto const* rect = static_cast<std::uint16_t const*>(viewport);
        x      = rect[0];
        y      = rect[1];
        width  = rect[2];
        height = rect[3];
    }
    auto const up = readUpscaling(upscaling);
    // P0.4: the draw callback samples these two, so both are captured on the
    // declaration side of the same frame (the declarator runs before the draw).
#ifdef TEST
    gJitterIndex.store(up.jitterIndex, std::memory_order_relaxed);
    gCameraData.store(camera, std::memory_order_relaxed);
#endif
    if (width && height && up.factor > 0.0f && up.factor <= 1.0f) {
        gDisplayWidth.store(width, std::memory_order_relaxed);
        gDisplayHeight.store(height, std::memory_order_relaxed);
        // The engine truncates (1131 x 0.33 -> 373x239, not 373x240), so this must
        // truncate too.
        gRenderWidth.store(static_cast<unsigned>(static_cast<float>(width) * up.factor), std::memory_order_relaxed);
        gRenderHeight.store(static_cast<unsigned>(static_cast<float>(height) * up.factor), std::memory_order_relaxed);
    }

    bool const verbose = index <= 4 || index % 300 == 0;
    if (verbose) {
        utils::logger().info(
            "[vv] taau #{} viewport={}x{}+{},{} mode={}({}) factor={:.4f} dynScale={:.4f} jitter={} "
            "clipping={} mvFilter={} varianceBias={:.3f}..{:.3f} minFactor={:.3f} targetFps={:.1f} "
            "pixelScaling={:.3f} smoothing={:.3f} submits={} lists={}",
            index,
            width,
            height,
            x,
            y,
            static_cast<int>(up.mode),
            modeName(up.mode),
            static_cast<double>(up.factor),
            static_cast<double>(up.dynamicResolutionScale),
            up.jitterIndex,
            up.colorClipping,
            up.motionVectorFiltering,
            static_cast<double>(up.varianceBiasMin),
            static_cast<double>(up.varianceBiasMax),
            static_cast<double>(up.minFactor),
            static_cast<double>(up.targetFramerate),
            static_cast<double>(up.pixelScaling),
            static_cast<double>(up.smoothingAlpha),
            gSubmits.load(std::memory_order_relaxed),
            gSubmittedLists.load(std::memory_order_relaxed)
        );
    }
    // EXP-0028: only while the replacement is actually armed (that is the state the
    // experiment asks about), and only the first few armed frames - after that the
    // object graph is the same one and per-frame scanning would be pure cost. The
    // dependency is kept for the draw callback, which is the phase the engine resolves
    // it in; the two phases count separately.
    if (gReplaceTaauDraw.load(std::memory_order_acquire)) {
        gInputColorDependency.store(dep0, std::memory_order_relaxed);
        static std::atomic<unsigned> declScans{0};
        unsigned const               call = declScans.fetch_add(1, std::memory_order_relaxed);
        if (call < kInputColorNativeCalls) {
            scanInputColorNative("decl", dep0, index, call + 1);
        }
    }

    gOriginalTaau(
        declarator,
        allocator,
        materialManager,
        materialService,
        viewport,
        upscaling,
        camera,
        meshFilter,
        dep0,
        dep1,
        dep2,
        dep3
    );
    if (gArg10ProbeFrames.load(std::memory_order_relaxed) > 0
        || gArg10SolidArmed.load(std::memory_order_relaxed)) {
        void*         wrapper = nullptr;
        void*         holder = nullptr;
        void*         resource = nullptr;
        std::uint16_t handle = kHandleInvalid;
        TextureLookup lookup{};
        char const*   reason = "invalidChain";
        if (peek(dep1, 0x8, wrapper) && peek(wrapper, 0x10, holder) && peek(holder, 0x18, resource)
            && peek(resource, 0, handle)) {
            if (handle != kHandleInvalid) {
                lookup = lookupTextureNative(handle);
                reason = lookup.native ? "ok" : lookup.reason;
            } else {
                reason = "invalidHandle";
            }
        }
        gArg10PreviousNative.store(gArg10Native.load(std::memory_order_acquire), std::memory_order_release);
        gArg10Native.store(lookup.native, std::memory_order_release);
        gArg10DeclaredSerial.fetch_add(1, std::memory_order_release);
        gArg10Handle.store(handle, std::memory_order_release);
        gArg10Reason.store(reason, std::memory_order_release);
    }
    // Stage D probe: one-shot walk of all four declarator dependencies. The arg10
    // (dep1) chain is known; this says whether dep2 (arg11 MV) / dep3 (history) resolve
    // here at all - which decides whether the MV channel can be made independent of
    // the suppressed draw.
    if (gChainProbeFrames.load(std::memory_order_acquire) > 0) {
        gChainProbeFrames.fetch_sub(1, std::memory_order_acq_rel);
        void* const       deps[4]  = {dep0, dep1, dep2, dep3};
        char const* const names[4] = {"arg9 colour", "arg10 upscaling", "arg11 mv", "arg12 history"};
        for (unsigned d = 0; d < 4; ++d) {
            void*         wrapper  = nullptr;
            void*         holder   = nullptr;
            void*         resource = nullptr;
            std::uint16_t handle   = kHandleInvalid;
            char const*   why      = "invalidChain";
            if (peek(deps[d], 0x8, wrapper)) {
                why = "noWrapperField";
                if (peek(wrapper, 0x10, holder)) {
                    why = "noHolder";
                    if (peek(holder, 0x18, resource)) {
                        why = "noResource";
                        if (peek(resource, 0, handle)) {
                            why = handle == kHandleInvalid ? "invalidHandle" : "resolved";
                        }
                    }
                }
            }
            void* native = nullptr;
            if (handle != kHandleInvalid) {
                native = lookupTextureNative(handle).native;
            }
            utils::logger().info(
                "[vv] d-probe: chain {} dep={:#x} wrapper={:#x} holder={:#x} resource={:#x}"
                " handle={:#x} why={} native={:#x}",
                names[d],
                reinterpret_cast<std::uintptr_t>(deps[d]),
                reinterpret_cast<std::uintptr_t>(wrapper),
                reinterpret_cast<std::uintptr_t>(holder),
                reinterpret_cast<std::uintptr_t>(resource),
                handle,
                why,
                reinterpret_cast<std::uintptr_t>(native)
            );
        }
    }
}

// The macOS decompile of taau's declarator is explicit about where the four textures
// live: every dependency is read as `*(dep + 0x10)` and stored into the pass' own
// `TAAContext`, next to the uniform names InputFinalColor / InputTAAHistory /
// InputBufferMotionVectors. In the Windows log the same field is exactly the one that
// differs per dependency (colour = a heap pointer, motion = 0x5242, history/output = 0
// while the pass is only *declared*), so the declared values are not the resolved ones.
//
// The resolved set is in the TAAContext handed to the draw callback - our hook's third
// argument - which is why the dump happens there and not in the declarator. Nothing is
// dereferenced unless a qword's own vtable is a known ID3D12Resource, so a heap block
// that happens to look like a pointer cannot fault the render thread.
void logTaaContext(unsigned frame, void const* object, unsigned words) {
    if (!object || !readable(object, static_cast<std::size_t>(words) * 8)) {
        utils::logger().warn("[vv] taa ctx not readable at {:#x}", reinterpret_cast<std::uintptr_t>(object));
        return;
    }
    auto const vtable = gResourceVtable.load(std::memory_order_relaxed);
    utils::logger().info(
        "[vv] taa ctx #{} base={:#x} words={}",
        frame,
        reinterpret_cast<std::uintptr_t>(object),
        words
    );
    for (unsigned i = 0; i < words; ++i) {
        std::uint64_t value = 0;
        if (!peek(object, static_cast<std::size_t>(i) * 8, value)) {
            return;
        }
        if (!value) {
            continue;
        }
        auto const  module = moduleOf(value);
        std::string texture;
        auto* const candidate = reinterpret_cast<void*>(value);
        if (vtable && readable(candidate, 8) && *static_cast<void* const*>(candidate) == vtable) {
            auto const info = nr::describe(candidate);
            if (info.dimension == 3 && info.width >= 16 && info.height >= 16) {
                char text[96]{};
                std::snprintf(
                    text,
                    sizeof(text),
                    " -> texture %#llx %ux%u fmt=%u",
                    static_cast<unsigned long long>(reinterpret_cast<std::uintptr_t>(candidate)),
                    static_cast<unsigned>(info.width),
                    static_cast<unsigned>(info.height),
                    info.format
                );
                texture = text;
            }
        }
        utils::logger().info(
            "[vv] taa ctx #{} +{:#x} = {:#x} mod={}{}",
            frame,
            i * 8,
            value,
            module,
            texture
        );
    }
}


// The macOS decompile of taau's draw callback settles what the motion vectors are:
// there is no MV texture. The callback uploads
//   PreviousViewProjectionMatrixUniform / CurrentViewProjectionMatrixUniform,
//   CurrentWorldOrigin / PreviousWorldOrigin, SubPixelJitterAndValidHistory
// and the shader therefore reconstructs reprojection from depth + those matrices
// (standard TAA). M6 has to do the same, which means it needs those matrices - and the
// callback's second argument (the ExecutionContext) is where they live.
//
// On macOS they sit at +520 and +584; the Windows layout is its own, so the probe scans
// the context for the two 4x4 float matrices instead of hardcoding an offset. A
// projection/view-projection matrix ends in (x, y, 0, 1) with a non-zero x/y, which is a
// cheap and reliable filter.
// The macOS decompile settles what the motion vectors are: there is no MV texture. The
// callback uploads PreviousViewProjectionMatrixUniform / CurrentViewProjectionMatrixUniform,
// CurrentWorldOrigin / PreviousWorldOrigin and SubPixelJitterAndValidHistory, so the
// shader reconstructs reprojection from depth + those matrices (standard TAA). M6 needs
// the same matrices, so find where Windows keeps them.
//
// macOS puts them at +520 / +584 of the callback's third argument; on Windows that area
// is empty, and a "last column is (x,0,0,1)" filter finds nothing (a projection matrix
// ends in (0,0,1,0)). So print the raw floats around there and read the layout off the
// numbers instead of guessing a predicate.
void dumpContextFloats(void const* object) {
    if (!object || !readable(object, 0x2C0)) {
        utils::logger().warn("[vv] ctxf: not readable");
        return;
    }
    auto const* const bytes = static_cast<unsigned char const*>(object);
    for (std::size_t offset = 0; offset + 32 <= 0x2C0; offset += 32) {
        float v[8]{};
        if (!readable(bytes + offset, sizeof(v))) {
            return;
        }
        std::memcpy(v, bytes + offset, sizeof(v));
        utils::logger().info(
            "[vv] ctxf +{:#04x}: {:+.4f} {:+.4f} {:+.4f} {:+.4f} | {:+.4f} {:+.4f} {:+.4f} {:+.4f}",
            offset,
            static_cast<double>(v[0]),
            static_cast<double>(v[1]),
            static_cast<double>(v[2]),
            static_cast<double>(v[3]),
            static_cast<double>(v[4]),
            static_cast<double>(v[5]),
            static_cast<double>(v[6]),
            static_cast<double>(v[7])
        );
    }
}

#ifdef TEST
// P0.4: one full float window, tagged by batch/sample/object. The window starts
// at 0 so an offset found later is unambiguous; 0x300 bytes covers everything
// the earlier scans ever pointed at (matrices at 0x200/0x240, jitter and origins
// are known to be nearby, not elsewhere). Raw floats, no interpretation: the
// offline pass decides which 4x4 is a view-projection and which pair is jitter.
void sampleFloatWindow(void const* object, char const* label, unsigned batch, unsigned sample) {
    if (!object || !readable(object, 0x300)) {
        utils::logger().warn(
            "[vv] ctxs b{} #{} {} unreadable (base={:#x})",
            batch,
            sample,
            label,
            reinterpret_cast<std::uintptr_t>(object)
        );
        return;
    }
    auto const* const bytes = static_cast<unsigned char const*>(object);
    LARGE_INTEGER     now{};
    QueryPerformanceCounter(&now);
    utils::logger().info(
        "[vv] ctxs b{} #{} {} base={:#x} jitter={} qpc={}",
        batch,
        sample,
        label,
        reinterpret_cast<std::uintptr_t>(object),
        gJitterIndex.load(std::memory_order_relaxed),
        now.QuadPart
    );
    for (std::size_t offset = 0; offset < 0x300; offset += 32) {
        float v[8]{};
        if (!readable(bytes + offset, sizeof(v))) {
            return;
        }
        std::memcpy(v, bytes + offset, sizeof(v));
        utils::logger().info(
            "[vv] ctxs b{} #{} {} +{:#04x}: {:+.5f} {:+.5f} {:+.5f} {:+.5f} | {:+.5f} {:+.5f} {:+.5f} {:+.5f}",
            batch,
            sample,
            label,
            offset,
            static_cast<double>(v[0]),
            static_cast<double>(v[1]),
            static_cast<double>(v[2]),
            static_cast<double>(v[3]),
            static_cast<double>(v[4]),
            static_cast<double>(v[5]),
            static_cast<double>(v[6]),
            static_cast<double>(v[7])
        );
    }
}

// The two matrices the earlier run pinned, printed as one line per sample: the
// offline pass uses them to check whether they are view-projection or projection
// only (a projection has no translation in the last row/column; a view-projection
// changes under camera rotation and, for a world-space matrix, translation too).
void samplePinnedMatrices(void const* object, unsigned batch, unsigned sample) {
    if (!object || !readable(object, 0x280)) {
        return;
    }
    float current[16]{};
    float previous[16]{};
    std::memcpy(current, static_cast<unsigned char const*>(object) + 0x200, sizeof(current));
    std::memcpy(previous, static_cast<unsigned char const*>(object) + 0x240, sizeof(previous));
    utils::logger().info(
        "[vv] ctxm b{} #{} jitter={} cur=[{:+.6f} {:+.6f} {:+.6f} {:+.6f} | {:+.6f} {:+.6f} {:+.6f} {:+.6f} | "
        "{:+.6f} {:+.6f} {:+.6f} {:+.6f} | {:+.6f} {:+.6f} {:+.6f} {:+.6f}] prev=[{:+.6f} {:+.6f} {:+.6f} {:+.6f} "
        "| {:+.6f} {:+.6f} {:+.6f} {:+.6f} | {:+.6f} {:+.6f} {:+.6f} {:+.6f} | {:+.6f} {:+.6f} {:+.6f} {:+.6f}]",
        batch,
        sample,
        gJitterIndex.load(std::memory_order_relaxed),
        static_cast<double>(current[0]),
        static_cast<double>(current[1]),
        static_cast<double>(current[2]),
        static_cast<double>(current[3]),
        static_cast<double>(current[4]),
        static_cast<double>(current[5]),
        static_cast<double>(current[6]),
        static_cast<double>(current[7]),
        static_cast<double>(current[8]),
        static_cast<double>(current[9]),
        static_cast<double>(current[10]),
        static_cast<double>(current[11]),
        static_cast<double>(current[12]),
        static_cast<double>(current[13]),
        static_cast<double>(current[14]),
        static_cast<double>(current[15]),
        static_cast<double>(previous[0]),
        static_cast<double>(previous[1]),
        static_cast<double>(previous[2]),
        static_cast<double>(previous[3]),
        static_cast<double>(previous[4]),
        static_cast<double>(previous[5]),
        static_cast<double>(previous[6]),
        static_cast<double>(previous[7]),
        static_cast<double>(previous[8]),
        static_cast<double>(previous[9]),
        static_cast<double>(previous[10]),
        static_cast<double>(previous[11]),
        static_cast<double>(previous[12]),
        static_cast<double>(previous[13]),
        static_cast<double>(previous[14]),
        static_cast<double>(previous[15])
    );
}
#endif // TEST

void drawDetour(void* self, void* context, void* third, void* fourth) {
    auto const index = gDrawHits.fetch_add(1, std::memory_order_relaxed) + 1;
    gTaaContext.store(third, std::memory_order_relaxed);
    gExecContext.store(context, std::memory_order_relaxed);
    // Snapshot the per-frame copy supplied by the declarator. The original callback
    // only reads these fields; their history is advanced upstream.
    if (third && readable(third, 0x2a0)) {
        auto const* const base = static_cast<unsigned char const*>(third);
        TaaSnapshot       snap{};
        std::memcpy(snap.current, base + 0x200, sizeof(snap.current));
        std::memcpy(snap.previous, base + 0x240, sizeof(snap.previous));
        std::memcpy(snap.camera, base + 0x280, sizeof(snap.camera));
        std::memcpy(snap.jitter, base + 0x1e4, sizeof(snap.jitter));
        gTaaSnapshot = snap;
        gTaaSnapshotValid.store(true, std::memory_order_release);
    }
    // EXP-0028 draw phase: the InputFinalColor dependency saved by the last armed
    // declarator, scanned again where the engine has actually resolved the pass.
    // Deliberately before the suppress/forward decision below - whether the original
    // callback runs does not change what this reads. Its own three-call budget, because
    // sharing the declarator's would spend it before this ever runs. The pointer may be
    // stale by now; the scan is read-only per level (readable()) and never dereferences
    // an unmatched value.
    if (void* const dep = gInputColorDependency.load(std::memory_order_relaxed)) {
        static std::atomic<unsigned> drawScans{0};
        unsigned const               call = drawScans.fetch_add(1, std::memory_order_relaxed);
        if (call < kInputColorNativeCalls) {
            scanInputColorNative("draw", dep, index, call + 1);
        }
    }
    if (index <= 12 || index % 600 == 0) {
        LARGE_INTEGER now{};
        QueryPerformanceCounter(&now);
        utils::logger().info(
            "[vv] taau draw #{} self={:#x} tid={} qpc={}",
            index,
            reinterpret_cast<std::uintptr_t>(self),
            GetCurrentThreadId(),
            now.QuadPart
        );
    }
    if (index <= 2) {
        logTaaContext(static_cast<unsigned>(index), third, 0x60);
        dumpContextFloats(third);
    }
    // P0.4 marker sampling: 12 consecutive frames, both objects, raw floats.
#ifdef TEST
    if (gCtxSamplesLeft.load(std::memory_order_relaxed) > 0
        && GetTickCount64() >= gCtxStartMs.load(std::memory_order_relaxed)) {
        unsigned const sample = gCtxSampleIndex.fetch_add(1, std::memory_order_relaxed);
        gCtxSamplesLeft.fetch_sub(1, std::memory_order_relaxed);
        unsigned const batch = gCtxBatch.load(std::memory_order_relaxed);
        sampleFloatWindow(third, "ctx", batch, sample);
        sampleFloatWindow(gCameraData.load(std::memory_order_relaxed), "cam", batch, sample);
        // The callback's second argument is the execution context. The matrices at
        // TAAContext+0x200 turned out to be identical under pure forward movement
        // (2026-10-25 03:23), so the view/VP may live here instead; sampling it is
        // cheap and only happens on an explicit request.
        sampleFloatWindow(context, "exec", batch, sample);
        samplePinnedMatrices(third, batch, sample);
    }
#endif
    bool const replace   = gReplaceTaauDraw.load(std::memory_order_acquire);
    bool const recapture = replace && gInputFinalColorRecaptureDraws.load(std::memory_order_relaxed) > 0;
    if (replace && !recapture) {
        auto const suppressed = gDrawSuppressed.fetch_add(1, std::memory_order_relaxed) + 1;
        if (suppressed <= 3 || suppressed % 600 == 0) {
            utils::logger().info(
                "[vv] taau replace declared={} callbacks={} forwarded={} suppressed={} missed={}",
                gTaauHits.load(std::memory_order_relaxed),
                gDrawHits.load(std::memory_order_relaxed),
                gDrawForwarded.load(std::memory_order_relaxed),
                suppressed,
                gReplacementMissed.load(std::memory_order_relaxed)
            );
        }
        return;
    }
    // Stage D probe: skip exactly N draw callbacks, then restore. Independent of the
    // replacement gate above - this is the diagnostic "what disappears with the draw".
    if (gSuppressProbeFrames.load(std::memory_order_acquire) > 0) {
        unsigned const left = gSuppressProbeFrames.fetch_sub(1, std::memory_order_acq_rel) - 1;
        gProbeFrameSuppressed.store(true, std::memory_order_release);
        utils::logger().info("[vv] d-probe: callback suppressed left={} draw#={}", left, index);
        return;
    }
    unsigned recaptureLeft = 0;
    if (recapture) {
        recaptureLeft = gInputFinalColorRecaptureDraws.fetch_sub(1, std::memory_order_relaxed) - 1;
        utils::logger().info("[vv] taau recapture: forwarding original draw, left={}", recaptureLeft);
    }
    gDrawForwarded.fetch_add(1, std::memory_order_relaxed);
    gInTaauDraw = true;
    gOriginalDraw(self, context, third, fourth);
    gInTaauDraw = false;
    if (recapture) {
        bool const captured = gInputFinalColorNative.load(std::memory_order_acquire)
            && gInputFinalColorWidth.load(std::memory_order_relaxed) == renderWidth()
            && gInputFinalColorHeight.load(std::memory_order_relaxed) == renderHeight();
        if (captured) {
            gInputFinalColorRecaptureDraws.store(0, std::memory_order_release);
            utils::logger().info("[vv] taau recapture: captured render-size InputFinalColor");
        } else if (recaptureLeft == 0) {
            utils::logger().warn("[vv] taau recapture: budget exhausted without a render-size match");
        }
    }
}

// One-shot: write the raw bytes of the render-resolution group to <game dir>. The
// motion vectors are UINT there, and whether those uint16 pairs are half floats or
// fixed point cannot be answered by looking at the D3D12 format - only by decoding the
// bytes both ways offline. Same for the depth: two candidates are dumped so the right
// one is picked from data, not from a guess.
// Whether the dumped file contains anything but zeros. At the main menu the world
// textures exist but are never written, so a dump there is all zeros and tells us
// nothing: the probe retries until the frame graph actually rendered (i.e. in a world).
bool dumpHasContent(wchar_t const* path) {
    HANDLE const file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return false;
    }
    SetFilePointer(file, 32, nullptr, FILE_BEGIN); // past the "NDRW" header
    unsigned char buffer[4096]{};
    DWORD         read = 0;
    ReadFile(file, buffer, static_cast<DWORD>(sizeof(buffer)), &read, nullptr);
    CloseHandle(file);
    for (DWORD i = 0; i < read; ++i) {
        if (buffer[i] != 0) {
            return true;
        }
    }
    return false;
}

// A dump is requested by creating <game dir>/vv_dump_now.txt. The probe removes the
// marker and takes one batch, which makes the capture exact: it happens in whatever
// state the game is in at that moment, instead of guessing from a frame counter (the
// automatic version burned all its batches while the world was still loading).
bool takeDumpRequest() {
    auto const marker = utils::applicationDirectory() + L"\\vv_dump_now.txt";
    std::error_code error;
    if (!std::filesystem::exists(marker, error)) {
        return false;
    }
    std::filesystem::remove(marker, error);
    return true;
}

// P0.4: arm 12 consecutive context samples. Separate from the dump marker so
// "dump now" (a still frame, for the colour/depth identity) and "sample now"
// (stand still, then walk, for matrices/jitter/origins) can be sequenced by the
// player in one run. A forward slash is a valid separator on Windows, which keeps
// the path building free of escape sequences.
#ifdef TEST
bool takeContextRequest() {
    std::filesystem::path const marker = std::filesystem::path(utils::applicationDirectory()) / L"vv_ctx_now.txt";
    std::error_code             error;
    if (!std::filesystem::exists(marker, error)) {
        return false;
    }
    std::filesystem::remove(marker, error);
    return true;
}

void armContextSample(unsigned frames, unsigned delayMs) {
    unsigned const batch = gCtxBatch.fetch_add(1, std::memory_order_relaxed) + 1;
    gCtxSampleIndex.store(0, std::memory_order_relaxed);
    gCtxSamplesLeft.store(frames, std::memory_order_relaxed);
    gCtxStartMs.store(delayMs ? GetTickCount64() + delayMs : 0, std::memory_order_relaxed);
    utils::logger().info(
        "[vv] ctxs: batch {} armed ({} frames{})",
        batch,
        frames,
        delayMs ? ", starting in 2 s - walk now" : ""
    );
}
#endif // TEST

unsigned dumpSemantics(unsigned batch) {
    auto* const device = gEngineDevice.load(std::memory_order_relaxed);
    auto* const queue  = gGameQueue.load(std::memory_order_relaxed);
    unsigned const w   = gRenderWidth.load(std::memory_order_relaxed);
    unsigned const h   = gRenderHeight.load(std::memory_order_relaxed);
    if (!device || !queue || !w || !h) {
        utils::logger().warn("[vv] semantics: no device/queue/render size yet ({}x{})", w, h);
        return 0;
    }
    auto const directory = utils::applicationDirectory();
    auto const withinTwo = [](unsigned a, unsigned b) {
        return (a > b ? a - b : b - a) <= 2;
    };

    // The render-resolution group is small (a handful of textures), and which format
    // carries what turned out to be counter-intuitive: fmt=10 is the world-space NORMALS,
    // fmt=12 is the scene COLOUR, and the motion vectors are not a render target at all
    // (2026-10-25). So dump everything at this size instead of a guessed format list and
    // let the offline tool identify them.
    unsigned written = 0;
    unsigned index   = 0;
    {
        LARGE_INTEGER now{};
        QueryPerformanceCounter(&now);
        utils::logger().info("[vv] semantics: batch {} dump start qpc={}", batch, now.QuadPart);
    }
    for (unsigned i = 0; i < gTextureCount; ++i) {
        auto const& record = gTextures[i];
        if (!withinTwo(record.width, w) || !withinTwo(record.height, h)) {
            continue;
        }
        // The pointer is part of the file name: P0.3 has to say which resource a
        // picture belongs to, and the texture log line is the only other place the
        // address appears. Without it, two same-format candidates are
        // indistinguishable offline.
        unsigned const slot = index++;
        wchar_t        name[128]{};
        std::swprintf(
            name,
            std::size(name),
            L"vv_b%u_res_%02u_p%llX_%ux%u_fmt%u.bin",
            batch,
            slot,
            static_cast<unsigned long long>(reinterpret_cast<std::uintptr_t>(record.resource)),
            w,
            h,
            record.format
        );
        auto const path = directory + L"\\" + name;
        if (dumpTextureRawAs(device, queue, record.resource, path.c_str(), DumpState::Common)) {
            ++written;
            utils::logger().info(
                "[vv] semantics: res idx={} ptr={:#x} fmt={} {}x{} src={} written",
                slot,
                reinterpret_cast<std::uintptr_t>(record.resource),
                record.format,
                record.width,
                record.height,
                record.source
            );
        }
    }
    // Fallback: the engine's render graph is often visible only through the RTV
    // inventory, not the CreateCommittedResource hook (2026-10-25 03:28: the
    // 844x475 group was created at .510, before our resize hook at .518, and the
    // creation hook never logged it). gHeld is the reference-counted, small-size
    // subset of that inventory, so describing/copying it cannot touch a freed
    // resource (the 03:4x AV was a raw gSeen pointer going into nr::describe).
    if (!written) {
        unsigned rtvIndex = 0;
        for (unsigned i = 0; i < gHeldCount && written < 32; ++i) {
            unsigned const idx = (gHeldNext + kHeldSlots - 1 - i) % kHeldSlots;
            auto const&    held = gHeld[idx];
            if (!held.resource) {
                continue;
            }
            if (!withinTwo(held.width, w) || !withinTwo(held.height, h)) {
                continue;
            }
            bool excluded = false;
            for (unsigned k = 0; k < gExcludedCount; ++k) {
                if (gExcluded[k] == held.resource) {
                    excluded = true;
                    break;
                }
            }
            if (excluded) {
                continue;
            }
            // gHeld already owns a reference; no AddRef here.
            unsigned const slot = 100 + rtvIndex++;
            wchar_t        name[128]{};
            std::swprintf(
                name,
                std::size(name),
                L"vv_b%u_res_%02u_p%llX_%ux%u_fmt%u.bin",
                batch,
                slot,
                static_cast<unsigned long long>(reinterpret_cast<std::uintptr_t>(held.resource)),
                held.width,
                held.height,
                held.format
            );
            auto const path = directory + L"\\" + name;
            bool const ok   = dumpTextureRawAs(device, queue, held.resource, path.c_str(), DumpState::Common);
            if (ok) {
                ++written;
                utils::logger().info(
                    "[vv] semantics: rtv idx={} ptr={:#x} fmt={} {}x{} written",
                    slot,
                    reinterpret_cast<std::uintptr_t>(held.resource),
                    held.format,
                    held.width,
                    held.height
                );
            }
        }
    }
    utils::logger().info("[vv] semantics: {} render-resolution target(s) dumped", written);
    if (!written) {
        utils::logger().warn(
            "[vv] semantics: no texture matches {}x{} - the engine built its render graph before the "
            "creation hook was in place; press F11 once (fullscreen toggle) to rebuild it, then F9 again",
            w,
            h
        );
    }

    // "Did it render" is answered by the files themselves: a world frame writes them, the
    // main menu leaves them empty.
    bool hasContent = false;
    for (auto const& entry : std::filesystem::directory_iterator(directory)) {
        auto const filename = entry.path().filename().wstring();
        if (filename.rfind(L"vv_", 0) != 0 || filename.find(L"_res_") == std::wstring::npos != 0) {
            continue;
        }
        if (dumpHasContent(entry.path().c_str())) {
            hasContent = true;
            break;
        }
    }
    return hasContent ? 0x1Fu : 0u;
}

void logSubmitRate(std::uint64_t presents) {
    auto const submits = gSubmits.load(std::memory_order_relaxed);
    auto const lists   = gSubmittedLists.load(std::memory_order_relaxed);
    auto const taau    = gTaauHits.load(std::memory_order_relaxed);
    auto const draws   = gDrawHits.load(std::memory_order_relaxed);
    utils::logger().info(
        "[vv] presents={} submits={} lists={} maxListsPerSubmit={} taau={} draws={} seen={} dumps={} | submits/frame={:.2f} "
        "lists/frame={:.2f} taau/frame={:.2f} draws/frame={:.2f}",
        presents,
        submits,
        lists,
        gMaxListsPerSubmit.load(std::memory_order_relaxed),
        taau,
        draws,
        gSeenCount,
        gDumpPasses,
        presents ? static_cast<double>(submits) / static_cast<double>(presents) : 0.0,
        presents ? static_cast<double>(lists) / static_cast<double>(presents) : 0.0,
        presents ? static_cast<double>(taau) / static_cast<double>(presents) : 0.0,
        presents ? static_cast<double>(draws) / static_cast<double>(presents) : 0.0
    );
}

} // namespace

// ---------------------------------------------------------------------------
// P0.1: is there an NGX session this process may use?
// ---------------------------------------------------------------------------
//
// VV has no engine DLSS evaluate of its own, so "the core is loaded" and "the
// core was initialised" are two different facts and only the second one makes a
// self-built feature legal. The engine owns the session; the probe only queries
// the core after its Init was observed, because GetCapabilityParameters on a
// core that was never initialised has no defined answer. Result is logged once
// per state change (install, world enter, and a slow poll until it flips).
#ifdef TEST
using NgxCapabilitiesFn = int (*)(void**);

void logNgxSession(char const* why) {
    HMODULE shim     = GetModuleHandleW(L"nvngx.dll");
    HMODULE core     = GetModuleHandleW(L"_nvngx.dll");
    HMODULE dlss     = GetModuleHandleW(L"nvngx_dlss.dll");
    HMODULE dlssNr   = GetModuleHandleW(L"nvngx_dlssnr.dll");
    HMODULE d3d12Core = GetModuleHandleW(L"D3D12Core.dll");
    if (!core) {
        core = shim;
    }

    unsigned long long appId = 0;
    wchar_t            dataPath[MAX_PATH]{};
    bool const         init = nr::nrEngineInitCaptured(&appId, dataPath, MAX_PATH);
    // fmt refuses non-void pointers and a wchar_t* would otherwise be printed as
    // an address; the path is only interesting as text.
    char pathUtf8[MAX_PATH * 4]{};
    if (dataPath[0]) {
        WideCharToMultiByte(CP_UTF8, 0, dataPath, -1, pathUtf8, sizeof(pathUtf8), nullptr, nullptr);
    }

    bool const ready = gSessionReady.load(std::memory_order_relaxed);
    utils::logger().info(
        "[vv] session ({}): shim={:#x} core={:#x} dlss={:#x} dlssnr={:#x} d3d12core={:#x} engineInit={} appId={:#x} "
        "dataPath={}",
        why,
        reinterpret_cast<std::uintptr_t>(shim),
        reinterpret_cast<std::uintptr_t>(core),
        reinterpret_cast<std::uintptr_t>(dlss),
        reinterpret_cast<std::uintptr_t>(dlssNr),
        reinterpret_cast<std::uintptr_t>(d3d12Core),
        init ? 1 : 0,
        appId,
        pathUtf8[0] ? pathUtf8 : "<none>"
    );

    if (!core) {
        utils::logger().warn("[vv] session: no NGX core module; DLSS-SR cannot be created here");
        return;
    }

    auto const caps     = GetProcAddress(core, "NVSDK_NGX_D3D12_GetCapabilityParameters");
    auto const create   = GetProcAddress(core, "NVSDK_NGX_D3D12_CreateFeature");
    auto const evaluate = GetProcAddress(core, "NVSDK_NGX_D3D12_EvaluateFeature");
    auto const allocate = GetProcAddress(core, "NVSDK_NGX_D3D12_AllocateParameters");
    auto const release  = GetProcAddress(core, "NVSDK_NGX_D3D12_ReleaseFeature");
    utils::logger().info(
        "[vv] session: exports caps={} create={} evaluate={} allocate={} release={}",
        caps ? 1 : 0,
        create ? 1 : 0,
        evaluate ? 1 : 0,
        allocate ? 1 : 0,
        release ? 1 : 0
    );

    if (!init) {
        utils::logger().warn(
            "[vv] session: core query skipped - the engine's NGX Init has not been observed in this process"
        );
        return;
    }
    if (!caps) {
        utils::logger().warn("[vv] session: core has no GetCapabilityParameters export");
        return;
    }
    if (ready) {
        return;
    }
    void*     params = nullptr;
    int const ret    = reinterpret_cast<NgxCapabilitiesFn>(caps)(&params);
    utils::logger().info(
        "[vv] session: GetCapabilityParameters ret=0x{:08X} params={:#x}",
        static_cast<unsigned>(ret),
        reinterpret_cast<std::uintptr_t>(params)
    );
    if (ret == 1 && params) {
        gSessionReady.store(true, std::memory_order_relaxed);
        utils::logger().info("[vv] session: usable (B0 P0.1 passes; DLSS-SR may be built on this core)");
    }
}
#endif // TEST

void noteWorldEntered() {
#ifdef TEST
    logNgxSession("world enter");
#endif
    gWorldEnteredTick.store(GetTickCount(), std::memory_order_relaxed);
    gRenderGeneration.fetch_add(1, std::memory_order_relaxed);
    utils::logger().info("[vv] semantics: world entered (B0: manual dump only; press F11, then F9)");
}

bool install() {
    if (gInstalled.load(std::memory_order_acquire)) {
        return true;
    }
    auto const base = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
    if (!base) {
        utils::logger().error("[vv] no main module; VV probe not installed");
        return false;
    }
    auto* const target = reinterpret_cast<void*>(base + kTaauRva);
    if (!readable(target, kPrologueSize)) {
        utils::logger().error("[vv] taau at {:#x} is not readable", reinterpret_cast<std::uintptr_t>(target));
        return false;
    }
    if (std::memcmp(target, kPrologue, kPrologueSize) != 0) {
        utils::logger().error(
            "[vv] taau prologue mismatch at {:#x} ({}); this build moved, refusing to hook",
            reinterpret_cast<std::uintptr_t>(target),
            kTaauRva
        );
        return false;
    }

    auto const status = MH_Initialize();
    if (status != MH_OK && status != MH_ERROR_ALREADY_INITIALIZED) {
        utils::logger().error("[vv] MinHook initialization failed ({})", static_cast<int>(status));
        return false;
    }
    if (MH_CreateHook(target, reinterpret_cast<void*>(&taauDetour), reinterpret_cast<void**>(&gOriginalTaau))
            != MH_OK
        || MH_EnableHook(target) != MH_OK) {
        utils::logger().error("[vv] could not hook taau at {:#x}", reinterpret_cast<std::uintptr_t>(target));
        return false;
    }
    gTaauTarget = target;
    gInstalled.store(true, std::memory_order_release);
    utils::logger().info("[vv] taau hooked at {:#x} (rva {:#x})", reinterpret_cast<std::uintptr_t>(target), kTaauRva);

    // Level presence for the SR gate: the same event pair the RT path uses, so the
    // flag follows every world enter/exit instead of a poll that latches once.
    {
        auto& bus          = ll::event::EventBus::getInstance();
        gJoinLevelListener = bus.emplaceListener<ll::event::client::ClientJoinLevelEvent>([](auto&) {
            gWorldLoaded.store(true, std::memory_order_relaxed);
            // TEST: the client thread is right here, so a queued option write (A2, or
            // the panel) does not have to wait for the executor queue that did not
            // deliver during startup (EXP-0018/0019). No-op when nothing is pending.
            options::applyPending();
        });
        gExitLevelListener = bus.emplaceListener<ll::event::client::ClientExitLevelEvent>([](auto&) {
            gWorldLoaded.store(false, std::memory_order_relaxed);
        });
        // The join event only fires once per world; a panel write happens while the
        // level is already running. The level tick is the client-thread heartbeat that
        // is guaranteed to keep running (the executor drains itself on it, §17.3), so
        // a queued write lands within one tick instead of waiting for a two-second
        // retry window.
        gLevelTickListener = bus.emplaceListener<ll::event::world::ClientLevelTickEvent>([](auto&) {
            options::applyPending();
        });
        utils::logger().info(
            "[vv] level listeners join={} exit={} tick={}",
            gJoinLevelListener != nullptr,
            gExitLevelListener != nullptr,
            gLevelTickListener != nullptr
        );
    }

    // Second, independent signal: the draw callback. Counted only - see the header.
    auto* const drawTarget = reinterpret_cast<void*>(base + kTaauDrawRva);
    if (readable(drawTarget, kPrologueSize) && std::memcmp(drawTarget, kPrologue, kPrologueSize) == 0) {
        if (MH_CreateHook(drawTarget, reinterpret_cast<void*>(&drawDetour), reinterpret_cast<void**>(&gOriginalDraw))
                == MH_OK
            && MH_EnableHook(drawTarget) == MH_OK) {
            gDrawTarget = drawTarget;
            utils::logger().info(
                "[vv] taau draw hooked at {:#x} (rva {:#x})",
                reinterpret_cast<std::uintptr_t>(drawTarget),
                kTaauDrawRva
            );
        }
    } else {
        utils::logger().warn("[vv] taau draw prologue mismatch at {:#x} ({})", reinterpret_cast<std::uintptr_t>(drawTarget), kTaauDrawRva);
    }

    // EXP-0030: bgfx handle -> native resource, read from the engine's table. Independent
    // of the taau hooks: if this build's layout moved, only the fingerprint fails and the
    // log says so.
    installTextureTable(base);
    installSetTextureHook(base);

    // v4: take the engine's D3D12 device early. LeviLamina loads mods before the
    // renderer is built, so the export hook is normally in place before the first
    // device exists; if it is not, the render-target map stays empty and the log says
    // so instead of the probe pretending it has textures.
    if (HMODULE const d3d12 = LoadLibraryW(L"d3d12.dll")) {
        auto* const create = reinterpret_cast<void*>(GetProcAddress(d3d12, "D3D12CreateDevice"));
        if (create
            && MH_CreateHook(
                   create,
                   reinterpret_cast<void*>(&createDeviceDetour),
                   reinterpret_cast<void**>(&gOriginalCreateDevice)
               )
                   == MH_OK
            && MH_EnableHook(create) == MH_OK) {
            utils::logger().info("[vv] D3D12CreateDevice hooked");
        } else {
            utils::logger().warn("[vv] D3D12CreateDevice not hooked; render targets stay unresolved");
        }
    }
#ifdef TEST
    // P0.1 runs at install (the modules are usually loaded by now) and again at
    // world enter / a slow poll; see logNgxSession.
    logNgxSession("install");
    utils::logger().info(
        "[vv] B0: F9 = dump now (stand still, then F2), F8 = context sample in 2 s (press, then walk); "
        "file markers vv_dump_now.txt / vv_ctx_now.txt still work"
    );
#endif
    return true;
}

void uninstall() {
    setTaauDrawReplacement(false);
    uninstallSetTextureHook();
    // EXP-0030: drop the cached renderer context so a reload cannot read through it.
    uninstallTextureTable();
    // The boundary experiment created objects on the engine's device, so it has to
    // let go of them before the probe's own hooks come down.
    inject::release();
    {
        auto& bus = ll::event::EventBus::getInstance();
        if (gJoinLevelListener) {
            bus.removeListener<ll::event::client::ClientJoinLevelEvent>(gJoinLevelListener);
            gJoinLevelListener = nullptr;
        }
        if (gExitLevelListener) {
            bus.removeListener<ll::event::client::ClientExitLevelEvent>(gExitLevelListener);
            gExitLevelListener = nullptr;
        }
        if (gLevelTickListener) {
            bus.removeListener<ll::event::world::ClientLevelTickEvent>(gLevelTickListener);
            gLevelTickListener = nullptr;
        }
        gWorldLoaded.store(false, std::memory_order_relaxed);
    }
    // gSeen holds raw pointers only (no references - see createRtvDetour); the
    // reference-counted inventories are gTextures and gHeld, released here.
    gSeenCount = 0;
    for (unsigned i = 0; i < gHeldCount; ++i) {
        releaseComObject(gHeld[i].resource);
        gHeld[i] = HeldRtv{};
    }
    gHeldCount = 0;
    gHeldNext  = 0;
    for (unsigned i = 0; i < gTextureCount; ++i) {
        releaseComObject(gTextures[i].resource);
        gTextures[i].resource = nullptr;
    }
    gTextureCount = 0;
    if (gDrawTarget) {
        MH_DisableHook(gDrawTarget);
        MH_RemoveHook(gDrawTarget);
        gDrawTarget = nullptr;
    }
    gOriginalDraw = nullptr;
    if (gTaauTarget) {
        MH_DisableHook(gTaauTarget);
        MH_RemoveHook(gTaauTarget);
        gTaauTarget = nullptr;
    }
    gOriginalTaau = nullptr;
    gInstalled.store(false, std::memory_order_release);
}

bool isInstalled() { return gInstalled.load(std::memory_order_acquire); }

void onExecuteCommandLists(void* const* lists, unsigned listCount) {
    auto const index = gSubmits.fetch_add(1, std::memory_order_relaxed) + 1;
    gSubmittedLists.fetch_add(listCount, std::memory_order_relaxed);
    // Which thread submits and how big each submission is, for the first frames
    // only: that is what tells the engine's own submissions apart from the
    // overlay's own D3D11On12 work, and whether a frame is one submission or many.
    if (index <= 24) {
        LARGE_INTEGER now{};
        QueryPerformanceCounter(&now);
        utils::logger().info("[vv] submit #{} tid={} lists={} qpc={}", index, GetCurrentThreadId(), listCount, now.QuadPart);
    }
    if (lists && listCount) {
        // The vtable is shared by every command list the device hands out, so one
        // hook covers all 48 of them (and every later frame). It must be taken from
        // the ENGINE's submission: the swap also submits the overlay's own
        // D3D11On12 list, whose vtable is a different implementation, and hooking
        // that one caught nothing but a single call - the whole reason the first
        // cut saw one Close and zero draws.
        if (listCount > 8) {
            hookCommandList(const_cast<void*>(lists[0]));
            // Content boundary (B1d). Two independent ways to find the world composite:
            //
            //  1. its measured signature (2026-10-25, 18 frame shapes from 48 to 108
            //     lists): a display-sized viewport, a depth-stencil view bound, and
            //     exactly ONE draw. `tail 4` in every one of them, but the signature is
            //     what is used, so the frame's list count does not matter. The world
            //     passes render at the render resolution; the UI lists have their own
            //     viewports or many draws;
            //  2. the back-buffer view handle, when the RTV inventory happens to hold it.
            unsigned split = 0;
            void*    found = nullptr;
            char const* via = "none";
            unsigned const marks = gMarkCount.load(std::memory_order_relaxed);
            unsigned const sigs  = gSigCount.load(std::memory_order_relaxed);
            unsigned const displayW = gDisplayWidth.load(std::memory_order_relaxed);
            unsigned const displayH = gDisplayHeight.load(std::memory_order_relaxed);
            // Not named `near`: <Windows.h> still defines that as a macro.
            auto const aboutSame = [](unsigned a, unsigned b) { return a > b ? a - b <= 2 : b - a <= 2; };
            auto const sigAt = [&](unsigned i) -> ListSig const* {
                for (unsigned s = 0; s < sigs && s < kSigSlots; ++s) {
                    if (gSigs[s].list == lists[i]) {
                        return &gSigs[s];
                    }
                }
                return nullptr;
            };
            // Stage A: the writer is the list that binds the frame's arg10 native in
            // any OMSetRenderTargets call. EXP-0057 showed that the first binding alone
            // is not enough; EXP-0060 showed that a list-level DSV test is too coarse
            // (the writer can bind depth elsewhere), so the arg10 binding is the only
            // criterion.
            bool const     arg10Armed = gArg10SolidArmed.load(std::memory_order_acquire);
            unsigned const declared   = gArg10DeclaredSerial.load(std::memory_order_acquire);
            bool const     fresh = arg10Armed && declared != gArg10ConsumedSerial.load(std::memory_order_acquire);
            if (arg10Armed) {
                void* const native = gArg10Native.load(std::memory_order_acquire);
                unsigned    writer = 0;
                if (fresh) {
                    for (unsigned i = 0; native && i < listCount; ++i) {
                        auto const* sig = sigAt(i);
                        if (sig && sigBindsArg10(sig, native, declared)) {
                            writer = i + 1;
                            break;
                        }
                    }
                }
                gArg10WriterSplit.store(writer, std::memory_order_release);
                gArg10WriterList.store(writer ? lists[writer - 1] : nullptr, std::memory_order_release);
                // Consume even on a miss: a UI submission in the same frame must not get
                // a second chance at the previous frame's native.
                gArg10ConsumedSerial.store(declared, std::memory_order_release);
            } else {
                gArg10WriterSplit.store(0, std::memory_order_release);
                gArg10WriterList.store(nullptr, std::memory_order_release);
            }
            gArg10FreshSubmission.store(fresh, std::memory_order_release);
            // Stage D probe: the submission right after a probe-suppressed draw says
            // whether anything still bound arg10 as a render target (writer), and what
            // the MV capture holds. This is the measurement the suppress decision needs.
            if (gProbeFrameSuppressed.exchange(false, std::memory_order_acq_rel)) {
                utils::logger().info(
                    "[vv] d-probe: submit lists={} arg10={:#x} reason={} writer={} fresh={} mv={:#x}",
                    listCount,
                    reinterpret_cast<std::uintptr_t>(gArg10Native.load(std::memory_order_acquire)),
                    gArg10Reason.load(std::memory_order_relaxed),
                    gArg10WriterSplit.load(std::memory_order_acquire),
                    fresh ? 1 : 0,
                    reinterpret_cast<std::uintptr_t>(gMotionVectorNative.load(std::memory_order_acquire))
                );
            }
            if (displayW && displayH && sigs) {
                if (gReplaceTaauDraw.load(std::memory_order_acquire)) {
                    // The suppressed composite no longer satisfies draws==1. Cut
                    // immediately before the display-sized, many-draw HUD list instead;
                    // unlike the back-buffer-writer fallback, this cannot cover the HUD.
                    for (unsigned i = 1; i < listCount; ++i) {
                        auto const* sig = sigAt(i);
                        if (!sig || !sig->dsv || sig->draws < 5) {
                            continue;
                        }
                        if (!aboutSame(sig->width, displayW) || !aboutSame(sig->height, displayH)) {
                            continue;
                        }
                        split = i;
                        found = lists[i - 1];
                        via   = "pre-hud";
                        break;
                    }
                } else {
                    unsigned firstMatch = 0;
                    unsigned beforeHud  = 0;
                    for (unsigned i = 0; i < listCount; ++i) {
                        auto const* sig = sigAt(i);
                        if (!sig || !sig->dsv || sig->draws != 1) {
                            continue;
                        }
                        if (!aboutSame(sig->width, displayW) || !aboutSame(sig->height, displayH)) {
                            continue;
                        }
                        if (!firstMatch) {
                            firstMatch = i + 1;
                        }
                        // The world composite is immediately followed by the HUD, which is
                        // the many-draw list. Preferring that pairing keeps a stray
                        // single-draw blit in front of the composite from being picked.
                        auto const* next = (i + 1 < listCount) ? sigAt(i + 1) : nullptr;
                        if (next && next->draws >= 5) {
                            beforeHud = i + 1;
                        }
                    }
                    if (beforeHud) {
                        split = beforeHud;
                        found = lists[beforeHud - 1];
                        via   = "signature+hud";
                    } else if (firstMatch) {
                        split = firstMatch;
                        found = lists[firstMatch - 1];
                        via   = "signature";
                    }
                }
                if (gArg10ProbeFrames.load(std::memory_order_relaxed) > 0) {
                    void* const  native = gArg10Native.load(std::memory_order_acquire);
                    unsigned     resolved = 0;
                    unsigned     unresolved = 0;
                    std::string  hits;
                    std::string  candidates;
                    std::string  copies;
                    for (unsigned i = 0; i < listCount; ++i) {
                        auto const* sig = sigAt(i);
                        if (!sig) {
                            continue;
                        }
                        if (sig->rtvHandle) {
                            if (sig->rtvResource) {
                                ++resolved;
                            } else {
                                ++unresolved;
                            }
                        }
                        if (native && sigBindsArg10(sig, native, gArg10DeclaredSerial.load(std::memory_order_acquire))) {
                            hits += " " + std::to_string(i);
                        }
                        if (sig->draws == 1 && aboutSame(sig->width, displayW) && aboutSame(sig->height, displayH)) {
                            char item[128]{};
                            std::snprintf(
                                item,
                                sizeof(item),
                                " %u:r=%p,z=%u,rt=%u",
                                i,
                                sig->rtvResource,
                                sig->dsv,
                                sig->rtvCalls
                            );
                            candidates += item;
                        }
                        if (sig->arg10CopyMask) {
                            char item[128]{};
                            std::snprintf(
                                item,
                                sizeof(item),
                                " %u:mask=%x,peer=%p",
                                i,
                                sig->arg10CopyMask,
                                sig->arg10CopyPeer
                            );
                            copies += item;
                        }
                    }
                    utils::logger().info(
                        "[vv] arg10 probe #{} native={:#x} handle={:#x} reason={} hits=[{}] candidates=[{}] copies=[{}] resolved={} unresolved={}",
                        gSigFrames.load(std::memory_order_relaxed) + 1,
                        reinterpret_cast<std::uintptr_t>(native),
                        gArg10Handle.load(std::memory_order_relaxed),
                        gArg10Reason.load(std::memory_order_relaxed),
                        hits,
                        candidates,
                        copies,
                        resolved,
                        unresolved
                    );
                    gArg10ProbeFrames.fetch_sub(1, std::memory_order_relaxed);
                }
            }
            if (!split && !gReplaceTaauDraw.load(std::memory_order_acquire)) {
                for (unsigned i = 0; i < listCount && !split; ++i) {
                    for (unsigned m = 0; m < marks && m < kMarkSlots; ++m) {
                        if (gMarks[m].list == lists[i] && gMarks[m].bound && gMarks[m].drew) {
                            split = i + 1;
                            found = lists[i];
                            via   = "handle";
                            break;
                        }
                    }
                }
            }
            gBoundarySplit.store(split, std::memory_order_relaxed);
            gBoundaryList.store(found, std::memory_order_relaxed);
            gMarkCount.store(0, std::memory_order_relaxed);
            unsigned const logs = gBoundaryLogs.fetch_add(1, std::memory_order_relaxed) + 1;
            if (logs <= 6 || (split == 0 && logs % 600 == 0) || (logs % 1200 == 0 && split)) {
                utils::logger().info(
                    "[vv] content boundary split={}/{} list={:#x} via={} marks={} sigs={}",
                    split,
                    listCount,
                    reinterpret_cast<std::uintptr_t>(found),
                    via,
                    marks,
                    sigs
                );
            }
            // Diagnostic for the fallback anchor: one compact line that describes what
            // each list did this frame (viewport, depth bound, draw shape). Logged when
            // the frame changes shape (menu 48 / world 70 / ...), plus the first frames
            // and a slow sample - the world frame is what we need, and it only appears
            // after the player has entered a world.
            unsigned const sigFrame = gSigFrames.load(std::memory_order_relaxed);
            gSigFrames.store(sigFrame + 1, std::memory_order_relaxed);
            static unsigned lastLogged = 0;
            if (sigFrame < 3 || listCount != lastLogged || sigFrame % 600 == 0) {
                lastLogged = listCount;
                std::string      line;
                unsigned const   sigs = gSigCount.load(std::memory_order_relaxed);
                for (unsigned i = 0; i < listCount; ++i) {
                    for (unsigned s = 0; s < sigs && s < kSigSlots; ++s) {
                        if (gSigs[s].list != lists[i]) {
                            continue;
                        }
                        auto const& sig = gSigs[s];
                        line += " " + std::to_string(i) + ":v" + std::to_string(sig.width) + "x"
                                + std::to_string(sig.height);
                        if (sig.dsv) {
                            line += "z";
                        }
                        line += " d" + std::to_string(sig.draws);
                        if (sig.firstVerts) {
                            line += " s" + std::to_string(sig.firstVerts) + "x" + std::to_string(sig.firstInst);
                        }
                        if (sig.firstIndices) {
                            line += " x" + std::to_string(sig.firstIndices);
                        }
                        break;
                    }
                }
                utils::logger().info("[vv] frame #{} lists={}:{}", sigFrame + 1, listCount, line);
            }
            gSigCount.store(0, std::memory_order_relaxed);
        }
        // Only the engine's own frame is big enough to matter. The frame-list sample
        // that used to be armed here (one frame in 120, 49 synchronous log lines on
        // the render thread) was removed on 2026-10-25 22:46: the shape line below
        // already prints every list's viewport/draws whenever the frame changes.
    }
    unsigned previous = gMaxListsPerSubmit.load(std::memory_order_relaxed);
    while (listCount > previous
           && !gMaxListsPerSubmit.compare_exchange_weak(
               previous,
               listCount,
               std::memory_order_relaxed,
               std::memory_order_relaxed
           )) {
    }
}

void onPresent() {
    auto const presents = gPresents.fetch_add(1, std::memory_order_relaxed) + 1;
    // First ten frames and then one line every ~10 s at 60 fps: the first frames
    // show how the counters grow, the slow line shows them settle, and neither
    // turns latest.log into a counter dump over a long session.
    if (presents <= 10 || presents % 600 == 0) {
        logSubmitRate(presents);
    }
#ifdef TEST
    // B0 hotkeys, polled here so they work regardless of the panel/input focus and
    // need no file creation while playing:
    //   F9 = dump now (stand still, then press F2 for the screenshot)
    //   F7 = two SR input dumps, 2 s and then 2 s later (EXP-0027): the pair is the
    //        freshness evidence, and the player keeps turning the camera in between.
    //   F8 / F10 / V = arm 24 consecutive context samples that START IN 2 s, so the
    //        player can press while standing and then walk/turn during the wait.
    // Three keys because a laptop may route F8 (and sometimes F10) to a media
    // action under Fn-lock; `V` is a plain key Bedrock does not bind.
    static bool f8Down  = false;
    static bool f9Down  = false;
    static bool f10Down = false;
    static bool vDown   = false;
    bool const  f8      = (GetAsyncKeyState(VK_F8) & 0x8000) != 0;
    bool const  f9      = (GetAsyncKeyState(VK_F9) & 0x8000) != 0;
    bool const  f10     = (GetAsyncKeyState(VK_F10) & 0x8000) != 0;
    bool const  v       = (GetAsyncKeyState('V') & 0x8000) != 0;
    if ((f8 && !f8Down) || (f10 && !f10Down) || (v && !vDown)) {
        utils::logger().info(
            "[vv] ctxs: hotkey {} pressed",
            (f8 && !f8Down) ? "F8" : ((f10 && !f10Down) ? "F10" : "V")
        );
        armContextSample(24, 2000);
    }
    if (f9 && !f9Down) {
        unsigned const w = gRenderWidth.load(std::memory_order_relaxed);
        unsigned const h = gRenderHeight.load(std::memory_order_relaxed);
        if (w && h) {
            unsigned const batch = gDumpBatch.fetch_add(1, std::memory_order_acq_rel);
            utils::logger().info("[vv] semantics: batch {} requested by F9", batch);
            dumpSemantics(batch);
        } else {
            utils::logger().warn("[vv] semantics: F9 pressed but the render size is unknown");
        }
    }
    f8Down  = f8;
    f9Down  = f9;
    f10Down = f10;
    vDown   = v;
    // F7: two SR input dumps 2 s apart, polled on the present thread so the panel can
    // stay closed and the timing does not depend on the UI. Edge-triggered (a held key
    // or a key repeat must not reschedule) and refused while a pair is still running;
    // it arms no context sample, so it cannot collide with F8/F10/V.
    static bool     f7Down     = false;
    static unsigned f7Stage    = 0; ///< 0 idle, 1 first dump due, 2 second dump due
    static unsigned f7FirstRun = 0;
    static DWORD    f7Due      = 0;
    bool const      f7         = (GetAsyncKeyState(VK_F7) & 0x8000) != 0;
    if (f7 && !f7Down) {
        if (f7Stage == 0) {
            f7Stage = 1;
            f7Due   = ::GetTickCount() + 2000;
            utils::logger().info("[vvsr] sr dump scheduled: F7 pair, first in 2 s, second 2 s later");
        } else {
            utils::logger().warn("[vvsr] sr dump pair already running (stage={}); F7 ignored", f7Stage);
        }
    }
    // Signed difference: GetTickCount wraps every 49 days and a plain `<` would then
    // wait for the next wrap.
    if (f7Stage != 0 && static_cast<std::int32_t>(::GetTickCount() - f7Due) >= 0) {
        unsigned const before = ::neurodragon::vv::sr::dumpRuns();
        bool const     ok     = ::neurodragon::vv::sr::dumpInputs();
        unsigned const last   = ::neurodragon::vv::sr::dumpRuns();
        unsigned const run    = last != before ? last : 0; // 0 = the dump never started
        if (f7Stage == 1) {
            f7FirstRun = run;
            utils::logger().info("[vvsr] sr dump pair 1/2 run={} ok={}", run, ok ? 1 : 0);
            f7Stage = 2;
            f7Due   = ::GetTickCount() + 2000;
        } else {
            utils::logger().info(
                "[vvsr] sr dump pair 2/2 run={} ok={} (pair done runs={},{})",
                run,
                ok ? 1 : 0,
                f7FirstRun,
                run
            );
            f7Stage = 0;
        }
    }
    f7Down = f7;
#endif
    // One shot, a few seconds in: by then the frame graph has been rebuilt at least
    // once (the render targets only become visible after a rebuild, see the header).
    // The render-target inventory is only complete after the engine rebuilt its
    // resources (one fullscreen toggle), so the dump waits for it instead of firing at
    // a fixed frame: three or more render targets besides the back buffers means the
    // rebuild has happened.
    // The automatic dump is disabled: the crash trace of 2026-10-24 22:58 shows
    // dumpTextureBmp copying a render target while the engine was replacing it for a
    // fullscreen toggle - nvwgf2umx AV at test byte [r10+0x3C]. A texture copy has to
    // happen when nothing can resize, so the dump is only ever started explicitly
    // (the B0 manual path) once a trigger that cannot race a resize exists.
    // One shot, as soon as the inventory actually contains the render-resolution
    // colour (fmt=10) and nothing has been created for three seconds. The engine only
    // creates those views when it rebuilds the render graph, so this waits for a real
    // rebuild instead of firing at a fixed frame - and it never fires while a rebuild
    // is still in progress.
    // All five candidates captured once = done. The inventory can be incomplete right
    // after a resize (only part of the render-resolution group gets a new view), so this
    // keeps retrying instead of stopping at the first partial success.
    // World-entered trigger: one batch, three seconds after the join event (the world is
    // still streaming in before that).
    // Automatic world-enter dump is disabled for B0: it runs a synchronous GPU copy
    // on the present thread three seconds after the join, which can race a
    // fullscreen toggle / render-graph rebuild (the 2026-10-24 22:58 driver AV).
    // The manual path (F9 / panel button) is taken after the rebuild has settled.
    gWorldEnteredTick.store(0, std::memory_order_relaxed);
    if (presents % 30 == 0 && takeDumpRequest()) {
        unsigned const w = gRenderWidth.load(std::memory_order_relaxed);
        unsigned const h = gRenderHeight.load(std::memory_order_relaxed);
        if (w && h) {
            unsigned const batch = gDumpBatch.fetch_add(1, std::memory_order_acq_rel);
            if (dumpSemantics(batch)) {
                utils::logger().info("[vv] semantics: batch {} captured (requested)", batch);
            } else {
                utils::logger().warn("[vv] semantics: batch {} empty", batch);
            }
        } else {
            utils::logger().warn("[vv] semantics: requested but the render size is unknown");
        }
    }
#ifdef TEST
    // P0.4 marker: arm 12 consecutive TAAContext / UpscalingCameraData samples.
    // The marker is consumed here and the frames are consumed by the draw hook,
    // so the two threads never read the same file.
    if (presents % 30 == 0 && takeContextRequest()) {
        armContextSample(12, 0);
    }
    // P0.1: keep checking until the engine's Init appears (it may happen after a
    // world load on some starts). Once the core answers, this stops.
    if (!gSessionReady.load(std::memory_order_relaxed) && presents % 600 == 0) {
        logNgxSession("present poll");
    }
    // TEST: one-shot SR input dump triggered by a file marker. The panel button needs
    // the mouse, which means standing still - and the ghosting only shows in motion, so
    // the agent arms this marker while the player keeps walking and the next present
    // captures that frame.
    if (presents % 30 == 0) {
        std::error_code error;
        auto const      marker = std::filesystem::path(utils::applicationDirectory()) / "vv_dump_sr.txt";
        if (std::filesystem::exists(marker, error)) {
            std::filesystem::remove(marker, error);
            utils::logger().info("[vv] sr dump requested by file marker");
            ::neurodragon::vv::sr::dumpInputs();
        }
        auto const arg10Marker = std::filesystem::path(utils::applicationDirectory()) / "vv_arg10_now.txt";
        if (std::filesystem::exists(arg10Marker, error)) {
            std::filesystem::remove(arg10Marker, error);
            gArg10ProbeFrames.store(8, std::memory_order_release);
            utils::logger().info("[vv] arg10 probe armed for 8 frames");
        }
    }
    // A2 camera probe: once a world is loaded, up to six samples ~10 s apart. The
    // menu leaves the camera unpopulated (EXP-0021), so the world gate is what makes
    // the samples meaningful. The raw window is only logged for the first two, and a
    // matrix stack is only read when it is non-empty (the projection stack was empty
    // in EXP-0021 and back() on it produced garbage).
    {
        static unsigned camSamples = 0;
        if (worldLoaded() && camSamples < 6 && presents % 600 == 0) {
            auto const     sample = camera::read();
            unsigned const n      = ++camSamples;
            utils::logger().info(
                "[vv] cam #{} valid={} view_ok={} world_ok={} proj_ok={} pos=({:+.4f},{:+.4f},{:+.4f}) "
                "right=({:+.4f},{:+.4f},{:+.4f}) up=({:+.4f},{:+.4f},{:+.4f}) fwd=({:+.4f},{:+.4f},{:+.4f}) "
                "aspect={:.4f} fov={:.4f} near={:.4f} far={:.4f}",
                presents,
                sample.valid ? 1 : 0,
                sample.viewOk ? 1 : 0,
                sample.worldOk ? 1 : 0,
                sample.projectionOk ? 1 : 0,
                sample.position[0],
                sample.position[1],
                sample.position[2],
                sample.right[0],
                sample.right[1],
                sample.right[2],
                sample.up[0],
                sample.up[1],
                sample.up[2],
                sample.forward[0],
                sample.forward[1],
                sample.forward[2],
                sample.aspect,
                sample.fov,
                sample.zNear,
                sample.zFar
            );
            auto const text = [](float const* m) {
                std::string out;
                char        one[32]{};
                for (int i = 0; i < 16; ++i) {
                    std::snprintf(one, sizeof(one), " %+.5f", m[i]);
                    out += one;
                }
                return out;
            };
            if (sample.viewOk) {
                utils::logger().info("[vv] cam #{} view=[{}]", presents, text(sample.view));
            }
            if (sample.worldOk) {
                utils::logger().info("[vv] cam #{} world=[{}]", presents, text(sample.world));
            }
            if (sample.projectionOk) {
                utils::logger().info("[vv] cam #{} proj=[{}]", presents, text(sample.projection));
            }
            utils::logger().info("[vv] cam #{} invview=[{}]", presents, text(sample.inverseView));
            if (n <= 2) {
                for (std::size_t offset = 0; offset < std::size(sample.raw); offset += 8) {
                    utils::logger().info(
                        "[vv] camraw #{} +{:#05x}: {:+.5f} {:+.5f} {:+.5f} {:+.5f} | {:+.5f} {:+.5f} {:+.5f} {:+.5f}",
                        presents,
                        offset * sizeof(float),
                        static_cast<double>(sample.raw[offset + 0]),
                        static_cast<double>(sample.raw[offset + 1]),
                        static_cast<double>(sample.raw[offset + 2]),
                        static_cast<double>(sample.raw[offset + 3]),
                        static_cast<double>(sample.raw[offset + 4]),
                        static_cast<double>(sample.raw[offset + 5]),
                        static_cast<double>(sample.raw[offset + 6]),
                        static_cast<double>(sample.raw[offset + 7])
                    );
                }
            }
            float cur[16]{};
            float prev[16]{};
            float camOrigin[6]{};
            float jitter[2]{};
            if (taaMatrices(cur, prev, camOrigin, jitter)) {
                utils::logger().info("[vv] cam #{} taau cur=[{}]", presents, text(cur));
                utils::logger().info("[vv] cam #{} taau prev=[{}]", presents, text(prev));
                utils::logger().info(
                    "[vv] cam #{} taau origin=({:+.4f},{:+.4f},{:+.4f}) prev=({:+.4f},{:+.4f},{:+.4f}) jitter=({:+.4f},{:+.4f})",
                    presents,
                    camOrigin[0],
                    camOrigin[1],
                    camOrigin[2],
                    camOrigin[3],
                    camOrigin[4],
                    camOrigin[5],
                    jitter[0],
                    jitter[1]
                );
            }
        }
    }
#endif
    (void)gSeenCount;
    (void)gLastSeenCount;
    (void)gSeenStableSince;
    (void)gDumpPasses;
    // Frame-time marker. A hitch is a fact the user can feel, so it is measured here
    // instead of guessed at: only frames that lose >= 25 ms are logged (first ten, then
    // every fiftieth), which is enough to tell "the periodic diagnostics are gone" from
    // "something else is stalling" without turning the log into a counter dump.
    static LARGE_INTEGER lastPresent{};
    static long long     frequency{};
    if (!frequency) {
        LARGE_INTEGER f{};
        if (QueryPerformanceFrequency(&f)) {
            frequency = f.QuadPart;
        }
    }
    LARGE_INTEGER now{};
    QueryPerformanceCounter(&now);
    if (frequency && lastPresent.QuadPart) {
        double const ms = 1000.0 * static_cast<double>(now.QuadPart - lastPresent.QuadPart)
                        / static_cast<double>(frequency);
        if (ms >= 25.0) {
            static unsigned long long longFrames = 0;
            if (++longFrames <= 10 || longFrames % 50 == 0) {
                utils::logger().info("[vv] long frame {:.1f} ms at present #{}", ms, presents);
            }
        }
    }
    lastPresent = now;
}

void onSwapChainResize() {
    resetTextureInventory();
    gInputFinalColorNative.store(nullptr, std::memory_order_release);
    gInputFinalColorWidth.store(0, std::memory_order_release);
    gInputFinalColorHeight.store(0, std::memory_order_release);
    gInputFinalColorFormat.store(0, std::memory_order_release);
    gInputFinalColorRecaptureDraws.store(4, std::memory_order_release);
    // The motion-vector binding belongs to the old graph; the next TAAU draw re-captures
    // it from the rebuilt material binding.
    gMotionVectorNative.store(nullptr, std::memory_order_release);
    gArg10ProbeFrames.store(8, std::memory_order_release);
    gSetTextureLogs.store(0, std::memory_order_relaxed);
    gRenderGeneration.fetch_add(1, std::memory_order_relaxed);
    // A derived render size is stale after a rebuild; the next record re-derives it
    // from the new inventory (a TAAU-set size is left alone: the declarator refreshes
    // it on its own).
    if (gRenderDerived.exchange(false, std::memory_order_relaxed)) {
        gRenderWidth.store(0, std::memory_order_relaxed);
        gRenderHeight.store(0, std::memory_order_relaxed);
    }
    utils::logger().info("[vv] resize: released the old render-graph inventory");
}

void noteSwapChain(void* swapChain) {
    if (!swapChain) {
        return;
    }
    gSwapChain.store(swapChain, std::memory_order_relaxed);
    // The injection experiment reads the current back buffer from the same chain, so
    // it is handed the pointer before the per-present bookkeeping below.
    inject::noteSwapChain(swapChain);
    // Re-read every present: a resize replaces the buffers, and a stale exclusion
    // list would let the dump touch a back buffer (which lives in PRESENT). Four
    // slots because the chain is 2 or 3 buffers (EXP-0078); gExcluded has four.
    void*    buffers[4]{};
    unsigned const count = swapChainBuffers(swapChain, buffers, 4);
    // The back buffer is the display size, and it is the only source that does not
    // depend on the TAAU pass: `upscaling_mode` Bilinear never runs taauDetour, so
    // without this the inventory filter (tooSmallForRenderGraph) and the SR fallback
    // would both work with display = 0.
    if (count && buffers[0]) {
        // nr::describe goes through the resource vtable the probe already validated
        // (CreateRenderTargetView's first argument); no d3d12.h types are needed here.
        auto const info = nr::describe(buffers[0]);
        if (info.width >= 64 && info.height >= 64) {
            gDisplayWidth.store(static_cast<unsigned>(info.width), std::memory_order_relaxed);
            gDisplayHeight.store(static_cast<unsigned>(info.height), std::memory_order_relaxed);
        }
    }
    gExcludedCount       = 0;
    for (unsigned i = 0; i < count && gExcludedCount < 4; ++i) {
        gExcluded[gExcludedCount++] = buffers[i];
        // The reference must go immediately: holding one makes the engine's
        // ResizeBuffers fail, and that failure ends in abort() (0xC0000409).
        releaseComObject(buffers[i]);
    }
    if (count && !gLoggedExclusion) {
        gLoggedExclusion = true;
        utils::logger().info("[vv] {} back buffer(s) excluded from the dump", count);
    }
    // The content boundary compares bound RTV handles against the back buffers' own
    // views; this maps the two once per change of the chain.
    refreshBackBufferHandles();
}

void setGameQueue(void* queue) { gGameQueue.store(queue, std::memory_order_relaxed); }

void* engineDevice() { return gEngineDevice.load(std::memory_order_relaxed); }
void* inputFinalColorResource() { return gInputFinalColorNative.load(std::memory_order_acquire); }
RenderTargetInfo inputFinalColorTarget() {
    return {
        gInputFinalColorNative.load(std::memory_order_acquire),
        gInputFinalColorWidth.load(std::memory_order_acquire),
        gInputFinalColorHeight.load(std::memory_order_acquire),
        gInputFinalColorFormat.load(std::memory_order_acquire),
    };
}
bool worldLoaded() { return gWorldLoaded.load(std::memory_order_relaxed); }

bool taaMatrices(float* current, float* previous, float* camera, float* jitter) {
    // The snapshot taken in drawDetour, not the live context: the engine's own draw
    // rewrites the previous half, so a post-frame read would return prev==cur.
    if (!current || !previous || !gTaaSnapshotValid.load(std::memory_order_acquire)) {
        return false;
    }
    TaaSnapshot const snap = gTaaSnapshot;
    std::memcpy(current, snap.current, sizeof(snap.current));
    std::memcpy(previous, snap.previous, sizeof(snap.previous));
    if (camera) {
        std::memcpy(camera, snap.camera, sizeof(snap.camera));
    }
    if (jitter) {
        std::memcpy(jitter, snap.jitter, sizeof(snap.jitter));
    }
    return true;
}

void dumpTaaContextWindows() {
#ifdef TEST
    void* const ctx  = gTaaContext.load(std::memory_order_relaxed);
    void* const exec = gExecContext.load(std::memory_order_relaxed);
    void* const cam  = gCameraData.load(std::memory_order_relaxed);
    utils::logger().info(
        "[vv] ctxw ctx={:#x} exec={:#x} cam={:#x}",
        reinterpret_cast<std::uintptr_t>(ctx),
        reinterpret_cast<std::uintptr_t>(exec),
        reinterpret_cast<std::uintptr_t>(cam)
    );
    if (ctx) {
        sampleFloatWindow(ctx, "ctx", 90, 0);
    }
    if (exec && exec != ctx) {
        sampleFloatWindow(exec, "exec", 90, 0);
    }
    if (cam && cam != ctx && cam != exec) {
        sampleFloatWindow(cam, "cam", 90, 0);
    }
#endif
}
void* gameQueue() { return gGameQueue.load(std::memory_order_relaxed); }
unsigned renderWidth() { return gRenderWidth.load(std::memory_order_relaxed); }
unsigned renderHeight() { return gRenderHeight.load(std::memory_order_relaxed); }

bool renderSizeHint(unsigned displayWidth, unsigned displayHeight, unsigned* width, unsigned* height) {
    if (!width || !height || !displayWidth || !displayHeight || !gHeldCount) {
        return false;
    }
    float const  aspect = static_cast<float>(displayWidth) / static_cast<float>(displayHeight);
    unsigned     logged = 0;
    for (unsigned i = 0; i < gHeldCount; ++i) {
        unsigned const idx = (gHeldNext + kHeldSlots - 1 - i) % kHeldSlots;
        auto const&    e   = gHeld[idx];
        if (!e.resource || e.format != 26 || !e.width || !e.height) {
            continue;
        }
        bool const  below = e.width < displayWidth && e.height < displayHeight;
        float const ratio = static_cast<float>(e.width) / static_cast<float>(e.height);
        float       delta = ratio - aspect;
        if (delta < 0.0f) {
            delta = -delta;
        }
        bool const fits = below && delta <= aspect * 0.02f;
        if (logged < 6) {
            ++logged;
            utils::logger().info(
                "[vv] render size hint: candidate {}x{} fmt26 fits={}",
                e.width,
                e.height,
                fits ? 1 : 0
            );
        }
        if (fits) {
            *width  = e.width;
            *height = e.height;
            return true;
        }
    }
    return false;
}

void latchRenderSize(unsigned width, unsigned height) {
    if (!width || !height) {
        return;
    }
    if (gRenderWidth.load(std::memory_order_relaxed) || gRenderHeight.load(std::memory_order_relaxed)) {
        return;
    }
    gRenderWidth.store(width, std::memory_order_relaxed);
    gRenderHeight.store(height, std::memory_order_relaxed);
    gRenderDerived.store(true, std::memory_order_relaxed);
}
unsigned displayWidth() { return gDisplayWidth.load(std::memory_order_relaxed); }
unsigned displayHeight() { return gDisplayHeight.load(std::memory_order_relaxed); }

unsigned renderTargets(RenderTargetInfo* out, unsigned max, unsigned tolerance) {    if (!out || !max) {
        return 0;
    }
    unsigned const w = gRenderWidth.load(std::memory_order_relaxed);
    unsigned const h = gRenderHeight.load(std::memory_order_relaxed);
    if (!w || !h) {
        return 0;
    }
    // Not named `near`: <Windows.h> still defines that as a macro.
    auto const close = [tolerance](unsigned a, unsigned b) { return (a > b ? a - b : b - a) <= tolerance; };
    unsigned   count = 0;
    for (unsigned i = 0; i < gHeldCount && count < max; ++i) {
        unsigned const idx = (gHeldNext + kHeldSlots - 1 - i) % kHeldSlots;
        auto const&    e   = gHeld[idx];
        if (!e.resource || !close(e.width, w) || !close(e.height, h)) {
            continue;
        }
        out[count++] = RenderTargetInfo{e.resource, e.width, e.height, e.format};
    }
    return count;
}

unsigned depthTargets(RenderTargetInfo* out, unsigned max, unsigned tolerance) {
    if (!out || !max) {
        return 0;
    }
    unsigned const w = gRenderWidth.load(std::memory_order_relaxed);
    unsigned const h = gRenderHeight.load(std::memory_order_relaxed);
    if (!w || !h) {
        return 0;
    }
    auto const close = [tolerance](unsigned a, unsigned b) { return (a > b ? a - b : b - a) <= tolerance; };
    unsigned   count = 0;
    for (unsigned i = 0; i < gHeldDepthCount && count < max; ++i) {
        unsigned const idx = (gHeldDepthNext + kHeldDepthSlots - 1 - i) % kHeldDepthSlots;
        auto const&    e   = gHeldDepth[idx];
        if (!e.resource || !close(e.width, w) || !close(e.height, h)) {
            continue;
        }
        out[count++] = RenderTargetInfo{e.resource, e.width, e.height, e.format};
    }
    return count;
}

unsigned renderGeneration() { return gRenderGeneration.load(std::memory_order_relaxed); }

unsigned backBufferWriterSplit() { return gBoundarySplit.load(std::memory_order_relaxed); }
void*    backBufferWriterList() { return gBoundaryList.load(std::memory_order_relaxed); }

void* arg10Target() { return gArg10Native.load(std::memory_order_acquire); }
unsigned arg10WriterSplit() { return gArg10WriterSplit.load(std::memory_order_acquire); }
void*    arg10WriterList() { return gArg10WriterList.load(std::memory_order_acquire); }

void setArg10Solid(bool armed) {
    bool const previous = gArg10SolidArmed.exchange(armed, std::memory_order_acq_rel);
    if (previous == armed) {
        return;
    }
    // Force one fresh RTV capture per list: the lists that existed before this arming
    // would otherwise keep a null/stale target forever.
    gArg10ArmGeneration.fetch_add(1, std::memory_order_relaxed);
    gSetTextureLogs.store(0, std::memory_order_relaxed);
    gArg10WriterSplit.store(0, std::memory_order_relaxed);
    gArg10WriterList.store(nullptr, std::memory_order_relaxed);
    // Do not let a stale native from before the arming claim the first submission.
    gArg10ConsumedSerial.store(gArg10DeclaredSerial.load(std::memory_order_acquire), std::memory_order_release);
    utils::logger().info("[vv] arg10 write armed={}", armed ? 1 : 0);
}

bool arg10SolidArmed() { return gArg10SolidArmed.load(std::memory_order_acquire); }
bool arg10FreshSubmission() { return gArg10FreshSubmission.load(std::memory_order_acquire); }
void noteArg10Gap() { gArg10Gap.store(true, std::memory_order_release); }
bool takeArg10Gap() { return gArg10Gap.exchange(false, std::memory_order_acq_rel); }

void* motionVectorResource() { return gMotionVectorNative.load(std::memory_order_acquire); }

void setSuppressProbeFrames(unsigned frames) {
    gSuppressProbeFrames.store(frames > 240 ? 240 : frames, std::memory_order_release);
    utils::logger().info("[vv] d-probe: suppress armed frames={}", frames > 240 ? 240 : frames);
}
unsigned suppressProbeFrames() { return gSuppressProbeFrames.load(std::memory_order_acquire); }

void setDrawTagProbeFrames(unsigned frames) {
    gDrawTagProbeFrames.store(frames > 3600 ? 3600 : frames, std::memory_order_release);
    utils::logger().info("[vv] d-probe: draw tag armed frames={}", frames > 3600 ? 3600 : frames);
}
unsigned drawTagProbeFrames() { return gDrawTagProbeFrames.load(std::memory_order_relaxed); }

void setChainProbeFrames(unsigned frames) {
    gChainProbeFrames.store(frames > 4 ? 4 : frames, std::memory_order_release);
    utils::logger().info("[vv] d-probe: chain armed frames={}", frames > 4 ? 4 : frames);
}
unsigned chainProbeFrames() { return gChainProbeFrames.load(std::memory_order_relaxed); }

void setDrawSuppress(bool armed) {
    bool const previous = gDrawSuppressArmed.exchange(armed, std::memory_order_acq_rel);
    if (previous == armed) {
        return;
    }
    utils::logger().info("[vv] d-suppress armed={}", armed ? 1 : 0);
}
bool drawSuppressArmed() { return gDrawSuppressArmed.load(std::memory_order_acquire); }
unsigned drawSuppressSkips() { return gDrawSuppressSkips.load(std::memory_order_relaxed); }

void noteInjectionResult(bool ok) {
    gLastInjectionOk.store(ok, std::memory_order_release);
    if (ok) {
        // The frame really wrote arg10: only now may the draw be skipped under this
        // render generation.
        gInjectedGeneration.store(gRenderGeneration.load(std::memory_order_relaxed), std::memory_order_release);
    }
}

void setTaauDrawReplacement(bool armed) {
    bool const previous = gReplaceTaauDraw.exchange(armed, std::memory_order_acq_rel);
    if (previous == armed) {
        return;
    }
    utils::logger().info("[vv] taau replacement armed={}", armed ? 1 : 0);
}

bool taauDrawReplacementArmed() { return gReplaceTaauDraw.load(std::memory_order_acquire); }

void noteTaauReplacementMiss() {
    auto const missed = gReplacementMissed.fetch_add(1, std::memory_order_relaxed) + 1;
    // Same throttle as the restore path: the first few are the interesting ones, and a
    // frame that keeps missing would otherwise write a line per frame.
    if (missed <= 3 || missed % 120 == 0) {
        utils::logger().warn(
            "[vv] taau replacement missed={} suppressed={} (armed frame injected nothing; TAAU restored next frame)",
            missed,
            gDrawSuppressed.load(std::memory_order_relaxed)
        );
    }
}

TaauDrawStats taauDrawStats() {
    return TaauDrawStats{
        gTaauHits.load(std::memory_order_relaxed),
        gDrawHits.load(std::memory_order_relaxed),
        gDrawForwarded.load(std::memory_order_relaxed),
        gDrawSuppressed.load(std::memory_order_relaxed),
        gReplacementMissed.load(std::memory_order_relaxed),
    };
}

void requestContextSample(unsigned frames, unsigned delayMs) {
#ifdef TEST
    armContextSample(frames ? frames : 24, delayMs);
#else
    (void)frames;
    (void)delayMs;
#endif
}

void requestDumpNow() {
#ifdef TEST
    unsigned const w = gRenderWidth.load(std::memory_order_relaxed);
    unsigned const h = gRenderHeight.load(std::memory_order_relaxed);
    if (!w || !h) {
        utils::logger().warn("[vv] semantics: dump requested before the render size is known");
        return;
    }
    unsigned const batch = gDumpBatch.fetch_add(1, std::memory_order_acq_rel);
    utils::logger().info("[vv] semantics: batch {} requested by panel", batch);
    dumpSemantics(batch);
#endif
}

} // namespace neurodragon::probe::vv
