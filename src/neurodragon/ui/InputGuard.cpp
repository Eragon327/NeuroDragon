#include "neurodragon/ui/InputGuard.h"

#include "neurodragon/Test.h"
#include "neurodragon/input/KeyBind.h"
#include "neurodragon/utils/Utils.h"

#include "ll/api/memory/Hook.h"

#include "mc/deps/input/Keyboard.h"
#include "mc/deps/input/MouseDevice.h"
#include "mc/deps/input/win/HIDControllerGameCoreDesktop.h"

#include <Windows.h>

#include <atomic>
#include <cstdint>
#include <string>

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

#ifdef TEST
std::string textForLog(std::string const& text) {
    constexpr char kHex[] = "0123456789ABCDEF";
    std::string    escaped;
    escaped.reserve(text.size());
    for (unsigned char const unit : text) {
        if (unit == '"' || unit == '\\') {
            escaped.push_back('\\');
            escaped.push_back(static_cast<char>(unit));
        } else if (unit < 0x20 || unit == 0x7F) {
            escaped += "\\x";
            escaped.push_back(kHex[unit >> 4]);
            escaped.push_back(kHex[unit & 0x0F]);
        } else {
            escaped.push_back(static_cast<char>(unit));
        }
    }
    return escaped;
}
#endif

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
    bool const owned = menuOwnsGameInput();
#ifdef TEST
    if (actionButtonId != 0) {
        ND_DEBUG(
            "[input] t={} hid mouse button={} data={} action={}",
            GetTickCount64(),
            static_cast<int>(actionButtonId),
            static_cast<int>(buttonData),
            owned ? "eaten" : "forwarded"
        );
    }
#endif
    if (owned) return;
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
    bool const owned = menuOwnsGameInput();
    bool const eaten = owned && keyCode != Keyboard::F11;
    ND_DEBUG(
        "[input] t={} hid vk=0x{:02X}({}) name=\"{}\" state=down origin={} action={}",
        GetTickCount64(),
        static_cast<unsigned>(keyCode),
        keyCode,
        input::keyName(keyCode),
        static_cast<int>(originType),
        eaten ? "eaten" : "forwarded"
    );
    // F11 (toggle fullscreen) is a window action: while the panel is open it is the
    // only way out of a bad resolution, so it must never be eaten.
    if (eaten) return;
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
    bool const owned = menuOwnsGameInput();
    bool const eaten = owned && keyCode != Keyboard::F11;
    ND_DEBUG(
        "[input] t={} hid vk=0x{:02X}({}) name=\"{}\" state=up action={}",
        GetTickCount64(),
        static_cast<unsigned>(keyCode),
        keyCode,
        input::keyName(keyCode),
        eaten ? "eaten" : "forwarded"
    );
    if (eaten) return;
    origin(keyCode);
}

#ifdef TEST
LL_TYPE_INSTANCE_HOOK(
    GuardTextInputHook,
    ll::memory::HookPriority::Highest,
    HIDControllerGameCoreDesktop,
    &HIDControllerGameCoreDesktop::$onTextInput,
    void,
    std::string const& utf8Text
) {
    ND_DEBUG(
        "[input] t={} hid text=\"{}\" owned={}",
        GetTickCount64(),
        textForLog(utf8Text),
        menuOwnsGameInput() ? 1 : 0
    );
    origin(utf8Text);
}
#endif

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
#ifdef TEST
    if (!gStatus.textInputHookInstalled) gStatus.textInputHookInstalled = GuardTextInputHook::hook() == 0;
#endif
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
#ifdef TEST
    if (gStatus.textInputHookInstalled) {
        GuardTextInputHook::unhook();
        gStatus.textInputHookInstalled = false;
    }
#endif

    gHandoffDepth = 0;
    gCaptureActive.store(false, std::memory_order_release);
    gCaptureUntil.store(0, std::memory_order_release);
}

} // namespace neurodragon::ui
