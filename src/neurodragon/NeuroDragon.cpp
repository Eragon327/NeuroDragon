#include "neurodragon/NeuroDragon.h"

#include "neurodragon/probe/RenderProbe.h"

#include "ll/api/mod/RegisterHelper.h"

namespace neurodragon {

NeuroDragon& NeuroDragon::getInstance() {
    static NeuroDragon instance;
    return instance;
}

bool NeuroDragon::load() {
    getSelf().getLogger().debug("Loading...");
    // Code for loading the mod goes here.
    return true;
}

bool NeuroDragon::enable() {
    getSelf().getLogger().debug("Enabling...");
    // Locate + hook dragon::framerenderer::modules::taau without any PDB.
    probe::install();
    return true;
}

bool NeuroDragon::disable() {
    getSelf().getLogger().debug("Disabling...");
    probe::uninstall();
    return true;
}

} // namespace neurodragon

LL_REGISTER_MOD(neurodragon::NeuroDragon, neurodragon::NeuroDragon::getInstance());
