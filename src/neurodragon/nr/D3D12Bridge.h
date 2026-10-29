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
// `engineParams` is only used to prove that our capability parameter object has
// the same vtable as the one the engine already writes through; 'createFeature'
// and 'evaluateFeature' must be the *original* NGX exports (never our detours).
// `outputResource` is the engine's own DLSS Output; `placement` (docs/N) selects
// 0 = NR before the upscaler (Color/render size) or 1 = after it (Output/display
// size). A refused after create falls back to before for the session.
// Every NGX return code is logged with an [nr] tag.
bool nrCreate(
    void*    commandList,
    void*    colorResource,
    unsigned width,
    unsigned height,
    void*    outputResource,
    int      placement,
    void*    engineParams,
    void*    createFeature,
    void*    evaluateFeature
);

// What the engine passed to its own NVSDK_NGX_D3D12_Init* call, captured by the
// probe. The runtime's Init_Ext has to name the same application (and, in
// practice, the same application data path) that the core was initialised for;
// without it the model answers PlatformError for every entry point.
void nrSetEngineInit(unsigned long long appId, wchar_t const* dataPath);

// Whether the engine's own NGX Init was observed in this process, and what it
// passed. The VV path needs this before it may call into the core at all: the
// engine owns the session, and a `GetCapabilityParameters` on a core that was
// never initialised has no defined answer (docs/specs/vv.md §13, probe P0.1).
// `dataPath` is optional and copied up to `capacity` wide characters.
[[nodiscard]] bool nrEngineInitCaptured(
    unsigned long long* appId,
    wchar_t*            dataPath,
    unsigned            capacity
);

[[nodiscard]] bool nrIsCreated();

// Effective placement (docs/N): 0 = before upscaling, 1 = after upscaling.
// Returns the requested value before the first create; after a refused after
// create it stays 0 for the session (query nrAfterFallback for the reason).
[[nodiscard]] int nrPlacement();

// True when the requested "after" placement was refused by the runtime and the
// session fell back to "before". The panel shows this instead of pretending the
// switch took effect.
[[nodiscard]] bool nrAfterFallback();

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

// Domain of the resource above: 0 = linear HDR, 1 = display-encoded.
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

// One-shot temporal wipe for both sides (NR's DLSSNR.Reset and the engine DLSS's
// unprefixed Reset). Bound to ui.resetKey by default; the next evaluation consumes
// it, so holding the key does not erase the history every frame.
void nrRequestReset();

// Gives the engine's own Color resource back to its parameter object. Called when NR
// is switched off (panel Apply / hotkey) and before unloading: the object can still
// be pointing at our texture, which is a frozen picture at best and a released
// resource at worst. `engineParams` / `engineColor` may be null: the values from the
// last swap are used then.
void nrRestoreEngineColor(void* engineParams, void* engineColor);

// Keeps the feature on the engine's current sizes and on the configured placement.
//
// Two extents exist (docs/N): the *domain* NR processes (Color/render size before
// upscaling, engine Output/display size after) and the *guide* extent (Depth/MV,
// always the engine render size). A feature is created for one work extent, so the
// only way to follow a resize or a placement switch is to build a replacement:
// this creates it first (releasing the last feature would make the driver unload
// the 264 MB model), swaps the textures in when they no longer fit, and shows the
// picture once the new feature exists. A refused after create falls back to before
// for the session. Retired objects are only released after the GPU is idle. Cheap
// when nothing changed (a few compares), so the probe can call it every frame.
bool nrEnsureRenderSize(
    void*    commandList,
    void*    color,
    unsigned guideWidth,
    unsigned guideHeight,
    void*    output,
    int      requestedPlacement
);

// Engine-side frame facts for the panel, published from the evaluation hook before
// any NR decision. Keeps the monitor alive when NR is switched off or unloaded - the
// feature must not be the only source of "what size is this frame".
void nrPublishEngineFrame(unsigned renderWidth, unsigned renderHeight, void* output);

// Creates the GPU timestamp objects if they do not exist yet. Called from the
// evaluation hook, so the engine's own DLSS timing also works without NR.
void nrEnsureStamps(void* commandList);

// The game's D3D12 direct queue, captured by the overlay's ExecuteCommandLists
// hook. Only used to wait for the GPU before releasing retired objects; when it is
// null those objects are kept instead (a leak is cheaper than a driver crash).
void nrSetGameQueue(void* queue);

// Records one evaluation on the game's command list. Handles every resource
// state transition around the NGX call. `engineParams` (may be null) is the
// engine's own NGX parameter object: it is pointed at our ratio-resolved texture
// after the composite (docs/D §6.16).
//
// NR stays 1:1 at the render resolution; it never upscales.
//
// `frame` is the engine frame counter the probe uses for its own brackets; the
// stage timestamps inside this function (encode / evaluate / composite) use the
// same ring, so the three lines the panel shows belong to the same frame.
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
    void*    engineOutput,
    std::uint64_t frame
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

// Set by the panel's "Save as default" when the persisted config has NR off; the
// render thread consumes it at the next evaluation and calls nrUnload(). This is the
// only path that unloads - "Apply" keeps the feature resident so toggling NR while
// tuning stays instant.
void nrRequestUnload();

