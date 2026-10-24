#pragma once

#include "neurodragon/input/Keys.h"

#include <array>
#include <optional>
#include <string>
#include <vector>

namespace neurodragon::input {

/// Every key in a combination is a Windows virtual-key code (`VK_*`), 0..255.
inline constexpr int kMaxVirtualKey = 256;

// ---------------------------------------------------------------------------
// Mouse buttons
// ---------------------------------------------------------------------------
//
// Mouse buttons are encoded in the same VK space (Windows itself defines
// VK_LBUTTON..VK_XBUTTON2 there), so a mouse button is simply "another key" and a
// combination such as "X + right mouse button" needs no mouse-specific branch in
// the matching logic.
//
// The macros are repeated instead of including <WinUser.h>, so that
// input/KeyBind.h does not drag <Windows.h> into every translation unit.

inline constexpr int kMouseLeft   = 0x01; ///< VK_LBUTTON
inline constexpr int kMouseRight  = 0x02; ///< VK_RBUTTON
inline constexpr int kMouseMiddle = 0x04; ///< VK_MBUTTON
inline constexpr int kMouseX1     = 0x05; ///< VK_XBUTTON1 (side button 1)
inline constexpr int kMouseX2     = 0x06; ///< VK_XBUTTON2 (side button 2)

/// Whether the code is one of the five mouse buttons above.
[[nodiscard]] bool isMouseButton(int virtualKey);

/// Modifier keys (Shift / Ctrl / Alt / Win). In a combination they are only a
/// precondition, never the trigger.
[[nodiscard]] bool isModifierKey(int virtualKey);

/// Whether the code can be stored in a binding.
[[nodiscard]] bool isValidKey(int virtualKey);

/// The combination's trigger: the last non-modifier key, or the last key when the
/// combination consists of modifiers only.
[[nodiscard]] int triggerKey(std::vector<int> const& keys);

/// Display name, e.g. "X", "Ctrl", "F1", "Mouse right".
/// Keyboard keys go through the system's `GetKeyNameText`, so the name follows the
/// Windows display language and needs no translation table of its own. Mouse buttons
/// have no Windows name, so those use the `ui.key.*` catalogue.
[[nodiscard]] std::string keyName(int virtualKey);

/// "Ctrl + X" style text; an empty combination becomes "-".
[[nodiscard]] std::string formatCombo(std::vector<int> const& keys);

/// One key combination, stored in press order (modifiers first, see `make()`).
///
/// Matching rule: at the moment the trigger goes down, every key in the combination
/// is held. A stricter "held count == combination length" was tried in Mangrove and
/// rejected: it would make hotkeys unusable while the player walks with W held, so
/// ambiguity between overlapping bindings is resolved by the dispatcher ("most keys
/// wins") instead.
///
/// **An empty combination is a valid value**: "this hotkey slot has no keys bound".
/// A default-constructed `KeyBind{}` is exactly that; `display()` shows "-" and
/// `trigger()` is 0, so it can never match anything. `make({})` still returns
/// `std::nullopt`, because that check is for *capture results*: capturing nothing is
/// invalid input, not "unbind" (unbind is expressed by an engaged-but-empty value).
class KeyBind {
public:
    KeyBind() = default;

    /// Validates and builds; returns `std::nullopt` for an empty list or an invalid
    /// key code.
    ///
    /// Modifier keys are moved to the front (stable partition), keeping the press
    /// order among them. That is not cosmetic: the trigger is the last non-modifier,
    /// so a modifier *pressed after* the trigger would never be held while that
    /// trigger goes down - the binding could never fire. Normalising here makes
    /// "C then Ctrl" the same usable combination as "Ctrl then C".
    [[nodiscard]] static std::optional<KeyBind> make(std::vector<int> keys);

    /// Convenience constructor for a single key.
    [[nodiscard]] static std::optional<KeyBind> single(int virtualKey) { return make({virtualKey}); }

    [[nodiscard]] std::vector<int> const& keys() const { return mKeys; }
    [[nodiscard]] int                     trigger() const { return mTrigger; }
    [[nodiscard]] size_t                  size() const { return mKeys.size(); }
    [[nodiscard]] bool                    empty() const { return mKeys.empty(); }

    /// Every key of the combination is currently held.
    [[nodiscard]] bool allHeld(std::array<bool, kMaxVirtualKey> const& held) const;

    /// Same key sequence, in the same order (used to tell "changed back to the
    /// default" apart from a real edit).
    [[nodiscard]] bool equals(KeyBind const& other) const { return mKeys == other.mKeys; }

    /// Display text, e.g. "X + C"; an empty combination shows "-".
    [[nodiscard]] std::string display() const;

private:
    explicit KeyBind(std::vector<int> keys);

    std::vector<int> mKeys;
    int              mTrigger{};
};

/// How badly two bindings collide. The panel colours rows by severity: an exact
/// duplicate is the worst case, a prefix overlap the next one.
enum class ConflictSeverity {
    None,    ///< unrelated
    Overlap, ///< the shorter one is a prefix of the longer one (`X` and `X + C`)
    Exact,   ///< the same keys in the same order
};

/// How two bindings collide.
///
/// There is exactly one kind of collision that matters: **the shorter binding is a
/// prefix of the longer one** (in stored order, i.e. press order) - `X` and
/// `X + C`. The short one then fires while the player is still forming the long one,
/// so a player who wanted `X + C` also gets an `X`.
///
/// The opposite direction does **not** collide: `C` and `X + C` share the trigger
/// `C`, and the dispatcher's "most keys wins" drops the short binding whenever the
/// long one matches. Pressing `C` alone still works.
///
/// @note The values are ordered `None < Overlap < Exact`, so combining several
///       collisions with `std::max` makes the worst one win without extra branching.
[[nodiscard]] ConflictSeverity classifyConflict(KeyBind const& a, KeyBind const& b);

} // namespace neurodragon::input
