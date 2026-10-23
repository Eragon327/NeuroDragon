#include "neurodragon/NeuroDragon.h"

#include "neurodragon/Config.h"
#include "neurodragon/probe/RenderProbe.h"
#include "neurodragon/ui/Overlay.h"
#include "neurodragon/utils/Utils.h"

#include "ll/api/i18n/I18n.h"
#include "ll/api/io/Logger.h"
#include "ll/api/mod/RegisterHelper.h"

namespace neurodragon {

NeuroDragon& NeuroDragon::getInstance() {
    static NeuroDragon instance;
    return instance;
}

constexpr wchar_t const* kModelFileName = L"nvngx_dlssnr.dll";

bool NeuroDragon::load() {
    // Flush every info line straight to latest.log. LeviLamina's file sink buffers
    // by default (it flushes on level <= Off, i.e. never), which is the one thing a
    // stage marker must not do: the line written *before* a step that then stalls
    // for seconds has to be on disk while the game is stuck, otherwise a client
    // that has to be killed leaves no record of where it was. Flushing happens on
    // the loader's log thread, never on the render thread, so it cannot hitch a
    // frame.
    getSelf().getLogger().setFlushLevel(ll::io::LogLevel::Info);

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

    // The panel is not essential to NR: if the game window is not up yet the probe
    // watcher retries it in the background. install() writes its own stage pair -
    // creating the temporary D3D11 device and swap chain is the slow part of enable()
    // (3.5 s on this machine, 2026-10-23).
    if (!ui::Overlay::getInstance().install()) {
        getSelf().getLogger().warn("overlay: not installed yet; the probe watcher will retry");
    }
    return true;
}

bool NeuroDragon::disable() {
    // Take the panel down first: it owns the window procedure and hooks Present,
    // both of which must be in their original state before the NR hooks are dropped.
    ui::Overlay::getInstance().uninstall();
    probe::uninstall();
    return true;
}

} // namespace neurodragon

LL_REGISTER_MOD(neurodragon::NeuroDragon, neurodragon::NeuroDragon::getInstance());
