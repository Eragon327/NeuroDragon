#include "neurodragon/probe/VvProbe.h"

#include "neurodragon/nr/D3D12Bridge.h"
#include "neurodragon/probe/RuntimeOptions.h"
#include "neurodragon/probe/VvCalibrate.h"
#include "neurodragon/probe/VvInject.h"
#include "neurodragon/probe/VvUpscaling.h"
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

struct RtvBinding {
    std::uint64_t handle;
    void*         resource;
};
constexpr unsigned    kRtvSlots = 4096;
RtvBinding            gRtvs[kRtvSlots]{};
std::atomic<unsigned> gRtvCount{0};

std::atomic<unsigned> gDescriptorIncrement{0};

// One entry per command list seen in the frame, reset on every engine submission.
// The only thing it has to answer is "did this list bind the current arg10", which is
// how the injection finds its cut point and how the draw gate finds TAAU's own quad.
struct ListSig {
    void*          list;
    unsigned       draws; ///< DrawInstanced + DrawIndexedInstanced in this list
    // Every RTV handle this list bound during the current arming, not just its
    // first call: a list may bind a small target first and arg10 later, and the
    // writer is "any binding", not "the first one" (EXP-0057 postmortem). 16, not 8:
    // the 108/109-list shapes bind a longer mip chain before arg10, and when the
    // eight slots filled the writer stopped resolving after the shape changed.
    std::uintptr_t rtvSeen[16];
    unsigned       rtvSeenCount;
    unsigned       rtvSeenGen;
    // Direct evidence recorded at bind time: this list bound the *current* arg10
    // native while the capture was armed, tagged with the declaration serial so a
    // stale binding from the previous frame cannot match the alternating buffer.
    void*    arg10Bound;
    unsigned arg10BoundSerial;
};
// 512, not 128: UI frames (panel open) bring new list objects into the table, and
// when it filled the arg10 writer stopped resolving after the panel closed (EXP-0058).
constexpr unsigned kSigSlots = 512;
ListSig               gSigs[kSigSlots]{};
std::atomic<unsigned> gSigCount{0};

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
        gSigs[slot] = ListSig{list, 0, {}, 0, ~0u, nullptr, 0};
        return &gSigs[slot];
    }
    static std::atomic<bool> warned{false};
    if (!warned.exchange(true, std::memory_order_relaxed)) {
        utils::logger().warn("[vv] list signature table full ({} slots); new lists are not tracked", kSigSlots);
    }
    return nullptr;
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
using CmdDrawIndexedFn = void (*)(void* list, unsigned indexCount, unsigned instanceCount, unsigned startIndex, int baseVertex, unsigned startInstance);
CmdDrawIndexedFn      gOriginalDrawIndexed{};
std::atomic<unsigned> gDrawIndexedCalls{0};

std::atomic<bool> gDeviceHooked{false};
std::atomic<bool> gListHooked{false};

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
std::atomic<void*>     gGameQueue{nullptr};
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
// Every resource that has ever been given a render-target view, so the reference-
// counted ring below holds each resource once. Raw pointers only, no references (see
// createRtvDetour), and deliberately NOT cleared on resize: the engine creates the new
// render graph's views *before* our ResizeBuffers hook runs (2026-10-25 03:28:50.510
// vs .518).
constexpr unsigned kSeenSlots = 4096;
void*              gSeen[kSeenSlots]{};
unsigned           gSeenCount = 0;

// Reference-counted subset of the RTV inventory, safe to describe later.
// Only small, non-swap-chain resources enter it (see createRtvDetour), it is a
// ring so the newest 256 survive, and it is NOT cleared on resize: the new
// graph's views are created just before our ResizeBuffers hook runs. This is the
// list renderTargets()/renderSizeHint() read; the full gSeen list stays raw because a
// *stale* raw pointer passed to nr::describe() is exactly the 2026-10-25 03:4x AV.
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
// Bumped on resize / world enter; the SR record resets the DLSS history with it.
std::atomic<unsigned> gRenderGeneration{1};
std::atomic<void*> gSwapChain{nullptr};

