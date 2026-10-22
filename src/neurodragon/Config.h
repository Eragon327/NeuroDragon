#pragma once

#include <cstdint>

namespace neurodragon::config {

// Runtime settings, stored as <mod dir>/config/config.json through ll::config
// (nlohmann::json via boost::pfr aggregate reflection): every field below is a
// real key. There is no migration from older formats - delete the file and the
// defaults below are written again. `version` is required by ll::config's
// concept; bump it only when a key's meaning changes.
struct Nr {
    // Master switch. NR is the only feature of this mod, so it defaults to on.
    bool enabled = true;

    // 0 = evaluate only (picture untouched), 5 = gain composite onto the engine's
    // DLSS input (docs/D §6.16). 6 = TRP ratio resolve.
    int replace = 5;

    float gainBoost = 1.0f; // exaggerate the network's gain, for judging only

    int   style           = 0;
    float intensity       = 1.0f;
    float localToneStrength = 1.0f;
    float localStructureStrength = 1.0f;
    int   depthInverted   = -1; // -1 = follow what the engine asked for

    int sweep = 0; // 1 = cycle Style 0..2 per window, 2 = Enabled freeze test

    float mvecScale = -1.0f; // < 0 = derive from the render size (recommended)
    int   nrEncode  = -1;    // < 0 = per Replace mode (0 = Reinhard, 3 = TRP)

    // TRP's reconstruction knobs (docs/D §6.16, "ratio resolve").
    float whitePoint       = 1.0f;
    float transferStrength = 1.0f;
    float colourStrength   = 1.0f;
    float maxRatio         = 2.0f;

    bool resetEveryFrame = false;   // NRReset: kills NR temporal history, diagnostic only
    bool srResetEveryFrame = false; // SRReset: wipes the engine DLSS history every frame, diagnostic only
    bool paramLog        = false; // log every engine NGX parameter write
};

struct Config {
    int version = 1;
    Nr  nr{};
};

Config& get();

// Loads <mod dir>/config/config.json, creating it with the defaults above when
// it is missing or older than `version`. False only when the file cannot be
// written at all - a malformed file is repaired with the defaults.
bool loadOrCreate();

// Live tuning: re-read the file (used every ~2 s while rendering so Replace or
// GainBoost can be judged without restarting the game).
void reload();

} // namespace neurodragon::config
