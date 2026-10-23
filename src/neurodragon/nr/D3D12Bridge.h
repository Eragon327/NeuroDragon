#pragma once

#include <cstdint>

// Isolated D3D12 knowledge for the probe: the engine hands us "resource"
// pointers taken from NGX parameters, and we must prove what they are without
// ever calling into a pointer we do not recognise.
//
// This header is safe to include from a translation unit that also includes
// LeviLamina / engine headers (<d3d12.h> stays inside D3D12Bridge.cpp, per
// docs/D §4 pitfall 5).

namespace neurodragon::nr {

struct ResourceInfo {
    void*         device        = nullptr;
    std::uint64_t width         = 0;
    std::uint64_t height        = 0;
    std::uint32_t dimension     = 0;
    std::uint32_t format        = 0;
    std::uint16_t depthOrArray  = 0;
    std::uint16_t mips          = 0;
    std::uint32_t sampleCount   = 0;
    std::uint32_t sampleQuality = 0;
    std::uint32_t flags         = 0;
};

// Returns zero-initialised info when the pointer is null, unreadable, or does
// not look like a COM object backed by an image mapping.
[[nodiscard]] ResourceInfo describe(void* resource);

// ---------------------------------------------------------------------------
// Colour dumps - TEST build only (src/neurodragon/Test.h).
//
// Diagnostic readback: records a full copy of 'color' into an internal readback
// buffer on the given command list (one shot). flushColorDump() then maps that
// buffer on a later frame and writes a BMP next to the game executable. None of
// this exists in a release DLL.
// ---------------------------------------------------------------------------
#ifdef TEST
void queueColorDump(void* commandList, void* color, char const* fileName);
// Same, but the stored values are already display-encoded (no transform on write).
void queueColorDumpEncoded(void* commandList, void* color, char const* fileName);
// Stored values are linear but already in [0,1] (gamma only, no tone map).
void               queueColorDumpLinear(void* commandList, void* color, char const* fileName);
[[nodiscard]] bool flushColorDump(float* maxValue, std::uint32_t* nonZeroWords);

// Second independent dump slot: queue/flush one more resource on the very same
// frame, so two textures can be compared without camera motion in between.
void               queueColorDump2(void* commandList, void* resource, char const* fileName);
void               queueColorDumpEncoded2(void* commandList, void* resource, char const* fileName);
[[nodiscard]] bool flushColorDump2(float* maxValue, std::uint32_t* nonZeroWords);

// Generic slot form (0..3): capture several resources on the SAME frame. Domain is
// 0 = linear HDR, 1 = display-encoded, 2 = linear in [0,1].
void               queueColorDumpN(int slot, void* commandList, void* resource, char const* fileName, int domain);
[[nodiscard]] bool flushColorDumpN(int slot, float* maxValue, std::uint32_t* nonZeroWords);
#endif

// ---------------------------------------------------------------------------
// Stage 2: NR (NGX feature 0x12) at the engine's render resolution.
//
// Contract, from TRP and the stage-1 measurements (docs/D §6):
//   * DLSSNR.Width/Height are the feature's working size and the Color subrect must
//     match them 1:1 - so this is a same-resolution pass at the engine's current
//     render size (1485x835 on a 2560x1440 output, but that is just one sample of
//     it), written into the top-left corner of a Color-sized allocation that can
//     later be handed to the engine's DLSS as "Color".
//   * Depth/MV are guides: their subrect is the full texture.
//   * Jitter/exposure are not part of the NR parameter table.
// ---------------------------------------------------------------------------

// Sensitivity test hook: the probe tells us which window is running so the
// parameter sweep can vary one knob per window (docs/D §6.14 experiment).
void nrSetWindowIndex(int index);

#ifdef TEST
// Directory that holds this mod's DLL (LeviLamina's Mod::getModDir(), passed in
// by the probe so this translation unit stays free of engine headers). Only the
// dump writer reads it.
void nrSetModDirectory(wchar_t const* directory);
#endif

// Creates the private output texture, the timestamp ring and feature 0x12.
// 'engineParams' is only used to prove that our capability parameter object has
// the same vtable as the one the engine already writes through; 'createFeature'
// and 'evaluateFeature' must be the *original* NGX exports (never our detours).
// Every NGX return code is logged with an [nr] tag.
bool nrCreate(
    void*    commandList,
    void*    colorResource,
    unsigned width,
    unsigned height,
    void*    engineParams,
    void*    createFeature,
    void*    evaluateFeature
);

// What the engine passed to its own NVSDK_NGX_D3D12_Init* call, captured by the
// probe. The runtime's Init_Ext has to name the same application (and, in
// practice, the same application data path) that the core was initialised for;
// without it the model answers PlatformError for every entry point.
void nrSetEngineInit(unsigned long long appId, wchar_t const* dataPath);

[[nodiscard]] bool nrIsCreated();

#ifdef TEST
// ---------------------------------------------------------------------------
// Diagnostic accessors - TEST build only, used by the dump schedule.
// ---------------------------------------------------------------------------

// The private 2560x1440 output texture; only the top-left width x height is
// written by the feature.
[[nodiscard]] void* nrOutputResource();

// The resource we most recently pointed the engine's own DLSS "Color" at, and
// whether its pixels are display-encoded (1) or linear HDR (0). Null until a
// Replace mode actually swaps. Diagnostic only: dumped so a frozen picture can
// be attributed to our write versus the upscaler.
[[nodiscard]] void* nrFedResource();
[[nodiscard]] int   nrFedDomain();

// The tone-mapped, gamma-encoded copy of Color that the network is fed (item
// "HDR -> LDR encode"). Null until nrCreate has built the encoder.
[[nodiscard]] void* nrEncodedColorResource();

// nr.paramLog in config.json: the probe logs every engine NGX parameter write.
[[nodiscard]] bool nrParamLogEnabled();
#endif

// True when config.json asks for Replace (route A). The probe then evaluates
// NR on every in-world frame instead of only inside the measurement windows, so
// the picture can actually be judged by eye.
[[nodiscard]] bool nrReplaceContinuous();

// Called on entering a world: the engine never resets its own DLSS, so one
// one-shot Reset=1 is sent to its parameter object at the first swap after a load.
void nrNotifyLevelLoaded();

// Keeps the feature on the engine's current render size.
//
// The engine configures its own DLSS per resolution: when the window size or the
// upscaling quality changes it hands NR a different Width/Height (1485x835 was only
// the first size ever measured). An NGX feature is created for one size, so the
// only way to follow is to build a replacement: this creates it first (releasing the
// last feature would make the driver unload the 264 MB model), swaps the textures in
// when they no longer fit, and shows the picture once the new feature exists.
// Retired objects are only released after the GPU is idle. Cheap when the size has
// not changed (two compares), so the probe can call it every in-world frame.
bool nrEnsureRenderSize(void* commandList, void* color, unsigned width, unsigned height);

// The game's D3D12 direct queue, captured by the overlay's ExecuteCommandLists
// hook. Only used to wait for the GPU before releasing retired objects; when it is
// null those objects are kept instead (a leak is cheaper than a driver crash).
void nrSetGameQueue(void* queue);

// Records one evaluation on the game's command list. Handles every resource
// state transition around the NGX call. `engineParams` (may be null) is the
// engine's own NGX parameter object: when config.json asks for Replace=5 it is
// pointed at our gain-composited texture (docs/D §6.16).
//
// NR stays 1:1 at the render resolution; it never upscales.
bool nrRecord(
    void*    commandList,
    void*    color,
    void*    depth,
    void*    motionVectors,
    unsigned guideWidth,
    unsigned guideHeight,
    float    motionVectorScaleX,
    float    motionVectorScaleY,
    int      depthInverted,
    bool     reset,
    void*    engineParams,
    void*    engineOutput
);

// Releases the feature and every GPU object this translation unit owns. Called
// from the probe's uninstall(): a detour or a live D3D12 object left behind by an
// unloaded module is what crashed the client on disable (2026-10-21).
void nrShutdown();

// Switches NR off until it is enabled again: the feature, the output texture and the
// HDR<->LDR textures are retired (released once the GPU is idle) and creation is
// re-armed, so the next enabled frame builds everything again. Called when
// nr.enabled turns false - an idle feature keeps the 264 MB model and its history
// buffers resident, so merely skipping the evaluation is not "off".
void nrUnload();

// GPU timestamps. id 0 brackets our NR evaluation, id 1 the engine's DLSS, so
// both numbers come from the same run. Timing is harvested a few frames later:
// there is no fence on the game's queue, so pairs that read as unfinished (zero,
// backwards, absurd length) are dropped and counted instead of being used.
void nrStampBegin(void* commandList, std::uint64_t frame, int id);
void nrStampEnd(void* commandList, std::uint64_t frame, int id);
void nrStampTick(std::uint64_t frame);
void nrStampReport();
// Drop the raw samples (the measurement machinery calls this per window).
void nrStampReset();
// Forget the published medians too - only the panel's "clear samples" button
// should do this; resetting them per window is what made the panel read "no
// samples" forever.
void nrStampClearReported();

// ---------------------------------------------------------------------------
// Read-only snapshot for the in-game panel (src/neurodragon/ui).
// Every number is either a size the last frame actually used or a GPU median
// published atomically once per measurement window; nothing here is a guess.
// ---------------------------------------------------------------------------
struct PanelStats {
    bool     featureCreated = false;
    bool     stampsActive   = false; // false = GPU timing unavailable this run
    unsigned evalCount      = 0;
    int      lastError      = 0;
    unsigned renderWidth    = 0; // NR render size (1:1 with the engine's Color)
    unsigned renderHeight   = 0;
    unsigned outputWidth    = 0; // engine DLSS output (the presented picture)
    unsigned outputHeight   = 0;
    unsigned nrSamples      = 0; // samples behind nrMs
    unsigned engineSamples  = 0; // samples behind engineMs
    bool     hasNr          = false;
    bool     hasEngine      = false;
    double   nrMs           = 0.0;
    double   engineMs       = 0.0;
};

[[nodiscard]] PanelStats nrPanelStats();

} // namespace neurodragon::nr