void hookDevice(void* device);
void hookCommandList(void* list);

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
    // Our own views (arg10's RTV) are not part of the frame graph this hook inventories.
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
    // Keep each *distinct* resource once: a render-target view is only ever created
    // when a texture becomes a target, so this is the closest thing to an inventory of
    // the frame's render targets - and it stays readable because the pool is reused and
    // the same textures reappear.
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
        // The reference-counted list renderTargets() reads is gHeld, restricted to
        // small non-swap-chain textures.
        gSeen[gSeenCount++] = resource;
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
        ND_DEBUG(
            "[vv] rtv res={:#x} {}x{} fmt={}",
            reinterpret_cast<std::uintptr_t>(resource),
            static_cast<unsigned>(info.width),
            static_cast<unsigned>(info.height),
            info.format
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
                ND_DEBUG(
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

extern std::atomic<bool>     gProductArmed;
extern std::atomic<unsigned> gArg10ArmGeneration;

// Declared below in this translation unit; the bind-time arg10 proof inside the
// OMRT detour needs them before their definitions.
extern std::atomic<void*>    gArg10Native;
extern std::atomic<unsigned> gArg10DeclaredSerial;
extern std::atomic<void*>    gMotionVectorNative;
extern std::atomic<bool>     gDrawSuppressArmed;
extern std::atomic<bool>     gLastInjectionOk;
extern std::atomic<unsigned> gDrawSuppressSkips;
extern std::atomic<unsigned> gInjectedGeneration;

void omSetRtvDetour(void* list, unsigned count, std::uintptr_t const* rtvs, int single, std::uintptr_t const* dsv) {
    // Pure forward: nothing here changes an argument or a state. What it adds is the
    // one fact the frame does not expose anywhere else - which list bound this frame's
    // arg10. Recorded on every binding, not only while the product is armed: the same
    // bind-time proof is what lets the timing path recognise TAAU's own 6x1 draw with
    // the replacement switched off (drawIndexedInstancedDetour -> inArg10WriterList).
    if (list && rtvs && count) {
        static std::atomic<unsigned> calls{0};
        unsigned const               increment = gDescriptorIncrement.load(std::memory_order_relaxed);
        if (auto* const sig = sigFor(list)) {
            unsigned const gen = gArg10ArmGeneration.load(std::memory_order_relaxed);
            if (sig->rtvSeenGen != gen) {
                sig->rtvSeenCount = 0;
                sig->rtvSeenGen   = gen;
                sig->arg10Bound   = nullptr;
            }
            void* const native   = gArg10Native.load(std::memory_order_acquire);
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
        }
        unsigned const n = calls.fetch_add(1, std::memory_order_relaxed) + 1;
        if (n <= 2) {
            // Shape check for the slot itself: a real OMSetRenderTargets arrives with
            // count 1..8, a small single flag and a valid descriptor handle.
            ND_DEBUG(
                "[vv] om rt #{} list={:#x} count={} single={} handle0={:#x} increment={}",
                n,
                reinterpret_cast<std::uintptr_t>(list),
                count,
                single,
                static_cast<std::uintptr_t>(rtvs[0]),
                increment
            );
        }
    }
    gOriginalOmSetRtv(list, count, rtvs, single, dsv);
}


void hookDevice(void* device) {
    if (!device) {
        return;
    }
    // Every device is reported, and every *distinct* CreateRenderTargetView target is
    // hooked: the overlay creates a throw-away device for the swap-chain vtable, and
    // latching on the first device seen (that one) is why the engine's own render
    // targets never showed up.
    ND_DEBUG("[vv] device created {:#x}", reinterpret_cast<std::uintptr_t>(device));
    if (!gEngineDevice.load(std::memory_order_relaxed)) {
        gEngineDevice.store(device, std::memory_order_relaxed);
    }
    // Needed when the engine passes a single RTV handle for a contiguous range: the
    // arg10 writer scan computes the i-th handle from it.
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
        ND_DEBUG(
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
        ND_DEBUG(
            "[vv] CreateDepthStencilView hooked on device {:#x} (target {:#x})",
            reinterpret_cast<std::uintptr_t>(device),
            reinterpret_cast<std::uintptr_t>(dsvTarget)
        );
    } else if (!dsvHooked) {
        utils::logger().warn("[vv] could not hook CreateDepthStencilView for device {:#x}", reinterpret_cast<std::uintptr_t>(device));
    }
}

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

void drawInstancedDetour(void* list, unsigned vertexCount, unsigned instanceCount, unsigned startVertex, unsigned startInstance) {
    if (auto* const sig = sigFor(list)) {
        ++sig->draws;
    }
    auto const n = gDrawInstancedCalls.fetch_add(1, std::memory_order_relaxed) + 1;
    if (n <= 8) {
        ND_DEBUG(
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

void drawIndexedInstancedDetour(void* list, unsigned indexCount, unsigned instanceCount, unsigned startIndex, int baseVertex, unsigned startInstance) {
    // The TAAU quad is identified by the same bind-time proof the injection uses: the
    // first 6x1 indexed draw of the list that already bound this frame's arg10
    // (EXP-0066). That identification is deliberately independent of every switch below:
    // the timing path and the product decide different things from it.
    std::uint64_t const taauFrame = gTaauHits.load(std::memory_order_relaxed);
    bool const          isTaauDraw = indexCount == 6 && instanceCount == 1 && inArg10WriterList(list, true);
    // Timestamps are engine infrastructure (nr::), not the product's: the objects are
    // created on the first real TAAU draw so a session that never arms the replacement
    // still reports the upscaler's cost.
    if (isTaauDraw) {
        nr::nrEnsureStamps(list);
    }

    // Stage D: skip exactly the TAAU quad, and only while the last engine frame really
    // injected. The callback, its material binds, the arg10 writer binding and the MV
    // capture all stay; only the GPU draw leaves the submission.
    //
    // There is deliberately no world gate: since the user's 2026-10-28 decision the
    // pipeline keeps running after leaving a save, and the menu background is processed
    // by the same DLSS -> arg10 path (EXP-0068 showed what happens when the skip runs
    // but the injection is a no-op: the background freezes on the last world frame).
    // `productArmed` makes Stop/Start take effect immediately; `gLastInjectionOk`
    // restores TAAU on the frame after any failed injection; the generation checks keep
    // a resize frame on TAAU until a correctly sized injection exists (rapid F11 used to
    // leave a stale/truncated arg10, EXP-0075).
    bool skipped = false;
    if (isTaauDraw
        && gDrawSuppressArmed.load(std::memory_order_acquire)
        && gProductArmed.load(std::memory_order_acquire)
        && gLastInjectionOk.load(std::memory_order_acquire)
        && gInjectedGeneration.load(std::memory_order_acquire)
               == gRenderGeneration.load(std::memory_order_relaxed)
        && gMotionVectorNative.load(std::memory_order_acquire)) {
        unsigned const n = gDrawSuppressSkips.fetch_add(1, std::memory_order_relaxed) + 1;
        if (n <= 8) {
            ND_DEBUG(
                "[vv] d-suppress: skipped TAAU draw #{} list={:#x}",
                n,
                reinterpret_cast<std::uintptr_t>(list)
            );
        }
        skipped = true;
    }
    if (auto* const sig = sigFor(list)) {
        ++sig->draws;
    }
    if (skipped) {
        return;
    }
    auto const n = gDrawIndexedCalls.fetch_add(1, std::memory_order_relaxed) + 1;
    if (n <= 8) {
        ND_DEBUG(
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
    if (!isTaauDraw) {
        gOriginalDrawIndexed(list, indexCount, instanceCount, startIndex, baseVertex, startInstance);
        return;
    }
    // Only a draw that is really forwarded is a TAAU sample. When the product replaced
    // it there is no TAAU work in the frame, and stamping an interval here would report
    // a number nobody ran; the replacement's own cost is stamped by the evaluate detour
    // (RenderProbe::ngxEvaluateDetour, id 1) instead.
    nr::nrStampBegin(list, taauFrame, 1);
    gOriginalDrawIndexed(list, indexCount, instanceCount, startIndex, baseVertex, startInstance);
    nr::nrStampEnd(list, taauFrame, 1);
    nr::nrStampTick(taauFrame);
#ifdef TEST
    // One-shot real-machine assertion: as soon as the ring has a median for this id,
    // say so with the number. Reachable only from a forwarded TAAU draw, so an RT
    // session's engine-DLSS sample cannot trigger it; release has neither the call nor
    // the log.
    {
        static bool logged = false;
        if (!logged) {
            auto const stats = nr::nrPanelStats();
            if (stats.hasEngine) {
                logged = true;
                ND_DEBUG(
                    "[vv] TAAU timing available: {} ms ({} samples)",
                    stats.engineMs,
                    stats.engineSamples
                );
            }
        }
    }
#endif
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
    ND_DEBUG(
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
            ND_DEBUG("[vv] engine device {:#x}", reinterpret_cast<std::uintptr_t>(device));
            // This device came from an engine submission, so it is authoritative -
            // the overlay's throw-away device may have been created first.
            gEngineDevice.store(device, std::memory_order_relaxed);
            hookDevice(device);
            // The injection records on the engine's device too; it only needs to be
            // told which one that is (docs/specs/vv.md §35.2).
            inject::setDevice(device);
        }
    }

    auto hookAt = [vtbl](unsigned index, void* detour, void** original, char const* what) {
        auto* const target = const_cast<void*>(vtbl[index]);
        bool const  ok = target && MH_CreateHook(target, detour, original) == MH_OK && MH_EnableHook(target) == MH_OK;
        if (!ok) {
            utils::logger().warn("[vv] command list slot {} ({}) not hooked", index, what);
        } else {
            ND_DEBUG("[vv] command list slot {} ({}) hooked", index, what);
        }
        return ok;
    };
    // Every detour forwards every argument unchanged. Slot numbers come from the SDK
    // declaration order, which the working slots above already matched.
    hookAt(12, reinterpret_cast<void*>(&drawInstancedDetour), reinterpret_cast<void**>(&gOriginalDrawInstanced), "DrawInstanced");
    hookAt(13, reinterpret_cast<void*>(&drawIndexedInstancedDetour), reinterpret_cast<void**>(&gOriginalDrawIndexed), "DrawIndexedInstanced");
    hookAt(46, reinterpret_cast<void*>(&omSetRtvDetour), reinterpret_cast<void**>(&gOriginalOmSetRtv), "OMSetRenderTargets");
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

#ifdef TEST
// Only the TEST taau log line names the mode; a release build has no caller.
char const* modeName(int mode) {
    switch (mode) {
    case 0: return "Taau";
    case 1: return "Bilinear";
    case 2: return "Fsr1";
    case 3: return "Fsr1DebugBilinear";
    default: return "?";
    }
}
#endif

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
std::atomic<void*>          gArg10Native{nullptr};
// Stage A: when armed, every frame resolves the current arg10 native and the list
// whose first OMSetRenderTargets binds it. The injection then cuts after that list
// and writes the DLSS output into arg10.
std::atomic<bool>           gProductArmed{false};
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
        ND_DEBUG(
            "[vv] texture table ctx={:#x} (global rva {:#x}); handle->native is a plain read",
            value,
            kTextureContextGlobalRva
        );
    }
    return resolved;
}

// One handle -> native lookup. `reason` is the diagnosis and the only thing the caller
// branches on; the other fields say *where* the walk stopped instead of only that it did.
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
    ND_DEBUG(
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
    ND_DEBUG(
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
    if (resource && gInTaauDraw) {
        std::uint16_t handle = kHandleInvalid;
        if (peek(resource, 0, handle) && handle != kHandleInvalid) {
            TextureLookup const lookup = lookupTextureNative(handle);
            // Classify the bound texture by its own description instead of by whether
            // it is still in the RTV ring. A rebuild creates several hundred views,
            // and the ring holds 256: the older entries (the ones the TAAU draw binds)
            // are evicted before the draw runs. That is how the motion vectors stopped
            // being captured after the 2026-10-28 23:11 world re-entry - the resize
            // cleared gMotionVectorNative, nothing refilled it, and every SR frame was
            // skipped for the rest of the session ("no usable arg11 ... native=0x0").
            RenderTargetInfo detail{};
            if (lookup.native) {
                auto const info = nr::describe(lookup.native);
                detail.resource = lookup.native;
                detail.width    = static_cast<unsigned>(info.width);
                detail.height   = static_cast<unsigned>(info.height);
                detail.format   = static_cast<unsigned>(info.format);
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
                if (mvNo <= 6) {
                    ND_DEBUG(
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
    ND_DEBUG("[vv] material texture hooked at {:#x} (rva {:#x})", gSetTextureTarget, kSetTextureRva);
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
    if (width && height && up.factor > 0.0f && up.factor <= 1.0f) {
        gDisplayWidth.store(width, std::memory_order_relaxed);
        gDisplayHeight.store(height, std::memory_order_relaxed);
        if (nativeRender()) {
            // 100% camera + branch keepalive (probe/VvUpscaling.cpp): the camera and
            // colour/depth targets are display sized; the restored <1 factor only
            // keeps TAAU/arg10 declared, so display is the real render size.
            gRenderWidth.store(width, std::memory_order_relaxed);
            gRenderHeight.store(height, std::memory_order_relaxed);
        } else {
            // The engine truncates (1131 x 0.33 -> 373x239, not 373x240), so this must
            // truncate too.
            gRenderWidth.store(
                static_cast<unsigned>(static_cast<float>(width) * up.factor),
                std::memory_order_relaxed
            );
            gRenderHeight.store(
                static_cast<unsigned>(static_cast<float>(height) * up.factor),
                std::memory_order_relaxed
            );
        }
    }

    bool const verbose = index <= 4;
    if (verbose) {
        ND_DEBUG(
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
    // The declarator's arg10 chain (dep1 -> wrapper +0x8 -> holder +0x10 -> resource
    // +0x18 -> bgfx handle) is parsed on EVERY declaration, not only while the product
    // is armed. Two readers need it and neither may depend on the switch:
    //   * the injection, which cuts after the list that binds this native;
    //   * the timing path, which identifies TAAU's own 6x1 draw by the same bind-time
    //     proof (drawIndexedInstancedDetour) even with the replacement switched off.
    // It is a read-only parse: nothing here writes a resource or records GPU work.
    {
        void*         wrapper = nullptr;
        void*         holder = nullptr;
        void*         resource = nullptr;
        std::uint16_t handle = kHandleInvalid;
        TextureLookup lookup{};
        if (peek(dep1, 0x8, wrapper) && peek(wrapper, 0x10, holder) && peek(holder, 0x18, resource)
            && peek(resource, 0, handle)) {
            if (handle != kHandleInvalid) {
                lookup = lookupTextureNative(handle);
            }
        }
        gArg10Native.store(lookup.native, std::memory_order_release);
        gArg10DeclaredSerial.fetch_add(1, std::memory_order_release);
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
// argument - which is why the snapshot is taken there and not in the declarator.

void drawDetour(void* self, void* context, void* third, void* fourth) {
    ++gDrawHits;
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
    gDrawForwarded.fetch_add(1, std::memory_order_relaxed);
    // The MV / colour role split is captured from the material bindings the original
    // callback issues, so the flag has to be up around it.
    gInTaauDraw = true;
    gOriginalDraw(self, context, third, fourth);
    gInTaauDraw = false;
}

void logSubmitRate(std::uint64_t presents) {
    auto const submits = gSubmits.load(std::memory_order_relaxed);
    auto const lists   = gSubmittedLists.load(std::memory_order_relaxed);
    auto const taau    = gTaauHits.load(std::memory_order_relaxed);
    auto const draws   = gDrawHits.load(std::memory_order_relaxed);
    ND_DEBUG(
        "[vv] presents={} submits={} lists={} maxListsPerSubmit={} taau={} draws={} | submits/frame={:.2f} "
        "lists/frame={:.2f} taau/frame={:.2f} draws/frame={:.2f}",
        presents,
        submits,
        lists,
        gMaxListsPerSubmit.load(std::memory_order_relaxed),
        taau,
        draws,
        presents ? static_cast<double>(submits) / static_cast<double>(presents) : 0.0,
        presents ? static_cast<double>(lists) / static_cast<double>(presents) : 0.0,
        presents ? static_cast<double>(taau) / static_cast<double>(presents) : 0.0,
        presents ? static_cast<double>(draws) / static_cast<double>(presents) : 0.0
    );
}

} // namespace

void noteWorldEntered() {
    gRenderGeneration.fetch_add(1, std::memory_order_relaxed);
    ND_DEBUG("[vv] world entered; render generation bumped");
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
    ND_DEBUG("[vv] taau hooked at {:#x} (rva {:#x})", reinterpret_cast<std::uintptr_t>(target), kTaauRva);

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
            // The panel's "configured for VV" reading is the only thing observation
            // cannot answer (at 100% nothing runs), so refresh it on every join.
            options::refreshKnownValues(true);
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
            // Throttled to ~2 s inside; keeps the panel's "configured" reading honest
            // when the player changes the graphics setting while in a world.
            options::refreshKnownValues();
        });
        ND_DEBUG(
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
            ND_DEBUG(
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
            ND_DEBUG("[vv] D3D12CreateDevice hooked");
        } else {
            utils::logger().warn("[vv] D3D12CreateDevice not hooked; render targets stay unresolved");
        }
    }
    return true;
}

void uninstall() {
    uninstallSetTextureHook();
    // EXP-0030: drop the cached renderer context so a reload cannot read through it.
    uninstallTextureTable();
    // The injection created objects on the engine's device, so it has to let go of
    // them before the probe's own hooks come down.
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
    // gSeen holds raw pointers only (no references - see createRtvDetour); gHeld is
    // the reference-counted inventory, released here.
    gSeenCount = 0;
    for (unsigned i = 0; i < gHeldCount; ++i) {
        releaseComObject(gHeld[i].resource);
        gHeld[i] = HeldRtv{};
    }
    gHeldCount = 0;
    gHeldNext  = 0;
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
        ND_DEBUG("[vv] submit #{} tid={} lists={}", index, GetCurrentThreadId(), listCount);
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
            unsigned const sigs = gSigCount.load(std::memory_order_relaxed);
            auto const     sigAt = [&](unsigned i) -> ListSig const* {
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
            bool const     arg10Armed = gProductArmed.load(std::memory_order_acquire);
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
            // The per-frame list table is rebuilt from scratch on every engine
            // submission: lists are pooled by the engine, and a stale entry would make
            // the writer proof match a list that is not in this frame.
            gSigCount.store(0, std::memory_order_relaxed);
        }
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
    // First frames only: how the counters grow is a start-up fact, not a per-frame one.
    if (presents <= 10) {
        logSubmitRate(presents);
    }
    // The product switch (docs/specs/ui.md §3): `sr.vvEnabled` is a config value, but
    // the arming itself is four injector switches, so it is applied here - every frame,
    // and only when the value changed. A no-op otherwise.
    neurodragon::vv::sr::syncConfig();
    // Frame-time marker. A hitch is a fact the user can feel, so it is measured here
    // instead of guessed at; the line is a debug-level one, so a release build pays
    // neither the formatting nor the log volume.
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
#ifdef TEST
    // Present-to-present average/min/max over ~10 s: the contract's "frame time is
    // recorded" (S6) needs a number, not a frame count. Values outside 0..1000 ms are
    // alt-tab or loading hitches, not frames. TEST only: the panel already shows
    // fps/frame time, and a release log stays free of per-frame aggregation.
    static std::uint64_t frames = 0;
    static double        sumMs  = 0.0;
    static double        minMs  = 0.0;
    static double        maxMs  = 0.0;
#endif
    if (frequency && lastPresent.QuadPart) {
        double const ms = 1000.0 * static_cast<double>(now.QuadPart - lastPresent.QuadPart)
                        / static_cast<double>(frequency);
        if (ms >= 25.0) {
            static unsigned long long longFrames = 0;
            if (++longFrames <= 10) {
                ND_DEBUG("[vv] long frame {:.1f} ms at present #{}", ms, presents);
            }
        }
#ifdef TEST
        if (ms > 0.0 && ms < 1000.0) {
            sumMs += ms;
            if (frames == 0 || ms < minMs) {
                minMs = ms;
            }
            if (ms > maxMs) {
                maxMs = ms;
            }
            ++frames;
        }
        if (frames > 0 && (presents <= 10 || presents % 600 == 0)) {
            utils::logger().info(
                "[vv] frame time: avg={:.2f} ms min={:.2f} max={:.2f} over {} presents",
                sumMs / static_cast<double>(frames),
                minMs,
                maxMs,
                frames
            );
            sumMs  = 0.0;
            minMs  = 0.0;
            maxMs  = 0.0;
            frames = 0;
        }
#endif
    }
    lastPresent = now;
}

void onSwapChainResize() {
    gInputFinalColorNative.store(nullptr, std::memory_order_release);
    gInputFinalColorWidth.store(0, std::memory_order_release);
    gInputFinalColorHeight.store(0, std::memory_order_release);
    gInputFinalColorFormat.store(0, std::memory_order_release);
    // The motion-vector binding belongs to the old graph; the next TAAU draw re-captures
    // it from the rebuilt material binding.
    gMotionVectorNative.store(nullptr, std::memory_order_release);
    gRenderGeneration.fetch_add(1, std::memory_order_relaxed);
    // A derived render size is stale after a rebuild; the next record re-derives it
    // from the new inventory (a TAAU-set size is left alone: the declarator refreshes
    // it on its own).
    if (gRenderDerived.exchange(false, std::memory_order_relaxed)) {
        gRenderWidth.store(0, std::memory_order_relaxed);
        gRenderHeight.store(0, std::memory_order_relaxed);
    }
    ND_DEBUG("[vv] resize: released the old render-graph inventory");
}

void noteSwapChain(void* swapChain) {
    if (!swapChain) {
        return;
    }
    gSwapChain.store(swapChain, std::memory_order_relaxed);
    // The injection reads the current back buffer from the same chain, so it is
    // handed the pointer before the per-present bookkeeping below.
    inject::noteSwapChain(swapChain);
    // Re-read every present: a resize replaces the buffers, and a stale exclusion
    // list would let the render-target inventory keep a back buffer (which lives in
    // PRESENT). Four slots because the chain is 2 or 3 buffers (EXP-0078); gExcluded
    // has four.
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
        ND_DEBUG("[vv] {} back buffer(s) excluded from the render-target inventory", count);
    }
}

void setGameQueue(void* queue) { gGameQueue.store(queue, std::memory_order_relaxed); }

void* engineDevice() { return gEngineDevice.load(std::memory_order_relaxed); }
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

void* gameQueue() { return gGameQueue.load(std::memory_order_relaxed); }
unsigned renderWidth() { return gRenderWidth.load(std::memory_order_relaxed); }
unsigned renderHeight() { return gRenderHeight.load(std::memory_order_relaxed); }

bool renderSizeHint(unsigned displayWidth, unsigned displayHeight, unsigned* width, unsigned* height) {
    if (!width || !height || !displayWidth || !displayHeight || !gHeldCount) {
        return false;
    }
    float const  aspect = static_cast<float>(displayWidth) / static_cast<float>(displayHeight);
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

void* arg10Target() { return gArg10Native.load(std::memory_order_acquire); }
unsigned arg10WriterSplit() { return gArg10WriterSplit.load(std::memory_order_acquire); }
void*    arg10WriterList() { return gArg10WriterList.load(std::memory_order_acquire); }

void setProductArmed(bool armed) {
    bool const previous = gProductArmed.exchange(armed, std::memory_order_acq_rel);
    if (previous == armed) {
        return;
    }
    // Force one fresh RTV capture per list: the lists that existed before this arming
    // would otherwise keep a null/stale target forever.
    gArg10ArmGeneration.fetch_add(1, std::memory_order_relaxed);
    gArg10WriterSplit.store(0, std::memory_order_relaxed);
    gArg10WriterList.store(nullptr, std::memory_order_relaxed);
    // Do not let a stale native from before the arming claim the first submission.
    gArg10ConsumedSerial.store(gArg10DeclaredSerial.load(std::memory_order_acquire), std::memory_order_release);
    ND_DEBUG("[vv] arg10 write armed={}", armed ? 1 : 0);
}

bool productArmed() { return gProductArmed.load(std::memory_order_acquire); }
bool arg10FreshSubmission() { return gArg10FreshSubmission.load(std::memory_order_acquire); }
void noteArg10Gap() { gArg10Gap.store(true, std::memory_order_release); }
bool takeArg10Gap() { return gArg10Gap.exchange(false, std::memory_order_acq_rel); }

void* motionVectorResource() { return gMotionVectorNative.load(std::memory_order_acquire); }

void setDrawSuppress(bool armed) {
    bool const previous = gDrawSuppressArmed.exchange(armed, std::memory_order_acq_rel);
    if (previous == armed) {
        return;
    }
    ND_DEBUG("[vv] d-suppress armed={}", armed ? 1 : 0);
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

TaauDrawStats taauDrawStats() {
    return TaauDrawStats{
        gTaauHits.load(std::memory_order_relaxed),
        gDrawHits.load(std::memory_order_relaxed),
        gDrawForwarded.load(std::memory_order_relaxed),
    };
}

} // namespace neurodragon::probe::vv
