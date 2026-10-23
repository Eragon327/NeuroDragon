#include "neurodragon/NeuroDragon.h"

#include "neurodragon/Config.h"
#include "neurodragon/Log.h"
#include "neurodragon/probe/RenderProbe.h"
#include "neurodragon/ui/Overlay.h"
#include "neurodragon/utils/Utils.h"

#include "ll/api/i18n/I18n.h"
#include "ll/api/io/Logger.h"
#include "ll/api/mod/RegisterHelper.h"

#include <filesystem>

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
#ifdef TEST
    // The diagnostic build's whole point is its probe lines, and they are written at
    // debug level (NR_DEBUG / ND_DEBUG). LeviLamina's logger drops debug at its default
    // level, which made every probe invisible in latest.log - the reason P2/P4 could not
    // be answered from a run. Release builds keep the default level and flush policy.
    getSelf().getLogger().setLevel(ll::io::LogLevel::Debug);
    getSelf().getLogger().setFlushLevel(ll::io::LogLevel::Debug);
#endif

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
    // disk use) -> copy -> a message that says exactly what to do by hand. Without a
    // panel pick an existing file is left alone (whichever build the user put there
    // wins); with a pick the picked build replaces it, so the panel switch works.
    auto const& configured = config::get().nr.modelFile;
    std::filesystem::path sourceName = std::filesystem::path(kModelFileName);
    bool                  replaceExisting = false;
    if (!configured.empty()) {
        std::error_code error;
        if (std::filesystem::exists(std::filesystem::path(getSelf().getModDir()) / std::filesystem::u8path(configured), error)) {
            sourceName      = std::filesystem::u8path(configured);
            replaceExisting = true;
        } else {
            // A stale pick must not stop the mod from loading: say so and use the
            // canonical file instead.
            log::writeKey(1, "nr.warn.modelPickMissing", configured);
        }
    }
    if (!utils::ensureApplicationModel(getSelf(), sourceName, replaceExisting)) {
        return false;
    }

    return true;
}

bool NeuroDragon::enable() {
    // Locate + hook dragon::framerenderer::modules::taau without any PDB.
    probe::install();

    // Screenshots go next to the mod, not next to the executable: the mod folder is
    // the one place a user can find without knowing how LeviLauncher lays out a
    // version directory.
    ui::Overlay::getInstance().setScreenshotDirectory((getSelf().getModDir() / "screenshots").wstring());

    // The panel is not essential to NR: if the game window is not up yet the probe
    // watcher retries it in the background. install() writes its own stage pair - the
    // temporary objects that find Present are the slow part of enable(), but since
    // 2026-10-23 that is 375 ms (a D3D12 device + flip chain) instead of the 3.5-4.9 s
    // the temporary D3D11 device cost when it was destroyed.
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
