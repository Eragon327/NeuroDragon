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

// Diagnostic readback: records a full copy of 'color' into an internal readback
// buffer on the given command list (one shot). flushColorDump() then maps that
// buffer on a later frame and writes a BMP next to the game executable.
void queueColorDump(void* commandList, void* color, char const* fileName);
// Same, but the stored values are already display-encoded (no transform on write).
void queueColorDumpEncoded(void* commandList, void* color, char const* fileName);
// Stored values are linear but already in [0,1] (gamma only, no tone map).
void queueColorDumpLinear(void* commandList, void* color, char const* fileName);
[[nodiscard]] bool flushColorDump(float* maxValue, std::uint32_t* nonZeroWords);

// ---------------------------------------------------------------------------
// Stage 2: NR (NGX feature 0x12) at the engine's render resolution.
//
// Contract, from TRP and the stage-1 measurements (docs/D §6):
//   * DLSSNR.Width/Height are the feature's OUTPUT size, and the Color subrect
//     must match them 1:1 - so this is a same-resolution pass at 1485x835,
//     written into the top-left corner of a 2560x1440 allocation that can later
//     be handed to the engine's DLSS as "Color".
//   * Depth/MV are guides: their subrect is the full texture (1485x836).
//   * Jitter/exposure are not part of the NR parameter table.
// ---------------------------------------------------------------------------

// Sensitivity test hook: the probe tells us which window is running so the
// parameter sweep can vary one knob per window (docs/D §6.14 experiment).
void nrSetWindowIndex(int index);

// Directory that holds this mod's DLL (LeviLamina's Mod::getModDir(), passed in
// by the probe so this translation unit stays free of engine headers). The NR
// runtime is loaded from here under its original file name.
void nrSetModDirectory(wchar_t const* directory);

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

// The private 2560x1440 output texture; only the top-left width x height is
// written by the feature.
[[nodiscard]] void* nrOutputResource();

// The tone-mapped, gamma-encoded copy of Color that the network is fed (item
// "HDR -> LDR encode"). Null until nrCreate has built the encoder.
[[nodiscard]] void* nrEncodedColorResource();

// True when nr_params.txt asked for Replace (route A). The probe then evaluates
// NR on every in-world frame instead of only inside the measurement windows, so
// the picture can actually be judged by eye.
[[nodiscard]] bool nrReplaceContinuous();

// ---------------------------------------------------------------------------
// Our own DLSS SR (Replace=7). Same structure as TRP: instead of re-pointing the
// engine's DLSS at our texture (which froze the picture - NGX binds the feature's
// input resource at CreateFeature), we create feature 1 ourselves, feed it our
// composited image plus the engine's Depth/MV, and write into a texture we own.
// The engine's own DLSS keeps running untouched in this step: the result is only
// dumped, so a mistake cannot freeze or black the picture.
// ---------------------------------------------------------------------------
// The private 2560x1440 SR output; null until the feature exists.
[[nodiscard]] void* nrSrOutputResource();
[[nodiscard]] bool  nrSrIsCreated();
[[nodiscard]] int   nrReplaceMode();
// ParamLog=1 in nr_params.txt: the probe logs every engine NGX parameter write.
[[nodiscard]] bool  nrParamLogEnabled();
// Call on entering a world: resets our own SR history so the previous world's
// image does not follow the player.
void nrNotifyLevelLoaded();
// True when our own SR already wrote the engine's Output this frame, so the
// caller must skip the engine's EvaluateFeature.
[[nodiscard]] bool  nrSrOwnsOutput();

// Hand over the *original* NGX CreateFeature/EvaluateFeature (the detours' next
// pointers). Our own SR must call those, never the patched exports: the patched
// entry point is our detour, so calling it recurses until the stack dies.
void nrSetNgxOriginals(void* createFeature, void* evaluateFeature);

// Records one evaluation on the game's command list. Handles every resource
// state transition around the NGX call. `engineParams` (may be null) is the
// engine's own NGX parameter object: when nr_params.txt asks for Replace=5 it is
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

// Route A (docs/D §6.16): make the engine's own DLSS consume our texture, so the
// picture on screen contains NR pixels. `replace=1` decodes the NR result first
// (gamma + inverse Reinhard); `replace=2` is the mechanism test: it hands DLSS a
// faithful *copy* of the engine's own Color from a foreign texture, which shows
// whether NGX accepts a swapped resource at all. Call after nrRecord() and before
// the engine's own EvaluateFeature. Returns false when it could not swap.
bool nrReplaceEngineColor(void* commandList, void* engineColor, void* engineParams);

// Second half of Replace=3 (docs/D §6.16): give the engine's own Color pixels
// back right after its EvaluateFeature returned. Same command list; no-op when
// nothing was borrowed.
void nrRestoreColor(void* commandList);

// Releases the feature and every GPU object this translation unit owns. Called
// from the probe's uninstall(): a detour or a live D3D12 object left behind by an
// unloaded module is what crashed the client on disable (2026-10-21).
void nrShutdown();

// Level: 0 = info, 1 = warn, 2 = error. The D3D12 translation unit stays free
// of engine headers, so logging is injected from the probe instead.
using NrLogFn = void (*)(int level, char const* message);
void nrSetLogger(NrLogFn fn);

// GPU timestamps. id 0 brackets our NR evaluation, id 1 the engine's DLSS, so
// both numbers come from the same run. Timing is harvested a few frames later:
// there is no fence on the game's queue, so pairs that read as unfinished (zero,
// backwards, absurd length) are dropped and counted instead of being used.
void nrStampBegin(void* commandList, std::uint64_t frame, int id);
void nrStampEnd(void* commandList, std::uint64_t frame, int id);
void nrStampTick(std::uint64_t frame);
void nrStampReport();
void nrStampReset();

} // namespace neurodragon::nr
