#include "neurodragon/ui/InputGuard.h"

#include "ll/api/memory/Hook.h"

#include "mc/deps/input/Keyboard.h"
#include "mc/deps/input/MouseDevice.h"
#include "mc/deps/input/win/HIDControllerGameCoreDesktop.h"

#include <Windows.h>

#include <atomic>
#include <cstdint>

namespace neurodragon::ui {
namespace {

InputGuardStatus           gStatus{};
thread_local std::uint32_t gHandoffDepth{};

// Keep swallowing for a moment after the panel closes, so the release of the key
// that closed it does not leak into the game.
constexpr ULONGLONG kHandoffTailMs = 180;

std::atomic_bool   gCaptureActive{false};
std::atomic_ullong gCaptureUntil{};

bool menuOwnsGameInput() {
    if (gHandoffDepth != 0) return false;
    if (gCaptureActive.load(std::memory_order_acquire)) return true;
    return GetTickCount64() <= gCaptureUntil.load(std::memory_order_acquire);
}

LL_TYPE_INSTANCE_HOOK(
    GuardMouseHook,
    ll::memory::HookPriority::Highest,
    MouseDevice,
    &MouseDevice::feed,
    void,
    char  actionButtonId,
    schar buttonData,
    short x,
    short y,
    short dx,
    short dy,
    bool  forceMotionlessPointer
) {
    if (menuOwnsGameInput()) return;
    origin(actionButtonId, buttonData, x, y, dx, dy, forceMotionlessPointer);
}

LL_TYPE_INSTANCE_HOOK(
    GuardKeyDownHook,
    ll::memory::HookPriority::Highest,
    HIDControllerGameCoreDesktop,
    &HIDControllerGameCoreDesktop::$onKeyDown,
    void,
    int                                                 keyCode,
    Bedrock::Input::KeyboardEventProcessor::InputOrigin originType
) {
    // F11 (toggle fullscreen) is a window action: while the panel is open it is the
    // only way out of a bad resolution, so it must never be eaten.
    if (menuOwnsGameInput() && keyCode != Keyboard::F11) return;
    origin(keyCode, originType);
}

LL_TYPE_INSTANCE_HOOK(
    GuardKeyUpHook,
    ll::memory::HookPriority::Highest,
    HIDControllerGameCoreDesktop,
    &HIDControllerGameCoreDesktop::$onKeyUp,
    void,
    int keyCode
) {
    if (menuOwnsGameInput() && keyCode != Keyboard::F11) return;
    origin(keyCode);
}

} // namespace

InputHandoffScope::InputHandoffScope() { ++gHandoffDepth; }

InputHandoffScope::~InputHandoffScope() { --gHandoffDepth; }

bool overlayOwnsGameInput() { return menuOwnsGameInput(); }

void setOverlayInputCapture(bool capture) {
    gCaptureActive.store(capture, std::memory_order_release);
    gCaptureUntil.store(GetTickCount64() + kHandoffTailMs, std::memory_order_release);
}

InputGuardStatus installInputGuard() {
    if (!gStatus.mouseHookInstalled) gStatus.mouseHookInstalled = GuardMouseHook::hook() == 0;
    if (!gStatus.keyDownHookInstalled) gStatus.keyDownHookInstalled = GuardKeyDownHook::hook() == 0;
    if (!gStatus.keyUpHookInstalled) gStatus.keyUpHookInstalled = GuardKeyUpHook::hook() == 0;
    return gStatus;
}

void uninstallInputGuard() {
    if (gStatus.mouseHookInstalled) {
        GuardMouseHook::unhook();
        gStatus.mouseHookInstalled = false;
    }
    if (gStatus.keyDownHookInstalled) {
        GuardKeyDownHook::unhook();
        gStatus.keyDownHookInstalled = false;
    }
    if (gStatus.keyUpHookInstalled) {
        GuardKeyUpHook::unhook();
        gStatus.keyUpHookInstalled = false;
    }

    gHandoffDepth = 0;
    gCaptureActive.store(false, std::memory_order_release);
    gCaptureUntil.store(0, std::memory_order_release);
}

} // namespace neurodragon::ui
