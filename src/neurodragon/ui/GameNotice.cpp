#include "neurodragon/ui/GameNotice.h"

#include "neurodragon/ui/Overlay.h"

#include "ll/api/service/TargetedBedrock.h"
#include "ll/api/thread/ClientThreadExecutor.h"

#include "mc/client/game/ClientInstance.h"
#include "mc/client/gui/GuiData.h"

#include <optional>
#include <utility>

// Deliberately its own translation unit: everything in this file pulls in the
// engine headers (mc/_HeaderOutputPredefine.h and friends), which redefine
// DXGI/D3D12 structures, while Overlay.cpp next door must include <d3d12.h>
// unmodified (docs/D §4 pitfall 5). Keeping the two apart is why this is not just
// another function in Overlay.cpp.
namespace neurodragon::ui::notice {

void chat(std::string message) {
    if (message.empty()) return;

    // The callers are the Present hook's paths (the screenshot key is read on the
    // render thread), and GuiData is UI-thread state. The line crosses over as a
    // value; the executor queues it for the client thread's next tick.
    ll::thread::ClientThreadExecutor::getDefault().execute([message = std::move(message)] {
        if (auto client = ll::service::getClientInstance()) {
            // filteredMessage = nullopt: this string is ours, not player input, so
            // there is nothing to filter. ttsRequired = false: a screenshot notice
            // being read aloud would be noise.
            client->getGuiData()->displayClientMessage(message, std::nullopt, /*ttsRequired=*/false);
            return;
        }
        // No client instance: the game is still starting (there is no chat to read
        // yet) or it is shutting down. Keep the line on screen via the overlay
        // toast rather than losing it silently.
        Overlay::getInstance().notify(message);
    });
}

} // namespace neurodragon::ui::notice