// True once per request (the flag is cleared).
[[nodiscard]] bool nrTakeUnloadRequest();

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
    unsigned evalFailures   = 0; // telemetry (docs/L 3.3)
    int      lastError      = 0;
    unsigned renderWidth    = 0; // guide extent (engine render size, Depth/MVec)
    unsigned renderHeight   = 0;
    unsigned outputWidth    = 0; // engine DLSS output (the presented picture)
    unsigned outputHeight   = 0;
    // Placement axis (docs/N): effective side, the extent NR actually processes
    // (render size before, Output size after) and the guide extent. The panel
    // shows them so a switch that did nothing is visible immediately.
    int      placement      = 0; // 0 = before, 1 = after
    bool     afterFallback  = false;
    unsigned domainWidth    = 0;
    unsigned domainHeight   = 0;
    unsigned guideWidth     = 0;
    unsigned guideHeight    = 0;
    unsigned nrSamples      = 0; // samples behind nrMs
    unsigned engineSamples  = 0; // samples behind engineMs
    bool     hasNr          = false;
    bool     hasEngine      = false;
    double   nrMs           = 0.0;
    double   engineMs       = 0.0;

    // Stage split inside nrMs (batch 1, docs/L 1.7). encode + evaluate + composite
    // are measured with their own timestamp pairs on the same frame as nrMs, so
    // the three should add up to roughly nrMs (the rest is our own bookkeeping).
    bool     hasEncode      = false;
    bool     hasEvaluate    = false;
    bool     hasComposite   = false;
    unsigned encodeSamples  = 0;
    unsigned evaluateSamples = 0;
    unsigned compositeSamples = 0;
    double   encodeMs       = 0.0;
    double   evaluateMs     = 0.0;
    double   compositeMs    = 0.0;

    // RCAS sharpening (src/neurodragon/sr). Recorded only while it is on, so these
    // stay empty when the panel shows the switch off.
    unsigned rcasSamples    = 0;
    bool     hasRcas        = false;
    double   rcasMs         = 0.0;
};

// What the last evaluation actually wrote into the NGX parameter object, plus the
// composite mode it picked (batch 1, docs/L 1.2). This is a read-back, not a
// guess: the panel shows these numbers next to the knobs so a switch that does
// nothing is visible immediately.
struct NrFrameParams {
    bool  valid        = false;
    int   replace      = 0;  // 6 for every frame now: the ratio resolve is the only composite
    int   compositeMode = -1; // 4 = ratio resolve at native size, 6 = the same when the
                              // network ran at a smaller extent, -1 = no composite
    int   encodeMode   = -1;  // 0/1/3, -1 = raw linear HDR (no encode)
    int   style        = 0;
    int   networkPreset = 0; // 0 = default network, 1 = previous (P2 probe)
    float intensity    = 0.0f;
    float localTone    = 0.0f;
    float localStructure = 0.0f;
    float skinStructure = 0.0f;
    int   reset        = 0;
    float mvScaleX     = 0.0f;
    float mvScaleY     = 0.0f;
    bool  compare      = false;
    float compareSplit = 0.0f;
    // Broad/detail weights of the ratio resolve (1.0 = unchanged).
    float broad        = 1.0f;
    float detail       = 1.0f;
    // Adaptive exposure (docs/L 3.2): the switch and its EV bias. The white point
    // itself is computed on the GPU and is not read back.
    bool  adaptiveExposure = false;
    float exposureBias     = 0.0f;
    // Input scaling (docs/L 2.1): the percentage actually in effect (100 = native)
    // and the two extents, so the panel shows the fallback when a scaled create was
    // refused instead of echoing the requested value.
    int      inputPercent  = 100;
    unsigned featureWidth  = 0;
    unsigned featureHeight = 0;
    // Placement axis (docs/N): effective side, the extent NR processed (render
    // size before, Output size after) and the guide extent. `renderWidth/Height`
    // stay the engine render size (the guide), so the panel can show all three.
    int      placement     = 0;
    bool     afterFallback = false;
    unsigned domainWidth   = 0;
    unsigned domainHeight  = 0;
    unsigned renderWidth   = 0;
    unsigned renderHeight  = 0;
    unsigned guideWidth    = 0;
    unsigned guideHeight   = 0;
    bool  rendered     = false; // false until the first completed evaluation
};

[[nodiscard]] NrFrameParams nrFrameParams();

// Engine SR (DLSS feature 1) read-back for the "SR" tab. Everything here is
// observed, never applied: the callback wrapper forwards the engine's own
// optimal-settings answer unchanged and only records it (docs/I §10).
struct SrStats {
    bool     wrapped       = false;
    unsigned callbackCount = 0;
    unsigned cbDisplayW    = 0;
    unsigned cbDisplayH    = 0;
    unsigned cbRenderW     = 0;
    unsigned cbRenderH     = 0;
    unsigned cbQuality     = 0;
    float    cbSharpness   = 0.0f;
    unsigned dynMinW       = 0;
    unsigned dynMinH       = 0;
    unsigned dynMaxW       = 0;
    unsigned dynMaxH       = 0;
    unsigned quality       = 0;
    unsigned flags         = 0;
    unsigned preset        = 0;
    float    sharpness     = 0.0f;
    // True while a forced feature rebuild is in flight (plus the settle window).
    // The engine creates the feature with the *temporary* quality slot during this
    // period, so `quality`/`preset` are not a result yet and the panel must not show
    // them as one (docs/J §6: the transient slot was still occasionally visible).
    bool     rebuilding    = false;

