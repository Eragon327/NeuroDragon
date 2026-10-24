#pragma once

#include <cstdint>
#include <string>

namespace neurodragon::config {

// Runtime settings, stored as <mod dir>/config/config.json through ll::config
// (nlohmann::json via boost::pfr aggregate reflection): every field below is a
// real key. There is no migration from older formats - delete the file and the
// defaults below are written again. `version` is required by ll::config's
// concept; bump it only when a key's meaning changes.
struct Nr {
    bool enabled = false;

    // 5 = gain composite onto the engine's DLSS input, 6 = TRP ratio resolve (default:
    // the one that reaches the screen with highlights taken from the original).
    // No other value has a meaning any more (the old "evaluate only" 0 is gone): the
    // renderer treats everything except 5 as 6, deliberately without a config migration.
    int replace = 6;

    float gainBoost = 1.0f; // exaggerate the network's gain, for judging only

    int   style           = 0;
    float intensity       = 1.0f;
    float localToneStrength = 1.0f;
    float localStructureStrength = 1.0f;
    // Runtime picker (docs/L 3.1): file name in the mod directory that is materialised
    // next to the game executable as nvngx_dlssnr.dll. Empty = the canonical name.
    // Read at load, so a change takes effect on the next start.
    std::string modelFile;

    // DLSSNR.SkinStructureStrength: 0..2, -1 = follow LocalStructureStrength
    // (the 310.x sentinel). Probed with Sweep=3 (docs/L P1): on Minecraft content
    // there was no visible difference at -1/0/1, and TRP ties this knob to its
    // automatic skin mask, which needs skin classification our inputs cannot carry.
    // Kept for the TEST build's probe; 1.0 = the old hardcoded value.
    float skinStructure   = 1.0f;
    int   depthInverted   = -1; // -1 = follow what the engine asked for

    // NR input scaling (docs/L batch 2): 50 / 66 / 100. The feature's work extent
    // becomes render size x percent while depth/MV guides stay full resolution and
    // the composite still writes the full-resolution picture - the 310.x contract
    // TRP uses. Anything else means 100.
    int   inputPercent    = 100;

    // DLSSNR.Hint.Render.Preset: 0 = default network, 1 = the previous one. Only
    // read at CreateFeature, so a change needs a restart (docs/L P2).
    int   networkPreset   = 0;

    // Split comparison (batch 1): 0 = off, 1 = the left part of the picture keeps
    // the engine's original colour while the right part is composited. Purely a
    // diagnostic - it is the only way to judge what NR did to a frame. Session-only:
    // loadOrCreate() clears it, so a forgotten seam never survives a restart (TRP's
    // Compare is session-only for the same reason).
    int   compare         = 0;
    float compareSplit    = 0.5f; // 0..1 of the width that stays original

    int sweep = 0; // 1 = cycle Style 0..2 per window, 2 = Enabled freeze test

    float mvecScale = -1.0f; // < 0 = derive from the render size (recommended)
    int   nrEncode  = -1;    // < 0 = per Replace mode (0 = Reinhard, 3 = TRP)

    // TRP's reconstruction knobs (docs/D §6.16, "ratio resolve").
    float whitePoint       = 1.0f;
    float transferStrength = 1.0f;
    float colourStrength   = 1.0f;
    float maxRatio         = 2.0f;
    // TRP's "broad / detail" split of NR's change (in stops): the wide part of the
    // change and its local contrast can be scaled independently. 1.0 / 1.0 leaves the
    // result unchanged; only Replace=6 (ratio resolve) uses them.
    float broad            = 1.0f;
    float detail           = 1.0f;
    // Adaptive exposure (docs/L 3.2): derive the white point from the scene's geometric
    // mean luminance instead of the constant above, so indoor and outdoor scenes reach
    // NR at a similar brightness. The bias is in EV (+ brightens NR's view).
    bool  adaptiveExposure = false;
    float exposureBias     = 0.0f;

    bool resetEveryFrame = false;   // NRReset: kills NR temporal history, diagnostic only
    bool srResetEveryFrame = false; // SRReset: wipes the engine DLSS history every frame, diagnostic only
    bool paramLog        = false; // TEST builds only: log every engine NGX parameter write
};

// Engine SR (DLSS feature 1) knobs. The internal resolution has two sides that
// must agree: the DLSS quality slot (which the engine turns into its input size)
// and the frame builder's scene factor (which the world render rect follows).
// One percentage drives both; the panel's choice applies immediately, without a
// window resize (docs/I §11).
struct Sr {
    // 0 = follow the engine (Balanced 58% in practice). 33/50/58/66/100 = render
    // and upscale at that ratio: the DLSS quality slot is set accordingly and the
    // frame builder's factor table entry is patched to the same value. Only the
    // ratios DLSS has a native slot for are offered (no 75%).
    int scalePercent = 0;

    // DLSS model preset request (NGX DLSS.Hint.Render.Preset.*): 0 = driver
    // default, 5/6 = legacy E/F, 10 = J, 11 = K, 12 = L, 13 = M. Written into all
    // six per-slot hints at CreateFeature, keeping the 0x80000000 bit the engine
    // sets. 0 is written too, so "driver default" actually clears a previous
    // choice (the parameter object is reused across feature builds).
    int preset = 0;

    // RCAS sharpening of the frame the engine's DLSS produced (docs/K §2.2 ①,
    // docs/J §5: the DLSS "Sharpness" parameter is dead, so this is a real pass).
    // Runs after DLSS and before the engine's colour grading/UI, in a Reinhard-
    // encoded domain because the DLSS output is linear HDR (src/neurodragon/sr/
    // SrSharpening.h). Off by default: an image-changing post-process is opt-in.
    bool sharpening = false;
    // 0..1, TRP's default. Ignored while `sharpening` is false; 0 also records
    // nothing, so "on but 0" costs exactly what "off" costs. The value scales the
    // RCAS delta by 0..4x (see sr/SrSharpening.h), not only the lobe: the first
    // version scaled the lobe like TRP and measured 0.38/255 mean change.
    float sharpness = 0.672f;
};

struct Config;

// Panel settings. Like everything else here: read once at load, written only by
// "Save as default" in the panel.
struct Ui {
    // Virtual-key code of the panel hotkey. 0x23 = VK_END.
    int openKey = 0x23;

    // In-game NR hotkeys, live while the panel is closed. 0xDB / 0xDD are '[' / ']'
    // on a US layout (VK_OEM_4 / VK_OEM_6); 0 disables one.
    int nrKey    = 0xDB; // NR on/off
    int resetKey = 0xDD; // wipe NR + engine DLSS temporal history once

    // Virtual-key code of the "save the finished frame" screenshot key.
    // 0x71 = VK_F2 (Bedrock does not use it). The PNG goes to <version>/screenshots/
    // (the game root, next to the executable). The panel's Advanced tab rebinds it
    // like the other keys; 0 disables the key.
    int screenshotKey = 0x71;
};

struct Config {
    int version = 1;
    Nr  nr{};
    Sr  sr{};
    Ui  ui{};
};

Config& get();

// Loads <mod dir>/config/config.json, creating it with the defaults above when
// it is missing or older than `version`. False only when the file cannot be
// written at all - a malformed file is repaired with the defaults.
bool loadOrCreate();

// Write config.json now ("Save as default" in the panel, and mod disable).
// There is no runtime read: the file is imported once at load and is write-only
// afterwards, so a file edited while the game runs has no effect until restart.
bool save();

} // namespace neurodragon::config
