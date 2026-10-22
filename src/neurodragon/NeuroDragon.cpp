#include "neurodragon/NeuroDragon.h"

#include "neurodragon/Config.h"
#include "neurodragon/probe/RenderProbe.h"
#include "neurodragon/utils/Utils.h"

#include "ll/api/i18n/I18n.h"
#include "ll/api/mod/RegisterHelper.h"

namespace neurodragon {

NeuroDragon& NeuroDragon::getInstance() {
    static NeuroDragon instance;
    return instance;
}

constexpr wchar_t const* kModelFileName = L"nvngx_dlssnr.dll";

bool NeuroDragon::load() {
    // Everything the release build prints goes through lang/<locale>.json, so the
    // catalogue must be in place before the first message (the model bootstrap
    // below already speaks i18n).
    if (!ll::i18n::getInstance().load(getSelf().getLangDir())) {
        getSelf().getLogger().error("Could not load lang/ from {}", getSelf().getLangDir().string());
    }

    // config/config.json (ll::config): NR is the only feature of this mod, so a
    // missing or outdated file is repaired with the defaults rather than skipped.
    if (!config::loadOrCreate()) {
        getSelf().getLogger().error("Could not write config/config.json; NR cannot start");
        return false;
    }
    // The NGX core only looks for the NR model (a) next to the game executable and
    // (b) in the directories handed to NVSDK_NGX_D3D12_Init* through
    // FeatureCommonInfo::PathListInfo. Verified 2026-10-22 with ngx_probe on one
    // machine, one variable at a time: the process loader search directory
    // (SetDllDirectoryW), the working directory, AddDllDirectory and the NGX data
    // path all answer 0xBAD0000B ("UnableToInitializeFeature").
    //
    // So the model travels inside the mod folder and is materialised next to the
    // executable at load time: hard link (same volume: no bytes copied, no extra
    // disk use) -> copy -> a message that says exactly what to do by hand. An
    // existing file is never overwritten: whichever build the user put there wins.
    if (!utils::ensureApplicationModel(getSelf(), kModelFileName)) {
        return false;
    }

    return true;
}

bool NeuroDragon::enable() {
    // Locate + hook dragon::framerenderer::modules::taau without any PDB.
    probe::install();
    return true;
}

bool NeuroDragon::disable() {
    probe::uninstall();
    return true;
}

} // namespace neurodragon

LL_REGISTER_MOD(neurodragon::NeuroDragon, neurodragon::NeuroDragon::getInstance());