    // ---- telemetry (docs/K §2.2 ③, docs/J §8) --------------------------------
    // Engine SR evaluations seen and how many did not return 1. A rising failure
    // count is the cheapest explanation for "SR stopped working" and needs no log.
    unsigned evalCount     = 0;
    unsigned evalFailures  = 0;
    // Format of the engine's own `Color` texture (DXGI_FORMAT as unsigned; the panel
    // maps the common ones) and the jitter the engine is writing this frame.
    unsigned inputFormat      = 0;
    bool     inputFormatValid = false;
    float    jitterX          = 0.0f;
    float    jitterY          = 0.0f;

    // ---- self-check (docs/I §7 risk register) -------------------------------
    // Whether each hook SR depends on is actually installed. A game update that
    // moves a signature now shows up as one failed row instead of a silent no-op.
    bool     callbackWrapped    = false; // optimal-settings callback replaced
    bool     frameContextHook   = false; // per-frame ctx copy hooked
    bool     factorTable        = false; // FrameBuilder factor table located
    bool     createOverridesRan = false; // CreateFeature detour saw the SR feature
};

// Engine upscaling configuration, read from the per-frame render context
// (base+0xdfc53c0 args) and the renderer config object (base+0x3c564xx).
// Everything here is measured, not guessed: the panel shows both sides so a
// write can be checked against what the engine asked for (docs/I §11).
struct SrEngineStats {
    bool     valid      = false;
    unsigned calls      = 0;
    unsigned ctxMode    = 0;
    unsigned ctxQuality = 0;
    unsigned ctxBools   = 0;
    float    ctxFactor  = 0.0f;
    float    ctxExtra   = 0.0f;
    unsigned activeQ    = 0;
    unsigned requestedQ = 0;
    float    activeF    = 0.0f;
    float    requestedF = 0.0f;
    unsigned renderW    = 0;
    unsigned renderH    = 0;
    float    tailFactor = 0.0f;
};

[[nodiscard]] SrEngineStats srEngineStats();

// Factor behind a percentage, matching the engine's own upscaling table:
// 33 -> 0.3334, 50 -> 0.5, 58 -> 0.58, 66 -> 0.667, 75 -> 0.75, 100 -> 1.0.
// 0 for "follow the engine" or an unknown value.
float srFactorForPercent(int percent);

// DLSS quality slot that produces the requested ratio: 33/50/58/66/100 map to
// the engine's native slots 3/0/1/2/5. -1 = no override (follow the engine).
int srQualityForPercent(int percent);

// Applies the panel's DLSS model preset and sharpness override to the engine's
// CreateFeature parameter object. Called from the CreateFeature detour before the
// original runs; preset changes only take effect on the next feature rebuild.
void srApplyCreateOverrides(void* params, int feature);

// Forces the engine to rebuild its DLSS feature, which is what applies a new
// model preset (it is only read at CreateFeature). The render size is pinned
// during the rebuild, so there is no visible resolution change.
void srRequestFeatureRebuild();

// Called at the top of the engine's per-frame frame-context copy. Publishes the
// measured values and, when the config asks for it, rewrites the quality slot
// and/or the upscaling factor before the engine copies them into the renderer.
void srDriveFrameContext(void* engine, void* ctx);

// Per-evaluate request write: pushes the configured quality/factor into the
// renderer's *requested* fields so the engine's own sizing function notices the
// change and rebuilds the DLSS feature/render size on its next pass. This is the
// path that does not need a window resize; it becomes live once the renderer
// object has been seen by srDriveFrameContext at least once.
void srDriveEngineRequest();

[[nodiscard]] SrStats srPanelStats();

// Installs the read-only wrapper into an NGX parameter object. Called wherever
// an engine parameter object becomes visible (docs/I §10.3): GetCapabilityParameters,
// GetParameters and CreateFeature.
void srInstallCallback(void* params);

// Publishes the engine SR parameters the SR tab shows and applies a pending
// one-shot Reset. Called once per engine evaluation.
void srReadEngineParams(void* params);

// One-shot history wipe for the engine's DLSS: writes Reset=1 for one frame,
// then back to 0 (the same key the engine uses when a world loads).
void srRequestReset();

// ---- self-check / telemetry writers (called from the probe) -----------------
// The panel lists the hooks SR depends on, so a game update that moves a signature
// reads as one failed row instead of "SR silently does nothing" (docs/I §7).
void srSetFrameContextHook(bool ok);
void srSetFactorTable(bool ok);
// Engine SR health: called with the engine's own EvaluateFeature return value.
void srReportEvaluation(int result);

[[nodiscard]] PanelStats nrPanelStats();

} // namespace neurodragon::nr
