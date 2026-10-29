#include "neurodragon/NeuroDragon.h"

#include "neurodragon/Config.h"
#include "neurodragon/Log.h"
#include "neurodragon/probe/RenderProbe.h"
#include "neurodragon/probe/RuntimeOptions.h"
#include "neurodragon/probe/VvProbe.h"
#include "neurodragon/ui/Overlay.h"
#include "neurodragon/utils/Utils.h"
#include "neurodragon/vv/VvDlssSr.h"
#include "neurodragon/vv/VvSr.h"

#include "ll/api/event/EventBus.h"
#include "ll/api/event/player/PlayerJoinEvent.h"
#include "ll/api/i18n/I18n.h"
#include "ll/api/io/Logger.h"
#include "ll/api/mod/RegisterHelper.h"

#include <filesystem>

// One i18n idiom for the whole project: "key"_tr(args...) (docs/README.md 代码约定).
using ll::i18n_literals::operator""_tr;

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
            log::write(1, "nr.warn.modelPickMissing"_tr(configured));
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

    // Vibrant Visuals reconnaissance: a read-only hook on the same `taau` function
    // that shows whether the VV upscaler is live and with which numbers
    // (docs/O-vv-path.md). It touches nothing the game owns.
    probe::vv::install();

#ifdef TEST
    // A2 groundwork: switch the engine out of the TAAU branch through its own option
    // registry instead of editing options.txt. Explicitly opt-in through a marker file
    // so a TEST build used for other diagnostics keeps the engine as it is. Both
    // locations are checked because `getModDir()` has not been observed in this
    // context yet: the mod's own config folder and the game root next to the exe.
    {
        std::error_code error;
        auto const      modFlag =
            std::filesystem::path(getSelf().getModDir()) / "config" / "force_bilinear.flag";
        auto const gameFlag =
            std::filesystem::path(utils::applicationDirectory()) / "force_bilinear.flag";
        bool const modSet  = std::filesystem::exists(modFlag, error);
        bool const gameSet = std::filesystem::exists(gameFlag, error);
        utils::logger().info(
            "[vv] options: marker '{}' present={} (mod dir '{}'), '{}' present={}",
            modFlag.string(),
            modSet ? 1 : 0,
            getSelf().getModDir().string(),
            gameFlag.string(),
            gameSet ? 1 : 0
        );
        if (modSet || gameSet) {
            probe::options::requestBilinear();
        }
    }
#endif

    // The semantic dump is only meaningful inside a world (the main menu leaves the
    // render-resolution textures empty), so it is armed by the join event instead of a
    // frame counter.
    ll::event::EventBus::getInstance().emplaceListener<ll::event::PlayerJoinEvent>(
        [](ll::event::PlayerJoinEvent&) { probe::vv::noteWorldEntered(); }
    );

    // Screenshots go to the game root (`<version>/screenshots`, a sibling of `mods/`
    // and `logs/`) rather than inside the mod folder: that top-level folder is where
    // a player already looks for the game's own output, and the mod directory stays
    // the mod's. applicationDirectory() is the executable's folder, so the path is
    // absolute - the same string the chat notice shows and latest.log records.
    ui::Overlay::getInstance().setScreenshotDirectory(
        (std::filesystem::path(utils::applicationDirectory()) / "screenshots").wstring()
    );

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
    // Stop the product action first: the render thread may still be recording an
    // injected list, and vv::sr::release() waits for that record before freeing the
    // textures/PSO. Releasing first left the DLSS evaluate reading a dead colorHalf at
    // exit (EXP-0085 crash inside nvwgf2umx).
    vv::sr::setEnabled(false);
    vv::sr::release();
    vv::dlss::releaseSr();
    probe::vv::uninstall();
    probe::uninstall();
    return true;
}

} // namespace neurodragon

LL_REGISTER_MOD(neurodragon::NeuroDragon, neurodragon::NeuroDragon::getInstance());
