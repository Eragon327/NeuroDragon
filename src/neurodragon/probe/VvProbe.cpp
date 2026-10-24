#include "neurodragon/probe/VvProbe.h"

#include "neurodragon/nr/D3D12Bridge.h"
#include "neurodragon/probe/VvCalibrate.h"
#include "neurodragon/probe/VvInject.h"
#include "neurodragon/utils/Utils.h"

#include <MinHook.h>
#include <Windows.h>


#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <filesystem>
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
//   * ID3D12GraphicsCommandList::Close (vtable 9) -> list boundaries, in recording
//     order, so a list pointer can be turned into an index in the submitted array;
//   * ID3D12GraphicsCommandList::OMSetRenderTargets (vtable 25) -> the render target
//     each list writes. The method's `this` IS the list object, so every write is
//     self-labelled.
//   * ID3D12Device::CreateRenderTargetView (vtable 20) -> CPU descriptor handle to
//     ID3D12Resource, which is what turns a render-target write into a texture.
//
// All four are observation only: each detour forwards immediately. No method is
// marshalled, no resource state is touched.
using CreateDeviceFn = long(WINAPI*)(void* adapter, int minFeatureLevel, void const* riid, void** device);
using CreateRtvFn    = void (*)(void* device, void* resource, void const* desc, std::uintptr_t handle);
using CloseFn        = void (*)(void* list);
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
std::atomic<void*>    gResourceVtable{nullptr};
// Every distinct CreateRenderTargetView implementation we have hooked (one per
// device class): the overlay's throw-away device and the engine's own are not the
// same function.
constexpr unsigned kHookedVtables = 4;
void*              gHookedRtvTargets[kHookedVtables]{};
unsigned           gHookCount = 0;

CreateDeviceFn gOriginalCreateDevice{};
CreateRtvFn    gOriginalCreateRtv{};
CloseFn        gOriginalClose{};
OmSetRtvFn     gOriginalOmSetRtv{};
CmdDrawFn      gOriginalDrawInstanced{};
std::atomic<unsigned> gDrawInstancedCalls{0};
using CmdDrawIndexedFn = void (*)(void* list, unsigned indexCount, unsigned instanceCount, unsigned startIndex, int baseVertex, unsigned startInstance);
CmdDrawIndexedFn      gOriginalDrawIndexed{};
std::atomic<unsigned> gDrawIndexedCalls{0};

std::atomic<bool> gDeviceHooked{false};
std::atomic<bool> gListHooked{false};
// True for the recording of one frame every 600 presented frames: the frame graph is
// stable, so one sampled frame is enough and a per-frame log would be unusable.
std::atomic<bool> gDumpFrame{false};
std::atomic<unsigned> gCloseInFrame{0};
std::atomic<unsigned> gBigFrames{0};

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
std::atomic<void*> gGameQueue{nullptr};
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
constexpr unsigned kHeldSlots = 128;
HeldRtv            gHeld[kHeldSlots]{};
unsigned           gHeldCount = 0;
unsigned           gHeldNext = 0;

// The render resolution the TAAU pass upscales FROM: display viewport x the engine's
// factor. It is the key that turns the render-target inventory into named inputs
// (docs/specs/vv.md §6): 373x239 fmt=10 is the HDR colour, fmt=12 the motion vectors,
// fmt=54 the depth. Filled by taauDetour, which is the only place both numbers exist.
std::atomic<unsigned> gRenderWidth{0};
std::atomic<unsigned> gRenderHeight{0};
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

void recordRtv(std::uintptr_t handle, void* resource) {
    unsigned const n = gRtvCount.fetch_add(1, std::memory_order_relaxed);
    if (n < kRtvSlots) {
        gRtvs[n].handle   = handle;
        gRtvs[n].resource = resource;
    }
}

void* lookupRtv(std::uintptr_t handle) {
    unsigned const n = gRtvCount.load(std::memory_order_relaxed);
    for (unsigned i = n < kRtvSlots ? n : kRtvSlots; i > 0; --i) {
        if (gRtvs[i - 1].handle == handle) {
            return gRtvs[i - 1].resource;
        }
    }
    return nullptr;
}

