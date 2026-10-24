#include "neurodragon/input/KeyBind.h"

#include "ll/api/i18n/I18n.h"

#include <Windows.h>

#include <algorithm>

namespace neurodragon::input {

using ll::i18n_literals::operator""_tr;

bool isModifierKey(int virtualKey) {
    switch (virtualKey) {
    case VK_SHIFT:
    case VK_LSHIFT:
    case VK_RSHIFT:
    case VK_CONTROL:
    case VK_LCONTROL:
    case VK_RCONTROL:
    case VK_MENU:
    case VK_LMENU:
    case VK_RMENU:
    case VK_LWIN:
    case VK_RWIN:
        return true;
    default:
        return false;
    }
}

bool isValidKey(int virtualKey) { return virtualKey > 0 && virtualKey < kMaxVirtualKey; }

int triggerKey(std::vector<int> const& keys) {
    for (auto iterator = keys.rbegin(); iterator != keys.rend(); ++iterator) {
        if (!isModifierKey(*iterator)) return *iterator;
    }
    return keys.empty() ? 0 : keys.back();
}

bool isMouseButton(int virtualKey) {
    switch (virtualKey) {
    case kMouseLeft:
    case kMouseRight:
    case kMouseMiddle:
    case kMouseX1:
    case kMouseX2:
        return true;
    default:
        return false;
    }
}

std::string keyName(int virtualKey) {
    if (!isValidKey(virtualKey)) return "?";

    if (isMouseButton(virtualKey)) {
        // Windows has no names for mouse buttons (`GetKeyNameText` cannot answer
        // for them), so they come from the catalogue.
        switch (virtualKey) {
        case kMouseLeft:
            return "ui.key.mouseLeft"_tr();
        case kMouseRight:
            return "ui.key.mouseRight"_tr();
        case kMouseMiddle:
            return "ui.key.mouseMiddle"_tr();
        case kMouseX1:
            return "ui.key.mouseX1"_tr();
        default:
            return "ui.key.mouseX2"_tr();
        }
    }

    if (virtualKey == VK_APPS) return "Menu"; // no name from GetKeyNameText either

    LONG keyData = static_cast<LONG>(MapVirtualKeyW(static_cast<UINT>(virtualKey), MAPVK_VK_TO_VSC)) << 16;
    // These keys carry the "extended" scan-code bit; without it GetKeyNameText
    // names a different key entirely (VK_END would come back as "Num 1").
    switch (virtualKey) {
    case VK_INSERT:
    case VK_DELETE:
    case VK_HOME:
    case VK_END:
    case VK_PRIOR:
    case VK_NEXT:
    case VK_LEFT:
    case VK_RIGHT:
    case VK_UP:
    case VK_DOWN:
    case VK_NUMLOCK:
    case VK_DIVIDE:
    case VK_RCONTROL:
    case VK_RMENU:
    case VK_LWIN:
    case VK_RWIN:
        keyData |= 1 << 24;
        break;
    default:
        break;
    }

    wchar_t name[64]{};
    if (GetKeyNameTextW(keyData, name, 64) <= 0) return "ui.key.unknown"_tr(virtualKey);

    int const bytes = WideCharToMultiByte(CP_UTF8, 0, name, -1, nullptr, 0, nullptr, nullptr);
    if (bytes <= 1) return "ui.key.unknown"_tr(virtualKey);

    std::string result(static_cast<size_t>(bytes - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, name, -1, result.data(), bytes, nullptr, nullptr);
    return result;
}

std::string formatCombo(std::vector<int> const& keys) {
    if (keys.empty()) return "-";

    std::string result;
    for (size_t index = 0; index < keys.size(); ++index) {
        if (index != 0) result += " + ";
        result += keyName(keys[index]);
    }
    return result;
}

KeyBind::KeyBind(std::vector<int> keys) : mKeys(std::move(keys)), mTrigger(triggerKey(mKeys)) {}

std::optional<KeyBind> KeyBind::make(std::vector<int> keys) {
    if (keys.empty()) return std::nullopt;
    if (!std::ranges::all_of(keys, [](int key) { return isValidKey(key); })) return std::nullopt;

    // See the header: without this a modifier pressed after the trigger would leave
    // a combination whose trigger can never see the modifier held.
    std::stable_partition(keys.begin(), keys.end(), [](int key) { return isModifierKey(key); });
    return KeyBind{std::move(keys)};
}

bool KeyBind::allHeld(std::array<bool, kMaxVirtualKey> const& held) const {
    return std::ranges::all_of(mKeys, [&held](int key) { return held[static_cast<size_t>(key)]; });
}

std::string KeyBind::display() const { return formatCombo(mKeys); }

namespace {

/// @param shorter is a strict prefix of @param longer (in stored order), and
///        genuinely shorter.
[[nodiscard]] bool isLeadingPartOf(std::vector<int> const& shorter, std::vector<int> const& longer) {
    if (shorter.size() >= longer.size()) return false;
    return std::equal(shorter.begin(), shorter.end(), longer.begin());
}

} // namespace

ConflictSeverity classifyConflict(KeyBind const& a, KeyBind const& b) {
    // Unbound slots never match anything, so they cannot collide.
    if (a.empty() || b.empty()) return ConflictSeverity::None;
    if (a.equals(b)) return ConflictSeverity::Exact;

    // Only "the short one is a prefix of the long one" collides (`X` and `X + C`).
    // The opposite direction (`C` and `X + C`) does not: both share trigger `C` and
    // the longer one wins the dispatch.
    if (isLeadingPartOf(a.keys(), b.keys()) || isLeadingPartOf(b.keys(), a.keys())) {
        return ConflictSeverity::Overlap;
    }
    return ConflictSeverity::None;
}

} // namespace neurodragon::input