// True when the resource is one of the swap chain's current back buffers. Those
// must never be AddRef'd (ResizeBuffers would fail and the engine aborts); the
// size gate below already excludes them, but this is the exact test when the
// chain is known.
bool isSwapChainBuffer(void* resource) {
    if (!resource) {
        return false;
    }
    auto* const chain = gSwapChain.load(std::memory_order_relaxed);
    if (!chain) {
        return false;
    }
    void*    buffers[2]{};
    unsigned const count = swapChainBuffers(chain, buffers, 2);
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
        // The render graph at this engine is <=1000x600 at the current VV factor
        // (844x475 at 2560x1440, 360x242 windowed); display-sized back buffers,
        // 1024x2048 atlases and 1937x333 UI strips fall outside. The swap-chain
        // test is the exact one when the chain is already known.
        bool const renderSized = info.width <= 1000 && info.height <= 600;
        if (renderSized && !isSwapChainBuffer(resource)) {
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

void closeDetour(void* list) {
    gOriginalClose(list);
    // Recording the injection list calls Close too; it is not part of the engine's
    // frame, so it must not be counted as a boundary (docs/specs/vv.md §3 measures
    // the engine's ~48 lists per frame).
    if (inject::isOwnList(list)) {
        return;
    }
    if (!gDumpFrame.load(std::memory_order_relaxed)) {
        return;
    }
    unsigned const n = gCloseInFrame.fetch_add(1, std::memory_order_relaxed) + 1;
    if (n <= 64) {
        LARGE_INTEGER now{};
        QueryPerformanceCounter(&now);
        utils::logger().info("[vv] close #{} list={:#x} tid={} qpc={}", n, reinterpret_cast<std::uintptr_t>(list), GetCurrentThreadId(), now.QuadPart);
    }
}

void omSetRtvDetour(void* list, unsigned count, std::uintptr_t const* rtvs, int single, std::uintptr_t const* dsv) {
    // Diagnose the slot itself: the first calls print their raw arguments, so a
    // wrong vtable index shows up as impossible numbers instead of silently
    // producing nonsense resource pointers.
    static std::atomic<unsigned> calls{0};
    auto const                  n = calls.fetch_add(1, std::memory_order_relaxed) + 1;
    if (n <= 3) {
        utils::logger().info(
            "[vv] slot25 raw #{} this={:#x} a1={:#x} a2={:#x} a3={:#x} a4={:#x}",
            n,
            reinterpret_cast<std::uintptr_t>(list),
            static_cast<std::uintptr_t>(count),
            reinterpret_cast<std::uintptr_t>(rtvs),
            static_cast<std::uintptr_t>(static_cast<unsigned>(single)),
            reinterpret_cast<std::uintptr_t>(dsv)
        );
    }
    if (gDumpFrame.load(std::memory_order_relaxed) && rtvs && count) {
        auto* const resource = static_cast<void*>(lookupRtv(rtvs[0]));
        auto const  info     = nr::describe(resource);
        auto const  depth    = dsv ? static_cast<void*>(lookupRtv(dsv[0])) : nullptr;
        auto const  depthInfo = nr::describe(depth);
        utils::logger().info(
            "[vv] rt list={:#x} n={} handle={:#x} res={:#x} {}x{} fmt={} | depth res={:#x} {}x{} fmt={}",
            reinterpret_cast<std::uintptr_t>(list),
            count,
            static_cast<std::uintptr_t>(rtvs[0]),
            reinterpret_cast<std::uintptr_t>(resource),
            static_cast<unsigned>(info.width),
            static_cast<unsigned>(info.height),
            info.format,
            reinterpret_cast<std::uintptr_t>(depth),
            static_cast<unsigned>(depthInfo.width),
            static_cast<unsigned>(depthInfo.height),
            depthInfo.format
        );
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

void drawInstancedDetour(void* list, unsigned vertexCount, unsigned instanceCount, unsigned startVertex, unsigned startInstance) {
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

void drawIndexedInstancedDetour(void* list, unsigned indexCount, unsigned instanceCount, unsigned startIndex, int baseVertex, unsigned startInstance) {
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
    // its slots point at. Pure reads - this is what tells us whether a table measured
    // on a command list of our own can be applied to the engine's lists at all.
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
    // Only Close is hooked. Slots 12/13/25 were guessed from the published D3D12
    // vtable order and slot 25 forwards garbage into the runtime (the client died on
    // its second call), so the render-target route is parked until the index is
    // proven - see docs/O-vv-path.md §6. Close is proven: it is called ~48 times per
    // frame, once per list, with no arguments to get wrong.
    hookAt(9, reinterpret_cast<void*>(&closeDetour), reinterpret_cast<void**>(&gOriginalClose), "Close");
    // Slot 25 is NOT OMSetRenderTargets on this runtime: the first call arrives with
    // rdx = a heap pointer and r8 = a hash-like 56-bit value, i.e. (ptr) not (UINT,
    // descriptors). Forwarding it crashes inside D3D12Core.dll (2026-10-24 22:0x,
    // AV at 0x6F214130+0xCC). Do not re-enable without a *measured* table.
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

// Every printable run of at least four bytes in a block: frame-graph objects carry
// the producing pass' name as an inline string, and a name is worth more than any
// pointer for identifying which resource a dependency resolved to.
std::string printableRuns(void const* base, std::size_t bytes) {
    if (!readable(base, bytes)) {
        return {};
    }
    auto const* p = static_cast<unsigned char const*>(base);
    std::string out;
    std::size_t i = 0;
    while (i < bytes) {
        if (p[i] < 0x20 || p[i] >= 0x7F) {
            ++i;
            continue;
        }
        std::size_t j = i;
        while (j < bytes && p[j] >= 0x20 && p[j] < 0x7F) {
            ++j;
        }
        if (j - i >= 4) {
            if (!out.empty()) {
                out += '|';
            }
            out.append(reinterpret_cast<char const*>(p + i), j - i);
        }
        i = j;
    }
    return out;
}

// A qword is worth asking about only when it is a COM object whose vtable lives in
// the D3D12 runtime: that rejects heap blocks, hashes and code pointers before
// describe() ever issues a virtual call on the wrong object.
bool looksLikeD3D12Object(void const* p) {
    if (!readable(p, sizeof(void*))) {
        return false;
    }
    void* const vtable = *reinterpret_cast<void* const*>(p);
    if (!vtable || !readable(vtable, sizeof(void*))) {
        return false;
    }
    // Exact match against the vtable of a known ID3D12Resource. Until the first
    // render-target view has been seen there is no such reference, so nothing is
    // accepted at all - a false positive here is a fault inside the D3D12 runtime.
    if (vtable != gResourceVtable.load(std::memory_order_relaxed)) {
        return false;
    }
    HMODULE module{};
    if (!GetModuleHandleExW(
            GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            reinterpret_cast<LPCWSTR>(vtable),
            &module
        )
        || !module) {
        return false;
    }
    wchar_t path[MAX_PATH]{};
    if (!GetModuleFileNameW(module, path, MAX_PATH)) {
        return false;
    }
    for (wchar_t const* c = path; *c; ++c) {
        if ((*c == L'd' || *c == L'D') && c[1] == L'3' && (c[2] == L'd' || c[2] == L'D') && c[3] == L'1'
            && c[4] == L'2') {
            return true;
        }
    }
    return false;
}

// Walk a block and report every qword that already IS a texture. The four dependency
// objects are frame-graph nodes, so the real resource sits one or two hops inside;
// this finds it without knowing the layout, and `nr::describe` proves each hit with
// the resource's own description instead of trusting the pointer.
void scanForTextures(char const* tag, std::uint64_t index, char const* slot, void const* base, std::size_t bytes) {
    if (!base || !readable(base, bytes)) {
        return;
    }
    void const* seen[8]{};
    unsigned    seenCount = 0;
    for (std::size_t offset = 0; offset + 8 <= bytes; offset += 8) {
        std::uint64_t value = 0;
        peek(base, offset, value);
        auto* const candidate = reinterpret_cast<void const*>(value);
        if (!candidate || !looksLikeD3D12Object(candidate)) {
            continue;
        }
        bool duplicate = false;
        for (unsigned i = 0; i < seenCount; ++i) {
            if (seen[i] == candidate) {
                duplicate = true;
            }
        }
        if (duplicate) {
            continue;
        }
        auto const info = nr::describe(const_cast<void*>(candidate));
        if (info.width < 32 || info.height < 32) {
            continue;
        }
        if (seenCount < 8) {
            seen[seenCount++] = candidate;
        }
        utils::logger().info(
            "[vv] taau #{} {}.{} +{:#x} -> texture {:#x} {}x{} fmt={} dim={} flags={:#x}",
            index,
            tag,
            slot,
            offset,
            reinterpret_cast<std::uintptr_t>(candidate),
            static_cast<unsigned>(info.width),
            static_cast<unsigned>(info.height),
            info.format,
            info.dimension,
            info.flags
        );
    }
}

// The dependency's +0x8 is NOT a resource and NOT a bgfx handle: it is a framegraph
// node (it has its own vtable and carries pass names like "DoDeferredShadingPhase"
// in its first 0x100 bytes). Looking for a resource *at* that level can never hit -
// which is what docs/specs/vv.md §4.1 measured ("no ID3D12Resource in the dependency
// object"). What the graph actually holds is a handful of pointers inside the node,
// so the productive question is: does any qword in this small object graph equal a
// render target we already know?
//
// That comparison needs no guess about vtables or layouts, and it cannot fault: only
// pointers into committed memory are followed, and the walk has a node budget.
constexpr unsigned kNodeBytes  = 0x100;
constexpr unsigned kNodeBudget = 96;

void scanForKnownTargets(
    char const* tag,
    std::uint64_t index,
    char const*   slot,
    void const*   node,
    unsigned      depth,
    unsigned&     budget
) {
    if (!node || depth == 0 || budget == 0 || !readable(node, 8)) {
        return;
    }
    --budget;
    for (std::size_t offset = 0; offset + 8 <= kNodeBytes; offset += 8) {
        std::uint64_t value = 0;
        if (!peek(node, offset, value)) {
            return;
        }
        auto* const candidate = reinterpret_cast<void*>(value);
        if (!candidate) {
            continue;
        }
        for (unsigned i = 0; i < gSeenCount; ++i) {
            if (gSeen[i] != candidate) {
                continue;
            }
            auto const info = nr::describe(candidate);
            utils::logger().info(
                "[vv] taau #{} {}.{} node+{:#x} -> render target {:#x} {}x{} fmt={} flags={:#x}",
                index,
                tag,
                slot,
                offset,
                reinterpret_cast<std::uintptr_t>(candidate),
                static_cast<unsigned>(info.width),
                static_cast<unsigned>(info.height),
                info.format,
                info.flags
            );
            break;
        }
        auto const address = reinterpret_cast<std::uintptr_t>(candidate);
        if (depth > 1 && address > 0x10000 && address < 0x7fff00000000ULL) {
            scanForKnownTargets(tag, index, slot, candidate, depth - 1, budget);
        }
    }
}

// BindFramebufferColor<RenderContext> is opaque (the SDK header is an empty
// struct). Disassembling the shipped `taau` shows exactly what it reads: one qword
// at **+0x8** per dependency, handed to the material as the texture override value
// (docs/A §7.1 has the macOS build reading +0x10 instead - the two layouts differ,
// which is why this offset comes from the Windows disassembly and not from the Mac
// IDB). Argument order is the source order shared by both builds, and the macOS
// decompile names the consumers: argument 9 = InputTAAHistory, 10 = InputFinalColor,
// 11 = output, 12 = InputBufferMotionVectors - i.e. *not* the naive order.
//
// The +0x8 value is a pointer to whatever the framegraph resolved for this frame, so
// the dump also walks it: if the texture identity lives one hop further, the bytes
// are here to prove it.
void logDependencies(char const* tag, std::uint64_t index, void* d0, void* d1, void* d2, void* d3) {
    void*       const deps[4]  = {d0, d1, d2, d3};
    char const* const names[4] = {"history", "color", "output", "motion"};
    for (int i = 0; i < 4; ++i) {
        std::uint64_t head[3]{};
        for (std::size_t k = 0; k < 3; ++k) {
            peek(deps[i], k * 8, head[k]);
        }
        auto const resolved = reinterpret_cast<void*>(head[1]);
        std::uint64_t prefix[4]{};
        for (std::size_t k = 0; k < 4; ++k) {
            peek(resolved, k * 8, prefix[k]);
        }
        auto const tail = printableRuns(resolved, 0x100);
        // The +0x10 word is a pointer for some dependencies and a small id for
        // others, so follow it only when it is readable - the same object graph is
        // one hop deep at most.
        auto const      second      = reinterpret_cast<void*>(head[2]);
        std::string     secondText;
        if (second && second != resolved) {
            secondText = printableRuns(second, 0x100);
        }
        // The dependency object itself, the node it points at, and the +0x10 hop:
        // whichever of the three holds a texture, the scan names it with its size.
        scanForTextures(tag, index, names[i], deps[i], 0x40);
        scanForTextures(tag, index, names[i], resolved, 0x200);
        scanForTextures(tag, index, names[i], reinterpret_cast<void const*>(head[2]), 0x100);
        // And the question that actually matters for M6: is any render target we
        // already know reachable from this node by one to three pointer hops?
        unsigned budget = kNodeBudget;
        scanForKnownTargets(tag, index, names[i], deps[i], 3, budget);
        utils::logger().info(
            "[vv] taau #{} {}.{} this={:#x} id={:#x} res={:#x} +10={:#x} head={:#x},{:#x},{:#x},{:#x}"
            " resText=[{}] mod0={} secondText=[{}]",
            index,
            tag,
            names[i],
            reinterpret_cast<std::uintptr_t>(deps[i]),
            head[0],
            head[1],
            head[2],
            prefix[0],
            prefix[1],
            prefix[2],
            prefix[3],
            tail,
            moduleOf(head[0]),
            secondText
        );
    }
}

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
        // The engine truncates (1131 x 0.33 -> 373x239, not 373x240), so this must
        // truncate too; findTarget still allows a two-pixel tolerance.
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
    // The four dependencies are the only route to the textures, so dump them
    // before and after the original runs: if the output handle is only filled
    // while the pass is declared, the two lines differ and that is the answer.
    // Sampling: the first two frames plus every 2000th hit, so a run that starts in
    // the (already upscaled) main menu still produces samples from inside a world.
    bool const dumpDeps = index <= 2 || index % 2000 == 0;
    if (dumpDeps) {
        logDependencies("in", index, dep0, dep1, dep2, dep3);
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

    if (dumpDeps) {
        logDependencies("out", index, dep0, dep1, dep2, dep3);
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
    gOriginalDraw(self, context, third, fourth);
}

// One-shot diagnostic: write every render target we have seen to a BMP, so the frame
// graph can be identified by what the pixels look like instead of by guessing which
// vtable slot sets which target. Back buffers are skipped - they live in PRESENT and
// must not be transitioned by us.
void dumpCandidates() {
    auto* const device = gEngineDevice.load(std::memory_order_relaxed);
    auto* const queue  = gGameQueue.load(std::memory_order_relaxed);
    if (!device || !queue) {
        utils::logger().warn("[vv] dump: no device/queue yet");
        return;
    }
    auto const directory = utils::applicationDirectory();
    unsigned   written   = 0;
    for (unsigned i = 0; i < gSeenCount && written < 16; ++i) {
        void* const resource = gSeen[i];
        if (!resource) {
            continue;
        }
        bool excluded = false;
        for (unsigned k = 0; k < gExcludedCount; ++k) {
            if (gExcluded[k] == resource) {
                excluded = true;
            }
        }
        if (excluded) {
            continue;
        }
        auto const info = nr::describe(resource);
        if (info.width < 16 || info.height < 16) {
            continue;
        }
        wchar_t name[96]{};
        std::swprintf(
            name,
            std::size(name),
            L"vv_%ux%u_fmt%u.bmp",
            static_cast<unsigned>(info.width),
            static_cast<unsigned>(info.height),
            info.format
        );
        auto const path = directory + L"\\" + name;
        if (dumpTextureBmp(device, queue, resource, path.c_str())) {
            utils::logger().info(
                "[vv] dump {}x{} fmt={} written",
                static_cast<unsigned>(info.width),
                static_cast<unsigned>(info.height),
                info.format
            );
            ++written;
        }
    }
    utils::logger().info("[vv] dump done, {} file(s) in {}", written, "the game directory");
}

// The render target whose description matches (width, height, format). A resource is
// logged once, when its first view is created, so this is a stable lookup: the engine
// reuses these textures instead of recreating them per frame.
// Newest first: a resize rebuilds the whole render graph, so the same size/format can
// appear several times in the inventory and only the last generation is live. Reading
// the first one returns a released resource whose bytes are whatever the allocator left
// behind (observed as all zeros on 2026-10-25 01:27).
void* findTarget(unsigned width, unsigned height, unsigned format) {
    for (unsigned i = gSeenCount; i > 0; --i) {
        auto const info = nr::describe(gSeen[i - 1]);
        auto const w = static_cast<unsigned>(info.width);
        auto const h = static_cast<unsigned>(info.height);
        // The render size is computed from viewport x factor, and the engine rounds it
        // its own way; a two-pixel tolerance keeps that from hiding every input.
        // (Not named `near`: <Windows.h> still defines that as a macro.)
        auto const withinTwo = [](unsigned a, unsigned b) {
            return (a > b ? a - b : b - a) <= 2;
        };
        if (withinTwo(w, width) && withinTwo(h, height) && info.format == format) {
            return gSeen[i - 1];
        }
    }
    return nullptr;
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
    // The boundary experiment created objects on the engine's device, so it has to
    // let go of them before the probe's own hooks come down.
    inject::release();
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
        }
        // Only the engine's own frame is big enough to matter; the overlay's own
        // 1-list D3D11On12 submission arrives in the middle of the swap and must not
        // arm or disarm the sample (it did in the first cut, which is why only one
        // Close was ever logged).
        if (listCount > 8) {
            if (gDumpFrame.load(std::memory_order_relaxed)) {
                utils::logger().info(
                    "[vv] frame lists[0]={:#x} lists[1]={:#x} lists[2]={:#x} closes={}",
                    reinterpret_cast<std::uintptr_t>(lists[0]),
                    reinterpret_cast<std::uintptr_t>(lists[1]),
                    reinterpret_cast<std::uintptr_t>(lists[2]),
                    gCloseInFrame.load(std::memory_order_relaxed)
                );
                gDumpFrame.store(false, std::memory_order_relaxed);
            } else {
                unsigned const n = gBigFrames.fetch_add(1, std::memory_order_relaxed) + 1;
                // Sample one frame every 120 engine frames (~2 s at 60 fps).
                if (n % 120 == 0) {
                    gCloseInFrame.store(0, std::memory_order_relaxed);
                    gDumpFrame.store(true, std::memory_order_relaxed);
                }
            }
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
    // (see dumpCandidates) once a trigger that cannot race a resize exists.
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
#endif
    (void)gSeenCount;
    (void)gLastSeenCount;
    (void)gSeenStableSince;
    (void)gDumpPasses;
}

void onSwapChainResize() {
    resetTextureInventory();
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
    // list would let the dump touch a back buffer (which lives in PRESENT).
    void*    buffers[2]{};
    unsigned const count = swapChainBuffers(swapChain, buffers, 2);
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
}

void setGameQueue(void* queue) { gGameQueue.store(queue, std::memory_order_relaxed); }

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
